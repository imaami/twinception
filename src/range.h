/** @file range.h
 *
 * An inclusive range of numbers, such as the lines 120 to 140 of a file or
 * the subordinate ids 100000 to 165535 of a user.
 */
#ifndef ASKAI_SRC_RANGE_H_
#define ASKAI_SRC_RANGE_H_

#include <stdint.h>
#include <stdlib.h>

#include "compat.h"

/** The numbers from first to last, both included. first is not greater
 *  than last.
 */
struct range {
	uint32_t first;
	uint32_t last;
};

/** Returns how many numbers @p r contains. The result is 64 bits wide,
 *  because the range 0 to UINT32_MAX contains UINT32_MAX + 1 numbers.
 */
static inline uint64_t
range_count (struct range r)
{
	return (uint64_t)r.last - r.first + 1;
}

/** Returns true if every number of @p inner is in @p outer. */
static inline bool
range_contains (struct range outer,
                struct range inner)
{
	return inner.first >= outer.first && inner.last <= outer.last;
}

/** Orders two ranges by their first numbers, for qsort(). */
static inline int
range_order (void const *a,
             void const *b)
{
	struct range const *x = a;
	struct range const *y = b;
	return (x->first > y->first) - (x->first < y->first);
}

/**
 * Sorts the @p n ranges at @p r by their first numbers and merges the
 * ranges that overlap or touch, such as 3-6 with 5-9 or with 7-9. Returns
 * the number of ranges that are left, which are at the start of @p r. They
 * ascend, and at least one number that is in no range lies between two of
 * them.
 */
static inline size_t
range_merge (struct range *r,
             size_t        n)
{
	if (!n)
		return 0;
	qsort(r, n, sizeof *r, range_order);
	size_t k = 0;
	for (size_t i = 1; i < n; ++i) {
		/* The sum is computed in 64 bits, so that it does not wrap for a
		 * range that ends at UINT32_MAX. */
		if (r[i].first > (uint64_t)r[k].last + 1)
			r[++k] = r[i];
		else if (r[i].last > r[k].last)
			r[k].last = r[i].last;
	}
	return k + 1;
}

#endif /* ASKAI_SRC_RANGE_H_ */
