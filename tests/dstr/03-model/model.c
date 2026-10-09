/**
 * @file model.c
 * @brief Seeded random operations against a reference model, for one
 *        object size (trial 03)
 *
 * Compiled once per legal DSTR_OBJ_SIZE with -DSUITE=name and linked
 * into one program with tests/dstr/suites_main.c. Three objects live
 * at once, each with a model of its content and of the block count and
 * growth count the growth policy predicts. The operations append with
 * dstr_cat(), dstr_cat_n() (null characters included) and dstr_printf(),
 * reserve, erase, clear, move one object to another, and start over.
 * Every operation is followed
 * by a check of every accessor, the zero tail (all bytes after the
 * terminator up to the end of the storage are zero, which the library
 * maintains on purpose), the meta byte of a heap string (read
 * white-box) and, when no growth happened, the stability of the data
 * pointer. The first failure prints the seed and the last operations.
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

/** Operations per object size, before DSTR_SCALE */
#ifndef MODEL_OPS
# define MODEL_OPS 20000U
#endif

/** Size of the object type under test */
#define OBJ (sizeof (struct dstr))

/** Objects alive at once */
#define MD_OBJS 3U

/** Longest modeled string: long enough for appends to grow a buffer
 *  past the largest step, 4 KiB, more than once */
#define MD_CAP 16384U

/** Length of the operation log printed on failure */
#define MD_LOG 16U

/** An object under test and its model */
struct md_obj {
	struct dstr s;
	size_t           len;            /**< Modeled length */
	size_t           blocks;         /**< Modeled blocks, 0 for SSO */
	unsigned         count;          /**< Modeled growth count */
	char             want[MD_CAP + 1U];
};

/** Every object and the recent operations */
static struct {
	struct md_obj obj[MD_OBJS];
	char          src[MD_CAP + 1U];
	struct {
		char const *what;
		size_t      obj;
		size_t      arg;
	}             log[MD_LOG];
	size_t        ops;
} md;

/**
 * @brief Remember an operation for the failure report
 */
static void
md_note (char const *what,
         size_t      obj,
         size_t      arg)
{
	size_t const i = md.ops++ % MD_LOG;
	md.log[i].what = what;
	md.log[i].obj = obj;
	md.log[i].arg = arg;
}

/**
 * @brief Print the recent operations to stderr
 */
static void
md_report (void)
{
	(void)fprintf(stderr, "object size %zu, seed %" PRIu64
	              ", after %zu operations:\n", OBJ, hell_seed(), md.ops);
	size_t const n = md.ops < MD_LOG ? md.ops : MD_LOG;
	for (size_t k = md.ops - n; k < md.ops; ++k) {
		size_t const i = k % MD_LOG;
		(void)fprintf(stderr, "  #%zu %s obj=%zu arg=%zu\n", k,
		              md.log[i].what, md.log[i].obj, md.log[i].arg);
	}
}

/**
 * @brief Random letters into md.src, terminated, `n` <= MD_CAP
 */
static char const *
md_random_text (size_t n)
{
	for (size_t i = 0; i < n; ++i)
		md.src[i] = (char)('a' + hell_below(26));
	md.src[n] = '\0';
	return md.src;
}

/**
 * @brief Random letters into md.src, about every eighth a null
 *        character, followed by a letter instead of a terminator,
 *        `n` <= MD_CAP
 */
static char const *
md_random_bytes (size_t n)
{
	for (size_t i = 0; i < n; ++i)
		md.src[i] = hell_below(8) ? (char)('a' + hell_below(26)) : '\0';
	md.src[n] = 'Z';
	return md.src;
}

/**
 * @brief Storage size of an object with `blocks` blocks
 */
static size_t
md_cap (size_t blocks)
{
	return blocks ? blocks * DSTR_BLK_SIZE : OBJ;
}

/**
 * @brief Growth step of an append to `m`, in blocks: 1 doubled once for
 *        every growth by an append, at most DSTR_GROW_SHIFT_MAX
 *        times (an in-place string has a count of 0)
 */
static size_t
md_step_blocks (struct md_obj const *m)
{
	size_t step = 1;
	for (unsigned c = 0; c < m->count && c < DSTR_GROW_SHIFT_MAX; ++c)
		step *= 2U;
	return step;
}

/**
 * @brief Blocks after a request for `n` more bytes, per the growth
 *        policy, or SIZE_MAX if the request must fail
 *
 * The new string and its terminator get the fewest blocks that hold
 * them: any number of blocks for a reserve (`exact`), a multiple of the
 * step for an append.
 */
static size_t
md_after (struct md_obj const *m,
          size_t               n,
          bool                 exact)
{
	if (!n || n <= md_cap(m->blocks) - 1U - m->len)
		return m->blocks;
	size_t const need = m->len + n;
	if (need < m->len)
		return SIZE_MAX;
	size_t const step = exact ? 1U : md_step_blocks(m);
	size_t const blocks = (need / DSTR_BLK_SIZE / step + 1U) * step;
	if (blocks > DSTR_BLK_COUNT_MASK)
		return SIZE_MAX;
	return blocks;
}

/**
 * @brief Check the object against its model
 *
 * @return `false` if any check failed
 */
static bool
md_check (struct md_obj *m)
{
	unsigned long const failed = hell_count.failed;
	struct dstr *const s = &m->s;
	size_t const cap = md_cap(m->blocks);
	char const *const str = dstr_get(s);
	struct dstr_info const info = dstr_info(s);

	HELL_EQ(info.state, m->blocks ? DSTR_IS_PTR : DSTR_IS_SSO);
	HELL_EQ(info.blocks, m->blocks);
	HELL_EQ(info.length, m->len);
	HELL_EQ(info.space, cap - 1U - m->len);
	HELL_EQ(dstr_len(s), m->len);
	HELL_EQ(dstr_space_left(s), cap - 1U - m->len);
	HELL_EQ(dstr_get_block_count(s), m->blocks);
	HELL_CHECK(str != NULL);
	if (str) {
		HELL_EQ_PTR(dstr_get_write_ptr(s), str + m->len);
		HELL_EQ_PTR(dstr_raw(s), str);
		HELL_EQ_MEM(str, m->want, m->len);

		/* The terminator and the zero tail, up to the meta byte in
		 * place (the terminator of a full string) */
		size_t const end = m->blocks ? cap : OBJ - 1U;
		size_t nonzero = 0;
		for (size_t i = m->len; i < end; ++i)
			nonzero += str[i] != 0;
		HELL_EQ(nonzero, 0U);
		if (!m->blocks)
			HELL_EQ(s->raw[OBJ - 1U], OBJ - 1U - m->len);
	}

	/* The meta byte of a heap string: the growth count, the pointer
	 * flag, and zero bits between them */
	if (m->blocks) {
		unsigned const meta = s->raw[OBJ - 1U];
		HELL_EQ(meta & DSTR_GROW_COUNT_MASK, m->count);
		HELL_EQ(meta, DSTR_PTR_FLAG | m->count);
	}
	return hell_count.failed == failed;
}

/**
 * @brief Length of the next append to `m`, biased toward the edges
 */
static size_t
md_pick (struct md_obj const *m)
{
	size_t const space = md_cap(m->blocks) - 1U - m->len;
	size_t const step = md_step_blocks(m) * DSTR_BLK_SIZE;
	switch (hell_below(9)) {
	case 0:  return 0;
	case 1:  return 1;
	case 2:  return space;
	case 3:  return space + 1U;
	case 4:  return space ? space - 1U : 0U;
	case 5:  return (size_t)hell_below(64) + 1U;
	case 6:  return (size_t)hell_below(600) + 1U;
	case 7:  return DSTR_BLK_SIZE - m->len % DSTR_BLK_SIZE
	                + (size_t)hell_below(3) - 1U;
	default: return step - m->len % step + (size_t)hell_below(3) - 1U;
	}
}

/** How md_cat() appends */
enum md_how {
	MD_CAT,    /**< dstr_cat(), which stops at a null character */
	MD_CAT_N,  /**< dstr_cat_n() */
	MD_PRINTF, /**< md_printf() */
};

/**
 * @brief printf() the first `n` characters of md.src, letters, to `s`,
 *        in one of several formats; the last character of md.src may
 *        change to what the format makes
 */
static struct dstr *
md_printf (struct dstr *s,
           size_t       n)
{
	unsigned const form = n ? (unsigned)hell_below(4) : 0U;
	if (!form)
		return dstr_printf(s, "%s", md.src);

	struct dstr *r;
	char const last = md.src[n - 1U];
	md.src[n - 1U] = '\0';
	switch (form) {
	case 1:
		r = dstr_printf(s, "%s%c", md.src, last);
		md.src[n - 1U] = last;
		break;
	case 2:
		r = dstr_printf(s, "%s%%", md.src);
		md.src[n - 1U] = '%';
		break;
	default: {
		unsigned const d = (unsigned)hell_below(10);
		r = dstr_printf(s, "%s%u", md.src, d);
		md.src[n - 1U] = (char)('0' + d);
		break;
	}
	}
	return r;
}

/**
 * @brief Apply an append of `n` bytes at `src` to the model and the
 *        object, then check
 */
static bool
md_cat (size_t      i,
        char const *src,
        size_t      n,
        enum md_how how)
{
	static char const *const what[][2] = {
		{ "cat", "cat-other" }, { "cat_n", "cat_n-other" },
		{ "printf", "printf" },
	};
	struct md_obj *const m = &md.obj[i];
	char const *const before = dstr_get(&m->s);

	if (how == MD_CAT) {
		size_t k = 0;
		while (k < n && src[k])
			++k;
		n = k;
	}
	size_t const blocks = md_after(m, n, false);

	md_note(what[how][src != md.src], i, n);
	struct dstr *r;
	switch (how) {
	case MD_CAT:
		r = dstr_cat(&m->s, n ? src : "");
		break;
	case MD_CAT_N:
		r = dstr_cat_n(&m->s, n || hell_below(2) ? src : NULL, n);
		break;
	default:
		r = md_printf(&m->s, n);
		break;
	}
	HELL_EQ_PTR(r, &m->s);
	memcpy(m->want + m->len, src, n);
	m->len += n;
	m->want[m->len] = '\0';

	/* A string that doesn't grow stays where it is; a growth by an
	 * append is counted, up to the limit */
	if (blocks == m->blocks)
		HELL_EQ_PTR(dstr_get(&m->s), before);
	else if (m->count < DSTR_GROW_COUNT_MASK)
		++m->count;
	m->blocks = blocks;
	return md_check(m);
}

/**
 * @brief Erase a random span of object `i`, sometimes from past the
 *        end, then check: the buffer, its block count and growth count
 *        stay
 */
static bool
md_erase (size_t i)
{
	struct md_obj *const m = &md.obj[i];
	struct dstr const before = m->s;
	char const *const data = dstr_get(&m->s);
	size_t const pos = (size_t)hell_below(m->len + 2U);
	size_t const rest = pos <= m->len ? m->len - pos : 0U;
	size_t len;
	switch (hell_below(5)) {
	case 0:  len = 0; break;
	case 1:  len = 1; break;
	case 2:  len = rest + (size_t)hell_below(2); break;
	case 3:  len = SIZE_MAX; break;
	default: len = (size_t)hell_below(rest + 1U); break;
	}

	md_note("erase", i, pos);
	struct dstr *const r = dstr_erase(&m->s, pos, len);
	if (pos > m->len) {
		HELL_EQ_PTR(r, NULL);
		HELL_EQ_MEM(&m->s, &before, sizeof m->s);
		return md_check(m);
	}

	size_t const n = len < rest ? len : rest;
	HELL_EQ_PTR(r, &m->s);
	HELL_EQ_PTR(dstr_get(&m->s), data);
	memmove(m->want + pos, m->want + pos + n, rest - n + 1U);
	m->len -= n;
	return md_check(m);
}

/**
 * @brief Clear object `i`, then check: the buffer, its block count and
 *        growth count stay
 */
static bool
md_clear (size_t i)
{
	struct md_obj *const m = &md.obj[i];
	char const *const data = dstr_get(&m->s);

	md_note("clear", i, m->len);
	HELL_EQ_PTR(dstr_clear(&m->s), &m->s);
	HELL_EQ_PTR(dstr_get(&m->s), data);
	m->len = 0;
	m->want[0] = '\0';
	return md_check(m);
}

/**
 * @brief Move object `i` to a random object `j`, which is cleaned up
 *        first, then check both: `j` has what `i` had, a heap buffer
 *        where it was, and `i` is empty. Moving to itself changes
 *        nothing.
 */
static bool
md_move (size_t i)
{
	size_t const j = (size_t)hell_below(MD_OBJS);
	struct md_obj *const m = &md.obj[i];
	struct md_obj *const o = &md.obj[j];

	md_note("move", i, j);
	if (i == j) {
		struct dstr const before = m->s;
		m->s = dstr_move(&m->s);
		HELL_EQ_MEM(&m->s, &before, sizeof m->s);
		return md_check(m);
	}

	char const *const data = dstr_get(&m->s);
	dstr_cleanup(&o->s);
	o->s = dstr_move(&m->s);
	memcpy(o->want, m->want, m->len + 1U);
	o->len = m->len;
	o->blocks = m->blocks;
	o->count = m->count;
	if (o->blocks)
		HELL_EQ_PTR(dstr_get(&o->s), data);

	struct dstr const empty = DSTR_INIT;
	HELL_EQ_MEM(&m->s, &empty, sizeof m->s);
	m->len = 0;
	m->blocks = 0;
	m->count = 0;
	m->want[0] = '\0';
	return md_check(o) && md_check(m);
}

/**
 * @brief Reserve `k` bytes in object `i`, then check
 */
static bool
md_reserve (size_t i,
            size_t k)
{
	struct md_obj *const m = &md.obj[i];
	struct dstr const before = m->s;
	size_t const blocks = md_after(m, k, true);

	md_note("reserve", i, k);
	struct dstr *const r = dstr_reserve(&m->s, k);
	if (blocks == SIZE_MAX) {
		HELL_EQ_PTR(r, NULL);
		HELL_EQ_MEM(&m->s, &before, sizeof m->s);
	} else {
		HELL_EQ_PTR(r, &m->s);
		if (blocks == m->blocks)
			HELL_EQ_MEM(&m->s, &before, sizeof m->s);

		/* A reserve leaves the growth count as it is */
		m->blocks = blocks;
		HELL_CHECK(dstr_space_left(&m->s) >= k);
	}
	return md_check(m);
}

/**
 * @brief Run one random operation on object `i`
 */
static bool
md_step (size_t i)
{
	struct md_obj *const m = &md.obj[i];
	size_t const op = (size_t)hell_below(100);

	if (op < 52) {
		size_t n = md_pick(m);
		if (m->len + n > MD_CAP)
			n = MD_CAP - m->len;
		if (op < 25)
			return md_cat(i, md_random_text(n), n, MD_CAT);
		if (op < 40)
			return md_cat(i, md_random_bytes(n), n, MD_CAT_N);
		return md_cat(i, md_random_text(n), n, MD_PRINTF);
	}

	if (op < 64) {
		size_t const space = md_cap(m->blocks) - 1U - m->len;
		size_t k;
		switch (hell_below(6)) {
		case 0:  k = 0; break;
		case 1:  k = space; break;
		case 2:  k = space + 1U; break;
		case 3:  k = SIZE_MAX; break;
		case 4:  k = SIZE_MAX - m->len; break;
		default: k = (size_t)hell_below(MD_CAP - m->len + 1U); break;
		}
		return md_reserve(i, k);
	}

	if (op < 74) {
		/* Append part of another object's content */
		size_t const j = (i + 1U + (size_t)hell_below(MD_OBJS - 1U))
		                 % MD_OBJS;
		struct md_obj const *const o = &md.obj[j];
		size_t const at = (size_t)hell_below(o->len + 1U);
		size_t n = o->len - at;
		if (m->len + n > MD_CAP)
			n = MD_CAP - m->len;
		if (n < o->len - at) {
			/* Too long: append a shorter random text instead */
			return md_cat(i, md_random_text(n), n, MD_CAT);
		}
		return md_cat(i, dstr_get(&o->s) + at, n,
		              hell_below(2) ? MD_CAT_N : MD_CAT);
	}

	if (op < 81)
		return md_erase(i);
	if (op < 83)
		return md_clear(i);
	if (op < 86)
		return md_move(i);

	dstr_cleanup(&m->s);
	m->len = 0;
	m->blocks = 0;
	m->count = 0;
	m->want[0] = '\0';

	if (op < 92) {
		size_t const n = (size_t)hell_below(700);
		md_note("init", i, n);
		m->s = dstr(md_random_text(n));
		memcpy(m->want, md.src, n + 1U);
		m->len = n;
		m->blocks = n < OBJ ? 0U : n / DSTR_BLK_SIZE + 1U;
	} else if (op < 96) {
		md_note("reset", i, 0);
		m->s = (struct dstr)DSTR_INIT;
	} else {
		md_note("cleanup", i, 0);
	}
	return md_check(m);
}

/**
 * @brief Run the random operations for this object size
 */
void
SUITE (struct hell_totals *t)
{
	(void)fprintf(stderr, "object size %zu\n", OBJ);
	for (size_t i = 0; i < MD_OBJS; ++i)
		md.obj[i].s = (struct dstr)DSTR_INIT;

	unsigned long const ops = MODEL_OPS * hell_scale();
	for (unsigned long k = 0; k < ops; ++k) {
		if (!md_step((size_t)hell_below(MD_OBJS))) {
			md_report();
			break;
		}
	}

	for (size_t i = 0; i < MD_OBJS; ++i)
		dstr_cleanup(&md.obj[i].s);
	hell_collect(t);
}
