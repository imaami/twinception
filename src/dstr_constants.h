/**
 * @file dstr_constants.h
 * @brief Constants for @ref dstr
 */
#ifndef TWINCEPTION_SRC_DSTR_CONSTANTS_H_
#define TWINCEPTION_SRC_DSTR_CONSTANTS_H_

#include <limits.h> /* CHAR_BIT */
#include <stddef.h> /* size_t */
#include <stdint.h> /* uintptr_t */

/**
 * @brief Architecture-specific compile-time constants for @ref dstr
 *
 * @note Every enumerator is paired with a pre-processor macro of the
 *       same name. This is a workaround to enforce the evaluated type
 *       when the enumerators are used in code. In C, an enumerator's
 *       evaluated type is `int` irrespective of the enumerated type's
 *       underlying type, which can be surprising. For example, given
 *       two conformant C compilers, the following program might abort
 *       when compiled with one but not the other.
 *
 *           #include <assert.h>
 *           int main(void) {
 *               enum E { X, Y };
 *               enum E x = X, y = Y;
 *               long long a = X - Y;
 *               long long b = x - y;
 *               assert(a == b);
 *           }
 */
enum dstr_constants {
	/** Basic allocation unit (allocation granularity) */
	DSTR_BLK_SIZE = 1U << (unsigned)CHAR_BIT,
#define DSTR_BLK_SIZE (size_t)DSTR_BLK_SIZE

	/** Largest growth step of a buffer grown by dstr_cat(),
	 *  dstr_cat_n() or dstr_printf(), as a shift of
	 *  @ref DSTR_BLK_SIZE (see dstr_cat()) */
	DSTR_GROW_SHIFT_MAX = 4U,
#define DSTR_GROW_SHIFT_MAX (size_t)DSTR_GROW_SHIFT_MAX

	/** Offset of the block count within the containing `uintptr_t` */
#if defined __BYTE_ORDER__ && defined __ORDER_BIG_ENDIAN__ && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
	DSTR_BLK_COUNT_POS = (unsigned)CHAR_BIT,
#else
	DSTR_BLK_COUNT_POS = 0U,
#endif
#define DSTR_BLK_COUNT_POS (size_t)DSTR_BLK_COUNT_POS

	/** Offset of the meta byte within the containing `uintptr_t` */
#if defined __BYTE_ORDER__ && defined __ORDER_BIG_ENDIAN__ && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
	DSTR_UPTR_META_POS = 0U,
#else
	DSTR_UPTR_META_POS = (sizeof (uintptr_t) - 1U) * (unsigned)CHAR_BIT,
#endif
#define DSTR_UPTR_META_POS (size_t)DSTR_UPTR_META_POS

	/**
	 * Size of the smallest possible @ref dstr type
	 * i.e. the minimum required `DSTR_OBJ_SIZE`
	 */
	DSTR_MIN_SIZE = 3U * sizeof (uintptr_t),
#define DSTR_MIN_SIZE (size_t)DSTR_MIN_SIZE

	/**
	 * Size of the largest possible @ref dstr type
	 * i.e. the maximum allowed `DSTR_OBJ_SIZE`
	 */
	DSTR_MAX_SIZE = 1U << ((unsigned)CHAR_BIT - 1U),
#define DSTR_MAX_SIZE (size_t)DSTR_MAX_SIZE

	/** Metadata bit flag indicating an allocated string */
	DSTR_PTR_FLAG = 1U << ((unsigned)CHAR_BIT - 1U),
#define DSTR_PTR_FLAG (unsigned char)DSTR_PTR_FLAG

	/** Metadata bits of an allocated string that count how many times
	 *  dstr_cat(), dstr_cat_n() and dstr_printf() have grown its
	 *  buffer, up to the mask's value */
	DSTR_GROW_COUNT_MASK = 0x0FU,
#define DSTR_GROW_COUNT_MASK (unsigned char)DSTR_GROW_COUNT_MASK
};

/* Mask for the block count bits without shift */
#define DSTR_BLK_COUNT_MASK (UINTPTR_MAX >> (size_t)CHAR_BIT)

#endif /* TWINCEPTION_SRC_DSTR_CONSTANTS_H_ */
