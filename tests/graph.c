/** @file Tests of the bytes of the graph of a translation unit,
 *  src/graph.c: a draft that is written and read back, and bytes that
 *  graph() refuses.
 */
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "check.h"
#include "cstr_view.h"
#include "graph.h"

enum { MAX = 1 << 16 };

/* Writes @p draft to a pipe and reads the bytes into @p bytes. The bytes
 * of a test fit in the pipe.
 */
static int
written (struct graph_draft *draft,
         struct buf         *bytes)
{
	int fd[2];
	if (pipe(fd))
		return errno;
	int e = graph_draft_write(draft, fd[1]);
	close(fd[1]);
	e = e ? e : buf_read(bytes, fd[0], 0, MAX);
	close(fd[0]);
	return e;
}

/* Returns the error of graph() for the bytes of @p draft, with the
 * uint32_t at the offset @p at replaced by @p value if @p at is not
 * SIZE_MAX, and without the last @p cut bytes.
 */
static int
refused (struct graph_draft *draft,
         size_t              at,
         uint32_t            value,
         size_t              cut)
{
	struct buf bytes = buf();
	int e = written(draft, &bytes);
	CHECK(!e);
	struct span const all = buf_peek(&bytes);
	CHECK(cut <= all.len);
	if (at != SIZE_MAX && at + sizeof value <= all.len)
		memcpy(all.ptr + at, &value, sizeof value);
	buf_cut(&bytes, all.len - cut, cut);
	struct graph g = graph(&bytes, &e);
	/* The bytes of a refused graph stay with the caller, and the graph
	 * is empty. */
	CHECK(buf_peek(&bytes).len == (e ? all.len - cut : 0));
	CHECK(!e || (!g.fns && !g.bytes.data));
	graph_fini(&g);
	buf_fini(&bytes);
	return e;
}

int
main (void)
{
	struct graph_draft draft = graph_draft();
	uint32_t file;
	uint32_t f;
	uint32_t g_name;
	uint32_t macro;
	CHECK(!graph_draft_text(&draft, CSTR_VIEW("src/a.c"), &file));
	CHECK(!graph_draft_text(&draft, CSTR_VIEW("f"), &f));
	CHECK(!graph_draft_text(&draft, CSTR_VIEW("g"), &g_name));
	CHECK(!graph_draft_text(&draft, cstr_view(nullptr), &macro));
	CHECK(!graph_draft_text(&draft, CSTR_VIEW("M"), &macro));
	CHECK(file == 0 && f == 8 && g_name == 10 && macro == 13);

	/* f is declared and not defined. g is defined and calls f twice. */
	CHECK(!util_push(draft.fns, (struct graph_fn){
		.name     = f,
		.flags    = GRAPH_FN_STATIC,
		.declared = { file, 1, 1 },
		.defined  = { GRAPH_NONE, 0, 0 },
	}));
	CHECK(!util_push(draft.fns, (struct graph_fn){
		.name     = g_name,
		.declared = { file, 2, 2 },
		.defined  = { file, 3, 9 },
		.calls    = 0,
		.n_calls  = 2,
	}));
	CHECK(!util_push(draft.to, 0));
	CHECK(!util_push(draft.calls, (struct graph_call){
		.file  = file,
		.line  = 4,
		.to    = 0,
		.n     = 1,
		.macro = GRAPH_NONE,
	}));
	CHECK(!util_push(draft.calls, (struct graph_call){
		.file  = file,
		.line  = 5,
		.to    = 1,
		.n     = 0,
		.macro = macro,
		.state = GRAPH_UNPROVEN,
		.flags = GRAPH_CALL_CLEANUP,
	}));

	draft.flags = GRAPH_ERRORS;

	/* The graph of the bytes contains the flags and the records of the
	 * draft. */
	struct buf bytes = buf();
	int e = written(&draft, &bytes);
	CHECK(!e);
	struct graph g = graph(&bytes, &e);
	CHECK(!e && !buf_peek(&bytes).len);
	CHECK(g.flags == GRAPH_ERRORS);
	CHECK(g.n_fns == 2 && g.n_calls == 2 && g.n_to == 1 && g.n_text == 15);
	CHECK(!strcmp(graph_text(&g, g.fns[0].name), "f"));
	CHECK(g.fns[0].flags == GRAPH_FN_STATIC && g.fns[0].defined.file == GRAPH_NONE);
	CHECK(!strcmp(graph_text(&g, g.fns[1].name), "g"));
	CHECK(!strcmp(graph_text(&g, g.fns[1].defined.file), "src/a.c"));
	CHECK(g.fns[1].defined.first == 3 && g.fns[1].defined.last == 9);
	CHECK(g.fns[1].calls == 0 && g.fns[1].n_calls == 2);
	CHECK(g.calls[0].line == 4 && g.calls[0].n == 1 && g.to[g.calls[0].to] == 0);
	CHECK(g.calls[0].macro == GRAPH_NONE && g.calls[0].state == GRAPH_EVALUATED);
	CHECK(!strcmp(graph_text(&g, g.calls[1].macro), "M"));
	CHECK(g.calls[1].state == GRAPH_UNPROVEN && g.calls[1].flags == GRAPH_CALL_CLEANUP);
	/* The one use of f is the call 0 in the code of g. The names are in
	 * their order, and a name of the graph is found whole. */
	CHECK(g.n_uses == 1 && g.fns[0].n_uses == 1 && !g.fns[1].n_uses);
	CHECK(g.uses[g.fns[0].uses].call == 0 && g.uses[g.fns[0].uses].by == 1);
	CHECK(g.names[0] == 0 && g.names[1] == 1);
	uint32_t n = 0;
	uint32_t const *named = graph_named(&g, CSTR_VIEW("g"), &n);
	CHECK(n == 1 && named && *named == 1);
	CHECK(!graph_named(&g, CSTR_VIEW("ff"), &n) && !n);
	CHECK(!graph_named(&g, CSTR_VIEW("h"), &n) && !n);
	CHECK(!graph_named(&g, cstr_view(nullptr), &n) && !n);
	graph_fini(&g);
	CHECK(!g.fns && !g.n_text && !g.bytes.data);

	/* The bytes as they are written are a graph. */
	CHECK(!refused(&draft, SIZE_MAX, 0, 0));
	/* Bytes that end early, another magic, a flag that is none, and a count
	 * that does not agree with the length of the bytes. */
	size_t const head = 8 * sizeof(uint32_t);
	size_t const fn = sizeof(struct graph_fn);
	size_t const call = head + 2 * fn;
	size_t const to = call + 2 * sizeof(struct graph_call);
	size_t const uses = to + sizeof(uint32_t);
	size_t const names = uses + sizeof(struct graph_use);
	size_t const text = names + 2 * sizeof(uint32_t);
	CHECK(refused(&draft, SIZE_MAX, 0, 1) == EINVAL);
	CHECK(refused(&draft, SIZE_MAX, 0, 15) == EINVAL);
	CHECK(refused(&draft, SIZE_MAX, 0, text + 15 - 3) == EINVAL);
	CHECK(refused(&draft, SIZE_MAX, 0, text + 15) == EINVAL);
	CHECK(refused(&draft, 0, 0, 0) == EINVAL);
	CHECK(refused(&draft, sizeof(uint32_t), GRAPH_ERRORS << 1, 0) == EINVAL);
	CHECK(refused(&draft, 2 * sizeof(uint32_t), 3, 0) == EINVAL);
	/* A record that names a text, a call, a callee or a function that the
	 * graph does not contain, and a state that is none. The name is the
	 * first member of a function. */
	CHECK(refused(&draft, head, 15, 0) == EINVAL);
	CHECK(refused(&draft, head + offsetof(struct graph_fn, declared.file), 15, 0) == EINVAL);
	CHECK(refused(&draft, head + offsetof(struct graph_fn, defined.file), 15, 0) == EINVAL);
	CHECK(refused(&draft, head + fn + offsetof(struct graph_fn, calls), 3, 0) == EINVAL);
	CHECK(refused(&draft, head + fn + offsetof(struct graph_fn, n_calls), 3, 0) == EINVAL);
	CHECK(refused(&draft, head + fn + offsetof(struct graph_fn, n_calls), GRAPH_NONE, 0) ==
	      EINVAL);
	CHECK(refused(&draft, call + offsetof(struct graph_call, file), 15, 0) == EINVAL);
	CHECK(refused(&draft, call + offsetof(struct graph_call, to), 2, 0) == EINVAL);
	CHECK(refused(&draft, call + offsetof(struct graph_call, n), 2, 0) == EINVAL);
	CHECK(refused(&draft, call + offsetof(struct graph_call, macro), 15, 0) == EINVAL);
	CHECK(refused(&draft, call + offsetof(struct graph_call, state), 3, 0) == EINVAL);
	/* A call that the program does not evaluate is without a callee. */
	CHECK(refused(&draft, call + offsetof(struct graph_call, state), GRAPH_UNEVALUATED, 0) ==
	      EINVAL);
	CHECK(refused(&draft, to, 2, 0) == EINVAL);
	/* A use that names a call or a function that the graph does not
	 * contain, and uses of a function that it does not contain. */
	CHECK(refused(&draft, uses, 2, 0) == EINVAL);
	CHECK(refused(&draft, uses + sizeof(uint32_t), 2, 0) == EINVAL);
	CHECK(refused(&draft, head + offsetof(struct graph_fn, n_uses), 2, 0) == EINVAL);
	/* A name of a function that the graph does not contain, and names that
	 * are not in their order: f is then named as the file is. */
	CHECK(refused(&draft, names, 2, 0) == EINVAL);
	CHECK(refused(&draft, head, 0, 0) == EINVAL);
	/* A text part that does not end with a NUL: its last four bytes are
	 * replaced. */
	CHECK(refused(&draft, text + 15 - sizeof(uint32_t), 0x4d4d4d4d, 0) == EINVAL);

	/* An empty draft is a graph without records. */
	graph_draft_fini(&draft);
	CHECK(!draft.fns.items && !draft.text.n);
	e = written(&draft, &bytes);
	CHECK(!e);
	g = graph(&bytes, &e);
	CHECK(!e && !g.n_fns && !g.n_calls && !g.n_to && !g.n_text);
	graph_fini(&g);
	return check_report("graph");
}
