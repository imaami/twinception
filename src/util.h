/** @file Small compile-time, container, hashing and formatting helpers. */
#ifndef TWINCEPTION_SRC_UTIL_H_
#define TWINCEPTION_SRC_UTIL_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

/**
 * Format words that a log, a client or the model reads into the @p size
 * bytes at @p buf, @p size > 0, as snprintf() does: text cut short ends in
 * "...", and a format that fails gives "?".  Returns the length written.
 */
[[gnu::format(printf, 3, 4)]]
static inline size_t
util_format (char       *buf,
             size_t      size,
             char const *fmt,
             ...)
{
	static char const cut[] = "...";
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
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

#endif /* TWINCEPTION_SRC_UTIL_H_ */
