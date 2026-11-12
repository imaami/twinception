/** @file Unit tests of the ring of the calls of an ask call (src/seen.c):
 *  the lines that a window shows whole, the copy of a source and the JSON
 *  object of the copy, the calls that the ring stores and drops by count
 *  and by bytes, the newest call, calls too large to store, the offset of
 *  the window of a replayed result, the reuse of the storage, the calls
 *  before a restart, the lookup of a call by number, the evidence items
 *  that results showed, and the items in the journal state of a resumed ask
 *  call.
 */
#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include "buf.h"
#include "check.h"
#include "dstr.h"
#include "jb.h"
#include "oid.h"
#include "range.h"
#include "seen.h"
#include "tool.h"

/* A commit ID. */
#define A "55aca1e8682351e4c39a92b040721def3f005402"

/* A commit ID of a SHA-256 repository. */
#define B "7d865e959b2466918c9863afca942d0fb89d7c9ac0c99bafc3749504ded97730"

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
        struct seen_source *src,
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
	*src = (struct seen_source){ CSTR_VIEW("proj"), CSTR_VIEW("src/a.c"), { at, len }, 9,
	                             { first, last }, { A } };
}

/* Check that the window of @p window bytes from @p skip of the output of
 * lines 1 to 9 shows lines @p first to @p last whole, from a new read.
 */
static void
lines_in (size_t   skip,
          size_t   window,
          uint32_t first,
          uint32_t last,
          int      line)
{
	struct dstr out = DSTR_INIT;
	struct seen_source src;
	output(&out, &src, 1, 9, false);
	int e = -1;
	struct seen_read *read = seen_read_create(&src, cstr_view_dstr(&out), &e);
	struct seen_lines const got = seen_lines(read, cstr_view_dstr(&out),
	                                         (struct extent){ skip, window }, &e);
	if (!read || e || got.range.first != first || got.range.last != last) {
		++failures;
		fprintf(stderr, "%s:%d: lines %u-%u, error %d, want %u-%u\n", __FILE__, line,
		        got.range.first, got.range.last, e, first, last);
	}
	seen_read_destroy(&read);
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
	 * read if that mark is after the window. A new read marks the start of
	 * its numbered lines. */
	struct dstr out = DSTR_INIT;
	struct seen_source src;
	output(&out, &src, 1, 9, false);
	struct cstr_view o = cstr_view_dstr(&out);
	/* all is a window that contains the whole output. */
	struct extent const all = { 0, SIZE_MAX };
	int e = -1;
	struct seen_read *read = seen_read_create(&src, o, &e);
	CHECK(read && !e && read->mark.line == 1 && read->mark.at == src.output.at);
	struct seen_lines got = seen_lines(read, o, (struct extent){ 4 + 9 * 4, 9 * 2 }, &e);
	CHECK(!e && got.range.first == 5 && got.range.last == 6 && read->mark.at == 4 + 9 * 4 &&
	      read->mark.line == 5);
	CHECK(!got.at);
	got = seen_lines(read, o, (struct extent){ 4 + 9 * 6, 9 * 2 }, &e);
	CHECK(!e && got.range.first == 7 && got.range.last == 8 && read->mark.at == 4 + 9 * 6 &&
	      read->mark.line == 7);
	got = seen_lines(read, o, (struct extent){ 4, 9 * 2 }, &e);
	CHECK(!e && got.range.first == 1 && got.range.last == 2 && read->mark.at == 4 &&
	      read->mark.line == 1);
	/* A mark in the middle of a line counts that line. The first whole
	 * line starts 6 bytes into the window. */
	got = seen_lines(read, o, (struct extent){ 4 + 9 * 2 + 3, 9 * 2 }, &e);
	CHECK(!e && got.range.first == 4 && got.range.last == 4 && read->mark.line == 3 && got.at == 6);
	/* A window from the start of the output shows the header first. The
	 * mark is after the window, so the lines count from the start. */
	got = seen_lines(read, o, all, &e);
	CHECK(!e && got.range.first == 1 && got.range.last == 9 && got.at == 4 &&
	      read->mark.line == 1 && read->mark.at == src.output.at);
	seen_read_destroy(&read);

	/* A last line without a newline counts only if the window shows its
	 * end. */
	output(&out, &src, 1, 3, true);
	o = cstr_view_dstr(&out);
	read = seen_read_create(&src, o, &e);
	CHECK(read && !e);
	got = seen_lines(read, o, (struct extent){ 0, src.output.at + src.output.len - 1 }, &e);
	CHECK(!e && got.range.first == 1 && got.range.last == 2);
	got = seen_lines(read, o, (struct extent){ 0, src.output.at + src.output.len }, &e);
	CHECK(!e && got.range.first == 1 && got.range.last == 3);
	/* Numbered lines that are empty or not within the output. */
	got = seen_lines(read, (struct cstr_view){ o.data, src.output.at + src.output.len - 1 },
	                 all, &e);
	CHECK(e == EINVAL && !got.range.first && !got.range.last);
	read->src.output.len = 0;
	e = -1;
	CHECK(!seen_lines(read, o, all, &e).range.first && e == EINVAL);
	read->src.output.len = src.output.len;
	read->src.output.at = SIZE_MAX;
	e = -1;
	CHECK(!seen_lines(read, o, all, &e).range.first && e == EINVAL);
	read->src.output.at = src.output.at;
	e = -1;
	CHECK(seen_lines(read, o, all, &e).range.last == 3 && !e);
	seen_read_destroy(&read);
	/* More lines than the source says. */
	struct dstr more = DSTR_INIT;
	CHECK(dstr_printf(&more, "[h]\n"));
	for (int i = 0; i < 13; ++i)
		CHECK(dstr_printf(&more, "a\n"));
	struct seen_source const many = { CSTR_VIEW("proj"), CSTR_VIEW("src/a.c"),
	                                  { 4, dstr_len(&more) - 4 }, 0, { 1, 13 }, { A } };
	read = seen_read_create(&many, cstr_view_dstr(&more), &e);
	CHECK(read && !e);
	read->src.lines.last = 3;
	e = -1;
	CHECK(!seen_lines(read, cstr_view_dstr(&more), all, &e).range.first && e == EINVAL);
	/* Null arguments. */
	e = -1;
	CHECK(!seen_lines(nullptr, o, all, &e).range.first && e == EFAULT);
	e = -1;
	CHECK(!seen_lines(read, (struct cstr_view){ nullptr, 2 }, all, &e).range.first &&
	      e == EFAULT);
	CHECK(!seen_lines(read, o, all, nullptr).range.first);
	seen_read_destroy(&read);
	dstr_cleanup(&more);
	dstr_cleanup(&out);
}

/* Check that @p read is a copy of the source that output() stores for
 * lines 3 to 5, with the commit @p commit, and that the mark of @p read is
 * at the start of the lines.
 */
static void
read_is (struct seen_read const *read,
         char const             *commit,
         int                     line)
{
	if (read && read->src.repo.data == read->text && read->src.path.data == read->text + 4 &&
	    cstr_view_equal(read->src.repo, CSTR_VIEW("proj")) &&
	    cstr_view_equal(read->src.path, CSTR_VIEW("src/a.c")) &&
	    !strcmp(read->src.commit.hex, commit) && read->src.total == 9 &&
	    read->src.lines.first == 3 && read->src.lines.last == 5 && read->src.output.at == 4 &&
	    read->src.output.len == 9 * 3 && read->mark.line == 3 && read->mark.at == 4)
		return;
	++failures;
	fprintf(stderr, "%s:%d: not the read of lines 3 to 5 at %s\n", __FILE__, line, commit);
}

/* Returns the error of seen_read_create() for @p src in the output
 * @p out. Checks that seen_read_create() returns a read if and only if the
 * error is 0.
 */
static int
read_error (struct seen_source const *src,
            struct cstr_view          out)
{
	int e = -1;
	struct seen_read *read = seen_read_create(src, out, &e);
	CHECK(!read == !!e);
	seen_read_destroy(&read);
	return e;
}

/* Returns the error of seen_read_create_json() in the output @p out for a
 * JSON object of lines 3 to 5 without the member commit. @p members is the
 * JSON text of more members of the object, each after a comma, or "". A
 * member of @p members replaces an earlier member of the same name. If
 * @p commit is not nullptr, the function checks that the read is a copy of
 * the source with the commit @p commit.
 */
static int
read_json_error (char const       *members,
                 char const       *commit,
                 struct cstr_view  out)
{
	char text[256];
	int const n = snprintf(text, sizeof text, "{\"repo\":\"proj\",\"path\":\"src/a.c\",\"at\":4,"
	                       "\"len\":27,\"first\":3,\"last\":5,\"total\":9%s}", members);
	CHECK(n > 0 && n < (int)sizeof text);
	struct json_object *obj = json_tokener_parse(text);
	CHECK(obj);
	int e = -1;
	struct seen_read *read = seen_read_create_json(obj, out, &e);
	CHECK(!read == !!e);
	if (commit)
		read_is(read, commit, __LINE__);
	seen_read_destroy(&read);
	json_object_put(obj);
	return e;
}

/* seen_read_create() copies a source that fits the output, with the commit
 * by value, and refuses any other source. seen_read_json() writes the copy
 * as a JSON object, and seen_read_create_json() reads the object.
 */
static void
test_read (void)
{
	struct dstr out = DSTR_INIT;
	struct seen_source src;
	output(&out, &src, 3, 5, false);
	struct cstr_view const o = cstr_view_dstr(&out);
	/* The read stores a copy of the repository name and the path. The
	 * storage of the caller can change after the call. */
	char repo[] = "proj";
	char path[] = "src/a.c";
	src.repo = (struct cstr_view){ repo, sizeof repo - 1 };
	src.path = (struct cstr_view){ path, sizeof path - 1 };
	int e = -1;
	struct seen_read *read = seen_read_create(&src, o, &e);
	CHECK(read && !e);
	memset(repo, 'x', sizeof repo - 1);
	memset(path, 'y', sizeof path - 1);
	read_is(read, A, __LINE__);

	/* The JSON object of the read, and a read from that object. */
	struct json_object *obj = seen_read_json(read, &e);
	CHECK(obj && !e);
	SAME(jb_wire(obj).data, jb_wire(obj).len,
	     "{\"repo\":\"proj\",\"commit\":\"" A "\",\"path\":\"src/a.c\",\"at\":4,\"len\":27,"
	     "\"first\":3,\"last\":5,\"total\":9}");
	e = -1;
	struct seen_read *again = seen_read_create_json(obj, o, &e);
	CHECK(again && !e && again != read);
	read_is(again, A, __LINE__);
	seen_read_destroy(&again);
	CHECK(!again);
	/* The object does not fit a shorter output. */
	e = -1;
	CHECK(!seen_read_create_json(obj, (struct cstr_view){ o.data, 4 + 9 * 3 - 1 }, &e) &&
	      e == EINVAL);
	json_object_put(obj);
	seen_read_destroy(&read);
	e = -1;
	CHECK(!seen_read_json(nullptr, &e) && e == EFAULT);
	CHECK(!seen_read_json(nullptr, nullptr));
	e = -1;
	CHECK(!seen_read_create_json(nullptr, o, &e) && e == EFAULT);
	CHECK(!seen_read_create_json(nullptr, o, nullptr));

	/* The commit of an object is the name of a commit: 40 or 64 lowercase
	 * hexadecimal digits. */
	CHECK(!read_json_error(",\"commit\":\"" A "\"", A, o));
	CHECK(!read_json_error(",\"commit\":\"" B "\"", B, o));
	CHECK(read_json_error("", nullptr, o) == EINVAL);
	CHECK(read_json_error(",\"commit\":\"\"", nullptr, o) == EINVAL);
	CHECK(read_json_error(",\"commit\":\"" A "0\"", nullptr, o) == EINVAL);
	CHECK(read_json_error(",\"commit\":\"5aca1e8682351e4c39a92b040721def3f005402\"", nullptr,
	                      o) == EINVAL);
	CHECK(read_json_error(",\"commit\":\"55ACA1E8682351E4C39A92B040721DEF3F005402\"", nullptr,
	                      o) == EINVAL);
	CHECK(read_json_error(",\"commit\":\"g5aca1e8682351e4c39a92b040721def3f005402\"", nullptr,
	                      o) == EINVAL);
	CHECK(read_json_error(",\"commit\":55", nullptr, o) == EINVAL);
	CHECK(read_json_error(",\"commit\":null", nullptr, o) == EINVAL);
	CHECK(read_json_error(",\"commit\":[\"" A "\"]", nullptr, o) == EINVAL);
	/* An object with the commit and a member that is not a number in the
	 * range of the member, and an object for another output. */
	CHECK(read_json_error(",\"commit\":\"" A "\",\"total\":\"9\"", nullptr, o) == EINVAL);
	CHECK(read_json_error(",\"commit\":\"" A "\",\"first\":-3", nullptr, o) == EINVAL);
	CHECK(read_json_error(",\"commit\":\"" A "\",\"last\":4294967296", nullptr, o) == EINVAL);
	CHECK(read_json_error(",\"commit\":\"" A "\",\"at\":null", nullptr, o) == EINVAL);
	CHECK(read_json_error(",\"commit\":\"" A "\",\"len\":2.5", nullptr, o) == EINVAL);
	CHECK(read_json_error(",\"commit\":\"" A "\",\"repo\":7", nullptr, o) == EINVAL);
	CHECK(read_json_error(",\"commit\":\"" A "\"", nullptr, CSTR_VIEW("[h]\n")) == EINVAL);

	/* A source without a commit. An empty struct oid is no name. */
	src.repo = CSTR_VIEW("proj");
	src.path = CSTR_VIEW("src/a.c");
	CHECK(!read_error(&src, o));
	struct seen_source bad = src;
	bad.commit = (struct oid){};
	CHECK(read_error(&bad, o) == EINVAL);
	/* A commit of a SHA-256 repository. The identifiers of a call with the
	 * read contain the 64 digits. */
	bad.commit = (struct oid){ B };
	read = seen_read_create(&bad, o, &e);
	CHECK(read && !e);
	read_is(read, B, __LINE__);
	struct seen *seen = nullptr;
	struct buf request = {};
	struct cstr_view const parts[SEEN_PARTS] = { o };
	struct seen_result const r = { .parts = parts, .out = o, .read = read,
	                               .stamp = { .output = o.len, .number = 1, .turn = 1 } };
	CHECK(!seen_keep(&seen, &request, &r) && seen && seen->n == 1);
	SAME(dstr_get(&seen->calls[0].ids), dstr_len(&seen->calls[0].ids),
	     "{\"repo\":\"proj\",\"commit\":\"" B "\",\"path\":\"src/a.c\",\"total\":9}");
	CHECK(seen->calls[0].lines.range.first == 3 && seen->calls[0].lines.range.last == 5);
	seen_destroy(&seen);
	seen_read_destroy(&read);
	/* A repository name or a path that is empty, too long, or not UTF-8
	 * text without NUL. */
	char name[SEEN_NAME_MAX + 1];
	memset(name, 'n', sizeof name);
	bad = src;
	bad.repo = (struct cstr_view){};
	CHECK(read_error(&bad, o) == EINVAL);
	bad.repo = (struct cstr_view){ name, 0 };
	CHECK(read_error(&bad, o) == EINVAL);
	bad.repo = (struct cstr_view){ name, SEEN_NAME_MAX + 1 };
	CHECK(read_error(&bad, o) == EINVAL);
	bad.repo = (struct cstr_view){ name, SEEN_NAME_MAX };
	CHECK(!read_error(&bad, o));
	bad.path = (struct cstr_view){ name, SEEN_NAME_MAX + 1 };
	CHECK(read_error(&bad, o) == EINVAL);
	bad.path = (struct cstr_view){ name, SEEN_NAME_MAX };
	CHECK(!read_error(&bad, o));
	bad.path = CSTR_VIEW("a\xff");
	CHECK(read_error(&bad, o) == EINVAL);
	bad.path = (struct cstr_view){ "a\0b", 3 };
	CHECK(read_error(&bad, o) == EINVAL);
	bad.path = (struct cstr_view){};
	CHECK(read_error(&bad, o) == EINVAL);
	/* Line numbers that do not satisfy 1 <= first <= last <= total. A
	 * total of 0 is unknown. */
	bad = src;
	bad.lines = (struct range){ 0, 2 };
	CHECK(read_error(&bad, o) == EINVAL);
	bad.lines = (struct range){ 5, 3 };
	CHECK(read_error(&bad, o) == EINVAL);
	bad.lines = src.lines;
	bad.total = 4;
	CHECK(read_error(&bad, o) == EINVAL);
	bad.total = 5;
	CHECK(!read_error(&bad, o));
	bad.total = 0;
	CHECK(!read_error(&bad, o));
	bad.total = UINT32_MAX;
	CHECK(!read_error(&bad, o));
	bad.total = (uint64_t)UINT32_MAX + 1;
	CHECK(read_error(&bad, o) == EINVAL);
	/* Numbered lines that are empty, not within the output, or not one
	 * line for each number. */
	bad = src;
	bad.output.len = 0;
	CHECK(read_error(&bad, o) == EINVAL);
	bad.output = (struct extent){ o.len, 1 };
	CHECK(read_error(&bad, o) == EINVAL);
	bad.output = (struct extent){ 4, o.len - 3 };
	CHECK(read_error(&bad, o) == EINVAL);
	bad.output = (struct extent){ SIZE_MAX, 9 * 3 };
	CHECK(read_error(&bad, o) == EINVAL);
	bad.output = (struct extent){ 4, SIZE_MAX };
	CHECK(read_error(&bad, o) == EINVAL);
	bad.output = (struct extent){ 4, 9 * 2 };
	CHECK(read_error(&bad, o) == EINVAL);
	bad.output = src.output;
	bad.lines.last = 6;
	CHECK(read_error(&bad, o) == EINVAL);
	CHECK(read_error(&src, (struct cstr_view){}) == EINVAL);
	/* The last line can end without a newline. */
	bad.lines.last = 5;
	bad.output.len = 9 * 3 - 1;
	CHECK(!read_error(&bad, o));
	/* Null arguments. */
	CHECK(read_error(nullptr, o) == EFAULT);
	CHECK(read_error(&src, (struct cstr_view){ nullptr, 2 }) == EFAULT);
	CHECK(!seen_read_create(&src, o, nullptr));
	seen_read_destroy(nullptr);
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
		.stamp = { .output = out.len, .number = number, .turn = number, .tool = 3 },
	};
	int e = seen_keep(p_seen, &request, &r);
	buf_fini(&request);
	return e;
}

/* Returns the result of seen_resume() for the ring @p seen with @p calls
 * as a JSON number and the items @p items.
 */
static int
resume (struct seen              **seen,
        uint64_t                   calls,
        struct json_object const  *items)
{
	struct json_object *number = json_object_new_uint64(calls);
	CHECK(number);
	int const e = seen_resume(seen, number, items);
	json_object_put(number);
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
 * lines of a call. seen_notice() names the calls of the ring.
 */
static void
test_keep (void)
{
	struct dstr out = DSTR_INIT;
	struct seen_source src;
	output(&out, &src, 1, 9, false);
	struct cstr_view const o = cstr_view_dstr(&out);
	int e = -1;
	struct seen_read *read = seen_read_create(&src, o, &e);
	CHECK(read && !e);
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
	struct seen_stamp const stamp = {
		.output = 1000,
		.number = 7,
		.turn   = 3,
		.tool   = 5,
		.index  = 2,
		.flags  = SEEN_FAILED,
	};
	struct seen_result r = { .parts = parts, .out = o, .read = read, .stamp = stamp, .skip = skip };
	struct seen *seen = nullptr;
	CHECK(!seen_keep(&seen, &request, &r));
	CHECK(seen && seen->n == 1 && seen->head == 0);
	/* The storage of the arguments moved into the ring. */
	CHECK(!request.data && !request.cap && !buf_peek(&request).len);
	struct seen_call const *c = call_at(seen, 0);
	CHECK(c->request.data == data && buf_peek(&c->request).len == 100);
	SAME(dstr_get(&c->result), dstr_len(&c->result),
	     "3\tline 3\n4\tline 4\n5\tline 5\n6\t\n\n[askai: call c7]\n\n[Job 1 ended.]");
	SAME(dstr_get(&c->ids), dstr_len(&c->ids),
	     "{\"repo\":\"proj\",\"commit\":\"" A "\",\"path\":\"src/a.c\",\"total\":9}");
	/* The ring stores the stamp as the caller passed it. A stamp is 24
	 * bytes without padding. */
	static_assert(sizeof stamp == 24, "struct seen_stamp is not 24 bytes");
	CHECK(c->skip == skip && c->stamp.output == 1000 && c->stamp.number == 7 &&
	      c->stamp.turn == 3 && c->stamp.tool == 5 && c->stamp.index == 2 &&
	      c->stamp.flags == SEEN_FAILED);
	CHECK(!memcmp(&c->stamp, &stamp, sizeof stamp));
	/* The tool of a place of the ring without a call is TOOL_MAX. */
	CHECK(seen->calls[1].stamp.tool == TOOL_MAX && !seen->calls[1].stamp.number &&
	      !seen->calls[1].stamp.flags);
	CHECK(c->lines.range.first == 3 && c->lines.range.last == 5 && !c->lines.at);
	CHECK(seen->bytes == 100 + dstr_len(&c->result));

	/* A replayed result: out is the part of the output that the journal
	 * recorded, from byte 5000 of the output on. The ring stores the
	 * offset of the window in the whole output. */
	request_of(&request, 10);
	parts[0] = (struct cstr_view){ o.data + 4, 9 };
	parts[2] = parts[3] = (struct cstr_view){};
	r = (struct seen_result){ .parts = parts, .out = o, .read = read, .at = 5000, .skip = 4,
	                          .stamp = { .output = 6000, .number = 8, .turn = 4 } };
	CHECK(!seen_keep(&seen, &request, &r));
	c = call_at(seen, 1);
	CHECK(c->skip == 5004 && c->stamp.output == 6000 && c->lines.range.first == 1 &&
	      c->lines.range.last == 1);
	SAME(dstr_get(&c->result), dstr_len(&c->result), "1\tline 1\n\n\n[askai: call c7]");

	/* A call without lines, identifiers or notices. */
	request_of(&request, 10);
	parts[0] = CSTR_VIEW("plain");
	parts[1] = (struct cstr_view){};
	r = (struct seen_result){ .parts = parts, .out = parts[0],
	                          .stamp = { .output = 5, .number = 9, .turn = 4, .tool = TOOL_MAX } };
	CHECK(!seen_keep(&seen, &request, &r));
	c = call_at(seen, 2);
	CHECK(seen->n == 3 && !c->lines.range.first && !c->lines.range.last && !dstr_len(&c->ids) &&
	      c->stamp.tool == TOOL_MAX && !c->stamp.index && !c->stamp.flags);
	SAME(dstr_get(&c->result), dstr_len(&c->result), "plain");
	/* An empty output without data. seen_keep() copies SEEN_FAILED and no
	 * other flag of the stamp, so the call is not too large. */
	request_of(&request, 10);
	parts[0] = (struct cstr_view){};
	parts[1] = CSTR_VIEW("note");
	r = (struct seen_result){ .parts = parts,
	                          .stamp = { .number = 10, .flags = (uint16_t)~SEEN_FAILED } };
	CHECK(!seen_keep(&seen, &request, &r));
	c = call_at(seen, 3);
	CHECK(c->stamp.number == 10 && !c->stamp.flags && buf_peek(&c->request).len == 10);
	SAME(dstr_get(&c->result), dstr_len(&c->result), "note");
	request_of(&request, 10);
	r.stamp = (struct seen_stamp){ .number = 11, .flags = UINT16_MAX };
	CHECK(!seen_keep(&seen, &request, &r));
	c = call_at(seen, 4);
	CHECK(c->stamp.number == 11 && c->stamp.flags == SEEN_FAILED);

	/* Refusals. The ring stays as it was after a refused call. */
	request_of(&request, 10);
	struct cstr_view const abc = CSTR_VIEW("abc");
	parts[0] = (struct cstr_view){ abc.data + 2, 2 };
	r = (struct seen_result){ .parts = parts, .out = abc, .skip = 2 };
	CHECK(seen_keep(&seen, &request, &r) == EINVAL);
	parts[0] = (struct cstr_view){};
	r = (struct seen_result){ .parts = parts, .out = abc, .skip = 4 };
	CHECK(seen_keep(&seen, &request, &r) == EINVAL);
	/* A window that is not the bytes of out from skip on. */
	parts[0] = (struct cstr_view){ abc.data, 2 };
	r = (struct seen_result){ .parts = parts, .out = abc, .skip = 1 };
	CHECK(seen_keep(&seen, &request, &r) == EINVAL);
	parts[0] = CSTR_VIEW("ab");
	r = (struct seen_result){ .parts = parts, .out = abc };
	CHECK(seen_keep(&seen, &request, &r) == EINVAL);
	parts[0] = (struct cstr_view){};
	r = (struct seen_result){ .parts = parts, .out = { nullptr, 2 } };
	CHECK(seen_keep(&seen, &request, &r) == EFAULT);
	r = (struct seen_result){ .out = abc };
	CHECK(seen_keep(&seen, &request, &r) == EFAULT);
	r = (struct seen_result){ .parts = parts };
	CHECK(seen_keep(nullptr, &request, &r) == EFAULT);
	CHECK(seen_keep(&seen, nullptr, &r) == EFAULT);
	CHECK(seen_keep(&seen, &request, nullptr) == EFAULT);
	/* A window past TOOL_OUTPUT_MAX in the whole output. */
	r = (struct seen_result){ .parts = parts, .at = TOOL_OUTPUT_MAX, .stamp = { .number = 12 } };
	CHECK(!seen_keep(&seen, &request, &r));
	request_of(&request, 10);
	r = (struct seen_result){ .parts = parts, .out = abc, .at = TOOL_OUTPUT_MAX, .skip = 1 };
	CHECK(seen_keep(&seen, &request, &r) == EINVAL);
	r = (struct seen_result){ .parts = parts, .at = (uint64_t)TOOL_OUTPUT_MAX + 1 };
	CHECK(seen_keep(&seen, &request, &r) == EINVAL);
	/* Numbered lines that do not fit the output. */
	struct seen_read *short_read = seen_read_create(&src, o, &e);
	short_read->src.lines.last = 4;
	parts[0] = o;
	r = (struct seen_result){ .parts = parts, .out = o, .read = short_read };
	CHECK(seen_keep(&seen, &request, &r) == EINVAL);
	seen_read_destroy(&short_read);
	CHECK(seen->n == 6 && buf_peek(&request).len == 10 &&
	      call_at(seen, 5)->skip == TOOL_OUTPUT_MAX);
	buf_fini(&request);

	/* The notice of the calls that the ring keeps. */
	struct dstr text = DSTR_INIT;
	CHECK(!seen_notice(seen, &text));
	SAME(dstr_get(&text), dstr_len(&text), "[askai: askai keeps your calls c7 to c12 of this ask "
	     "call. Cite the calls as evidence by number.]\n");
	(void)dstr_clear(&text);
	CHECK(!seen_notice(nullptr, &text) && !dstr_len(&text));

	/* seen_clear() drops the calls. The storage of the results stays. */
	size_t const blocks = dstr_get_block_count(&seen->calls[0].result);
	size_t const kept = seen_kept(seen);
	CHECK(kept > sizeof *seen);
	seen_clear(seen);
	CHECK(!seen->n && !seen->bytes && !seen->head && !seen->calls[0].request.data);
	CHECK(!seen_notice(seen, &text) && !dstr_len(&text));
	CHECK(!keep(&seen, 4, 1, CSTR_VIEW("4")) && !seen_notice(seen, &text));
	SAME(dstr_get(&text), dstr_len(&text), "[askai: askai keeps your call c4 of this ask call. "
	     "Cite the call as evidence by number.]\n");
	dstr_cleanup(&text);
	seen_clear(seen);
	CHECK(dstr_get_block_count(&seen->calls[0].result) == blocks && !dstr_len(&seen->calls[0].result));
	CHECK(seen_kept(seen) < kept && seen_kept(seen) > sizeof *seen);
	seen_clear(nullptr);
	CHECK(!seen_kept(nullptr));
	seen_destroy(&seen);
	CHECK(!seen);
	seen_destroy(&seen);
	seen_destroy(nullptr);
	seen_read_destroy(&read);
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
	CHECK(seen->n == SEEN_CALLS && seen->head == 0 && call_at(seen, 0)->stamp.number == 1);
	/* Call 65 goes into the place of call 1 and reuses the storage of the
	 * result. */
	CHECK(!keep(&seen, SEEN_CALLS + 1, 16, big));
	CHECK(seen->n == SEEN_CALLS && seen->head == 1);
	CHECK(call_at(seen, 0)->stamp.number == 2 &&
	      call_at(seen, SEEN_CALLS - 1)->stamp.number == SEEN_CALLS + 1);
	CHECK(dstr_get(&seen->calls[0].result) == first);
	for (uint32_t k = SEEN_CALLS + 2; k <= 2 * SEEN_CALLS + 6; ++k)
		CHECK(!keep(&seen, k, 16, CSTR_VIEW("small")));
	CHECK(seen->n == SEEN_CALLS && seen->head == 6);
	for (uint32_t i = 0; i < SEEN_CALLS; ++i)
		CHECK(call_at(seen, i)->stamp.number == SEEN_CALLS + 7 + i);
	size_t bytes = 0;
	for (uint32_t i = 0; i < SEEN_CALLS; ++i)
		bytes += buf_peek(&call_at(seen, i)->request).len + dstr_len(&call_at(seen, i)->result);
	CHECK(seen->bytes == bytes);
	seen_destroy(&seen);

	/* Calls of 3 MiB: five fit into 16 MiB, and the sixth drops the
	 * first. */
	size_t const three = 3U << 20;
	for (uint32_t k = 1; k <= 5; ++k)
		CHECK(!keep(&seen, k, three, CSTR_VIEW("r")));
	CHECK(seen->n == 5 && seen->bytes == 5 * (three + 19));
	CHECK(!keep(&seen, 6, three, CSTR_VIEW("r")));
	CHECK(seen->n == 5 && call_at(seen, 0)->stamp.number == 2 &&
	      call_at(seen, 4)->stamp.number == 6);
	/* No storage of arguments stays after a dropped call. */
	CHECK(!seen->calls[0].request.data && seen->calls[1].request.data);
	/* A call of 4 MiB less a little drops two more, and stays. */
	CHECK(!keep(&seen, 7, TOOL_TEXT_MAX - 50, CSTR_VIEW("r")));
	CHECK(seen->n == 4 && call_at(seen, 0)->stamp.number == 4 &&
	      call_at(seen, 3)->stamp.number == 7);
	CHECK(seen->bytes == 3 * (three + 19) + TOOL_TEXT_MAX - 50 + 19);
	CHECK(buf_peek(&call_at(seen, 3)->request).len == TOOL_TEXT_MAX - 50);

	/* A call larger than TOOL_TEXT_MAX stays as a number. The arguments
	 * stay with the caller. */
	struct buf request = {};
	request_of(&request, TOOL_TEXT_MAX);
	struct cstr_view const parts[SEEN_PARTS] = { CSTR_VIEW("r") };
	struct seen_result const r = { .parts = parts, .out = parts[0],
	                               .stamp = { .output = 1, .number = 8, .turn = 2, .tool = 1,
	                                          .index = 7, .flags = SEEN_FAILED } };
	size_t const before = seen->bytes;
	CHECK(!seen_keep(&seen, &request, &r));
	struct seen_call const *c = call_at(seen, seen->n - 1);
	CHECK(seen->n == 5 && c->stamp.number == 8 && c->stamp.turn == 2 && c->stamp.tool == 1 &&
	      c->stamp.index == 7 && c->stamp.flags == (SEEN_FAILED | SEEN_TOO_LARGE) &&
	      c->stamp.output == 1);
	CHECK(!c->request.data && !dstr_len(&c->result) && seen->bytes == before);
	CHECK(buf_peek(&request).len == TOOL_TEXT_MAX);
	buf_fini(&request);
	/* A call of TOOL_TEXT_MAX bytes is not too large, and stays. */
	CHECK(!keep(&seen, 9, TOOL_TEXT_MAX - 18, CSTR_VIEW("")));
	c = call_at(seen, seen->n - 1);
	CHECK(c->stamp.number == 9 && !c->stamp.flags &&
	      buf_peek(&c->request).len == TOOL_TEXT_MAX - 18);
	CHECK(seen->bytes <= SEEN_BYTES);
	seen_destroy(&seen);
}

/* Check that seen_find() finds the call @p number in @p seen @p where, and
 * a call exactly if SEEN_KEPT.
 */
static void
found (struct seen const *seen,
       uint32_t           number,
       enum seen_where    where,
       int                line)
{
	struct seen_call const *c = (void *)1;
	enum seen_where const got = seen_find(seen, number, &c);
	if (got == where && (where == SEEN_KEPT ? c && c->stamp.number == number : !c))
		return;
	++failures;
	fprintf(stderr, "%s:%d: c%u found %d, want %d\n", __FILE__, line, number, got, where);
}

/* seen_find() finds the calls of the ring, and tells the calls that left
 * the ring, the calls before a restart and the other calls apart. A
 * findings or compact call takes no place in the ring, and seen_find()
 * finds it missing, as a call that did not run.
 */
static void
test_find (void)
{
	struct seen *seen = nullptr;
	found(seen, 1, SEEN_MISSING, __LINE__);
	CHECK(!seen_oldest(seen));
	/* Call 2 is a findings call. */
	CHECK(!keep(&seen, 1, 10, CSTR_VIEW("one")));
	CHECK(!keep(&seen, 3, 10, CSTR_VIEW("three")));
	CHECK(seen->n == 2 && call_at(seen, 0)->stamp.number == 1 &&
	      call_at(seen, 1)->stamp.number == 3);
	CHECK(seen->bytes == 2 * 10 + dstr_len(&call_at(seen, 0)->result) +
	                     dstr_len(&call_at(seen, 1)->result));
	struct seen_call const *c = (void *)1;
	CHECK(seen_find(seen, 2, &c) == SEEN_MISSING && !c);
	CHECK(seen_find(seen, 3, &c) == SEEN_KEPT && c->stamp.number == 3 && c == call_at(seen, 1));
	found(seen, 0, SEEN_MISSING, __LINE__);
	found(seen, 4, SEEN_MISSING, __LINE__);
	CHECK(seen_oldest(seen) == 1);
	/* Calls 4 and 5 did not run. */
	CHECK(!keep(&seen, 6, 10, CSTR_VIEW("six")));
	found(seen, 4, SEEN_MISSING, __LINE__);
	found(seen, 5, SEEN_MISSING, __LINE__);
	found(seen, 6, SEEN_KEPT, __LINE__);
	/* Calls leave the ring for newer calls. seen_find() returns
	 * SEEN_DROPPED for the findings call 2 too, because it is older than
	 * the oldest call of the ring. */
	for (uint32_t k = 7; k < 7 + SEEN_CALLS; ++k)
		CHECK(!keep(&seen, k, 10, CSTR_VIEW("k")));
	CHECK(seen->n == SEEN_CALLS && seen_oldest(seen) == 7);
	found(seen, 1, SEEN_DROPPED, __LINE__);
	found(seen, 2, SEEN_DROPPED, __LINE__);
	found(seen, 6, SEEN_DROPPED, __LINE__);
	found(seen, 7, SEEN_KEPT, __LINE__);
	found(seen, 6 + SEEN_CALLS, SEEN_KEPT, __LINE__);
	found(seen, 7 + SEEN_CALLS, SEEN_MISSING, __LINE__);
	seen_clear(seen);
	found(seen, 6, SEEN_MISSING, __LINE__);
	found(seen, 7, SEEN_MISSING, __LINE__);
	CHECK(!seen_oldest(seen) && !seen->bytes);
	seen_destroy(&seen);

	/* A resumed ask call. askai made calls 1 to 10 before it restarted. */
	CHECK(!resume(&seen, 10, nullptr));
	CHECK(seen && !seen->n && seen->resumed == 10);
	found(seen, 1, SEEN_RESTARTED, __LINE__);
	found(seen, 10, SEEN_RESTARTED, __LINE__);
	found(seen, 11, SEEN_MISSING, __LINE__);
	CHECK(!keep(&seen, 11, 10, CSTR_VIEW("eleven")));
	found(seen, 10, SEEN_RESTARTED, __LINE__);
	found(seen, 11, SEEN_KEPT, __LINE__);
	CHECK(seen_oldest(seen) == 11);
	/* Calls after the restart that left the ring. */
	for (uint32_t k = 12; k < 12 + SEEN_CALLS; ++k)
		CHECK(!keep(&seen, k, 10, CSTR_VIEW("k")));
	found(seen, 11, SEEN_DROPPED, __LINE__);
	found(seen, 9, SEEN_RESTARTED, __LINE__);
	/* A ring that exists keeps its calls when the resume point changes. */
	CHECK(!resume(&seen, 11, nullptr) && seen->resumed == 11 && seen->n == SEEN_CALLS);
	found(seen, 11, SEEN_RESTARTED, __LINE__);
	/* The number of the calls is a JSON number of 32 bits, or none for 0.
	 * Any other value leaves the ring as it was. */
	CHECK(resume(&seen, UINT64_C(1) << 32, nullptr) == EINVAL && seen->resumed == 11);
	struct json_object *text = json_object_new_string("12");
	CHECK(seen_resume(&seen, text, nullptr) == EINVAL && seen->resumed == 11);
	json_object_put(text);
	CHECK(!resume(&seen, UINT32_MAX, nullptr) && seen->resumed == UINT32_MAX);
	CHECK(!seen_resume(&seen, nullptr, nullptr) && !seen->resumed && seen->n == SEEN_CALLS);
	CHECK(!resume(&seen, 11, nullptr) && seen->resumed == 11);
	/* seen_clear() forgets the restart. */
	seen_clear(seen);
	CHECK(!seen->resumed);
	found(seen, 5, SEEN_MISSING, __LINE__);
	seen_destroy(&seen);

	/* Null arguments. */
	CHECK(seen_resume(nullptr, nullptr, nullptr) == EFAULT);
}

/* Returns an item with the ID @p id, the record @p record and the blobs
 * of the IDs 1... and 2... for seen_print(). The caller frees the record.
 */
static struct seen_item
item_of (char const *id,
         char const *record)
{
	struct seen_item it = { .record = DSTR_INIT };
	CHECK(!cstr_view_store(cstr_view(record), &it.record));
	memset(it.blobs[0].hex, '1', OID_SHA1);
	memset(it.blobs[1].hex, '2', OID_SHA1);
	snprintf(it.id, sizeof it.id, "%s", id);
	return it;
}

/* seen_print() names the bytes of an item in the output of a call, and
 * seen_shown() counts the window of the result of the call. An item counts
 * as shown in the turn whose result showed its last byte, after its other
 * bytes in order. seen_notice() names the items shown whole.
 */
static void
test_items (void)
{
	struct seen *seen = nullptr;
	struct seen_item a = item_of("cuh#2/1", "{\"item\":1}");
	struct seen_item b = item_of("cuh#2/2", "{\"item\":2}");
	CHECK(!seen_find_item(seen, CSTR_VIEW("cuh#2/1")));
	seen_shown(seen, 5, (struct extent){ 0, 100 }, 1);
	/* The output of call 5 prints a at bytes 100 to 150 and b at bytes 150
	 * to 400. The window of 120 bytes shows 20 bytes of a. */
	CHECK(!seen_print(&seen, &a, 5, (struct extent){ 100, 50 }));
	CHECK(seen && seen->items.n == 1);
	CHECK(!seen_print(&seen, &b, 5, (struct extent){ 150, 250 }));
	struct seen_item const *x = seen_find_item(seen, CSTR_VIEW("cuh#2/1"));
	struct seen_item const *y = seen_find_item(seen, CSTR_VIEW("cuh#2/2"));
	CHECK(x && x->output.len == 50 && !x->shown && x->output.at == 100 && x->call == 5 && !x->turn);
	CHECK(y && y->output.len == 250 && y->output.at == 150 && y->call == 5);
	SAME(dstr_get(&x->record), dstr_len(&x->record), "{\"item\":1}");
	CHECK(!memcmp(x->blobs, a.blobs, sizeof a.blobs) && !strcmp(x->id, "cuh#2/1"));
	CHECK(!seen_find_item(seen, CSTR_VIEW("cuh#2/3")) && !seen_find_item(seen, CSTR_VIEW("")));
	/* The list of the printed items names each item once for its call. */
	CHECK(!seen_print(&seen, &a, 5, (struct extent){ 100, 50 }) && seen->printed.n == 2);
	seen_shown(seen, 4, (struct extent){ 0, 1000 }, 1);
	CHECK(!x->shown && x->call == 5 && seen->printed.n == 2);
	seen_shown(seen, 5, (struct extent){ 0, 120 }, 1);
	CHECK(x->shown == 20 && !x->call && !x->turn && !y->shown && !y->call && !seen->printed.n);
	/* The same call again counts nothing. */
	seen_shown(seen, 5, (struct extent){ 0, 1000 }, 1);
	CHECK(x->shown == 20);
	/* Call 6 prints a at byte 0. A window from byte 25 does not continue
	 * the 20 bytes shown, and a window that ends at byte 20 adds none. */
	CHECK(!seen_print(&seen, &a, 6, (struct extent){ 0, 50 }) && x->output.at == 0 &&
	      x->call == 6 && x->shown == 20);
	seen_shown(seen, 6, (struct extent){ 25, 100 }, 2);
	CHECK(x->shown == 20 && !x->turn);
	CHECK(!seen_print(&seen, &a, 7, (struct extent){ 0, 50 }));
	seen_shown(seen, 7, (struct extent){ 5, 15 }, 2);
	CHECK(x->shown == 20);
	/* A window from byte 20 to 40 continues them, and a window from byte
	 * 30 to the end shows a whole in turn 3. */
	CHECK(!seen_print(&seen, &a, 8, (struct extent){ 0, 50 }));
	seen_shown(seen, 8, (struct extent){ 20, 20 }, 2);
	CHECK(x->shown == 40 && !x->turn);
	CHECK(!seen_print(&seen, &a, 9, (struct extent){ 0, 50 }));
	seen_shown(seen, 9, (struct extent){ 30, 4000 }, 3);
	CHECK(x->shown == 50 && x->turn == 3);
	/* An item shown whole keeps its turn when it is shown again. */
	CHECK(!seen_print(&seen, &a, 10, (struct extent){ 7, 50 }));
	seen_shown(seen, 10, (struct extent){ 0, 4000 }, 5);
	CHECK(x->shown == 50 && x->turn == 3 && x->output.at == 7);
	/* b shown whole in one window. */
	CHECK(!seen_print(&seen, &b, 11, (struct extent){ 0, 250 }));
	seen_shown(seen, 11, (struct extent){ 0, 250 }, 4);
	CHECK(y->shown == 250 && y->turn == 4);

	/* The notices name the items shown whole. */
	struct dstr text = DSTR_INIT;
	CHECK(!seen_notice(seen, &text));
	SAME(dstr_get(&text), dstr_len(&text), "[askai: Evidence items that you expanded in this ask "
	     "call: cuh#2/1 and cuh#2/2. These items count as seen.]\n");
	/* An item of another size starts anew, with the record and the blobs
	 * of the new item. */
	struct seen_item c = item_of("cuh#2/2", "{\"item\":3}");
	CHECK(!seen_print(&seen, &c, 12, (struct extent){ 0, 300 }));
	CHECK(seen->items.n == 2 && y->output.len == 300 && !y->shown && !y->turn && y->call == 12);
	SAME(dstr_get(&y->record), dstr_len(&y->record), "{\"item\":3}");
	(void)dstr_clear(&text);
	CHECK(!keep(&seen, 13, 1, CSTR_VIEW("x")) && !seen_notice(seen, &text));
	SAME(dstr_get(&text), dstr_len(&text), "[askai: askai keeps your call c13 of this ask call. "
	     "Cite the call as evidence by number.]\n[askai: Evidence items that you expanded in "
	     "this ask call: cuh#2/1. These items count as seen.]\n");
	/* Call 12 left no result. Calls 14 and 15 print items before their
	 * results. A result counts only the items that its call printed last,
	 * and the list keeps the items of the other calls. */
	CHECK(seen->printed.n == 1 && y->call == 12);
	CHECK(!seen_print(&seen, &c, 14, (struct extent){ 0, 300 }) &&
	      !seen_print(&seen, &a, 15, (struct extent){ 0, 50 }));
	CHECK(seen->printed.n == 3 && y->call == 14 && x->call == 15);
	seen_shown(seen, 15, (struct extent){ 0, 50 }, 6);
	CHECK(seen->printed.n == 2 && !x->call && x->turn == 3 && y->call == 14 && !y->shown);
	seen_shown(seen, 14, (struct extent){ 0, 300 }, 6);
	CHECK(!seen->printed.n && !y->call && y->shown == 300 && y->turn == 6);
	/* An item can end at the last byte of the largest output. */
	CHECK(!seen_print(&seen, &a, 16, (struct extent){ TOOL_OUTPUT_MAX - 50, 50 }) &&
	      x->output.at == TOOL_OUTPUT_MAX - 50 && x->output.len == 50 && x->call == 16);
	seen_shown(seen, 16, (struct extent){ TOOL_OUTPUT_MAX - 50, 50 }, 7);
	CHECK(!seen->printed.n && !x->call && x->shown == 50 && x->turn == 3);

	/* Refusals. */
	CHECK(seen_print(nullptr, &a, 1, (struct extent){ 0, 1 }) == EFAULT);
	CHECK(seen_print(&seen, nullptr, 1, (struct extent){ 0, 1 }) == EFAULT);
	CHECK(seen_print(&seen, &a, 0, (struct extent){ 0, 1 }) == EINVAL);
	CHECK(seen_print(&seen, &a, 1, (struct extent){ 0, 0 }) == EINVAL);
	CHECK(seen_print(&seen, &a, 1, (struct extent){ 0, TOOL_OUTPUT_MAX + 1 }) == EINVAL);
	CHECK(seen_print(&seen, &a, 1, (struct extent){ 1, TOOL_OUTPUT_MAX }) == EINVAL);
	CHECK(seen_print(&seen, &a, 1, (struct extent){ SIZE_MAX, 1 }) == EINVAL);
	CHECK(seen_print(&seen, &a, 1, (struct extent){ 1, SIZE_MAX }) == EINVAL);
	struct seen_item bad = a;
	bad.id[0] = '\0';
	CHECK(seen_print(&seen, &bad, 1, (struct extent){ 0, 1 }) == EINVAL);
	memset(bad.id, 'x', sizeof bad.id);
	CHECK(seen_print(&seen, &bad, 1, (struct extent){ 0, 1 }) == EINVAL);
	CHECK(seen->items.n == 2);

	/* The ring stores SEEN_ITEMS items at most and drops the oldest. Past
	 * SEEN_LISTED items, the notice counts the rest. */
	seen_clear(seen);
	CHECK(!seen->items.n && !seen_find_item(seen, CSTR_VIEW("cuh#2/1")));
	for (uint32_t k = 1; k <= SEEN_ITEMS + 2; ++k) {
		snprintf(c.id, sizeof c.id, "bro#1/%u", k);
		CHECK(!seen_print(&seen, &c, k, (struct extent){ 0, 10 }));
		seen_shown(seen, k, (struct extent){ 0, 10 }, 1);
	}
	CHECK(seen->items.n == SEEN_ITEMS && !seen_find_item(seen, CSTR_VIEW("bro#1/2")) &&
	      seen_find_item(seen, CSTR_VIEW("bro#1/3")) &&
	      seen_find_item(seen, CSTR_VIEW("bro#1/1026")));
	/* The two newest items took the places of the two oldest. */
	CHECK(seen->items_head == 2 &&
	      seen_find_item(seen, CSTR_VIEW("bro#1/3")) == &seen->items.items[2] &&
	      seen_find_item(seen, CSTR_VIEW("bro#1/1026")) == &seen->items.items[1]);
	/* An item that the ring stores stays in its place, and the items stay
	 * in order in the notice and in the journal state. */
	CHECK(!seen_print(&seen, &c, SEEN_ITEMS + 3, (struct extent){ 0, 10 }) &&
	      seen->items_head == 2 &&
	      seen_find_item(seen, CSTR_VIEW("bro#1/1026")) == &seen->items.items[1]);
	seen_shown(seen, SEEN_ITEMS + 3, (struct extent){ 0, 10 }, 1);
	(void)dstr_clear(&text);
	CHECK(!seen_notice(seen, &text));
	CHECK(strstr(dstr_get(&text), ": bro#1/3, bro#1/4, ") &&
	      strstr(dstr_get(&text), ", bro#1/66 and 960 more. These items count as seen.]\n"));
	int e = -1;
	struct json_object *wrapped = seen_items_json(seen, &e);
	CHECK(!e && jb_len(wrapped) == SEEN_ITEMS &&
	      cstr_view_equal(jb_view(jb_get(jb_at(wrapped, 0), "id")), CSTR_VIEW("bro#1/3")) &&
	      cstr_view_equal(jb_view(jb_get(jb_at(wrapped, SEEN_ITEMS - 1), "id")),
	                      CSTR_VIEW("bro#1/1026")));
	struct seen *again = nullptr;
	CHECK(!resume(&again, 1, wrapped) && !again->items_head && again->items.n == SEEN_ITEMS &&
	      !strcmp(again->items.items[0].id, "bro#1/3"));
	seen_destroy(&again);
	json_object_put(wrapped);
	size_t const kept = seen_kept(seen);
	CHECK(kept >= sizeof *seen + SEEN_ITEMS * sizeof c);
	seen_clear(seen);
	CHECK(seen_kept(seen) <= kept && seen->items.cap >= SEEN_ITEMS && !seen->printed.n &&
	      seen->printed.cap);
	(void)dstr_clear(&text);
	CHECK(!seen_notice(seen, &text) && !dstr_len(&text));
	seen_destroy(&seen);
	dstr_cleanup(&text);
	dstr_cleanup(&a.record);
	dstr_cleanup(&b.record);
	dstr_cleanup(&c.record);
}

/* Returns the result of seen_resume() with an array of the one item
 * @p item (JSON text) for the ring @p seen. A refusal leaves the items of
 * the ring as they were.
 */
static int
resume_with (struct seen **seen,
             char const   *item)
{
	char text[512];
	snprintf(text, sizeof text, "[%s]", item);
	struct json_object *items = json_tokener_parse(text);
	CHECK(items);
	int const e = resume(seen, 9, items);
	json_object_put(items);
	return e;
}

/* seen_items_json() writes the items that results showed for the journal
 * state, and seen_resume() reads them back into the ring of a resumed ask
 * call, where they count as seen and their pages go on.
 */
static void
test_resume_items (void)
{
	struct seen *seen = nullptr;
	int e = -1;
	struct json_object *none = seen_items_json(nullptr, &e);
	CHECK(!e && json_object_is_type(none, json_type_array) && !jb_len(none));
	json_object_put(none);
	CHECK(!seen_items_json(nullptr, nullptr));
	/* a is shown whole in turn 3, b in part, and c not at all. */
	struct seen_item a = item_of("cuh#2/1", "{\"item\":1}");
	struct seen_item b = item_of("cuh#2/2", "{\"item\":2}");
	struct seen_item c = item_of("bro#7/1", "{\"item\":3}");
	b.blobs[0] = (struct oid){};
	CHECK(!seen_print(&seen, &a, 5, (struct extent){ 0, 50 }) &&
	      !seen_print(&seen, &b, 5, (struct extent){ 50, 250 }) &&
	      !seen_print(&seen, &c, 5, (struct extent){ 300, 10 }));
	seen_shown(seen, 5, (struct extent){ 0, 70 }, 3);
	CHECK(seen->items.n == 3);
	struct json_object *items = seen_items_json(seen, &e);
	CHECK(!e && jb_len(items) == 2);
	SAME(jb_wire(items).data, jb_wire(items).len,
	     "[{\"id\":\"cuh#2/1\",\"record\":\"{\\\"item\\\":1}\",\"blobs\":[\""
	     "1111111111111111111111111111111111111111\",\"2222222222222222222222222222222222222222\"],"
	     "\"bytes\":50,\"shown\":50,\"turn\":3},{\"id\":\"cuh#2/2\",\"record\":\""
	     "{\\\"item\\\":2}\",\"blobs\":[\"\",\"2222222222222222222222222222222222222222\"],"
	     "\"bytes\":250,\"shown\":20,\"turn\":0}]");

	/* A resumed ask call gets the items, with no call that prints them. */
	struct seen *resumed = nullptr;
	CHECK(!resume(&resumed, 7, items));
	CHECK(resumed && resumed->resumed == 7 && !resumed->n && resumed->items.n == 2);
	struct seen_item const *x = seen_find_item(resumed, CSTR_VIEW("cuh#2/1"));
	struct seen_item const *y = seen_find_item(resumed, CSTR_VIEW("cuh#2/2"));
	CHECK(!seen_find_item(resumed, CSTR_VIEW("bro#7/1")));
	CHECK(x && x->output.len == 50 && x->shown == 50 && x->turn == 3 && !x->call && !x->output.at &&
	      !memcmp(x->blobs, a.blobs, sizeof a.blobs));
	SAME(dstr_get(&x->record), dstr_len(&x->record), "{\"item\":1}");
	CHECK(y && y->output.len == 250 && y->shown == 20 && !y->turn && !oid_len(&y->blobs[0]) &&
	      oid_equal(&y->blobs[1], &b.blobs[1]));
	struct dstr text = DSTR_INIT;
	CHECK(!seen_notice(resumed, &text));
	SAME(dstr_get(&text), dstr_len(&text), "[askai: Evidence items that you expanded in this ask "
	     "call: cuh#2/1. These items count as seen.]\n");
	/* The pages of b go on from the bytes shown before the restart. */
	CHECK(!seen_print(&resumed, &b, 9, (struct extent){ 0, 250 }) && y->shown == 20 &&
	      y->call == 9);
	seen_shown(resumed, 9, (struct extent){ 20, 4000 }, 4);
	CHECK(y->shown == 250 && y->turn == 4);
	/* The items replace the items of a ring, and no items drop them. */
	CHECK(!resume(&resumed, 8, items) && resumed->items.n == 2 && resumed->resumed == 8);
	CHECK(!resume(&resumed, 8, nullptr) && !resumed->items.n);
	CHECK(!resume(&resumed, 8, items) && resumed->items.n == 2);

	/* Refusals leave the ring as it was. */
	static char const *const bad[] = {
		"{}",
		"1",
		"{\"record\":\"r\",\"blobs\":[\"\",\"\"],\"bytes\":1,\"shown\":1,\"turn\":0}",
		"{\"id\":\"\",\"record\":\"r\",\"blobs\":[\"\",\"\"],\"bytes\":1,\"shown\":1,"
		"\"turn\":0}",
		"{\"id\":\"x\",\"record\":\"\",\"blobs\":[\"\",\"\"],\"bytes\":1,\"shown\":1,"
		"\"turn\":0}",
		"{\"id\":\"x\",\"record\":\"r\",\"blobs\":[\"\"],\"bytes\":1,\"shown\":1,"
		"\"turn\":0}",
		"{\"id\":\"x\",\"record\":\"r\",\"blobs\":[\"\",\"12\"],\"bytes\":1,\"shown\":1,"
		"\"turn\":0}",
		"{\"id\":\"x\",\"record\":\"r\",\"blobs\":[\"\",7],\"bytes\":1,\"shown\":1,"
		"\"turn\":0}",
		"{\"id\":\"x\",\"record\":\"r\",\"blobs\":[\"\",\"\"],\"bytes\":0,\"shown\":0,"
		"\"turn\":0}",
		"{\"id\":\"x\",\"record\":\"r\",\"blobs\":[\"\",\"\"],\"bytes\":1,\"shown\":2,"
		"\"turn\":0}",
		"{\"id\":\"x\",\"record\":\"r\",\"blobs\":[\"\",\"\"],\"bytes\":2,\"shown\":1,"
		"\"turn\":1}",
		"{\"id\":\"x\",\"record\":\"r\",\"blobs\":[\"\",\"\"],\"bytes\":1,\"shown\":1,"
		"\"turn\":-1}",
		"{\"id\":\"x\",\"record\":\"r\",\"blobs\":[\"\",\"\"],\"bytes\":16777217,"
		"\"shown\":1,\"turn\":0}",
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
		if (resume_with(&resumed, bad[i]) != EINVAL || resumed->items.n != 2 ||
		    resumed->resumed != 8) {
			++failures;
			fprintf(stderr, "%s:%d: accepted %s\n", __FILE__, __LINE__, bad[i]);
		}
	}
	char id[SEEN_ID + 64];
	memset(id, 'x', SEEN_ID);
	snprintf(id + SEEN_ID, sizeof id - SEEN_ID, "\"");
	char item[SEEN_ID + 128];
	snprintf(item, sizeof item, "{\"id\":\"%s,\"record\":\"r\",\"blobs\":[\"\",\"\"],"
	         "\"bytes\":1,\"shown\":1,\"turn\":0}", id);
	CHECK(resume_with(&resumed, item) == EINVAL);
	id[SEEN_ID - 1] = '"';
	id[SEEN_ID] = '\0';
	snprintf(item, sizeof item, "{\"id\":\"%s,\"record\":\"r\",\"blobs\":[\"\",\"\"],"
	         "\"bytes\":1,\"shown\":1,\"turn\":0}", id);
	CHECK(!resume_with(&resumed, item) && resumed->items.n == 1 &&
	      strlen(resumed->items.items[0].id) == SEEN_ID - 1);
	/* At most SEEN_ITEMS items. */
	struct json_object *many = json_object_new_array();
	for (size_t i = 0; i <= SEEN_ITEMS; ++i)
		CHECK(!jb_push(many, json_object_get(jb_at(items, 0))));
	CHECK(resume(&resumed, 1, many) == EINVAL && resumed->items.n == 1);
	CHECK(!json_object_array_del_idx(many, 0, 1) && !resume(&resumed, 1, many) &&
	      resumed->items.n == SEEN_ITEMS);
	json_object_put(many);
	json_object_put(items);
	seen_destroy(&resumed);
	seen_destroy(&seen);
	dstr_cleanup(&text);
	dstr_cleanup(&a.record);
	dstr_cleanup(&b.record);
	dstr_cleanup(&c.record);
}

int
main (void)
{
	test_lines();
	test_read();
	test_keep();
	test_drop();
	test_find();
	test_items();
	test_resume_items();
	return check_report("seen");
}
