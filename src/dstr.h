/**
 * @file dstr.h
 * @brief Dynamic string with Short String Optimization for C
 *
 * A @ref dstr holds UTF-8 text and nothing else. Every function that
 * puts bytes into one expects them to be UTF-8, so data in any other
 * encoding must be converted at the boundary, before it goes in. The
 * library doesn't validate this yet.
 *
 * The null character U+0000 is valid UTF-8, a one-byte code point like
 * any other, and a @ref dstr may contain it: the object keeps its own
 * length, so it's never the terminator that ends the content. Mind the
 * difference when reading: dstr_len() counts every byte, but anything
 * that treats dstr_get() as a C string stops at the first null.
 *
 * Rationale behind some design choices:
 *
 * 1. Declaring an `extern` function in the function body of an `inline`
 *    function that calls it. This keeps internal function declarations
 *    out of global scope. As a general rule, a user of this API should
 *    only see identifiers _of_ the API in their IDE's tab completion.
 *
 * 2. Defining helper macros and then undefining them when no longer in
 *    use. See 1.
 *
 * 3. Using static assertions to break compilation in some situations.
 *    Appropriate when an accidentally successful build would be worse,
 *    irrespective of compiler warnings suppressed and/or overlooked.
 *
 * 4. No typedefs. Tediously typing out the full `struct` @ref dstr
 *    isn't a stylistic choice or some sacred tradition. A struct tag:
 *
 *      - allows the initializer function name to be @ref dstr();
 *      - can be forward-declared if needed, whereas a typedef cannot;
 *      - communicates type information explicitly.
 *
 * 5. The initializer function returns a struct by value, not a pointer.
 *    This follows from the intended use case of temporary variables and
 *    persistent work buffers in structs. It's also why @ref dstr
 *    employs Short String Optimization.
 *
 * 6. No support for C++ mode. @ref dstr is fundamentally incompatible
 *    with both C++ syntax and language rules. In C++ code `std::string` is
 *    the reasonable choice - after all, @ref dstr is modeled after
 *    `std::string` in the first place.
 *
 * 7. dstr() is a macro that picks the initialization at compile time.
 *    It expands at the caller's site, so the helper macros it uses can't
 *    be undefined like the others (see 2). Their names end in `_`, like
 *    the internal inline functions behind the API.
 */
#ifndef TWINCEPTION_SRC_DSTR_H_
#define TWINCEPTION_SRC_DSTR_H_

#include <stdarg.h> /* va_list */
#include <stdbool.h>
#include <string.h> /* strlen() */

#ifdef __clang__
# pragma clang diagnostic push
# pragma clang diagnostic ignored "-Wunknown-warning-option"
# pragma clang diagnostic ignored "-Wdeclaration-after-statement"
# pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
# pragma clang diagnostic ignored "-Wimplicit-void-ptr-cast"
# pragma clang diagnostic ignored "-Wpre-c11-compat"
# pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
# pragma clang diagnostic ignored "-Wunused-function"
# pragma clang diagnostic ignored "-Wvla"
#endif

#include "dstr_constants.h"

/* Default object size */
#ifndef DSTR_OBJ_SIZE
# define DSTR_OBJ_SIZE DSTR_MIN_SIZE
#endif

/* Compile-time validate object size; these are non-negotiable */
_Static_assert(DSTR_OBJ_SIZE <= DSTR_MAX_SIZE,
               "sizeof (struct dstr) is too large");
_Static_assert(DSTR_OBJ_SIZE >= DSTR_MIN_SIZE,
               "sizeof (struct dstr) is too small");
_Static_assert(!(DSTR_OBJ_SIZE % sizeof (uintptr_t)),
               "sizeof (struct dstr) is not a multiple of sizeof (uintptr_t)");

/**
 * @brief A dynamic UTF-8 string object
 *
 * Initialize every object with dstr() or @ref DSTR_INIT before use. An
 * all-zero object is not an empty string.
 */
struct dstr {
	union {
		unsigned char raw[DSTR_OBJ_SIZE];
		char          str[DSTR_OBJ_SIZE];
		uintptr_t     ptr[DSTR_OBJ_SIZE / sizeof (uintptr_t)];

		/** The in-place string and the meta byte as separate members,
		 *  so that an initializer can set both (see dstr()) */
		struct {
			char          str[DSTR_OBJ_SIZE > 1 ?
			                  DSTR_OBJ_SIZE - 1 : 1];
			unsigned char meta;
		} sso;
	};
};

/* This is unlikely to happen, but we need to be 100% certain */
_Static_assert(sizeof (struct dstr) ==
               sizeof (struct dstr){0}.raw,
               "struct dstr contains padding bytes");

/**
 * @brief Initializer for an empty @ref dstr
 *
 * Usable where dstr() isn't, e.g. for objects with static storage
 * duration and in initializers of structs with @ref dstr members.
 * Objects zeroed by `{0}`, calloc() or memset() are not valid: their
 * meta byte reads as a full string of null bytes.
 */
#define DSTR_INIT \
  { .raw = { [sizeof (struct dstr) - \
              (sizeof (struct dstr) != 0U)] = \
             (unsigned char)(sizeof (struct dstr) - 1U) } }

enum dstr_state {
	DSTR_IS_NOT = 0U,
#define DSTR_IS_NOT (size_t)DSTR_IS_NOT

	DSTR_IS_SSO = 1U << 0U,
#define DSTR_IS_SSO (size_t)DSTR_IS_SSO

	DSTR_IS_PTR = 1U << 1U,
#define DSTR_IS_PTR (size_t)DSTR_IS_PTR

	DSTR_IS_BAD = DSTR_IS_SSO
	            | DSTR_IS_PTR,
#define DSTR_IS_BAD (size_t)DSTR_IS_BAD
};

struct dstr_info {
	size_t blocks;
	size_t length;
	size_t space;
	size_t state;
};

#define dstr_last_byte_(obj_size, obj_raw) (obj_raw)[(obj_size) - 1U]
#define dstr_last_byte(obj) dstr_last_byte_(sizeof *(obj), (obj)->raw)

#define dstr_last_uptr_const_(obj_size, obj_raw) \
  ((uintptr_t const *)&(obj_raw)[0])[(obj_size) / sizeof (uintptr_t) - 1U]

#define dstr_last_uptr_(obj_size, obj_raw) \
  ((uintptr_t *)&(obj_raw)[0])[(obj_size) / sizeof (uintptr_t) - 1U]

#define dstr_last_uptr(obj) \
  (obj)->ptr[sizeof *(obj) / sizeof (obj)->ptr[0] - 1U]

#define dstr_is_sso_(obj_size, obj_raw) \
  (dstr_last_byte_(obj_size, obj_raw) < (obj_size))

#define dstr_is_sso(obj) dstr_is_sso_(sizeof *(obj), (obj)->raw)

#define dstr_is_ptr_(obj_size, obj_raw) \
  (dstr_last_byte_(obj_size, obj_raw) & DSTR_PTR_FLAG)

#define dstr_is_ptr(obj) dstr_is_ptr_(sizeof *(obj), (obj)->raw)

#define dstr_sso_space_left_(obj_size, obj_raw) \
  dstr_last_byte_(obj_size, obj_raw)

#define dstr_sso_space_left(obj) \
  dstr_sso_space_left_(sizeof *(obj), (obj)->raw)

#define dstr_sso_len_(obj_size, obj_raw) \
  ((obj_size) - 1U - dstr_last_byte_(obj_size, obj_raw))

#define dstr_sso_len(obj) dstr_sso_len_(sizeof *(obj), (obj)->raw)

#define dstr_ptr_len_(obj_uptr) \
  (size_t)((unsigned char const *)((obj_uptr)[1]) \
         - (unsigned char const *)((obj_uptr)[0]))

#define dstr_ptr_len(obj) dstr_ptr_len_((obj)->ptr)

#define dstr_uptr_block_count_(up) \
  (((up) >> DSTR_BLK_COUNT_POS) & DSTR_BLK_COUNT_MASK)

#define dstr_ptr_block_count_(obj_size, obj_raw) \
  dstr_uptr_block_count_(dstr_last_uptr_const_(obj_size, obj_raw))

#define dstr_ptr_block_count(obj) \
  dstr_uptr_block_count_(dstr_last_uptr(obj))

#define dstr_ptr_space_left_(obj_size, obj_raw) \
  (dstr_ptr_block_count_(obj_size, obj_raw) * DSTR_BLK_SIZE \
   - 1U - dstr_ptr_len_((uintptr_t const *)(obj_raw)))

#define dstr_ptr_space_left(obj) \
  (dstr_ptr_block_count(obj) * DSTR_BLK_SIZE \
   - 1U - dstr_ptr_len(obj))

/* Note: MSVC support isn't actually planned */
#ifndef _MSC_VER
# define dstr_inline       inline __attribute__((always_inline))
# define dstr_const_inline dstr_inline __attribute__((const))
# define dstr_useless      __attribute__((unused))
# define dstr_printf_like(fmt_idx, arg_idx) \
  __attribute__((format(printf, fmt_idx, arg_idx)))
#else
# define dstr_inline       __forceinline
# define dstr_const_inline __forceinline
# define dstr_useless
# define dstr_printf_like(fmt_idx, arg_idx)
#endif

#define dstr_auto \
  __attribute__((cleanup(dstr_cleanup))) struct dstr

/**
 * @brief Initialize from `len` characters at `src`, without measuring
 *
 * Internal. A string that fits in place is copied inline, so that the
 * compiler can see through it; a longer one is allocated by the
 * library.
 */
static dstr_inline struct dstr
dstr_n_ (char const *const src,
         size_t const      len)
{
	struct dstr ret = DSTR_INIT;

	if (len < sizeof ret) {
		(void)memcpy(&ret.str[0], src, len);
		dstr_last_byte(&ret) -= (unsigned char)len;
		return ret;
	}

	extern int
	dstr_init_n_ (size_t         obj_size,
	              unsigned char  obj_raw[obj_size],
	              char const    *src_str,
	              size_t         len);
	(void)dstr_init_n_(sizeof ret, &ret.raw[0], src, len);

	return ret;
}

/**
 * @brief RAII-style initializer for @ref dstr
 *
 * Passing `NULL` is equivalent to passing an empty string (`""`).
 *
 * A string whose length the compiler knows is copied inline, so that a
 * local object can fold into constants. Any other string is measured
 * and copied by the library, which keeps the call site small.
 *
 * @note Doesn't return a pointer - this is a direct initializer
 *
 * @param str Pointer to an initial UTF-8 string, or `NULL`
 * @return Initialized @ref dstr object by value
 */
static dstr_inline struct dstr
dstr (char const *const str)
{
	if (__builtin_constant_p(!str) && !str)
		return (struct dstr)DSTR_INIT;
	if (str && __builtin_constant_p(strlen(str)))
		return dstr_n_(str, strlen(str));

	struct dstr ret = DSTR_INIT;

	extern int
	dstr_init_ (size_t         obj_size,
	            unsigned char  obj_raw[obj_size],
	            char const    *src_str);
	(void)dstr_init_(sizeof ret, &ret.raw[0], str);

	return ret;
}

#if defined __clang__ && __clang_major__ < 9
/* Clang 6 to 8 crash on the literal path below */
# define dstr(x) (dstr)(x)
#else

/* Whether `x` is an array of `char`. Internal; stays defined (see 7) */
# define dstr_is_array_(x) _Generic((__typeof__(x) *)0, \
  char (*)[sizeof (__typeof__(x))]: 1, \
  char const (*)[sizeof (__typeof__(x))]: 1, \
  default: 0)

/* Whether `x` is a string literal. Internal; stays defined (see 7) */
# define dstr_is_literal_(x) \
  __builtin_choose_expr(dstr_is_array_(x), __builtin_constant_p(x), 0)

/* `x` if it is a string literal of at most `n` bytes, otherwise "": the
 * branches of __builtin_choose_expr() that aren't taken are still type
 * checked. Internal; stays defined (see 7)
 */
# define dstr_fit_(x, n) \
  __builtin_choose_expr( \
    dstr_is_literal_(x) && sizeof (__typeof__(x)) <= (n), x, "")

/* Initializer for a string literal: a constant if it fits in place
 * (the meta byte of a full one is its terminator), otherwise allocated
 * without measuring. Internal; stays defined (see 7)
 */
# define dstr_literal_(x) \
  __builtin_choose_expr( \
    sizeof (__typeof__(x)) < sizeof (struct dstr), \
    __extension__ (struct dstr){ .sso = { \
      .str  = dstr_fit_(x, sizeof (struct dstr) - 1U), \
      .meta = (unsigned char)(sizeof (struct dstr) - \
                              sizeof (__typeof__(x))) } }, \
    __builtin_choose_expr( \
      sizeof (__typeof__(x)) == sizeof (struct dstr), \
      __extension__ (struct dstr){ \
        .str = dstr_fit_(x, sizeof (struct dstr)) }, \
      dstr_n_(dstr_fit_(x, SIZE_MAX), \
              sizeof (__typeof__(x)) - 1U)))

/**
 * @brief Initialize a @ref dstr, choosing how at compile time
 *
 * A string literal that fits in place becomes a constant: no code runs,
 * and a local object can vanish from the compiled program. A longer
 * literal is allocated without being measured. Anything else, including
 * a literal with an embedded null character, goes to the dstr()
 * function; `(dstr)(x)` calls it directly.
 *
 * `x` is evaluated once. It can't be an array of unknown size, such as
 * a flexible array member, or a variable length array; pass those as
 * `(dstr)(x)`. Not for static initializers: use @ref DSTR_INIT.
 *
 * @param x A UTF-8 string literal, a pointer to a UTF-8 string or
 *          `char` array, or `NULL`
 * @return Initialized @ref dstr object by value
 */
# define dstr(x) \
  __builtin_choose_expr( \
    dstr_is_literal_(x) && \
    __builtin_strlen(dstr_fit_(x, SIZE_MAX)) == \
    sizeof (__typeof__(x)) - 1U, \
    dstr_literal_(x), \
    (dstr)(x))

#endif /* __clang__ && __clang_major__ < 9 */

/**
 * @brief Zero out string content, free allocated memory, and reset the
 *        @ref dstr object to an empty state
 *
 * Reusing the @ref dstr object is safe after the call, it becomes a
 * freshly initialized empty string. Passing `NULL` is a no-op.
 *
 * @note The pointed-to @ref dstr object itself is _not_ freed,
 *       only its internal resources are. This is in line with the
 *       intended use of @ref dstr, i.e. a local variable or
 *       struct member, not a pointer.
 *
 * @param ds Pointer to the @ref dstr object, or `NULL`
 */
static dstr_inline void
dstr_cleanup (struct dstr *const ds)
{
	extern void
	dstr_free_ (void *buf,
	            void *end);
	if (!ds)
		return;

	/* The library gets the heap buffer, never the object, so that the
	 * object's address doesn't escape; the object is reset inline */
	if (dstr_is_ptr(ds))
		dstr_free_((void *)ds->ptr[0], (void *)ds->ptr[1]);
	*ds = (struct dstr)DSTR_INIT;
}

/**
 * @brief Get the string length without null terminator
 *
 * Unlike `strlen()`, this counts the null characters in the content.
 *
 * @param ds Pointer to the @ref dstr object
 * @return The string length in bytes
 */
static dstr_inline size_t
dstr_len (struct dstr const *const ds)
{
	if (ds) {
		if (dstr_is_sso(ds))
			return dstr_sso_len(ds);
		if (dstr_is_ptr(ds) &&
		    (char *)ds->ptr[0] && (char *)ds->ptr[1])
			return dstr_ptr_len(ds);
	}
	return 0U;
}

/**
 * @brief Get the number of blocks allocated by a @ref dstr
 *
 * If the content string is a short string (stored in-place) the
 * return value is 0. Block size is `DSTR_BLK_SIZE` (typically
 * 256 bytes); multiply with it to get the total allocated
 * buffer size in bytes.
 *
 * @param ds Pointer to the @ref dstr object
 * @return The allocated block count
 */
static dstr_inline size_t
dstr_get_block_count (struct dstr const *const ds)
{
	if (!ds || !dstr_is_ptr(ds))
		return 0U;
	return dstr_ptr_block_count(ds);
}

/**
 * @brief Get the number of bytes still available in the string buffer
 *        if no reallocation were to occur
 *
 * "Buffer" here can mean either the in-place storage or an allocated
 * buffer. The returned value means how many bytes can be appended so
 * that the resulting string is null-terminated, without allocating a
 * larger buffer.
 *
 * For example, if @ref dstr_len() returns 250, and buffer capacity is
 * 256 bytes, then @ref dstr_space_left() will return 5.
 *
 * @param ds Pointer to the @ref dstr object
 * @return Available space still left in the buffer before more needs
 *         to be allocated
 */
static dstr_inline size_t
dstr_space_left (struct dstr const *const ds)
{
	if (ds) {
		if (dstr_is_sso(ds))
			return dstr_sso_space_left(ds);

		if (dstr_is_ptr(ds) &&
		    (char *)ds->ptr[0] && (char *)ds->ptr[1])
			return dstr_ptr_space_left(ds);
	}
	return 0U;
}

static dstr_inline struct dstr_info
dstr_info (struct dstr const *const ds)
{
	if (ds) {
		if (dstr_is_sso(ds)) {
			return (struct dstr_info){
				.length = dstr_sso_len(ds),
				.space  = dstr_sso_space_left(ds),
				.state  = DSTR_IS_SSO
			};
		}

		if (dstr_is_ptr(ds) && (char *)ds->ptr[0]
		                    && (char *)ds->ptr[1]) {
			size_t const blk = dstr_ptr_block_count(ds);
			if (blk) {
				size_t const size = blk * DSTR_BLK_SIZE;
				size_t const len = dstr_ptr_len(ds);
				if (len < size) {
					return (struct dstr_info){
						.blocks = blk,
						.length = len,
						.space  = size - 1U - len,
						.state  = DSTR_IS_PTR
					};
				}
			}
		}

		return (struct dstr_info){
			.state  = DSTR_IS_BAD
		};
	}

	return (struct dstr_info){
		.state = DSTR_IS_NOT
	};
}

/**
 * @brief Get a pointer to the (read-only) content string
 *
 * The content is null-terminated, but it may contain null characters
 * too: use dstr_len() for its length.
 *
 * @param ds Pointer to the @ref dstr object.
 * @return Pointer to the content string
 */
static dstr_inline char const *
dstr_get (struct dstr const *const ds)
{
	if (ds) {
		if (dstr_is_sso(ds))
			return &ds->str[0];
		if (dstr_is_ptr(ds))
			return (char const *)ds->ptr[0];
	}
	return NULL;
}

/**
 * @brief Get a pointer to the string buffer's raw content bytes
 *
 * The bytes can be modified in place, but they must stay UTF-8.
 *
 * @param ds Pointer to the @ref dstr object.
 * @return Pointer to the raw content bytes
 */
static dstr_inline unsigned char *
dstr_raw (struct dstr *const ds)
{
	if (ds) {
		if (dstr_is_sso(ds))
			return &ds->raw[0];
		if (dstr_is_ptr(ds))
			return (unsigned char *)ds->ptr[0];
	}
	return NULL;
}

/**
 * @brief Get a pointer to the null byte following the content string
 *
 * Returns a pointer to writable memory - which doesn't imply writing
 * to it is automatically OK. Preferably you should use the functions
 * meant for modifying the string content.
 *
 * @param ds Pointer to the @ref dstr object.
 */
static dstr_inline char *
dstr_get_write_ptr (struct dstr *const ds)
{
	if (ds) {
		if (dstr_is_sso(ds))
			return &ds->str[
				dstr_sso_len(ds)
			];
		if (dstr_is_ptr(ds) && (char *)ds->ptr[0])
			return (char *)ds->ptr[1];
	}
	return NULL;
}

/**
 * @brief Internal implementation of dstr_reserve() and the growth
 *        of dstr_cat_n() and dstr_cat()
 *
 * @param ds    Pointer to the @ref dstr object
 * @param len   Number of additional bytes to reserve
 * @param exact Round the new size up to a multiple of the block size
 *              only, and don't count the growth
 */
static dstr_inline struct dstr *
dstr_reserve_ (struct dstr *const ds,
               size_t const       len,
               _Bool const        exact)
{
	struct dstr_info info = dstr_info(ds);

	switch (info.state) {
	case DSTR_IS_SSO:
	case DSTR_IS_PTR:
		if (len <= info.space)
			return ds; // Enough space already
		break;
	default:
		return NULL; // Null object or broken state
	}

	extern int
	dstr_grow_ (size_t        obj_size,
	            unsigned char obj_raw[obj_size],
	            size_t        len,
	            _Bool         exact);

	/* Grow a copy, so that the object's address doesn't escape */
	struct dstr tmp = *ds;
	if (dstr_grow_(sizeof tmp, &tmp.raw[0], len, exact))
		return NULL;

	*ds = tmp;
	return ds;
}

/**
 * @brief Ensure it's safe to write `len` more bytes of text to `ds`
 *
 * May allocate more memory, in which case the current content string
 * is copied unmodified to the new buffer.
 *
 * Unused buffer space is taken into account when calculating how much
 * additional memory, if any, to allocate. If there's already enough,
 * nothing is done and `ds` is returned.
 *
 * The buffer grows to the smallest multiple of `DSTR_BLK_SIZE` that
 * holds the string, `len` more bytes and the terminator, with no spare
 * blocks. This suits sizes planned ahead. A string that grows by many
 * appends should leave the growing to dstr_cat(), which makes room by
 * itself and rounds up further: calling this function before every
 * append makes every growth exact.
 *
 * If `len` is 0, nothing is done and `ds` is returned.
 *
 * If `ds` is `NULL`, nothing is done and `NULL` is returned.
 *
 * @note If calling this function returns non-null, all references to
 *       the string content of `ds` should be considered invalid and
 *       re-acquired.
 *
 * @param ds Pointer to the @ref dstr object
 * @param len Number of additional bytes to reserve
 *
 * @return Pointer to the input object on success (including when no
 *         action needed to be taken), otherwise `NULL`
 * @retval `NULL` if `ds` is `NULL`
 */
static dstr_inline struct dstr *
dstr_reserve (struct dstr *const ds,
              size_t const       len)
{
	return dstr_reserve_(ds, len, true);
}

/**
 * @brief Append `len` bytes of UTF-8 text to a @ref dstr object
 *
 * Exactly `len` bytes are copied from `src` and followed by a null
 * terminator. They may include null characters (U+0000 is UTF-8 too),
 * and `src` needn't be null-terminated: dstr_len() counts every byte,
 * while dstr_get() reads as a C string up to the first null character.
 *
 * On success, `ds` is returned. If `len` is 0, nothing is done and `ds`
 * is returned, whatever `src` is. If `ds` is `NULL`, or `src` is `NULL`
 * and `len` isn't 0, nothing is done and `NULL` is returned. If memory
 * allocation fails, nothing is done and `NULL` is returned.
 *
 * The buffer grows like it does for dstr_cat().
 *
 * @note `src` must not point into the string held by `ds`, for the
 *       reason given for dstr_cat().
 *
 * @param ds  Pointer to the @ref dstr object
 * @param src Pointer to the bytes to append
 * @param len Number of bytes to append
 * @return Pointer to the input object on success, otherwise `NULL`
 */
static dstr_inline struct dstr *
dstr_cat_n (struct dstr *const ds,
            char const *const  src,
            size_t const       len)
{
	if (!len)
		return ds;
	if (!src)
		return NULL;

	char *const ptr = dstr_get_write_ptr(
		dstr_reserve_(ds, len, false));
	if (!ptr)
		return NULL;

	(void)memcpy(ptr, src, len);

	if (dstr_is_ptr(ds))
		ds->ptr[1] += len;
	else if (dstr_is_sso(ds))
		dstr_last_byte(ds) -= (unsigned char)len;

	/* This right here, inserting a null byte, MUST be done
	 * _after_ the length is subtracted from the meta byte.
	 * Otherwise, if the moon and the stars are just right,
	 * there shall be an overflow and a segmentation fault.
	 */
	ptr[len] = '\0';

	return ds;
}

/**
 * @brief Append a null-terminated UTF-8 string to a @ref dstr object
 *
 * On success, `ds` is returned. If `src` is `NULL` or points to an
 * empty string, nothing is done and `ds` is returned. If `ds` is
 * `NULL`, nothing is done and `NULL` is returned. If memory allocation
 * fails, nothing is done and `NULL` is returned.
 *
 * When the buffer must grow, its new size is rounded up to a multiple
 * of a step. The step starts at `DSTR_BLK_SIZE` and doubles with every
 * growth by an append (this function, dstr_cat_n() or dstr_printf(),
 * which share the step), up to `DSTR_BLK_SIZE` shifted left by
 * `DSTR_GROW_SHIFT_MAX`. Appends that fit, up to the last byte of the
 * buffer, never allocate.
 *
 * @note `src` must not point into the string held by `ds`. As with
 *       overlapping memcpy() arguments, the behavior is undefined: if
 *       the buffer grows, `src` is freed or overwritten before it is
 *       read.
 */
static dstr_inline struct dstr *
dstr_cat (struct dstr *const ds,
          char const *const  src)
{
	return dstr_cat_n(ds, src, src ? strlen(src) : 0U);
}

/**
 * @brief Append formatted text to a @ref dstr object à la `snprintf()`
 *
 * The text that `snprintf()` would make of `fmt` and the arguments is
 * appended; like all @ref dstr content, it must be UTF-8. On success,
 * `ds` is returned, also when the text is empty. If `ds` or `fmt` is
 * `NULL`, or the text can't be formatted (as when a wide character has
 * no multibyte encoding, or the text would be longer than `INT_MAX`
 * bytes), nothing is appended and `NULL` is returned. If memory
 * allocation fails, nothing is done and `NULL` is returned.
 *
 * The buffer grows like it does for dstr_cat().
 *
 * @note No argument may point into the string held by `ds`. The text is
 *       formatted twice, first to measure it and then into the buffer:
 *       a growth in between frees or moves what the pointer points to,
 *       and without one the text overwrites what it is made of. As with
 *       dstr_cat(), the behavior is undefined.
 *
 * @param ds  Pointer to the @ref dstr object
 * @param fmt `printf()` format string
 * @param ... Arguments for `fmt`
 * @return Pointer to the input object on success, otherwise `NULL`
 */
dstr_printf_like(2, 3) static inline struct dstr *
dstr_printf (struct dstr *const ds,
             char const *const  fmt,
             ...)
{
	if (!ds || !fmt)
		return NULL;

	extern int
	dstr_vprintf_ (size_t         obj_size,
	               unsigned char  obj_raw[obj_size],
	               char const    *fmt,
	               va_list        ap) dstr_printf_like(3, 0);

	/* Format into a copy, so that the object's address doesn't escape.
	 * The library leaves the copy valid even when it fails. */
	struct dstr tmp = *ds;
	va_list ap;
	va_start(ap, fmt);
	int const err = dstr_vprintf_(sizeof tmp, &tmp.raw[0], fmt, ap);
	va_end(ap);

	*ds = tmp;
	return err ? NULL : ds;
}

/**
 * @brief Erase bytes from the middle of a @ref dstr object à la
 *        `std::string::erase()`
 *
 * Removes `len` bytes at offset `pos`, or all of them from `pos` to the
 * end of the string if there are fewer, and moves the rest down. The
 * buffer stays as it is, and so does the growth step of the appends to
 * come. The bytes that the string no longer covers are wiped.
 *
 * On success, `ds` is returned. If `pos` is the length of the string or
 * `len` is 0, nothing is done and `ds` is returned. If `ds` is `NULL`
 * or not a valid object, or `pos` is past the end of the string,
 * nothing is done and `NULL` is returned.
 *
 * @param ds  Pointer to the @ref dstr object
 * @param pos Offset of the first byte to erase
 * @param len Number of bytes to erase
 * @return Pointer to the input object on success, otherwise `NULL`
 */
static dstr_inline struct dstr *
dstr_erase (struct dstr *const ds,
            size_t const       pos,
            size_t const       len)
{
	struct dstr_info const info = dstr_info(ds);
	if ((info.state != DSTR_IS_SSO && info.state != DSTR_IS_PTR) ||
	    pos > info.length)
		return NULL; // Null object, broken state or out of range

	size_t const n = len < info.length - pos ? len : info.length - pos;
	if (!n)
		return ds;

	/* Move the rest down and wipe the bytes it leaves behind, up to
	 * the old terminator */
	size_t const end = info.length - n;
	char *const str = info.state == DSTR_IS_PTR ? (char *)ds->ptr[0]
	                                            : &ds->str[0];
	(void)memmove(&str[pos], &str[pos + n], end - pos);
	(void)memset(&str[end], 0, n);

	if (info.state == DSTR_IS_PTR)
		ds->ptr[1] -= n;
	else
		dstr_last_byte(ds) += (unsigned char)n;

	return ds;
}

/**
 * @brief Empty a @ref dstr object but keep its buffer
 *
 * Unlike dstr_cleanup(), which frees an allocated buffer, this keeps it
 * for the appends to come; the content is wiped. Equivalent to
 * `dstr_erase(ds, 0, SIZE_MAX)`.
 *
 * @param ds Pointer to the @ref dstr object
 * @return Pointer to the input object on success, otherwise `NULL`
 * @retval `NULL` if `ds` is `NULL` or not a valid object
 */
static dstr_inline struct dstr *
dstr_clear (struct dstr *const ds)
{
	return dstr_erase(ds, 0U, SIZE_MAX);
}

/**
 * @brief Move the content of a @ref dstr object to a new one
 *
 * Returns the object by value and resets `*ds` to an empty string. An
 * allocated buffer changes owners: nothing is copied, allocated or
 * freed. `*ds` can be used again like a freshly initialized object.
 * Passing `NULL` returns an empty string.
 *
 * @note Doesn't return a pointer - like dstr(), this is a direct
 *       initializer
 *
 * @param ds Pointer to the @ref dstr object, or `NULL`
 * @return The moved @ref dstr object by value
 */
static dstr_inline struct dstr
dstr_move (struct dstr *const ds)
{
	if (!ds)
		return (struct dstr)DSTR_INIT;

	struct dstr const ret = *ds;
	*ds = (struct dstr)DSTR_INIT;
	return ret;
}

#ifdef __clang__
# pragma clang diagnostic pop
#endif

/* Don't move this include statement or reorder lines in relation to it! */
#include "dstr_priv.h"

#endif /* TWINCEPTION_SRC_DSTR_H_ */
