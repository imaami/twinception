/** @file util.c
 *
 * The functions of util.h that format text. A function with a variable
 * argument list is never expanded inline, so each of these is here once,
 * and not once in each file that calls it.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

size_t
util_vformat (char       *buf,
              size_t      size,
              char const *fmt,
              va_list     ap)
{
	/* Set `len` to `SIZE_MAX` if args are valid, otherwise 0. */
	size_t len = !buf | !size;

	if (--len) {
		union { int i; unsigned u; } x = {
			vsnprintf(buf, size, fmt, ap)
		};
		bool const err = x.u & ~(~0U >> 1U);

		/* Wipe on error, otherwise assign the length. */
		len += err;
		len &= x.u;

		if (err)
			/* Terminate buffer on error just in case. */
			buf[0] = '\0';

		if (len >= size) {
			/* Truncated write. Adjust the length we intend to
			 * return, and indicate truncation by changing the
			 * last characters to "..." if there's room left.
			 * The dots also replace the rest of a UTF-8
			 * character that they would split.
			 */
			len = --size;
			if (len > 2U) {
				size_t const dots = util_clip(buf, len, len - 3U);
				memset(buf + dots, '.', len - dots);
			}
		}
	}

	return len;
}

size_t
util_format (char       *buf,
             size_t      size,
             char const *fmt,
             ...)
{
	va_list ap;
	va_start(ap, fmt);
	size_t len = util_vformat(buf, size, fmt, ap);
	va_end(ap);
	return len;
}

struct cstr_view
util_format_view (char       *buf,
                  size_t      size,
                  char const *fmt,
                  ...)
{
	va_list ap;
	va_start(ap, fmt);
	size_t len = util_vformat(buf, size, fmt, ap);
	va_end(ap);
	return cstr_view_n(buf, len);
}

int
util_refuse (char       *why,
             size_t      len,
             char const *fmt,
             ...)
{
	va_list ap;
	va_start(ap, fmt);
	(void)util_vformat(why, len, fmt, ap);
	va_end(ap);
	return EINVAL;
}

int
util_text_printf_ (uint32_t   *len,
                   char       *data,
                   size_t      size,
                   char const *fmt,
                   ...)
{
	size_t const room = size - *len;
	if (room < 2)
		return EINVAL;
	va_list ap;
	va_start(ap, fmt);
	*len += (uint32_t)util_vformat(data + *len, room, fmt, ap);
	va_end(ap);
	return *len < size - 1 ? 0 : EINVAL;
}
