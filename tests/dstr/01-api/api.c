/**
 * @file api.c
 * @brief The documented contract of every public function (trial 01)
 *
 * Linked with the allocation shim (tests/dstr/shim.h) so that leaks
 * and bad frees are caught too.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dstr.h"
#include "hell.h"
#include "shim.h"

/** Size of the object type under test */
#define OBJ (sizeof (struct dstr))

/** A string literal too long for in-place storage (300 characters) */
#define API_L300 \
  "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwx" \
  "yzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv" \
  "wxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrst" \
  "uvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmn"

/** Buffer behind api_text() */
static char api_buf[2048];

/**
 * @brief A string of `n` letters, valid until the next call
 */
static char const *
api_text (size_t n)
{
	for (size_t i = 0; i < n; ++i)
		api_buf[i] = (char)('a' + i % 26U);
	api_buf[n] = '\0';
	return api_buf;
}

/**
 * @brief Check every accessor of `s` against the expected state, and
 *        the zero tail: every byte after the terminator, up to the end
 *        of the storage, is zero
 *
 * @param s      Object to check
 * @param data   Expected content, which may include null characters
 * @param len    Expected length
 * @param blocks Expected block count, 0 for in-place storage
 */
static void
api_expect_n (struct dstr *s,
              char const  *data,
              size_t       len,
              size_t       blocks)
{
	size_t const cap = blocks ? blocks * DSTR_BLK_SIZE : OBJ;
	char const *const str = dstr_get(s);

	HELL_EQ(dstr_len(s), len);
	HELL_EQ(dstr_get_block_count(s), blocks);
	HELL_EQ(dstr_space_left(s), cap - 1U - len);
	HELL_EQ_PTR(dstr_raw(s), str);
	HELL_EQ_PTR(dstr_get_write_ptr(s), str ? str + len : NULL);
	if (!blocks)
		HELL_EQ_PTR(str, s->str);

	struct dstr_info const info = dstr_info(s);
	HELL_EQ(info.state, blocks ? DSTR_IS_PTR : DSTR_IS_SSO);
	HELL_EQ(info.blocks, blocks);
	HELL_EQ(info.length, len);
	HELL_EQ(info.space, cap - 1U - len);

	HELL_CHECK(str != NULL);
	if (!str || len >= cap)
		return;
	HELL_EQ_MEM(str, data, len);

	/* In place, the meta byte is the last byte, and the terminator of
	 * a full string */
	size_t const end = blocks ? cap : OBJ - 1U;
	size_t nonzero = 0;
	for (size_t i = len; i < end; ++i)
		nonzero += str[i] != 0;
	HELL_EQ(nonzero, 0U);
	if (!blocks)
		HELL_EQ(s->raw[OBJ - 1U], OBJ - 1U - len);
}

/**
 * @brief api_expect_n() of a null-terminated `text`
 */
static void
api_expect (struct dstr *s,
            char const  *text,
            size_t       blocks)
{
	HELL_EQ_STR(dstr_get(s), text);
	api_expect_n(s, text, strlen(text), blocks);
}

/**
 * @brief Check that `b` is in the state of `a`: the same content, the
 *        same storage and the same growth count
 */
static void
api_same (struct dstr *a,
          struct dstr *b)
{
	struct dstr_info const x = dstr_info(a);
	struct dstr_info const y = dstr_info(b);

	HELL_EQ(y.state, x.state);
	HELL_EQ(y.blocks, x.blocks);
	HELL_EQ(y.length, x.length);
	HELL_EQ(y.space, x.space);
	HELL_EQ(b->raw[OBJ - 1U], a->raw[OBJ - 1U]);
	if (x.length == y.length)
		HELL_EQ_MEM(dstr_get(b), dstr_get(a), x.length + 1U);
}

/**
 * @brief dstr() with NULL, empty, short, SSO-full and heap input
 */
static void
api_init (void)
{
	struct dstr s = dstr(NULL);
	api_expect(&s, "", 0);
	dstr_cleanup(&s);

	s = dstr("");
	api_expect(&s, "", 0);
	dstr_cleanup(&s);

	for (size_t len = 1; len < OBJ; ++len) {
		s = dstr(api_text(len));
		api_expect(&s, api_text(len), 0);
		dstr_cleanup(&s);
	}

	/* A full SSO string: the meta byte doubles as the terminator */
	s = dstr(api_text(OBJ - 1U));
	HELL_EQ(s.raw[OBJ - 1U], 0U);
	dstr_cleanup(&s);

	/* One character more moves it to an exact heap allocation */
	static size_t const heap[][2] = {
		{ OBJ, 1 }, { 255, 1 }, { 256, 2 }, { 300, 2 },
		{ 511, 2 }, { 512, 3 }, { 1000, 4 },
	};
	for (size_t i = 0; i < sizeof heap / sizeof heap[0]; ++i) {
		s = dstr(api_text(heap[i][0]));
		api_expect(&s, api_text(heap[i][0]), heap[i][1]);
		dstr_cleanup(&s);
		api_expect(&s, "", 0);
	}
}

/**
 * @brief NULL objects and NULL or empty sources, as documented
 */
static void
api_null (void)
{
	dstr_cleanup(NULL);
	HELL_EQ(dstr_len(NULL), 0U);
	HELL_EQ_PTR(dstr_get(NULL), NULL);
	HELL_EQ_PTR(dstr_raw(NULL), NULL);
	HELL_EQ_PTR(dstr_get_write_ptr(NULL), NULL);
	HELL_EQ(dstr_space_left(NULL), 0U);
	HELL_EQ(dstr_get_block_count(NULL), 0U);
	HELL_EQ(dstr_info(NULL).state, DSTR_IS_NOT);
	HELL_EQ_PTR(dstr_reserve(NULL, 0), NULL);
	HELL_EQ_PTR(dstr_reserve(NULL, 10), NULL);
	HELL_EQ_PTR(dstr_cat(NULL, "x"), NULL);
	HELL_EQ_PTR(dstr_cat(NULL, ""), NULL);
	HELL_EQ_PTR(dstr_cat(NULL, NULL), NULL);

	HELL_EQ_PTR(dstr_cat_n(NULL, "x", 1), NULL);
	HELL_EQ_PTR(dstr_cat_n(NULL, "x", 0), NULL);
	HELL_EQ_PTR(dstr_cat_n(NULL, NULL, 0), NULL);
	HELL_EQ_PTR(dstr_cat_n(NULL, NULL, 1), NULL);
	HELL_EQ_PTR(dstr_printf(NULL, "%s", "x"), NULL);
	HELL_EQ_PTR(dstr_printf(NULL, "%s", ""), NULL);
	HELL_EQ_PTR(dstr_erase(NULL, 0, 0), NULL);
	HELL_EQ_PTR(dstr_erase(NULL, 0, 1), NULL);
	HELL_EQ_PTR(dstr_clear(NULL), NULL);

	struct dstr s = dstr_move(NULL);
	api_expect(&s, "", 0);

	s = dstr("abc");
	HELL_EQ_PTR(dstr_cat(&s, NULL), &s);
	HELL_EQ_PTR(dstr_cat(&s, ""), &s);
	api_expect(&s, "abc", 0);

	/* A length of 0 is a no-op whatever the source; a null source of
	 * any other length fails, before the buffer grows for it */
	struct dstr const before = s;
	HELL_EQ_PTR(dstr_cat_n(&s, NULL, 0), &s);
	HELL_EQ_PTR(dstr_cat_n(&s, "x", 0), &s);
	HELL_EQ_MEM(&s, &before, sizeof s);
	HELL_EQ_PTR(dstr_cat_n(&s, NULL, 1), NULL);
	HELL_EQ_PTR(dstr_cat_n(&s, NULL, sizeof s), NULL);
	HELL_EQ_PTR(dstr_cat_n(&s, NULL, SIZE_MAX), NULL);
	HELL_EQ_MEM(&s, &before, sizeof s);

	/* A null format fails (volatile hides it from the compilers'
	 * format checks) */
	char const *volatile const nofmt = NULL;
	HELL_EQ_PTR(dstr_printf(&s, nofmt, 1), NULL);
	HELL_EQ_MEM(&s, &before, sizeof s);
	dstr_cleanup(&s);
}

/**
 * @brief The examples and promises in the doc comments
 */
static void
api_docs (void)
{
	/* "if dstr_len() returns 250, and buffer capacity is 256
	 * bytes, then dstr_space_left() will return 5" */
	struct dstr s = dstr(api_text(250));
	api_expect(&s, api_text(250), 1);
	HELL_EQ(dstr_space_left(&s), 5U);

	/* reserve(0) and a reserve that fits change nothing */
	struct dstr const before = s;
	HELL_EQ_PTR(dstr_reserve(&s, 0), &s);
	HELL_EQ_MEM(&s, &before, sizeof s);
	HELL_EQ_PTR(dstr_reserve(&s, 5), &s);
	HELL_EQ_MEM(&s, &before, sizeof s);

	/* One more byte grows it to (250 + 6) / 256 + 1 blocks, with no
	 * spare blocks */
	HELL_EQ_PTR(dstr_reserve(&s, 6), &s);
	api_expect(&s, api_text(250), 2);

	/* The write pointer is the terminator */
	HELL_EQ(*dstr_get_write_ptr(&s), 0U);
	dstr_cleanup(&s);

	/* "Appends that fit, up to the last byte of the buffer, never
	 * allocate", and "the step starts at DSTR_BLK_SIZE and
	 * doubles with every growth by this function": each time the
	 * buffer is full, one more byte grows it. 513 bytes round up to a
	 * multiple of 256, 768, then 769 bytes to a multiple of 512, 1024,
	 * and 1025 bytes to a multiple of 1 KiB, 2048. */
	static size_t const steps[][2] = {
		/* Length after the growth, blocks */
		{ 512, 3 }, { 768, 4 }, { 1024, 8 },
	};
	s = dstr(api_text(300));
	for (size_t i = 0; i < sizeof steps / sizeof steps[0]; ++i) {
		size_t const len = dstr_len(&s);
		size_t const full = steps[i][0] - 1U;
		size_t const calls = shim_calls();
		HELL_EQ_PTR(dstr_cat(&s, api_text(full) + len), &s);
		HELL_EQ(shim_calls(), calls);
		HELL_EQ(dstr_space_left(&s), 0U);
		HELL_EQ_PTR(dstr_cat(&s, api_text(full + 1U) + full), &s);
		HELL_EQ(shim_calls(), calls + 1U);
		api_expect(&s, api_text(full + 1U), steps[i][1]);
	}

	/* A reserve stays exact after them: 1024 more characters and the
	 * terminator make 2049 bytes, 9 blocks, where an append would
	 * round up to 2 KiB */
	HELL_EQ_PTR(dstr_reserve(&s, 1024), &s);
	api_expect(&s, api_text(1024), 9);
	dstr_cleanup(&s);
}

/**
 * @brief Appending, within SSO, across the SSO limit and on the heap
 */
static void
api_cat (void)
{
	struct dstr s = dstr("abc");
	HELL_EQ_PTR(dstr_cat(&s, "def"), &s);
	api_expect(&s, "abcdef", 0);
	dstr_cleanup(&s);

	/* Fill SSO exactly, then one more character: exact heap block */
	s = (struct dstr)DSTR_INIT;
	HELL_EQ_PTR(dstr_cat(&s, api_text(OBJ - 1U)), &s);
	api_expect(&s, api_text(OBJ - 1U), 0);
	HELL_EQ(s.raw[OBJ - 1U], 0U);

	char expect[OBJ + 1U];
	memcpy(expect, api_text(OBJ - 1U), OBJ - 1U);
	expect[OBJ - 1U] = 'X';
	expect[OBJ] = '\0';
	HELL_EQ_PTR(dstr_cat(&s, "X"), &s);
	api_expect(&s, expect, 1);
	dstr_cleanup(&s);

	/* Build a heap string out of many pieces */
	char want[1024];
	size_t len = 0;
	s = dstr(NULL);
	for (size_t i = 0; i < 40; ++i) {
		char const *const piece = api_text(i % 23U + 1U);
		size_t const n = strlen(piece);
		memcpy(want + len, piece, n + 1U);
		len += n;
		HELL_EQ_PTR(dstr_cat(&s, piece), &s);
		HELL_EQ(dstr_len(&s), len);
		HELL_EQ_STR(dstr_get(&s), want);
	}
	dstr_cleanup(&s);
}

/**
 * @brief cat_n(): exactly `len` bytes, null characters included, from
 *        a source that needn't be null-terminated
 */
static void
api_cat_n (void)
{
	/* No terminator after these, and null characters among them */
	static char const raw[] = { 'w', '\0', 'x', '\0', '\0', 'y', 'z' };

	struct dstr s = dstr("ab");
	HELL_EQ_PTR(dstr_cat_n(&s, raw, sizeof raw), &s);
	api_expect_n(&s, "abw\0x\0\0yz", 9, 0);

	/* A C string up to the first null character */
	HELL_EQ_STR(dstr_get(&s), "abw");

	/* Part of them, from the middle */
	HELL_EQ_PTR(dstr_cat_n(&s, &raw[4], 2), &s);
	api_expect_n(&s, "abw\0x\0\0yz\0y", 11, 0);
	dstr_cleanup(&s);

	/* Only null characters */
	HELL_EQ_PTR(dstr_cat_n(&s, &raw[3], 2), &s);
	api_expect_n(&s, "\0\0", 2, 0);
	dstr_cleanup(&s);

	/* Fill in place exactly, then one byte more: an exact heap block
	 * and a growth count of 1, as with cat() */
	char want[600];
	memcpy(want, api_text(OBJ - 1U), OBJ - 1U);
	want[3] = '\0';
	HELL_EQ_PTR(dstr_cat_n(&s, want, OBJ - 1U), &s);
	api_expect_n(&s, want, OBJ - 1U, 0);
	HELL_EQ(s.raw[OBJ - 1U], 0U);
	want[OBJ - 1U] = '\0';
	HELL_EQ_PTR(dstr_cat_n(&s, &raw[1], 1), &s);
	api_expect_n(&s, want, OBJ, 1);
	HELL_EQ(s.raw[OBJ - 1U], DSTR_PTR_FLAG | 1U);
	dstr_cleanup(&s);

	/* Pieces with nulls (U+0000) in and out of place, every third byte */
	for (size_t i = 0; i < sizeof want; ++i)
		want[i] = i % 3U ? (char)('a' + i % 26U) : '\0';
	size_t len = 0;
	for (size_t n = 1; len + n <= sizeof want; len += n++) {
		HELL_EQ_PTR(dstr_cat_n(&s, &want[len], n), &s);
		HELL_EQ(dstr_len(&s), len + n);
		HELL_EQ_MEM(dstr_get(&s), want, len + n);
		HELL_EQ(dstr_get(&s)[len + n], 0);
	}
	dstr_cleanup(&s);
}

/**
 * @brief printf(): the text snprintf() makes, appended in and out of
 *        place, and nothing on failure
 */
static void
api_printf (void)
{
	struct dstr s = DSTR_INIT;
	HELL_EQ_PTR(dstr_printf(&s, "%s=%d,%zu%%", "x", -42, (size_t)7), &s);
	api_expect(&s, "x=-42,7%", 0);
	HELL_EQ_PTR(dstr_printf(&s, "%c%u", 'Y', 12U), &s);
	api_expect(&s, "x=-42,7%Y12", 0);

	/* An empty text changes nothing */
	struct dstr before = s;
	HELL_EQ_PTR(dstr_printf(&s, "%s", ""), &s);
	HELL_EQ_MEM(&s, &before, sizeof s);
	dstr_cleanup(&s);

	/* A null character from a conversion is a byte like the others,
	 * in place and on the heap */
	HELL_EQ_PTR(dstr_printf(&s, "a%cb", '\0'), &s);
	api_expect_n(&s, "a\0b", 3, 0);
	dstr_cleanup(&s);
	char want[4096];
	s = dstr(api_text(300));
	memcpy(want, api_text(300), 300);
	memcpy(want + 300, "a\0b", 4);
	HELL_EQ_PTR(dstr_printf(&s, "a%cb", '\0'), &s);
	api_expect_n(&s, want, 303, 2);
	dstr_cleanup(&s);

	/* Fill in place exactly: the terminator is the meta byte. Then one
	 * byte more: an exact heap block and a growth count of 1. */
	memcpy(want, "ab", 2);
	memcpy(want + 2, api_text(OBJ - 4U), OBJ - 4U);
	memcpy(want + OBJ - 2U, "!7", 3);
	s = dstr("ab");
	HELL_EQ_PTR(dstr_printf(&s, "%s%c", api_text(OBJ - 4U), '!'), &s);
	api_expect_n(&s, want, OBJ - 1U, 0);
	HELL_EQ(s.raw[OBJ - 1U], 0U);
	HELL_EQ_PTR(dstr_printf(&s, "%d", 7), &s);
	api_expect(&s, want, 1);
	HELL_EQ(s.raw[OBJ - 1U], DSTR_PTR_FLAG | 1U);
	dstr_cleanup(&s);

	/* From in place straight to two blocks */
	s = dstr("ab");
	HELL_EQ_PTR(dstr_printf(&s, "%s", api_text(300)), &s);
	memcpy(want + 2, api_text(300), 301);
	api_expect(&s, want, 2);
	HELL_EQ(s.raw[OBJ - 1U], DSTR_PTR_FLAG | 1U);
	dstr_cleanup(&s);

	/* Many pieces, checked against snprintf() */
	size_t len = 0;
	for (int i = 0; i < 80; ++i) {
		char const *const t = api_text((size_t)i % 29U);
		size_t const n = (size_t)snprintf(want + len, sizeof want - len,
		                                  "[%zu:%s:%d]", (size_t)i, t, -i);
		HELL_EQ_PTR(dstr_printf(&s, "[%zu:%s:%d]", (size_t)i, t, -i), &s);
		len += n;
		HELL_EQ(dstr_len(&s), len);
		HELL_EQ_MEM(dstr_get(&s), want, len + 1U);
	}
	dstr_cleanup(&s);

#if __STDC_HOSTED__
	/* An encoding error: a lone UTF-16 surrogate has no multibyte form
	 * in any locale. Nothing changes, in place or on the heap, and
	 * nothing is allocated. */
	static wchar_t const bad[] = { 0xD800, 0 };
	static char const *const texts[] = { "abc", API_L300 };
	for (size_t i = 0; i < sizeof texts / sizeof texts[0]; ++i) {
		s = dstr(texts[i]);
		before = s;
		size_t const calls = shim_calls();
		HELL_EQ_PTR(dstr_printf(&s, "%s%ls", API_L300, bad), NULL);
		HELL_EQ(shim_calls(), calls);
		HELL_EQ_MEM(&s, &before, sizeof s);
		HELL_EQ_PTR(dstr_printf(&s, "%s", "ok"), &s);
		dstr_cleanup(&s);
	}
#endif /* __STDC_HOSTED__ */
}

/**
 * @brief cat_n() and printf() grow a buffer like cat(): the same pieces
 *        make the same object, block count and growth count included
 */
static void
api_alike (void)
{
	struct dstr a = DSTR_INIT;
	struct dstr b = DSTR_INIT;
	struct dstr c = DSTR_INIT;

	for (size_t i = 0; i < 60; ++i) {
		size_t const n = i * 37U % 400U + 1U;
		char const *const piece = api_text(n);
		HELL_EQ_PTR(dstr_cat(&a, piece), &a);
		HELL_EQ_PTR(dstr_cat_n(&b, piece, n), &b);
		HELL_EQ_PTR(dstr_printf(&c, "%s", piece), &c);
		api_same(&a, &b);
		api_same(&a, &c);
	}
	dstr_cleanup(&a);
	dstr_cleanup(&b);
	dstr_cleanup(&c);
}

/**
 * @brief erase(): a prefix, the middle, a suffix, a length past the end,
 *        nothing at the end, and a position past the end
 */
static void
api_erase (void)
{
	struct dstr s = dstr("abcdefgh");
	HELL_EQ_PTR(dstr_erase(&s, 0, 2), &s);
	api_expect(&s, "cdefgh", 0);
	HELL_EQ_PTR(dstr_erase(&s, 2, 2), &s);
	api_expect(&s, "cdgh", 0);
	HELL_EQ_PTR(dstr_erase(&s, 3, 1), &s);
	api_expect(&s, "cdg", 0);
	HELL_EQ_PTR(dstr_erase(&s, 1, 1000), &s);
	api_expect(&s, "c", 0);

	/* Nothing to erase: at the end, or none of it */
	struct dstr const before = s;
	HELL_EQ_PTR(dstr_erase(&s, 1, 5), &s);
	HELL_EQ_PTR(dstr_erase(&s, 1, SIZE_MAX), &s);
	HELL_EQ_PTR(dstr_erase(&s, 0, 0), &s);
	HELL_EQ_MEM(&s, &before, sizeof s);

	/* Past the end: fails, and nothing changes */
	HELL_EQ_PTR(dstr_erase(&s, 2, 0), NULL);
	HELL_EQ_PTR(dstr_erase(&s, 2, 1), NULL);
	HELL_EQ_PTR(dstr_erase(&s, SIZE_MAX, SIZE_MAX), NULL);
	HELL_EQ_MEM(&s, &before, sizeof s);

	HELL_EQ_PTR(dstr_erase(&s, 0, SIZE_MAX), &s);
	api_expect(&s, "", 0);
	HELL_EQ_PTR(dstr_erase(&s, 0, 1), &s);
	HELL_EQ_PTR(dstr_erase(&s, 1, 0), NULL);
	api_expect(&s, "", 0);

	/* Null characters are bytes like the others */
	HELL_EQ_PTR(dstr_cat_n(&s, "a\0b\0c", 5), &s);
	HELL_EQ_PTR(dstr_erase(&s, 1, 2), &s);
	api_expect_n(&s, "a\0c", 3, 0);
	dstr_cleanup(&s);

	/* On the heap, the buffer stays where it is, with its block count
	 * and growth count, and nothing is allocated */
	char want[601];
	memcpy(want, api_text(600), 601);
	HELL_EQ_PTR(dstr_cat(&s, want), &s);
	api_expect(&s, want, 3);
	char const *const buf = dstr_get(&s);
	size_t const calls = shim_calls();

	static size_t const cuts[][2] = {
		/* Position, length */
		{ 10, 100 }, { 400, SIZE_MAX }, { 0, 399 }, { 0, 1 },
	};
	size_t len = 600;
	for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; ++i) {
		size_t const pos = cuts[i][0];
		size_t const n = cuts[i][1] < len - pos ? cuts[i][1] : len - pos;
		memmove(&want[pos], &want[pos + n], len - pos - n);
		len -= n;
		HELL_EQ_PTR(dstr_erase(&s, pos, cuts[i][1]), &s);
		api_expect_n(&s, want, len, 3);
		HELL_EQ_PTR(dstr_get(&s), buf);
		HELL_EQ(s.raw[OBJ - 1U], DSTR_PTR_FLAG | 1U);
	}
	HELL_EQ(len, 0U);
	HELL_EQ(shim_calls(), calls);
	HELL_EQ_PTR(dstr_erase(&s, 1, 0), NULL);
	dstr_cleanup(&s);
}

/**
 * @brief clear() empties the string, keeps a heap buffer with its block
 *        count, and the appends that fit in it don't allocate
 */
static void
api_clear (void)
{
	struct dstr const empty = DSTR_INIT;
	struct dstr s = dstr("abc");
	HELL_EQ_PTR(dstr_clear(&s), &s);
	api_expect(&s, "", 0);
	HELL_EQ_MEM(&s, &empty, sizeof s);

	size_t const live = shim_live();
	s = dstr(api_text(1000));
	char const *const buf = dstr_get(&s);
	HELL_EQ_PTR(dstr_clear(&s), &s);
	api_expect(&s, "", 4);
	HELL_EQ_PTR(dstr_get(&s), buf);
	HELL_EQ(s.raw[OBJ - 1U], DSTR_PTR_FLAG);
	HELL_EQ(shim_live(), live + 1U);

	size_t const calls = shim_calls();
	HELL_EQ_PTR(dstr_cat(&s, api_text(1023)), &s);
	api_expect(&s, api_text(1023), 4);
	HELL_EQ_PTR(dstr_clear(&s), &s);
	HELL_EQ_PTR(dstr_clear(&s), &s);
	api_expect(&s, "", 4);
	HELL_EQ(shim_calls(), calls);
	dstr_cleanup(&s);
	HELL_EQ(shim_live(), live);
}

/**
 * @brief move() hands the content over, a heap buffer without
 *        allocating, and leaves an empty, usable string behind
 */
static void
api_move (void)
{
	struct dstr const empty = DSTR_INIT;
	struct dstr s = dstr("abc");
	struct dstr t = dstr_move(&s);
	api_expect(&t, "abc", 0);
	HELL_EQ_MEM(&s, &empty, sizeof s);
	dstr_cleanup(&t);

	size_t const live = shim_live();
	s = dstr(api_text(300));
	char const *const buf = dstr_get(&s);
	size_t const calls = shim_calls();
	t = dstr_move(&s);
	HELL_EQ(shim_calls(), calls);
	HELL_EQ(shim_live(), live + 1U);
	HELL_EQ_PTR(dstr_get(&t), buf);
	api_expect(&t, api_text(300), 2);
	HELL_EQ_MEM(&s, &empty, sizeof s);

	/* The source is reusable, and the buffer can move back */
	HELL_EQ_PTR(dstr_cat(&s, "again"), &s);
	api_expect(&s, "again", 0);
	s = dstr_move(&t);
	HELL_EQ_PTR(dstr_get(&s), buf);
	HELL_EQ_MEM(&t, &empty, sizeof t);
	dstr_cleanup(&t);
	dstr_cleanup(&s);
	HELL_EQ(shim_live(), live);

	/* Automatic objects on both sides free the buffer once */
	{
		dstr_auto a = dstr(api_text(300));
		dstr_auto b = dstr_move(&a);
		api_expect(&b, api_text(300), 2);
		HELL_EQ(shim_live(), live + 1U);
	}
	HELL_EQ(shim_live(), live);
}

/**
 * @brief cleanup() frees, resets to empty, repeats safely, and the
 *        object can be used again
 */
static void
api_cleanup (void)
{
	size_t const live = shim_live();
	struct dstr s = dstr(api_text(700));
	HELL_EQ(shim_live(), live + 1U);
	dstr_cleanup(&s);
	HELL_EQ(shim_live(), live);
	api_expect(&s, "", 0);
	dstr_cleanup(&s);
	api_expect(&s, "", 0);

	HELL_EQ_PTR(dstr_cat(&s, "hello"), &s);
	api_expect(&s, "hello", 0);
	HELL_EQ_PTR(dstr_cat(&s, api_text(300)), &s);
	HELL_EQ(dstr_len(&s), 305U);
	dstr_cleanup(&s);
	HELL_EQ(shim_live(), live);
}

/** A struct with a dstr member, for DSTR_INIT */
struct api_record {
	struct dstr name;
	unsigned         id;
};

/** Static storage initialized with DSTR_INIT */
static struct dstr api_static = DSTR_INIT;

/** Designated member initializer with DSTR_INIT */
static struct api_record api_member = { .name = DSTR_INIT, .id = 7 };

/**
 * @brief DSTR_INIT wherever dstr() can't be used
 */
static void
api_init_macro (void)
{
	api_expect(&api_static, "", 0);
	HELL_EQ_PTR(dstr_cat(&api_static, "static"), &api_static);
	api_expect(&api_static, "static", 0);
	dstr_cleanup(&api_static);

	api_expect(&api_member.name, "", 0);
	HELL_EQ(api_member.id, 7U);

	struct dstr arr[3] = { DSTR_INIT, DSTR_INIT,
	                       DSTR_INIT };
	for (size_t i = 0; i < 3; ++i) {
		api_expect(&arr[i], "", 0);
		HELL_EQ_PTR(dstr_cat(&arr[i], api_text(300)), &arr[i]);
		api_expect(&arr[i], api_text(300), 2);
		dstr_cleanup(&arr[i]);
	}

	/* A zeroed struct is not valid until its member is initialized */
	struct api_record *const r = calloc(1, sizeof *r);
	HELL_CHECK(r != NULL);
	if (r) {
		r->name = (struct dstr)DSTR_INIT;
		api_expect(&r->name, "", 0);
		HELL_EQ_PTR(dstr_cat(&r->name, "record"), &r->name);
		api_expect(&r->name, "record", 0);
		dstr_cleanup(&r->name);
		free(r);
	}
}

/**
 * @brief dstr_auto frees its buffer when the scope ends
 */
static void
api_auto (void)
{
	size_t const live = shim_live();
	{
		dstr_auto s = dstr(api_text(300));
		HELL_EQ(shim_live(), live + 1U);
		api_expect(&s, api_text(300), 2);
		HELL_EQ_PTR(dstr_cat(&s, api_text(500)), &s);
		HELL_EQ(shim_live(), live + 1U);
	}
	HELL_EQ(shim_live(), live);
	{
		dstr_auto s = dstr("short");
		api_expect(&s, "short", 0);
	}
	HELL_EQ(shim_live(), live);
}

/**
 * @brief The dstr() macro: every kind of argument, evaluated once
 */
static void
api_dispatch (void)
{
	/* Elements of arrays of arrays and of pointers, with side effects */
	char tab[3][8] = { "one", "two", "three" };
	char const *ptrs[3] = { "uno", "dos", "tres" };
	size_t i = 0;
	struct dstr s = dstr(tab[i++]);
	HELL_EQ(i, 1U);
	api_expect(&s, "one", 0);
	dstr_cleanup(&s);
	s = dstr(ptrs[i++]);
	HELL_EQ(i, 2U);
	api_expect(&s, "dos", 0);
	dstr_cleanup(&s);

	/* A conditional expression is a pointer, not an array */
	s = dstr(i ? "yes" : "no");
	api_expect(&s, "yes", 0);
	dstr_cleanup(&s);

	/* NULL and a literal 0 */
	s = dstr(NULL);
	api_expect(&s, "", 0);
	s = dstr(0);
	api_expect(&s, "", 0);

	/* A constant from a literal is the same object the function makes */
	struct dstr a = dstr("hello");
	struct dstr b = (dstr)("hello");
	HELL_EQ_MEM(&a, &b, sizeof a);
	api_expect(&a, "hello", 0);
	dstr_cleanup(&a);
	dstr_cleanup(&b);

	/* A long literal is allocated, exactly */
	size_t const live = shim_live();
	s = dstr(API_L300);
	HELL_EQ(shim_live(), live + 1U);
	api_expect(&s, API_L300, 2);
	dstr_cleanup(&s);
	HELL_EQ(shim_live(), live);

	/* A literal with an embedded null, and a parenthesized literal */
	s = dstr("ab\0cd");
	api_expect(&s, "ab", 0);
	dstr_cleanup(&s);
	s = dstr(("paren"));
	api_expect(&s, "paren", 0);
	dstr_cleanup(&s);

	/* Arrays and pointers into them, in place or allocated by length */
	char small[OBJ] = "small";
	s = dstr(small);
	api_expect(&s, "small", 0);
	dstr_cleanup(&s);
	s = dstr(small + 1);
	api_expect(&s, "mall", 0);
	dstr_cleanup(&s);
	char large[300];
	memcpy(large, API_L300, 299);
	large[299] = '\0';
	s = dstr(large);
	HELL_EQ(dstr_len(&s), 299U);
	HELL_EQ(dstr_get_block_count(&s), 2U);
	dstr_cleanup(&s);

	/* A trailing one-element array (the "struct hack") holds a string
	 * much longer than its declared size */
	struct api_hack {
		size_t len;
		char   data[1];
	};
	struct api_hack *const h = malloc(sizeof *h + 100U);
	HELL_CHECK(h != NULL);
	if (h) {
		memcpy(h->data, api_text(66), 67);
		s = dstr(h->data);
		api_expect(&s, api_text(66), 66U < OBJ ? 0U : 1U);
		dstr_cleanup(&s);
		free(h);
	}
}

int
main (void)
{
	api_init();
	api_null();
	api_docs();
	api_cat();
	api_cat_n();
	api_printf();
	api_alike();
	api_erase();
	api_clear();
	api_move();
	api_cleanup();
	api_init_macro();
	api_auto();
	api_dispatch();

	HELL_EQ(shim_live(), 0U);
	HELL_EQ(shim_bad(), 0U);
	return hell_exit();
}
