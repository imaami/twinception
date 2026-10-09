/**
 * @file dstr.c
 * @brief Dynamic string with Short String Optimization
 */
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dstr_priv.h"

#ifdef __clang__
# pragma clang diagnostic ignored "-Wunknown-warning-option"
# pragma clang diagnostic ignored "-Wcast-align"
# pragma clang diagnostic ignored "-Wdeclaration-after-statement"
# pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
# pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
# pragma clang diagnostic ignored "-Wimplicit-void-ptr-cast"
# pragma clang diagnostic ignored "-Wmissing-prototypes"
# pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
# pragma clang diagnostic ignored "-Wunused-function"
# pragma clang diagnostic ignored "-Wvla"
#endif

/* Pre-C23 `typeof` backwards compatibility macro for GCC & Clang */
#ifdef __clang__
# if __STDC_VERSION__ < 202000L || __clang_major__ < 16
#  define typeof __typeof__
# endif // __STDC_VERSION__ < 202000L || __clang_major__ < 16
#elif defined __GNUC__ && defined __STRICT_ANSI__
# if __STDC_VERSION__ < 202000L || __GNUC__ < 13
#  define typeof __typeof__
# endif // __STDC_VERSION__ < 202000L || __GNUC__ < 13
#endif // __clang__ || (__GNUC__ && __STRICT_ANSI__)

#ifndef NDEBUG
# define dstr_errno(err, fmt, ...) \
  (void)fprintf(stderr, "%s: " fmt "%s%s\n", __func__, ##__VA_ARGS__, \
                &": "[(unsigned)(!(err) || !(fmt)[0]) << 1U], \
                (err) ? strerror((err)) : "")
# define dstr_error(fmt, ...) \
  (void)fprintf(stderr, "%s: " fmt "\n", __func__, ##__VA_ARGS__)
#else /* NDEBUG */
# define dstr_errno(err, fmt, ...) do{}while(0)
# define dstr_error(fmt, ...) do{}while(0)
#endif /* NDEBUG */

dstr_useless static dstr_inline size_t
dstr_len_ (size_t const        obj_size,
           unsigned char const obj_raw[const obj_size])
{
	if (dstr_is_sso_(obj_size, obj_raw))
		return dstr_sso_len_(obj_size, obj_raw);

	if (dstr_is_ptr_(obj_size, obj_raw)) {
		typedef uintptr_t obj_type[obj_size / sizeof (uintptr_t)];
		obj_type const *const dest = (typeof(dest))obj_raw;
		if ((char const *)(*dest)[0] && (char const *)(*dest)[1])
			return dstr_ptr_len_((uintptr_t const *)(obj_raw));
	}

	return 0U;
}

dstr_useless static dstr_inline size_t
dstr_space_left_ (size_t const        obj_size,
                  unsigned char const obj_raw[const obj_size])
{
	if (dstr_is_sso_(obj_size, obj_raw))
		return dstr_sso_space_left_(obj_size, obj_raw);

	if (dstr_is_ptr_(obj_size, obj_raw)) {
		typedef uintptr_t obj_type[obj_size / sizeof (uintptr_t)];
		obj_type const *const dest = (typeof(dest))obj_raw;
		if ((char const *)(*dest)[0] && (char const *)(*dest)[1])
			return dstr_ptr_space_left_(obj_size, obj_raw);
	}

	return 0U;
}

/**
 * @brief Initialize a `dstr` object of runtime-specified size from
 *        a string of known length
 *
 * @note Don't call this directly, use the `dstr()` wrapper.
 *
 * @param[in]  obj_size The object's size in bytes
 * @param[out] obj_raw  Pointer to the object itself, freshly initialized
 * @param[in]  src_str  Pointer to the source string
 * @param[in]  len      Length of the source string
 *
 * @return 0 on success or an errno value on failure
 */
int
dstr_init_n_ (size_t const      obj_size,
              unsigned char     obj_raw[const obj_size],
              char const *const src_str,
              size_t const      len)
{
	if (!len)
		return 0;

	typedef uintptr_t obj_type[obj_size / sizeof (uintptr_t)];
	obj_type *const dest = (typeof(dest))obj_raw;

	if (len < obj_size) {
		(void)memcpy(obj_raw, src_str, len);
		dstr_last_byte_(obj_size, obj_raw) -= (unsigned char)len;
		return 0;
	}

	/* Allocation required */

	size_t const n_blocks = (len >> (size_t)CHAR_BIT) + 1U;
	if (n_blocks > DSTR_BLK_COUNT_MASK)
		return EOVERFLOW;

	unsigned char *const buf = calloc(n_blocks, DSTR_BLK_SIZE);
	if (!buf)
		return errno ? errno : ENOMEM;

	(void)memcpy(buf, src_str, len);

	(*dest)[0] = (uintptr_t)buf;
	(*dest)[1] = (uintptr_t)&buf[len];
	dstr_last_uptr_(obj_size, obj_raw) = (uintptr_t)n_blocks
	                                   << DSTR_BLK_COUNT_POS;
	dstr_last_byte_(obj_size, obj_raw) |= DSTR_PTR_FLAG;

	return 0;
}

/**
 * @brief Initialize a `dstr` object of runtime-specified size
 *
 * @note Don't call this directly, use the `dstr()` wrapper.
 *
 * @param[in]  obj_size The object's size in bytes
 * @param[out] obj_raw  Pointer to the object itself, freshly initialized
 * @param[in]  src_str  Pointer to the source string, or `NULL`
 *
 * @return 0 on success or an errno value on failure
 */
int
dstr_init_ (size_t const      obj_size,
            unsigned char     obj_raw[const obj_size],
            char const *const src_str)
{
	if (!src_str)
		return 0;
	return dstr_init_n_(obj_size, obj_raw, src_str, strlen(src_str));
}

/**
 * @brief Wipe and free a heap buffer
 *
 * @param[in] buf The buffer, or `NULL`
 * @param[in] end The end of the string in it, or `NULL`
 */
void
dstr_free_ (void *const buf,
            void *const end)
{
	if (!buf)
		return;
	if (end)
		(void)memset(buf, 0, (size_t)((unsigned char *)end -
		                              (unsigned char *)buf));
	free(buf);
}

static dstr_inline struct dstr_info
dstr_info_ (size_t const        obj_size,
            unsigned char const obj_raw[const obj_size])
{
	if (!obj_size || !obj_raw)
		return (struct dstr_info){
			.state  = DSTR_IS_NOT
		};

	if (dstr_is_sso_(obj_size, obj_raw))
		return (struct dstr_info){
			.length = dstr_sso_len_(obj_size, obj_raw),
			.space  = dstr_sso_space_left_(obj_size, obj_raw),
			.state  = DSTR_IS_SSO
		};

	if (dstr_is_ptr_(obj_size, obj_raw)) {
		typedef uintptr_t uptr_arr[obj_size / sizeof (uintptr_t)];
		uptr_arr const *const uptr = (uptr_arr const *)obj_raw;

		if ((unsigned char const *)(*uptr)[0] &&
		    (unsigned char const *)(*uptr)[1]) {
			size_t const blk = dstr_ptr_block_count_(obj_size,
			                                         obj_raw);
			if (blk) {
				size_t const size = blk * DSTR_BLK_SIZE;
				size_t const len = dstr_ptr_len_(*uptr);
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
	}

	return (struct dstr_info){
		.state  = DSTR_IS_BAD
	};
}

/**
 * @brief Resize a `dstr` object's buffer, truncating the content
 *        string if necessary
 */
int
dstr_resize_ (size_t const  obj_size,
              unsigned char obj_raw[const obj_size],
              size_t        n_blocks)
{
	/* Prevent block count field overflow */
	if (n_blocks > DSTR_BLK_COUNT_MASK) {
		dstr_errno(EOVERFLOW, "n_blocks=%zu", n_blocks);
		return EOVERFLOW;
	}

	struct dstr_info i = dstr_info_(obj_size, obj_raw);
	switch (i.state) {
	case DSTR_IS_NOT:
		dstr_error("Null object pointer");
		return EFAULT;
	case DSTR_IS_SSO:
	case DSTR_IS_PTR:
		break;
	case DSTR_IS_BAD:
	default:
		dstr_error("Corrupt object state");
		return EILSEQ;
	}

	if (n_blocks == i.blocks)
		return 0;

	size_t cur_size = i.blocks ? i.blocks * DSTR_BLK_SIZE : obj_size;
	size_t new_size = n_blocks ? n_blocks * DSTR_BLK_SIZE : obj_size;
	size_t orig_len = i.length;

	typedef uintptr_t obj_type[obj_size / sizeof (uintptr_t)];
	obj_type *const dest = (obj_type *)obj_raw;

	if (i.state == DSTR_IS_PTR) {
		/* Grow or shrink the storage of an object that owns
		 * allocated memory. If the storage is shrunk enough
		 * to fit in the object, convert the object to SSO.
		 *
		 * Note that converting to SSO may involve truncation
		 * but doesn't necessitate it. Conversely, truncating
		 * the string doesn't imply that SSO conversion was
		 * done.
		 */
		unsigned char *ptr = (unsigned char *)(*dest)[0];
		bool truncate = orig_len >= new_size;

		/* The growth count survives a resize of the buffer */
		unsigned char const count = dstr_last_byte_(obj_size, obj_raw)
		                          & DSTR_GROW_COUNT_MASK;

		/* Possibly calculate a truncated string length */
		if (truncate)
			i.length = new_size - 1U;

		if (!n_blocks) {
			/* A block count of zero means conversion to SSO
			 * (by now we already know the object isn't one).
			 *
			 * First wipe the object. Then, if the buffer has
			 * a string, copy what we can fit into the object,
			 * and finally wipe and free the allocated buffer.
			 */
			(void)memset(obj_raw, 0, obj_size);
			if (orig_len) {
				if (i.length)
					(void)memcpy(obj_raw, ptr, i.length);
				(void)memset(ptr, 0, orig_len);
			}
			free(ptr);
			ptr = NULL;

			/* Encode the string length as available space
			 * and write it to the last byte (this acts as
			 * the SSO state indicator as well). After this
			 * we're done and the object is a valid SSO.
			 */
			dstr_last_byte_(obj_size, obj_raw) =
				(unsigned char)(obj_size - 1U - i.length);

		} else {
			/* Grow or shrink the allocated buffer, but not
			 * small enough to be converted to SSO. Always a
			 * `realloc()`. May truncate.
			 */

			/* Wipe to-be-lost bytes if about to truncate */
			if (truncate)
				(void)memset(&ptr[i.length], 0,
				             orig_len - i.length);

			unsigned char *p = realloc(ptr, new_size);
			if (!p) {
				/* Update end pointer if data was wiped. */
				if (truncate)
					(*dest)[1] = (uintptr_t)&ptr[i.length];

				/* Only failure to grow buffer is fatal. */
				if (new_size > cur_size) {
					int e = errno ? errno : ENOMEM;
					dstr_errno(e, "realloc(ptr, %zu)",
					           new_size);
					return e;
				}

			} else {
				ptr = p;

				/* Wipe uninitialized trailing bytes, if any */
				if (new_size > cur_size)
					(void)memset(&ptr[cur_size], 0,
					             new_size - cur_size);

				(*dest)[0] = (uintptr_t)ptr;
				(*dest)[1] = (uintptr_t)&ptr[i.length];
				dstr_last_uptr_(obj_size, obj_raw) = (uintptr_t)n_blocks
				                                   << DSTR_BLK_COUNT_POS;
				dstr_last_byte_(obj_size, obj_raw) |= DSTR_PTR_FLAG
				                                    | count;
			}
		}

	} else {
		/* Convert an SSO to an allocated buffer. The new buffer
		 * is always larger, so truncation is never considered.
		 */
		unsigned char *ptr = calloc(n_blocks, DSTR_BLK_SIZE);
		if (!ptr) {
			int e = errno ? errno : ENOMEM;
			dstr_errno(e, "calloc(%zu, %zu)", n_blocks,
			           DSTR_BLK_SIZE);
			return e;
		}

		/* Copy in-object string to allocated buffer, if any */
		if (i.length)
			(void)memcpy(ptr, obj_raw, i.length);

		/* Zero out the object and store allocation metadata */
		(void)memset(obj_raw, 0, obj_size);
		(*dest)[0] = (uintptr_t)ptr;
		(*dest)[1] = (uintptr_t)&ptr[i.length];
		dstr_last_uptr_(obj_size, obj_raw) = (uintptr_t)n_blocks
		                                   << DSTR_BLK_COUNT_POS;
		dstr_last_byte_(obj_size, obj_raw) |= DSTR_PTR_FLAG;
	}

	return 0;
}

_Static_assert(DSTR_GROW_SHIFT_MAX <= DSTR_GROW_COUNT_MASK,
               "the growth count can't reach DSTR_GROW_SHIFT_MAX");
_Static_assert(!(DSTR_GROW_COUNT_MASK & DSTR_PTR_FLAG) &&
               (DSTR_GROW_COUNT_MASK & 1U) &&
               !(DSTR_GROW_COUNT_MASK & (DSTR_GROW_COUNT_MASK + 1U)),
               "the growth count must be the lowest meta byte bits");
_Static_assert(DSTR_BLK_COUNT_MASK <= SIZE_MAX / DSTR_BLK_SIZE,
               "a block count's size in bytes must fit in size_t");

/**
 * @brief Grow a `dstr` object's buffer to fit `len` more bytes
 *
 * Does nothing if the string already has room for them. Otherwise the
 * new size holds the string, `len` more bytes and the terminator, and
 * is rounded up to a multiple of the growth step.
 *
 * An exact growth (for dstr_reserve()) has a step of one block and
 * leaves the growth count as it is. Otherwise (for the appends:
 * dstr_cat_n(), dstr_cat() and dstr_printf()) the step is one block
 * shifted left by the growth count, at most by `DSTR_GROW_SHIFT_MAX`,
 * and the growth is counted.
 *
 * @return 0 on success or an errno value on failure
 */
int
dstr_grow_ (size_t const  obj_size,
            unsigned char obj_raw[const obj_size],
            size_t const  len,
            bool const    exact)
{
	struct dstr_info const i = dstr_info_(obj_size, obj_raw);
	switch (i.state) {
	case DSTR_IS_NOT:
		dstr_error("Null object pointer");
		return EFAULT;
	case DSTR_IS_SSO:
	case DSTR_IS_PTR:
		break;
	case DSTR_IS_BAD:
	default:
		dstr_error("Corrupt object state");
		return EILSEQ;
	}

	if (len <= i.space)
		return 0;

	/* New string length, not counting the terminator */
	size_t const n = i.length + len;
	if (n < i.length) {
		dstr_errno(EOVERFLOW, "length=%zu, len=%zu", i.length, len);
		return EOVERFLOW;
	}

	/* An in-place string has no growth count: its meta byte is the
	 * space left */
	unsigned const count = i.state == DSTR_IS_PTR
	                     ? dstr_last_byte_(obj_size, obj_raw)
	                       & DSTR_GROW_COUNT_MASK
	                     : 0U;
	size_t const shift = exact ? 0U
	                   : count < DSTR_GROW_SHIFT_MAX ? count
	                   : DSTR_GROW_SHIFT_MAX;

	/* Round up to a multiple of the step. This can't wrap: at most it
	 * gives DSTR_BLK_COUNT_MASK + 1, which resize_ rejects. */
	size_t const n_blocks = ((n >> ((size_t)CHAR_BIT + shift)) + 1U)
	                      << shift;

	int const err = dstr_resize_(obj_size, obj_raw, n_blocks);
	if (err || exact)
		return err;

	/* Count the growth. The object owns a buffer now, and the count is
	 * in the lowest bits, so the increment stays inside them. */
	if (count < DSTR_GROW_COUNT_MASK)
		++dstr_last_byte_(obj_size, obj_raw);

	return 0;
}

/**
 * @brief Append formatted text to a `dstr` object of runtime-specified
 *        size
 *
 * Measures the text, grows the buffer for it like dstr_cat() does, and
 * formats it in place.
 *
 * @note Don't call this directly, use the `dstr_printf()` wrapper.
 *
 * @param[in]     obj_size The object's size in bytes
 * @param[in,out] obj_raw  Pointer to the object itself
 * @param[in]     fmt      `printf()` format string
 * @param[in]     ap       Arguments for `fmt`
 *
 * @return 0 on success or an errno value on failure
 */
dstr_printf_like(3, 0) int
dstr_vprintf_ (size_t const      obj_size,
               unsigned char     obj_raw[const obj_size],
               char const *const fmt,
               va_list           ap)
{
	/* Measure with a copy of the arguments, they are read again */
	va_list aq;
	va_copy(aq, ap);
	int const ret = vsnprintf(NULL, 0U, fmt, aq);
	va_end(aq);
	if (ret < 0) {
		int const e = errno ? errno : EILSEQ;
		dstr_errno(e, "vsnprintf");
		return e;
	}

	size_t const n = (size_t)ret;
	if (!n)
		return 0;

	int const err = dstr_grow_(obj_size, obj_raw, n, false);
	if (err)
		return err;

	struct dstr_info const i = dstr_info_(obj_size, obj_raw);
	typedef uintptr_t obj_type[obj_size / sizeof (uintptr_t)];
	obj_type *const dest = (obj_type *)obj_raw;
	char *const ptr = i.state == DSTR_IS_PTR ? (char *)(*dest)[1]
	                                         : (char *)&obj_raw[i.length];

	/* The terminator lands on the meta byte of an in-place string that
	 * the text fills, so the meta byte is assigned below, not adjusted */
	int const m = vsnprintf(ptr, n + 1U, fmt, ap);
	if (m != ret) {
		/* The arguments changed between the runs, or the second one
		 * failed: wipe the output, and restore the meta byte that its
		 * terminator may have overwritten */
		int const e = m < 0 && errno ? errno : EILSEQ;
		(void)memset(ptr, 0, n + 1U);
		if (i.state == DSTR_IS_SSO)
			dstr_last_byte_(obj_size, obj_raw) = (unsigned char)i.space;
		dstr_errno(e, "vsnprintf");
		return e;
	}

	if (i.state == DSTR_IS_PTR)
		(*dest)[1] += n;
	else
		dstr_last_byte_(obj_size, obj_raw) = (unsigned char)(i.space - n);

	return 0;
}
