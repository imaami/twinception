/** @file Small compile-time, container and hashing helpers. */
#ifndef TWINCEPTION_SRC_UTIL_H_
#define TWINCEPTION_SRC_UTIL_H_

#include <stddef.h>
#include <stdint.h>

#define ARRAY_SIZE(a) (sizeof (a) / sizeof *(a))
#define container_of(ptr, T, member) ((T *)(void *)( \
	(char *)(ptr) - offsetof(T, member)))

/** A hash of the @p len bytes at @p data, seeded with @p seed: a seed no
 *  client knows keeps clients from picking keys that crowd one bucket. */
static inline uint64_t
hash_bytes (uint64_t    seed,
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

#endif /* TWINCEPTION_SRC_UTIL_H_ */
