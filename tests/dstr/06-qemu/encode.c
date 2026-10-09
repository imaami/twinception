/**
 * @file encode.c
 * @brief The endian-dependent encoding, checked against the byte order
 *        found at run time (trial 06)
 *
 * A heap string keeps its block count in the last uintptr_t of the
 * object, shifted by DSTR_BLK_COUNT_POS, and its meta byte in the
 * object's last byte: the heap flag in the top bit, and in the bits of
 * DSTR_GROW_COUNT_MASK how many times the appends (dstr_cat(),
 * dstr_cat_n() and dstr_printf()) have grown the buffer. That byte is
 * the most significant byte of the last uintptr_t on a little-endian
 * machine and the least significant one on a big-endian machine, so
 * the shift must be 0 on little-endian and CHAR_BIT on big-endian, and
 * the count bits must stay out of the block count. Compiled for every
 * object size like the other per-size suites.
 */
#include <limits.h>
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

/**
 * @brief Check that the last uintptr_t of the heap string `s` holds
 *        exactly `blocks` at DSTR_BLK_COUNT_POS and the meta byte
 *        `meta` at `meta_pos`, and that the API reads the block count
 *        back
 */
static void
en_word (struct dstr const *s,
         size_t             blocks,
         unsigned char      meta,
         size_t             meta_pos)
{
	uintptr_t last;
	memcpy(&last, &s->raw[OBJ - sizeof last], sizeof last);

	HELL_EQ(s->raw[OBJ - 1U], meta);
	HELL_EQ((last >> meta_pos) & UCHAR_MAX, meta);
	HELL_EQ((last >> DSTR_BLK_COUNT_POS) & DSTR_BLK_COUNT_MASK,
	        blocks);
	HELL_EQ(last, ((uintptr_t)blocks << DSTR_BLK_COUNT_POS) |
	              ((uintptr_t)meta << meta_pos));

	struct dstr_info const info = dstr_info(s);
	HELL_EQ(info.state, DSTR_IS_PTR);
	HELL_EQ(info.blocks, blocks);
	HELL_EQ(dstr_get_block_count(s), blocks);
	HELL_EQ(dstr_space_left(s),
	        blocks * DSTR_BLK_SIZE - 1U - dstr_len(s));
}

/**
 * @brief The meta byte after `k` growths by dstr_cat(): the flag,
 *        and the count, which stops at its mask
 */
static unsigned char
en_meta (size_t k)
{
	return (unsigned char)(DSTR_PTR_FLAG |
	                       (k < DSTR_GROW_COUNT_MASK
	                        ? k : DSTR_GROW_COUNT_MASK));
}

/**
 * @brief Run the encoding checks for this object size
 */
void
SUITE (struct hell_totals *t)
{
	union {
		uint32_t      u;
		unsigned char b[4];
	} const probe = { .u = UINT32_C(0x01020304) };
	bool const big = probe.b[0] == 0x01U;
	size_t const meta_pos = big ? 0U : (sizeof (uintptr_t) - 1U) * CHAR_BIT;

	(void)fprintf(stderr, "object size %zu, %s-endian, uintptr_t %zu\n",
	              OBJ, big ? "big" : "little", sizeof (uintptr_t));
	HELL_CHECK(big || probe.b[0] == 0x04U);
	HELL_EQ(DSTR_BLK_COUNT_POS, big ? (size_t)CHAR_BIT : 0U);
	HELL_EQ(DSTR_UPTR_META_POS, meta_pos);

	for (size_t blocks = 1; blocks <= 300; ++blocks) {
		struct dstr s = DSTR_INIT;
		HELL_EQ_PTR(dstr_reserve(&s, blocks * DSTR_BLK_SIZE - 1U),
		            &s);
		HELL_EQ(dstr_get_block_count(&s), blocks);
		HELL_EQ(dstr_info(&s).state, DSTR_IS_PTR);

		/* The heap flag is the top bit of the object's last byte */
		HELL_EQ(s.raw[OBJ - 1U] & DSTR_PTR_FLAG, DSTR_PTR_FLAG);

		/* The block count is in the last uintptr_t at the expected
		 * shift */
		uintptr_t last;
		memcpy(&last, &s.raw[OBJ - sizeof last], sizeof last);
		HELL_EQ((last >> DSTR_BLK_COUNT_POS) & DSTR_BLK_COUNT_MASK,
		        blocks);

		/* And the flag byte holds nothing but the flag: reserve()
		 * doesn't count its growths */
		uintptr_t const flag_byte = big ? last & UCHAR_MAX
		                          : last >> (sizeof last - 1U) * CHAR_BIT;
		HELL_EQ(flag_byte, DSTR_PTR_FLAG);
		en_word(&s, blocks, DSTR_PTR_FLAG, meta_pos);

		/* Appending what fits keeps the encoding as it is */
		HELL_EQ_PTR(dstr_cat(&s, "x"), &s);
		HELL_EQ(dstr_len(&s), 1U);
		en_word(&s, blocks, DSTR_PTR_FLAG, meta_pos);
		dstr_cleanup(&s);
	}

	/* Byte-wise appends: after the kth growth the meta byte is the flag
	 * and min(k, 15), and the rest of the word is the block count of a
	 * buffer of 256, 512, 1024, 2048, 4096, 8192, 12288... bytes */
	struct dstr s = DSTR_INIT;
	size_t growths = 0;
	while (growths < 18U && dstr_len(&s) < ((size_t)1 << 20)) {
		bool const grows = !dstr_space_left(&s);
		struct dstr *const r = dstr_cat(&s, "x");
		if (r != &s) {
			HELL_EQ_PTR(r, &s);
			break;
		}
		if (!grows)
			continue;
		++growths;
		size_t const m = DSTR_GROW_SHIFT_MAX;
		size_t const blocks = growths <= m + 1U
		                    ? (size_t)1 << (growths - 1U)
		                    : ((size_t)1 << m) * (growths - m);
		en_word(&s, blocks, en_meta(growths), meta_pos);
	}
	HELL_EQ(growths, 18U);

	/* reserve() keeps the count, saturated by now */
	size_t const len = dstr_len(&s);
	HELL_EQ_PTR(dstr_reserve(&s, 100000U), &s);
	en_word(&s, ((len + 100000U) >> CHAR_BIT) + 1U, en_meta(growths),
	        meta_pos);
	dstr_cleanup(&s);

	/* Every count with block counts across the whole field, in forged
	 * objects that are only read: the field and the count never mix */
	static unsigned char buf[8];
	uintptr_t const counts[] = {
		1U, 2U, UCHAR_MAX, (uintptr_t)UCHAR_MAX + 1U,
		DSTR_BLK_COUNT_MASK >> 1,
		(DSTR_BLK_COUNT_MASK >> 1) + 1U,
		DSTR_BLK_COUNT_MASK - 1U,
		DSTR_BLK_COUNT_MASK,
	};
	for (size_t k = 0; k <= DSTR_GROW_COUNT_MASK; ++k) {
		for (size_t i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
			struct dstr f;
			memset(&f, 0, sizeof f);
			f.ptr[0] = (uintptr_t)&buf[0];
			f.ptr[1] = (uintptr_t)&buf[5];
			f.ptr[OBJ / sizeof f.ptr[0] - 1U] =
				(counts[i] << DSTR_BLK_COUNT_POS) |
				((uintptr_t)en_meta(k) << meta_pos);
			HELL_EQ(dstr_len(&f), 5U);
			en_word(&f, counts[i], en_meta(k), meta_pos);
		}
	}
	hell_collect(t);
}
