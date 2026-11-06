/** @file util.h
 *
 * Small compile-time, container, hashing, formatting, text and
 * file helpers.
 */
#ifndef TWINCEPTION_SRC_UTIL_H_
#define TWINCEPTION_SRC_UTIL_H_

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "clk.h"
#include "compat.h"
#include "cstr_view.h"

/** Nanoseconds in a second. */
static constexpr uint64_t NS = 1'000'000'000;

/** Write the @p n bytes at @p src to @p dst as 2 @p n lowercase hexadecimal
 *  digits, without a NUL.
 */
static inline void
util_hex (char *restrict       dst,
          void const *restrict src,
          size_t               n)
{
	static char const digits[] = "0123456789abcdef";
	unsigned char const *p = src;
	for (size_t i = 0; i < n; ++i) {
		dst[2 * i] = digits[p[i] >> 4];
		dst[2 * i + 1] = digits[p[i] & 15];
	}
}

/** Fill the @p n bytes at @p dst, at most 256, with random bytes from the
 *  kernel. Returns 0, or the error number.
 */
static inline int
util_random (void   *dst,
             size_t  n)
{
	if (n > 256)
		return EINVAL;
	/* For so few bytes, getrandom() returns fewer only if a signal
	 * interrupts it before it has any. */
	ssize_t got;
	while ((got = getrandom(dst, n, 0)) < 0)
		if (errno != EINTR)
			return errno;
	return (size_t)got == n ? 0 : EIO;
}

/** Write @p n random bytes, at most 256, from the kernel to @p dst as 2 @p n
 *  lowercase hexadecimal digits, without a NUL. Returns 0, or the error
 *  number.
 */
static inline int
util_random_hex (char   *dst,
                 size_t  n)
{
	unsigned char bits[256];
	if (n > sizeof bits)
		return EINVAL;
	int e = util_random(bits, n);
	if (!e)
		util_hex(dst, bits, n);
	return e;
}

/** Whether the @p n bytes at @p s are lowercase hexadecimal digits. */
static inline bool
util_is_hex (char const *s,
             size_t      n)
{
	for (size_t i = 0; i < n; ++i)
		if ((s[i] < '0' || s[i] > '9') && (s[i] < 'a' || s[i] > 'f'))
			return false;
	return true;
}

/** Whether the @p n bytes at @p s are the full name of a Git object: 40
 *  (SHA-1) or 64 (SHA-256) lowercase hexadecimal digits.
 */
static inline bool
util_is_oid (char const *s,
             size_t      n)
{
	return (n == 40 || n == 64) && util_is_hex(s, n);
}

/** The value of the hexadecimal digit @p c, of either case, or 16 if @p c
 *  is not one.
 */
static inline unsigned
util_hex_digit (unsigned char c)
{
	unsigned lower = c | 0x20u;
	return c >= '0' && c <= '9' ? (unsigned)(c - '0')
	     : lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : 16;
}

/** The most digits of a uint64_t in decimal. */
enum { UTIL_DECIMAL = 20 };

/** Write @p n in decimal into the bytes before @p end, at most UTIL_DECIMAL
 *  of them. Returns a pointer to the first digit.
 */
static inline char *
util_decimal (char     *end,
              uint64_t  n)
{
	do *--end = (char)('0' + n % 10);
	while (n /= 10);
	return end;
}

/** The plural ending of a noun after the count @p n: "" if @p n is 1, else
 *  "s".
 */
static inline char const *
util_s (uint64_t n)
{
	return n == 1 ? "" : "s";
}

/** Write the @p n low bytes of @p v, at most 8, to @p to, the most
 *  significant first.
 */
static inline void
util_be_put (unsigned char *to,
             uint64_t       v,
             size_t         n)
{
	while (n--) {
		to[n] = (unsigned char)v;
		v >>= 8;
	}
}

/** The number in the @p n bytes at @p from, at most 8, the most
 *  significant first.
 */
static inline uint64_t
util_be_get (unsigned char const *from,
             size_t               n)
{
	uint64_t v = 0;
	for (size_t i = 0; i < n; ++i)
		v = v << 8 | from[i];
	return v;
}

/** Parse the decimal number at the start of the @p len bytes at @p s into
 *  @p *value. Returns the number of digits. If there are no digits or the
 *  number is greater than @p max, returns 0 and sets @p *value to 0.
 */
static inline size_t
util_digits (char const *s,
             size_t      len,
             uint64_t    max,
             uint64_t   *value)
{
	*value = 0;
	size_t i = 0;
	for (; i < len && s[i] >= '0' && s[i] <= '9'; ++i) {
		unsigned digit = (unsigned)(s[i] - '0');
		if (digit > max || *value > (max - digit) / 10) {
			*value = 0;
			return 0;
		}
		*value = *value * 10 + digit;
	}
	return i;
}

/** Returns how many of the @p len bytes at @p s fit in @p max bytes
 *  without ending within a UTF-8 character.
 */
static inline size_t
util_clip (char const *s,
           size_t      len,
           size_t      max)
{
	if (len <= max)
		return len;
	while (max && ((unsigned char)s[max] & 0xc0) == 0x80)
		--max;
	return max;
}

/** Returns @p off, at most @p len, moved forward past the UTF-8
 *  continuation bytes of the @p len bytes at @p s: the start of a
 *  character, or @p len.
 */
static inline size_t
util_char_start (char const *s,
                 size_t      len,
                 size_t      off)
{
	while (off < len && ((unsigned char)s[off] & 0xc0) == 0x80)
		++off;
	return off;
}

/** The ASCII letters and digits, as a string: A to Z, a to z, then 0 to 9. */
#define UTIL_ALNUM "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"

/** The most bytes of a word in askai's Git refs, such as a participant. */
enum { UTIL_WORD_MAX = 64 };

/** Whether the @p n bytes at @p s, 1 to @p max of them, are ASCII letters,
 *  digits, underscores or hyphens, as the words in askai's Git refs are.
 */
static inline bool
util_word (char const *s,
           size_t      n,
           size_t      max)
{
	if (!s || !n || n > max)
		return false;
	for (size_t i = 0; i < n; ++i)
		if (!(s[i] >= 'a' && s[i] <= 'z') && !(s[i] >= 'A' && s[i] <= 'Z') &&
		    !(s[i] >= '0' && s[i] <= '9') && s[i] != '_' && s[i] != '-')
			return false;
	return true;
}

/** Returns the length of the string @p s if util_word() accepts it as a
 *  word of 1 to @p max bytes, or 0 if not or if @p s is nullptr.
 */
static inline size_t
util_word_len (char const *s,
               size_t      max)
{
	size_t const n = s ? strnlen(s, max + 1) : 0;
	return util_word(s, n, max) ? n : 0;
}

/** The entry "NAME=value" of the process's environment whose "NAME=" is
 *  the @p len bytes at @p name, or nullptr if it has none. Nothing in
 *  askai changes its environment, so the entry lasts.
 */
static inline char *
util_env (char const *name,
          size_t      len)
{
	extern char **environ;
	for (char **e = environ; e && *e; ++e)
		if (!strncmp(*e, name, len))
			return *e;
	return nullptr;
}

/** The entry "PATH=..." of the process's environment for a child to
 *  inherit, or "PATH=/usr/bin:/bin" if it has no PATH or an empty one.
 */
static inline char *
util_env_path (void)
{
	char *p = util_env("PATH=", sizeof "PATH=" - 1U);
	return p && p[sizeof "PATH=" - 1U]
	       ? p : "PATH=/usr/bin:/bin";
}

/** Close the descriptor @p *fd unless it is negative, and set it to -1. */
static inline void
util_close (int *fd)
{
	if (*fd >= 0)
		(void)close(*fd);
	*fd = -1;
}

/** Write the @p len bytes at @p buf to @p fd, again after a signal or a
 *  partial write. Returns 0, or the error number, EIO if no byte could be
 *  written.
 */
static inline int
util_write_all (int         fd,
                void const *buf,
                size_t      len)
{
	unsigned char const *p = buf;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n ? errno : EIO;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/** Read up to @p len bytes from @p fd into @p buf, from the offset @p at
 *  with pread(), or from where the file stands with read() if @p at is
 *  negative, again after a signal or a partial read, until the end of the
 *  file; their count goes to @p *got. Returns 0, or the error number.
 */
static inline int
util_read_all (int     fd,
               void   *buf,
               size_t  len,
               off_t   at,
               size_t *got)
{
	unsigned char *p = buf;
	*got = 0;
	while (*got < len) {
		ssize_t n = at < 0 ? read(fd, p + *got, len - *got)
		                   : pread(fd, p + *got, len - *got, at + (off_t)*got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n ? errno : 0;
		*got += (size_t)n;
	}
	return 0;
}

/** A hash of the @p len bytes at @p data, seeded with @p seed: a seed no
 *  client knows keeps clients from picking keys that crowd one bucket.
 */
static inline uint64_t
util_hash (uint64_t    seed,
           void const *data,
           size_t      len)
{
	unsigned char const *p = data;
	uint64_t h = seed ^ len;
	for (size_t i = 0; i < len; ++i)
		h = (h ^ p[i]) * 0x100000001b3;
	h = (h ^ h >> 33) * 0xff51afd7ed558ccd;
	return h ^ h >> 33;
}

/** Returns a seed for util_hash(). The seed is 8 random bytes from the
 *  kernel, or the wall clock if the kernel has no random bytes yet.
 */
static inline uint64_t
util_seed (void)
{
	uint64_t seed;
	if (getrandom(&seed, sizeof seed, GRND_NONBLOCK) == (ssize_t)sizeof seed)
		return seed;
	return clk_realtime_ns();
}

/** Grow @p items, an array with room for @p *cap items of @p size bytes,
 *  so that it has room for @p n items, and update @p *cap. Returns the
 *  array, which may have moved. If out of memory, returns @p items and
 *  leaves @p *cap as it is, less than @p n.
 */
static inline void *
util_room (void   *items,
           size_t *cap,
           size_t  n,
           size_t  size)
{
	if (n <= *cap)
		return items;
	size_t more = *cap ? 2 * *cap : 4;
	more = more < n || more > SIZE_MAX / size ? n : more;
	void *p = n <= SIZE_MAX / size ? realloc(items, more * size) : nullptr;
	if (!p)
		return items;
	*cap = more;
	return p;
}

/** Grow the array @p items with room for @p cap items, both lvalues, to
 *  room for @p n items with util_room(). Evaluates to 0, or to ENOMEM
 *  with @p items and @p cap unchanged.
 */
#define util_grow(items, cap, n) \
        ((items) = util_room((items), &(cap), (n), sizeof *(items)), \
         (cap) < (n) ? ENOMEM : 0)

/** Returns the length of the scheme of @p url with its "://", 7 for http
 *  and 8 for https, if more follows it and each byte of @p url is printable
 *  ASCII and not a space; 0 otherwise.
 */
static inline size_t
util_url_scheme (struct cstr_view url)
{
	size_t const scheme = cstr_view_starts(url, CSTR_VIEW("http://")) ? sizeof "http://" - 1
	                    : cstr_view_starts(url, CSTR_VIEW("https://")) ? sizeof "https://" - 1
	                    : 0;
	if (!scheme || scheme == url.len)
		return 0;
	for (size_t i = 0; i < url.len; ++i)
		if ((unsigned char)url.data[i] <= ' ' || (unsigned char)url.data[i] > '~')
			return 0;
	return scheme;
}

/** The part of @p url that a log shows after its scheme: the rest of the
 *  URL after its user name and password, if it has them. Stores the length
 *  of the scheme and its "://" in @p *scheme.
 */
static inline char const *
util_url_shown (char const *restrict url,
                int        *restrict scheme)
{
	char const *host = url;
	for (size_t i = 0; url[i]; ) {
		if (url[i++] == ':' &&
		    url[ i ] == '/' &&
		    url[++i] == '/') {
			host = &url[++i];
			break;
		}
	}

	*scheme = (int)(host - url);

	for (url = host;; ) {
		switch (*url++) {
		case '\0':
		case '#':
		case '?':
		case '/':
			return host;
		case '@':
			return url;
		}
	}

	return host;
}

[[gnu::format(printf, 3, 0)]]
static inline size_t
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
			 */
			len = --size;
			if (len > 2U) {
				buf[len - 3U] = '.';
				buf[len - 2U] = '.';
				buf[len - 1U] = '.';
			}
		}
	}

	return len;
}

[[gnu::format(printf, 3, 4)]]
static inline size_t
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

/** Formats text into the @p size bytes at @p buf as util_format() does.
 *  Returns a view of the text in @p buf.
 */
[[gnu::format(printf, 3, 4)]]
static inline struct cstr_view
util_format_view (char       *buf,
                  size_t      size,
                  char const *fmt,
                  ...)
{
	va_list ap;
	va_start(ap, fmt);
	size_t len = util_vformat(buf, size, fmt, ap);
	va_end(ap);
	return (struct cstr_view){ buf, len };
}

/** Formats the reason of a refusal into the @p len bytes at @p why as
 *  util_format() does. Returns EINVAL.
 */
[[gnu::format(printf, 3, 4)]]
static inline int
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

#endif /* TWINCEPTION_SRC_UTIL_H_ */
