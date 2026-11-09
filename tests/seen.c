/** @file Unit tests of the ring of the calls of an ask call (src/seen.c):
 *  the lines that a window shows whole, the calls that the ring stores and
 *  drops by count and by bytes, the newest call, calls too large to store,
 *  the offset of the window of a replayed result, and the reuse of the
 *  storage.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "buf.h"
#include "check.h"
#include "dstr.h"
#include "review.h"
#include "seen.h"

/* A commit ID. */
#define A "55aca1e8682351e4c39a92b040721def3f005402"

/* Check that the @p len bytes at @p got are the string @p want. */
#define SAME(got, len, want) same(__LINE__, got, len, want)

static void
same (int         line,
      void const *got,
      size_t      len,
      char const *want)
{
	size_t const n = strlen(want);
	if (got && len == n && !memcmp(got, want, n))
		return;
	++failures;
	fprintf(stderr, "%s:%d: got\n%.*s\nwant\n%s\n", __FILE__, line, (int)len,
	        got ? (char const *)got : "", want);
}

/* Write to @p out a git read output: the header "[h]\n", lines first to
 * last as "N\tline N", and a footer. Store the source of the lines in
 * @p src. If @p bare is true, the last line ends without a newline.
 */
static void
output (struct dstr        *out,
        struct call_source *src,
        uint32_t            first,
        uint32_t            last,
        bool                bare)
{
	(void)dstr_clear(out);
	CHECK(dstr_printf(out, "[h]\n"));
	size_t const at = dstr_len(out);
	for (uint32_t n = first; n <= last; ++n)
		CHECK(dstr_printf(out, "%u\tline %u%s", n, n, n == last && bare ? "" : "\n"));
	size_t const len = dstr_len(out) - at;
	CHECK(dstr_printf(out, "\n[f]"));
	*src = (struct call_source){ CSTR_VIEW("proj"), CSTR_VIEW(A), CSTR_VIEW("src/a.c"), 9,
	                             at, len, first, last };
}

/* Check that the window of @p window bytes from @p skip of the output of
 * lines 1 to 9 shows lines @p first to @p last whole, from a new mark.
 */
static void
lines_in (size_t   skip,
          size_t   window,
          uint32_t first,
          uint32_t last,
          int      line)
{
	struct dstr out = DSTR_INIT;
	struct call_source src;
	output(&out, &src, 1, 9, false);
	int e = -1;
	struct seen_mark mark = {};
	struct seen_lines const got = seen_lines(&mark, &src, cstr_view_dstr(&out), skip, window,
	                                         &e);
	if (e || got.first != first || got.last != last) {
		++failures;
		fprintf(stderr, "%s:%d: lines %u-%u, error %d, want %u-%u\n", __FILE__, line,
		        got.first, got.last, e, first, last);
	}
	dstr_cleanup(&out);
}

/* seen_lines() returns the lines that a window shows whole. */
static void
test_lines (void)
{
	/* "[h]\n" is 4 bytes, and each line "N\tline N\n" of lines 1 to 9 is 9. */
	lines_in(0, SIZE_MAX, 1, 9, __LINE__);
	lines_in(0, 4 + 9 * 3 + 4, 1, 3, __LINE__);          /* cut in line 4 */
	lines_in(0, 4 + 9 * 3, 1, 3, __LINE__);              /* ends after a newline */
	lines_in(0, 4 + 9 * 3 - 1, 1, 2, __LINE__);          /* before one */
	lines_in(4 + 9 * 2 + 3, SIZE_MAX, 4, 9, __LINE__);   /* starts in line 3 */
	lines_in(4 + 9 * 3, SIZE_MAX, 4, 9, __LINE__);       /* at a line's start */
	lines_in(4 + 9 * 3, 9 * 2, 4, 5, __LINE__);          /* a page of two lines */
	lines_in(2, SIZE_MAX, 1, 9, __LINE__);               /* starts in the header */
	/* Windows that show no whole line. */
	lines_in(0, 3, 0, 0, __LINE__);
	lines_in(0, 4 + 8, 0, 0, __LINE__);
	lines_in(4 + 9 * 9, SIZE_MAX, 0, 0, __LINE__);
	lines_in(4 + 9 * 2 + 3, 9, 0, 0, __LINE__);
	lines_in(SIZE_MAX, SIZE_MAX, 0, 0, __LINE__);

	/* A read paged forward and then back to its start. A window counts its
	 * lines from the mark of the window before it, or from the start of the
	 * read if that mark is after the window. */
	struct dstr out = DSTR_INIT;
	struct call_source src;
	output(&out, &src, 1, 9, false);
	struct cstr_view o = cstr_view_dstr(&out);
	int e = -1;
	struct seen_mark mark = {};
	struct seen_lines got = seen_lines(&mark, &src, o, 4 + 9 * 4, 9 * 2, &e);
	CHECK(!e && got.first == 5 && got.last == 6 && mark.at == 4 + 9 * 4 && mark.line == 5);
	got = seen_lines(&mark, &src, o, 4 + 9 * 6, 9 * 2, &e);
	CHECK(!e && got.first == 7 && got.last == 8 && mark.at == 4 + 9 * 6 && mark.line == 7);
	got = seen_lines(&mark, &src, o, 4, 9 * 2, &e);
	CHECK(!e && got.first == 1 && got.last == 2 && mark.at == 4 && mark.line == 1);
	/* A mark in the middle of a line counts that line. */
	got = seen_lines(&mark, &src, o, 4 + 9 * 2 + 3, 9 * 2, &e);
	CHECK(!e && got.first == 4 && got.last == 4 && mark.line == 3);

	/* A last line without a newline counts only if the window shows its
	 * end. */
	output(&out, &src, 1, 3, true);
	o = cstr_view_dstr(&out);
	mark = (struct seen_mark){};
	got = seen_lines(&mark, &src, o, 0, src.at + src.len - 1, &e);
	CHECK(!e && got.first == 1 && got.last == 2);
	got = seen_lines(&mark, &src, o, 0, src.at + src.len, &e);
	CHECK(!e && got.first == 1 && got.last == 3);
	/* Numbered lines that are empty or not within the output, and more
	 * lines than the source says. */
	got = seen_lines(&mark, &src, (struct cstr_view){ o.data, src.at + src.len - 1 }, 0,
	                 SIZE_MAX, &e);
	CHECK(e == EINVAL && !got.first && !got.last);
	struct call_source bad = src;
	bad.len = 0;
	e = -1;
	CHECK(!seen_lines(&mark, &bad, o, 0, SIZE_MAX, &e).first && e == EINVAL);
	bad = src;
	bad.at = SIZE_MAX;
	e = -1;
	CHECK(!seen_lines(&mark, &bad, o, 0, SIZE_MAX, &e).first && e == EINVAL);
	struct dstr more = DSTR_INIT;
	CHECK(dstr_printf(&more, "[h]\n"));
	for (int i = 0; i < 13; ++i)
		CHECK(dstr_printf(&more, "a\n"));
	bad = (struct call_source){ CSTR_VIEW("proj"), CSTR_VIEW(A), CSTR_VIEW("src/a.c"), 0, 4,
	                            dstr_len(&more) - 4, 1, 3 };
	mark = (struct seen_mark){};
	e = -1;
	CHECK(!seen_lines(&mark, &bad, cstr_view_dstr(&more), 0, SIZE_MAX, &e).first &&
	      e == EINVAL);
	/* Null arguments. */
	e = -1;
	CHECK(!seen_lines(nullptr, &src, o, 0, SIZE_MAX, &e).first && e == EFAULT);
	e = -1;
	CHECK(!seen_lines(&mark, nullptr, o, 0, SIZE_MAX, &e).first && e == EFAULT);
	e = -1;
	CHECK(!seen_lines(&mark, &src, (struct cstr_view){ nullptr, 2 }, 0, SIZE_MAX, &e).first &&
	      e == EFAULT);
	CHECK(!seen_lines(&mark, &src, o, 0, SIZE_MAX, nullptr).first);
	dstr_cleanup(&more);
	dstr_cleanup(&out);
}

/* Fill @p request with @p len bytes of arguments, all 'a'. */
static void
request_of (struct buf *request,
            size_t      len)
{
	struct span room;
	CHECK(!buf_reserve(request, len, SIZE_MAX, &room));
	memset(room.ptr, 'a', len);
	buf_commit(request, len);
}

/* Add the call @p number with @p asked bytes of arguments and the output
 * @p out, all of it the window, to @p *p_seen. Returns the error of
 * seen_keep().
 */
static int
keep (struct seen     **p_seen,
      uint32_t          number,
      size_t            asked,
      struct cstr_view  out)
{
	struct buf request = {};
	request_of(&request, asked);
	struct cstr_view const parts[SEEN_PARTS] = { out, CSTR_VIEW("\n\n[askai: call c?]") };
	struct seen_result const r = {
		.parts = parts,
		.out = out,
		.output = out.len,
		.number = number,
		.turn = number,
		.tool = 3,
	};
	int e = seen_keep(p_seen, &request, &r, nullptr);
	buf_fini(&request);
	return e;
}

/* Returns the call i of the n calls of @p seen, from the oldest. */
static struct seen_call const *
call_at (struct seen const *seen,
         uint32_t           i)
{
	return &seen->calls[(seen->head + i) % SEEN_CALLS];
}

/* seen_keep() stores the arguments, the result, the identifiers and the
 * lines of a call, and passes the lines to the caller.
 */
static void
test_keep (void)
{
	struct dstr out = DSTR_INIT;
	struct call_source src;
	output(&out, &src, 1, 9, false);
	struct cstr_view const o = cstr_view_dstr(&out);
	int e = -1;
	struct seen_read *read = seen_read_create(&src, o, &e);
	CHECK(read && !e);
	struct review *review = review_create(REVIEW_READ_LOG, &e);
	CHECK(review && !e);
	struct buf request = {};
	request_of(&request, 100);
	unsigned char const *data = request.data;
	size_t const skip = 4 + 9 * 2;
	struct cstr_view parts[SEEN_PARTS] = {
		{ o.data + skip, 9 * 3 + 2 },
		CSTR_VIEW("\n\n[askai: call c7]"),
		CSTR_VIEW("\n\n"),
		CSTR_VIEW("[Job 1 ended.]"),
	};
	struct seen_result r = {
		.parts  = parts,
		.out    = o,
		.read   = read,
		.output = 1000,
		.skip   = skip,
		.number = 7,
		.turn   = 3,
		.tool   = 5,
		.index  = 2,
		.flags  = SEEN_FAILED,
	};
	struct seen *seen = nullptr;
	struct seen_lines lines = { 1, 1 };
	CHECK(!seen_keep(&seen, &request, &r, &lines));
	CHECK(seen && seen->n == 1 && seen->head == 0);
	/* The storage of the arguments moved into the ring. */
	CHECK(!request.data && !request.cap && !buf_peek(&request).len);
	struct seen_call const *c = call_at(seen, 0);
	CHECK(c->request.data == data && buf_peek(&c->request).len == 100);
	SAME(dstr_get(&c->result), dstr_len(&c->result),
	     "3\tline 3\n4\tline 4\n5\tline 5\n6\t\n\n[askai: call c7]\n\n[Job 1 ended.]");
	SAME(dstr_get(&c->ids), dstr_len(&c->ids),
	     "{\"repo\":\"proj\",\"commit\":\"" A "\",\"path\":\"src/a.c\",\"total\":9}");
	CHECK(c->output == 1000 && c->skip == skip && c->number == 7 && c->turn == 3 &&
	      c->tool == 5 && c->index == 2 && c->flags == SEEN_FAILED);
	CHECK(c->lines.first == 3 && c->lines.last == 5);
	CHECK(lines.first == 3 && lines.last == 5);
	CHECK(seen->bytes == 100 + dstr_len(&c->result));
	/* The caller passes the lines to the review record. */
	CHECK(!review_pending_add(review, read, lines, r.turn));
	CHECK(!review_pending_log(review));
	struct dstr text = DSTR_INIT;
	CHECK(!review_render(review, &text, SIZE_MAX));
	CHECK(strstr(dstr_get(&text), "src/a.c  lines 3-5: 3 of 9 lines"));
	dstr_cleanup(&text);

	/* A replayed result: out is the part of the output that the journal
	 * recorded, from byte 5000 of the output on. The ring stores the
	 * offset of the window in the whole output. */
	request_of(&request, 10);
	parts[0] = (struct cstr_view){ o.data + 4, 9 };
	parts[2] = parts[3] = (struct cstr_view){};
	r = (struct seen_result){ .parts = parts, .out = o, .read = read, .output = 6000,
	                          .at = 5000, .skip = 4, .number = 8, .turn = 4 };
	CHECK(!seen_keep(&seen, &request, &r, &lines));
	c = call_at(seen, 1);
	CHECK(c->skip == 5004 && c->output == 6000 && c->lines.first == 1 && c->lines.last == 1);
	CHECK(lines.first == 1 && lines.last == 1);
	SAME(dstr_get(&c->result), dstr_len(&c->result), "1\tline 1\n\n\n[askai: call c7]");

	/* A call without lines, identifiers or notices. */
	request_of(&request, 10);
	parts[0] = CSTR_VIEW("plain");
	parts[1] = (struct cstr_view){};
	r = (struct seen_result){ .parts = parts, .out = parts[0], .output = 5, .number = 9,
	                          .turn = 4, .tool = TOOL_MAX };
	CHECK(!seen_keep(&seen, &request, &r, &lines));
	c = call_at(seen, 2);
	CHECK(seen->n == 3 && !c->lines.first && !c->lines.last && !dstr_len(&c->ids) &&
	      c->tool == TOOL_MAX && !c->index && !c->flags && !lines.first && !lines.last);
	SAME(dstr_get(&c->result), dstr_len(&c->result), "plain");
	/* An empty output without data. */
	request_of(&request, 10);
	parts[0] = (struct cstr_view){};
	parts[1] = CSTR_VIEW("note");
	r = (struct seen_result){ .parts = parts, .number = 10 };
	CHECK(!seen_keep(&seen, &request, &r, nullptr));
	SAME(dstr_get(&call_at(seen, 3)->result), dstr_len(&call_at(seen, 3)->result), "note");

	/* Refusals. The ring stays as it was after a refused call, and no
	 * line comes back. */
	request_of(&request, 10);
	struct cstr_view const abc = CSTR_VIEW("abc");
	parts[0] = (struct cstr_view){ abc.data + 2, 2 };
	r = (struct seen_result){ .parts = parts, .out = abc, .skip = 2 };
	lines = (struct seen_lines){ 1, 1 };
	CHECK(seen_keep(&seen, &request, &r, &lines) == EINVAL && !lines.first && !lines.last);
	parts[0] = (struct cstr_view){};
	r = (struct seen_result){ .parts = parts, .out = abc, .skip = 4 };
	CHECK(seen_keep(&seen, &request, &r, &lines) == EINVAL);
	/* A window that is not the bytes of out from skip on. */
	parts[0] = (struct cstr_view){ abc.data, 2 };
	r = (struct seen_result){ .parts = parts, .out = abc, .skip = 1 };
	CHECK(seen_keep(&seen, &request, &r, &lines) == EINVAL);
	parts[0] = CSTR_VIEW("ab");
	r = (struct seen_result){ .parts = parts, .out = abc };
	CHECK(seen_keep(&seen, &request, &r, &lines) == EINVAL);
	parts[0] = (struct cstr_view){};
	r = (struct seen_result){ .parts = parts, .out = { nullptr, 2 } };
	CHECK(seen_keep(&seen, &request, &r, &lines) == EFAULT);
	r = (struct seen_result){ .out = abc };
	CHECK(seen_keep(&seen, &request, &r, &lines) == EFAULT);
	r = (struct seen_result){ .parts = parts };
	CHECK(seen_keep(nullptr, &request, &r, &lines) == EFAULT);
	CHECK(seen_keep(&seen, nullptr, &r, &lines) == EFAULT);
	lines = (struct seen_lines){ 1, 1 };
	CHECK(seen_keep(&seen, &request, nullptr, &lines) == EFAULT && !lines.first);
	/* Numbered lines that do not fit the output. */
	struct seen_read *short_read = seen_read_create(&src, o, &e);
	short_read->src.last = 4;
	parts[0] = o;
	r = (struct seen_result){ .parts = parts, .out = o, .read = short_read };
	CHECK(seen_keep(&seen, &request, &r, &lines) == EINVAL);
	seen_read_destroy(&short_read);
	CHECK(seen->n == 4 && buf_peek(&request).len == 10);
	buf_fini(&request);

	/* seen_clear() drops the calls. The storage of the results stays. */
	size_t const blocks = dstr_get_block_count(&seen->calls[0].result);
	size_t const kept = seen_kept(seen);
	CHECK(kept > sizeof *seen);
	seen_clear(seen);
	CHECK(!seen->n && !seen->bytes && !seen->head && !seen->calls[0].request.data);
	CHECK(dstr_get_block_count(&seen->calls[0].result) == blocks && !dstr_len(&seen->calls[0].result));
	CHECK(seen_kept(seen) < kept && seen_kept(seen) > sizeof *seen);
	seen_clear(nullptr);
	CHECK(!seen_kept(nullptr));
	seen_destroy(&seen);
	CHECK(!seen);
	seen_destroy(&seen);
	seen_destroy(nullptr);
	seen_read_destroy(&read);
	review_destroy(&review);
	dstr_cleanup(&out);
}

/* The ring drops the oldest calls first: past SEEN_CALLS calls, and past
 * SEEN_BYTES bytes. The newest call always stays. The storage of a dropped
 * result stays for the next call in the same place.
 */
static void
test_drop (void)
{
	/* A result longer than the in-place storage of a dstr. */
	char text[1000];
	memset(text, 'x', sizeof text);
	struct cstr_view const big = { text, sizeof text };
	struct seen *seen = nullptr;
	CHECK(!keep(&seen, 1, 16, big));
	char const *first = dstr_get(&seen->calls[0].result);
	for (uint32_t k = 2; k <= SEEN_CALLS; ++k)
		CHECK(!keep(&seen, k, 16, CSTR_VIEW("small")));
	CHECK(seen->n == SEEN_CALLS && seen->head == 0 && call_at(seen, 0)->number == 1);
	/* Call 65 goes into the place of call 1 and reuses the storage of the
	 * result. */
	CHECK(!keep(&seen, SEEN_CALLS + 1, 16, big));
	CHECK(seen->n == SEEN_CALLS && seen->head == 1);
	CHECK(call_at(seen, 0)->number == 2 && call_at(seen, SEEN_CALLS - 1)->number == SEEN_CALLS + 1);
	CHECK(dstr_get(&seen->calls[0].result) == first);
	for (uint32_t k = SEEN_CALLS + 2; k <= 2 * SEEN_CALLS + 6; ++k)
		CHECK(!keep(&seen, k, 16, CSTR_VIEW("small")));
	CHECK(seen->n == SEEN_CALLS && seen->head == 6);
	for (uint32_t i = 0; i < SEEN_CALLS; ++i)
		CHECK(call_at(seen, i)->number == SEEN_CALLS + 7 + i);
	size_t bytes = 0;
	for (uint32_t i = 0; i < SEEN_CALLS; ++i)
		bytes += buf_peek(&call_at(seen, i)->request).len + dstr_len(&call_at(seen, i)->result);
	CHECK(seen->bytes == bytes);
	seen_destroy(&seen);

	/* Calls of 3 MiB: five fit into 16 MiB, and the sixth drops the
	 * first. */
	static constexpr size_t three = 3U << 20;
	for (uint32_t k = 1; k <= 5; ++k)
		CHECK(!keep(&seen, k, three, CSTR_VIEW("r")));
	CHECK(seen->n == 5 && seen->bytes == 5 * (three + 19));
	CHECK(!keep(&seen, 6, three, CSTR_VIEW("r")));
	CHECK(seen->n == 5 && call_at(seen, 0)->number == 2 && call_at(seen, 4)->number == 6);
	/* No storage of arguments stays after a dropped call. */
	CHECK(!seen->calls[0].request.data && seen->calls[1].request.data);
	/* A call of 4 MiB less a little drops two more, and stays. */
	CHECK(!keep(&seen, 7, TOOL_TEXT_MAX - 50, CSTR_VIEW("r")));
	CHECK(seen->n == 4 && call_at(seen, 0)->number == 4 && call_at(seen, 3)->number == 7);
	CHECK(seen->bytes == 3 * (three + 19) + TOOL_TEXT_MAX - 50 + 19);
	CHECK(buf_peek(&call_at(seen, 3)->request).len == TOOL_TEXT_MAX - 50);

	/* A call larger than TOOL_TEXT_MAX stays as a number. The arguments
	 * stay with the caller. */
	struct buf request = {};
	request_of(&request, TOOL_TEXT_MAX);
	struct cstr_view const parts[SEEN_PARTS] = { CSTR_VIEW("r") };
	struct seen_result const r = { .parts = parts, .out = parts[0], .output = 1, .number = 8,
	                               .turn = 2, .tool = 1, .index = 7, .flags = SEEN_FAILED };
	size_t const before = seen->bytes;
	CHECK(!seen_keep(&seen, &request, &r, nullptr));
	struct seen_call const *c = call_at(seen, seen->n - 1);
	CHECK(seen->n == 5 && c->number == 8 && c->turn == 2 && c->tool == 1 && c->index == 7 &&
	      c->flags == (SEEN_FAILED | SEEN_TOO_LARGE) && c->output == 1);
	CHECK(!c->request.data && !dstr_len(&c->result) && seen->bytes == before);
	CHECK(buf_peek(&request).len == TOOL_TEXT_MAX);
	buf_fini(&request);
	/* A call of TOOL_TEXT_MAX bytes is not too large, and stays. */
	CHECK(!keep(&seen, 9, TOOL_TEXT_MAX - 18, CSTR_VIEW("")));
	c = call_at(seen, seen->n - 1);
	CHECK(c->number == 9 && !c->flags && buf_peek(&c->request).len == TOOL_TEXT_MAX - 18);
	CHECK(seen->bytes <= SEEN_BYTES);
	seen_destroy(&seen);
}

int
main (void)
{
	test_lines();
	test_keep();
	test_drop();
	return check_report("seen");
}
