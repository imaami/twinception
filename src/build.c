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
           struct cstr_view  path,
           struct cstr_view  words)
{
	size_t const at = buf_peek(&b->text).len;
	int e = buf_put(&b->text, path.data, path.len, UINT32_MAX);
	if (!e)
		e = buf_put(&b->text, "", 1, UINT32_MAX);
	if (!e)
		e = buf_put(&b->text, words.data, words.len, UINT32_MAX);
	if (!e)
		e = util_push(*b, (struct build_unit){
			.path    = (uint32_t)at,
			.words   = (uint32_t)(at + path.len + 1),
			.n_words = (uint32_t)words.len,
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
 * file, state, flags, macro and callees.
 */
static bool
build_call_same (struct graph const      *g,
                 struct graph_call const *p,
                 struct graph_call const *q)
{
	return p->file  == q->file  &&
	       p->state == q->state &&
	       p->flags == q->flags &&
	       p->macro == q->macro &&
	       p->n     == q->n     &&
	       !memcmp(&g->to[p->to], &g->to[q->to], p->n * sizeof *g->to);
}

/* Appends to @p out the rows of the uses of the function @p fn of @p g.
 * The uses in the code of one function follow each other. A row is @p key,
 * a tab, the file, a tab, the lines of the calls of one row
 * (build_call_same()) in the code of one function, that function with its
 * lines, after the word macro if it is one, or the words at file scope,
 * and in brackets a note for each of: the macro that the calls are
 * written through, the attribute cleanup, a callee that is one of several,
 * and a call that the program maybe does not evaluate. A call that the
 * program does not evaluate has no callee.
 */
static void
build_callee_uses (struct cstr_view       key,
                   struct graph const    *g,
                   struct graph_fn const *fn,
                   struct cxi_out        *out)
{
	struct graph_use const *uses = fn->n_uses ? &g->uses[fn->uses] : nullptr;
	for (uint32_t i = 0; i < fn->n_uses; ++i) {
		struct graph_call const *call = &g->calls[uses[i].call];
		uint32_t const by = uses[i].by;
		/* A row is appended for its first call. */
		uint32_t j = i;
		while (j && uses[j - 1].by == by &&
		       !build_call_same(g, &g->calls[uses[j - 1].call], call))
			--j;
		if (j && uses[j - 1].by == by)
			continue;

		uint32_t line = call->line;
		cxi_out_view(out, key);
		cxi_out_lit(out, "\t");
		cxi_out_path(out, cstr_view(graph_text(g, call->file)));
		cxi_out_lit(out, "\t");
		cxi_out_num(out, line);
		for (j = i + 1; j < fn->n_uses && uses[j].by == by; ++j) {
			struct graph_call const *next = &g->calls[uses[j].call];
			if (next->line == line || !build_call_same(g, next, call))
				continue;
			line = next->line;
			cxi_out_lit(out, ",");
			cxi_out_num(out, line);
		}
		if (by == GRAPH_NONE) {
			cxi_out_lit(out, " at file scope");
		} else {
			struct graph_fn const *in = &g->fns[by];
			cxi_out_view(out, in->flags & GRAPH_FN_MACRO ? CSTR_VIEW(" macro ")
			                                             : CSTR_VIEW(" "));
			cxi_out_text(out, graph_text(g, in->name));
			cxi_out_lit(out, " (");
			cxi_out_range(out, in->defined.first, in->defined.last);
			cxi_out_lit(out, ")");
		}
		if (call->macro != GRAPH_NONE) {
			cxi_out_lit(out, " [macro ");
			cxi_out_text(out, graph_text(g, call->macro));
			cxi_out_lit(out, "]");
		}
		if (call->flags & GRAPH_CALL_CLEANUP)
			cxi_out_lit(out, " [attribute cleanup]");
		if (call->n > 1)
			cxi_out_view(out, call->flags & GRAPH_CALL_MACRO
			             ? CSTR_VIEW(" [or another macro of the name]")
			             : CSTR_VIEW(" [or another function]"));
		if (call->state == GRAPH_UNPROVEN)
			cxi_out_lit(out, " [maybe not evaluated]");
		cxi_out_lit(out, "\n");
	}
}

/* Returns true if @p callee names the function @p fn of @p g. An
 * object-like macro is no callee. A macro or a static function that one
 * translation unit declares is the one of its file, and another function
 * the one of its name for the linker. The definitions of that function in
 * other files are left out after all rows are known (build_rows()).
 */
static bool
build_callee_is (struct build_callee const *callee,
                 struct graph const        *g,
                 struct graph_fn const     *fn)
{
	return !(fn->flags & GRAPH_FN_OBJECT) &&
	       (fn->flags & callee->mask) == callee->flags &&
	       (!callee->file.len || !(fn->flags & (GRAPH_FN_STATIC | GRAPH_FN_MACRO)) ||
	        build_text_is(graph_text(g, fn->declared.file), callee->file)) &&
	       (!callee->link.len || build_text_is(graph_link(g, fn), callee->link));
}

/* Returns the first name for the linker after @p after, in the order of
 * strcmp(), of the functions of @p b that @p callee names, or the first
 * one of all for a null @p after. Returns nullptr if there is none.
 */
static char const *
build_link_next (struct build const        *b,
                 struct build_callee const *callee,
                 char const                *after)
{
	char const *next = nullptr;
	for (size_t i = 0; i < b->n; ++i) {
		struct graph const *g = &b->items[i].g;
		uint32_t n;
		uint32_t const *named = graph_named(g, callee->name, &n);
		for (uint32_t k = 0; k < n; ++k) {
			struct graph_fn const *fn = &g->fns[named[k]];
			char const *link = graph_link(g, fn);
			if (build_callee_is(callee, g, fn) &&
			    (!after || strcmp(link, after) > 0) &&
			    (!next || strcmp(link, next) < 0))
				next = link;
		}
	}
	return next;
}

/* Appends to @p out the rows of each function of @p g that @p callee
 * names and that the linker knows as @p link: a row of the key of the
 * function and two tabs, and the rows of the uses of the function
 * (build_callee_uses()). @p key is the text of the key of one function at
 * a time.
 *
 * The key is a letter and the place of the function as FILE:FIRST-LAST.
 * The place is that of the definition in the translation unit, or of the
 * first declaration if there is none. The letter is M for a macro, S for
 * a static function and E for any other one, in lower case if the
 * translation unit does not define the function: the definition of an e is
 * then in another translation unit, or in none.
 */
static void
build_callee_rows (struct build_callee const *callee,
                   struct graph const        *g,
                   char const                *link,
                   struct cxi_out            *key,
                   struct cxi_out            *out)
{
	uint32_t n;
	uint32_t const *named = graph_named(g, callee->name, &n);
	for (uint32_t k = 0; k < n; ++k) {
		struct graph_fn const *fn = &g->fns[named[k]];
		bool const defined = fn->defined.file != GRAPH_NONE;
		bool const is_static = fn->flags & GRAPH_FN_STATIC;
		bool const is_macro = fn->flags & GRAPH_FN_MACRO;
		if (!build_callee_is(callee, g, fn) || strcmp(graph_link(g, fn), link))
			continue;

		struct graph_place const *at = defined ? &fn->defined
		                                       : &fn->declared;
		if (!dstr_clear(&key->text))
			key->e = EINVAL;
		cxi_out_put(key, is_macro ? "M" : &"SsEe"[2 * !is_static + !defined], 1);
		cxi_out_path(key, cstr_view(graph_text(g, at->file)));
		cxi_out_lit(key, ":");
		cxi_out_range(key, at->first, at->last);
		cxi_out_view(out, cstr_view_dstr(&key->text));
		cxi_out_lit(out, "\t\t\n");
		build_callee_uses(cstr_view_dstr(&key->text), g, fn, out);
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
 * The callees e are then one callee, with the place of one declaration.
 * The place of a macro is appended after the word macro. @p symbol
 * follows each callee as [symbol NAME] if it is not empty: the name of the
 * functions of the rows for the linker, if it is not @p name. Without a
 * row, the function is not found.
 *
 * @p only is empty, or E and the path of a file with a colon, as the
 * callee of a function that this file defines starts: the rows of a
 * function that another file defines are then left out, and a callee e is
 * the one E only if that E is of this file. A call in a translation unit
 * that only declares the function is thus not a call of a definition that
 * the caller did not ask for. If no row is left, the function is not
 * found.
 */
static void
build_rows (struct cxi_out   *out,
            struct cstr_view  name,
            struct cstr_view  symbol,
            struct cstr_view  text,
            struct cstr_view  only)
{
	size_t n = cstr_view_count(text, '\n');
	struct build_row *row = n ? malloc(n * sizeof *row) : nullptr;
	if (n && !row) {
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
	/* The rest of a callee of the file of only is the lines, without a
	 * colon. */
	#define BUILD_ONLY(callee)                                             \
	        (cstr_view_starts((callee), only) &&                           \
	         !memchr((callee).data + only.len, ':', (callee).len - only.len))
	bool const one = defined.data && !several && (!only.len || BUILD_ONLY(defined));
	/* Without the one E, the callees e are one function that is not
	 * static, also if the translation units first declare it in different
	 * headers: its place is the first of these in the order of the rows. */
	struct cstr_view declared = one ? defined : cstr_view(nullptr);
	for (size_t i = 0; !one && i < n; ++i) {
		if (cstr_view_starts(row[i].callee, CSTR_VIEW("e")) &&
		    (!declared.data || cstr_view_compare(row[i].callee, declared) < 0))
			declared = row[i].callee;
	}
	for (size_t i = 0; declared.data && i < n; ++i) {
		if (cstr_view_starts(row[i].callee, CSTR_VIEW("e")))
			row[i].callee = declared;
	}
	size_t kept = 0;
	for (size_t i = 0; i < n; ++i) {
		if (!only.len || !cstr_view_starts(row[i].callee, CSTR_VIEW("E")) ||
		    BUILD_ONLY(row[i].callee))
			row[kept++] = row[i];
	}
	#undef BUILD_ONLY
	n = kept;
	if (!n) {
		cxi_out_lit(out, "- callee: ");
		cxi_out_view(out, name);
		cxi_out_lit(out, " (not found)\n");
		free(row);
		return;
	}
	qsort(row, n, sizeof *row, build_row_order);
	bool bare = false;
	for (size_t i = 0; i < n; ++i) {
		struct build_row const *r = &row[i];
		bool const first = !i || !cstr_view_equal(r->callee, r[-1].callee);
		if (first) {
			cxi_out_lit(out, "- callee: ");
			cxi_out_view(out, name);
			char const letter = r->callee.len ? *r->callee.data : 'E';
			cxi_out_view(out, letter >= 'a' ? CSTR_VIEW(" (declared ")
			                  : letter == 'M' ? CSTR_VIEW(" (macro ")
			                  : CSTR_VIEW(" ("));
			cxi_out_view(out, cstr_view_slice(r->callee, 1, SIZE_MAX));
			if (symbol.len) {
				cxi_out_lit(out, ") [symbol ");
				cxi_out_path(out, symbol);
				cxi_out_lit(out, "]\n");
			} else {
				cxi_out_lit(out, ")\n");
			}
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
	struct cxi_out only = cxi_out(cstr_view(nullptr));
	/* The one definition of a function that is not static, for a callee
	 * that one translation unit declares. */
	if (callee->file.len && callee->mask && !callee->flags) {
		cxi_out_lit(&only, "E");
		cxi_out_path(&only, callee->file);
		cxi_out_lit(&only, ":");
	}
	/* The functions of one name for the linker are appended together,
	 * one such name after the other. The graph of a unit that is not
	 * BUILD_GRAPH is empty. */
	char const *link = build_link_next(b, callee, nullptr);
	if (!link)
		build_rows(out, callee->name, cstr_view(nullptr), cstr_view(nullptr),
		           cstr_view(nullptr));
	for (; link; link = build_link_next(b, callee, link)) {
		if (!dstr_clear(&rows.text))
			rows.e = EINVAL;
		for (size_t i = 0; i < b->n; ++i)
			build_callee_rows(callee, &b->items[i].g, link, &key, &rows);
		build_rows(out, callee->name,
		           build_text_is(link, callee->name) ? cstr_view(nullptr)
		                                             : cstr_view(link),
		           cstr_view_dstr(&rows.text), cstr_view_dstr(&only.text));
	}
	size_t late = 0;
	for (size_t i = 0; i < b->n; ++i) {
		uint32_t const state = b->items[i].state;
		late += state != BUILD_GRAPH && state != BUILD_FAILED;
	}
	build_units(b, CSTR_VIEW("- not parsed: "), BUILD_FAILED, 0, out);
	build_units(b, CSTR_VIEW("- parsed with errors: "), BUILD_GRAPH,
	            GRAPH_ERRORS, out);
	if (late)
		cxi_out_fmt(out, "- not searched: %zu translation unit%s (time limit)\n",
		            late, util_s(late));
	if (!out->e)
		out->e = rows.e ? rows.e : key.e ? key.e : only.e;
	cxi_out_fini(&only);
	cxi_out_fini(&key);
	cxi_out_fini(&rows);
}
