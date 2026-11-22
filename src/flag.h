/** @file flag.h
 *
 * The flag word: one type for each width, the ranges of bits that mean
 * the same in every flag word, and the functions that read and write a
 * flag word.
 *
 * A flag word is a struct flag8, flag16, flag32 or flag64. It is a member
 * of an object of a module, and it describes one subject: a tool call, a
 * job, a child process, a connection. The comments below call that
 * subject the work. The comment of the member says what the work is.
 *
 * A word is divided into ranges. A range is one bit or several adjacent
 * bits, and code reads a range as one number.
 * - A range of one bit is a fact that is true or false. Facts in
 *   different ranges are independent of each other.
 * - A range of several bits is one value out of values that exclude each
 *   other. A range of two bits is one of four values.
 *
 * The low FLAG_BITS bits of every word are the common ranges of enum
 * flag. A common range is at the same bits and has the same values in
 * every word, so code copies a common range from one word to another
 * with a mask and does not translate it. A word uses the common ranges
 * that its work has and leaves the others 0. The bits from FLAG_BITS on
 * are the ranges of the module that defines the word. The module defines
 * them with FLAG_OWN().
 *
 * A word either describes its work or is a message to the side that does
 * the work. In a message, an order bit is the order. In a word that
 * describes, an order bit says that the order was given to the work.
 *
 * A module can give its word a type of its own: a union with a member
 * `struct flagN flag`. The functions of this header take that member, and
 * the functions of the module take the union, so the compiler rejects the
 * word of another module.
 *
 * These are integers and not flag words:
 * - A word that several threads write is an atomic integer. Its bits are
 *   defined with the names of this header all the same, as the messages
 *   of src/job.h are.
 * - A number that a file, a protocol or a plugin fixes: the flags of a
 *   graph file, the status of a ledger record, enum tool_out and enum
 *   tool_flag of src/tool.h.
 */
#ifndef ASKAI_SRC_FLAG_H_
#define ASKAI_SRC_FLAG_H_

#include <stdint.h>

#include "compat.h"

/** The number of bits of the common ranges. */
enum { FLAG_BITS = 9 };

/** The common ranges and their values. A name with a value of several
 *  bits is the mask of a range, and the names after it are the values of
 *  that range. A value is at the bits of its range, so code compares the
 *  bits of the mask with the value.
 */
enum flag : uint16_t {
	/** How the work ended. 0 means that the work has not ended, or that
	 *  the word does not say how it ended. The three values are the
	 *  statuses of a ledger record (src/ledger.h) after "working".
	 */
	FLAG_HOW       = 3u << 0,
	FLAG_COMPLETED = 1u << 0,  /**< the work ended and gave its result */
	FLAG_FAILED    = 2u << 0,  /**< the work ended because it failed */
	FLAG_CANCELLED = 3u << 0,  /**< the work ended because of an order,
	                                before it gave its result */

	/** The bit of FLAG_HOW that FLAG_FAILED and FLAG_CANCELLED contain
	 *  and FLAG_COMPLETED does not: the work ended without its result.
	 */
	FLAG_UNMET     = 2u << 0,

	/** Where the work is in its life. 0 means that there is no work, or
	 *  that the word does not say where the work is.
	 */
	FLAG_WHEN      = 3u << 2,
	FLAG_BEFORE    = 1u << 2,  /**< the work is expected and has not begun */
	FLAG_DURING    = 2u << 2,  /**< the work has begun and has not ended */
	FLAG_AFTER     = 3u << 2,  /**< the work has ended */

	/** The orders. Each order is a bit, so a word can contain several
	 *  orders that different callers gave.
	 */
	FLAG_START     = 1u << 4,  /**< the work is to begin, or to go on after
	                                it stopped */
	FLAG_END       = 1u << 5,  /**< the work is to end. Without FLAG_NOW,
	                                the work first finishes what is under
	                                way and accepts nothing new. */
	FLAG_NOW       = 1u << 6,  /**< the work is to stop at once and leave
	                                unfinished what is under way. With
	                                FLAG_END the work ends. Without
	                                FLAG_END the work can go on later. */

	/** The reasons. A reason says why the work ended or stopped early. */
	FLAG_CUT       = 1u << 7,  /**< a size limit was reached, so the output
	                                of the work is incomplete */
	FLAG_LATE      = 1u << 8,  /**< a time limit passed before the work
	                                ended */
};

/** The n-th bit that a module defines for its own flag word. n counts
 *  from 0. A word of 16 bits contains 7 such bits, a word of 32 bits 23
 *  and a word of 64 bits 55. A word of 8 bits contains none: it contains
 *  the common ranges without FLAG_LATE. The value is a uint64_t constant.
 */
#define FLAG_OWN(n) (UINT64_C(1) << (FLAG_BITS + (n)))

/** A range of @p width bits that a module defines for its own flag word,
 *  from its n-th bit on: the mask of the range. FLAG_OWN_VALUE() is the
 *  value @p v of that range, at the bits of the range.
 */
#define FLAG_OWN_RANGE(n, width)   (((UINT64_C(1) << (width)) - 1) << (FLAG_BITS + (n)))
#define FLAG_OWN_VALUE(n, v)       ((uint64_t)(v) << (FLAG_BITS + (n)))

struct flag8 {
	uint8_t bits;
};

struct flag16 {
	uint16_t bits;
};

struct flag32 {
	uint32_t bits;
};

struct flag64 {
	uint64_t bits;
};

/* The functions of one width. flagN() returns a word that contains
 * exactly the given bits. The other functions are documented at the
 * macros below, which select the function of the width of a word.
 */
#define FLAG_FUNCTIONS(n)                                                \
        static const_inline struct flag##n                               \
        flag##n (uint##n##_t bits)                                       \
        {                                                                \
                return (struct flag##n){ bits };                         \
        }                                                                \
                                                                         \
        static const_inline uint##n##_t                                  \
        flag##n##_get (struct flag##n f,                                 \
                       uint##n##_t    mask)                              \
        {                                                                \
                return f.bits & mask;                                    \
        }                                                                \
                                                                         \
        static const_inline bool                                         \
        flag##n##_any (struct flag##n f,                                 \
                       uint##n##_t    mask)                              \
        {                                                                \
                return f.bits & mask;                                    \
        }                                                                \
                                                                         \
        static const_inline bool                                         \
        flag##n##_all (struct flag##n f,                                 \
                       uint##n##_t    mask)                              \
        {                                                                \
                return (f.bits & mask) == mask;                          \
        }                                                                \
                                                                         \
        static force_inline void                                         \
        flag##n##_set (struct flag##n *f,                                \
                       uint##n##_t     bits)                             \
        {                                                                \
                f->bits |= bits;                                         \
        }                                                                \
                                                                         \
        static force_inline void                                         \
        flag##n##_clear (struct flag##n *f,                              \
                         uint##n##_t     bits)                           \
        {                                                                \
                f->bits &= (uint##n##_t)~bits;                           \
        }                                                                \
                                                                         \
        static force_inline void                                         \
        flag##n##_flip (struct flag##n *f,                               \
                        uint##n##_t     bits)                            \
        {                                                                \
                f->bits ^= bits;                                         \
        }                                                                \
                                                                         \
        static force_inline void                                         \
        flag##n##_put (struct flag##n *f,                                \
                       uint##n##_t     mask,                             \
                       uint##n##_t     value)                            \
        {                                                                \
                f->bits = (uint##n##_t)((f->bits & ~mask) | value);      \
        }

FLAG_FUNCTIONS(8)
FLAG_FUNCTIONS(16)
FLAG_FUNCTIONS(32)
FLAG_FUNCTIONS(64)

#undef FLAG_FUNCTIONS

/* Selects the function @p op of the width of the word @p f, or of the
 * word that the pointer @p f points to.
 */
#define FLAG_OF(f, op) _Generic((f),                                     \
        struct flag8:  flag8_##op,                                       \
        struct flag16: flag16_##op,                                      \
        struct flag32: flag32_##op,                                      \
        struct flag64: flag64_##op)
#define FLAG_AT(f, op) _Generic((f),                                     \
        struct flag8 *:  flag8_##op,                                     \
        struct flag16 *: flag16_##op,                                    \
        struct flag32 *: flag32_##op,                                    \
        struct flag64 *: flag64_##op)

/** Returns the bits of the word @p f that @p mask contains: the value of
 *  a range, at the bits of the range, if @p mask is the mask of a range.
 */
#define flag_get(f, mask)        FLAG_OF(f, get)((f), (mask))

/** Returns whether the word @p f contains at least one bit of @p mask. */
#define flag_any(f, mask)        FLAG_OF(f, any)((f), (mask))

/** Returns whether the word @p f contains every bit of @p mask. */
#define flag_all(f, mask)        FLAG_OF(f, all)((f), (mask))

/** Returns the value of the range FLAG_HOW of the word @p f. */
#define flag_how(f)              flag_get(f, FLAG_HOW)

/** Returns the value of the range FLAG_WHEN of the word @p f. */
#define flag_when(f)             flag_get(f, FLAG_WHEN)

/** Sets the bits @p bits in the word that @p f points to. */
#define flag_set(f, bits)        FLAG_AT(f, set)((f), (bits))

/** Clears the bits @p bits in the word that @p f points to. */
#define flag_clear(f, bits)      FLAG_AT(f, clear)((f), (bits))

/** Inverts the bits @p bits in the word that @p f points to. */
#define flag_flip(f, bits)       FLAG_AT(f, flip)((f), (bits))

/** Replaces the bits of @p mask in the word that @p f points to with
 *  @p value, which contains no bit outside @p mask. With the mask of a
 *  range, the call gives the range a new value.
 */
#define flag_put(f, mask, value) FLAG_AT(f, put)((f), (mask), (value))

#endif /* ASKAI_SRC_FLAG_H_ */
