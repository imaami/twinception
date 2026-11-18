/** @file build.c
 *
 * The translation units of a build, and the answer of callers from their
 * graphs (src/build.h).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "cxi_out.h"
#include "dstr.h"

void
build_fini (struct build *b)
{
	for (size_t i = 0; i < b->n; ++i)
		graph_fini(&b->items[i].g);
	free(b->items);
	buf_fini(&b->text);
	*b = build();
}

int
build_add (struct build     *b,
           size_t            index,
           struct cstr_view  path)
{
	size_t const at = buf_peek(&b->text).len;
	int e = buf_put(&b->text, path.data, path.len, UINT32_MAX);
	if (!e)
		e = buf_put(&b->text, "", 1, UINT32_MAX);
	if (!e)
		e = util_push(*b, (struct build_unit){
			.index = index,
			.path  = (uint32_t)at
		});
	if (e)
		/* The bytes of a path without a unit are removed. */
		buf_cut(&b->text, at, buf_peek(&b->text).len - at);
	return e == ENOBUFS ? EOVERFLOW : e;
}

void
build_parsed (struct build *b,
              size_t        unit,
              struct buf   *bytes)
{
	struct build_unit *u = &b->items[unit];
	int e = EINVAL;
	graph_fini(&u->g);
	if (bytes) {
		/* The graph is kept, so its storage is made as small as its
		 * bytes allow. */
		(void)buf_trim(bytes);
		u->g = graph(bytes, &e);
	}
	u->state = e ? BUILD_FAILED : BUILD_GRAPH;
}

/* Returns true if the text @p text of a graph is @p s, a text without a
 * NUL.
 */
static bool
build_text_is (char const       *text,
               struct cstr_view  s)
{
	return !strncmp(text, s.data, s.len) && !text[s.len];
}

/* Returns true if the calls @p p and @p q of @p g are of one row: the same
 * state, flags, macro and callees.
 */
static bool
build_call_same (struct graph const      *g,
                 struct graph_call const *p,
                 struct graph_call const *q)
{
	return p->state == q->state &&
	       p->flags == q->flags &&
	       p->macro == q->macro &&
	       p->n     == q->n     &&
	       !memcmp(&g->to[p->to], &g->to[q->to], p->n * sizeof *g->to);
}

/* Returns true if the function @p fn of @p g is a callee of @p call. */
static bool
build_call_to (struct graph const      *g,
               struct graph_call const *call,
               uint32_t                 fn)
{
	uint32_t i = 0;
	while (i < call->n && g->to[call->to + i] != fn)
		++i;
	return i < call->n;
}

/* Appends to @p out the rows of the calls of the function @p fn of @p g in
 * the code of the function @p by. A row is @p key, a tab, the file, a tab,
 * the lines of the calls of one row (build_call_same()), the function
 * @p by with its lines, and in brackets a note for each of: the macro that
 * the calls are written through, the attribute cleanup, a callee that is
 * one of several, and a call that the program maybe does not evaluate. A
 * call that the program does not evaluate has no callee.
 */
static void
build_callee_calls (struct cstr_view         key,
                    struct graph const      *g,
                    uint32_t                 fn,
                    struct graph_fn const   *by,
                    struct cxi_out          *out)
{
	struct graph_call const *calls = &g->calls[by->calls];
	for (uint32_t i = 0; i < by->n_calls; ++i) {
		struct graph_call const *call = &calls[i];
		if (!build_call_to(g, call, fn))
			continue;

		/* A row is appended for its first call. */
		uint32_t j = 0;
		while (j < i && !(build_call_to(g, &calls[j], fn) &&
		                  build_call_same(g, &calls[j], call)))
			++j;
		if (j < i)
			continue;

		uint32_t line = call->line;
		cxi_out_view(out, key);
		cxi_out_lit(out, "\t");
		cxi_out_path(out, cstr_view(graph_text(g, call->file)));
		cxi_out_lit(out, "\t");
		cxi_out_num(out, line);
		for (j = i + 1; j < by->n_calls; ++j) {
			if (calls[j].line == line ||
			    !build_call_to(g, &calls[j], fn) ||
			    !build_call_same(g, &calls[j], call))
				continue;
			line = calls[j].line;
			cxi_out_lit(out, ",");
			cxi_out_num(out, line);
		}
		cxi_out_lit(out, " ");
		cxi_out_text(out, graph_text(g, by->name));
		cxi_out_lit(out, " (");
		cxi_out_range(out, by->defined.first, by->defined.last);
		cxi_out_lit(out, ")");
		if (call->macro != GRAPH_NONE) {
			cxi_out_lit(out, " [macro ");
			cxi_out_text(out, graph_text(g, call->macro));
			cxi_out_lit(out, "]");
		}
		if (call->flags & GRAPH_CALL_CLEANUP)
			cxi_out_lit(out, " [attribute cleanup]");
		if (call->n > 1)
			cxi_out_lit(out, " [or another function]");
		if (call->state == GRAPH_UNPROVEN)
			cxi_out_lit(out, " [maybe not evaluated]");
		cxi_out_lit(out, "\n");
	}
}

/* Appends to @p out the rows of each function of @p g that @p callee
 * names: a row of the key of the function and two tabs, and the rows of the
 * calls of the function (build_callee_calls()). @p key is the text of the
 * key of one function at a time.
 *
 * The key is a letter and the place of the function as FILE:FIRST-LAST.
 * The place is that of the definition in the translation unit, or of the
 * first declaration if there is none. The letter is S for a static
 * function and E for any other one, in lower case if the translation unit
 * does not define the function: the definition of an e is then in another
 * translation unit, or in none.
 */
static void
build_callee_rows (struct build_callee const *callee,
                   struct graph const        *g,
                   struct cxi_out            *key,
                   struct cxi_out            *out)
{
	for (uint32_t i = 0; i < g->n_fns; ++i) {
		struct graph_fn const *fn = &g->fns[i];
		if ((fn->flags & callee->mask) != callee->flags ||
		    !build_text_is(graph_text(g, fn->name), callee->name) ||
		    (callee->flags &&
		     !build_text_is(graph_text(g, fn->declared.file), callee->file)))
			continue;

		bool const defined = fn->defined.file != GRAPH_NONE;
		struct graph_place const *at = defined ? &fn->defined
		                                       : &fn->declared;
		bool const is_static = fn->flags & GRAPH_FN_STATIC;
		if (!dstr_clear(&key->text))
			key->e = EINVAL;
		cxi_out_put(key, &"SsEe"[2 * !is_static + !defined], 1);
		cxi_out_path(key, cstr_view(graph_text(g, at->file)));
		cxi_out_lit(key, ":");
		cxi_out_range(key, at->first, at->last);
		cxi_out_view(out, cstr_view_dstr(&key->text));
		cxi_out_lit(out, "\t\t\n");
		for (uint32_t by = 0; by < g->n_fns; ++by)
			build_callee_calls(cstr_view_dstr(&key->text), g, i,
			                   &g->fns[by], out);
	}
}

/* A row of callers (build_callee_rows()): the key of the callee, the file,
 * the text after the second tab, and the first line of the text. The row
 * of a callee without a call is the key and two tabs.
 */
struct build_row {
	struct cstr_view callee;
	struct cstr_view file;
	struct cstr_view text;
	uint64_t         line;
};

/* Orders the rows of callers by callee, file, line and text. */
static int
build_row_order (void const *a,
                 void const *b)
{
	struct build_row const *p = a;
	struct build_row const *q = b;
	int const callee = cstr_view_compare(p->callee, q->callee);
	int const file = callee ? callee : cstr_view_compare(p->file, q->file);
	return file ? file
	     : p->line != q->line ? (p->line > q->line) - (p->line < q->line)
	     : cstr_view_compare(p->text, q->text);
}

/* Appends the rows @p text of callers for the functions @p name: for each
 * callee the name and the place, and under it the file and the text of each
 * row, in the order of the files and the lines. A row that several
 * translation units give is appended once. A callee e
 * (build_callee_rows()) is the one E of the rows. If the rows contain no E
 * or several, the callee is the declaration, as is a static function
 * without a definition: the place is then appended after the word declared.
 * Without a row, the function is not found.
 */
static void
build_rows (struct cxi_out   *out,
            struct cstr_view  name,
            struct cstr_view  text)
{
	size_t const n = cstr_view_count(text, '\n');
	if (!n) {
		cxi_out_lit(out, "- callee: ");
		cxi_out_view(out, name);
		cxi_out_lit(out, " (not found)\n");
		return;
	}
	struct build_row *row = malloc(n * sizeof *row);
	if (!row) {
		out->e = ENOMEM;
		return;
	}
	/* defined is an E of the rows, and several is true if the rows contain
	 * more than one. */
	struct cstr_view defined = {};
	bool several = false;
	for (size_t i = 0; i < n; ++i) {
		struct build_row *r = &row[i];
		*r = (struct build_row){};
		cstr_view_cut(&text, '\n', &r->text);
		cstr_view_cut(&r->text, '\t', &r->callee);
		cstr_view_cut(&r->text, '\t', &r->file);
		(void)util_digits(r->text.data, r->text.len, UINT64_MAX, &r->line);
		if (!cstr_view_starts(r->callee, CSTR_VIEW("E")))
			continue;
		several = several || (defined.data && !cstr_view_equal(r->callee, defined));
		defined = r->callee;
	}
	for (size_t i = 0; defined.data && !several && i < n; ++i) {
		if (cstr_view_starts(row[i].callee, CSTR_VIEW("e")))
			row[i].callee = defined;
	}
	qsort(row, n, sizeof *row, build_row_order);
	bool bare = false;
	for (size_t i = 0; i < n; ++i) {
		struct build_row const *r = &row[i];
		bool const first = !i || !cstr_view_equal(r->callee, r[-1].callee);
		if (first) {
			cxi_out_lit(out, "- callee: ");
			cxi_out_view(out, name);
			cxi_out_view(out, r->callee.len && *r->callee.data >= 'a'
			             ? CSTR_VIEW(" (declared ") : CSTR_VIEW(" ("));
			cxi_out_view(out, cstr_view_slice(r->callee, 1, SIZE_MAX));
			cxi_out_lit(out, ")\n");
			bare = true;
		}
		if (r->text.len && (first || !cstr_view_equal(r->file, r[-1].file) ||
		                    !cstr_view_equal(r->text, r[-1].text))) {
			if (bare)
				cxi_out_lit(out, "  - callers\n");
			bare = false;
			cxi_out_lit(out, "    - ");
			cxi_out_view(out, r->file);
			cxi_out_lit(out, ":");
			/* The byte after the text of a row is the line feed of the
			 * row. */
			cxi_out_put(out, r->text.data, r->text.len + 1);
		}
		if (bare && (i + 1 == n || !cstr_view_equal(r->callee, r[1].callee)))
			cxi_out_lit(out, "  - callers: none\n");
	}
	free(row);
}

/* Appends to @p out a line of @p before and the files of the units of @p b
 * that are in the state @p state and whose graph has the flags @p flags.
 * Appends nothing if there is no such unit.
 */
static void
build_units (struct build const *b,
             struct cstr_view    before,
             uint32_t            state,
             uint64_t            flags,
             struct cxi_out     *out)
{
	bool any = false;
	for (size_t i = 0; i < b->n; ++i) {
		struct build_unit const *unit = &b->items[i];
		if (unit->state != state || (unit->g.flags & flags) != flags)
			continue;
		cxi_out_view(out, any ? CSTR_VIEW(", ") : before);
		cxi_out_path(out, cstr_view(build_path(b, unit)));
		any = true;
	}
	if (any)
		cxi_out_lit(out, "\n");
}

void
build_callers (struct build const        *b,
               struct build_callee const *callee,
               struct cxi_out            *out)
{
	struct cxi_out rows = cxi_out(cstr_view(nullptr));
	struct cxi_out key = cxi_out(cstr_view(nullptr));
	size_t late = 0;
	/* The graph of a unit that is not BUILD_GRAPH is empty. */
	for (size_t i = 0; i < b->n; ++i) {
		uint32_t const state = b->items[i].state;
		late += state == BUILD_NONE || state == BUILD_PARSING;
		build_callee_rows(callee, &b->items[i].g, &key, &rows);
	}
	build_rows(out, callee->name, cstr_view_dstr(&rows.text));
	build_units(b, CSTR_VIEW("- not parsed: "), BUILD_FAILED, 0, out);
	build_units(b, CSTR_VIEW("- parsed with errors: "), BUILD_GRAPH,
	            GRAPH_ERRORS, out);
	if (late)
		cxi_out_fmt(out, "- not searched: %zu translation unit%s (time limit)\n",
		            late, util_s(late));
	out->e = out->e ? out->e : rows.e ? rows.e : key.e;
	cxi_out_fini(&key);
	cxi_out_fini(&rows);
}
