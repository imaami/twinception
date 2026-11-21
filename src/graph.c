/** @file graph.c
 *
 * The bytes of the graph of a translation unit (src/graph.h): a draft
 * writes them, and graph() checks them for a reader.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "graph.h"

/* The parts of the bytes of a graph, in their order after the head. The
 * text is last, because the size of each other part is a multiple of the
 * size of uint32_t.
 */
enum {
	GRAPH_FNS,
	GRAPH_CALLS,
	GRAPH_TO,
	GRAPH_USES,
	GRAPH_NAMES,
	GRAPH_TEXT,
	GRAPH_PARTS
};

/* The first bytes of a graph: the magic of this layout of the records, the
 * flags of the graph (enum graph_flag), and the number of the records of
 * each part. Another layout of a record or of the parts gets another magic.
 */
struct graph_head {
	uint32_t magic;
	uint32_t flags;
	uint32_t n[GRAPH_PARTS];
};

#define GRAPH_MAGIC UINT32_C(0x33677361)  /* "asg3" */

/* The size of a record of each part. */
static size_t const graph_size[GRAPH_PARTS] = {
	[GRAPH_FNS]   = sizeof(struct graph_fn),
	[GRAPH_CALLS] = sizeof(struct graph_call),
	[GRAPH_TO]    = sizeof(uint32_t),
	[GRAPH_USES]  = sizeof(struct graph_use),
	[GRAPH_NAMES] = sizeof(uint32_t),
	[GRAPH_TEXT]  = 1,
};

/* Compares the text @p text of a graph with @p s, a text of one byte or
 * more without a NUL, as strcmp() compares two texts.
 */
static int
graph_text_order (char const       *text,
                  struct cstr_view  s)
{
	int const order = strncmp(text, s.data, s.len);
	return order ? order : text[s.len] != '\0';
}

/* Returns true if each index and offset of the records of @p g is of a
 * record or a text of @p g, the names are each function once in the order
 * of the names and the indices, and each use is a call that names the
 * function, in the code of the function that the use tells. The offset of
 * a text is valid if it is in the text part, because the part ends with a
 * NUL. The name of a function that is GRAPH_FN_SYMBOL is not the last
 * text. The callees of each call are in the order of the indices, and the
 * calls have as many callees as the graph has uses: the time of the check
 * is then limited by the size of the graph.
 */
static bool
graph_valid (struct graph const *g)
{
	if (g->n_text && g->text[g->n_text - 1])
		return false;

	for (uint32_t i = 0; i < g->n_fns; ++i) {
		struct graph_fn const *fn = &g->fns[i];
		if (fn->name >= g->n_text ||
		    (fn->flags & GRAPH_FN_SYMBOL &&
		     strlen(g->text + fn->name) + 1 >= g->n_text - fn->name) ||
		    fn->declared.file >= g->n_text ||
		    (fn->defined.file != GRAPH_NONE &&
		     fn->defined.file >= g->n_text) ||
		    fn->calls > g->n_calls ||
		    fn->n_calls > g->n_calls - fn->calls ||
		    fn->uses > g->n_uses ||
		    fn->n_uses > g->n_uses - fn->uses ||
		    g->names[i] >= g->n_fns)
			return false;
	}

	/* The names of the functions are valid texts after the loop above. */
	for (uint32_t i = 1; i < g->n_fns; ++i) {
		int const order = strcmp(graph_text(g, g->fns[g->names[i - 1]].name),
		                         graph_text(g, g->fns[g->names[i]].name));
		if (order > 0 || (!order && g->names[i - 1] >= g->names[i]))
			return false;
	}

	uint64_t callees = 0;
	for (uint32_t i = 0; i < g->n_calls; ++i) {
		struct graph_call const *call = &g->calls[i];
		if (call->file >= g->n_text ||
		    call->to > g->n_to ||
		    call->n > g->n_to - call->to ||
		    (call->macro != GRAPH_NONE && call->macro >= g->n_text) ||
		    call->state > GRAPH_UNEVALUATED ||
		    (call->state == GRAPH_UNEVALUATED && call->n))
			return false;

		callees += call->n;
		if (callees > g->n_uses)
			return false;
		for (uint32_t k = 1; k < call->n; ++k) {
			if (g->to[call->to + k - 1] > g->to[call->to + k])
				return false;
		}
	}
	if (callees != g->n_uses)
		return false;

	for (uint32_t i = 0; i < g->n_to; ++i) {
		if (g->to[i] >= g->n_fns)
			return false;
	}

	for (uint32_t i = 0; i < g->n_uses; ++i) {
		struct graph_use const *use = &g->uses[i];
		if (use->call >= g->n_calls ||
		    (use->by != GRAPH_NONE && use->by >= g->n_fns))
			return false;
	}

	for (uint32_t f = 0; f < g->n_fns; ++f) {
		struct graph_fn const *fn = &g->fns[f];
		for (uint32_t u = 0; u < fn->n_uses; ++u) {
			struct graph_use const *use = &g->uses[fn->uses + u];
			struct graph_call const *call = &g->calls[use->call];
			struct graph_fn const *by = use->by == GRAPH_NONE ? nullptr
			                                                  : &g->fns[use->by];
			uint32_t const *to = g->to + call->to;
			uint32_t at = 0;
			for (uint32_t end = call->n; at < end;) {
				uint32_t const mid = at + (end - at) / 2;
				if (to[mid] < f)
					at = mid + 1;
				else
					end = mid;
			}
			if (at == call->n || to[at] != f ||
			    (by && (use->call < by->calls ||
			            use->call - by->calls >= by->n_calls)))
				return false;
		}
	}

	return true;
}

struct graph
graph (struct buf *bytes,
       int        *e)
{
	struct span const all = buf_peek(bytes);
	struct graph_head head;
	*e = EINVAL;
	if (all.len < sizeof head || (uintptr_t)all.ptr % alignof(uint32_t))
		return (struct graph){};

	memcpy(&head, all.ptr, sizeof head);
	/* A count is less than 2^32 and a record less than 2^8 bytes. */
	uint64_t at[GRAPH_PARTS + 1] = { sizeof head };
	for (size_t i = 0; i < GRAPH_PARTS; ++i)
		at[i + 1] = at[i] + (uint64_t)head.n[i] * graph_size[i];
	if (head.magic != GRAPH_MAGIC || (head.flags & ~(uint32_t)GRAPH_ERRORS) ||
	    head.n[GRAPH_NAMES] != head.n[GRAPH_FNS] || at[GRAPH_PARTS] != all.len)
		return (struct graph){};

	struct graph g = {
		.fns     = (void const *)(all.ptr + at[GRAPH_FNS]),
		.calls   = (void const *)(all.ptr + at[GRAPH_CALLS]),
		.to      = (void const *)(all.ptr + at[GRAPH_TO]),
		.uses    = (void const *)(all.ptr + at[GRAPH_USES]),
		.names   = (void const *)(all.ptr + at[GRAPH_NAMES]),
		.text    = (char const *)(all.ptr + at[GRAPH_TEXT]),
		.flags   = head.flags,
		.n_fns   = head.n[GRAPH_FNS],
		.n_calls = head.n[GRAPH_CALLS],
		.n_to    = head.n[GRAPH_TO],
		.n_uses  = head.n[GRAPH_USES],
		.n_text  = head.n[GRAPH_TEXT],
	};
	if (!graph_valid(&g))
		return (struct graph){};

	g.bytes = *bytes;
	*bytes = buf();
	*e = 0;
	return g;
}

void
graph_fini (struct graph *g)
{
	buf_fini(&g->bytes);
	*g = (struct graph){};
}

uint32_t const *
graph_named (struct graph const *g,
             struct cstr_view    name,
             uint32_t           *n)
{
	/* first is the first function whose name is not before @p name. */
	bool const none = !name.len || memchr(name.data, '\0', name.len);
	uint32_t first = 0;
	uint32_t end = none ? 0 : g->n_fns;
	while (first < end) {
		uint32_t const mid = first + (end - first) / 2;
		if (graph_text_order(graph_text(g, g->fns[g->names[mid]].name), name) < 0)
			first = mid + 1;
		else
			end = mid;
	}
	while (!none && end < g->n_fns &&
	       !graph_text_order(graph_text(g, g->fns[g->names[end]].name), name))
		++end;
	*n = end - first;
	return *n ? &g->names[first] : nullptr;
}

void
graph_draft_fini (struct graph_draft *draft)
{
	free(draft->text.items);
	free(draft->to.items);
	free(draft->calls.items);
	free(draft->fns.items);
	*draft = (struct graph_draft){};
}

int
graph_draft_text (struct graph_draft *draft,
                  struct cstr_view    s,
                  uint32_t           *at)
{
	size_t const n = draft->text.n;
	if (n >= GRAPH_NONE || s.len >= GRAPH_NONE - n)
		return EOVERFLOW;
	if (util_grow(draft->text.items, draft->text.cap, n + s.len + 1))
		return ENOMEM;

	if (s.len)
		memcpy(draft->text.items + n, s.data, s.len);
	draft->text.items[n + s.len] = '\0';
	draft->text.n = n + s.len + 1;
	*at = (uint32_t)n;
	return 0;
}

/* A function of a draft with its name, for the order of the names. */
struct graph_name {
	char const *name;
	size_t      fn;
};

/* Orders two functions of a draft by name, and those of one name by
 * index.
 */
static int
graph_name_order (void const *a,
                  void const *b)
{
	struct graph_name const *p = a;
	struct graph_name const *q = b;
	int const order = strcmp(p->name, q->name);
	return order ? order : (p->fn > q->fn) - (p->fn < q->fn);
}

/* Writes to @p uses the uses of the functions of @p draft, for each
 * function those of the calls that name it, in the order of the calls, and
 * sets the uses of each function. The callees of each call are in the
 * draft, and @p uses has room for a use of each of them. @p by and @p next
 * are scratch of a number for each call and for each function. Returns
 * false if a record of the draft names a call or a function that the draft
 * does not contain.
 */
static bool
graph_draft_uses (struct graph_draft *draft,
                  struct graph_use   *uses,
                  uint32_t           *by,
                  uint32_t           *next)
{
	struct graph_fn *fns = draft->fns.items;
	struct graph_call const *calls = draft->calls.items;
	size_t const n_fns = draft->fns.n;
	size_t const n_calls = draft->calls.n;
	/* The function whose code contains a call, or none. */
	for (size_t c = 0; c < n_calls; ++c)
		by[c] = GRAPH_NONE;
	for (size_t f = 0; f < n_fns; ++f) {
		struct graph_fn *fn = &fns[f];
		if (fn->calls > n_calls || fn->n_calls > n_calls - fn->calls)
			return false;
		for (uint32_t c = 0; c < fn->n_calls; ++c)
			by[fn->calls + c] = (uint32_t)f;
		fn->n_uses = 0;
	}
	for (size_t c = 0; c < n_calls; ++c) {
		for (uint32_t i = 0; i < calls[c].n; ++i) {
			uint32_t const to = draft->to.items[calls[c].to + i];
			if (to >= n_fns)
				return false;
			++fns[to].n_uses;
		}
	}
	uint32_t at = 0;
	for (size_t f = 0; f < n_fns; ++f) {
		fns[f].uses = at;
		next[f] = at;
		at += fns[f].n_uses;
	}
	for (size_t c = 0; c < n_calls; ++c) {
		for (uint32_t i = 0; i < calls[c].n; ++i)
			uses[next[draft->to.items[calls[c].to + i]]++] = (struct graph_use){
				.call = (uint32_t)c,
				.by   = by[c],
			};
	}
	return true;
}

/* Orders two indices. */
static int
graph_index_order (void const *a,
                   void const *b)
{
	uint32_t const p = *(uint32_t const *)a;
	uint32_t const q = *(uint32_t const *)b;
	return (p > q) - (p < q);
}

int
graph_draft_write (struct graph_draft *draft,
                   int                 fd)
{
	size_t const n_fns = draft->fns.n;
	size_t const n_calls = draft->calls.n;
	size_t const n_to = draft->to.n;
	if (n_fns >= GRAPH_NONE || n_calls >= GRAPH_NONE || n_to >= GRAPH_NONE ||
	    draft->text.n >= GRAPH_NONE)
		return EOVERFLOW;

	/* A use for each callee of each call: two calls can name one callee
	 * of the draft. */
	size_t n_uses = 0;
	for (size_t c = 0; c < n_calls; ++c) {
		struct graph_call const *call = &draft->calls.items[c];
		if (call->to > n_to || call->n > n_to - call->to)
			return EINVAL;
		n_uses += call->n;
		/* graph() looks for a callee of a call in the order of the
		 * indices. */
		if (call->n > 1)
			qsort(draft->to.items + call->to, call->n, sizeof *draft->to.items,
			      graph_index_order);
	}
	if (n_uses >= GRAPH_NONE)
		return EOVERFLOW;

	/* One more record than needed, so that no size is 0. */
	uint32_t *numbers = malloc((n_calls + n_fns + 1) * sizeof *numbers);
	struct graph_use *uses = malloc((n_uses + 1) * sizeof *uses);
	struct graph_name *order = malloc((n_fns + 1) * sizeof *order);
	int e = !numbers || !uses || !order ? ENOMEM : 0;
	uint32_t *names = e ? nullptr : numbers + n_calls;
	if (!e && !graph_draft_uses(draft, uses, numbers, names))
		e = EINVAL;
	for (size_t f = 0; !e && f < n_fns; ++f) {
		if (draft->fns.items[f].name >= draft->text.n)
			e = EINVAL;
		else
			order[f] = (struct graph_name){
				.name = draft->text.items + draft->fns.items[f].name,
				.fn   = f,
			};
	}
	if (!e) {
		qsort(order, n_fns, sizeof *order, graph_name_order);
		for (size_t f = 0; f < n_fns; ++f)
			names[f] = (uint32_t)order[f].fn;
	}

	struct {
		void const *items;
		size_t      n;
	} const part[GRAPH_PARTS] = {
		[GRAPH_FNS]   = { draft->fns.items,   n_fns         },
		[GRAPH_CALLS] = { draft->calls.items, n_calls       },
		[GRAPH_TO]    = { draft->to.items,    n_to          },
		[GRAPH_USES]  = { uses,               n_uses        },
		[GRAPH_NAMES] = { names,              n_fns         },
		[GRAPH_TEXT]  = { draft->text.items,  draft->text.n },
	};
	struct graph_head head = {
		.magic = GRAPH_MAGIC,
		.flags = (uint32_t)draft->flags,
	};
	for (size_t i = 0; i < GRAPH_PARTS; ++i)
		head.n[i] = (uint32_t)part[i].n;

	if (!e)
		e = util_write_all(fd, &head, sizeof head);
	for (size_t i = 0; !e && i < GRAPH_PARTS; ++i)
		e = util_write_all(fd, part[i].items, part[i].n * graph_size[i]);
	free(order);
	free(uses);
	free(numbers);
	return e;
}
