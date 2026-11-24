#ifndef DEBRUIJN_H_
#define DEBRUIJN_H_

#include <stdbool.h>
#include <stdint.h>

/** Supported sequence orders; an order-6 word occupies all 64 bits. */
enum {
	DEBRUIJN_MIN_ORDER = 2,
	DEBRUIJN_MAX_ORDER = 6,
	DEBRUIJN_ROOT_SEED = 0x3, /* the unique normalized order-2 word */
};

/** Prepared seed; obtain by constructor and treat its contents as read-only. */
struct debruijn_generator {
	unsigned edge[16];
	unsigned order;
	int      error;
};

/** Validate an order-2..5 seed and prepare its last-exit tree once. */
extern struct debruijn_generator
debruijn_generator (uint64_t seed,
                    unsigned order);

/**
 * Generate from a prepared seed. This avoids reexamining a seed for every mask.
 * Return 0, the constructor's error, EFAULT for a null pointer, or EINVAL for
 * an out-of-range mask. Leave *word unchanged on failure; do not change errno.
 */
extern int
debruijn_generate (uint64_t                         *word,
                   struct debruijn_generator const *src,
                   unsigned                         choices);

/**
 * Check a normalized binary de Bruijn word of the given order.
 *
 * The word is read most-significant bit first within its 2^order-bit width.
 * Its first 'order' bits must be zero. Rotations are thereby identified;
 * reversal and complementation remain distinct. Unused high bits must be zero.
 */
extern bool
debruijn_valid (uint64_t word,
                unsigned order);

/**
 * Construct an order-(order + 1) word from an order-'order' seed.
 *
 * order must be 2..5. There are 2^(order - 1) - 1 independent choice bits;
 * bit (v - 1) belongs to nonroot vertex v. Hence choices is 0..127 for
 * order 4, or 0..32767 for order 5. Every permitted choice succeeds.
 *
 * Return 0 on success, EFAULT for a null output pointer, or EINVAL for an
 * invalid seed, order, or choice mask. Leave *word unchanged on failure.
 * Error numbers are returned directly; errno is not changed.
 */
extern int
debruijn_expand (uint64_t *word,
                 uint64_t  seed,
                 unsigned  order,
                 unsigned  choices);

/**
 * Invert debruijn_expand: recover the unique seed and choice mask.
 *
 * order is the input word's order, 3..6. The recovered seed has order - 1.
 * Return 0, EFAULT for a null output pointer, or EINVAL for invalid input.
 * Leave both outputs unchanged on failure. errno is not changed.
 */
extern int
debruijn_collapse (uint64_t *seed,
                   unsigned *choices,
                   uint64_t  word,
                   unsigned  order);

#endif /* DEBRUIJN_H_ */
