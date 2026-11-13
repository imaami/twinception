/** @file Unit tests of the evidence of the findings (src/evidence.c): the
 *  references, the items that the calls of the ring make, the refusals,
 *  the external items, the archive paths, the JSON of an item, the
 *  summaries, the items cited again, and the items as expand prints them.
 *  The ranges of numbers (src/range.h) are tested here too.
 */
#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "check.h"
#include "dstr.h"
#include "evidence.h"
#include "finding_id.h"
#include "flag.h"
#include "jb.h"
#include "job.h"
#include "range.h"
#include "seen.h"
#include "tool.h"
#include "util.h"

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

/* The names of the tools by registry ID: 1 git, 2 man, 3 findings. */
static char const *
tool_name (void const *owner,
           uint32_t    tool)
{
	static char const *const names[] = { "zero", "git", "man", "findings" };
	(void)owner;
	return tool < sizeof names / sizeof *names ? names[tool] : nullptr;
}

/* Fill @p request with a call of the provider ID @p id and the arguments
 * @p args, as the upstream sent them (struct job_call).
 */
static void
request_of (struct buf *request,
            char const *id,
            char const *args)
{
	struct job_call const h = {
		.id   = (uint32_t)strlen(id),
		.name = 3,
		.args = (uint32_t)strlen(args),
	};
	CHECK(!buf_put(request, &h, sizeof h, SIZE_MAX));
	CHECK(!buf_put(request, id, h.id, SIZE_MAX));
	CHECK(!buf_put(request, "abc", 3, SIZE_MAX));
	CHECK(!buf_put(request, args, h.args, SIZE_MAX));
}

/* Add the call @p number of the turn @p turn and the tool @p tool to
 * @p *p_seen, with the provider ID @p id, the arguments @p args and the
 * output @p out, all of it the window. @p read names numbered lines of
 * @p out, or is nullptr. @p large makes the call too large to keep.
 */
static void
keep (struct seen     **p_seen,
      uint32_t          number,
      uint32_t          turn,
      uint32_t          tool,
      char const       *id,
      char const       *args,
      struct cstr_view  out,
      struct seen_read *read,
      bool              large)
{
	struct buf request = buf();
	request_of(&request, id, args);
	if (large) {
		struct span room = {};
		CHECK(!buf_reserve(&request, TOOL_TEXT_MAX, SIZE_MAX, &room));
		memset(room.ptr, 'a', TOOL_TEXT_MAX);
		buf_commit(&request, TOOL_TEXT_MAX);
	}
	char note[64];
	snprintf(note, sizeof note, "\n\n[askai: call c%u]", number);
	struct cstr_view const parts[SEEN_PARTS] = { out, cstr_view(note) };
	struct seen_result const r = {
		.parts = parts,
		.out   = out,
		.read  = read,
		.stamp = { .output = out.len,
		           .number = number,
		           .turn   = turn,
		           .tool   = tool,
		           .index  = (uint16_t)(number % 3),
		           .flags  = number == 2 ? FLAG_FAILED : 0 },
	};
	CHECK(!seen_keep(p_seen, &request, &r));
	buf_fini(&request);
}

/* Check that the reference @p text parses with the form @p form, or fails
 * with @p form -1.
 */
static void
parsed (char const *text,
        int         form,
        int         line)
{
	struct evidence_ref ref;
	int const e = evidence_ref_parse(&ref, cstr_view(text));
	if (form < 0 ? e == EINVAL : !e && ref.form == form)
		return;
	++failures;
	fprintf(stderr, "%s:%d: %s: error %d, form %d\n", __FILE__, line, text, e, ref.form);
}

/* range_count() counts the numbers of a range, range_contains() compares
 * two ranges, and range_merge() sorts ranges and merges the ranges that
 * overlap or touch.
 */
static void
test_range (void)
{
	CHECK(range_count(range(5, 5)) == 1 && range_count(range(3, 9)) == 7);
	CHECK(range_count(range(0, UINT32_MAX)) == (uint64_t)UINT32_MAX + 1);
	struct range const outer = range(3, 9);
	CHECK(range_contains(outer, outer) && range_contains(outer, range(3, 3)) &&
	      range_contains(outer, range(9, 9)) &&
	      range_contains(outer, range(4, 8)));
	CHECK(!range_contains(outer, range(2, 9)) &&
	      !range_contains(outer, range(3, 10)) &&
	      !range_contains(outer, range(2, 10)) &&
	      !range_contains(outer, range(1, 2)) &&
	      !range_contains(outer, range(10, UINT32_MAX)));
	static struct {
		struct range in[4];
		struct range out[4];
		uint32_t     n;     /* ranges of in */
		uint32_t     left;  /* ranges of out */
	} const cases[] = {
		/* One range, and equal ranges. */
		{ { { 7, 7 } }, { { 7, 7 } }, 1, 1 },
		{ { { 5, 8 }, { 5, 8 }, { 5, 8 } }, { { 5, 8 } }, 3, 1 },
		/* Unsorted ranges that are apart. */
		{ { { 9, 9 }, { 1, 3 }, { 5, 7 } }, { { 1, 3 }, { 5, 7 }, { 9, 9 } }, 3, 3 },
		/* Ranges that overlap, and a range within another. */
		{ { { 3, 6 }, { 5, 9 }, { 1, 1 }, { 4, 4 } }, { { 1, 1 }, { 3, 9 } }, 4, 2 },
		{ { { 5, 6 }, { 1, 10 }, { 2, 2 }, { 12, 12 } }, { { 1, 10 }, { 12, 12 } }, 4, 2 },
		{ { { 5, 7 }, { 5, 9 }, { 5, 6 } }, { { 5, 9 } }, 3, 1 },
		/* Ranges that touch. */
		{ { { 141, 150 }, { 120, 140 } }, { { 120, 150 } }, 2, 1 },
		{ { { 3, 3 }, { 1, 1 }, { 2, 2 }, { 5, 5 } }, { { 1, 3 }, { 5, 5 } }, 4, 2 },
		{ { { 0, 0 }, { 1, 1 } }, { { 0, 1 } }, 2, 1 },
		/* Ranges that end at UINT32_MAX. The sum last + 1 does not wrap. */
		{ { { 5, 9 }, { 1, UINT32_MAX }, { 3, 3 } }, { { 1, UINT32_MAX } }, 3, 1 },
		{ { { UINT32_MAX, UINT32_MAX }, { 0, UINT32_MAX }, { 0, 0 } },
		  { { 0, UINT32_MAX } }, 3, 1 },
		{ { { UINT32_MAX, UINT32_MAX }, { UINT32_MAX - 1, UINT32_MAX - 1 } },
		  { { UINT32_MAX - 1, UINT32_MAX } }, 2, 1 },
		{ { { 5, UINT32_MAX }, { 1, 3 } }, { { 1, 3 }, { 5, UINT32_MAX } }, 2, 2 },
	};
	for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
		struct range r[4] = {};
		memcpy(r, cases[i].in, sizeof r);
		size_t const left = range_merge(r, cases[i].n);
		if (left == cases[i].left && !memcmp(r, cases[i].out, left * sizeof *r))
			continue;
		++failures;
		fprintf(stderr, "%s:%d: case %zu: %zu ranges, the first %u-%u\n", __FILE__, __LINE__, i,
		        left, r[0].first, r[0].last);
	}
	/* No range. */
	struct range none = range(2, 4);
	CHECK(!range_merge(&none, 0) && none.first == 2 && none.last == 4);
	CHECK(!range_merge(nullptr, 0));
}

/* util_ordinal() reads a number from 1 without a leading zero at the
 * start of a text and drops its digits, or leaves the text and the number
 * as they were.
 */
static void
test_ordinal (void)
{
	static struct {
		char const *text;
		char const *rest;  /* the text after the number */
		uint32_t    n;
	} const good[] = {
		{ "1", "", 1 },
		{ "7", "", 7 },
		{ "10", "", 10 },
		{ "4294967295", "", UINT32_MAX },
		{ "12-15,3", "-15,3", 12 },
		{ "5/2", "/2", 5 },
		{ "9 ", " ", 9 },
		{ "100x", "x", 100 },
		{ "1.5", ".5", 1 },
	};
	for (size_t i = 0; i < sizeof good / sizeof *good; ++i) {
		struct cstr_view text = cstr_view(good[i].text);
		uint32_t n = 77;
		if (!util_ordinal(&text, &n) && n == good[i].n &&
		    cstr_view_equal(text, cstr_view(good[i].rest)) &&
		    text.data == good[i].text + strlen(good[i].text) - strlen(good[i].rest))
			continue;
		++failures;
		fprintf(stderr, "%s:%d: %s: got %u and %.*s\n", __FILE__, __LINE__, good[i].text, n,
		        (int)text.len, text.data);
	}
	static char const *const bad[] = {
		"", "0", "00", "07", "0x", "x", "x7", " 7", "-7", "+7", ".7", "4294967296",
		"42949672950", "99999999999999999999",
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
		struct cstr_view text = cstr_view(bad[i]);
		uint32_t n = 77;
		if (util_ordinal(&text, &n) == EINVAL && n == 77 && text.data == bad[i] &&
		    text.len == strlen(bad[i]))
			continue;
		++failures;
		fprintf(stderr, "%s:%d: read %s\n", __FILE__, __LINE__, bad[i]);
	}
	/* The number ends where the view ends, and a view without bytes is no
	 * number. */
	struct cstr_view part = cstr_view_n("1234", 2);
	uint32_t n = 77;
	CHECK(!util_ordinal(&part, &n) && n == 12 && !part.len);
	CHECK(util_ordinal(&part, &n) == EINVAL && n == 12);
	struct cstr_view none = {};
	CHECK(util_ordinal(&none, &n) == EINVAL && n == 12 && !none.data && !none.len);
}

/* evidence_ref_parse() reads every form of a reference, and refuses
 * anything else.
 */
static void
test_parse (void)
{
	parsed("c7", EVIDENCE_REF_CALL, __LINE__);
	parsed("c4294967295", EVIDENCE_REF_CALL, __LINE__);
	parsed("c7:120-140,150", EVIDENCE_REF_LINES, __LINE__);
	parsed("c7:5", EVIDENCE_REF_LINES, __LINE__);
	parsed("cuh#2", EVIDENCE_REF_FINDING, __LINE__);
	parsed("cuh#2/1", EVIDENCE_REF_ITEM, __LINE__);
	parsed("first.last@ad#12/3:161-165", EVIDENCE_REF_ITEM_LINES, __LINE__);
	char const *const bad[] = {
		"", "c", "c0", "c07", "C7", "c-1", "c7 ", " c7", "c7:", "c7:0", "c7:5-3", "c7:1-",
		"c7:1,", "c7:,1", "c7:msg", "c7:1-2:3", "c7:message", "c7:files", "c7:sources",
		"c7:message,1", "c4294967296", "cuh#0", "cuh#2/0", "cuh#2/", "cuh#", "#2/1",
		"c7#2/1", ".x#2/1", "cuh#2:message", "cuh#2/1:files", "cuh#2/1/1", "cuh#02/1",
		"cuh#2:1", "cuh#2:1-2", "cuh#2/1:", "cuh#2/1:0", "cuh#2/01", "cuh#2/1 ", "cuh#2#1",
		"c7/1", "c7/1:2", "c7x", "x7", "7", "#", ":", ":1", "c:1", "cuh:1", "cuh/1:1",
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i)
		parsed(bad[i], -1, __LINE__);
	/* At most 16 ranges. */
	parsed("c1:1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16", EVIDENCE_REF_LINES, __LINE__);
	parsed("c1:1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17", -1, __LINE__);
	struct evidence_ref ref;
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c12:120-140,150")));
	CHECK(ref.call == 12 && ref.n_ranges == 2 && ref.ranges[0].first == 120 &&
	      ref.ranges[0].last == 140 && ref.ranges[1].first == 150 && ref.ranges[1].last == 150 &&
	      !*ref.id.author && !ref.id.number && !ref.id.item);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("bro#5/2:3-4")));
	CHECK(!strcmp(ref.id.author, "bro") && ref.id.number == 5 && ref.id.item == 2 &&
	      ref.n_ranges == 1 && !ref.call && ref.form == EVIDENCE_REF_ITEM_LINES);
	/* The ID of a call reference is empty, and the call of an item
	 * reference or a finding reference is 0. */
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c4294967295")) && ref.call == UINT32_MAX &&
	      !*ref.id.author && !ref.id.number && !ref.id.item && !ref.n_ranges);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1")) && !strcmp(ref.id.author, "cuh") &&
	      ref.id.number == 2 && ref.id.item == 1 && !ref.call && !ref.n_ranges &&
	      ref.form == EVIDENCE_REF_ITEM);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2")) && !strcmp(ref.id.author, "cuh") &&
	      ref.id.number == 2 && !ref.id.item && !ref.call && !ref.n_ranges &&
	      ref.form == EVIDENCE_REF_FINDING);
	/* A member name can start with c and digits. */
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c7x#3/4:5")) && !strcmp(ref.id.author, "c7x") &&
	      ref.id.number == 3 && ref.id.item == 4 && !ref.call && ref.n_ranges == 1 &&
	      ref.form == EVIDENCE_REF_ITEM_LINES);
	CHECK(evidence_ref_parse(nullptr, CSTR_VIEW("c1")) == EINVAL);
	CHECK(evidence_ref_parse(&ref, cstr_view(nullptr)) == EINVAL);
	/* The ranges ascend, and ranges that overlap or touch become one.
	 * Ranges with a line between them stay apart. */
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c7:150,130-145,5,120-140,5,146-149,140-141")));
	CHECK(ref.form == EVIDENCE_REF_LINES && ref.n_ranges == 2 &&
	      ref.ranges[0].first == 5 && ref.ranges[0].last == 5 &&
	      ref.ranges[1].first == 120 && ref.ranges[1].last == 150);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c7:141-150,120-140")) && ref.n_ranges == 1 &&
	      ref.ranges[0].first == 120 && ref.ranges[0].last == 150);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c7:9,5-7,1-3")) && ref.n_ranges == 3 &&
	      ref.ranges[0].first == 1 && ref.ranges[0].last == 3 &&
	      ref.ranges[1].first == 5 && ref.ranges[1].last == 7 &&
	      ref.ranges[2].first == 9 && ref.ranges[2].last == 9);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c1:1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16")) &&
	      ref.n_ranges == 1 && ref.ranges[0].first == 1 && ref.ranges[0].last == 16);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c1:4294967295,7,2-4294967295,9")) &&
	      ref.n_ranges == 1 && ref.ranges[0].first == 2 && ref.ranges[0].last == UINT32_MAX);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c1:1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1")) &&
	      ref.n_ranges == 1 && ref.ranges[0].first == 1 && ref.ranges[0].last == 1);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:9-12,1-20")) &&
	      ref.form == EVIDENCE_REF_ITEM_LINES && ref.n_ranges == 1 &&
	      ref.ranges[0].first == 1 && ref.ranges[0].last == 20);
}

/* Check that evidence_ref_put() writes the reference @p text as @p want. */
static void
put (char const *text,
     char const *want,
     int         line)
{
	struct evidence_ref ref;
	struct dstr d = DSTR_INIT;
	CHECK(dstr_cat(&d, "x "));
	bool const ok = !evidence_ref_parse(&ref, cstr_view(text)) && !evidence_ref_put(&ref, &d);
	struct cstr_view const got = cstr_view_dstr(&d);
	if (!ok || !cstr_view_starts(got, CSTR_VIEW("x ")) ||
	    !cstr_view_equal(cstr_view_slice(got, 2, SIZE_MAX), cstr_view(want))) {
		++failures;
		fprintf(stderr, "%s:%d: %s: got %s, want %s\n", __FILE__, line, text, dstr_get(&d),
		        want);
	}
	dstr_cleanup(&d);
}

/* evidence_ref_put() is the inverse of evidence_ref_parse(), with the
 * ranges in order, and evidence_ranges_put() writes ranges with any
 * separators.
 */
static void
test_put (void)
{
	put("c7", "c7", __LINE__);
	put("c4294967295", "c4294967295", __LINE__);
	put("c7:120-140,150", "c7:120-140,150", __LINE__);
	put("c7:150,120-140", "c7:120-140,150", __LINE__);
	put("c7:5-5", "c7:5", __LINE__);
	put("c7:3-6,4-9,1", "c7:1,3-9", __LINE__);
	put("c7:141-150,120-140", "c7:120-150", __LINE__);
	put("cuh#2/1", "cuh#2/1", __LINE__);
	put("first.last@ad#12/3:165,161-163", "first.last@ad#12/3:161-163,165", __LINE__);
	struct range const r[] = { { 1, 1 }, { 3, 9 }, { 11, 4294967295 } };
	struct dstr d = DSTR_INIT;
	CHECK(!evidence_ranges_put(r, 3, ", ", " to ", &d));
	SAME(dstr_get(&d), dstr_len(&d), "1, 3 to 9, 11 to 4294967295");
	(void)dstr_clear(&d);
	CHECK(!evidence_ranges_put(r, 0, ",", "-", &d) && !dstr_len(&d));
	CHECK(!evidence_ranges_put(r + 1, 1, ",", "-", &d));
	SAME(dstr_get(&d), dstr_len(&d), "3-9");
	dstr_cleanup(&d);
}

/* Check that citing @p text refuses with the reason @p want. */
static void
refused (struct evidence_cite const *cite,
         char const                 *text,
         bool                        strict,
         char const                 *want,
         int                         line)
{
	struct evidence_ref ref;
	struct evidence e = evidence();
	struct evidence_material m;
	struct dstr lines = DSTR_INIT;
	char why[512] = "x";
	int const r = evidence_ref_parse(&ref, cstr_view(text));
	int const got = r ? r : evidence_cite(&e, &m, &lines, &ref, cite, strict, why, sizeof why);
	if (got != EINVAL || strcmp(why, want)) {
		++failures;
		fprintf(stderr, "%s:%d: %s: error %d, refusal\n%s\nwant\n%s\n", __FILE__, line, text,
		        got, why, want);
	}
	evidence_fini(&e);
	dstr_cleanup(&lines);
}

/* Check that @p blob is the size and the SHA-256 of @p material. */
static void
hashed (struct evidence_blob const *blob,
        struct cstr_view            material,
        int                         line)
{
	unsigned char sha[SHA256_DIGEST_LENGTH];
	SHA256((unsigned char const *)(material.data ? material.data : ""), material.len, sha);
	if (blob->bytes == material.len && !memcmp(blob->sha256, sha, sizeof sha))
		return;
	++failures;
	fprintf(stderr, "%s:%d: blob of %zu bytes does not match\n", __FILE__, line, material.len);
}

/* evidence_cite() makes items from the calls of the ring and refuses the
 * calls that it cannot cite.
 */
static void
test_cite (void)
{
	/* Call c1, a git read of lines 1 to 9 whose window shows lines 2 to 6
	 * whole. */
	struct dstr out = DSTR_INIT;
	CHECK(dstr_printf(&out, "[h]\n"));
	size_t const at = dstr_len(&out);
	for (unsigned n = 1; n <= 9; ++n)
		CHECK(dstr_printf(&out, "%u\tline %u\n", n, n));
	struct seen_source const src = { CSTR_VIEW("proj"), CSTR_VIEW("src/a.c"),
	                                 { at, dstr_len(&out) - at }, 9, { 1, 9 }, { A } };
	struct cstr_view const o = cstr_view_dstr(&out);
	int e = -1;
	struct seen_read *read = seen_read_create(&src, o, &e);
	CHECK(read && !e);
	struct seen *seen = nullptr;
	struct cstr_view const window = cstr_view_n(o.data + at + 3, 9 * 6 - 3);
	struct buf request = buf();
	request_of(&request, "", "{\"command\":\"read\"}");
	struct cstr_view const parts[SEEN_PARTS] = { window, CSTR_VIEW("\n\n[askai: call c1]") };
	struct seen_result const r = { .parts = parts, .out = o, .read = read, .skip = at + 3,
	                               .stamp = { .output = o.len, .number = 1, .turn = 1,
	                                          .tool = 1 } };
	CHECK(!seen_keep(&seen, &request, &r));
	buf_fini(&request);
	CHECK(seen->calls[0].lines.range.first == 2 && seen->calls[0].lines.range.last == 6);
	keep(&seen, 2, 1, 2, "toolu_x", "{\"page\":\"git\"}", CSTR_VIEW("A man page.\n"), nullptr,
	     false);
	/* Call c3 is a findings call, which the ring does not store. */
	keep(&seen, 4, 2, 1, "", "{\"command\":\"grep\"}", CSTR_VIEW("a:1:x\n"), nullptr, false);
	keep(&seen, 5, 2, 2, "", "{}", CSTR_VIEW("big"), nullptr, true);
	keep(&seen, 6, 3, 2, "", "{}", CSTR_VIEW("now"), nullptr, false);
	struct hexid ask;
	CHECK(!hexid_make(&ask));
	struct evidence_cite const cite = { seen, &ask, tool_name, nullptr, 8, 3 };
	struct evidence_ref ref;
	struct evidence ev = evidence();
	struct evidence_material m;
	struct dstr lines = DSTR_INIT;
	char why[512] = "";

	/* c1 is all the lines that the window showed whole. */
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c1")));
	CHECK(!evidence_cite(&ev, &m, &lines, &ref, &cite, true, why, sizeof why) && !*why);
	CHECK(ev.kind == EVIDENCE_LINES && ev.call == 1 && ev.turn == 1 && !ev.item &&
	      ev.ranges.n == 1 && ev.ranges.items[0].first == 2 && ev.ranges.items[0].last == 6 &&
	      !ev.flags && !strcmp(ev.ask.hex, ask.hex) && ev.output == o.len && ev.skip == at + 3 &&
	      !*ev.from.author && !ev.from.number && !ev.from.item && !m.request.len);
	SAME(m.result.data, m.result.len, "2\tline 2\n3\tline 3\n4\tline 4\n5\tline 5\n6\tline 6\n");
	hashed(&ev.result, m.result, __LINE__);
	SAME(dstr_get(&ev.tool), dstr_len(&ev.tool), "git");
	SAME(dstr_get(&ev.call_id), dstr_len(&ev.call_id), "call_1_0");
	SAME(dstr_get(&ev.ids), dstr_len(&ev.ids),
	     "{\"repo\":\"proj\",\"commit\":\"" A "\",\"path\":\"src/a.c\",\"total\":9}");
	/* Some lines, and two ranges. */
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c1:3-4,6")));
	CHECK(!evidence_cite(&ev, &m, &lines, &ref, &cite, true, why, sizeof why));
	CHECK(ev.kind == EVIDENCE_LINES && ev.ranges.n == 2 && ev.ranges.items[1].first == 6);
	SAME(m.result.data, m.result.len, "3\tline 3\n4\tline 4\n6\tline 6\n");
	hashed(&ev.result, m.result, __LINE__);
	/* evidence_cite() copies each line once, in ascending order, for ranges
	 * in any order and for ranges that overlap or touch. The lines of the
	 * reference below are all lines that the window showed whole, from the
	 * first to the last. */
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c1:6,4-5,2,3-4")));
	CHECK(!evidence_cite(&ev, &m, &lines, &ref, &cite, true, why, sizeof why));
	CHECK(ev.kind == EVIDENCE_LINES && ev.ranges.n == 1 && ev.ranges.items[0].first == 2 &&
	      ev.ranges.items[0].last == 6);
	SAME(m.result.data, m.result.len,
	     "2\tline 2\n3\tline 3\n4\tline 4\n5\tline 5\n6\tline 6\n");
	hashed(&ev.result, m.result, __LINE__);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c1:6,3-4,2-3")));
	CHECK(!evidence_cite(&ev, &m, &lines, &ref, &cite, true, why, sizeof why));
	CHECK(ev.kind == EVIDENCE_LINES && ev.ranges.n == 2 && ev.ranges.items[0].first == 2 &&
	      ev.ranges.items[0].last == 4 && ev.ranges.items[1].first == 6 &&
	      ev.ranges.items[1].last == 6);
	SAME(m.result.data, m.result.len, "2\tline 2\n3\tline 3\n4\tline 4\n6\tline 6\n");
	hashed(&ev.result, m.result, __LINE__);
	/* A call of another tool, which failed, with an ID from the provider. */
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c2")));
	CHECK(!evidence_cite(&ev, &m, &lines, &ref, &cite, true, why, sizeof why));
	CHECK(ev.kind == EVIDENCE_CALL && ev.flags == FLAG_FAILED && ev.call == 2 &&
	      !dstr_len(&ev.ids));
	SAME(dstr_get(&ev.call_id), dstr_len(&ev.call_id), "toolu_x");
	SAME(dstr_get(&ev.tool), dstr_len(&ev.tool), "man");
	SAME(m.result.data, m.result.len, "A man page.\n\n\n[askai: call c2]");
	/* A git grep shows no lines, so c4 is a call item. */
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c4")));
	CHECK(!evidence_cite(&ev, &m, &lines, &ref, &cite, true, why, sizeof why));
	CHECK(ev.kind == EVIDENCE_CALL && !ev.flags && ev.call == 4 && ev.turn == 2 &&
	      !ev.ranges.n);
	SAME(m.request.data, m.request.len, "{\"command\":\"grep\"}");
	SAME(m.result.data, m.result.len, "a:1:x\n\n\n[askai: call c4]");
	hashed(&ev.request, m.request, __LINE__);
	hashed(&ev.result, m.result, __LINE__);
	/* A call of this turn is evidence only for a candidate. */
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c6")));
	CHECK(!evidence_cite(&ev, &m, &lines, &ref, &cite, false, why, sizeof why));
	CHECK(ev.kind == EVIDENCE_CALL && ev.turn == 3);
	refused(&cite, "c6", true, "Call c6 was made in this turn. Cite c6 for verified or "
	        "disproved in a later turn.", __LINE__);

	/* Refusals. */
	refused(&cite, "c1:1-3", true, "Call c1 showed lines 2 to 6 of src/a.c whole. Give lines "
	        "within that range.", __LINE__);
	refused(&cite, "c1:2-6,7", true, "Call c1 showed lines 2 to 6 of src/a.c whole. Give lines "
	        "within that range.", __LINE__);
	refused(&cite, "c1:7,2", true, "Call c1 showed lines 2 to 6 of src/a.c whole. Give lines "
	        "within that range.", __LINE__);
	refused(&cite, "c2:1-2,4", true, "c2:1-2,4 needs a git read or a code describe. Call c2 is "
	        "a man call.", __LINE__);
	refused(&cite, "c2:4,1-2,2", true, "c2:1-2,4 needs a git read or a code describe. Call c2 "
	        "is a man call.", __LINE__);
	refused(&cite, "c4:5", true, "c4:5 needs a git read or a code describe. Call c4 is a git "
	        "grep.", __LINE__);
	refused(&cite, "c3", false, "Call c3 is a findings or compact call, or did not run. Cite "
	        "another call.", __LINE__);
	refused(&cite, "c5", false, "Call c5 was too large to keep. askai keeps no call of more "
	        "than 4 MiB.", __LINE__);
	refused(&cite, "c7", false, "Call c7 is a findings or compact call, or did not run. Cite "
	        "another call.", __LINE__);
	refused(&cite, "c8", false, "c8 is a findings call. findings calls cannot be evidence.",
	        __LINE__);
	refused(&cite, "c9", false, "No call c9 was made before this call.", __LINE__);
	refused(&cite, "cuh#2", false, "cuh#2 is a finding. evidence takes an evidence item such "
	        "as cuh#2/1, or a call such as c7. expand with id cuh#2 lists the items.", __LINE__);
	/* evidence_recite() cites an item again. */
	refused(&cite, "cuh#2/1", false, "", __LINE__);
	refused(&cite, "cuh#2/1:3", false, "", __LINE__);

	/* Calls that left the ring. The first new calls are calls without
	 * lines. The refusal of lines of such a call names the command of a
	 * git call, which is 1 to 32 lowercase letters and hyphens, and else
	 * the tool. */
	static struct {
		uint32_t    tool;
		char const *args;
		char const *what;
	} const unlined[] = {
		{ 1, "{}",                                                  "a git call" },
		{ 1, "{\"command\":\"\"}",                                  "a git call" },
		{ 1, "{\"command\":\"LOG\"}",                               "a git call" },
		{ 1, "{\"command\":7}",                                     "a git call" },
		{ 1, "{\"command\":\"cherry-pick\"}",                       "a git cherry-pick" },
		{ 1, "{\"command\":\"abcdefghijklmnopqrstuvwxyz-abcde\"}",
		  "a git abcdefghijklmnopqrstuvwxyz-abcde" },
		{ 1, "{\"command\":\"abcdefghijklmnopqrstuvwxyz-abcdef\"}", "a git call" },
		{ 1, "not JSON",                                            "a git call" },
		{ 2, "{\"command\":\"grep\"}",                              "a man call" },
	};
	uint32_t number = 7;
	for (size_t i = 0; i < array_size(unlined); ++i, ++number)
		keep(&seen, number, 3, unlined[i].tool, "", unlined[i].args, CSTR_VIEW("k"), nullptr,
		     false);
	for (; number < 7 + SEEN_CALLS; ++number)
		keep(&seen, number, 3, 2, "", "{}", CSTR_VIEW("k"), nullptr, false);
	struct evidence_cite later = cite;
	later.call = 7 + SEEN_CALLS;
	later.turn = 4;
	for (size_t i = 0; i < array_size(unlined); ++i) {
		char text[32];
		char want[160];
		CHECK(snprintf(text, sizeof text, "c%zu:1", 7 + i) > 0);
		CHECK(snprintf(want, sizeof want, "c%zu:1 needs a git read or a code describe. Call "
		               "c%zu is %s.", 7 + i, 7 + i, unlined[i].what) > 0);
		refused(&later, text, true, want, __LINE__);
	}
	/* The refusal is cut at the size of its buffer. */
	char cut[48] = "";
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c11:1")));
	CHECK(evidence_cite(&ev, &m, &lines, &ref, &later, true, cut, sizeof cut) == EINVAL);
	SAME(cut, strlen(cut), "c11:1 needs a git read or a code describe. C...");
	refused(&later, "c2", true, "c2 is no longer kept. askai keeps the last 64 calls of an ask "
	        "call. The oldest kept call is c7.", __LINE__);
	/* The findings call c3 is older than the oldest kept call. */
	refused(&later, "c3", true, "c3 is no longer kept. askai keeps the last 64 calls of an ask "
	        "call. The oldest kept call is c7.", __LINE__);
	seen_destroy(&seen);

	/* Calls before a restart. */
	struct json_object *calls = json_object_new_int(10);
	CHECK(!seen_resume(&seen, calls, nullptr));
	json_object_put(calls);
	later.seen = seen;
	later.call = 12;
	refused(&later, "c5", true, "c5 is not kept, because askai restarted after c5. askai keeps "
	        "no call of this ask call yet.", __LINE__);
	keep(&seen, 11, 3, 2, "", "{}", CSTR_VIEW("k"), nullptr, false);
	later.seen = seen;
	refused(&later, "c10", true, "c10 is not kept, because askai restarted after c10. The "
	        "oldest kept call is c11.", __LINE__);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("c11")));
	CHECK(!evidence_cite(&ev, &m, &lines, &ref, &later, true, why, sizeof why));
	/* No ring at all. */
	later.seen = nullptr;
	refused(&later, "c11", true, "Call c11 is a findings or compact call, or did not run. Cite "
	        "another call.", __LINE__);

	/* Null arguments. */
	CHECK(evidence_cite(nullptr, &m, &lines, &ref, &cite, true, why, sizeof why) == EINVAL);
	CHECK(evidence_cite(&ev, &m, &lines, &ref, &cite, true, nullptr, 0) == EINVAL);
	struct evidence_cite nameless = cite;
	nameless.name = nullptr;
	CHECK(evidence_cite(&ev, &m, &lines, &ref, &nameless, true, why, sizeof why) == EINVAL);

	evidence_fini(&ev);
	dstr_cleanup(&lines);
	seen_destroy(&seen);
	seen_read_destroy(&read);
	dstr_cleanup(&out);
}

/* Check that evidence_external() accepts @p text, or refuses it with
 * @p want.
 */
static void
external (char const *text,
          char const *want,
          int         line)
{
	char why[256] = "";
	int const e = evidence_external(cstr_view(text), 3, why, sizeof why);
	if (want ? e == EINVAL && !strcmp(why, want) : !e)
		return;
	++failures;
	fprintf(stderr, "%s:%d: %s: error %d, %s\n", __FILE__, line, text, e, why);
}

/* evidence_external() needs an exact hash in a short text on one line. */
static void
test_external (void)
{
	char const *const nohash = "external[3] needs an exact hash of the material: 40 or 64 "
	                           "hexadecimal digits, such as sha256:9b1c...";
	char const *const bad = "external[3] must be 1 byte to 1 KiB of text on one line.";
	external("libclang-19-dev 1:19.1.7-3 amd64 sha256:"
	         "9b1c0e1d2c3b4a5968778695a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5", nullptr,
	         __LINE__);
	external(A, nullptr, __LINE__);
	external("commit " A " of upstream", nullptr, __LINE__);
	external("SHA1 55ACA1E8682351E4C39A92B040721DEF3F005402", nullptr, __LINE__);
	external("no hash here", nohash, __LINE__);
	external(A "0", nohash, __LINE__);
	external("x" A "y", nullptr, __LINE__);
	external("abc", nohash, __LINE__);
	external("", bad, __LINE__);
	external("line\n" A, bad, __LINE__);
	char big[EVIDENCE_TEXT + 2];
	memset(big, 'a', sizeof big - 1);
	big[sizeof big - 1] = '\0';
	external(big, bad, __LINE__);
	big[EVIDENCE_TEXT] = '\0';
	memcpy(big, A, sizeof A - 1);
	big[sizeof A - 1] = ' ';
	external(big, nullptr, __LINE__);
	external("\xff " A, bad, __LINE__);
}

/* evidence_path() names the archive file of material by its SHA-256. */
static void
test_path (void)
{
	unsigned char sha[EVIDENCE_SHA256];
	for (size_t i = 0; i < sizeof sha; ++i)
		sha[i] = (unsigned char)(i * 9 + 1);
	char path[EVIDENCE_PATH];
	evidence_path(sha, path);
	SAME(path, strlen(path), "archive/01/0a131c252e37404952"
	     "5b646d767f88919aa3acb5bec7d0d9e2ebf4fd060f18");
	CHECK(strlen(path) == EVIDENCE_PATH - 1);
}

/* evidence_json() and evidence_parse() write and read an item, and
 * evidence_parse() refuses what is not one.
 */
static void
test_json (void)
{
	struct evidence e = evidence();
	e.kind = EVIDENCE_LINES;
	e.item = 2;
	e.call = 12;
	e.turn = 3;
	e.output = 900;
	e.skip = 100;
	e.flags = FLAG_FAILED;
	CHECK(!hexid_init(&e.ask, CSTR_VIEW("9f3c2a71e0b14c55a1d2e3f405162738")));
	CHECK(!finding_id_parse(&e.from, CSTR_VIEW("cuh#2/2")));
	CHECK(!cstr_view_store(CSTR_VIEW("git"), &e.tool));
	CHECK(!cstr_view_store(CSTR_VIEW("call_3_1"), &e.call_id));
	CHECK(!cstr_view_store(CSTR_VIEW("{\"repo\":\"askai\",\"path\":\"src/git.c\",\"commit\":\""
	                                 A "\"}"), &e.ids));
	CHECK(!util_grow(e.ranges.items, e.ranges.cap, 2));
	e.ranges.items[e.ranges.n++] = range(161, 171);
	e.ranges.items[e.ranges.n++] = range(180, 180);
	e.result.bytes = 702;
	memset(e.result.sha256, 0xab, sizeof e.result.sha256);
	int err = -1;
	struct json_object *obj = evidence_json(&e, &err);
	CHECK(obj && !err);
	struct cstr_view const wire = jb_wire(obj);
	SAME(wire.data, wire.len, "{\"item\":2,\"kind\":\"lines\",\"tool\":\"git\",\"call\":12,"
	     "\"turn\":3,\"call_id\":\"call_3_1\",\"failed\":true,\"output\":900,\"skip\":100,"
	     "\"ask\":\"9f3c2a71e0b14c55a1d2e3f405162738\",\"from\":\"cuh#2/2\",\"ids\":{\"repo\":"
	     "\"askai\",\"path\":\"src/git.c\",\"commit\":\"" A "\"},\"lines\":[[161,171],"
	     "[180,180]],\"result\":"
	     "{\"sha256\":\"abababababababababababababababababababababababababababababababab\","
	     "\"bytes\":702}}");
	struct evidence back = evidence();
	CHECK(!evidence_parse(&back, obj));
	CHECK(back.kind == EVIDENCE_LINES && back.item == 2 && back.call == 12 && back.turn == 3 &&
	      back.output == 900 && back.skip == 100 && back.flags == FLAG_FAILED &&
	      !strcmp(back.ask.hex, e.ask.hex) && finding_id_equal(&back.from, &e.from) &&
	      !strcmp(back.from.author, "cuh") && back.from.number == 2 && back.from.item == 2 &&
	      back.ranges.n == 2 && back.ranges.items[1].first == 180 &&
	      back.result.bytes == 702 && !memcmp(back.result.sha256, e.result.sha256, 32));
	SAME(dstr_get(&back.ids), dstr_len(&back.ids), "{\"repo\":\"askai\",\"path\":\"src/git.c\","
	     "\"commit\":\"" A "\"}");
	struct dstr text = DSTR_INIT;
	CHECK(!evidence_summary(&back, &text));
	SAME(dstr_get(&text), dstr_len(&text), "lines 161 to 171, 180 of src/git.c in repository "
	     "askai at commit 55aca1e86823");
	json_object_put(obj);

	/* A call with its arguments, without ask and from. */
	evidence_fini(&back);
	evidence_fini(&e);
	e.kind = EVIDENCE_CALL;
	e.item = 1;
	e.call = 4;
	e.request.bytes = 98;
	e.result.bytes = 1;
	CHECK(!cstr_view_store(CSTR_VIEW("man"), &e.tool));
	CHECK(!cstr_view_store(CSTR_VIEW("toolu_1"), &e.call_id));
	obj = evidence_json(&e, &err);
	CHECK(obj && !err && !jb_get(obj, "ask") && !jb_get(obj, "from") && !jb_get(obj, "ids") &&
	      !jb_get(obj, "lines") && jb_get(obj, "request"));
	CHECK(!evidence_parse(&back, obj));
	CHECK(back.kind == EVIDENCE_CALL && !back.flags &&
	      back.request.bytes == 98 && !*back.ask.hex && !*back.from.author &&
	      !back.from.number && !back.from.item && !dstr_len(&back.ids));
	(void)dstr_clear(&text);
	CHECK(!evidence_summary(&back, &text));
	SAME(dstr_get(&text), dstr_len(&text), "call c4, man, 1 byte");
	/* The summary of a call names no identifiers. */
	back.flags |= FLAG_FAILED;
	CHECK(!cstr_view_store(CSTR_VIEW("{\"repo\":\"askai\",\"commit\":\"" A "\"}"), &back.ids));
	(void)dstr_clear(&text);
	CHECK(!evidence_summary(&back, &text));
	SAME(dstr_get(&text), dstr_len(&text), "call c4, man, failed, 1 byte");
	back.kind = EVIDENCE_LINES;
	CHECK(!util_grow(back.ranges.items, back.ranges.cap, 1));
	back.ranges.items[back.ranges.n++] = range(7, 7);
	(void)dstr_clear(&back.ids);
	(void)dstr_clear(&text);
	CHECK(!evidence_summary(&back, &text));
	SAME(dstr_get(&text), dstr_len(&text), "line 7 of the file");
	dstr_cleanup(&text);

	/* Members that are missing or not valid. A call item without the
	 * member request is refused. */
	char const *const drops[] = { "kind", "tool", "call_id", "failed", "item", "call", "turn",
	                              "output", "skip", "request", "result" };
	for (size_t i = 0; i < sizeof drops / sizeof *drops; ++i) {
		struct json_object *copy = nullptr;
		CHECK(!json_object_deep_copy(obj, &copy, nullptr));
		json_object_object_del(copy, drops[i]);
		CHECK(evidence_parse(&back, copy) == EINVAL && !back.item);
		json_object_put(copy);
	}
	struct {
		char const *key;
		char const *json;
	} const bad[] = {
		{ "kind",    "\"blob\"" },
		{ "kind",    "\"message\"" },
		{ "kind",    "\"files\"" },
		{ "kind",    "\"sources\"" },
		{ "item",    "0" },
		{ "item",    "-1" },
		{ "call",    "\"4\"" },
		{ "ask",     "\"9f3c\"" },
		{ "from",    "\"\"" },
		{ "from",    "7" },
		{ "from",    "\"x\"" },
		{ "from",    "\"cuh#2\"" },
		{ "from",    "\"cuh#2/0\"" },
		{ "from",    "\"cuh#2/1:3\"" },
		{ "from",    "\"cuh#2/1 \"" },
		{ "from",    "\"c7\"" },
		{ "from",    "\"c7#2/1\"" },
		{ "ids",     "[]" },
		{ "request", "{\"sha256\":\"ab\",\"bytes\":1}" },
		{ "result",  "{\"sha256\":\"abababababababababababababababababababababababababababababababab\"}" },
		{ "result",  "{\"sha256\":\"zbababababababababababababababababababababababababababababababab\",\"bytes\":1}" },
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
		struct json_object *copy = nullptr;
		CHECK(!json_object_deep_copy(obj, &copy, nullptr));
		CHECK(!json_object_object_add(copy, bad[i].key, json_tokener_parse(bad[i].json)));
		if (evidence_parse(&back, copy) != EINVAL || *back.from.author || back.from.number ||
		    back.from.item) {
			++failures;
			fprintf(stderr, "%s:%d: %s %s parsed\n", __FILE__, __LINE__, bad[i].key,
			        bad[i].json);
		}
		json_object_put(copy);
	}
	/* A lines item needs its lines. */
	char const *const lines[] = { "[]", "[[0,1]]", "[[2,1]]", "[[1]]", "7", "[[1,2],3]" };
	for (size_t i = 0; i < sizeof lines / sizeof *lines; ++i) {
		struct json_object *copy = nullptr;
		CHECK(!json_object_deep_copy(obj, &copy, nullptr));
		json_object_object_del(copy, "request");
		CHECK(!json_object_object_add(copy, "kind", json_object_new_string("lines")));
		CHECK(!json_object_object_add(copy, "lines", json_tokener_parse(lines[i])));
		CHECK(evidence_parse(&back, copy) == EINVAL);
		json_object_put(copy);
	}
	/* A lines item with the member request is refused. */
	struct json_object *meet = nullptr;
	CHECK(!json_object_deep_copy(obj, &meet, nullptr));
	CHECK(!json_object_object_add(meet, "kind", json_object_new_string("lines")));
	CHECK(!json_object_object_add(meet, "lines", json_tokener_parse("[[1,2],[3,4],[6,6]]")));
	CHECK(evidence_parse(&back, meet) == EINVAL && back.kind == EVIDENCE_CALL &&
	      !back.ranges.n && !back.request.bytes);
	json_object_object_del(meet, "request");
	CHECK(!evidence_parse(&back, meet) && back.kind == EVIDENCE_LINES && back.ranges.n == 3 &&
	      !back.request.bytes);
	json_object_put(meet);
	/* An earlier askai stored the ranges in the order of the reference, and
	 * overlapping. Such an item stays readable, in the stored order. */
	static struct {
		char const   *json;
		struct range  second;
	} const older[] = {
		{ "[[5,6],[1,2]]", { 1, 2 } },
		{ "[[1,3],[3,4]]", { 3, 4 } },
		{ "[[1,3],[2,2]]", { 2, 2 } },
	};
	for (size_t i = 0; i < sizeof older / sizeof *older; ++i) {
		struct json_object *copy = nullptr;
		CHECK(!json_object_deep_copy(obj, &copy, nullptr));
		json_object_object_del(copy, "request");
		CHECK(!json_object_object_add(copy, "kind", json_object_new_string("lines")));
		CHECK(!json_object_object_add(copy, "lines", json_tokener_parse(older[i].json)));
		CHECK(!evidence_parse(&back, copy) && back.ranges.n == 2 &&
		      back.ranges.items[1].first == older[i].second.first &&
		      back.ranges.items[1].last == older[i].second.last);
		json_object_put(copy);
	}
	CHECK(evidence_parse(&back, nullptr) == EINVAL);
	CHECK(evidence_parse(nullptr, obj) == EINVAL);
	CHECK(!evidence_json(nullptr, &err) && err == EFAULT);
	json_object_put(obj);
	evidence_fini(&back);
	evidence_fini(&e);
}

/* Stores the size and the SHA-256 of @p material in @p blob. */
static void
hash_of (struct evidence_blob *blob,
         struct cstr_view      material)
{
	SHA256((unsigned char const *)(material.data ? material.data : ""), material.len,
	       blob->sha256);
	blob->bytes = material.len;
}

/* Returns the JSON object text of @p e, which the caller frees. */
static char *
record_of (struct evidence const *e)
{
	int err = -1;
	struct json_object *obj = evidence_json(e, &err);
	CHECK(obj && !err);
	char *text = strdup(json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN));
	json_object_put(obj);
	return text;
}

/* Check that citing the item @p text again from @p record and
 * @p material refuses with @p want.
 */
static void
recite_refused (char const       *text,
                char const       *record,
                struct cstr_view  material,
                char const       *want,
                int               line)
{
	struct evidence_ref ref;
	struct evidence e = evidence();
	struct dstr lines = DSTR_INIT;
	char why[512] = "x";
	int const r = evidence_ref_parse(&ref, cstr_view(text));
	int const got = r ? r : evidence_recite(&e, &lines, &ref, cstr_view(record), material, why,
	                                        sizeof why);
	if (got != EINVAL || strcmp(why, want)) {
		++failures;
		fprintf(stderr, "%s:%d: %s: error %d, refusal\n%s\nwant\n%s\n", __FILE__, line, text,
		        got, why, want);
	}
	evidence_fini(&e);
	dstr_cleanup(&lines);
}

/* evidence_recite() cites an item again, whole or some of its lines. */
static void
test_recite (void)
{
	/* cuh#2/1 is lines 2 to 6 and 8 to 9, the last without a line feed. */
	static char const stored[] = "2\tline 2\n3\tline 3\n4\tline 4\n5\tline 5\n6\tline 6\n"
	                             "8\tline 8\n9\tline 9";
	struct cstr_view const material = cstr_view_n(stored, sizeof stored - 1);
	struct evidence item = evidence();
	item.kind = EVIDENCE_LINES;
	item.item = 1;
	item.call = 3;
	item.turn = 2;
	item.output = 400;
	CHECK(!hexid_init(&item.ask, CSTR_VIEW("68177013a5c94f0e8b1d2c3e4f506172")));
	CHECK(!cstr_view_store(CSTR_VIEW("git"), &item.tool));
	CHECK(!cstr_view_store(CSTR_VIEW("call_2_0"), &item.call_id));
	CHECK(!cstr_view_store(CSTR_VIEW("{\"repo\":\"proj\",\"commit\":\"" A "\",\"path\":"
	                                 "\"src/a.c\",\"total\":9}"), &item.ids));
	CHECK(!util_grow(item.ranges.items, item.ranges.cap, 2));
	item.ranges.items[item.ranges.n++] = range(2, 6);
	item.ranges.items[item.ranges.n++] = range(8, 9);
	hash_of(&item.result, material);
	char *record = record_of(&item);

	/* Whole: the same item, without its number, from cuh#2/1. */
	struct evidence_ref ref;
	struct evidence e = evidence();
	struct dstr lines = DSTR_INIT;
	char why[512] = "x";
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(record), cstr_view(nullptr), why,
	                       sizeof why) && !*why);
	CHECK(e.kind == EVIDENCE_LINES && !e.item && e.call == 3 && e.turn == 2 && e.output == 400 &&
	      finding_id_equal(&e.from, &ref.id) && !strcmp(e.from.author, "cuh") &&
	      e.from.number == 2 && e.from.item == 1 && !strcmp(e.ask.hex, item.ask.hex) &&
	      e.ranges.n == 2 &&
	      e.ranges.items[1].last == 9 && !memcmp(&e.result, &item.result, sizeof e.result) &&
	      !dstr_len(&lines) && !e.flags);
	SAME(dstr_get(&e.ids), dstr_len(&e.ids), dstr_get(&item.ids));
	/* Some lines, from both ranges and up to the last line. */
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:3-4,9")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(record), material, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "3\tline 3\n4\tline 4\n9\tline 9");
	hashed(&e.result, cstr_view_dstr(&lines), __LINE__);
	CHECK(e.kind == EVIDENCE_LINES && e.ranges.n == 2 && e.ranges.items[0].first == 3 &&
	      e.ranges.items[0].last == 4 && e.ranges.items[1].first == 9 &&
	      finding_id_equal(&e.from, &ref.id) && e.from.item == 1 && !e.item && !e.flags);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:2,6-6,8")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(record), material, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "2\tline 2\n6\tline 6\n8\tline 8\n");
	/* Ranges in any order, and ranges that overlap or touch, in one pass. */
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:9,5-6,3-5,8,2")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(record), material, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "2\tline 2\n3\tline 3\n4\tline 4\n5\tline 5\n"
	     "6\tline 6\n8\tline 8\n9\tline 9");
	CHECK(e.ranges.n == 2 && e.ranges.items[0].first == 2 && e.ranges.items[0].last == 6 &&
	      e.ranges.items[1].first == 8 && e.ranges.items[1].last == 9);
	hashed(&e.result, cstr_view_dstr(&lines), __LINE__);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:9,5-6,4-5,2")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(record), material, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "2\tline 2\n4\tline 4\n5\tline 5\n6\tline 6\n"
	     "9\tline 9");
	CHECK(e.ranges.n == 3 && e.ranges.items[0].first == 2 && e.ranges.items[0].last == 2 &&
	      e.ranges.items[1].first == 4 && e.ranges.items[1].last == 6 &&
	      e.ranges.items[2].first == 9 && e.ranges.items[2].last == 9);
	hashed(&e.result, cstr_view_dstr(&lines), __LINE__);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:8-9,2-6")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(record), material, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), stored);

	/* Refusals. */
	recite_refused("cuh#2/1:6-8", record, material, "cuh#2/1 shows lines 2 to 6, 8 to 9 of "
	               "src/a.c. Give lines within those ranges.", __LINE__);
	recite_refused("cuh#2/1:1", record, material, "cuh#2/1 shows lines 2 to 6, 8 to 9 of "
	               "src/a.c. Give lines within those ranges.", __LINE__);
	recite_refused("cuh#2/1:9-10", record, material, "cuh#2/1 shows lines 2 to 6, 8 to 9 of "
	               "src/a.c. Give lines within those ranges.", __LINE__);
	recite_refused("cuh#2/1:8,7", record, material, "cuh#2/1 shows lines 2 to 6, 8 to 9 of "
	               "src/a.c. Give lines within those ranges.", __LINE__);
	/* Stored lines that end before the ranges of the item. */
	struct evidence shorter = item;
	shorter.ids = dstr(nullptr);
	shorter.tool = dstr(nullptr);
	shorter.call_id = dstr(nullptr);
	CHECK(!cstr_view_store(cstr_view_dstr(&item.ids), &shorter.ids));
	shorter.ranges.items = nullptr;
	shorter.ranges.n = shorter.ranges.cap = 0;
	CHECK(!util_grow(shorter.ranges.items, shorter.ranges.cap, 2));
	shorter.ranges.items[shorter.ranges.n++] = range(2, 6);
	shorter.ranges.items[shorter.ranges.n++] = range(8, 12);
	hash_of(&shorter.result, material);
	char *cut = record_of(&shorter);
	recite_refused("cuh#2/1:10", cut, material, "cuh#2/1 shows lines 2 to 6, 8 to 12 of src/a.c. "
	               "Give lines within those ranges.", __LINE__);
	free(cut);
	evidence_fini(&shorter);
	/* The ranges of an item that an earlier askai stored can be out of
	 * order. The material is the lines in the stored order. Each line of a
	 * reference comes from the stored range that contains the line, and a
	 * reference with a line that no stored range contains is refused. */
	static char const unsorted[] = "8\tline 8\n9\tline 9\n2\tline 2\n3\tline 3\n";
	struct cstr_view const backwards = cstr_view_n(unsorted, sizeof unsorted - 1);
	struct evidence older = evidence();
	older.kind = EVIDENCE_LINES;
	older.item = 1;
	CHECK(!cstr_view_store(cstr_view_dstr(&item.ids), &older.ids));
	CHECK(!cstr_view_store(CSTR_VIEW("git"), &older.tool));
	CHECK(!cstr_view_store(CSTR_VIEW("call_2_0"), &older.call_id));
	CHECK(!util_grow(older.ranges.items, older.ranges.cap, 2));
	older.ranges.items[older.ranges.n++] = range(8, 9);
	older.ranges.items[older.ranges.n++] = range(2, 3);
	hash_of(&older.result, backwards);
	char *old = record_of(&older);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:9")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), backwards, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "9\tline 9\n");
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:3")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), backwards, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "3\tline 3\n");
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:9,2-3")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), backwards, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "2\tline 2\n3\tline 3\n9\tline 9\n");
	CHECK(e.ranges.n == 2 && e.ranges.items[0].first == 2 && e.ranges.items[0].last == 3 &&
	      e.ranges.items[1].first == 9 && e.ranges.items[1].last == 9);
	hashed(&e.result, cstr_view_dstr(&lines), __LINE__);
	recite_refused("cuh#2/1:5", old, backwards, "cuh#2/1 shows lines 8 to 9, 2 to 3 of src/a.c. "
	               "Give lines within those ranges.", __LINE__);
	recite_refused("cuh#2/1:3-8", old, backwards, "cuh#2/1 shows lines 8 to 9, 2 to 3 of "
	               "src/a.c. Give lines within those ranges.", __LINE__);
	free(old);
	/* Stored ranges that are out of order can touch, such as 4-5 and 2-3.
	 * A reference to the lines 2 to 5 is one range, and the lines of that
	 * range are in both stored ranges. */
	static char const reversed[] = "4\tline 4\n5\tline 5\n2\tline 2\n3\tline 3\n";
	struct cstr_view const reverse = cstr_view_n(reversed, sizeof reversed - 1);
	older.ranges.items[0] = range(4, 5);
	older.ranges.items[1] = range(2, 3);
	hash_of(&older.result, reverse);
	old = record_of(&older);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:2-3,4-5")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), reverse, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "2\tline 2\n3\tline 3\n4\tline 4\n5\tline 5\n");
	CHECK(e.ranges.n == 1 && e.ranges.items[0].first == 2 && e.ranges.items[0].last == 5);
	hashed(&e.result, cstr_view_dstr(&lines), __LINE__);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:3,4")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), reverse, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "3\tline 3\n4\tline 4\n");
	CHECK(e.ranges.n == 1 && e.ranges.items[0].first == 3 && e.ranges.items[0].last == 4);
	hashed(&e.result, cstr_view_dstr(&lines), __LINE__);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:5,2")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), reverse, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "2\tline 2\n5\tline 5\n");
	recite_refused("cuh#2/1:4-6", old, reverse, "cuh#2/1 shows lines 4 to 5, 2 to 3 of src/a.c. "
	               "Give lines within those ranges.", __LINE__);
	recite_refused("cuh#2/1:1-3", old, reverse, "cuh#2/1 shows lines 4 to 5, 2 to 3 of src/a.c. "
	               "Give lines within those ranges.", __LINE__);
	free(old);
	/* The ranges of an item that an earlier askai stored can overlap. The
	 * material then contains line 3 twice. evidence_recite() copies each
	 * line of a reference once, from the first stored range that contains
	 * the line. */
	static char const twice[] = "1\tline 1\n2\tline 2\n3\tline 3\n3\tline 3\n4\tline 4\n";
	struct cstr_view const overlap = cstr_view_n(twice, sizeof twice - 1);
	older.ranges.items[0] = range(1, 3);
	older.ranges.items[1] = range(3, 4);
	hash_of(&older.result, overlap);
	old = record_of(&older);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:3-4")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), overlap, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "3\tline 3\n4\tline 4\n");
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:1-2,4")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), overlap, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "1\tline 1\n2\tline 2\n4\tline 4\n");
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:2-4")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), overlap, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "2\tline 2\n3\tline 3\n4\tline 4\n");
	CHECK(e.ranges.n == 1 && e.ranges.items[0].first == 2 && e.ranges.items[0].last == 4);
	hashed(&e.result, cstr_view_dstr(&lines), __LINE__);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:1-3,4")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), overlap, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "1\tline 1\n2\tline 2\n3\tline 3\n4\tline 4\n");
	CHECK(e.ranges.n == 1 && e.ranges.items[0].first == 1 && e.ranges.items[0].last == 4);
	recite_refused("cuh#2/1:3-5", old, overlap, "cuh#2/1 shows lines 1 to 3, 3 to 4 of src/a.c. "
	               "Give lines within those ranges.", __LINE__);
	free(old);
	/* The ranges of an item of an earlier askai can ascend and touch. The
	 * lines of the two ranges are next to each other in the material, so a
	 * range of the reference can span both ranges. */
	static char const joined[] = "2\tline 2\n3\tline 3\n4\tline 4\n5\tline 5\n";
	struct cstr_view const touch = cstr_view_n(joined, sizeof joined - 1);
	older.ranges.items[0] = range(2, 3);
	older.ranges.items[1] = range(4, 5);
	hash_of(&older.result, touch);
	old = record_of(&older);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:3-4")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), touch, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "3\tline 3\n4\tline 4\n");
	CHECK(e.ranges.n == 1 && e.ranges.items[0].first == 3 && e.ranges.items[0].last == 4);
	hashed(&e.result, cstr_view_dstr(&lines), __LINE__);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:4-5,2-3")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), touch, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), joined);
	CHECK(e.ranges.n == 1 && e.ranges.items[0].first == 2 && e.ranges.items[0].last == 5);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:5,2")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), touch, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "2\tline 2\n5\tline 5\n");
	recite_refused("cuh#2/1:5-6", old, touch, "cuh#2/1 shows lines 2 to 3, 4 to 5 of src/a.c. "
	               "Give lines within those ranges.", __LINE__);
	free(old);
	/* Each of the two ranges of the references below contains lines of two
	 * stored ranges. The stored ranges ascend, so evidence_recite() copies
	 * the lines in one pass over the material. */
	static char const pairs[] = "2\tline 2\n3\tline 3\n4\tline 4\n5\tline 5\n7\tline 7\n"
	                            "8\tline 8\n9\tline 9\n";
	struct cstr_view const four = cstr_view_n(pairs, sizeof pairs - 1);
	CHECK(!util_grow(older.ranges.items, older.ranges.cap, 4));
	older.ranges.items[2] = range(7, 8);
	older.ranges.items[3] = range(9, 9);
	older.ranges.n = 4;
	hash_of(&older.result, four);
	old = record_of(&older);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:8-9,3-4")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), four, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), "3\tline 3\n4\tline 4\n8\tline 8\n9\tline 9\n");
	CHECK(e.ranges.n == 2 && e.ranges.items[0].first == 3 && e.ranges.items[0].last == 4 &&
	      e.ranges.items[1].first == 8 && e.ranges.items[1].last == 9);
	hashed(&e.result, cstr_view_dstr(&lines), __LINE__);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1:7-9,2-5")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(old), four, why, sizeof why));
	SAME(dstr_get(&lines), dstr_len(&lines), pairs);
	recite_refused("cuh#2/1:5-7", old, four, "cuh#2/1 shows lines 2 to 3, 4 to 5, 7 to 8, 9 of "
	               "src/a.c. Give lines within those ranges.", __LINE__);
	free(old);
	evidence_fini(&older);
	recite_refused("cuh#2/1:3", record, CSTR_VIEW("2\tline 2\n"), "The stored material of "
	               "cuh#2/1 does not match the recorded SHA-256.", __LINE__);
	recite_refused("cuh#2/1", "{}", material, "Could not read finding cuh#2: unknown format.",
	               __LINE__);
	recite_refused("cuh#2/1:3", "not json", material, "Could not read finding cuh#2: unknown "
	               "format.", __LINE__);
	recite_refused("cuh#2", record, material, "", __LINE__);
	recite_refused("c7", record, material, "", __LINE__);
	/* One range: "that range". */
	item.ranges.n = 1;
	item.ranges.items[0] = range(2, 6);
	char *one = record_of(&item);
	recite_refused("cuh#2/1:7", one, material, "cuh#2/1 shows lines 2 to 6 of src/a.c. Give "
	               "lines within that range.", __LINE__);
	free(one);
	/* A call item has no lines. */
	item.kind = EVIDENCE_CALL;
	char *call = record_of(&item);
	recite_refused("cuh#2/1:3", call, material, "cuh#2/1:3 needs a lines item. cuh#2/1 is a "
	               "call item.", __LINE__);
	recite_refused("cuh#2/1:7,3-4", call, material, "cuh#2/1:3-4,7 needs a lines item. cuh#2/1 "
	               "is a call item.", __LINE__);
	recite_refused("cuh#2/1:5,3-4", call, material, "cuh#2/1:3-5 needs a lines item. cuh#2/1 is "
	               "a call item.", __LINE__);
	CHECK(!evidence_ref_parse(&ref, CSTR_VIEW("cuh#2/1")));
	CHECK(!evidence_recite(&e, &lines, &ref, cstr_view(call), cstr_view(nullptr), why,
	                       sizeof why));
	CHECK(e.kind == EVIDENCE_CALL && !e.flags && finding_id_equal(&e.from, &ref.id) &&
	      e.from.item == 1);
	free(call);
	CHECK(evidence_recite(nullptr, &lines, &ref, cstr_view(record), material, why,
	                      sizeof why) == EINVAL);
	CHECK(evidence_recite(&e, nullptr, &ref, cstr_view(record), material, why,
	                      sizeof why) == EINVAL);
	CHECK(evidence_recite(&e, &lines, nullptr, cstr_view(record), material, why,
	                      sizeof why) == EINVAL);
	CHECK(evidence_recite(&e, &lines, &ref, cstr_view(record), material, nullptr, 0) == EINVAL);
	free(record);
	evidence_fini(&e);
	evidence_fini(&item);
	dstr_cleanup(&lines);
}

/* Check that evidence_expand() prints @p e of cuh#2 by cuh with the
 * material @p request and @p result as @p want.
 */
static void
expanded (struct evidence const *e,
          struct cstr_view       request,
          struct cstr_view       result,
          char const            *want,
          int                    line)
{
	struct dstr d = DSTR_INIT;
	struct evidence_material const m = { request, result };
	struct finding_id const id = { .number = 2, .item = e->item, .author = "cuh" };
	int const got = evidence_expand(e, &id, &m, &d);
	if (got || strcmp(dstr_get(&d), want)) {
		++failures;
		fprintf(stderr, "%s:%d: error %d, got\n%s\nwant\n%s\n", __FILE__, line, got,
		        dstr_get(&d), want);
	}
	dstr_cleanup(&d);
}

/* evidence_expand() prints an item of each kind with its material, and
 * says when the material is missing or does not match.
 */
static void
test_expand (void)
{
	struct cstr_view const args = CSTR_VIEW("{\"command\":\"grep\"}");
	struct cstr_view const out = CSTR_VIEW("src/a.c\n\n[askai: call c9]");
	struct evidence e = evidence();
	e.kind = EVIDENCE_CALL;
	e.item = 1;
	e.call = 9;
	e.turn = 19;
	e.output = 9000;
	e.skip = 100;
	CHECK(!hexid_init(&e.ask, CSTR_VIEW("68177013a5c94f0e8b1d2c3e4f506172")));
	CHECK(!cstr_view_store(CSTR_VIEW("git"), &e.tool));
	CHECK(!cstr_view_store(CSTR_VIEW("{\"repo\":\"askai\",\"commit\":\"" A "\"}"), &e.ids));
	hash_of(&e.request, args);
	hash_of(&e.result, out);
	expanded(&e, args, out, "[askai: cuh#2/1: call c9 of cuh in ask 68177013, turn 19, tool git, "
	         "stored exactly as sent. Repository askai, commit " A ". Arguments:]\n"
	         "{\"command\":\"grep\"}\n[askai: cuh#2/1: result, 25 bytes, from byte 100 of an "
	         "output of 9000 bytes:]\nsrc/a.c\n\n[askai: call c9]\n", __LINE__);
	/* The whole output, no identifiers, a failed call after a restart. */
	e.skip = 0;
	e.output = 7;
	e.flags |= FLAG_FAILED;
	e.ask = (struct hexid){};
	(void)dstr_clear(&e.ids);
	expanded(&e, args, out, "[askai: cuh#2/1: call c9 of cuh, turn 19, tool git, failed, stored "
	         "exactly as sent. Arguments:]\n{\"command\":\"grep\"}\n[askai: cuh#2/1: result, "
	         "25 bytes:]\nsrc/a.c\n\n[askai: call c9]\n", __LINE__);
	/* Missing and changed material. */
	expanded(&e, args, cstr_view(nullptr), "[askai: cuh#2/1: call c9 of cuh, turn 19, tool "
	         "git, failed, stored exactly as sent. Arguments:]\n{\"command\":\"grep\"}\n"
	         "[askai: cuh#2/1: result, 25 bytes:]\n[askai: the stored material of cuh#2/1 is "
	         "missing.]\n", __LINE__);
	expanded(&e, CSTR_VIEW("{}"), out, "[askai: cuh#2/1: call c9 of cuh, turn 19, tool git, "
	         "failed, stored exactly as sent. Arguments:]\n{}\n[askai: cuh#2/1: result, 25 "
	         "bytes:]\nsrc/a.c\n\n[askai: call c9]\n[askai: the stored material of cuh#2/1 "
	         "does not match the recorded SHA-256.]\n", __LINE__);
	/* An item cited again names no author of the call. The identifiers
	 * name only a repository, and then only a commit. */
	e.item = 3;
	e.flags = 0;
	CHECK(!finding_id_parse(&e.from, CSTR_VIEW("bro#1/1")));
	CHECK(!hexid_init(&e.ask, CSTR_VIEW("68177013a5c94f0e8b1d2c3e4f506172")));
	CHECK(!cstr_view_store(CSTR_VIEW("{\"repo\":\"askai\"}"), &e.ids));
	expanded(&e, args, out, "[askai: cuh#2/3 from bro#1/1: call c9 in ask 68177013, turn 19, "
	         "tool git, stored exactly as sent. Repository askai. Arguments:]\n"
	         "{\"command\":\"grep\"}\n[askai: cuh#2/3: result, 25 bytes:]\nsrc/a.c\n\n"
	         "[askai: call c9]\n", __LINE__);
	CHECK(!cstr_view_store(CSTR_VIEW("{\"commit\":\"" A "\"}"), &e.ids));
	expanded(&e, args, out, "[askai: cuh#2/3 from bro#1/1: call c9 in ask 68177013, turn 19, "
	         "tool git, stored exactly as sent. Commit " A ". Arguments:]\n"
	         "{\"command\":\"grep\"}\n[askai: cuh#2/3: result, 25 bytes:]\nsrc/a.c\n\n"
	         "[askai: call c9]\n", __LINE__);
	/* Lines, with the whole IDs. */
	struct cstr_view const lines = CSTR_VIEW("161\tx\n171\ty");
	evidence_fini(&e);
	e.kind = EVIDENCE_LINES;
	e.item = 2;
	CHECK(!finding_id_parse(&e.from, CSTR_VIEW("cuh#1/3")));
	CHECK(!cstr_view_store(CSTR_VIEW("{\"repo\":\"askai\",\"commit\":\"" A "\",\"path\":"
	                                 "\"src/git.c\"}"), &e.ids));
	CHECK(!util_grow(e.ranges.items, e.ranges.cap, 2));
	e.ranges.items[e.ranges.n++] = range(161, 161);
	e.ranges.items[e.ranges.n++] = range(171, 171);
	hash_of(&e.result, lines);
	expanded(&e, cstr_view(nullptr), lines, "[askai: cuh#2/2 from cuh#1/3: lines 161, 171 of "
	         "src/git.c in repository askai at commit " A ", stored as shown:]\n"
	         "161\tx\n171\ty\n", __LINE__);
	expanded(&e, cstr_view(nullptr), cstr_view(nullptr), "[askai: cuh#2/2 from cuh#1/3: "
	         "lines 161, 171 of src/git.c in repository askai at commit " A ", stored as "
	         "shown:]\n[askai: the stored material of cuh#2/2 is missing.]\n",
	         __LINE__);
	struct dstr d = DSTR_INIT;
	struct evidence_material const m = { args, out };
	struct finding_id const id = { .number = 2, .item = 2, .author = "cuh" };
	CHECK(evidence_expand(nullptr, &id, &m, &d) == EFAULT);
	CHECK(evidence_expand(&e, nullptr, &m, &d) == EFAULT);
	CHECK(evidence_expand(&e, &id, nullptr, &d) == EFAULT);
	CHECK(evidence_expand(&e, &id, &m, nullptr) == EFAULT);
	CHECK(!dstr_len(&d));
	dstr_cleanup(&d);
	evidence_fini(&e);
}

int
main (void)
{
	test_range();
	test_ordinal();
	test_parse();
	test_put();
	test_cite();
	test_external();
	test_path();
	test_json();
	test_recite();
	test_expand();
	return check_report("evidence");
}
