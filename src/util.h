/** @file Small compile-time and container helpers. */
#ifndef TWINCEPTION_SRC_UTIL_H_
#define TWINCEPTION_SRC_UTIL_H_

#include <stddef.h>

#define ARRAY_SIZE(a) (sizeof (a) / sizeof *(a))
#define container_of(ptr, T, member) ((T *)(void *)( \
	(char *)(ptr) - offsetof(T, member)))

#endif /* TWINCEPTION_SRC_UTIL_H_ */
