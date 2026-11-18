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

#define GRAPH_MAGIC UINT32_C(0x32677361)  /* "asg2" */

/* The size of a record of each part. */
static size_t const graph_size[GRAPH_PARTS] = {
	[GRAPH_FNS]   = sizeof(struct graph_fn),
	[GRAPH_CALLS] = sizeof(struct graph_call),
	[GRAPH_TO]    = sizeof(uint32_t),
	[GRAPH_TEXT]  = 1,
};

/* Returns true if each index and offset of the records of @p g is of a
 * record or a text of @p g. The offset of a text is valid if it is in the
 * text part, because the part ends with a NUL.
 */
static bool
graph_valid (struct graph const *g)
{
	if (g->n_text && g->text[g->n_text - 1])
		return false;

	for (uint32_t i = 0; i < g->n_fns; ++i) {
		struct graph_fn const *fn = &g->fns[i];
		if (fn->name >= g->n_text ||
		    fn->declared.file >= g->n_text ||
		    (fn->defined.file != GRAPH_NONE &&
		     fn->defined.file >= g->n_text) ||
		    fn->calls > g->n_calls ||
		    fn->n_calls > g->n_calls - fn->calls)
			return false;
	}

	for (uint32_t i = 0; i < g->n_calls; ++i) {
		struct graph_call const *call = &g->calls[i];
		if (call->file >= g->n_text ||
		    call->to > g->n_to ||
		    call->n > g->n_to - call->to ||
		    (call->macro != GRAPH_NONE && call->macro >= g->n_text) ||
		    call->state > GRAPH_UNEVALUATED ||
		    (call->state == GRAPH_UNEVALUATED && call->n))
			return false;
	}

	for (uint32_t i = 0; i < g->n_to; ++i) {
		if (g->to[i] >= g->n_fns)
			return false;
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
	    at[GRAPH_PARTS] != all.len)
		return (struct graph){};

	struct graph g = {
		.fns     = (void const *)(all.ptr + at[GRAPH_FNS]),
		.calls   = (void const *)(all.ptr + at[GRAPH_CALLS]),
		.to      = (void const *)(all.ptr + at[GRAPH_TO]),
		.text    = (char const *)(all.ptr + at[GRAPH_TEXT]),
		.flags   = head.flags,
		.n_fns   = head.n[GRAPH_FNS],
		.n_calls = head.n[GRAPH_CALLS],
		.n_to    = head.n[GRAPH_TO],
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

int
graph_draft_write (struct graph_draft const *draft,
                   int                       fd)
{
	struct {
		void const *items;
		size_t      n;
	} const part[GRAPH_PARTS] = {
		[GRAPH_FNS]   = { draft->fns.items,   draft->fns.n   },
		[GRAPH_CALLS] = { draft->calls.items, draft->calls.n },
		[GRAPH_TO]    = { draft->to.items,    draft->to.n    },
		[GRAPH_TEXT]  = { draft->text.items,  draft->text.n  },
	};
	struct graph_head head = {
		.magic = GRAPH_MAGIC,
		.flags = (uint32_t)draft->flags
	};
	for (size_t i = 0; i < GRAPH_PARTS; ++i) {
		if (part[i].n >= GRAPH_NONE)
			return EOVERFLOW;
		head.n[i] = (uint32_t)part[i].n;
	}

	int e = util_write_all(fd, &head, sizeof head);
	for (size_t i = 0; !e && i < GRAPH_PARTS; ++i)
		e = util_write_all(fd, part[i].items,
		                   part[i].n * graph_size[i]);
	return e;
}
