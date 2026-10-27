/** @file Small compile-time, container, hashing, formatting, text and
 *  file helpers. */
#ifndef TWINCEPTION_SRC_UTIL_H_
#define TWINCEPTION_SRC_UTIL_H_

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "compat.h"

#define ARRAY_SIZE(a) (sizeof (a) / sizeof *(a))

/** Nanoseconds in a second. */
static constexpr uint64_t NS = 1'000'000'000;

/** The wall clock, CLOCK_REALTIME, in nanoseconds since the epoch, or 0 if
 *  it cannot be read or is before the epoch. */
static inline uint64_t
util_realtime (void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) || ts.tv_sec < 0)
		return 0;
	return (uint64_t)ts.tv_sec * NS + (uint64_t)ts.tv_nsec;
}

/** Write the @p n bytes at @p src to @p dst as 2 @p n lowercase hexadecimal
 *  digits, without a NUL. */
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

/** Write @p n random bytes, at most 256, from the kernel to @p dst as 2 @p n
 *  lowercase hexadecimal digits, without a NUL.  Returns 0, or the error
 *  number. */
static inline int
util_random_hex (char   *dst,
                 size_t  n)
{
	unsigned char bits[256];
	if (n > sizeof bits)
		return EINVAL;
	/* For so few bytes, getrandom() returns fewer only if a signal
	 * interrupts it before it has any. */
	ssize_t got;
	while ((got = getrandom(bits, n, 0)) < 0)
		if (errno != EINTR)
			return errno;
	if ((size_t)got != n)
		return EIO;
	util_hex(dst, bits, n);
	return 0;
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
 *  (SHA-1) or 64 (SHA-256) lowercase hexadecimal digits. */
static inline bool
util_is_oid (char const *s,
             size_t      n)
{
	return (n == 40 || n == 64) && util_is_hex(s, n);
}

/** The value of the hexadecimal digit @p c, of either case, or 16 if @p c
 *  is not one. */
static inline unsigned
util_hex_digit (unsigned char c)
{
	unsigned lower = c | 0x20u;
	return c >= '0' && c <= '9' ? (unsigned)(c - '0')
	     : lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : 16;
}

/** The most bytes of a word in askai's Git refs, such as a participant. */
enum { UTIL_WORD_MAX = 64 };

/** Whether the @p n bytes at @p s, 1 to @p max of them, are ASCII letters,
 *  digits, underscores or hyphens, as the words in askai's Git refs are. */
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

/** The entry "NAME=value" of the process's environment whose "NAME=" is
 *  the @p len bytes at @p name, or nullptr if it has none.  Nothing in
 *  askai changes its environment, so the entry lasts. */
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
 *  inherit, or "PATH=/usr/bin:/bin" if it has no PATH or an empty one. */
static inline char *
util_env_path (void)
{
	static char const name[] = "PATH=";
	char *path = util_env(name, sizeof name - 1);
	return path && path[sizeof name - 1] ? path : "PATH=/usr/bin:/bin";
}

/** Write the @p len bytes at @p buf to @p fd, again after a signal or a
 *  partial write.  Returns 0, or the error number, EIO if no byte could be
 *  written. */
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
 *  file; their count goes to @p *got.  Returns 0, or the error number. */
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
 *  client knows keeps clients from picking keys that crowd one bucket. */
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

/** The @p a_len bytes at @p a, then the string @p b, in memory of their
 *  own; nullptr if out of memory. */
static inline char *
util_concat (char const *a,
             size_t      a_len,
             char const *b)
{
	size_t b_len = strlen(b);
	char *s = malloc(a_len + b_len + 1);
	if (s) {
		memcpy(s, a, a_len);
		memcpy(s + a_len, b, b_len + 1);
	}
	return s;
}

/** The part of @p url that a log shows after its scheme: the rest of the
 *  URL after its user name and password, if it has them.  Stores the length
 *  of the scheme and its "://" in @p *scheme. */
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

/**
 * Format words that a log, a client or the model reads into the @p size
 * bytes at @p buf, @p size > 0, as vsnprintf() does: text cut short ends in
 * "...", and a format that fails gives "?".  Returns the length written.
 */
[[gnu::format(printf, 3, 0)]]
static inline size_t
util_vformat (char       *buf,
              size_t      size,
              char const *fmt,
              va_list     ap)
{
	static char const cut[] = "...";
	int n = vsnprintf(buf, size, fmt, ap);
	if (n < 0) {
		size_t len = size > 1;
		memcpy(buf, "?", len);
		buf[len] = '\0';
		return len;
	}
	if ((size_t)n < size)
		return (size_t)n;
	if (size >= sizeof cut)
		memcpy(buf + size - sizeof cut, cut, sizeof cut);
	return size - 1;
}

/** As util_vformat(), with the format's arguments. */
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

#endif /* TWINCEPTION_SRC_UTIL_H_ */
