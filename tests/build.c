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
	CHECK(!build_add(&b, CSTR_VIEW("src/a.c"), CSTR_VIEW("clang\0src/a.c\0")));
	CHECK(!build_add(&b, CSTR_VIEW("src/b.c"), CSTR_VIEW("clang\0-O2\0src/b.c\0")));
	CHECK(!build_add(&b, CSTR_VIEW("src/q\"x.c"), CSTR_VIEW("c\0")));
	CHECK(!build_add(&b, CSTR_VIEW("src/d.c"), CSTR_VIEW("c\0")));
	CHECK(!build_add(&b, CSTR_VIEW("src/e.c"), CSTR_VIEW("c\0")));
	CHECK(!build_add(&b, CSTR_VIEW("src/c.c"), CSTR_VIEW("c\0")));
	CHECK(b.n == 6 && b.items[3].state == BUILD_NONE && build_waits(b.items[3].state));
	CHECK(!strcmp(build_path(&b, &b.items[0]), "src/a.c"));
	CHECK(!strcmp(build_path(&b, &b.items[3]), "src/d.c"));
	CHECK(cstr_view_equal(build_words(&b, &b.items[1]), CSTR_VIEW("clang\0-O2\0src/b.c\0")));

	/* src/a.c defines f, which src/h.h declares, and g, which calls f on
	 * two lines, and in a line of src/h.h that the code of g includes. The
	 * static function inl of src/h.h calls f through the macro M. clang
	 * reported an error for the unit. */
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
		.n_calls  = 3,
	}));
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "inl"),
		.flags    = GRAPH_FN_STATIC,
		.declared = { h_h, 4, 4 },
		.defined  = { h_h, 4, 4 },
		.calls    = 3,
		.n_calls  = 2,
	}));
	/* The macro MAC of src/h.h is used in inl, in the definitions of the
	 * macro WRAP and of the object-like macro OBJ, and in the code of no
	 * function. src/h.h defines a second macro MAC, and the use in WRAP is
	 * of one of the two. */
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "MAC"),
		.flags    = GRAPH_FN_MACRO,
		.declared = { h_h, 30, 31 },
		.defined  = { h_h, 30, 31 },
	}));
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "WRAP"),
		.flags    = GRAPH_FN_MACRO,
		.declared = { h_h, 33, 33 },
		.defined  = { h_h, 33, 33 },
		.calls    = 5,
		.n_calls  = 1,
	}));
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "OBJ"),
		.flags    = GRAPH_FN_MACRO | GRAPH_FN_OBJECT,
		.declared = { h_h, 35, 35 },
		.defined  = { h_h, 35, 35 },
		.calls    = 7,
		.n_calls  = 1,
	}));
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "MAC"),
		.flags    = GRAPH_FN_MACRO,
		.declared = { h_h, 50, 50 },
		.defined  = { h_h, 50, 50 },
	}));
	uint32_t const f = 0;
	uint32_t const mac = 3;
	uint32_t const macs[] = { 3, 6 };
	call(&d, a_c, 8, &f, 1, (struct graph_call){ .macro = GRAPH_NONE });
	call(&d, h_h, 20, &f, 1, (struct graph_call){ .macro = GRAPH_NONE });
	call(&d, a_c, 9, &f, 1, (struct graph_call){ .macro = GRAPH_NONE });
	call(&d, h_h, 4, &f, 1, (struct graph_call){ .macro = text(&d, "M") });
	call(&d, h_h, 4, &mac, 1, (struct graph_call){
		.macro = GRAPH_NONE,
		.flags = GRAPH_CALL_MACRO,
	});
	call(&d, h_h, 33, macs, 2, (struct graph_call){
		.macro = GRAPH_NONE,
		.flags = GRAPH_CALL_MACRO,
	});
	call(&d, a_c, 40, &mac, 1, (struct graph_call){
		.macro = GRAPH_NONE,
		.flags = GRAPH_CALL_MACRO,
	});
	call(&d, h_h, 35, &mac, 1, (struct graph_call){
		.macro = GRAPH_NONE,
		.flags = GRAPH_CALL_MACRO,
	});
	d.flags = GRAPH_ERRORS;
	parsed(&b, 0, &d);

	/* src/b.c declares f and other and does not define them. k calls f in
	 * code that the program maybe does not evaluate, f or other, and f for
	 * the attribute cleanup of a variable. The unit contains inl of
	 * src/h.h as src/a.c does, and a function g of its own. */
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
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "g"),
		.declared = { b_c, 8, 8 },
		.defined  = { b_c, 8, 8 },
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

	/* src/c.c declares g, which it does not define, and calls it in m. */
	d = graph_draft();
	uint32_t const c_c = text(&d, "src/c.c");
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "g"),
		.declared = { text(&d, "src/h.h"), 9, 9 },
		.defined  = { GRAPH_NONE, 0, 0 },
	}));
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "m"),
		.declared = { c_c, 1, 1 },
		.defined  = { c_c, 1, 3 },
		.n_calls  = 1,
	}));
	call(&d, c_c, 2, (uint32_t const[]){ 0 }, 1, (struct graph_call){ .macro = GRAPH_NONE });
	parsed(&b, 5, &d);

	/* clang could not parse the third unit, and bytes that are no graph
	 * leave a unit as that. The fourth unit is to be parsed once more, and
	 * a parse of the fifth one runs. */
	b.items[3].state = BUILD_AGAIN;
	b.items[4].state = BUILD_PARSING;
	CHECK(build_waits(BUILD_RETRY) && !build_waits(BUILD_AGAIN) &&
	      !build_waits(BUILD_PARSING) && !build_waits(BUILD_GRAPH) &&
	      !build_waits(BUILD_FAILED));
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
		"    - src/h.h:4 inl (4) [macro M]\n"
		"    - src/h.h:20 g (7-9)\n")));
	/* A function that no unit defines is a declaration. */
	CHECK(answers(&b, (struct build_callee){ .name = CSTR_VIEW("other") }, CLOSED(
		"- callee: other (declared src/h.h:2)\n"
		"  - callers\n"
		"    - src/b.c:4 k (2-6) [or another function]\n")));
	/* Two files define a function g, and a third one only declares and
	 * calls it: the tool cannot tell which definition that call is of.
	 * With the file of a definition, the function that another file
	 * defines is no callee, and the call stays one of the declaration. */
	CHECK(answers(&b, (struct build_callee){ .name = CSTR_VIEW("g") }, CLOSED(
		"- callee: g (src/a.c:7-9)\n"
		"  - callers: none\n"
		"- callee: g (src/b.c:8)\n"
		"  - callers: none\n"
		"- callee: g (declared src/h.h:9)\n"
		"  - callers\n"
		"    - src/c.c:2 m (1-3)\n")));
	CHECK(answers(&b, (struct build_callee){
		.name = CSTR_VIEW("g"),
		.file = CSTR_VIEW("src/b.c"),
		.mask = GRAPH_FN_STATIC | GRAPH_FN_MACRO,
	}, CLOSED(
		"- callee: g (src/b.c:8)\n"
		"  - callers: none\n"
		"- callee: g (declared src/h.h:9)\n"
		"  - callers\n"
		"    - src/c.c:2 m (1-3)\n")));
	CHECK(answers(&b, (struct build_callee){
		.name = CSTR_VIEW("other"),
		.file = CSTR_VIEW("src/b.c"),
		.mask = GRAPH_FN_STATIC | GRAPH_FN_MACRO,
	}, CLOSED(
		"- callee: other (declared src/h.h:2)\n"
		"  - callers\n"
		"    - src/b.c:4 k (2-6) [or another function]\n")));
	/* A macro is a callee, with the word macro before its place. A use is
	 * in a function, in the definition of a macro, or at file scope. An
	 * object-like macro is a caller and no callee. With a mask, the callee
	 * is the macro of one file. */
	static char const mac_rows[] = CLOSED(
		"- callee: MAC (macro src/h.h:30-31)\n"
		"  - callers\n"
		"    - src/a.c:40 at file scope\n"
		"    - src/h.h:4 inl (4)\n"
		"    - src/h.h:33 macro WRAP (33) [or another macro of the name]\n"
		"    - src/h.h:35 macro OBJ (35)\n"
		"- callee: MAC (macro src/h.h:50)\n"
		"  - callers\n"
		"    - src/h.h:33 macro WRAP (33) [or another macro of the name]\n");
	CHECK(answers(&b, (struct build_callee){ .name = CSTR_VIEW("OBJ") },
	              CLOSED("- callee: OBJ (not found)\n")));
	CHECK(answers(&b, (struct build_callee){ .name = CSTR_VIEW("MAC") }, mac_rows));
	CHECK(answers(&b, (struct build_callee){
		.name  = CSTR_VIEW("MAC"),
		.file  = CSTR_VIEW("src/h.h"),
		.mask  = GRAPH_FN_STATIC | GRAPH_FN_MACRO,
		.flags = GRAPH_FN_MACRO,
	}, mac_rows));
	CHECK(answers(&b, (struct build_callee){
		.name = CSTR_VIEW("MAC"),
		.mask = GRAPH_FN_STATIC | GRAPH_FN_MACRO,
	}, CLOSED("- callee: MAC (not found)\n")));
	/* A name of no function. A name that starts with the name of a
	 * function is another name. */
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
		.mask  = GRAPH_FN_STATIC | GRAPH_FN_MACRO,
		.flags = GRAPH_FN_STATIC,
	}, inl));
	CHECK(answers(&b, (struct build_callee){
		.name  = CSTR_VIEW("inl"),
		.file  = CSTR_VIEW("src/h"),
		.mask  = GRAPH_FN_STATIC | GRAPH_FN_MACRO,
		.flags = GRAPH_FN_STATIC,
	}, CLOSED("- callee: inl (not found)\n")));
	CHECK(answers(&b, (struct build_callee){
		.name = CSTR_VIEW("inl"),
		.mask = GRAPH_FN_STATIC | GRAPH_FN_MACRO,
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

	/* A build without the unit that defines g in src/b.c: another file
	 * defines g, in a path that starts as src/b.c does, and src/c.c only
	 * declares and calls it. For the g of src/b.c, the other definition is
	 * no callee, and the call stays one of the declaration. Without the
	 * unit of src/c.c, no row is left, and the function is not found. */
	struct build part = build();
	CHECK(!build_add(&part, CSTR_VIEW("src/b.c:x.c"), CSTR_VIEW("c\0")));
	CHECK(!build_add(&part, CSTR_VIEW("src/c.c"), CSTR_VIEW("c\0")));
	d = graph_draft();
	uint32_t const other = text(&d, "src/b.c:x.c");
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "g"),
		.declared = { other, 8, 8 },
		.defined  = { other, 8, 8 },
	}));
	parsed(&part, 0, &d);
	struct build_callee const of_b = {
		.name = CSTR_VIEW("g"),
		.file = CSTR_VIEW("src/b.c"),
		.mask = GRAPH_FN_STATIC | GRAPH_FN_MACRO,
	};
	CHECK(answers(&part, of_b,
	              "- callee: g (not found)\n"
	              "- not searched: 1 translation unit (time limit)\n"));
	d = graph_draft();
	uint32_t const c_file = text(&d, "src/c.c");
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "g"),
		.declared = { text(&d, "src/h.h"), 9, 9 },
		.defined  = { GRAPH_NONE, 0, 0 },
	}));
	CHECK(!util_push(d.fns, (struct graph_fn){
		.name     = text(&d, "m"),
		.declared = { c_file, 1, 1 },
		.defined  = { c_file, 1, 3 },
		.n_calls  = 1,
	}));
	call(&d, c_file, 2, (uint32_t const[]){ 0 }, 1, (struct graph_call){ .macro = GRAPH_NONE });
	parsed(&part, 1, &d);
	CHECK(answers(&part, of_b,
	              "- callee: g (declared src/h.h:9)\n"
	              "  - callers\n"
	              "    - src/c.c:2 m (1-3)\n"));
	CHECK(answers(&part, (struct build_callee){ .name = CSTR_VIEW("g") },
	              "- callee: g (src/b.c:x.c:8)\n"
	              "  - callers\n"
	              "    - src/c.c:2 m (1-3)\n"));
	build_fini(&part);

	/* Two units declare a function first in different headers, and none
	 * defines it: it is one callee. */
	struct build two = build();
	CHECK(!build_add(&two, CSTR_VIEW("src/p.c"), CSTR_VIEW("c\0")));
	CHECK(!build_add(&two, CSTR_VIEW("src/q.c"), CSTR_VIEW("c\0")));
	for (uint32_t i = 0; i < 2; ++i) {
		d = graph_draft();
		uint32_t const file = text(&d, i ? "src/q.c" : "src/p.c");
		CHECK(!util_push(d.fns, (struct graph_fn){
			.name     = text(&d, "lib"),
			.declared = { text(&d, i ? "/usr/x.h" : "/usr/y.h"), 5 + i, 5 + i },
			.defined  = { GRAPH_NONE, 0, 0 },
		}));
		CHECK(!util_push(d.fns, (struct graph_fn){
			.name     = text(&d, i ? "q" : "p"),
			.declared = { file, 1, 1 },
			.defined  = { file, 1, 3 },
			.n_calls  = 1,
		}));
		call(&d, file, 2, (uint32_t const[]){ 0 }, 1,
		     (struct graph_call){ .macro = GRAPH_NONE });
		parsed(&two, i, &d);
	}
	CHECK(answers(&two, (struct build_callee){ .name = CSTR_VIEW("lib") },
	              "- callee: lib (declared /usr/x.h:6)\n"
	              "  - callers\n"
	              "    - src/p.c:2 p (1-3)\n"
	              "    - src/q.c:2 q (1-3)\n"));
	build_fini(&two);

	/* A build of parsed units without errors has no closing lines. */
	build_fini(&b);
	CHECK(!b.n && !b.items && !buf_peek(&b.text).len);
	CHECK(answers(&b, (struct build_callee){ .name = CSTR_VIEW("f") },
	              "- callee: f (not found)\n"));
	return check_report("build");
}
