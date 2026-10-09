/**
 * @file shim.h
 * @brief Allocation counting and failure injection for the tests
 *
 * Hosted builds link tests/dstr/shim.c with
 * -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
 * (dstr_shim_sources and dstr_shim_ldflags in tests/dstr/lib.sh). That
 * reroutes every allocation call made by the linked objects, the
 * library included, through the shim. The freestanding libc of the
 * qemu trial implements the same functions in its own allocator.
 */
#ifndef TWINCEPTION_TESTS_DSTR_SHIM_H_
#define TWINCEPTION_TESTS_DSTR_SHIM_H_

#include <stddef.h>

/**
 * @brief Reset the call count and cancel a pending failure
 *
 * Live blocks stay tracked.
 */
extern void
shim_reset (void);

/**
 * @brief Make an allocation call fail
 *
 * The `n`th malloc(), calloc() or realloc() call from now returns NULL
 * without allocating; realloc() leaves its block alone. The failing
 * call sets errno to `err` unless `err` is 0, in which case errno is
 * left as it was.
 *
 * @param n   Which call fails, counting from 1; 0 cancels
 * @param err errno value for the failure, or 0
 */
extern void
shim_fail_at (size_t n,
              int    err);

/**
 * @brief Number of malloc(), calloc() and realloc() calls since the
 *        last shim_reset(), failed ones included
 */
extern size_t
shim_calls (void);

/**
 * @brief Number of blocks currently allocated
 */
extern size_t
shim_live (void);

/**
 * @brief Number of free() and realloc() calls with a pointer that is
 *        not a live block (double frees, foreign pointers)
 */
extern size_t
shim_bad (void);

#endif /* TWINCEPTION_TESTS_DSTR_SHIM_H_ */
