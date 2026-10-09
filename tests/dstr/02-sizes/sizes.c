/**
 * @file sizes.c
 * @brief Boundary sweep for one object size (trial 02)
 *
 * Compiled once per legal DSTR_OBJ_SIZE with -DSUITE=name. All the
 * copies and a single dstr.c are linked into one program, which also
 * proves that objects of different sizes coexist (README).
 *
 * The expected block counts follow the growth policy. dstr() and
 * dstr_reserve() allocate exactly: a string of `len` characters gets
 * len / 256 + 1 blocks. dstr_cat() rounds the size up to a step of 256
 * bytes shifted left by the buffer's growth count, at most by
 * DSTR_GROW_SHIFT_MAX, and counts the growth, up to 15; dstr_cat_n()
 * and dstr_printf() grow the same way. The count lives in the meta byte
 * of a heap string, where these tests read it white-box.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dstr.h"
#include "hell.h"

#ifndef SUITE
# error "compile with -DSUITE=name"
#endif

/** Size of the object type under test */
#define OBJ (sizeof (struct dstr))

/** Longest test string */
#define SZ_MAX 70000U

/** Source text and the expected content */
static char sz_src[SZ_MAX + 1U];
static char sz_want[SZ_MAX + 1U];

/**
 * @brief A string of `n` characters from the alphabet starting at
 *        `first`, valid until the next call with the same buffer
 */
static char const *
sz_text (char   *buf,
         size_t  n,
         char    first)
{
	for (size_t i = 0; i < n; ++i)
		buf[i] = (char)(first + (char)(i % 26U));
	buf[n] = '\0';
	return buf;
}

/**
 * @brief Block count of a buffer that dstr_cat() grows to hold
 *        `need` characters when its growth count is `count`: `need`
 *        and the terminator, rounded up to a multiple of the step
 */
static size_t
sz_grown (size_t   need,
          unsigned count)
{
	size_t const shift = count < DSTR_GROW_SHIFT_MAX
	                   ? count : DSTR_GROW_SHIFT_MAX;
	size_t const step = DSTR_BLK_SIZE << shift;
	return (need + step) / step * step / DSTR_BLK_SIZE;
}

/**
 * @brief Check the meta byte of the heap string `s`: the pointer flag,
 *        the growth count `count` and zero bits between them
 */
static void
sz_expect_count (struct dstr const *s,
                 unsigned           count)
{
	unsigned const meta = s->raw[OBJ - 1U];
	unsigned const known = DSTR_PTR_FLAG | DSTR_GROW_COUNT_MASK;

	HELL_EQ(meta & DSTR_GROW_COUNT_MASK, count);
	HELL_EQ(meta & DSTR_PTR_FLAG, DSTR_PTR_FLAG);
	HELL_EQ(meta & ~known, 0U);
}

/** How sz_cat_next() appends */
static enum {
	SZ_CAT,
	SZ_CAT_N,
	SZ_PRINTF,
	SZ_MODES
} sz_mode;

/**
 * @brief Append the next `n` characters of the text in sz_want to `s`,
 *        which holds the first `len` of them, the way sz_mode says
 *
 * @return `false` if the text is too short, which only a buffer that
 *         grew far too much can cause
 */
static bool
sz_cat_next (struct dstr *s,
             size_t       len,
             size_t       n)
{
	HELL_CHECK(n <= SZ_MAX - len);
	if (n > SZ_MAX - len)
		return false;

	/* cat_n() gets the text where it is, unterminated */
	if (sz_mode == SZ_CAT_N) {
		HELL_EQ_PTR(dstr_cat_n(s, sz_want + len, n), s);
		return true;
	}
	memcpy(sz_src, sz_want + len, n);
	sz_src[n] = '\0';
	HELL_EQ_PTR(sz_mode == SZ_PRINTF ? dstr_printf(s, "%s", sz_src)
	                                 : dstr_cat(s, sz_src), s);
	return true;
}

/**
 * @brief Grow `s`, which holds the first `*len` characters of sz_want,
 *        `times` times with appends: fill the buffer up to its last
 *        byte, then add one more character
 */
static void
sz_regrow (struct dstr *s,
           size_t      *len,
           unsigned     times)
{
	while (times--) {
		size_t const space = dstr_space_left(s);
		if (!sz_cat_next(s, *len, space))
			return;
		*len += space;
		if (!sz_cat_next(s, *len, 1))
			return;
		++*len;
	}
}

/**
 * @brief Check `s` against `len` characters at `want` in `blocks`
 *        blocks (0 for in-place storage)
 */
static void
sz_expect (struct dstr *s,
           char const  *want,
           size_t       len,
           size_t       blocks)
{
	size_t const cap = blocks ? blocks * DSTR_BLK_SIZE : OBJ;
	char const *const str = dstr_get(s);
	struct dstr_info const info = dstr_info(s);

	HELL_EQ(info.state, blocks ? DSTR_IS_PTR : DSTR_IS_SSO);
	HELL_EQ(info.blocks, blocks);
	HELL_EQ(info.length, len);
	HELL_EQ(info.space, cap - 1U - len);
	HELL_EQ(dstr_len(s), len);
	HELL_EQ(dstr_get_block_count(s), blocks);
	HELL_EQ(dstr_space_left(s), cap - 1U - len);
	HELL_CHECK(str != NULL);
	if (!str)
		return;
	HELL_EQ_PTR(dstr_get_write_ptr(s), str + len);
	HELL_EQ_PTR(dstr_raw(s), str);
	HELL_EQ_MEM(str, want, len);
	HELL_EQ(str[len], 0U);
	if (!blocks)
		HELL_EQ_PTR(str, s->str);
}

/**
 * @brief Check the zero tail of `s`, which holds `len` bytes in
 *        `blocks` blocks: every byte after the terminator up to the end
 *        of the storage is zero, and in place the meta byte is the space
 *        left
 */
static void
sz_zero_tail (struct dstr *s,
              size_t       len,
              size_t       blocks)
{
	char const *const str = dstr_get(s);
	size_t const end = blocks ? blocks * DSTR_BLK_SIZE : OBJ - 1U;

	HELL_CHECK(str != NULL && len <= end);
	if (!str || len > end)
		return;
	size_t nonzero = 0;
	for (size_t i = len; i < end; ++i)
		nonzero += str[i] != 0;
	HELL_EQ(nonzero, 0U);
	if (!blocks)
		HELL_EQ(s->raw[OBJ - 1U], OBJ - 1U - len);
}

/**
 * @brief Append the `n` bytes at `src` to `s` with cat_n(), or with
 *        printf() if `fmt`, the last byte by a conversion of its own
 */
static struct dstr *
sz_append (struct dstr *s,
           char const  *src,
           size_t       n,
           bool         fmt)
{
	if (!fmt)
		return dstr_cat_n(s, src, n);
	if (!n)
		return dstr_printf(s, "%s", "");
	memcpy(sz_src, src, n - 1U);
	sz_src[n - 1U] = '\0';
	return dstr_printf(s, "%s%c", sz_src, src[n - 1U]);
}

/**
 * @brief dstr() at every length near the SSO limit and around
 *        block boundaries
 */
static void
sz_init (void)
{
	static size_t const edges[] = {
		255, 256, 257, 511, 512, 513, 767, 768, 769,
		4095, 4096, 4097, 65535, 65536, 65537,
	};
	size_t const sweep = OBJ + 600U;

	for (size_t i = 0; i < sweep + sizeof edges / sizeof edges[0]; ++i) {
		size_t const len = i < sweep ? i : edges[i - sweep];
		struct dstr s = dstr(sz_text(sz_src, len, 'a'));
		size_t const blocks = len < OBJ ? 0U
		                    : len / DSTR_BLK_SIZE + 1U;
		sz_expect(&s, sz_src, len, blocks);

		/* A full SSO string: the meta byte is the terminator */
		if (len == OBJ - 1U)
			HELL_EQ(s.raw[OBJ - 1U], 0U);

		/* An allocated one hasn't grown */
		if (blocks)
			sz_expect_count(&s, 0);

		dstr_cleanup(&s);
		sz_expect(&s, "", 0, 0);
	}
}

/**
 * @brief Append one character at a time from empty to 9000
 *        characters, checking the block count at every step: the
 *        capacity goes 256 bytes, 512, 1 KiB, 2 KiB, 4 KiB, then 4 KiB
 *        more with each growth (9000 reaches two of those), and the
 *        growth count goes up by one with each growth
 */
static void
sz_bytewise (void)
{
	/* Blocks after each growth */
	static size_t const caps[] = { 1, 2, 4, 8, 16, 32, 48 };
	size_t const n_caps = sizeof caps / sizeof caps[0];
	struct dstr s = DSTR_INIT;
	size_t blocks = 0;
	size_t growths = 0;

	sz_text(sz_want, 9000, 'a');
	for (size_t len = 0; len < 9000; ++len) {
		char const one[2] = { sz_want[len], '\0' };
		char const *const before = dstr_get(&s);
		size_t const need = len + 1U;

		HELL_EQ_PTR(dstr_cat(&s, one), &s);

		/* A character that fits, up to the last byte, doesn't move
		 * the string; the next one grows the buffer */
		if (need < (blocks ? blocks * DSTR_BLK_SIZE : OBJ)) {
			HELL_EQ_PTR(dstr_get(&s), before);
		} else {
			HELL_CHECK(growths < n_caps);
			blocks = caps[growths < n_caps ? growths : n_caps - 1U];
			sz_expect_count(&s, (unsigned)++growths);
		}

		/* The full content every 64 steps, the rest every step */
		if (!(need % 64U)) {
			sz_expect(&s, sz_want, need, blocks);
			continue;
		}
		HELL_EQ(dstr_len(&s), need);
		HELL_EQ(dstr_get_block_count(&s), blocks);
		HELL_EQ(dstr_space_left(&s),
		        (blocks ? blocks * DSTR_BLK_SIZE : OBJ) - 1U - need);
		HELL_EQ(dstr_get(&s)[need - 1U], one[0]);
		HELL_EQ(dstr_get(&s)[need], 0U);
	}
	HELL_EQ(growths, n_caps);
	sz_expect(&s, sz_want, 9000, blocks);
	dstr_cleanup(&s);
}

/**
 * @brief Appends that fill the buffer up to its last byte don't move
 *        the string, and the next byte grows the buffer by one step.
 *        Eighteen growths this way take the growth count to its limit
 *        of 15 and three past it, where the count stays at 15, the
 *        step at 4 KiB and the meta bits above the count at zero.
 */
static void
sz_fill (void)
{
	struct dstr s = DSTR_INIT;
	size_t len = 0;
	size_t blocks = 0;
	unsigned count = 0;

	sz_text(sz_want, SZ_MAX, 'a');
	for (unsigned g = 1; g <= 18U; ++g) {
		/* Fill up, from in place the first time */
		char const *const before = dstr_get(&s);
		size_t const space = (blocks ? blocks * DSTR_BLK_SIZE : OBJ)
		                   - 1U - len;
		HELL_EQ(dstr_space_left(&s), space);
		sz_cat_next(&s, len, space);
		len += space;
		HELL_EQ(dstr_space_left(&s), 0U);
		HELL_EQ_PTR(dstr_get(&s), before);
		sz_expect(&s, sz_want, len, blocks);
		if (blocks)
			sz_expect_count(&s, count);

		/* The next byte grows it: the capacity doubles up to 4 KiB,
		 * then grows by 4 KiB at a time */
		sz_cat_next(&s, len, 1);
		++len;
		blocks = g <= 5U ? (size_t)1 << (g - 1U) : 16U * (g - 4U);
		count = g < 15U ? g : 15U;
		sz_expect(&s, sz_want, len, blocks);
		sz_expect_count(&s, count);
	}
	dstr_cleanup(&s);
}

/**
 * @brief A string that dstr() allocates has growth count 0: its
 *        first growth by appends is exact, the next one rounds up to a
 *        multiple of 512 bytes and the one after it to 1 KiB
 */
static void
sz_constructed (void)
{
	static size_t const cases[][4] = {
		/* Length, then blocks after each of three growths. A
		 * growth fills the buffer and adds a byte, so an exact one
		 * adds a block: that tells the first growth of 300 and 5000
		 * from a 512-byte step, and the second one of 600 and 5300
		 * from an exact growth. */
		{  300,  3,  4,  8 },
		{  600,  4,  6,  8 },
		{ 5000, 21, 22, 24 },
		{ 5300, 22, 24, 28 },
	};

	sz_text(sz_want, SZ_MAX, 'a');
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		size_t len = cases[i][0];
		struct dstr s = dstr(sz_text(sz_src, len, 'a'));
		sz_expect(&s, sz_want, len, len / DSTR_BLK_SIZE + 1U);
		sz_expect_count(&s, 0);

		for (unsigned g = 1; g <= 3U; ++g) {
			sz_regrow(&s, &len, 1);
			sz_expect(&s, sz_want, len, cases[i][g]);
			sz_expect_count(&s, g);
		}
		dstr_cleanup(&s);
	}
}

/**
 * @brief An append bigger than the step still gets enough room: the
 *        string and its terminator, rounded up to the step, at every
 *        growth count from in place (0) to past the step limit (6)
 */
static void
sz_big_cat (void)
{
	sz_text(sz_want, SZ_MAX, 'a');
	for (unsigned c = 0; c <= 6U; ++c) {
		size_t const step = DSTR_BLK_SIZE
		                  << (c < DSTR_GROW_SHIFT_MAX
		                      ? c : DSTR_GROW_SHIFT_MAX);
		size_t const asks[] = {
			step, step + 1U, 2U * step - 1U, 2U * step + 100U,
			3U * step + 255U, 9000,
		};

		for (size_t j = 0; j < sizeof asks / sizeof asks[0]; ++j) {
			struct dstr s = DSTR_INIT;
			size_t len = 0;
			sz_regrow(&s, &len, c);

			/* Every ask is more than the space left */
			HELL_CHECK(asks[j] > dstr_space_left(&s));
			sz_cat_next(&s, len, asks[j]);
			len += asks[j];
			sz_expect(&s, sz_want, len, sz_grown(len, c));
			sz_expect_count(&s, c + 1U);
			HELL_CHECK(dstr_space_left(&s) < step);
			dstr_cleanup(&s);
		}
	}
}

/**
 * @brief Append chunks of every length to strings of every SSO length
 */
static void
sz_chunks (void)
{
	for (size_t a = 0; a < OBJ; ++a) {
		for (size_t n = 1; n <= OBJ + 300U; ++n) {
			struct dstr s = dstr(sz_text(sz_src, a, 'a'));
			memcpy(sz_want, sz_src, a);
			sz_text(sz_src, n, 'A');
			memcpy(sz_want + a, sz_src, n + 1U);

			HELL_EQ_PTR(dstr_cat(&s, sz_src), &s);
			size_t const len = a + n;
			sz_expect(&s, sz_want, len, len < OBJ ? 0U
			          : len / DSTR_BLK_SIZE + 1U);
			if (len >= OBJ)
				sz_expect_count(&s, 1);
			dstr_cleanup(&s);
		}
	}
}

/**
 * @brief Reserves on a heap string with growth count `count`, made by
 *        appends: exact, and the count stays, as the step of the next
 *        growth by an append shows
 */
static void
sz_reserve_counted (unsigned count)
{
	for (size_t j = 0; j < 9; ++j) {
		struct dstr s = DSTR_INIT;
		size_t len = 0;
		sz_regrow(&s, &len, count);

		struct dstr const before = s;
		size_t const space = dstr_space_left(&s);
		size_t const asks[] = {
			0, 1, space - 1U, space, space + 1U, space + 255U,
			space + 256U, space + 257U, 5000,
		};
		size_t const k = asks[j];

		HELL_EQ_PTR(dstr_reserve(&s, k), &s);
		if (k <= space) {
			HELL_EQ_MEM(&s, &before, sizeof s);
		} else {
			sz_expect(&s, sz_want, len,
			          (len + k) / DSTR_BLK_SIZE + 1U);
			sz_expect_count(&s, count);
		}

		size_t const blocks = dstr_get_block_count(&s);
		sz_regrow(&s, &len, 1);
		sz_expect(&s, sz_want, len,
		          sz_grown(blocks * DSTR_BLK_SIZE, count));
		sz_expect_count(&s, count < 15U ? count + 1U : 15U);
		dstr_cleanup(&s);
	}
}

/**
 * @brief reserve() of every size from every SSO length, and from heap
 *        strings: a reserve that fits changes nothing, one that doesn't
 *        allocates exactly (length + k) / 256 + 1 blocks, keeps the
 *        content and leaves the growth count as it was
 */
static void
sz_reserve (void)
{
	for (size_t a = 0; a < OBJ; ++a) {
		for (size_t k = 0; k <= OBJ + 300U; ++k) {
			struct dstr s = dstr(sz_text(sz_src, a, 'a'));
			struct dstr const before = s;

			HELL_EQ_PTR(dstr_reserve(&s, k), &s);
			if (k <= OBJ - 1U - a) {
				HELL_EQ_MEM(&s, &before, sizeof s);
			} else {
				sz_expect(&s, sz_src, a, (a + k) / DSTR_BLK_SIZE + 1U);
				sz_expect_count(&s, 0);
				HELL_CHECK(dstr_space_left(&s) >= k);
			}
			dstr_cleanup(&s);
		}
	}

	/* From the heap: 300 characters in 2 blocks leave 211 bytes */
	for (size_t k = 0; k < 2000; k += 7) {
		struct dstr s = dstr(sz_text(sz_src, 300, 'a'));
		struct dstr const before = s;

		HELL_EQ_PTR(dstr_reserve(&s, k), &s);
		if (k <= 211U) {
			HELL_EQ_MEM(&s, &before, sizeof s);
		} else {
			sz_expect(&s, sz_src, 300,
			          (300U + k) / DSTR_BLK_SIZE + 1U);
			sz_expect_count(&s, 0);
		}
		dstr_cleanup(&s);
	}

	/* From heap strings that appends grew, so that the count isn't 0;
	 * 15 is the limit */
	sz_text(sz_want, SZ_MAX, 'a');
	static unsigned const counts[] = { 1, 2, 3, 5, 15 };
	for (size_t i = 0; i < sizeof counts / sizeof counts[0]; ++i)
		sz_reserve_counted(counts[i]);
}

/**
 * @brief cat_n() and printf() that fill the in-place storage exactly,
 *        and that overflow it by one byte, from every in-place length:
 *        full, the meta byte is the terminator; one more, an exact
 *        block and a growth count of 1. cat_n() appends null characters
 *        among the rest.
 */
static void
sz_fill_sso (void)
{
	for (size_t a = 0; a < OBJ; ++a) {
		for (size_t over = 0; over < 2U; ++over) {
			for (unsigned fmt = 0; fmt < 2U; ++fmt) {
				size_t const n = OBJ - 1U - a + over;
				size_t const len = a + n;
				sz_text(sz_want, len, 'a');
				for (size_t i = a; !fmt && i < len; i += 3U)
					sz_want[i] = '\0';

				struct dstr s = dstr(sz_text(sz_src, a, 'a'));
				HELL_EQ_PTR(sz_append(&s, sz_want + a, n, fmt), &s);
				sz_expect(&s, sz_want, len, over);
				sz_zero_tail(&s, len, over);
				if (over)
					sz_expect_count(&s, 1);
				dstr_cleanup(&s);
			}
		}
	}
}

/**
 * @brief erase() of every span of a full in-place string, and clear()
 *        of it; from past the end, nothing changes
 */
static void
sz_erase_full (void)
{
	sz_text(sz_src, OBJ - 1U, 'a');

	for (size_t pos = 0; pos <= OBJ; ++pos) {
		size_t const rest = pos < OBJ ? OBJ - 1U - pos : 0U;
		size_t const lens[] = { 0, 1, 2, rest, rest + 1U, SIZE_MAX };

		for (size_t j = 0; j < sizeof lens / sizeof lens[0]; ++j) {
			struct dstr s = dstr(sz_src);
			struct dstr const before = s;
			HELL_EQ(s.raw[OBJ - 1U], 0U);
			if (pos == OBJ) {
				HELL_EQ_PTR(dstr_erase(&s, pos, lens[j]), NULL);
				HELL_EQ_MEM(&s, &before, sizeof s);
				continue;
			}

			size_t const n = lens[j] < rest ? lens[j] : rest;
			memcpy(sz_want, sz_src, pos);
			memcpy(sz_want + pos, sz_src + pos + n, rest - n);
			HELL_EQ_PTR(dstr_erase(&s, pos, lens[j]), &s);
			sz_expect(&s, sz_want, OBJ - 1U - n, 0);
			sz_zero_tail(&s, OBJ - 1U - n, 0);
		}
	}

	struct dstr const empty = DSTR_INIT;
	struct dstr s = dstr(sz_src);
	HELL_EQ_PTR(dstr_clear(&s), &s);
	sz_expect(&s, "", 0, 0);
	HELL_EQ_MEM(&s, &empty, sizeof s);
}

/**
 * @brief cat_n() and printf() that fill a heap buffer up to its last
 *        byte, the terminator then, without moving it; erase() of a byte
 *        of the full buffer and a refill; one byte more grows it like
 *        cat() does
 */
static void
sz_heap_fill (void)
{
	static size_t const lens[] = { 255, 256, 300, 511, 1000 };

	for (size_t i = 0; i < sizeof lens / sizeof lens[0]; ++i) {
		for (unsigned fmt = 0; fmt < 2U; ++fmt) {
			size_t len = lens[i];
			size_t const blocks = len / DSTR_BLK_SIZE + 1U;
			size_t const cap = blocks * DSTR_BLK_SIZE;
			sz_text(sz_want, cap + 1U, 'a');
			struct dstr s = dstr(sz_text(sz_src, len, 'a'));
			char const *const buf = dstr_get(&s);

			HELL_EQ_PTR(sz_append(&s, sz_want + len, cap - 1U - len, fmt),
			            &s);
			len = cap - 1U;
			sz_expect(&s, sz_want, len, blocks);
			sz_expect_count(&s, 0);
			HELL_EQ_PTR(dstr_get(&s), buf);

			/* Erase from the middle, and refill */
			size_t const pos = len / 2U;
			memmove(sz_want + pos, sz_want + pos + 1U, cap - pos);
			HELL_EQ_PTR(dstr_erase(&s, pos, 1), &s);
			sz_expect(&s, sz_want, len - 1U, blocks);
			sz_zero_tail(&s, len - 1U, blocks);
			HELL_EQ_PTR(sz_append(&s, sz_want + len - 1U, 1, fmt), &s);
			sz_expect(&s, sz_want, len, blocks);
			sz_expect_count(&s, 0);
			HELL_EQ_PTR(dstr_get(&s), buf);

			HELL_EQ_PTR(sz_append(&s, sz_want + len, 1, fmt), &s);
			++len;
			sz_expect(&s, sz_want, len, blocks + 1U);
			sz_expect_count(&s, 1);
			sz_zero_tail(&s, len, blocks + 1U);
			dstr_cleanup(&s);
		}
	}
}

/**
 * @brief Requests that overflow the length or the block count field
 *        fail without changing the object or allocating
 */
static void
sz_overflow (void)
{
	static size_t const lens[] = { 0, 3, OBJ - 1U, 300, 5000 };
	size_t const huge = DSTR_BLK_COUNT_MASK * DSTR_BLK_SIZE;

	for (size_t i = 0; i < sizeof lens / sizeof lens[0]; ++i) {
		size_t const len = lens[i];
		struct dstr s = dstr(sz_text(sz_src, len, 'a'));
		struct dstr const before = s;
		size_t const asks[] = {
			SIZE_MAX, SIZE_MAX - len, SIZE_MAX - len + 1U,
			huge, huge - len,
		};

		for (size_t j = 0; j < sizeof asks / sizeof asks[0]; ++j) {
			/* reserve(0) is a no-op; SIZE_MAX - 0 + 1 wraps to 0 */
			if (!asks[j])
				continue;
			HELL_EQ_PTR(dstr_reserve(&s, asks[j]), NULL);
			HELL_EQ_MEM(&s, &before, sizeof s);
		}

		/* Still usable */
		HELL_EQ_PTR(dstr_cat(&s, "z"), &s);
		HELL_EQ(dstr_len(&s), len + 1U);
		dstr_cleanup(&s);
	}
}

/**
 * @brief Check what dstr() made of a string literal of `len`
 *        characters against the expected content and, for an in-place
 *        string, byte for byte against the dstr() function
 */
static void
sz_literal (struct dstr  s,
            size_t       len,
            char const  *lit)
{
	struct dstr f = (dstr)(lit);
	size_t const blocks = len < OBJ ? 0U : len / DSTR_BLK_SIZE + 1U;

	sz_text(sz_want, len, 'a');
	sz_expect(&s, sz_want, len, blocks);
	if (!blocks)
		HELL_EQ_MEM(&s, &f, sizeof s);
	else
		sz_expect_count(&s, 0);
	dstr_cleanup(&f);
	dstr_cleanup(&s);
}

/**
 * @brief dstr() of string literals of every length from 0 to 300:
 *        constants up to the in-place limit, allocations beyond it
 */
static void
sz_literals (void)
{
#define LITERAL(n, lit) sz_literal(dstr(lit), n, lit);
#include "literals.inc"
#undef LITERAL

	/* A literal with an embedded null character is a string up to the
	 * first one, exactly as the function makes it */
	static char const *const nul[] = { "ab\0cd", "\0", "x\0", "" };
	struct dstr m[] = {
		dstr("ab\0cd"), dstr("\0"), dstr("x\0"),
		dstr(""),
	};
	for (size_t i = 0; i < sizeof m / sizeof m[0]; ++i) {
		struct dstr f = (dstr)(nul[i]);
		HELL_EQ(dstr_len(&m[i]), strlen(nul[i]));
		HELL_EQ_MEM(&m[i], &f, sizeof f);
		dstr_cleanup(&f);
		dstr_cleanup(&m[i]);
	}
}

/**
 * @brief dstr() of a `char` array filled with strings of every
 *        length that fits; only the string may be copied, never the
 *        bytes after its terminator
 */
#define SZ_ARRAY(arr) \
  for (size_t k = 0; k < sizeof (arr); ++k) { \
    memset(arr, 'Z', sizeof (arr)); \
    memcpy(arr, sz_text(sz_src, k, 'a'), k + 1U); \
    struct dstr s = dstr(arr); \
    size_t const blocks = k < OBJ ? 0U : k / DSTR_BLK_SIZE + 1U; \
    sz_expect(&s, sz_src, k, blocks); \
    if (!blocks) { \
      size_t nonzero = 0; \
      for (size_t i = k; i < OBJ - 1U; ++i) \
        nonzero += s.raw[i] != 0U; \
      HELL_EQ(nonzero, 0U); \
    } \
    dstr_cleanup(&s); \
  }

/**
 * @brief dstr() of arrays below, at and above the object size
 */
static void
sz_arrays (void)
{
	char below[OBJ - 1U];
	char exact[OBJ];
	char above[OBJ + 1U];
	char big[3U * DSTR_BLK_SIZE];

	SZ_ARRAY(below);
	SZ_ARRAY(exact);
	SZ_ARRAY(above);
	SZ_ARRAY(big);

	static char const konst[] = "konst";
	struct dstr s = dstr(konst);
	sz_expect(&s, "konst", 5, 0);
	dstr_cleanup(&s);
}
#undef SZ_ARRAY

/**
 * @brief Run every check for this object size
 */
void
SUITE (struct hell_totals *t)
{
	(void)fprintf(stderr, "object size %zu\n", OBJ);
	sz_init();
	sz_bytewise();
	for (sz_mode = SZ_CAT; sz_mode < SZ_MODES; ++sz_mode) {
		sz_fill();
		sz_constructed();
		sz_big_cat();
	}
	sz_mode = SZ_CAT;
	sz_chunks();
	sz_fill_sso();
	sz_erase_full();
	sz_heap_fill();
	sz_reserve();
	sz_overflow();
	sz_literals();
	sz_arrays();
	hell_collect(t);
}
