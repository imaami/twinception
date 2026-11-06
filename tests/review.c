/** @file Unit tests for the review record of an ask call (src/review.c). */
#define _GNU_SOURCE /* memrchr() */

#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dstr.h"
#include "review.h"

/* Check that the text @p got is @p want. */
#define SAME(got, want) same(__FILE__, __LINE__, got, want)

static void
same (char const *file,
      int         line,
      char const *got,
      char const *want)
{
	if (got && !strcmp(got, want))
		return;
	++failures;
	fprintf(stderr, "%s:%d: got\n%s\nwant\n%s\n", file, line, got ? got : "(null)", want);
}

/* Two commit IDs and their first 12 digits. */
#define A "55aca1e8682351e4c39a92b040721def3f005402"
#define A12 "55aca1e86823"
#define B "1111111111111111111111111111111111111111"
#define B12 "111111111111"

#define HEADER \
        "[askai: your review record, which askai keeps through every compaction " \
        "of this ask call.]\n"

/* The most bytes of the claim of a closed finding in the record text. */
#define CLOSED 200
#define NONE "[askai: you have recorded no findings; the findings tool records them.]"
#define LATER " The log takes the lines of a read once you have seen them, so call again in " \
              "a later turn."
#define WHOLE ", where the path is the file's whole path from the top of the repository"
#define NO_MORE "The record's limit is 4 MiB, and no further change fits in " \
                "this ask call. Keep further findings in your notes or your summary."
#define NO_ROOM "The record's limit is 4 MiB, and this change does not fit. " \
                "Keep further findings in your notes or your summary."
#define FINDINGS "[askai: your findings; findings action show with an id gives one's history:]\n"
#define READS "[askai: what you read with git read in this call:]\n"

/* A review_kept_fn that drops "[askai: x]", a stand-in for the notices of
 * askai, from the end of @p s.
 */
static size_t
kept (char const *s,
      size_t      len)
{
	static char const tail[] = "[askai: x]";
	size_t n = sizeof tail - 1;
	if (len < n || memcmp(s + len - n, tail, n))
		return len;
	return len - n;
}

/* Run a findings call with the arguments @p args, which it drops, in turn
 * @p turn with @p trim as its review_kept_fn, and put its result in
 * @p out. Returns the error number.
 */
static int
run (struct review      *r,
     struct json_object *args,
     uint32_t            turn,
     review_kept_fn     *trim,
     struct dstr        *out)
{
	dstr_clear(out);
	int e = review_call(r, args, turn, trim, &(struct tool_cursor){}, SIZE_MAX, out);
	json_object_put(args);
	return e;
}

/* Run a findings call with the arguments @p json in turn @p turn, and put
 * its result in @p out. Returns the error number.
 */
static int
call (struct review *r,
      char const    *json,
      uint32_t       turn,
      struct dstr   *out)
{
	struct json_object *args = json_tokener_parse(json);
	CHECK(args || !strcmp(json, "null"));
	return run(r, args, turn, kept, out);
}

/* Run a findings call in turn 1, without kept(), with the arguments
 * @p json and a member @p key of @p len bytes. Put its result in @p out.
 * Returns the error number.
 */
static int
call_long (struct review *r,
           char const    *json,
           char const    *key,
           size_t         len,
           struct dstr   *out)
{
	struct json_object *args = json_tokener_parse(json);
	char *s = malloc(len);
	CHECK(args && s);
	if (s)
		memset(s, 'c', len);
	json_object_object_add(args, key, json_object_new_string_len(s ? s : "", s ? (int)len : 0));
	free(s);
	s = nullptr;
	return run(r, args, 1, nullptr, out);
}

/* Returns the bytes of claim and evidence that the refusal @p text says
 * the record has room for.
 */
static size_t
left_in (char const *text)
{
	static char const lead[] = "The record's limit is 4 MiB. Give a claim and "
	                           "evidence of at most ";
	size_t n = 0;
	CHECK(text && !strncmp(text, lead, sizeof lead - 1) &&
	      sscanf(text + sizeof lead - 1, "%zu", &n) == 1);
	return n;
}

/* Returns the bytes of evidence that an update of finding 1 of @p r has
 * room for. Finding 1 must have no location.
 */
static size_t
room_of (struct review *r)
{
	struct dstr out = DSTR_INIT;
	CHECK(call_long(r, "{\"action\":\"update\",\"id\":1,\"state\":\"candidate\"}", "evidence",
	                TOOL_TEXT_MAX, &out) == EINVAL);
	size_t n = left_in(dstr_get(&out));
	dstr_cleanup(&out);
	return n;
}

/* Check that the call with @p json gives the error @p e and the text
 * @p want.
 */
#define EXPECT(r, json, turn, e, want) \
        do { \
                struct dstr out_ = DSTR_INIT; \
                CHECK(call(r, json, turn, &out_) == (e)); \
                SAME(dstr_get(&out_), want); \
                dstr_cleanup(&out_); \
        } while (0)

/* Write to @p out a git read output: a header, lines first to last as
 * "N\tline N", and a footer. Store the source of the lines in @p src. If
 * @p bare is true, the last line has no newline.
 */
static void
output (struct dstr          *out,
        struct review_source *src,
        char const           *repo,
        char const           *commit,
        char const           *path,
        uint32_t              first,
        uint32_t              last,
        uint32_t              total,
        bool                  bare)
{
	dstr_clear(out);
	CHECK(dstr_printf(out, "[h]\n"));
	size_t at = dstr_len(out);
	for (uint32_t n = first; n <= last; ++n)
		CHECK(dstr_printf(out, "%u\tline %u%s", n, n, n == last && bare ? "" : "\n"));
	size_t len = dstr_len(out) - at;
	CHECK(dstr_printf(out, "\n[f]"));
	*src = (struct review_source){ cstr_view(repo), cstr_view(commit), cstr_view(path),
	                               total, at, len, first, last };
}

/* Build a git read output of lines @p first to @p last, and pass
 * review_pending_add() its window of @p window bytes from @p skip, in turn
 * @p turn. Returns the error number.
 */
static int
shown_in (struct review *r,
          char const    *repo,
          char const    *commit,
          char const    *path,
          uint32_t       first,
          uint32_t       last,
          uint32_t       total,
          size_t         skip,
          size_t         window,
          uint32_t       turn)
{
	struct dstr out = DSTR_INIT;
	struct review_source src;
	output(&out, &src, repo, commit, path, first, last, total, false);
	struct cstr_view o = cstr_view_dstr(&out);
	int e;
	struct review_read *read = review_read_create(&src, o, &e);
	if (!e)
		e = review_pending_add(r, read, o, skip, window, turn);
	review_read_destroy(&read);
	CHECK(!read);
	dstr_cleanup(&out);
	return e;
}

/* Call shown_in(), then review_pending_log(). */
static int
seen_in (struct review *r,
         char const    *repo,
         char const    *commit,
         char const    *path,
         uint32_t       first,
         uint32_t       last,
         uint32_t       total,
         size_t         skip,
         size_t         window,
         uint32_t       turn)
{
	int e = shown_in(r, repo, commit, path, first, last, total, skip, window, turn);
	return e ? e : review_pending_log(r);
}

static int
seen (struct review *r,
      char const    *repo,
      char const    *commit,
      char const    *path,
      uint32_t       first,
      uint32_t       last,
      uint32_t       total,
      uint32_t       turn)
{
	return seen_in(r, repo, commit, path, first, last, total, 0, SIZE_MAX, turn);
}

/* Check that the render of @p r is @p want. */
#define RENDER(r, want) \
        do { \
                struct dstr out_ = DSTR_INIT; \
                CHECK(!review_render(r, &out_, SIZE_MAX)); \
                SAME(dstr_get(&out_), want); \
                dstr_cleanup(&out_); \
        } while (0)

static struct review *
fresh (void)
{
	int e;
	struct review *r = review_create(REVIEW_READ_LOG, &e);
	CHECK(r && !e);
	return r;
}

/* The render of the read log joins ranges that overlap or adjoin, puts
 * each file under its repository and commit, and keeps the groups in the
 * order of their first reads.
 */
static void
test_log (void)
{
	struct review *r = fresh();
	RENDER(r, HEADER NONE);
	RENDER(nullptr, HEADER NONE);
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 10, 300, 1));
	RENDER(r, HEADER NONE "\n" READS "proj " A "\n  src/a.c  lines 1-10: 10 of 300 lines");
	CHECK(!seen(r, "proj", A, "src/a.c", 21, 40, 300, 2));
	CHECK(!seen(r, "proj", B, "src/a.c", 1, 3, 300, 2));
	CHECK(!seen(r, "other", A, "src/b.c", 7, 7, 0, 3));
	CHECK(!seen(r, "proj", A, "src/c.c", 1, 2, 2, 3));
	CHECK(!seen(r, "proj", A, "one.txt", 1, 1, 1, 3));
	CHECK(!seen(r, "proj", A, "src/a.c", 50, 60, 300, 4));
	RENDER(r, HEADER NONE "\n" READS
	       "proj " A "\n"
	       "  src/a.c  lines 1-10, 21-40, 50-60: 41 of 300 lines\n"
	       "  src/c.c  lines 1-2: all 2 lines\n"
	       "  one.txt  line 1: all 1 line\n"
	       "proj " B "\n"
	       "  src/a.c  lines 1-3: 3 of 300 lines\n"
	       "other " A "\n"
	       "  src/b.c  line 7: 1 of an unknown number of lines");
	/* Lines that adjoin two ranges, lie inside one, or span several. */
	CHECK(!seen(r, "proj", A, "src/a.c", 11, 20, 300, 5));
	CHECK(!seen(r, "proj", A, "src/a.c", 5, 6, 300, 5));
	CHECK(!seen(r, "proj", A, "src/a.c", 45, 49, 300, 5));
	CHECK(!seen(r, "proj", B, "src/a.c", 5, 5, 300, 5));
	CHECK(!seen(r, "proj", B, "src/a.c", 9, 9, 300, 5));
	CHECK(!seen(r, "proj", B, "src/a.c", 4, 12, 300, 5));
	RENDER(r, HEADER NONE "\n" READS
	       "proj " A "\n"
	       "  src/a.c  lines 1-40, 45-60: 56 of 300 lines\n"
	       "  src/c.c  lines 1-2: all 2 lines\n"
	       "  one.txt  line 1: all 1 line\n"
	       "proj " B "\n"
	       "  src/a.c  lines 1-12: 12 of 300 lines\n"
	       "other " A "\n"
	       "  src/b.c  line 7: 1 of an unknown number of lines");
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 300, 300, 6));
	CHECK(!seen(r, "other", A, "src/b.c", 1, 9, 0, 6));
	RENDER(r, HEADER NONE "\n" READS
	       "proj " A "\n"
	       "  src/a.c  lines 1-300: all 300 lines\n"
	       "  src/c.c  lines 1-2: all 2 lines\n"
	       "  one.txt  line 1: all 1 line\n"
	       "proj " B "\n"
	       "  src/a.c  lines 1-12: 12 of 300 lines\n"
	       "other " A "\n"
	       "  src/b.c  lines 1-9: 9 of an unknown number of lines");
	review_destroy(&r);
	CHECK(!r);
}

/* The lines of src/a.c that the render of @p r says were read, after "  src/a.c  ". */
static void
lines_of (struct review const *r,
          char const          *want,
          int                  line)
{
	struct dstr out = DSTR_INIT;
	CHECK(!review_render(r, &out, SIZE_MAX));
	char const *s = strstr(dstr_get(&out), "  src/a.c  ");
	same(__FILE__, line, s ? s + sizeof "  src/a.c  " - 1 : "(none)", want);
	dstr_cleanup(&out);
}

/* review_pending_add() adds only the lines that the window shows whole. */
static void
test_seen (void)
{
	/* "[h]\n" is 4 bytes, and each line "N\tline N\n" of lines 1 to 9 is 9. */
	struct {
		size_t      skip;
		size_t      window;
		char const *want;
	} const cases[] = {
		{ 0, SIZE_MAX, "lines 1-9: all 9 lines" },
		{ 0, 4 + 9 * 3 + 4, "lines 1-3: 3 of 9 lines" },     /* cut in line 4 */
		{ 0, 4 + 9 * 3, "lines 1-3: 3 of 9 lines" },         /* ends after a newline */
		{ 0, 4 + 9 * 3 - 1, "lines 1-2: 2 of 9 lines" },     /* before one */
		{ 4 + 9 * 2 + 3, SIZE_MAX, "lines 4-9: 6 of 9 lines" }, /* starts in line 3 */
		{ 4 + 9 * 3, SIZE_MAX, "lines 4-9: 6 of 9 lines" },  /* at a line's start */
		{ 4 + 9 * 3, 9 * 2, "lines 4-5: 2 of 9 lines" },     /* a page of two lines */
		{ 2, SIZE_MAX, "lines 1-9: all 9 lines" },          /* starts in the header */
	};
	for (size_t i = 0; i < array_size(cases); ++i) {
		struct review *r = fresh();
		CHECK(!seen_in(r, "p", A, "src/a.c", 1, 9, 9, cases[i].skip, cases[i].window, 1));
		lines_of(r, cases[i].want, __LINE__);
		review_destroy(&r);
	}
	/* A read paged forward and then back to its start. A window counts its
	 * lines from the mark of the window before it, or from the start of the
	 * read if that mark is after the window. */
	{
		struct dstr paged = DSTR_INIT;
		struct review_source from;
		output(&paged, &from, "p", A, "src/a.c", 1, 9, 9, false);
		struct cstr_view const o = cstr_view_dstr(&paged);
		int e;
		struct review_read *read = review_read_create(&from, o, &e);
		CHECK(read && !e);
		struct review *r = fresh();
		CHECK(!review_pending_add(r, read, o, 4 + 9 * 4, 9 * 2, 1));
		CHECK(!review_pending_add(r, read, o, 4 + 9 * 6, 9 * 2, 1));
		CHECK(!review_pending_add(r, read, o, 4, 9 * 2, 1));
		CHECK(!review_pending_log(r));
		lines_of(r, "lines 1-2, 5-8: 6 of 9 lines", __LINE__);
		review_read_destroy(&read);
		review_destroy(&r);
		dstr_cleanup(&paged);
	}
	/* Hexadecimal digits before the last colon are a path, not a commit. */
	{
		struct review *r = fresh();
		EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"deadbee:12\"}", 1, 0,
		       "Added finding 1: candidate, deadbee:12, not read. Read its lines with git "
		       "read before you verify it.");
		review_destroy(&r);
	}
	/* Windows that hold no whole line. */
	size_t const none[][2] = {
		{ 0, 3 }, { 0, 4 + 8 }, { 4 + 9 * 9, SIZE_MAX }, { 4 + 9 * 2 + 3, 9 },
		{ SIZE_MAX, SIZE_MAX },
	};
	for (size_t i = 0; i < array_size(none); ++i) {
		struct review *r = fresh();
		CHECK(!seen_in(r, "p", A, "src/a.c", 1, 9, 9, none[i][0], none[i][1], 1));
		RENDER(r, HEADER NONE);
		review_destroy(&r);
	}

	/* A last line without a newline counts only if the window holds its end. */
	struct dstr out = DSTR_INIT;
	struct review_source src;
	output(&out, &src, "p", A, "src/a.c", 1, 3, 3, true);
	struct cstr_view o = cstr_view_dstr(&out);
	int e;
	struct review_read *read = review_read_create(&src, o, &e);
	CHECK(read && !e);
	struct review *r = fresh();
	CHECK(!review_pending_add(r, read, o, 0, src.at + src.len - 1, 1) && !review_pending_log(r));
	lines_of(r, "lines 1-2: 2 of 3 lines", __LINE__);
	CHECK(!review_pending_add(r, read, o, 0, src.at + src.len, 1) && !review_pending_log(r));
	lines_of(r, "lines 1-3: all 3 lines", __LINE__);
	/* An output too short for the read, and one with too many lines. */
	CHECK(review_pending_add(r, read, (struct cstr_view){ o.data, src.at + src.len - 1 }, 0,
	                         SIZE_MAX, 1) == EINVAL);
	struct dstr more = DSTR_INIT;
	CHECK(dstr_printf(&more, "[h]\n"));
	for (int i = 0; i < 13; ++i)
		CHECK(dstr_printf(&more, "a\n"));
	CHECK(review_pending_add(r, read, cstr_view_dstr(&more), 0, SIZE_MAX, 1) == EINVAL);
	dstr_cleanup(&more);
	review_read_destroy(&read);
	review_destroy(&r);

	/* Sources that do not match their output or are not valid. */
	output(&out, &src, "p", A, "src/a.c", 1, 3, 3, false);
	o = cstr_view_dstr(&out);
	struct review_source bad[] = {
		src, src, src, src, src, src, src, src, src, src, src, src, src, src,
	};
	bad[0].last = 4;                       /* a line more than the text */
	bad[1].first = 2;                      /* a line fewer */
	bad[2].first = 0;
	bad[3].commit = cstr_view(A "0");           /* 41 digits */
	bad[4].commit = cstr_view("55ACA1E8682351E4C39A92B040721DEF3F005402");
	bad[5].repo = cstr_view("");
	bad[6].path = (struct cstr_view){ "a\0b", 3 };
	bad[7].total = 2;                      /* fewer lines than the last */
	bad[8].len = 0;
	bad[9].at = o.len;                     /* past the output */
	bad[10].path = (struct cstr_view){ "\xff", 1 };
	bad[11].last = 0;
	bad[12].at = SIZE_MAX;
	bad[13].total = (uint64_t)UINT32_MAX + 1;  /* more than a file records */
	for (size_t i = 0; i < array_size(bad); ++i) {
		e = 0;
		CHECK(!review_read_create(&bad[i], o, &e) && e == EINVAL);
	}
	/* Bytes after the last newline are a line more than the read says. */
	struct review_source trail = { cstr_view("p"), cstr_view(A), cstr_view("src/a.c"), 0, 0, 5, 1, 1 };
	e = 0;
	CHECK(!review_read_create(&trail, cstr_view("1\ta\n2"), &e) && e == EINVAL);
	/* The numbered lines may end without the last newline. */
	src.len -= 1;
	read = review_read_create(&src, o, &e);
	CHECK(read && !e);
	review_read_destroy(&read);
	char path[REVIEW_NAME_MAX + 2] = {};
	memset(path, 'a', REVIEW_NAME_MAX + 1);
	src.path = cstr_view(path);
	CHECK(!review_read_create(&src, o, &e) && e == EINVAL);
	path[REVIEW_NAME_MAX] = '\0';
	src.path = cstr_view(path);
	read = review_read_create(&src, o, &e);
	CHECK(read && !e);
	review_read_destroy(&read);
	dstr_cleanup(&out);
}

/* The text of each refusal. */
static void
test_refusals (void)
{
	struct review *r = fresh();
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 10, 300, 1));
	struct {
		char const *json;
		char const *want;
	} const cases[] = {
		{ "{}", "The action argument must be add, update or show." },
		{ "null", "The action argument must be add, update or show." },
		{ "{\"action\":\"remove\"}", "The action argument must be add, update or show." },
		{ "{\"action\":3}", "The action argument must be add, update or show." },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"id\":1}",
		  "add takes claim, location, state and evidence, not id." },
		{ "{\"action\":\"update\",\"id\":1,\"offset\":0}",
		  "update takes id, claim, location, state and evidence, not offset." },
		{ "{\"action\":\"show\",\"claim\":\"x\"}", "show takes id and offset, not claim." },
		{ "{\"action\":\"add\"}", "add needs a claim: what you found." },
		{ "{\"action\":\"add\",\"claim\":\"\"}",
		  "The claim must be a string of UTF-8 text, not empty." },
		{ "{\"action\":\"add\",\"claim\":5}",
		  "The claim must be a string of UTF-8 text, not empty." },
		{ "{\"action\":\"add\",\"claim\":\"[askai: x]\"}",
		  "The claim has only askai's [askai: ...] lines, which askai adds to tool "
		  "results. Give the claim itself." },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"\"}",
		  "The location must be a string of UTF-8 text, not empty." },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"evidence\":[]}",
		  "The evidence must be a string of UTF-8 text, not empty." },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"evidence\":\"\\n\\n[askai: x]\"}",
		  "The evidence has only askai's [askai: ...] lines, which askai adds to tool "
		  "results. Give the evidence itself." },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"open\"}",
		  "The state argument must be candidate, verified, disproved or retracted." },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"retracted\"}",
		  "add takes the state candidate, verified or disproved; retract a finding with "
		  "update." },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\",\"location\":\"src/a.c:3\"}",
		  "verified needs evidence: what you read that shows it." },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"disproved\",\"location\":\"src/a.c:3\"}",
		  "disproved needs evidence: what you read that shows it false." },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\",\"evidence\":\"e\"}",
		  "verified needs a location: the lines that show it, such as src/mcp.c:120-140, "
		  "which you read with git read." },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\",\"evidence\":\"e\","
		  "\"location\":\"src/a.c:50-60\"}",
		  "verified needs the lines of its location in your read log, and src/a.c lines "
		  "50-60 are not in it. Read them with the git tool's read command, with "
		  "\"revisions\": [\"HEAD:src/a.c\"] and \"line\": 50." LATER },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"disproved\",\"evidence\":\"e\","
		  "\"location\":\"src/a.c:11\"}",
		  "disproved needs the lines of its location in your read log, and src/a.c line "
		  "11 is not in it. Read them with the git tool's read command, with "
		  "\"revisions\": [\"HEAD:src/a.c\"] and \"line\": 11." LATER },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\",\"evidence\":\"e\","
		  "\"location\":\"src/a.c:10-11\"}",
		  "verified needs the lines of its location in your read log, and src/a.c lines "
		  "10-11 are not all in it: it has line 10 of them, read at " A12 ". Read the rest "
		  "with the git tool's read command, with \"revisions\": [\"" A12 ":src/a.c\"] and "
		  "\"line\": 11, in as many calls as it takes." LATER },
		{ "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\",\"evidence\":\"e\","
		  "\"location\":\"1111111:src/a.c:3\"}",
		  "verified needs the lines of its location in your read log, and src/a.c at "
		  "1111111 line 3 is not in it. Read them with the git tool's read command, with "
		  "\"revisions\": [\"1111111:src/a.c\"] and \"line\": 3" WHOLE "." LATER },
		/* A HEAD: prefix means no commit. */
		{ "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\",\"evidence\":\"e\","
		  "\"location\":\"HEAD:src/a.c:50\"}",
		  "verified needs the lines of its location in your read log, and src/a.c line "
		  "50 is not in it. Read them with the git tool's read command, with "
		  "\"revisions\": [\"HEAD:src/a.c\"] and \"line\": 50." LATER },
		/* The path in the read call is escaped as in a JSON string. */
		{ "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\",\"evidence\":\"e\","
		  "\"location\":\"a\\\"b.c:3\"}",
		  "verified needs the lines of its location in your read log, and a\"b.c line 3 is "
		  "not in it. Read them with the git tool's read command, with \"revisions\": "
		  "[\"HEAD:a\\\"b.c\"] and \"line\": 3" WHOLE "." LATER },
		{ "{\"action\":\"update\"}",
		  "The id argument must be the number of a finding, such as 3." },
		{ "{\"action\":\"update\",\"id\":0}",
		  "The id argument must be the number of a finding, such as 3." },
		{ "{\"action\":\"update\",\"id\":\"1\"}",
		  "The id argument must be the number of a finding, such as 3." },
		{ "{\"action\":\"update\",\"id\":7}",
		  "There is no finding 7. The record contains no findings yet." },
		{ "{\"action\":\"show\",\"id\":1.0}",
		  "The id argument must be the number of a finding, such as 3." },
		{ "{\"action\":\"show\",\"id\":2}",
		  "There is no finding 2. The record contains no findings yet." },
	};
	for (size_t i = 0; i < array_size(cases); ++i)
		EXPECT(r, cases[i].json, 2, EINVAL, cases[i].want);

	/* The locations that are not path:line or path:first-last. */
	char const *const places[] = {
		"src/a.c", "src/a.c:0", "src/a.c:5-3", "src/a.c:3-", "src/a.c:-3", ":3",
		"src/a.c:4294967296", "src/a.c:3:", "src/a.c:3 ", A ":src/a.c:3:int x;",
		"1111111::3", "src/a.c:3-4-5", "src/a.c:+3",
	};
	for (size_t i = 0; i < array_size(places); ++i) {
		char json[256];
		snprintf(json, sizeof json, "{\"action\":\"add\",\"claim\":\"c\",\"location\":"
		         "\"%s\"}", places[i]);
		EXPECT(r, json, 2, EINVAL, "The location must be path:line or path:first-last, "
		                           "such as src/mcp.c:120-140.");
	}

	/* Texts that are not UTF-8. */
	struct dstr out = DSTR_INIT;
	char const *const keys[] = { "claim", "evidence", "location" };
	for (size_t i = 0; i < array_size(keys); ++i) {
		struct json_object *args = json_object_new_object();
		json_object_object_add(args, "action", json_object_new_string("add"));
		json_object_object_add(args, "claim", json_object_new_string("c"));
		json_object_object_add(args, keys[i], json_object_new_string_len("a\xff", 2));
		CHECK(run(r, args, 2, nullptr, &out) == EINVAL);
		char want[96];
		snprintf(want, sizeof want, "The %s must be a string of UTF-8 text, not empty.",
		         keys[i]);
		SAME(dstr_get(&out), want);
	}

	/* Finding ids past the last. */
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\"}", 2, 0, "Added finding 1: candidate.");
	EXPECT(r, "{\"action\":\"update\",\"id\":2}", 2, EINVAL,
	       "There is no finding 2. The record contains only finding 1.");
	for (unsigned i = 2; i <= 5; ++i) {
		char want[64];
		snprintf(want, sizeof want, "Added finding %u: candidate.", i);
		EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\"}", 2, 0, want);
	}
	EXPECT(r, "{\"action\":\"show\",\"id\":9}", 2, EINVAL,
	       "There is no finding 9. The record contains findings 1 to 5.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1}", 2, EINVAL,
	       "update needs state and evidence: what shows the new state, or why you retract "
	       "the finding.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"verified\"}", 2, EINVAL,
	       "update needs state and evidence: what shows the new state, or why you retract "
	       "the finding.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"evidence\":\"e\"}", 2, EINVAL,
	       "update needs state and evidence: what shows the new state, or why you retract "
	       "the finding.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"fixed\",\"evidence\":\"e\"}", 2,
	       EINVAL, "The state argument must be candidate, verified, disproved or retracted.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"verified\",\"evidence\":\"e\"}", 2,
	       EINVAL, "verified needs a location: the lines that show it, such as "
	               "src/mcp.c:120-140, which you read with git read.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"candidate\",\"evidence\":\"e\","
	          "\"claim\":\"\"}", 2,
	       EINVAL, "The claim must be a string of UTF-8 text, not empty.");
	dstr_cleanup(&out);
	review_destroy(&r);
}

/* Short paths resolve against the read log, unless they name more than
 * one path.
 */
static void
test_paths (void)
{
	struct review *r = fresh();
	CHECK(!seen(r, "proj", A, "src/x.c", 1, 10, 10, 1));
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 10, 10, 1));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"a.c:3\"}", 2, 0,
	       "Added finding 1: candidate, src/a.c:3 at " A12 ".");
	/* Neither "c" nor "/a.c" follows a '/' at the end of a logged path. */
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"c:3\"}", 2, 0,
	       "Added finding 2: candidate, c:3, not read. Read its lines with git read "
	       "before you verify it.");
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"/a.c:3\"}", 2, 0,
	       "Added finding 3: candidate, /a.c:3, not read. Read its lines with git read "
	       "before you verify it.");
	CHECK(!seen(r, "proj", B, "tests/x.c", 1, 10, 10, 2));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"x.c:3\"}", 3, EINVAL,
	       "x.c matches src/x.c and tests/x.c in your read log; give \"location\" with the "
	       "whole path.");
	/* A commit narrows the paths, and the whole path is never ambiguous. */
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"1111111:x.c:3\"}", 3, 0,
	       "Added finding 4: candidate, tests/x.c:3 at " B12 ".");
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\",\"evidence\":"
	          "\"e\",\"location\":\"src/x.c:3\"}", 3, 0,
	       "Added finding 5: verified, src/x.c:3 at " A12 ".");
	CHECK(!seen(r, "other", A, "doc/x.c", 1, 10, 10, 3));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"x.c:3\"}", 4, EINVAL,
	       "x.c matches src/x.c, tests/x.c and doc/x.c in your read log; give \"location\" "
	       "with the whole path.");
	CHECK(!seen(r, "other", A, "lib/x.c", 1, 10, 10, 3));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"x.c:3\"}", 4, EINVAL,
	       "x.c matches src/x.c, tests/x.c, doc/x.c and others in your read log; give "
	       "\"location\" with the whole path.");
	/* The same path in two repositories is one path. */
	CHECK(!seen(r, "other", A, "src/a.c", 1, 10, 10, 4));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"a.c:3\"}", 5, 0,
	       "Added finding 6: candidate, src/a.c:3 at " A12 ".");
	/* A HEAD: prefix means no commit before a short path too. */
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"HEAD:a.c:3\"}", 5, 0,
	       "Added finding 7: candidate, src/a.c:3 at " A12 ".");
	review_destroy(&r);

	/* A finding added with a short path before any read gets the full
	 * logged path at its next update. */
	r = fresh();
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"a.c:3\"}", 1, 0,
	       "Added finding 1: candidate, a.c:3, not read. Read its lines with git read before "
	       "you verify it.");
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 10, 10, 1));
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"verified\",\"evidence\":\"e\"}",
	       2, 0, "Finding 1 is now verified, src/a.c:3 at " A12 "; it was candidate.");
	EXPECT(r, "{\"action\":\"show\",\"id\":1}", 2, 0,
	       "Finding 1: verified, src/a.c:3 at " A12 "\nclaim: c\nturn 1: candidate\n"
	       "turn 2: verified at " A12 "\n  evidence: e");
	CHECK(!seen(r, "proj", A, "tests/a.c", 1, 10, 10, 2));
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"candidate\",\"evidence\":\"f\"}",
	       3, 0, "Finding 1 is now candidate, src/a.c:3 at " A12 "; it was verified.");
	review_destroy(&r);

	/* A short path that later reads make ambiguous stays as it is in an
	 * update without a location. An update to verified is refused. */
	r = fresh();
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"a.c:3\"}", 1, 0,
	       "Added finding 1: candidate, a.c:3, not read. Read its lines with git read before "
	       "you verify it.");
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 10, 10, 1));
	CHECK(!seen(r, "proj", A, "tests/a.c", 1, 10, 10, 1));
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"verified\",\"evidence\":\"e\"}",
	       2, EINVAL, "a.c matches src/a.c and tests/a.c in your read log; give \"location\" "
	       "with the whole path.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"candidate\",\"evidence\":\"f\"}",
	       2, 0, "Finding 1 stays candidate, a.c:3, not read; the new evidence is in its "
	       "history. Read its lines with git read before you verify it.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"retracted\",\"evidence\":\"w\"}",
	       2, 0, "Finding 1 is now retracted, a.c:3, not read; it was candidate.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"verified\",\"evidence\":\"e\","
	          "\"location\":\"src/a.c:3\"}", 2, 0,
	       "Finding 1 is now verified, src/a.c:3 at " A12 "; it was retracted. Its old location "
	       "is in its history.");
	review_destroy(&r);
}

/* Returns the ranges of the first file of the read log of @p r as
 * review_json() writes them. The text is valid until the next call.
 */
static char const *
ranges_of (struct review const *r)
{
	static char text[256];
	int e;
	struct json_object *obj = review_json(r, &e);
	CHECK(obj && !e);
	struct json_object *reads = json_object_object_get(obj, "reads");
	struct json_object *lines = json_object_object_get(json_object_array_get_idx(reads, 0),
	                                                   "lines");
	snprintf(text, sizeof text, "%s",
	         json_object_to_json_string_ext(lines, JSON_C_TO_STRING_PLAIN));
	json_object_put(obj);
	return text;
}

/* A read replaces the parts of the logged ranges that it covers, with its
 * own turn, and merges only with ranges of the same turn. The render
 * joins adjoining ranges.
 */
static void
test_paint (void)
{
	struct review *r = fresh();
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 10, 300, 1));
	/* A read inside a range splits it. */
	CHECK(!seen(r, "proj", A, "src/a.c", 4, 6, 300, 2));
	SAME(ranges_of(r), "[1,3,1,4,6,2,7,10,1]");
	/* A read over the end of one range and the start of the next merges
	 * with the first, which has the same turn, and trims the second. */
	CHECK(!seen(r, "proj", A, "src/a.c", 6, 8, 300, 2));
	SAME(ranges_of(r), "[1,3,1,4,8,2,9,10,1]");
	/* A read next to a range of another turn, then one of the same turn. */
	CHECK(!seen(r, "proj", A, "src/a.c", 11, 12, 300, 3));
	CHECK(!seen(r, "proj", A, "src/a.c", 13, 14, 300, 3));
	SAME(ranges_of(r), "[1,3,1,4,8,2,9,10,1,11,14,3]");
	/* A read over several ranges drops those inside and trims the ends. */
	CHECK(!seen(r, "proj", A, "src/a.c", 2, 12, 300, 4));
	SAME(ranges_of(r), "[1,1,1,2,12,4,13,14,3]");
	lines_of(r, "lines 1-14: 14 of 300 lines", __LINE__);
	/* A read over all ranges, and a read one line after them. */
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 14, 300, 5));
	CHECK(!seen(r, "proj", A, "src/a.c", 16, 16, 300, 5));
	SAME(ranges_of(r), "[1,14,5,16,16,5]");
	lines_of(r, "lines 1-14, 16: 15 of 300 lines", __LINE__);
	review_destroy(&r);
}

/* verified and disproved succeed once the lines are in the read log, and
 * the change gets the commit of the latest read that showed them.
 */
static void
test_gate (void)
{
	struct review *r = fresh();
	CHECK(!seen(r, "proj", B, "src/a.c", 1, 10, 300, 1));
	char const verify[] = "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	                      "\"evidence\":\"e\",\"location\":\"src/a.c:50\"}";
	EXPECT(r, verify, 2, EINVAL,
	       "verified needs the lines of its location in your read log, and src/a.c line 50 "
	       "is not in it. Read them with the git tool's read command, with \"revisions\": "
	       "[\"HEAD:src/a.c\"] and \"line\": 50." LATER);
	CHECK(!seen(r, "proj", A, "src/a.c", 45, 60, 300, 2));
	EXPECT(r, verify, 3, 0, "Added finding 1: verified, src/a.c:50 at " A12 ".");
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	          "\"evidence\":\"e\",\"location\":\"src/a.c:3-4\"}", 3, 0,
	       "Added finding 2: verified, src/a.c:3-4 at " B12 ".");
	/* A later read at another commit also holds the lines and wins. */
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 5, 300, 4));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"disproved\","
	          "\"evidence\":\"e\",\"location\":\"src/a.c:3-4\"}", 4, 0,
	       "Added finding 3: disproved, src/a.c:3-4 at " A12 ".");
	/* With a commit prefix, only the reads at that commit count. */
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	          "\"evidence\":\"e\",\"location\":\"111111111111:src/a.c:3\"}", 4, 0,
	       "Added finding 4: verified, src/a.c:3 at " B12 ".");
	/* A read of other lines at B does not make the older read of lines 3-4
	 * at B newer. */
	CHECK(!seen(r, "proj", B, "src/a.c", 200, 210, 300, 5));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	          "\"evidence\":\"e\",\"location\":\"src/a.c:3-4\"}", 6, 0,
	       "Added finding 5: verified, src/a.c:3-4 at " A12 ".");
	review_destroy(&r);

	/* Only part of the location is logged. The refusal names the commit
	 * whose reads reach furthest without a gap from the first line, and the
	 * line after them. */
	r = fresh();
	char const wide[] = "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	                    "\"evidence\":\"e\",\"location\":\"src/a.c:1-300\"}";
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 100, 300, 1));
	CHECK(!seen(r, "proj", B, "src/a.c", 1, 150, 300, 1));
	CHECK(!seen(r, "proj", A, "src/a.c", 160, 300, 300, 1));
	EXPECT(r, wide, 2, EINVAL,
	       "verified needs the lines of its location in your read log, and src/a.c lines 1-300 "
	       "are not all in it: it has lines 1-150 of them, read at " B12 ". Read the rest with "
	       "the git tool's read command, with \"revisions\": [\"" B12 ":src/a.c\"] and "
	       "\"line\": 151, in as many calls as it takes." LATER);
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	          "\"evidence\":\"e\",\"location\":\"" A12 ":src/a.c:100-300\"}", 2, EINVAL,
	       "verified needs the lines of its location in your read log, and src/a.c at " A12 " "
	       "lines 100-300 are not all in it: it has line 100 of them, read at " A12 ". Read "
	       "the rest with the git tool's read command, with \"revisions\": [\"" A12
	       ":src/a.c\"] and \"line\": 101, in as many calls as it takes." LATER);
	CHECK(!seen(r, "proj", B, "src/a.c", 151, 300, 300, 2));
	EXPECT(r, wide, 3, 0, "Added finding 1: verified, src/a.c:1-300 at " B12 ".");
	review_destroy(&r);

	/* Reading lines 11-12 at A in turn 3 does not make A newer for line 3,
	 * which B showed in turn 2. */
	r = fresh();
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 10, 300, 1));
	CHECK(!seen(r, "proj", B, "src/a.c", 3, 4, 300, 2));
	CHECK(!seen(r, "proj", A, "src/a.c", 11, 12, 300, 3));
	char const at3[] = "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	                   "\"evidence\":\"e\",\"location\":\"src/a.c:3\"}";
	EXPECT(r, at3, 4, 0, "Added finding 1: verified, src/a.c:3 at " B12 ".");
	/* Only the reads at A hold all of lines 3-11, in turns 1 and 3, so the
	 * finding is at A, although B read line 3 later. */
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	          "\"evidence\":\"e\",\"location\":\"src/a.c:3-11\"}", 4, 0,
	       "Added finding 2: verified, src/a.c:3-11 at " A12 ".");
	/* Pages read at two commits in turn: each line gets the commit of its
	 * latest read. */
	CHECK(!seen(r, "proj", B, "src/a.c", 1, 100, 300, 4));
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 50, 300, 5));
	CHECK(!seen(r, "proj", B, "src/a.c", 101, 200, 300, 6));
	EXPECT(r, at3, 7, 0, "Added finding 3: verified, src/a.c:3 at " A12 ".");
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	          "\"evidence\":\"e\",\"location\":\"src/a.c:60\"}", 7, 0,
	       "Added finding 4: verified, src/a.c:60 at " B12 ".");
	review_destroy(&r);

	/* A file ranks by its oldest read of the location. Lines 6-8 are from
	 * turn 1 at A and from turn 2 at B, so B wins. */
	r = fresh();
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 10, 300, 1));
	CHECK(!seen(r, "proj", B, "src/a.c", 1, 10, 300, 2));
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 5, 300, 3));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	          "\"evidence\":\"e\",\"location\":\"src/a.c:3-8\"}", 4, 0,
	       "Added finding 1: verified, src/a.c:3-8 at " B12 ".");
	review_destroy(&r);

	/* A location past the end of the file, by the totals in the log. */
	r = fresh();
	CHECK(!seen(r, "proj", A, "src/a.c", 295, 300, 300, 1));
	char const past[] = "src/a.c has 300 lines at " A12 ", so line 305 is past its end; give "
	                    "a location within lines 1 to 300.";
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	          "\"evidence\":\"e\",\"location\":\"src/a.c:299-305\"}", 2, EINVAL, past);
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	          "\"evidence\":\"e\",\"location\":\"src/a.c:301-305\"}", 2, EINVAL, past);
	/* The refusal gives the total of the file with the most lines. */
	CHECK(!seen(r, "proj", B, "src/a.c", 1, 5, 302, 2));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	          "\"evidence\":\"e\",\"location\":\"src/a.c:305\"}", 3, EINVAL,
	       "src/a.c has 302 lines at " B12 ", so line 305 is past its end; give a location "
	       "within lines 1 to 302.");
	/* No such refusal if a file has an unknown total or at least the last
	 * line. If the file that holds the first lines is too short, the read
	 * starts at the first line, at the commit of the longest file. */
	char const ends[] = "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	                    "\"evidence\":\"e\",\"location\":\"src/a.c:299-301\"}";
	EXPECT(r, ends, 3, EINVAL,
	       "verified needs the lines of its location in your read log, and src/a.c lines "
	       "299-301 are not all in it: it has lines 299-300 of them, read at " A12 ", where "
	       "the file has 300 lines. Read them with the git tool's read command, with "
	       "\"revisions\": [\"" B12 ":src/a.c\"] and \"line\": 299, in as many calls as it "
	       "takes." LATER);
	/* A file that can have the last line beats one that reaches further. */
	CHECK(!seen(r, "proj", B, "src/a.c", 299, 299, 302, 3));
	EXPECT(r, ends, 3, EINVAL,
	       "verified needs the lines of its location in your read log, and src/a.c lines "
	       "299-301 are not all in it: it has line 299 of them, read at " B12 ". Read the "
	       "rest with the git tool's read command, with \"revisions\": [\"" B12 ":src/a.c\"] "
	       "and \"line\": 300, in as many calls as it takes." LATER);
	CHECK(!seen(r, "other", A, "src/a.c", 1, 5, 0, 3));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	          "\"evidence\":\"e\",\"location\":\"src/a.c:305\"}", 4, EINVAL,
	       "verified needs the lines of its location in your read log, and src/a.c line 305 "
	       "is not in it. Read them with the git tool's read command, with \"revisions\": "
	       "[\"HEAD:src/a.c\"] and \"line\": 305." LATER);
	review_destroy(&r);
}

/* The pending lines that review.h describes. They count against
 * TOOL_TEXT_MAX from review_pending_add() on.
 */
static void
test_pending (void)
{
	struct review *r = fresh();
	char const verify[] = "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	                      "\"evidence\":\"e\",\"location\":\"src/a.c:3\"}";
	char const unread[] = "verified needs the lines of its location in your read log, and "
	                      "src/a.c line 3 is not in it. Read them with the git tool's read "
	                      "command, with \"revisions\": [\"HEAD:src/a.c\"] and \"line\": 3"
	                      WHOLE "." LATER;
	CHECK(!shown_in(r, "proj", A, "src/a.c", 1, 10, 300, 0, SIZE_MAX, 1));
	EXPECT(r, verify, 1, EINVAL, unread);
	RENDER(r, HEADER NONE);
	review_pending_drop(r);
	CHECK(!review_pending_log(r));
	EXPECT(r, verify, 2, EINVAL, unread);
	RENDER(r, HEADER NONE);
	CHECK(!shown_in(r, "proj", A, "src/a.c", 1, 10, 300, 0, SIZE_MAX, 2));
	CHECK(!shown_in(r, "proj", A, "src/a.c", 11, 12, 300, 0, SIZE_MAX, 2));
	CHECK(!review_pending_log(r));
	EXPECT(r, verify, 3, 0, "Added finding 1: verified, src/a.c:3 at " A12 ".");
	RENDER(r, HEADER FINDINGS "1. verified, src/a.c:3 at " A12 "\n   claim: c\n   evidence: e\n"
	          READS "proj " A "\n  src/a.c  lines 1-12: 12 of 300 lines");
	review_destroy(&r);

	/* Leave room for exactly one pending file with one range. A pending
	 * file is charged as much as a logged one, and review_pending_drop() frees
	 * its room. That room also fits an update with 1 byte of evidence, so
	 * while the file is pending, the refusal is NO_ROOM, not NO_MORE. */
	size_t file = 2 * REVIEW_ITEM + 1 + 7;
	char const add[] = "{\"action\":\"add\"}";
	char const more[] = "{\"action\":\"update\",\"id\":1,\"state\":\"candidate\","
	                    "\"evidence\":\"e\"}";
	for (int answered = 0; answered < 2; ++answered) {
		r = fresh();
		struct dstr out = DSTR_INIT;
		CHECK(call_long(r, add, "claim", TOOL_TEXT_MAX, &out) == EINVAL);
		CHECK(!call_long(r, add, "claim", left_in(dstr_get(&out)) - file, &out));
		dstr_cleanup(&out);
		CHECK(!shown_in(r, "p", A, "src/a.c", 1, 3, 3, 0, SIZE_MAX, 1));
		EXPECT(r, more, 1, EINVAL, NO_ROOM);
		if (answered) {
			CHECK(!review_pending_log(r));
			EXPECT(r, more, 2, EINVAL, NO_MORE);
		} else {
			review_pending_drop(r);
			EXPECT(r, more, 2, 0,
			       "Finding 1 stays candidate; the new evidence is in its history.");
		}
		review_destroy(&r);
	}
}

/* Updates keep the states and the evidence in the history, with the old
 * claims and locations.
 */
static void
test_history (void)
{
	struct review *r = fresh();
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"C1\",\"location\":\"src/a.c:5\","
	          "\"evidence\":\"seen in a grep\\n\\n[askai: x]\"}", 1, 0,
	       "Added finding 1: candidate, src/a.c:5, not read. Read its lines with git read "
	       "before you verify it. askai left out the [askai: ...] lines at the end of the "
	       "evidence. askai adds those lines to tool results, and they are not part of "
	       "the finding.");
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 9, 9, 2));
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"verified\",\"evidence\":\"E2\"}",
	       3, 0, "Finding 1 is now verified, src/a.c:5 at " A12 "; it was candidate.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"verified\",\"evidence\":\"E3\","
	          "\"claim\":\"C1\",\"location\":\"src/a.c:5\"}", 4, 0,
	       "Finding 1 stays verified, src/a.c:5 at " A12 "; the new evidence is in its "
	       "history.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"verified\",\"evidence\":\"E4\","
	          "\"claim\":\"C2\\n\\n[askai: x]\",\"location\":\"a.c:6-7\"}", 5, 0,
	       "Finding 1 stays verified, src/a.c:6-7 at " A12 "; the new evidence is in its "
	       "history. Its old claim and location are in its history. askai left out the "
	       "[askai: ...] lines at the end of the claim. askai adds those lines to tool "
	       "results, and they are not part of the finding.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"retracted\",\"evidence\":\"E5\","
	          "\"claim\":\"C3\"}", 6, 0,
	       "Finding 1 is now retracted, src/a.c:6-7 at " A12 "; it was verified. Its old "
	       "claim is in its history.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"candidate\",\"evidence\":\"E6\","
	          "\"location\":\"src/b.c:1\"}", 7, 0,
	       "Finding 1 is now candidate, src/b.c:1, not read; it was retracted. Its old "
	       "location is in its history. Read its lines with git read before you verify "
	       "it.");
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"D\"}", 7, 0, "Added finding 2: candidate.");
	EXPECT(r, "{\"action\":\"update\",\"id\":2,\"state\":\"candidate\",\"evidence\":\"F\","
	          "\"location\":\"src/a.c:1\"}", 8, 0,
	       "Finding 2 stays candidate, src/a.c:1 at " A12 "; the new evidence is in its "
	       "history. Its old location is in its history.");
	EXPECT(r, "{\"action\":\"show\",\"id\":1}", 8, 0,
	       "Finding 1: candidate, src/b.c:1, not read\n"
	       "claim: C3\n"
	       "turn 1: candidate\n"
	       "  evidence: seen in a grep\n"
	       "turn 3: verified at " A12 "\n"
	       "  evidence: E2\n"
	       "turn 4: verified at " A12 "\n"
	       "  evidence: E3\n"
	       "turn 5: verified at " A12 "\n"
	       "  evidence: E4\n"
	       "  the claim was: C1\n"
	       "  the location was: src/a.c:5\n"
	       "turn 6: retracted at " A12 "\n"
	       "  evidence: E5\n"
	       "  the claim was: C2\n"
	       "turn 7: candidate\n"
	       "  evidence: E6\n"
	       "  the location was: src/a.c:6-7");
	EXPECT(r, "{\"action\":\"show\",\"id\":2}", 8, 0,
	       "Finding 2: candidate, src/a.c:1 at " A12 "\n"
	       "claim: D\n"
	       "turn 7: candidate\n"
	       "turn 8: candidate at " A12 "\n"
	       "  evidence: F\n"
	       "  the location was: none");
	review_destroy(&r);
}

/* The render has the open findings with their claims and latest evidence,
 * then the closed ones, a line each, then the read log. A render that
 * goes past its size is cut.
 */
static void
test_render (void)
{
	struct review *r = fresh();
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 9, 300, 1));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"C1\",\"state\":\"verified\",\"evidence\":"
	          "\"E1\",\"location\":\"src/a.c:3-4\"}", 2, 0,
	       "Added finding 1: verified, src/a.c:3-4 at " A12 ".");
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"first line\\nsecond line\"}", 2, 0,
	       "Added finding 2: candidate.");
	EXPECT(r, "{\"action\":\"update\",\"id\":2,\"state\":\"retracted\",\"evidence\":\"R\"}",
	       3, 0, "Finding 2 is now retracted; it was candidate.");
	char long_claim[CLOSED + 50];
	memset(long_claim, 'x', sizeof long_claim - 1);
	long_claim[sizeof long_claim - 1] = '\0';
	/* Put "ä" across the CLOSED limit. */
	memcpy(long_claim + CLOSED - 1, "\xc3\xa4", 2);
	char json[512];
	snprintf(json, sizeof json, "{\"action\":\"add\",\"claim\":\"%s\",\"state\":"
	         "\"disproved\",\"evidence\":\"E3\",\"location\":\"src/a.c:9\"}", long_claim);
	EXPECT(r, json, 3, 0, "Added finding 3: disproved, src/a.c:9 at " A12 ".");
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"C4\",\"location\":\"src/z.c:2-3\"}", 4, 0,
	       "Added finding 4: candidate, src/z.c:2-3, not read. Read its lines with git read "
	       "before you verify it.");
	char want[2048];
	snprintf(want, sizeof want, HEADER FINDINGS
	         "1. verified, src/a.c:3-4 at " A12 "\n   claim: C1\n   evidence: E1\n"
	         "4. candidate, src/z.c:2-3, not read\n   claim: C4\n"
	         "2. retracted: first line...\n"
	         "3. disproved, src/a.c:9 at " A12 ": %.*s...\n"
	         READS "proj " A "\n  src/a.c  lines 1-9: 9 of 300 lines",
	         CLOSED - 1, long_claim);
	RENDER(r, want);
	EXPECT(r, "{\"action\":\"show\"}", 4, 0, want);
	EXPECT(r, "{\"action\":\"show\",\"offset\":5}", 4, 0, want);
	/* A show writes its text in parts of any size. Each part but the last
	 * has the full size. */
	struct dstr history = DSTR_INIT;
	CHECK(call(r, "{\"action\":\"show\",\"id\":1}", 4, &history) == 0);
	char const *const shows[] = { "{\"action\":\"show\"}", "{\"action\":\"show\",\"id\":1}" };
	char const *const texts[] = { want, dstr_get(&history) };
	for (size_t k = 0; k < 2; ++k) {
		struct json_object *args = json_tokener_parse(shows[k]);
		size_t len = strlen(texts[k]);
		for (size_t room = 1; room <= len + 1; ++room) {
			struct dstr out = DSTR_INIT;
			struct tool_cursor cursor = {};
			int e = EAGAIN;
			for (size_t parts = 1; e == EAGAIN; ++parts) {
				e = review_call(r, args, 4, nullptr, &cursor, room, &out);
				CHECK(e == EAGAIN ? dstr_len(&out) == parts * room : !e);
			}
			SAME(dstr_get(&out), texts[k]);
			dstr_cleanup(&out);
		}
		json_object_put(args);
	}
	dstr_cleanup(&history);

	/* A render cut at any size ends after the last newline within the size,
	 * and gives the offset of the rest. */
	size_t len = strlen(want);
	for (size_t most = 0; most <= len + 1; ++most) {
		struct dstr out = DSTR_INIT;
		CHECK(dstr_printf(&out, "before"));
		CHECK(!review_render(r, &out, most));
		char const *s = dstr_get(&out);
		CHECK(!strncmp(s, "before", 6));
		s += 6;
		if (most >= len) {
			SAME(s, want);
			dstr_cleanup(&out);
			continue;
		}
		size_t keep = 0;
		for (size_t i = 0; i < most; ++i)
			keep = want[i] == '\n' ? i + 1 : keep;
		char tail[160];
		snprintf(tail, sizeof tail, "[askai: the record goes on past these %zu bytes: call "
		         "findings with action show and \"offset\": %zu for the rest.]", keep, keep);
		CHECK(!strncmp(s, want, keep) && !strcmp(s + keep, tail));
		dstr_cleanup(&out);
	}
	review_destroy(&r);
}

/* A record is charged at most TOOL_TEXT_MAX bytes, and the refusal of a
 * change that does not fit names the bytes of claim and evidence that do.
 */
static void
test_full (void)
{
	struct review *r = fresh();
	size_t big = 1 << 20;
	struct dstr out = DSTR_INIT;
	char const add[] = "{\"action\":\"add\"}";
	char const update[] = "{\"action\":\"update\",\"id\":1,\"state\":\"candidate\"}";
	/* Three claims of 1 MiB fit, and a fourth does not. Apart from its
	 * claim, each finding charges the same, and so does each update apart
	 * from its evidence. */
	for (int i = 0; i < 4; ++i)
		CHECK(call_long(r, add, "claim", big, &out) == (i < 3 ? 0 : EINVAL));
	size_t left = left_in(dstr_get(&out));
	size_t finding = (TOOL_TEXT_MAX - 3 * big - left) / 4;
	CHECK(4 * finding == TOOL_TEXT_MAX - 3 * big - left);
	CHECK(call_long(r, update, "evidence", big, &out) == EINVAL);
	size_t change = TOOL_TEXT_MAX - 3 * (big + finding) - left_in(dstr_get(&out));
	/* A change charges REVIEW_ITEM and its texts, and a finding
	 * REVIEW_ITEM, its texts and its first change. */
	CHECK(change == REVIEW_ITEM);
	CHECK(finding == change + REVIEW_ITEM);
	/* The bytes that the refusal names fit, and a byte more does not. */
	CHECK(call_long(r, add, "claim", left + 1, &out) == EINVAL);
	CHECK(left_in(dstr_get(&out)) == left);
	CHECK(!call_long(r, add, "claim", left, &out));
	SAME(dstr_get(&out), "Added finding 4: candidate.");
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\"}", 1, EINVAL, NO_MORE);
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"retracted\",\"evidence\":\"e\"}",
	       1, EINVAL, NO_MORE);
	/* A read that does not fit is not logged, and the record says so. */
	CHECK(!seen(r, "p", A, "src/a.c", 1, 3, 3, 9));
	CHECK(!seen(r, "p", A, "src/a.c", 1, 3, 3, 10));
	struct dstr text = DSTR_INIT;
	CHECK(!review_render(r, &text, SIZE_MAX));
	char const *end = strrchr(dstr_get(&text), '\n');
	SAME(end, "\n[askai: the read log is full, so what you read from turn 9 on may be "
	          "missing from it.]");
	CHECK(!strstr(dstr_get(&text), READS));
	dstr_cleanup(&text);
	/* The refusal of verified then suggests no git read. */
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\",\"evidence\":"
	          "\"e\",\"location\":\"src/a.c:3\"}", 11, EINVAL,
	       "verified needs the lines of its location in your read log, and src/a.c line 3 "
	       "is not in it. The read log has been full since turn 9, so a read of them would "
	       "not be logged: cite lines that the log has, or keep the finding a candidate.");
	review_destroy(&r);

	/* Room for an update with a byte of evidence, but not for a finding. */
	r = fresh();
	for (int i = 0; i < 3; ++i)
		CHECK(!call_long(r, add, "claim", big, &out));
	CHECK(!call_long(r, add, "claim", left - change - 1, &out));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\"}", 1, EINVAL, NO_ROOM);
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"candidate\",\"evidence\":\"e\"}",
	       1, 0, "Finding 1 stays candidate; the new evidence is in its history.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"candidate\",\"evidence\":\"e\"}",
	       1, EINVAL, NO_MORE);
	review_destroy(&r);

	/* With room for less than a range, the refusal names the commit of the
	 * logged lines but suggests no read of the rest. */
	r = fresh();
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 10, 300, 1));
	CHECK(call_long(r, add, "claim", TOOL_TEXT_MAX, &out) == EINVAL);
	CHECK(!call_long(r, add, "claim", left_in(dstr_get(&out)) - 10, &out));
	char const wide[] = "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	                    "\"evidence\":\"e\",\"location\":\"src/a.c:10-11\"}";
	char const some[] = "verified needs the lines of its location in your read log, and "
	                    "src/a.c lines 10-11 are not all in it: it has line 10 of them, read at "
	                    A12 ".";
	char const unlogged[] = " so a read of them would not be logged: cite lines that the log "
	                        "has, or keep the finding a candidate.";
	char want[512];
	snprintf(want, sizeof want, "%s The record is full,%s", some, unlogged);
	EXPECT(r, wide, 2, EINVAL, want);
	CHECK(!seen(r, "proj", A, "src/a.c", 20, 30, 300, 2));
	snprintf(want, sizeof want, "%s The read log has been full since turn 2,%s", some, unlogged);
	EXPECT(r, wide, 3, EINVAL, want);
	review_destroy(&r);

	/* A suggested read adds a new pending file, charged 2 * REVIEW_ITEM and
	 * the bytes of its repository name and path. With one byte less room,
	 * the refusal suggests no read. With that room, it suggests the read,
	 * and the log then takes the lines. */
	size_t file = 2 * REVIEW_ITEM + 4 + 7;
	for (size_t room = file - 1; room <= file; ++room) {
		r = fresh();
		CHECK(!seen(r, "proj", A, "src/a.c", 1, 10, 300, 1));
		CHECK(call_long(r, add, "claim", TOOL_TEXT_MAX, &out) == EINVAL);
		CHECK(!call_long(r, add, "claim", left_in(dstr_get(&out)) - room, &out));
		if (room < file) {
			snprintf(want, sizeof want, "%s The record is full,%s", some, unlogged);
			EXPECT(r, wide, 2, EINVAL, want);
		} else {
			snprintf(want, sizeof want, "%s Read the rest with the git tool's read command, "
			         "with \"revisions\": [\"" A12 ":src/a.c\"] and \"line\": 11, in as many "
			         "calls as it takes." LATER, some);
			EXPECT(r, wide, 2, EINVAL, want);
			CHECK(!seen(r, "proj", A, "src/a.c", 11, 20, 300, 2));
			lines_of(r, "lines 1-20: 20 of 300 lines", __LINE__);
		}
		review_destroy(&r);
	}

	/* An update charges a change, its evidence and each new text: the old
	 * claim, the path of the old location, or the full path of a short one.
	 * A restore charges the same. */
	r = fresh();
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\"}", 1, 0, "Added finding 1: candidate.");
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"a.c:3\"}", 1, 0,
	       "Added finding 2: candidate, a.c:3, not read. Read its lines with git read before "
	       "you verify it.");
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 10, 300, 1));
	struct {
		char const *json;
		size_t      more;   /* bytes past the change and its evidence */
		char const *want;
	} const updates[] = {
		{ "{\"action\":\"update\",\"id\":2,\"state\":\"candidate\",\"evidence\":\"e\"}", 4,
		  "Finding 2 stays candidate, src/a.c:3 at " A12 "; the new evidence is in its "
		  "history." },
		{ "{\"action\":\"update\",\"id\":2,\"state\":\"candidate\",\"evidence\":\"e\","
		  "\"location\":\"src/a.c:5\"}", 7,
		  "Finding 2 stays candidate, src/a.c:5 at " A12 "; the new evidence is in its "
		  "history. Its old location is in its history." },
		{ "{\"action\":\"update\",\"id\":2,\"state\":\"candidate\",\"evidence\":\"e\","
		  "\"claim\":\"C2\"}", 2,
		  "Finding 2 stays candidate, src/a.c:5 at " A12 "; the new evidence is in its "
		  "history. Its old claim is in its history." },
		{ "{\"action\":\"update\",\"id\":2,\"state\":\"candidate\",\"evidence\":\"e\"}", 0,
		  "Finding 2 stays candidate, src/a.c:5 at " A12 "; the new evidence is in its "
		  "history." },
	};
	for (size_t i = 0; i < array_size(updates); ++i) {
		size_t had = room_of(r);
		EXPECT(r, updates[i].json, 2, 0, updates[i].want);
		CHECK(had - room_of(r) == change + 1 + updates[i].more);
	}
	int e;
	struct json_object *obj = review_json(r, &e);
	struct review *back = fresh();
	CHECK(obj && !e && !review_restore(back, obj));
	CHECK(room_of(back) == room_of(r));
	json_object_put(obj);
	review_destroy(&back);
	review_destroy(&r);
	dstr_cleanup(&out);
}

/* Without REVIEW_READ_LOG the read log stays empty, verified and disproved
 * are refused, and no result suggests a git read, also after a restore.
 */
static void
test_no_git (void)
{
	int e;
	struct review *r = review_create(0, &e);
	CHECK(r && !e);
	char const no_git[] = "This ask call has no git tool, so a finding cannot be verified or "
	                      "disproved here; keep it a candidate, or retract it.";
	char const verify[] = "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"verified\","
	                      "\"evidence\":\"e\",\"location\":\"src/a.c:3\"}";
	EXPECT(r, verify, 1, EINVAL, no_git);
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"state\":\"disproved\"}", 1, EINVAL,
	       no_git);
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"c\",\"location\":\"src/a.c:3\"}", 1, 0,
	       "Added finding 1: candidate, src/a.c:3, not read.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"disproved\",\"evidence\":\"e\"}",
	       1, EINVAL, no_git);
	struct json_object *obj = review_json(r, &e);
	CHECK(obj && !e);
	struct review *back = review_create(0, &e);
	CHECK(back && !e && !review_restore(back, obj));
	EXPECT(back, verify, 2, EINVAL, no_git);
	EXPECT(back, "{\"action\":\"update\",\"id\":1,\"state\":\"retracted\",\"evidence\":"
	             "\"e\"}", 2, 0, "Finding 1 is now retracted, src/a.c:3, not read; it was "
	                          "candidate.");
	/* review_restore() does not change the REVIEW_READ_LOG flag of the
	 * target. */
	struct review *reads = fresh();
	CHECK(!review_restore(reads, obj));
	EXPECT(reads, verify, 2, EINVAL,
	       "verified needs the lines of its location in your read log, and src/a.c line 3 is "
	       "not in it. Read them with the git tool's read command, with \"revisions\": "
	       "[\"HEAD:src/a.c\"] and \"line\": 3" WHOLE "." LATER);
	json_object_put(obj);
	review_destroy(&reads);
	review_destroy(&back);
	review_destroy(&r);

	/* A record without REVIEW_READ_LOG restores the findings of a saved
	 * record but not its read log and pending lines. */
	r = fresh();
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 9, 300, 1));
	CHECK(!shown_in(r, "proj", A, "src/b.c", 1, 2, 2, 0, SIZE_MAX, 2));
	EXPECT(r, verify, 2, 0, "Added finding 1: verified, src/a.c:3 at " A12 ".");
	obj = review_json(r, &e);
	CHECK(obj && !e);
	back = review_create(0, &e);
	CHECK(back && !e && !review_restore(back, obj));
	struct dstr text = DSTR_INIT;
	CHECK(!review_render(back, &text, SIZE_MAX));
	SAME(dstr_get(&text), HEADER FINDINGS "1. verified, src/a.c:3 at " A12 "\n   claim: c\n"
	                      "   evidence: e");
	dstr_cleanup(&text);
	CHECK(!review_pending_log(back));
	json_object_put(obj);
	obj = review_json(back, &e);
	CHECK(obj && !e);
	SAME(json_object_to_json_string_ext(json_object_object_get(obj, "reads"),
	                                    JSON_C_TO_STRING_PLAIN), "[]");
	json_object_put(obj);
	review_destroy(&back);
	review_destroy(&r);
}

/* review_json() and review_restore() give the same record back, and a
 * restore refuses what review_json() would not write.
 */
static void
test_json (void)
{
	struct review *r = fresh();
	CHECK(!seen(r, "proj", A, "src/a.c", 1, 9, 300, 1));
	CHECK(!seen(r, "proj", A, "src/a.c", 20, 29, 300, 1));
	CHECK(!seen(r, "other", B, "b.c", 2, 2, 0, 2));
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"C1\",\"state\":\"verified\",\"evidence\":"
	          "\"E1\",\"location\":\"src/a.c:3-4\"}", 2, 0,
	       "Added finding 1: verified, src/a.c:3-4 at " A12 ".");
	EXPECT(r, "{\"action\":\"add\",\"claim\":\"C2\"}", 2, 0, "Added finding 2: candidate.");
	EXPECT(r, "{\"action\":\"update\",\"id\":1,\"state\":\"retracted\",\"evidence\":\"R\","
	          "\"claim\":\"C3\",\"location\":\"b.c:2\"}", 3, 0,
	       "Finding 1 is now retracted, b.c:2 at " B12 "; it was verified. Its old claim and "
	       "location are in its history.");
	CHECK(!seen(r, "proj", A, "src/a.c", 10, 12, 300, 3));
	CHECK(!shown_in(r, "proj", A, "src/a.c", 40, 41, 300, 0, SIZE_MAX, 4));
	int e;
	struct json_object *obj = review_json(r, &e);
	CHECK(obj && !e);
	char const *json = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
	SAME(json, "{\"reads\":[{\"repo\":\"proj\",\"commit\":\"" A "\",\"path\":\"src\\/a.c\","
	           "\"total\":300,\"lines\":[1,9,1,10,12,3,20,29,1]},{\"repo\":\"other\","
	           "\"commit\":\"" B "\",\"path\":\"b.c\",\"total\":0,\"lines\":[2,2,2]}],"
	           "\"pending\":[{\"repo\":\"proj\",\"commit\":\"" A "\",\"path\":\"src\\/a.c\","
	           "\"total\":300,\"lines\":[40,41,4]}],\"full\":0,\"findings\":[{\"claim\":"
	           "\"C3\",\"history\":"
	           "[{\"turn\":2,\"state\":\"verified\",\"commit\":\"" A "\",\"evidence\":\"E1\"},"
	           "{\"turn\":3,\"state\":\"retracted\",\"commit\":\"" B "\",\"evidence\":\"R\","
	           "\"claim\":\"C1\",\"location\":{\"first\":3,\"last\":4,\"path\":\"src\\/a.c\"}}],"
	           "\"first\":2,\"last\":2,\"path\":\"b.c\"},"
	           "{\"claim\":\"C2\",\"history\":[{\"turn\":2,\"state\":"
	           "\"candidate\",\"commit\":\"\",\"evidence\":\"\"}],\"first\":0,\"last\":0}]}");
	struct review *back = fresh();
	CHECK(!review_restore(back, obj));
	struct dstr a = DSTR_INIT;
	struct dstr b = DSTR_INIT;
	CHECK(!review_render(r, &a, SIZE_MAX) && !review_render(back, &b, SIZE_MAX));
	SAME(dstr_get(&b), dstr_get(&a));
	struct json_object *again = review_json(back, &e);
	CHECK(again && !e);
	SAME(json_object_to_json_string_ext(again, JSON_C_TO_STRING_PLAIN), json);
	json_object_put(again);
	/* The restored record accepts further updates, and review_pending_log() logs
	 * its restored pending lines. */
	EXPECT(back, "{\"action\":\"update\",\"id\":2,\"state\":\"verified\",\"evidence\":\"E\","
	             "\"location\":\"a.c:25\"}", 5, 0,
	       "Finding 2 is now verified, src/a.c:25 at " A12 "; it was candidate. Its old "
	       "location is in its history.");
	CHECK(!review_pending_log(back));
	lines_of(back, "lines 1-12, 20-29, 40-41: 24 of 300 lines\n"
	               "other " B "\n  b.c  line 2: 1 of an unknown number of lines", __LINE__);
	/* The old location of a finding without one is saved without a path
	 * and with the lines 0 to 0, and it restores. */
	struct json_object *moved = review_json(back, &e);
	CHECK(moved && !e && strstr(json_object_to_json_string_ext(moved, JSON_C_TO_STRING_PLAIN),
	                            "\"location\":{\"first\":0,\"last\":0}}"));
	struct review *other = fresh();
	CHECK(!review_restore(other, moved));
	struct dstr shown = DSTR_INIT;
	struct dstr restored = DSTR_INIT;
	CHECK(!review_render(back, &shown, SIZE_MAX) && !review_render(other, &restored, SIZE_MAX));
	SAME(dstr_get(&restored), dstr_get(&shown));
	dstr_cleanup(&shown);
	dstr_cleanup(&restored);
	review_destroy(&other);
	json_object_put(moved);

	/* Each defect makes the restore fail and leaves the record as it was. */
	struct {
		char const *from;
		char const *to;
	} const defects[] = {
		/* Adjoining ranges of the same turn, which merge, overlapping
		 * ranges, ranges out of order and a range past the total. */
		{ "[1,9,1,10,12,3,20,29,1]", "[1,9,1,10,12,1,20,29,1]" },
		{ "[1,9,1,10,12,3,20,29,1]", "[1,9,1,9,12,3,20,29,1]" },
		{ "[1,9,1,10,12,3,20,29,1]", "[10,12,3,1,9,1,20,29,1]" },
		{ "[1,9,1,10,12,3,20,29,1]", "[1,9,1,10,12,3,20,301,1]" },
		{ "[1,9,1,10,12,3,20,29,1]", "[0,12,3]" },
		{ "[1,9,1,10,12,3,20,29,1]", "[12,1,3]" },
		{ "[1,9,1,10,12,3,20,29,1]", "[1,9,1,10,12,3,20,29]" },
		{ "[1,9,1,10,12,3,20,29,1]", "[]" },
		{ "[1,9,1,10,12,3,20,29,1]", "[1,\"12\",3]" },
		{ "[1,9,1,10,12,3,20,29,1]", "[1,12,-1]" },
		{ "[1,9,1,10,12,3,20,29,1]", "[1,12,4294967296]" },
		{ "[1,9,1,10,12,3,20,29,1]", "[1,12,\"3\"]" },
		{ "\"lines\":[40,41,4]", "\"lines\":[40,41]" },
		/* The pending file and the logged file disagree: the totals
		 * differ, the pending lines go past the logged total, or the
		 * logged lines go past the pending total. */
		{ "\"total\":300,\"lines\":[40,41,4]", "\"total\":200,\"lines\":[40,41,4]" },
		{ "\"total\":300,\"lines\":[40,41,4]", "\"total\":0,\"lines\":[301,302,4]" },
		{ "\"total\":300,\"lines\":[1,9,1,10,12,3,20,29,1]",
		  "\"total\":0,\"lines\":[1,9,1,10,12,3,20,301,1]" },
		/* A verified finding without a location. */
		{ "{\"turn\":2,\"state\":\"candidate\",\"commit\":\"\",\"evidence\":\"\"}",
		  "{\"turn\":2,\"state\":\"verified\",\"commit\":\"" A "\",\"evidence\":\"E\"}" },
		{ "\"commit\":\"" B "\",\"path\"", "\"commit\":\"" A "1\",\"path\"" },
		{ "\"repo\":\"other\"", "\"repo\":\"\"" },
		{ "\"total\":0", "\"total\":-1" },
		{ "\"pending\":[", "\"pending\":0,\"p\":[" },
		{ "\"full\":0", "\"full\":-1" },
		{ "\"full\":0", "\"full\":\"0\"" },
		{ "\"state\":\"verified\"", "\"state\":\"open\"" },
		{ "\"commit\":\"" A "\",\"evidence\":\"E1\"", "\"commit\":\"\",\"evidence\":\"E1\"" },
		{ "\"evidence\":\"E1\"", "\"evidence\":\"\"" },
		{ "\"evidence\":\"E1\"", "\"evidence\":1" },
		{ "\"claim\":\"C1\"", "\"claim\":\"\"" },
		{ "\"claim\":\"C2\"", "\"claim\":\"\"" },
		{ "\"first\":0,\"last\":0", "\"first\":1,\"last\":1" },
		{ "\"first\":2,\"last\":2", "\"first\":3,\"last\":2" },
		{ "\"path\":\"b.c\"}", "\"path\":\"\"}" },
		{ "\"history\":[{\"turn\":2,\"state\":\"candidate\",\"commit\":\"\",\"evidence\":\"\"}]",
		  "\"history\":[]" },
		{ "\"reads\":[", "\"reads\":0,\"r\":[" },
		/* An old location in the old form of a string, and old locations
		 * whose lines do not fit their path. */
		{ "{\"first\":3,\"last\":4,\"path\":\"src\\/a.c\"}", "\"src\\/a.c:3-4\"" },
		{ "{\"first\":3,\"last\":4,\"path\":\"src\\/a.c\"}", "\"\"" },
		{ "{\"first\":3,\"last\":4,\"path\":\"src\\/a.c\"}", "5" },
		{ "\"first\":3,\"last\":4", "\"first\":0,\"last\":4" },
		{ "\"first\":3,\"last\":4", "\"first\":4,\"last\":3" },
		{ "\"first\":3,\"last\":4", "\"last\":4" },
		{ "\"last\":4,\"path\":\"src\\/a.c\"", "\"last\":4,\"path\":\"\"" },
		{ "\"last\":4,\"path\":\"src\\/a.c\"", "\"last\":4" },
	};
	struct dstr before = DSTR_INIT;
	CHECK(!review_render(back, &before, SIZE_MAX));
	for (size_t i = 0; i < array_size(defects); ++i) {
		char const *at = strstr(json, defects[i].from);
		CHECK(at);
		if (!at)
			continue;
		struct dstr bad = DSTR_INIT;
		CHECK(dstr_cat_n(&bad, json, (size_t)(at - json)) && dstr_cat(&bad, defects[i].to) &&
		      dstr_cat(&bad, at + strlen(defects[i].from)));
		struct json_object *o = json_tokener_parse(dstr_get(&bad));
		CHECK(o);
		int got_e = review_restore(back, o);
		if (got_e != EINVAL)
			fprintf(stderr, "defect %zu: %d\n", i, got_e);
		CHECK(got_e == EINVAL);
		json_object_put(o);
		dstr_cleanup(&bad);
	}
	/* A pending file with total 0 (unknown, as after the 4 MiB cut) is
	 * accepted if its lines are within the logged total. */
	char const unknown[] = "\"total\":300,\"lines\":[40,41,4]";
	char const *at = strstr(json, unknown);
	struct dstr cut = DSTR_INIT;
	CHECK(at && dstr_cat_n(&cut, json, (size_t)(at - json)) &&
	      dstr_cat(&cut, "\"total\":0,\"lines\":[40,300,4]") &&
	      dstr_cat(&cut, at + sizeof unknown - 1));
	struct json_object *some = json_tokener_parse(dstr_get(&cut));
	struct review *known = fresh();
	CHECK(some && !review_restore(known, some));
	json_object_put(some);
	review_destroy(&known);
	dstr_cleanup(&cut);
	/* A file twice in the log or in the pending lines, and files of one
	 * commit that are not next to each other. */
	char const *const lists[] = { "reads", "pending" };
	for (size_t i = 0; i < array_size(lists); ++i) {
		struct json_object *dup = json_tokener_parse(json);
		struct json_object *files = json_object_object_get(dup, lists[i]);
		json_object_array_add(files, json_object_get(json_object_array_get_idx(files, 0)));
		CHECK(review_restore(back, dup) == EINVAL);
		json_object_put(dup);
	}
	struct json_object *apart = json_tokener_parse(json);
	struct json_object *reads = json_object_object_get(apart, "reads");
	struct json_object *c = json_tokener_parse(json_object_to_json_string(
		json_object_array_get_idx(reads, 0)));
	json_object_object_add(c, "path", json_object_new_string("src/c.c"));
	json_object_array_add(reads, c);
	/* A file after the files of another commit is restored next to the other
	 * file of its commit. */
	struct review *grouped = fresh();
	CHECK(!review_restore(grouped, apart));
	lines_of(grouped, "lines 1-12, 20-29: 22 of 300 lines\n  src/c.c  lines 1-12, 20-29: 22 of "
	                  "300 lines\nother " B "\n  b.c  line 2: 1 of an unknown number of lines",
	         __LINE__);
	review_destroy(&grouped);
	json_object_put(apart);
	/* A claim over TOOL_TEXT_MAX. */
	size_t big = TOOL_TEXT_MAX;
	char *claim = malloc(big);
	CHECK(claim);
	if (claim) {
		memset(claim, 'c', big);
		struct json_object *huge = json_tokener_parse(json);
		struct json_object *f = json_object_array_get_idx(json_object_object_get(huge,
		                                                                       "findings"), 1);
		json_object_object_add(f, "claim", json_object_new_string_len(claim, (int)big));
		CHECK(review_restore(back, huge) == EINVAL);
		json_object_put(huge);
		free(claim);
	}
	struct json_object *array = json_object_new_array();
	CHECK(review_restore(back, array) == EINVAL);
	json_object_put(array);
	struct dstr after = DSTR_INIT;
	CHECK(!review_render(back, &after, SIZE_MAX));
	SAME(dstr_get(&after), dstr_get(&before));
	/* The empty record. */
	struct review *empty = fresh();
	struct json_object *none = review_json(empty, &e);
	SAME(json_object_to_json_string_ext(none, JSON_C_TO_STRING_PLAIN),
	     "{\"reads\":[],\"pending\":[],\"full\":0,\"findings\":[]}");
	CHECK(!review_restore(back, none));
	RENDER(back, HEADER NONE);
	json_object_put(none);
	review_destroy(&empty);
	dstr_cleanup(&before);
	dstr_cleanup(&after);
	dstr_cleanup(&a);
	dstr_cleanup(&b);
	json_object_put(obj);
	review_destroy(&back);
	review_destroy(&r);

	/* A read and its JSON. */
	struct dstr out = DSTR_INIT;
	struct review_source src;
	output(&out, &src, "proj", A, "src/a.c", 3, 5, 9, false);
	struct cstr_view o = cstr_view_dstr(&out);
	struct review_read *read = review_read_create(&src, o, &e);
	CHECK(read && !e);
	struct json_object *rj = review_read_json(read, &e);
	CHECK(rj && !e);
	SAME(json_object_to_json_string_ext(rj, JSON_C_TO_STRING_PLAIN),
	     "{\"repo\":\"proj\",\"commit\":\"" A "\",\"path\":\"src\\/a.c\",\"at\":4,"
	     "\"len\":27,\"first\":3,\"last\":5,\"total\":9}");
	struct review_read *copy = review_read_create_json(rj, o, &e);
	CHECK(copy && !e);
	r = fresh();
	CHECK(!review_pending_add(r, copy, o, 0, SIZE_MAX, 1) && !review_pending_log(r));
	lines_of(r, "lines 3-5: 3 of 9 lines", __LINE__);
	review_destroy(&r);
	review_read_destroy(&copy);
	/* The output must still hold the lines, and each member must be there. */
	CHECK(!review_read_create_json(rj, (struct cstr_view){ o.data, 20 }, &e) && e == EINVAL);
	char const *const members[] = { "repo", "commit", "path", "at", "len", "first", "last",
	                                "total" };
	for (size_t i = 0; i < array_size(members); ++i) {
		struct json_object *less = json_tokener_parse(
			json_object_to_json_string_ext(rj, JSON_C_TO_STRING_PLAIN));
		json_object_object_del(less, members[i]);
		CHECK(!review_read_create_json(less, o, &e) && e == EINVAL);
		json_object_put(less);
	}
	json_object_put(rj);
	review_read_destroy(&read);
	dstr_cleanup(&out);
}

/* A record with as many files as fit in TOOL_TEXT_MAX restores, and a
 * record with one more file is too large.
 */
static void
test_many (void)
{
	/* Each file charges 2 * REVIEW_ITEM, 1 byte of repository name and 6
	 * bytes of path. */
	size_t const fit = TOOL_TEXT_MAX / (2 * REVIEW_ITEM + 1 + 6);
	for (size_t n = fit; n <= fit + 1; ++n) {
		struct dstr json = DSTR_INIT;
		bool ok = dstr_printf(&json, "{\"reads\":[");
		for (size_t i = 0; ok && i < n; ++i)
			ok = dstr_printf(&json, "%s{\"repo\":\"p\",\"commit\":\"" A "\",\"path\":"
			                 "\"f%05zu\",\"total\":0,\"lines\":[1,1,1]}", i ? "," : "", i);
		CHECK(ok && dstr_printf(&json, "],\"pending\":[],\"full\":0,\"findings\":[]}"));
		struct json_object *obj = json_tokener_parse(dstr_get(&json));
		CHECK(obj);
		struct review *r = fresh();
		CHECK(review_restore(r, obj) == (n == fit ? 0 : EINVAL));
		review_destroy(&r);
		json_object_put(obj);
		dstr_cleanup(&json);
	}
}

/* Every function checks its pointers. */
static void
test_nullptr (void)
{
	int e = 0;
	CHECK(!review_create(REVIEW_READ_LOG, nullptr));
	review_destroy(nullptr);
	struct review *none = nullptr;
	review_destroy(&none);
	struct review_read *no_read = nullptr;
	review_read_destroy(nullptr);
	review_read_destroy(&no_read);
	struct cstr_view out = { "x\n", 2 };
	CHECK(!review_read_create(nullptr, out, &e) && e == EFAULT);
	struct review_source src = { cstr_view("p"), cstr_view(A), cstr_view("a"), 0, 0, 2, 1, 1 };
	CHECK(!review_read_create(&src, out, nullptr));
	e = 0;
	CHECK(!review_read_create(&src, (struct cstr_view){ nullptr, 2 }, &e) && e == EFAULT);
	struct review_read *read = review_read_create(&src, out, &e);
	CHECK(read && !e);
	struct review *r = fresh();
	CHECK(review_pending_add(nullptr, read, out, 0, SIZE_MAX, 1) == EFAULT);
	CHECK(review_pending_add(r, nullptr, out, 0, SIZE_MAX, 1) == EFAULT);
	CHECK(review_pending_add(r, read, (struct cstr_view){ nullptr, 2 }, 0, SIZE_MAX, 1) == EFAULT);
	CHECK(review_pending_log(nullptr) == EFAULT);
	review_pending_drop(nullptr);
	struct dstr d = DSTR_INIT;
	struct json_object *args = json_tokener_parse("{\"action\":\"show\"}");
	CHECK(review_call(nullptr, args, 1, nullptr, &(struct tool_cursor){}, 1, &d) == EFAULT);
	CHECK(review_call(r, args, 1, nullptr, nullptr, 1, &d) == EFAULT);
	CHECK(review_call(r, args, 1, nullptr, &(struct tool_cursor){}, 1, nullptr) == EFAULT);
	CHECK(review_render(r, nullptr, SIZE_MAX) == EFAULT);
	CHECK(!review_json(r, nullptr));
	CHECK(!review_json(nullptr, &e) && e == EFAULT);
	CHECK(review_restore(nullptr, args) == EFAULT);
	CHECK(review_restore(r, nullptr) == EFAULT);
	CHECK(!review_read_json(nullptr, &e) && e == EFAULT);
	CHECK(!review_read_json(read, nullptr));
	CHECK(!review_read_create_json(nullptr, out, &e) && e == EFAULT);
	CHECK(!review_read_create_json(args, out, nullptr));
	CHECK(!dstr_len(&d));
	json_object_put(args);
	review_read_destroy(&read);
	review_destroy(&r);
}

int
main (void)
{
	test_log();
	test_seen();
	test_refusals();
	test_paths();
	test_paint();
	test_gate();
	test_pending();
	test_history();
	test_render();
	test_full();
	test_no_git();
	test_json();
	test_many();
	test_nullptr();
	return check_report("review");
}
