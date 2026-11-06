/** @file compat.h
 *
 * C23 polyfills, convenience macros, etc.
 */
#ifndef ASKAI_SRC_COMPAT_H_
#define ASKAI_SRC_COMPAT_H_

#include <stddef.h> /* offsetof() */

#include "compiler.h"
#include "pragma.h"

#undef HAVE_C23_BOOL
#undef HAVE_C23_NULLPTR

#if __STDC_VERSION__ >= 202000L
# if gcc_at_least_version(13,1) || clang_at_least_version(15)
#  define HAVE_C23_BOOL
# endif
# if gcc_at_least_version(13,1) || clang_at_least_version(16)
#  define HAVE_C23_NULLPTR
# endif
#endif // __STDC_VERSION__ >= 202000L

#ifndef HAVE_C23_BOOL
# include <stdbool.h>
#endif

#ifndef HAVE_C23_NULLPTR
# define nullptr NULL
#endif

#undef HAVE_C23_BOOL
#undef HAVE_C23_NULLPTR

// These whine about C23 when compiling C23
#if __STDC_VERSION__ >= 202000L
# if clang_at_least_version(16) && clang_older_than_version(18)
diag_clang(ignored "-Wpre-c2x-compat")
# endif // 16 <= __clang_major__ < 18
# if clang_at_least_version(18)
diag_clang(ignored "-Wpre-c23-compat")
# endif // __clang_major__ >= 18
#endif // __STDC_VERSION__ >= 202000L

#define array_size(x) (sizeof (x) / sizeof (x)[0])
#define last_index(x) (array_size(x) - 1U)
#define container_of(ptr, T, member) \
        ((T *)(void *)((unsigned char *)(ptr) - offsetof(T, member)))

#ifndef _MSC_VER
# define force_inline __attribute__((always_inline)) inline
# define const_inline __attribute__((const)) force_inline
#else
# define force_inline __forceinline
# define const_inline __forceinline
#endif

#endif /* ASKAI_SRC_COMPAT_H_ */
