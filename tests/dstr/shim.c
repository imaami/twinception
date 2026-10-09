/**
 * @file shim.c
 * @brief Hosted implementation of shim.h through ld --wrap
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "shim.h"

extern void *__real_malloc (size_t size);
extern void *__real_calloc (size_t nmemb, size_t size);
extern void *__real_realloc (void *ptr, size_t size);
extern void __real_free (void *ptr);

extern void *__wrap_malloc (size_t size);
extern void *__wrap_calloc (size_t nmemb, size_t size);
extern void *__wrap_realloc (void *ptr, size_t size);
extern void __wrap_free (void *ptr);

/** Slots in the live block table (a power of two) */
#define SHIM_SLOTS ((size_t)1 << 16)

/** Table marker of a removed entry */
#define SHIM_GONE ((uintptr_t)1)

/** State of the shim */
static struct {
	uintptr_t slot[SHIM_SLOTS]; /**< Live pointers, 0 or SHIM_GONE */
	size_t    live;             /**< Live blocks */
	size_t    calls;            /**< Allocation calls since reset */
	size_t    bad;              /**< Frees of unknown pointers */
	size_t    fail_at;          /**< Call number that fails, or 0 */
	int       fail_err;         /**< errno of the failure, or 0 */
} shim;

/**
 * @brief First table slot to probe for a pointer
 */
static size_t
shim_hash_ (uintptr_t p)
{
	return (size_t)((p >> 4) * UINT64_C(0x9e3779b97f4a7c15) >> 16) &
	       (SHIM_SLOTS - 1U);
}

/**
 * @brief Start tracking a new block
 */
static void
shim_add_ (void *ptr)
{
	uintptr_t const p = (uintptr_t)ptr;
	for (size_t i = shim_hash_(p), n = 0; n < SHIM_SLOTS; ++n) {
		if (shim.slot[i] <= SHIM_GONE) {
			shim.slot[i] = p;
			++shim.live;
			return;
		}
		i = (i + 1U) & (SHIM_SLOTS - 1U);
	}
	abort(); /* more live blocks than any test makes */
}

/**
 * @brief Stop tracking a block
 *
 * @return `false` if `ptr` was not a live block
 */
static bool
shim_del_ (void *ptr)
{
	uintptr_t const p = (uintptr_t)ptr;
	for (size_t i = shim_hash_(p), n = 0; n < SHIM_SLOTS; ++n) {
		if (shim.slot[i] == p) {
			shim.slot[i] = SHIM_GONE;
			--shim.live;
			return true;
		}
		if (!shim.slot[i])
			return false;
		i = (i + 1U) & (SHIM_SLOTS - 1U);
	}
	return false;
}

/**
 * @brief Count an allocation call and say whether it must fail
 */
static bool
shim_fails_ (void)
{
	if (++shim.calls != shim.fail_at)
		return false;
	shim.fail_at = 0;
	if (shim.fail_err)
		errno = shim.fail_err;
	return true;
}

void
shim_reset (void)
{
	shim.calls = 0;
	shim.fail_at = 0;
	shim.fail_err = 0;
}

void
shim_fail_at (size_t n,
              int    err)
{
	shim.fail_at = n ? shim.calls + n : 0U;
	shim.fail_err = err;
}

size_t
shim_calls (void)
{
	return shim.calls;
}

size_t
shim_live (void)
{
	return shim.live;
}

size_t
shim_bad (void)
{
	return shim.bad;
}

void *
__wrap_malloc (size_t size)
{
	if (shim_fails_())
		return NULL;
	void *const p = __real_malloc(size);
	if (p)
		shim_add_(p);
	return p;
}

void *
__wrap_calloc (size_t nmemb,
               size_t size)
{
	if (shim_fails_())
		return NULL;
	void *const p = __real_calloc(nmemb, size);
	if (p)
		shim_add_(p);
	return p;
}

void *
__wrap_realloc (void   *ptr,
                size_t  size)
{
	if (shim_fails_())
		return NULL;
	if (ptr && !shim_del_(ptr)) {
		++shim.bad;
		return NULL;
	}
	void *const p = __real_realloc(ptr, size);
	if (p)
		shim_add_(p);
	else if (ptr)
		shim_add_(ptr);
	return p;
}

void
__wrap_free (void *ptr)
{
	if (!ptr)
		return;
	if (!shim_del_(ptr)) {
		++shim.bad;
		return;
	}
	__real_free(ptr);
}
