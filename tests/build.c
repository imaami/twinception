/** @file Tests of the translation units of a build, src/build.c: the answer
 *  of callers from graphs that the test writes without clang.
 */
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "build.h"
#include "check.h"
#include "cstr_view.h"
#include "cxi_out.h"
#include "dstr.h"
#include "graph.h"
#include "tool.h"

enum { MAX = 1 << 16 };

/* Ends the parse of the unit @p unit of @p b with the graph of @p draft,
 * through the bytes of a pipe, which they fit in. Frees @p draft.
 */
static void
parsed (struct build       *b,
        size_t              unit,
        struct graph_draft *draft)
{
	int fd[2];
	struct buf bytes = buf();
	CHECK(!pipe(fd));
	CHECK(!graph_draft_write(draft, fd[1]));
	close(fd[1]);
	CHECK(!buf_read(&bytes, fd[0], 0, MAX));
	close(fd[0]);
	build_parsed(b, unit, &bytes);
	CHECK(b->items[unit].state == BUILD_GRAPH && !buf_peek(&bytes).len);
	buf_fini(&bytes);
	graph_draft_fini(draft);
}

/* Returns the offset of the text @p s that is appended to @p draft. */
static uint32_t
text (struct graph_draft *draft,
      char const         *s)
{
	uint32_t at = 0;
	CHECK(!graph_draft_text(draft, cstr_view(s), &at));
	return at;
}

/* Appends to @p draft a call at @p line of @p file of the @p n functions
 * at @p to.
 */
static void
call (struct graph_draft *draft,
      uint32_t            file,
      uint32_t            line,
      uint32_t const     *to,
      uint32_t            n,
      struct graph_call   more)
{
	more.file = file;
	more.line = line;
	more.to = (uint32_t)draft->to.n;
	more.n = n;
	for (uint32_t i = 0; i < n; ++i)
		CHECK(!util_push(draft->to, to[i]));
	CHECK(!util_push(draft->calls, more));
}

/* Returns true if the answer of callers for @p callee in @p b is @p want.
 * Prints the answer if it is another one.
 */
static bool
answers (struct build const  *b,
         struct build_callee  callee,
         char const          *want)
{
	struct cxi_out out = cxi_out(cstr_view(nullptr));
	build_callers(b, &callee, &out);
	bool const same = !out.e && !strcmp(dstr_get(&out.text), want);
	if (!same)
		fprintf(stderr, "callers of %s:\n%s", callee.name.data, dstr_get(&out.text));
	cxi_out_fini(&out);
	return same;
}

int
main (void)
{
	struct build b = build();
	CHECK(!build_add(&b, 0, CSTR_VIEW("src/a.c")));
	CHECK(!build_add(&b, 2, CSTR_VIEW("src/b.c")));
	CHECK(!build_add(&b, 3, CSTR_VIEW("src/q\"x.c")));
	CHECK(!build_add(&b, 7, CSTR_VIEW("src/d.c")));
	CHECK(!build_add(&b, 8, CSTR_VIEW("src/e.c")));
	CHECK(b.n == 5 && b.items[1].index == 2 && b.items[3].state == BUILD_NONE);
	CHECK(!strcmp(build_path(&b, &b.items[0]), "src/a.c"));
	CHECK(!strcmp(build_path(&b, &b.items[3]), "src/d.c"));

	/* src/a.c defines f, which src/h.h declares, and g, which calls f on
	 * two lines. The static function inl of src/h.h calls f through the
	 * macro M. clang reported an error for the unit. */
	struct graph_draft d = graph_draft();
	uint32_t a_c = text(&d, "src/a.c");
	uint32_t h_h = text(&d, "src/h.h");
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "f"),
		.declared = { h_h, 1, 1 },
		.defined  = { a_c, 3, 5 },
	}));
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "g"),
		.declared = { a_c, 7, 7 },
		.defined  = { a_c, 7, 9 },
		.calls    = 0,
		.n_calls  = 2,
	}));
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "inl"),
		.flags    = GRAPH_FN_STATIC,
		.declared = { h_h, 4, 4 },
		.defined  = { h_h, 4, 4 },
		.calls    = 2,
		.n_calls  = 1,
	}));
	uint32_t const f = 0;
	call(&d, a_c, 8, &f, 1, (struct graph_call){ .macro = GRAPH_NONE });
	call(&d, a_c, 9, &f, 1, (struct graph_call){ .macro = GRAPH_NONE });
	call(&d, h_h, 4, &f, 1, (struct graph_call){ .macro = text(&d, "M") });
	d.flags = GRAPH_ERRORS;
	parsed(&b, 0, &d);

	/* src/b.c declares f and other and does not define them. k calls f in
	 * code that the program maybe does not evaluate, f or other, and f for
	 * the attribute cleanup of a variable. The unit contains inl of
	 * src/h.h as src/a.c does. */
	d = graph_draft();
	uint32_t b_c = text(&d, "src/b.c");
	h_h = text(&d, "src/h.h");
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "f"),
		.declared = { h_h, 1, 1 },
		.defined  = { GRAPH_NONE, 0, 0 },
	}));
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "other"),
		.declared = { h_h, 2, 2 },
		.defined  = { GRAPH_NONE, 0, 0 },
	}));
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "k"),
		.declared = { b_c, 2, 2 },
		.defined  = { b_c, 2, 6 },
		.calls    = 0,
		.n_calls  = 3,
	}));
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "inl"),
		.flags    = GRAPH_FN_STATIC,
		.declared = { h_h, 4, 4 },
		.defined  = { h_h, 4, 4 },
		.calls    = 3,
		.n_calls  = 1,
	}));
	uint32_t const either[] = { 0, 1 };
	call(&d, b_c, 3, either, 1, (struct graph_call){
		.macro = GRAPH_NONE,
		.state = GRAPH_UNPROVEN,
	});
	call(&d, b_c, 4, either, 2, (struct graph_call){ .macro = GRAPH_NONE });
	call(&d, b_c, 5, either, 1, (struct graph_call){
		.macro = GRAPH_NONE,
		.flags = GRAPH_CALL_CLEANUP,
	});
	call(&d, h_h, 4, either, 1, (struct graph_call){ .macro = text(&d, "M") });
	parsed(&b, 1, &d);

	/* clang could not parse the third unit, and bytes that are no graph
	 * leave a unit as that. The fourth unit is not parsed, and a parse of
	 * the fifth one runs. */
	b.items[4].state = BUILD_PARSING;
	build_parsed(&b, 2, nullptr);
	CHECK(b.items[2].state == BUILD_FAILED);
	struct buf junk = buf();
	CHECK(!buf_put(&junk, "junk", 4, MAX));
	build_parsed(&b, 2, &junk);
	CHECK(b.items[2].state == BUILD_FAILED && buf_peek(&junk).len == 4);
	buf_fini(&junk);

	/* The callee of src/b.c is the definition of src/a.c. The rows are in
	 * the order of the files and the lines, and the row of inl, which both
	 * units give, is there once. The closing lines name the unit that is
	 * not parsed, with its path quoted, the unit with errors, and the
	 * number of the units without a parse that has ended. */
#define CLOSED(rows) rows "- not parsed: \"src/q\\\"x.c\"\n"   \
                          "- parsed with errors: src/a.c\n"     \
                          "- not searched: 2 translation units (time limit)\n"
	CHECK(answers(&b, (struct build_callee){ .name = CSTR_VIEW("f") }, CLOSED(
		"- callee: f (src/a.c:3-5)\n"
		"  - callers\n"
		"    - src/a.c:8,9 g (7-9)\n"
		"    - src/b.c:3 k (2-6) [maybe not evaluated]\n"
		"    - src/b.c:4 k (2-6) [or another function]\n"
		"    - src/b.c:5 k (2-6) [attribute cleanup]\n"
		"    - src/h.h:4 inl (4) [macro M]\n")));
	/* A function that no unit defines is a declaration. */
	CHECK(answers(&b, (struct build_callee){ .name = CSTR_VIEW("other") }, CLOSED(
		"- callee: other (declared src/h.h:2)\n"
		"  - callers\n"
		"    - src/b.c:4 k (2-6) [or another function]\n")));
	/* A function without a call, and a name of no function. A name that
	 * starts with the name of a function is another name. */
	CHECK(answers(&b, (struct build_callee){ .name = CSTR_VIEW("g") }, CLOSED(
		"- callee: g (src/a.c:7-9)\n"
		"  - callers: none\n")));
	CHECK(answers(&b, (struct build_callee){ .name = CSTR_VIEW("in") }, CLOSED(
		"- callee: in (not found)\n")));
	CHECK(answers(&b, (struct build_callee){ .name = CSTR_VIEW("inline") }, CLOSED(
		"- callee: inline (not found)\n")));
	/* The static function that two units contain is one callee. With a
	 * mask, the callee is the static function of one file, or only a
	 * function that is not static. */
	static char const inl[] = CLOSED(
		"- callee: inl (src/h.h:4)\n"
		"  - callers: none\n");
	CHECK(answers(&b, (struct build_callee){ .name = CSTR_VIEW("inl") }, inl));
	CHECK(answers(&b, (struct build_callee){
		.name  = CSTR_VIEW("inl"),
		.file  = CSTR_VIEW("src/h.h"),
		.mask  = GRAPH_FN_STATIC,
		.flags = GRAPH_FN_STATIC,
	}, inl));
	CHECK(answers(&b, (struct build_callee){
		.name  = CSTR_VIEW("inl"),
		.file  = CSTR_VIEW("src/h"),
		.mask  = GRAPH_FN_STATIC,
		.flags = GRAPH_FN_STATIC,
	}, CLOSED("- callee: inl (not found)\n")));
	CHECK(answers(&b, (struct build_callee){
		.name = CSTR_VIEW("inl"),
		.mask = GRAPH_FN_STATIC,
	}, CLOSED("- callee: inl (not found)\n")));
#undef CLOSED

	/* An answer of more than TOOL_TEXT_MAX bytes is cut after a whole
	 * line, and a note says so. A shorter one is the text. */
	static char const line[] = "0123456789abcdef0123456789abcde\n";
	static char const note[] = "\n[The answer is cut at 4 MiB.]";
	struct cxi_out out = cxi_out(cstr_view(nullptr));
	cxi_out_lit(&out, "short\n");
	CHECK(cstr_view_equal(cxi_out_answer(&out), CSTR_VIEW("short\n")));
	while (dstr_len(&out.text) <= TOOL_TEXT_MAX)
		cxi_out_put(&out, line, sizeof line - 1);
	struct cstr_view const cut = cxi_out_answer(&out);
	CHECK(!out.e && cut.len <= TOOL_TEXT_MAX);
	CHECK(cut.len > TOOL_TEXT_MAX - sizeof line - sizeof note);
	CHECK(cstr_view_ends(cut, cstr_view_n(note, sizeof note - 1)));
	CHECK(cut.data[cut.len - sizeof note] == 'e');
	cxi_out_fini(&out);

	/* A build of parsed units without errors has no closing lines. */
	build_fini(&b);
	CHECK(!b.n && !b.items && !buf_peek(&b.text).len);
	CHECK(answers(&b, (struct build_callee){ .name = CSTR_VIEW("f") },
	              "- callee: f (not found)\n"));
	return check_report("build");
}
