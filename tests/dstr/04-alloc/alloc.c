/**
 * @file alloc.c
 * @brief Every allocation failing in turn (trial 04)
 *
 * Each case first runs its operation without failures to count the
 * allocation calls it makes, then once more for every one of those
 * calls failing. A failed operation must report the failure the way
 * the docs say, leave the object byte-for-byte unchanged where nothing
 * is promised to happen, and leave it usable. The allocation shim
 * (tests/dstr/shim.h) counts live blocks, so leaks and bad frees show
 * up.
 *
 * A heap string's meta byte (its last byte) holds the heap flag and,
 * in the bits of DSTR_GROW_COUNT_MASK, how many times the appends
 * (dstr_cat(), dstr_cat_n() and dstr_printf()) have grown its buffer.
 * The cases check that count too: a failed growth leaves it alone like
 * the rest of the object. The cases of dstr_cat() run with dstr_cat_n()
 * and dstr_printf() as well, which grow the same way. The white-box
 * part calls the internal functions behind dstr_reserve(), dstr_cat()
 * and dstr_printf() directly.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dstr.h"
#include "hell.h"
#include "shim.h"

/** Size of the object type under test */
#define OBJ (sizeof (struct dstr))

/** Buffer behind al_text() */
static char al_buf[4096];

/**
 * @brief A string of `n` letters from `first` on, valid until the next
 *        call
 */
static char const *
al_text (size_t n,
         char   first)
{
	for (size_t i = 0; i < n; ++i)
		al_buf[i] = (char)(first + (char)(i % 26U));
	al_buf[n] = '\0';
	return al_buf;
}

/**
 * @brief Start counting calls; the `k`th one fails with `err` (k = 0:
 *        none fails)
 */
static void
al_arm (size_t k,
        int    err)
{
	shim_reset();
	shim_fail_at(k, err);
}

/**
 * @brief Stop injecting failures
 *
 * @return Whether the injected failure happened
 */
static bool
al_disarm (size_t  k,
           size_t *calls)
{
	*calls = shim_calls();
	shim_reset();
	return k && *calls >= k;
}

/**
 * @brief Check that `s` still works: append and compare
 */
static void
al_usable (struct dstr *s)
{
	size_t const len = dstr_len(s);
	HELL_EQ_PTR(dstr_cat(s, "ok"), s);
	HELL_EQ(dstr_len(s), len + 2U);
	HELL_EQ_STR(dstr_get(s) + len, "ok");
}

/** How al_cat() appends */
static enum {
	AL_CAT,
	AL_CAT_N,
	AL_PRINTF,
	AL_MODES
} al_mode;

/**
 * @brief Append the `n` characters of the string `src` to `s` the way
 *        al_mode says
 */
static struct dstr *
al_cat (struct dstr *s,
        char const  *src,
        size_t       n)
{
	switch (al_mode) {
	case AL_CAT_N:  return dstr_cat_n(s, src, n);
	case AL_PRINTF: return dstr_printf(s, "%s", src);
	default:        return dstr_cat(s, src);
	}
}

/**
 * @brief The meta byte of a heap string grown `c` times by cat():
 *        the flag and the count, which stops at its mask
 */
static unsigned char
al_meta (size_t c)
{
	return (unsigned char)(DSTR_PTR_FLAG |
	                       (c < DSTR_GROW_COUNT_MASK
	                        ? c : DSTR_GROW_COUNT_MASK));
}

/**
 * @brief Set the growth count of the heap string `s` directly
 */
static void
al_set_count (struct dstr *s,
              size_t       c)
{
	HELL_EQ(dstr_info(s).state, DSTR_IS_PTR);
	s->raw[OBJ - 1U] = al_meta(c);
}

/**
 * @brief Expected block count of a buffer for `n` characters grown in
 *        steps of 2^w blocks: the smallest multiple of the step that
 *        holds them and the terminator
 */
static size_t
al_blocks (size_t n,
           size_t w)
{
	size_t const step = (size_t)1 << w;
	size_t b = step;
	while (b * DSTR_BLK_SIZE < n + 1U)
		b += step;
	return b;
}

/**
 * @brief Expected buffer size after the `k`th growth by byte-wise
 *        appends: 256 bytes, doubling up to a step of 256 shifted by
 *        DSTR_GROW_SHIFT_MAX, then growing by that step
 */
static size_t
al_cap (size_t k)
{
	size_t const m = DSTR_GROW_SHIFT_MAX;
	return k <= m + 1U ? DSTR_BLK_SIZE << (k - 1U)
	                   : (DSTR_BLK_SIZE << m) * (k - m);
}

/**
 * @brief Append to `s` up to the last byte of its buffer, which must
 *        not allocate
 */
static void
al_fill (struct dstr *s)
{
	shim_reset();
	for (size_t space; (space = dstr_space_left(s)) > 0U; ) {
		struct dstr *const r =
			dstr_cat(s, al_text(space < 4000U ? space : 4000U, 'f'));
		HELL_EQ_PTR(r, s);
		if (r != s)
			break;
	}
	HELL_EQ(shim_calls(), 0U);
}

/**
 * @brief Grow the in-place string `s` `c` times with appends, filling
 *        the buffer before each growth
 */
static void
al_grow_to (struct dstr *s,
            size_t       c)
{
	for (size_t i = 0; i < c; ++i) {
		al_fill(s);
		HELL_EQ_PTR(dstr_cat(s, "x"), s);
	}
	HELL_EQ(s->raw[OBJ - 1U], al_meta(c));
}

/**
 * @brief Run the case `fn` once without failures, then with each of
 *        its allocation calls failing; no blocks may leak
 */
#define AL_EVERY(fn) \
  do { \
    size_t const al_live_ = shim_live(); \
    size_t const al_n_ = fn(0); \
    HELL_CHECK(al_n_ > 0U); \
    for (size_t al_k_ = 1; al_k_ <= al_n_; ++al_k_) \
      (void)fn(al_k_); \
    HELL_EQ(shim_live(), al_live_); \
    HELL_EQ(shim_bad(), 0U); \
    (void)fprintf(stderr, "%s: %zu allocation calls\n", #fn, al_n_); \
  } while (0)

/**
 * @brief dstr() of a heap string: on failure a valid empty string
 */
static size_t
al_init (size_t k)
{
	size_t calls;
	al_arm(k, ENOMEM);
	struct dstr s = dstr(al_text(300, 'a'));
	if (al_disarm(k, &calls)) {
		HELL_EQ(dstr_len(&s), 0U);
		HELL_EQ_STR(dstr_get(&s), "");
		HELL_EQ(dstr_info(&s).state, DSTR_IS_SSO);
	} else {
		HELL_EQ(dstr_len(&s), 300U);
		HELL_EQ_STR(dstr_get(&s), al_text(300, 'a'));

		/* (len >> 8) + 1 blocks, and no growths counted */
		HELL_EQ(dstr_get_block_count(&s), 2U);
		HELL_EQ(s.raw[OBJ - 1U], DSTR_PTR_FLAG);
	}
	al_usable(&s);
	dstr_cleanup(&s);
	return calls;
}

/**
 * @brief Check the result of a cat() or reserve() of `s` that may have
 *        failed: NULL and no change, or success with `len` characters
 *        in `blocks` blocks and the growth count `count`
 */
static void
al_outcome (bool               hit,
            struct dstr       *s,
            struct dstr       *r,
            struct dstr const *before,
            size_t             len,
            size_t             blocks,
            size_t             count)
{
	if (hit) {
		HELL_EQ_PTR(r, NULL);
		HELL_EQ_MEM(s, before, sizeof *s);
	} else {
		HELL_EQ_PTR(r, s);
		HELL_EQ(dstr_len(s), len);
		HELL_EQ(dstr_get_block_count(s), blocks);
		HELL_EQ(s->raw[OBJ - 1U], al_meta(count));
	}
	al_usable(s);
}

/**
 * @brief cat() that moves an SSO string to the heap
 */
static size_t
al_cat_sso (size_t k)
{
	size_t calls;
	struct dstr s = dstr("abc");
	struct dstr const before = s;
	al_arm(k, ENOMEM);
	struct dstr *const r = al_cat(&s, al_text(300, 'A'), 300);
	al_outcome(al_disarm(k, &calls), &s, r, &before, 303, 2, 1);
	dstr_cleanup(&s);
	return calls;
}

/**
 * @brief cat() that grows a heap string
 */
static size_t
al_cat_heap (size_t k)
{
	size_t calls;
	struct dstr s = dstr(al_text(300, 'a'));
	struct dstr const before = s;
	al_arm(k, ENOMEM);
	struct dstr *const r = al_cat(&s, al_text(1000, 'A'), 1000);
	al_outcome(al_disarm(k, &calls), &s, r, &before, 1300, 6, 1);
	dstr_cleanup(&s);
	return calls;
}

/**
 * @brief cat() that grows a heap string cat() has grown before: the
 *        step depends on the count, which a failure leaves alone and a
 *        success raises, up to its mask
 */
static size_t
al_cat_counted (size_t k)
{
	size_t const counts[] = {
		1, DSTR_GROW_SHIFT_MAX, DSTR_GROW_SHIFT_MAX + 1U,
		DSTR_GROW_COUNT_MASK - 1U, DSTR_GROW_COUNT_MASK
	};
	size_t most = 0;

	for (size_t i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
		size_t const c = counts[i];
		size_t const w = c < DSTR_GROW_SHIFT_MAX
		               ? c : DSTR_GROW_SHIFT_MAX;
		struct dstr s = DSTR_INIT;
		al_grow_to(&s, c);
		al_fill(&s);

		size_t calls;
		size_t const len = dstr_len(&s) + 300U;
		struct dstr const before = s;
		al_arm(k, ENOMEM);
		struct dstr *const r = al_cat(&s, al_text(300, 'A'), 300);
		al_outcome(al_disarm(k, &calls), &s, r, &before, len,
		           al_blocks(len, w), c + 1U);
		dstr_cleanup(&s);
		if (calls > most)
			most = calls;
	}
	return most;
}

/**
 * @brief reserve() that moves an SSO string to the heap
 */
static size_t
al_reserve_sso (size_t k)
{
	size_t calls;
	struct dstr s = dstr("abc");
	struct dstr const before = s;
	al_arm(k, ENOMEM);
	struct dstr *const r = dstr_reserve(&s, 1000);
	al_outcome(al_disarm(k, &calls), &s, r, &before, 3, 4, 0);
	dstr_cleanup(&s);
	return calls;
}

/**
 * @brief reserve() that grows a heap string
 */
static size_t
al_reserve_heap (size_t k)
{
	size_t calls;
	struct dstr s = dstr(al_text(300, 'a'));
	struct dstr const before = s;
	al_arm(k, ENOMEM);
	struct dstr *const r = dstr_reserve(&s, 5000);
	al_outcome(al_disarm(k, &calls), &s, r, &before, 300, 21, 0);
	dstr_cleanup(&s);
	return calls;
}

/**
 * @brief reserve() that grows a heap string cat() has grown before:
 *        exact either way, and the count stays
 */
static size_t
al_reserve_counted (size_t k)
{
	size_t const counts[] = { 1, DSTR_GROW_COUNT_MASK };
	size_t most = 0;

	for (size_t i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
		struct dstr s = DSTR_INIT;
		al_grow_to(&s, counts[i]);
		al_fill(&s);

		size_t calls;
		size_t const len = dstr_len(&s);
		struct dstr const before = s;
		al_arm(k, ENOMEM);
		struct dstr *const r = dstr_reserve(&s, 1000);
		al_outcome(al_disarm(k, &calls), &s, r, &before, len,
		           al_blocks(len + 1000U, 0), counts[i]);
		dstr_cleanup(&s);
		if (calls > most)
			most = calls;
	}
	return most;
}

/**
 * @brief Byte-wise appends: the buffer grows to 256, 512, 1024, 2048
 *        and 4096 bytes, then by 4096 bytes at a time, with one
 *        allocation call per growth; appends that fit, up to the last
 *        byte, make none; the count stops at its mask
 */
static void
al_appends (void)
{
	struct dstr s = DSTR_INIT;
	size_t growths = 0;

	while (growths < 20U && dstr_len(&s) < ((size_t)1 << 20)) {
		bool const grows = !dstr_space_left(&s);
		shim_reset();
		struct dstr *const r = dstr_cat(&s, "x");
		HELL_EQ_PTR(r, &s);
		if (r != &s)
			break;
		HELL_EQ(shim_calls(), grows ? 1U : 0U);
		if (!grows)
			continue;
		++growths;
		HELL_EQ(dstr_get_block_count(&s) * DSTR_BLK_SIZE,
		        al_cap(growths));
		HELL_EQ(s.raw[OBJ - 1U], al_meta(growths));
	}
	HELL_EQ(growths, 20U);
	HELL_EQ(dstr_len(&s), al_cap(19));

	/* reserve() grows exactly and leaves the count alone */
	size_t const len = dstr_len(&s);
	HELL_EQ_PTR(dstr_reserve(&s, 100000), &s);
	HELL_EQ(dstr_get_block_count(&s), al_blocks(len + 100000U, 0));
	HELL_EQ(s.raw[OBJ - 1U], al_meta(DSTR_GROW_COUNT_MASK));

	char const *const str = dstr_get(&s);
	size_t xs = 0;
	while (str[xs] == 'x')
		++xs;
	HELL_EQ(xs, len);
	HELL_EQ(dstr_len(&s), len);
	dstr_cleanup(&s);
}

/* White-box part: the internal functions, declared like the block-scope
 * declaration of dstr_grow_() in dstr.h */
extern int
dstr_resize_ (size_t        obj_size,
              unsigned char obj_raw[obj_size],
              size_t        n_blocks);

extern int
dstr_grow_ (size_t        obj_size,
            unsigned char obj_raw[obj_size],
            size_t        len,
            bool          exact);

extern int
dstr_vprintf_ (size_t         obj_size,
               unsigned char  obj_raw[obj_size],
               char const    *fmt,
               va_list        ap) __attribute__((format(printf, 3, 0)));

/**
 * @brief dstr_vprintf_() of `s` with a variable argument list
 */
__attribute__((format(printf, 2, 3))) static int
al_vprintf (struct dstr *s,
            char const  *fmt,
            ...)
{
	va_list ap;
	va_start(ap, fmt);
	int const rc = dstr_vprintf_(sizeof *s, s->raw, fmt, ap);
	va_end(ap);
	return rc;
}

/**
 * @brief dstr_resize_(), dstr_grow_() and dstr_vprintf_() return
 *        the allocator's errno, or ENOMEM if the allocator set none, and
 *        leave the object unchanged
 */
static void
al_errno (void)
{
	static int const errs[] = { ENOMEM, EAGAIN, 0 };
	static size_t const lens[] = { 3, 300 };

	for (size_t i = 0; i < sizeof lens / sizeof lens[0]; ++i) {
		for (size_t j = 0; j < sizeof errs / sizeof errs[0]; ++j) {
			/* resize_(), exact grow_(), counted grow_(), vprintf_() */
			for (unsigned f = 0; f < 4U; ++f) {
				struct dstr s = dstr(al_text(lens[i], 'a'));
				if (dstr_info(&s).state == DSTR_IS_PTR)
					al_set_count(&s, 9);
				struct dstr const before = s;
				size_t calls;
				errno = 0;
				al_arm(1, errs[j]);
				int const rc = !f
				             ? dstr_resize_(sizeof s, s.raw, 10)
				             : f < 3U
				             ? dstr_grow_(sizeof s, s.raw, 5000,
				                          f == 1U)
				             : al_vprintf(&s, "%s%u", al_text(3000, 'A'),
				                          7U);
				HELL_CHECK(al_disarm(1, &calls));
				HELL_EQ(calls, 1U);
				HELL_EQ(rc, errs[j] ? errs[j] : ENOMEM);
				HELL_EQ_MEM(&s, &before, sizeof s);
				dstr_cleanup(&s);
			}
		}
	}
}

/**
 * @brief dstr_vprintf_() whose arguments change between the run that
 *        measures and the run that writes, which dstr_printf() rules
 *        out: EILSEQ, and a valid object with the content it had, in
 *        the buffer that grew for the text
 *
 * The argument is the in-place string itself. Its growth to the heap
 * overwrites it with the buffer pointers and the block count, and the
 * string that they make ends before the block count's zero bytes, short
 * of what was measured.
 */
static void
al_vprintf_changed (void)
{
	char want[OBJ];
	memcpy(want, al_text(OBJ - 1U, 'a'), OBJ);
	struct dstr s = dstr(want);
	HELL_EQ(al_vprintf(&s, "%s", s.str), EILSEQ);

	struct dstr_info const info = dstr_info(&s);
	HELL_EQ(info.state, DSTR_IS_PTR);
	HELL_EQ(info.blocks, 1U);
	HELL_EQ(info.length, OBJ - 1U);
	HELL_EQ(s.raw[OBJ - 1U], al_meta(1));
	HELL_EQ_STR(dstr_get(&s), want);

	char const *const str = dstr_get(&s);
	size_t nonzero = 0;
	for (size_t i = OBJ - 1U; i < DSTR_BLK_SIZE; ++i)
		nonzero += str[i] != 0;
	HELL_EQ(nonzero, 0U);
	al_usable(&s);
	dstr_cleanup(&s);
}

#if __STDC_HOSTED__
/* The library's vsnprintf() calls go through this wrapper, linked with
 * -Wl,--wrap=vsnprintf */
extern int
__real_vsnprintf (char       *buf,
                  size_t      size,
                  char const *fmt,
                  va_list     ap);

extern int
__wrap_vsnprintf (char       *buf,
                  size_t      size,
                  char const *fmt,
                  va_list     ap);

/** The vsnprintf() call that fails, armed by al_vsnprintf_fails() */
static struct {
	unsigned at;     /**< Calls until the one that fails, or 0 */
	bool     longer; /**< It tells one byte more, instead of -1 */
} al_vs;

int
__wrap_vsnprintf (char       *buf,
                  size_t      size,
                  char const *fmt,
                  va_list     ap)
{
	int const n = __real_vsnprintf(buf, size, fmt, ap);
	if (!al_vs.at || --al_vs.at)
		return n;
	if (al_vs.longer)
		return n + 1;
	/* An errno the library never sets itself, so a case can't pass
	 * on a failure of the library's own */
	errno = EIO;
	return -1;
}

/**
 * @brief printf() of `n` characters to a string of `len`, whose
 *        vsnprintf() fails on call `at` (1 measures, 2 writes) after
 *        writing what it would, or tells one byte more if `longer`, as if
 *        the arguments changed; through dstr_printf() if `pub`, else
 *        through the internal function, which returns the errno of the
 *        failure, or EILSEQ for the wrong length
 *
 * The object must come out with the content it had and its zero tail.
 * Without a growth, it must be unchanged byte for byte, the meta byte
 * included, even when a terminator was written over it. With one, the
 * object must point to the grown buffer.
 */
static void
al_vsnprintf_case (size_t   len,
                   size_t   n,
                   unsigned at,
                   bool     longer,
                   bool     pub)
{
	char want[1500];
	memcpy(want, al_text(len, 'a'), len + 1U);
	struct dstr s = dstr(want);
	struct dstr const before = s;
	size_t const m = n + (at == 1U && longer);
	bool const grows = (at == 2U || longer) && m > dstr_space_left(&s);
	size_t calls;

	al_arm(0, 0);
	al_vs.at = at;
	al_vs.longer = longer;
	char const *const text = al_text(n, 'A');
	if (pub)
		HELL_EQ_PTR(dstr_printf(&s, "%s", text), NULL);
	else
		HELL_EQ(al_vprintf(&s, "%s", text), longer ? EILSEQ : EIO);
	HELL_EQ(al_vs.at, 0U);
	al_vs.at = 0;
	(void)al_disarm(0, &calls);
	HELL_EQ(calls, (size_t)grows);

	if (!grows) {
		HELL_EQ_MEM(&s, &before, sizeof s);
	} else {
		HELL_EQ(dstr_info(&s).state, DSTR_IS_PTR);
		HELL_EQ(dstr_get_block_count(&s), al_blocks(len + m, 0));
		HELL_EQ(s.raw[OBJ - 1U], al_meta(1));
	}

	struct dstr_info const info = dstr_info(&s);
	size_t const end = info.blocks ? info.blocks * DSTR_BLK_SIZE : OBJ - 1U;
	char const *const str = dstr_get(&s);
	HELL_EQ(info.length, len);
	HELL_EQ_MEM(str, want, len);
	size_t nonzero = 0;
	for (size_t k = len; k < end; ++k)
		nonzero += str[k] != 0;
	HELL_EQ(nonzero, 0U);
	al_usable(&s);
	dstr_cleanup(&s);
}

/**
 * @brief al_vsnprintf_case() in place and on the heap, with room to
 *        spare, filling the buffer exactly, and growing it
 */
static void
al_vsnprintf_fails (void)
{
	static size_t const cases[][2] = {
		/* Length, then the length of the text */
		{ 3, 5 }, { 3, OBJ - 4U }, { 3, 300 },
		{ 300, 5 }, { 300, 211 }, { 300, 1000 },
	};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		for (unsigned v = 0; v < 8U; ++v)
			al_vsnprintf_case(cases[i][0], cases[i][1], 1U + (v & 1U),
			                  v & 2U, v & 4U);
	}
}
#endif /* __STDC_HOSTED__ */

/**
 * @brief A shrinking realloc() that fails is not fatal: the string is
 *        truncated if it must be, and the buffer keeps its size and its
 *        growth count
 */
static void
al_shrink (void)
{
	size_t calls;

	/* 1000 characters in 4 blocks, shrunk to 2: truncated to 511 */
	struct dstr s = dstr(al_text(1000, 'a'));
	HELL_EQ(dstr_get_block_count(&s), 4U);
	al_set_count(&s, 7);
	al_arm(1, ENOMEM);
	HELL_EQ(dstr_resize_(sizeof s, s.raw, 2), 0);
	HELL_CHECK(al_disarm(1, &calls));
	HELL_EQ(dstr_get_block_count(&s), 4U);
	HELL_EQ(s.raw[OBJ - 1U], al_meta(7));
	HELL_EQ(dstr_len(&s), 511U);
	HELL_EQ(strlen(dstr_get(&s)), 511U);
	HELL_EQ_STR(dstr_get(&s), al_text(511, 'a'));
	HELL_EQ(dstr_space_left(&s), 4U * 256U - 1U - 511U);
	al_usable(&s);
	dstr_cleanup(&s);

	/* No truncation needed: nothing changes */
	s = dstr(al_text(300, 'a'));
	HELL_EQ_PTR(dstr_reserve(&s, 1000), &s);
	al_set_count(&s, 3);
	size_t const blocks = dstr_get_block_count(&s);
	struct dstr const before = s;
	al_arm(1, ENOMEM);
	HELL_EQ(dstr_resize_(sizeof s, s.raw, 3), 0);
	HELL_CHECK(al_disarm(1, &calls));
	HELL_EQ_MEM(&s, &before, sizeof s);
	HELL_EQ(dstr_get_block_count(&s), blocks);

	/* Without failures it shrinks, and to 0 blocks it returns to SSO */
	HELL_EQ(dstr_resize_(sizeof s, s.raw, 2), 0);
	HELL_EQ(dstr_get_block_count(&s), 2U);
	HELL_EQ(s.raw[OBJ - 1U], al_meta(3));
	HELL_EQ(dstr_len(&s), 300U);
	HELL_EQ(dstr_resize_(sizeof s, s.raw, 0), 0);
	HELL_EQ(dstr_info(&s).state, DSTR_IS_SSO);
	HELL_EQ(dstr_len(&s), OBJ - 1U);
	HELL_EQ_STR(dstr_get(&s), al_text(OBJ - 1U, 'a'));
	dstr_cleanup(&s);
}

/**
 * @brief dstr_resize_() keeps the growth count of a heap buffer it
 *        grows or shrinks, drops it when the string returns in place,
 *        and starts at 0 when a string leaves its place
 */
static void
al_resize_count (void)
{
	for (size_t c = 0; c <= DSTR_GROW_COUNT_MASK; ++c) {
		struct dstr s = dstr(al_text(300, 'a'));
		al_set_count(&s, c);

		/* Grow, shrink, shrink with truncation */
		HELL_EQ(dstr_resize_(sizeof s, s.raw, 9), 0);
		HELL_EQ(dstr_get_block_count(&s), 9U);
		HELL_EQ(s.raw[OBJ - 1U], al_meta(c));
		HELL_EQ_STR(dstr_get(&s), al_text(300, 'a'));
		HELL_EQ(dstr_resize_(sizeof s, s.raw, 3), 0);
		HELL_EQ(dstr_get_block_count(&s), 3U);
		HELL_EQ(s.raw[OBJ - 1U], al_meta(c));
		HELL_EQ(dstr_resize_(sizeof s, s.raw, 1), 0);
		HELL_EQ(dstr_get_block_count(&s), 1U);
		HELL_EQ(s.raw[OBJ - 1U], al_meta(c));
		HELL_EQ_STR(dstr_get(&s), al_text(255, 'a'));

		/* The same size: nothing happens, nothing is allocated */
		struct dstr const before = s;
		shim_reset();
		HELL_EQ(dstr_resize_(sizeof s, s.raw, 1), 0);
		HELL_EQ(shim_calls(), 0U);
		HELL_EQ_MEM(&s, &before, sizeof s);

		/* In place, the meta byte is the space left again */
		HELL_EQ(dstr_resize_(sizeof s, s.raw, 0), 0);
		HELL_EQ(dstr_info(&s).state, DSTR_IS_SSO);
		HELL_EQ(s.raw[OBJ - 1U], 0U);
		HELL_EQ_STR(dstr_get(&s), al_text(OBJ - 1U, 'a'));

		/* And out of place again, the count starts over */
		HELL_EQ(dstr_resize_(sizeof s, s.raw, 2), 0);
		HELL_EQ(dstr_get_block_count(&s), 2U);
		HELL_EQ(s.raw[OBJ - 1U], DSTR_PTR_FLAG);
		HELL_EQ_STR(dstr_get(&s), al_text(OBJ - 1U, 'a'));
		dstr_cleanup(&s);
	}

	/* Whatever the space left in the meta byte was */
	for (size_t len = 0; len < OBJ; ++len) {
		struct dstr s = dstr(al_text(len, 'a'));
		HELL_EQ(dstr_resize_(sizeof s, s.raw, 1), 0);
		HELL_EQ(s.raw[OBJ - 1U], DSTR_PTR_FLAG);
		HELL_EQ_STR(dstr_get(&s), al_text(len, 'a'));
		dstr_cleanup(&s);
	}
}

/**
 * @brief dstr_grow_() of `len` that must change nothing and
 *        allocate nothing, exact or not, and return `want`
 */
static void
al_grow_same (struct dstr *s,
              size_t       len,
              int          want)
{
	struct dstr const before = *s;
	for (unsigned exact = 0; exact < 2U; ++exact) {
		shim_reset();
		HELL_EQ(dstr_grow_(sizeof *s, s->raw, len, exact), want);
		HELL_EQ(shim_calls(), 0U);
		HELL_EQ_MEM(s, &before, sizeof *s);
	}
}

/**
 * @brief dstr_grow_() of a `len` that fits does nothing, and
 *        counts nothing
 */
static void
al_grow_fits (void)
{
	for (size_t len = 0; len < OBJ; ++len) {
		struct dstr s = dstr(al_text(len, 'a'));
		al_grow_same(&s, 0, 0);
		al_grow_same(&s, OBJ - 1U - len, 0);
	}

	for (size_t c = 0; c <= DSTR_GROW_COUNT_MASK; ++c) {
		struct dstr s = dstr(al_text(300, 'a'));
		al_set_count(&s, c);
		al_grow_same(&s, 0, 0);
		al_grow_same(&s, 1, 0);
		al_grow_same(&s, dstr_space_left(&s), 0);
		dstr_cleanup(&s);
	}
}

/** Buffer that forged objects point to; never freed */
static unsigned char al_fake[DSTR_BLK_SIZE];

/**
 * @brief A forged heap object: buffer `buf`, string end `end`, block
 *        count `blocks` and the meta byte `meta`
 */
static struct dstr
al_forge (uintptr_t     buf,
          uintptr_t     end,
          uintptr_t     blocks,
          unsigned char meta)
{
	struct dstr f;
	(void)memset(&f, 0, sizeof f);
	f.ptr[0] = buf;
	f.ptr[1] = end;
	f.ptr[OBJ / sizeof f.ptr[0] - 1U] = blocks << DSTR_BLK_COUNT_POS;
	f.raw[OBJ - 1U] |= meta;
	return f;
}

/**
 * @brief dstr_grow_() of a missing or corrupt object: EFAULT or
 *        EILSEQ, and nothing changes
 */
static void
al_grow_bad (void)
{
	/* No object: a null pointer (volatile hides it from the compilers'
	 * nonnull checks). A size of 0 isn't tried: the definition's array
	 * parameter has that size as its bound, which must be positive
	 * when it is evaluated on entry, so the call itself would be
	 * undefined (clang's -fsanitize=vla-bound reports it). */
	struct dstr s = dstr("abc");
	unsigned char *volatile const none = NULL;
	struct dstr const before = s;
	for (unsigned exact = 0; exact < 2U; ++exact) {
		HELL_EQ(dstr_grow_(sizeof s, none, 100, exact), EFAULT);
		HELL_EQ_MEM(&s, &before, sizeof s);
	}

	/* A forged object is valid as long as it is consistent... */
	uintptr_t const fake = (uintptr_t)&al_fake[0];
	struct dstr f = al_forge(fake, fake + 10U, 1, al_meta(3));
	HELL_EQ(dstr_info(&f).state, DSTR_IS_PTR);
	HELL_EQ(dstr_space_left(&f), DSTR_BLK_SIZE - 11U);
	al_grow_same(&f, DSTR_BLK_SIZE - 11U, 0);

	/* ...and corrupt without a buffer, without an end, without blocks,
	 * with a string that doesn't fit or ends before it starts, or with
	 * a meta byte that is neither in place nor heap */
	struct dstr const bad[] = {
		al_forge(0, fake + 10U, 1, al_meta(3)),
		al_forge(fake, 0, 1, al_meta(3)),
		al_forge(fake, fake, 0, al_meta(3)),
		al_forge(fake, fake + DSTR_BLK_SIZE, 1, al_meta(3)),
		al_forge(fake + 1U, fake, 1, al_meta(3)),
		al_forge(fake, fake, 1, (unsigned char)OBJ),
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
		if (bad[i].raw[OBJ - 1U] == OBJ && OBJ >= DSTR_PTR_FLAG)
			continue; /* that meta byte would be the heap flag */
		f = bad[i];
		HELL_EQ(dstr_info(&f).state, DSTR_IS_BAD);
		al_grow_same(&f, 0, EILSEQ);
		al_grow_same(&f, 1000, EILSEQ);

		/* The public functions don't get that far */
		HELL_EQ_PTR(dstr_reserve(&f, 1000), NULL);
		HELL_EQ_PTR(dstr_cat(&f, "x"), NULL);
		HELL_EQ_PTR(dstr_cat_n(&f, "x", 1), NULL);
		HELL_EQ_PTR(dstr_printf(&f, "%s", "x"), NULL);
		HELL_EQ_PTR(dstr_erase(&f, 0, 0), NULL);
		HELL_EQ_PTR(dstr_clear(&f), NULL);
		HELL_EQ_MEM(&f, &bad[i], sizeof f);
	}
}

/**
 * @brief dstr_grow_() of a length + len that wraps, or of a size
 *        with more blocks than DSTR_BLK_COUNT_MASK: EOVERFLOW
 *        before any allocation, and nothing changes; a size just below
 *        the limit gets to the allocator
 */
static void
al_grow_overflow (void)
{
	/* Heap strings with every count, then an in-place string */
	for (size_t c = 0; c <= DSTR_GROW_COUNT_MASK + 1U; ++c) {
		bool const heap = c <= DSTR_GROW_COUNT_MASK;
		struct dstr s = dstr(al_text(heap ? 300U : 3U, 'a'));
		if (heap)
			al_set_count(&s, c);
		size_t const len = dstr_len(&s);
		struct dstr const before = s;

		for (unsigned exact = 0; exact < 2U; ++exact) {
			size_t const w = exact || !heap ? 0U
			               : c < DSTR_GROW_SHIFT_MAX ? c
			               : DSTR_GROW_SHIFT_MAX;
			size_t const step = DSTR_BLK_SIZE << w;
			size_t const over[] = {
				SIZE_MAX,                   /* wraps */
				SIZE_MAX - len + 1U,        /* wraps to 0 */
				SIZE_MAX - len,             /* SIZE_MAX characters */
				SIZE_MAX - len + 1U - step, /* fewest too many */
			};
			for (size_t i = 0; i < sizeof over / sizeof over[0]; ++i) {
				shim_reset();
				HELL_EQ(dstr_grow_(sizeof s, s.raw, over[i], exact),
				        EOVERFLOW);
				HELL_EQ(shim_calls(), 0U);
				HELL_EQ_MEM(&s, &before, sizeof s);
			}

			/* One character less needs DSTR_BLK_COUNT_MASK + 1
			 * - 2^w blocks: the allocator is asked, and fails */
			size_t calls;
			al_arm(1, ENOMEM);
			HELL_EQ(dstr_grow_(sizeof s, s.raw,
			                   SIZE_MAX - len - step, exact),
			        ENOMEM);
			HELL_CHECK(al_disarm(1, &calls));
			HELL_EQ(calls, 1U);
			HELL_EQ_MEM(&s, &before, sizeof s);
		}
		dstr_cleanup(&s);
	}
}

/**
 * @brief An exact dstr_grow_() grows in one-block steps and leaves
 *        the count; otherwise the step is 2^count blocks, at most
 *        2^DSTR_GROW_SHIFT_MAX, and the count goes up to its mask
 */
static void
al_grow_count (void)
{
	for (size_t c = 0; c <= DSTR_GROW_COUNT_MASK; ++c) {
		size_t const w = c < DSTR_GROW_SHIFT_MAX
		               ? c : DSTR_GROW_SHIFT_MAX;
		struct dstr s = dstr(al_text(300, 'a'));
		al_set_count(&s, c);

		HELL_EQ(dstr_grow_(sizeof s, s.raw, 1000, true), 0);
		HELL_EQ(dstr_get_block_count(&s), al_blocks(1300, 0));
		HELL_EQ(s.raw[OBJ - 1U], al_meta(c));
		HELL_EQ(dstr_len(&s), 300U);

		HELL_EQ(dstr_grow_(sizeof s, s.raw, 5000, false), 0);
		HELL_EQ(dstr_get_block_count(&s), al_blocks(5300, w));
		HELL_EQ(s.raw[OBJ - 1U], al_meta(c + 1U));
		HELL_EQ_STR(dstr_get(&s), al_text(300, 'a'));
		dstr_cleanup(&s);
	}

	/* An in-place string has no count, whatever its meta byte holds:
	 * one-block steps, and the count starts at 0 or 1 */
	for (size_t len = 0; len < OBJ; ++len) {
		for (unsigned exact = 0; exact < 2U; ++exact) {
			struct dstr s = dstr(al_text(len, 'a'));
			HELL_EQ(dstr_grow_(sizeof s, s.raw, 300, exact), 0);
			HELL_EQ(dstr_get_block_count(&s), al_blocks(len + 300U, 0));
			HELL_EQ(s.raw[OBJ - 1U], al_meta(!exact));
			HELL_EQ_STR(dstr_get(&s), al_text(len, 'a'));
			dstr_cleanup(&s);
		}
	}
}

int
main (void)
{
	AL_EVERY(al_init);
	for (al_mode = AL_CAT; al_mode < AL_MODES; ++al_mode) {
		AL_EVERY(al_cat_sso);
		AL_EVERY(al_cat_heap);
		AL_EVERY(al_cat_counted);
	}
	AL_EVERY(al_reserve_sso);
	AL_EVERY(al_reserve_heap);
	AL_EVERY(al_reserve_counted);
	al_appends();
	al_errno();
	al_vprintf_changed();
#if __STDC_HOSTED__
	al_vsnprintf_fails();
#endif
	al_shrink();
	al_resize_count();
	al_grow_fits();
	al_grow_bad();
	al_grow_overflow();
	al_grow_count();

	HELL_EQ(shim_live(), 0U);
	HELL_EQ(shim_bad(), 0U);
	return hell_exit();
}
