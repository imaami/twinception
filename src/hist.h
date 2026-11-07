/** @file hist.h
 *
 * Log-linear latency histogram with HdrHistogram-style bucketing.
 *
 * Values are nanoseconds. The first @ref HIST_SUB values are counted exactly;
 * above that each power-of-two octave is split into @ref HIST_SUB linear
 * sub-buckets, so the relative error is at most one part in @ref HIST_SUB.
 * This keeps percentiles cheap and bounded in memory while a run records
 * millions of samples spanning sub-microsecond to multi-second latencies.
 *
 * A histogram has one writer, and any thread may read it. The fields are
 * atomic, and the writer updates them with relaxed loads and stores, which
 * cost what plain ones do: a reader sees each field whole, though not all of
 * them as of one instant.
 *
 * Atomic objects need care to be valid (C23, N3220 7.17.2). Those of static
 * or thread storage duration are, as are those declared with an initializer,
 * a struct value such as loop()'s included. Allocated ones, uninitialized
 * automatic ones, and any whose bytes a non-atomic store such as memset()
 * reached have an indeterminate representation until atomic_init(). The
 * standard leaves two cases open, which the program steers clear of:
 * - atomic_init() on an object initialized before. 7.17.2.1 sets no limit,
 *   and 7.17.2 has memset() leave an object as indeterminate as fresh storage,
 *   which atomic_init() is for, but it does not say that initializing again
 *   is sound. Here an atomic object is initialized once, as its storage is
 *   declared or allocated, and later reset by atomic stores; wipes skip it.
 * - Assigning a struct over an atomic member. That is a non-atomic store, and
 *   producing a non-value representation through an lvalue other than of
 *   character type is undefined (6.2.6.1p5), though storing a value is never
 *   to generate one (6.2.6.1p8). Here no struct with atomic members is
 *   assigned: it is declared from a value, or allocated and its atomic
 *   members initialized one by one.
 */
#ifndef ASKAI_SRC_HIST_H_
#define ASKAI_SRC_HIST_H_

#include <stdatomic.h>
#include <stdbit.h>
#include <stddef.h>
#include <stdint.h>

#include "compat.h"

enum : unsigned {
	HIST_SIG_BITS = 7U,                      /**< ~0.8 % relative resolution */
	HIST_SUB      = 1U << HIST_SIG_BITS,     /**< linear sub-buckets per octave */
	HIST_OCTAVES  = 64U - HIST_SIG_BITS,     /**< octaves a uint64 ns value spans */
	HIST_BUCKETS  = (HIST_OCTAVES + 1U) * HIST_SUB,
};

/** Bucketed value counts plus exact count, sum and extrema. */
struct hist {
	_Atomic uint64_t count;
	_Atomic uint64_t sum;
	_Atomic uint64_t min;
	_Atomic uint64_t max;
	_Atomic uint64_t bucket[HIST_BUCKETS];
};

/** Initialize @p h, empty, in storage just allocated (the file's note on
 *  atomics); hist_clear() empties it after that.
 */
static inline void
hist_init (struct hist *h)
{
	atomic_init(&h->count, 0U);
	atomic_init(&h->sum, 0U);
	atomic_init(&h->min, 0U);
	atomic_init(&h->max, 0U);

	for (size_t i = 0; i < array_size(h->bucket); ++i) {
		atomic_init(&h->bucket[i], 0U);
	}
}

/** A relaxed load of @p p, for readers of a histogram or a single-writer
 *  counter.
 */
static inline uint64_t
hist_get (_Atomic uint64_t const *p)
{
	return atomic_load_explicit(p, memory_order_relaxed);
}

/** Set @p p, which only the calling thread writes, to @p v with a relaxed
 *  store.
 */
static inline void
hist_set (_Atomic uint64_t *p,
          uint64_t          v)
{
	atomic_store_explicit(p, v, memory_order_relaxed);
}

/** Add @p n to @p p, which only the calling thread writes. With one
 *  writer, a relaxed load and a relaxed store are enough. On 64-bit
 *  targets they compile to ordinary load and store instructions, which cost
 *  less than the atomic read-modify-write of atomic_fetch_add().
 */
static inline void
hist_add (_Atomic uint64_t *p,
          uint64_t          n)
{
	hist_set(p, hist_get(p) + n);
}

/** Raise @p p, which only the calling thread writes, to @p v if @p v is
 *  greater.
 */
static inline void
hist_max (_Atomic uint64_t *p,
          uint64_t          v)
{
	if (v > hist_get(p))
		hist_set(p, v);
}

/** Add @p n to @p p, which several threads write, with a relaxed atomic
 *  read-modify-write. A subtraction adds the negated value.
 */
static inline void
hist_count (_Atomic uint64_t *p,
            uint64_t          n)
{
	atomic_fetch_add_explicit(p, n, memory_order_relaxed);
}

/** Empty @p h; only the histogram's one writer may. */
static inline void
hist_clear (struct hist *h)
{
	hist_set(&h->count, 0U);
	hist_set(&h->sum, 0U);
	hist_set(&h->min, 0U);
	hist_set(&h->max, 0U);

	for (size_t i = 0; i < array_size(h->bucket); ++i)
		hist_set(&h->bucket[i], 0U);
}

/** The bucket index a value falls in: a linear floor, then octave + offset. */
static const_inline size_t
hist_index (uint64_t v)
{
	if (v < HIST_SUB)
		return (size_t)v;
	unsigned const x = (unsigned)stdc_bit_width(v) - 1U - HIST_SIG_BITS;
	uint64_t const y = (v - ((uint64_t)HIST_SUB << x)) >> x;
	return (size_t)((x + 1U) * (uint64_t)HIST_SUB + y);
}

/** Midpoint of a bucket's value range, for reporting a percentile. */
static const_inline uint64_t
hist_value (size_t i)
{
	if (i < HIST_SUB)
		return i;
	unsigned const x = i / HIST_SUB - 1U;
	return ((uint64_t)(i % HIST_SUB) << x)
	     + ((uint64_t)     HIST_SUB  << x)
	     + ((uint64_t)1              << x) / 2U;
}

/** Record @p v; only the histogram's one writer may. */
static inline void
hist_record (struct hist *h,
             uint64_t     v)
{
	uint64_t count = hist_get(&h->count);
	if (!count || v < hist_get(&h->min))
		hist_set(&h->min, v);
	hist_max(&h->max, v);
	hist_set(&h->count, count + 1);
	hist_add(&h->sum, v);
	hist_add(&h->bucket[hist_index(v)], 1);
}

/** How many recorded values lie above @p v, to the bucket resolution. */
static inline uint64_t
hist_above (struct hist const *h,
            uint64_t const     v)
{
	uint64_t n = 0U;
	for (size_t i = hist_index(v) + 1U; i < array_size(h->bucket); ++i)
		n += hist_get(&h->bucket[i]);
	return n;
}

/** The smallest recorded value whose cumulative count reaches @p p of the total. */
static inline uint64_t
hist_pct (struct hist const *h,
          double             p)
{
	uint64_t count = hist_get(&h->count);
	if (!count)
		return 0;

	uint64_t rank = (uint64_t)((double)count * p + 0.5);
	if (!rank)
		rank = 1U;

	uint64_t max = hist_get(&h->max);

	uint64_t seen = 0U;
	for (size_t i = 0U; i < array_size(h->bucket); ++i) {
		seen += hist_get(&h->bucket[i]);
		if (seen >= rank) {
			uint64_t val = hist_value(i);
			return val > max ? max : val;
		}
	}

	return max;
}

#endif /* ASKAI_SRC_HIST_H_ */
