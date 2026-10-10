/** @file Log-linear latency histogram with HdrHistogram-style bucketing.
 *
 * Values are nanoseconds.  The first @ref HIST_SUB values are counted exactly;
 * above that each power-of-two octave is split into @ref HIST_SUB linear
 * sub-buckets, so the relative error is at most one part in @ref HIST_SUB.
 * This keeps percentiles cheap and bounded in memory while a run records
 * millions of samples spanning sub-microsecond to multi-second latencies.
 *
 * A histogram has one writer, and any thread may read it.  The fields are
 * atomic, and the writer updates them with relaxed loads and stores, which
 * cost what plain ones do: a reader sees each field whole, though not all of
 * them as of one instant.
 */
#ifndef TWINCEPTION_SRC_HIST_H_
#define TWINCEPTION_SRC_HIST_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

enum {
	HIST_SIG_BITS = 7,                      /**< ~0.8 % relative resolution */
	HIST_SUB      = 1 << HIST_SIG_BITS,     /**< linear sub-buckets per octave */
	HIST_OCTAVES  = 64 - HIST_SIG_BITS,     /**< octaves a uint64 ns value spans */
	HIST_BUCKETS  = (HIST_OCTAVES + 1) * HIST_SUB,
};

/** Bucketed value counts plus exact count, sum and extrema. */
struct hist {
	_Atomic uint64_t count;
	_Atomic uint64_t sum;
	_Atomic uint64_t min;
	_Atomic uint64_t max;
	_Atomic uint64_t bucket[HIST_BUCKETS];
};

/** A relaxed load of @p p, for readers of a histogram or a single-writer
 *  counter. */
static inline uint64_t
hist_get (_Atomic uint64_t const *p)
{
	return atomic_load_explicit(p, memory_order_relaxed);
}

/** Add @p n to @p p, which only the calling thread writes. */
static inline void
hist_add (_Atomic uint64_t *p,
          uint64_t          n)
{
	atomic_store_explicit(p, hist_get(p) + n, memory_order_relaxed);
}

/** The bucket index a value falls in: a linear floor, then octave + offset. */
static inline size_t
hist_index (uint64_t v)
{
	if (v < HIST_SUB)
		return (size_t)v;
	int octave = 63 - __builtin_clzll(v) - HIST_SIG_BITS;
	uint64_t sub = (v - ((uint64_t)HIST_SUB << octave)) >> octave;
	return (size_t)(octave + 1) * HIST_SUB + (size_t)sub;
}

/** Midpoint of a bucket's value range, for reporting a percentile. */
static inline uint64_t
hist_value (size_t idx)
{
	if (idx < HIST_SUB)
		return idx;
	int octave = (int)(idx / HIST_SUB) - 1;
	uint64_t sub = idx % HIST_SUB;
	uint64_t low = ((uint64_t)HIST_SUB << octave) + (sub << octave);
	return low + ((uint64_t)1 << octave) / 2;
}

/** Record @p v; only the histogram's one writer may. */
static inline void
hist_record (struct hist *h,
             uint64_t     v)
{
	uint64_t count = hist_get(&h->count);
	if (!count || v < hist_get(&h->min))
		atomic_store_explicit(&h->min, v, memory_order_relaxed);
	if (v > hist_get(&h->max))
		atomic_store_explicit(&h->max, v, memory_order_relaxed);
	atomic_store_explicit(&h->count, count + 1, memory_order_relaxed);
	hist_add(&h->sum, v);
	hist_add(&h->bucket[hist_index(v)], 1);
}

/** How many recorded values lie above @p v, to the bucket resolution. */
static inline uint64_t
hist_above (struct hist const *h,
            uint64_t           v)
{
	uint64_t n = 0;
	for (size_t i = hist_index(v) + 1; i < HIST_BUCKETS; ++i)
		n += hist_get(&h->bucket[i]);
	return n;
}

/** The smallest recorded value whose cumulative count reaches @p p of the total. */
static inline uint64_t
hist_pct (struct hist const *h,
          double             p)
{
	uint64_t count = hist_get(&h->count);
	uint64_t max = hist_get(&h->max);
	if (!count)
		return 0;
	uint64_t rank = (uint64_t)((double)count * p + 0.5);
	if (rank < 1)
		rank = 1;
	uint64_t seen = 0;
	for (size_t i = 0; i < HIST_BUCKETS; ++i) {
		seen += hist_get(&h->bucket[i]);
		if (seen >= rank) {
			uint64_t v = hist_value(i);
			return v > max ? max : v;
		}
	}
	return max;
}

#endif /* TWINCEPTION_SRC_HIST_H_ */
