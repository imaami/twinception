/** @file Small compile-time, container, hashing and formatting helpers. */
#ifndef TWINCEPTION_SRC_UTIL_H_
#define TWINCEPTION_SRC_UTIL_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof (a) / sizeof *(a))
#define container_of(ptr, T, member) ((T *)(void *)( \
	(char *)(ptr) - offsetof(T, member)))

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
util_url_shown (char const *url,
                int        *scheme)
{
	char const *host = strstr(url, "://");
	host = host ? host + sizeof "://" - 1 : url;
	char const *at = memchr(host, '@', strcspn(host, "/?#"));
	*scheme = (int)(host - url);
	return at ? at + 1 : host;
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
