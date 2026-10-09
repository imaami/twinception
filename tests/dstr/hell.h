/**
 * @file hell.h
 * @brief Minimal check harness for the dstr test levels
 *
 * Header-only. A test program makes any number of HELL_* checks and
 * ends with `return hell_exit();`. Failed checks print their location
 * and values to stderr; after `HELL_MAX_REPORTS` failures the rest are
 * only counted. hell_rand() is a deterministic generator seeded from
 * the `HELL_SEED` environment variable (default: a fixed value), so a
 * failing randomized run can be replayed. hell_scale() reads
 * `DSTR_SCALE` for iteration multipliers.
 *
 * Compatible with C11 and later.
 */
#ifndef TWINCEPTION_TESTS_DSTR_HELL_H_
#define TWINCEPTION_TESTS_DSTR_HELL_H_

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HELL_MAX_REPORTS
# define HELL_MAX_REPORTS 50U
#endif

/** Check counters shared by the HELL_* macros */
static struct {
	unsigned long checked;
	unsigned long failed;
} hell_count;

/**
 * @brief Record one check and say whether a failure should be printed
 *
 * @param ok   Result of the check
 * @param file Source file of the check
 * @param line Source line of the check
 * @param what Text of the checked expression
 * @return `true` if the check failed and is within the report limit
 */
static inline bool
hell_record_ (bool         ok,
              char const  *file,
              unsigned     line,
              char const  *what)
{
	++hell_count.checked;
	if (ok)
		return false;
	if (++hell_count.failed > HELL_MAX_REPORTS)
		return false;
	(void)fprintf(stderr, "%s:%u: check failed: %s\n", file, line, what);
	return true;
}

/** Check that `cond` holds */
#define HELL_CHECK(cond) \
  ((void)hell_record_(!!(cond), __FILE__, __LINE__, #cond))

/** Check that two unsigned integer expressions are equal */
#define HELL_EQ(a, b) \
  do { \
    uintmax_t const hell_a_ = (uintmax_t)(a); \
    uintmax_t const hell_b_ = (uintmax_t)(b); \
    if (hell_record_(hell_a_ == hell_b_, __FILE__, __LINE__, \
                     #a " == " #b)) \
      (void)fprintf(stderr, "  %" PRIuMAX " != %" PRIuMAX "\n", \
                    hell_a_, hell_b_); \
  } while (0)

/** Check that two pointers are equal */
#define HELL_EQ_PTR(a, b) \
  do { \
    void const *const hell_a_ = (void const *)(a); \
    void const *const hell_b_ = (void const *)(b); \
    if (hell_record_(hell_a_ == hell_b_, __FILE__, __LINE__, \
                     #a " == " #b)) \
      (void)fprintf(stderr, "  %p != %p\n", hell_a_, hell_b_); \
  } while (0)

/** Check that two strings are equal; either may be a null pointer */
#define HELL_EQ_STR(a, b) \
  do { \
    char const *const hell_a_ = (a); \
    char const *const hell_b_ = (b); \
    bool const hell_ok_ = hell_a_ && hell_b_ ? !strcmp(hell_a_, hell_b_) \
                                             : hell_a_ == hell_b_; \
    if (hell_record_(hell_ok_, __FILE__, __LINE__, #a " == " #b)) \
      (void)fprintf(stderr, "  \"%.80s\" != \"%.80s\"\n", \
                    hell_a_ ? hell_a_ : "(null)", \
                    hell_b_ ? hell_b_ : "(null)"); \
  } while (0)

/** Check that `n` bytes at `a` and `b` are equal */
#define HELL_EQ_MEM(a, b, n) \
  HELL_CHECK(!memcmp((a), (b), (n)))

/**
 * @brief Print a summary line and return the process exit status
 *
 * @return `EXIT_SUCCESS` if every check passed, else `EXIT_FAILURE`
 */
static inline int
hell_exit (void)
{
	(void)fprintf(stderr, "%lu checks, %lu failed\n",
	              hell_count.checked, hell_count.failed);
	return hell_count.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

/**
 * @brief Check totals of a program made of several translation units
 *
 * Every translation unit has its own counters. Each one adds them to a
 * shared total with hell_collect(), and main() reports the total with
 * hell_exit_totals().
 */
struct hell_totals {
	unsigned long checked;
	unsigned long failed;
};

/**
 * @brief Move this translation unit's counts into `t`
 */
static inline void
hell_collect (struct hell_totals *t)
{
	t->checked += hell_count.checked;
	t->failed += hell_count.failed;
	hell_count.checked = 0;
	hell_count.failed = 0;
}

/**
 * @brief Print the totals and return the process exit status
 */
static inline int
hell_exit_totals (struct hell_totals const *t)
{
	(void)fprintf(stderr, "%lu checks, %lu failed\n",
	              t->checked, t->failed);
	return t->failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

/** State of the generator behind hell_rand() */
static struct {
	uint64_t state;
	bool     seeded;
} hell_rng;

/**
 * @brief Seed hell_rand() from `HELL_SEED`, or a fixed default
 *
 * Prints the seed to stderr so a failing run can be replayed. Called
 * implicitly by the first hell_rand().
 *
 * @return The seed in use
 */
static inline uint64_t
hell_seed (void)
{
	if (!hell_rng.seeded) {
		uint64_t seed = UINT64_C(0x7c15deadbeef2026);
		char const *env = getenv("HELL_SEED");
		if (env && *env)
			seed = strtoull(env, NULL, 0);
		hell_rng.state = seed;
		hell_rng.seeded = true;
		(void)fprintf(stderr, "HELL_SEED=%" PRIu64 "\n", seed);
	}
	return hell_rng.state;
}

/**
 * @brief Next pseudo-random 64-bit value (splitmix64)
 */
static inline uint64_t
hell_rand (void)
{
	(void)hell_seed();
	uint64_t z = (hell_rng.state += UINT64_C(0x9e3779b97f4a7c15));
	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	return z ^ (z >> 31);
}

/**
 * @brief Pseudo-random value in [0, n), or 0 if `n` is 0
 *
 * Draws masked values until one is below `n`: no division, so that
 * freestanding 32-bit builds need no 64-bit division helpers from a
 * runtime library, and every target draws the same sequence.
 */
static inline uint64_t
hell_below (uint64_t n)
{
	if (!n)
		return 0U;
	uint64_t mask = n - 1U;
	mask |= mask >> 1;
	mask |= mask >> 2;
	mask |= mask >> 4;
	mask |= mask >> 8;
	mask |= mask >> 16;
	mask |= mask >> 32;
	for (;;) {
		uint64_t const r = hell_rand() & mask;
		if (r < n)
			return r;
	}
}

/**
 * @brief Iteration multiplier from `DSTR_SCALE` (at least 1)
 */
static inline unsigned long
hell_scale (void)
{
	char const *env = getenv("DSTR_SCALE");
	unsigned long scale = env && *env ? strtoul(env, NULL, 10) : 1UL;
	return scale ? scale : 1UL;
}

#endif /* TWINCEPTION_TESTS_DSTR_HELL_H_ */
