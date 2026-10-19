/** @file
 * Compatibility macros.
 */
#ifndef TWINCEPTION_SRC_COMPAT_H_
#define TWINCEPTION_SRC_COMPAT_H_

#include <stddef.h>

#define array_size(x) (sizeof (x) / sizeof (x)[0])
#define container_of(ptr, T, member) \
        ((T *)(void *)((unsigned char *)(ptr) - offsetof(T, member)))

#define force_inline __attribute__((always_inline)) inline
#define const_inline __attribute__((const)) force_inline

#endif /* TWINCEPTION_SRC_COMPAT_H_ */
