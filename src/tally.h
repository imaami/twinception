/** @file tally.h
 *
 * A tally of items and of their bytes, as a budget of a number of items
 * and of a number of bytes counts what it holds.
 */
#ifndef ASKAI_SRC_TALLY_H_
#define ASKAI_SRC_TALLY_H_

#include <stddef.h>

#include "compat.h"

/** A number of items and the sum of their bytes. A zeroed one is empty. */
struct tally {
	size_t n;
	size_t bytes;
};

/** Whether one more item of @p bytes bytes fits in the tally @p used
 *  within the bounds @p max. @p used may be over @p max already.
 */
static inline bool
tally_fits (struct tally const *used,
            struct tally        max,
            size_t              bytes)
{
	return used->n < max.n && used->bytes <= max.bytes && bytes <= max.bytes - used->bytes;
}

/** Count one more item of @p bytes bytes in @p used. */
static inline void
tally_add (struct tally *used,
           size_t        bytes)
{
	++used->n;
	used->bytes += bytes;
}

/** Take an item of @p bytes bytes out of @p used. */
static inline void
tally_sub (struct tally *used,
           size_t        bytes)
{
	--used->n;
	used->bytes -= bytes;
}

#endif /* ASKAI_SRC_TALLY_H_ */
