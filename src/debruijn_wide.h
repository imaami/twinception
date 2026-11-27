#ifndef DEBRUIJN_WIDE_H_
#define DEBRUIJN_WIDE_H_

#include <stdbool.h>
#include <stdint.h>

/** The orders of the functions below. A sequence of order 7 is 128 bits
 *  long and one of order 8 is 256 bits long, which is four 64-bit limbs.
 */
enum {
	DEBRUIJN_WIDE_MIN_ORDER = 2,
	DEBRUIJN_WIDE_MAX_ORDER = 8,
	DEBRUIJN_WIDE_LIMBS     = 4,
	/* the vertices of the graph of the largest order */
	DEBRUIJN_WIDE_VERTICES  = 1 << (DEBRUIJN_WIDE_MAX_ORDER - 1),
};

/** A binary de Bruijn sequence of an order from 2 to 8, or a rank, as a
 *  whole number in 64-bit limbs. limb[0] is the least significant 64 bits.
 *  A sequence of order 7 is in limb[0] and limb[1], and one of order 8 is
 *  in all four limbs. The bits above the 2^order bits of a sequence are
 *  zero. In debruijn.h a sequence is named a word, and the same functions
 *  are there for the orders 2 to 6 in one uint64_t.
 */
struct debruijn_wide {
	uint64_t limb[DEBRUIJN_WIDE_LIMBS];
};

/** A seed of order 2 to 7, prepared once for debruijn_wide_generate().
 *  Make one with debruijn_wide_generator() and do not change it.
 */
struct debruijn_wide_generator {
	uint32_t order;
	int      error;
	uint16_t edge[1 << (DEBRUIJN_WIDE_MAX_ORDER - 2)];
};

/** Returns true if the bit @p pos of the limbs at @p map is set. */
static inline bool
debruijn_wide_test (uint64_t const *map,
                    unsigned        pos)
{
	return map[pos >> 6U] >> (pos & 63U) & 1U;
}

/** Sets the bit @p pos of the limbs at @p map. */
static inline void
debruijn_wide_set (uint64_t *map,
                   unsigned  pos)
{
	map[pos >> 6U] |= UINT64_C(1) << (pos & 63U);
}

/** Clears the bit @p pos of the limbs at @p map. */
static inline void
debruijn_wide_clear (uint64_t *map,
                     unsigned  pos)
{
	map[pos >> 6U] &= ~(UINT64_C(1) << (pos & 63U));
}

/**
 * Returns the value of the @p order bits of @p seq from the bit @p index
 * up, with the bit @p index as the lowest bit of the value. Index 0 is the
 * least significant bit, and the bit above the top bit of the 2^order bits
 * is bit 0. @p order is from 2 to 8 and @p index is under 2^order.
 */
static inline unsigned
debruijn_wide_window (struct debruijn_wide const *seq,
                      unsigned                    order,
                      unsigned                    index)
{
	unsigned const bits = 1U << order;
	uint64_t const low = seq->limb[0];
	/* A sequence of one limb: both parts of the value are in the limb. */
	if (bits <= 64U)
		return (unsigned)((low >> index | low << ((bits - index) & (bits - 1U))) &
		                  (bits - 1U));
	unsigned const limb = index >> 6U;
	unsigned const shift = index & 63U;
	uint64_t value = seq->limb[limb] >> shift;
	/* The bits of the value over bit 63 of the limb are the low bits of
	 * the next limb, and the limb after the last limb is limb 0. */
	if (shift > 64U - order)
		value |= seq->limb[(limb + 1U) & ((bits >> 6U) - 1U)] << (64U - shift);
	return (unsigned)value & (bits - 1U);
}

/**
 * Check a normalized sequence of the given order: its first 'order' bits,
 * from the most significant bit of its 2^order bits, are zero, and every
 * value of 'order' bits is in it once.
 */
extern bool
debruijn_wide_valid (struct debruijn_wide const *seq,
                     unsigned                    order);

/**
 * Rotate a sequence of the given order to its normalized form. The
 * sequence may be any rotation within its 2^order bits. Store the rotation
 * whose first 'order' bits are zero and return 0, or return EFAULT for a
 * null pointer or EINVAL for an invalid order or a number that is no
 * rotation of a sequence of this order. Leave *seq unchanged on failure.
 * errno is not changed.
 */
extern int
debruijn_wide_normalize (struct debruijn_wide *seq,
                         unsigned              order);

/** Validate a seed of order 2 to 7 and prepare its last-exit tree once. */
extern struct debruijn_wide_generator
debruijn_wide_generator (struct debruijn_wide const *seed,
                         unsigned                    order);

/**
 * Generate the child of a prepared seed for a choice mask. The mask of a
 * seed of order n is 2^(n-1) - 1 bits wide, which is 63 bits for a seed of
 * order 7. Return 0, the error of the constructor, EFAULT for a null
 * pointer, or EINVAL for a mask that is out of range or a generator that
 * the constructor did not make. Leave *seq unchanged on failure.
 */
extern int
debruijn_wide_generate (struct debruijn_wide                 *seq,
                        struct debruijn_wide_generator const *src,
                        uint64_t                              choices);

/**
 * Construct the sequence of order 'order' + 1 from a seed of order 'order'
 * and a choice mask: debruijn_wide_generator() and
 * debruijn_wide_generate() in one call. order is from 2 to 7.
 */
extern int
debruijn_wide_expand (struct debruijn_wide       *seq,
                      struct debruijn_wide const *seed,
                      unsigned                    order,
                      uint64_t                    choices);

/**
 * Invert debruijn_wide_expand(): recover the seed and the choice mask of
 * a sequence of order 3 to 8. The seed is of order 'order' - 1. Return 0,
 * EFAULT for a null pointer, or EINVAL for an invalid order or sequence.
 * Leave both outputs unchanged on failure.
 */
extern int
debruijn_wide_collapse (struct debruijn_wide       *seed,
                        uint64_t                   *choices,
                        struct debruijn_wide const *seq,
                        unsigned                    order);

/**
 * Store in *rank the position of a normalized sequence among the
 * normalized sequences of its order in numeric order, from zero. The
 * sequences of an order n are 2^(2^(n-1) - n), so a rank of order 8 is up
 * to 120 bits wide. Return 0, EFAULT for a null pointer, EINVAL for an
 * invalid order or sequence, or ENOMEM. Leave *rank unchanged on failure.
 */
extern int
debruijn_wide_rank (struct debruijn_wide       *rank,
                    struct debruijn_wide const *seq,
                    unsigned                    order);

typedef int debruijn_wide_emit_fn (struct debruijn_wide const *seq, unsigned order, void *arg);

/**
 * Emit at most limit sequences in numeric order, from the rank *first on.
 * Nothing is emitted if *first is past the last rank or limit is zero.
 * Stop on and return the first error of emit. Return EFAULT for a null
 * pointer, EINVAL for an invalid order, or ENOMEM.
 */
extern int
debruijn_wide_ordered (unsigned                    order,
                       struct debruijn_wide const *first,
                       unsigned                    limit,
                       debruijn_wide_emit_fn      *emit,
                       void                       *arg);

#endif /* DEBRUIJN_WIDE_H_ */
