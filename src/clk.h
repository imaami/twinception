/** @file clk.h
 *
 * Clock readings.
 */
#ifndef TWINCEPTION_SRC_CLK_H_
#define TWINCEPTION_SRC_CLK_H_

#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "compat.h"

static const_inline bool
clk_timespec_ok (struct timespec const t)
{
	return t.tv_sec >= 0 && t.tv_nsec >= 0 && t.tv_nsec <= 999999999;
}

/**
 * Returns the time according to clock @p id, or an invalid
 * `struct timespec` on failure, with an error value encoded
 * in the fields of the returned `struct timespec` object.
 *
 * Failure is indicated by the `timespec::tv_sec` field having
 * a negative value. This field is never negative on success,
 * and it is always negative on failure.
 *
 * A negative `timespec::tv_sec` encodes an error number when
 * available. If `clock_gettime()` fails and provides an error
 * code via `errno`, the error is saved in `timespec::tv_sec`
 * with a negative sign. Recovering the original error number
 * is done by flipping the bits of `timespec::tv_sec` iff the
 * `timespec::tv_nsec` field is negative, or if the latter is
 * not negative, reading `timespec::tv_sec` as-is (it's already
 * the original error number).
 *
 * If `timespec::tv_sec` is -1 and `timespec::tv_nsec` is also
 * negative, flipping the bits of `timespec::tv_sec` yields 0.
 * It means `clock_gettime()` returned a failure condition but
 * did not set `errno`. This hypothetical contradictory result
 * is mirrored by the fact that the decoded `errno` value will
 * indeed be 0 while `timespec::tv_sec` indicates failure.
 *
 * If `timespec::tv_sec` is -1 and `timespec::tv_nsec` is not
 * negative, the error number is simply -1, which is a stand-in
 * indicator value instead of deliberately chosen standard error
 * number. It means `clock_gettime()` returned success but wrote
 * garbage data into the destination `struct timespec` object;
 * somewhat plausible but unlikely to happen.
 */
static force_inline struct timespec
clk_now_or_1969 (clockid_t const id)
{
	struct timespec t = {0};

	if (clock_gettime(id, &t)) {
		int e = errno;
		t.tv_sec  = e;
		t.tv_nsec = -(e >= 0);
		t.tv_sec ^= t.tv_nsec;

	} else if (!clk_timespec_ok(t))
		t = (struct timespec){
			.tv_sec = -1
		};

	return t;
}

static const_inline int
clk_errno (struct timespec t)
{
	return t.tv_sec >= 0
	       ? 0
	       : t.tv_nsec >= 0
	         ? (int)t.tv_sec
	         : (int)~t.tv_sec;
}

/**
 * Returns the time according to clock @p id, or a zeroed-out
 * `struct timespec` on failure, and writes an error value to
 * @p err.
 *
 * If `clock_gettime()` returns an error condition but leaves
 * `errno` unset, or if it reports success but writes garbage
 * into the destination object, `-1` is written to @p err.
 *
 * Always modifies @p err to guarantee zero-valued timestamps
 * are distinguishable from errors.
 */
static force_inline struct timespec
clk_now_or_1970_err (clockid_t const  id,
                     int             *err)
{
	struct timespec t = {0};

	int e = !clock_gettime(id, &t)
	        ? -!clk_timespec_ok(t)
	        : (errno ? errno : -1);
	*err = e;

	return e ? (typeof(t)){0} : t;
}

/**
 * Get the time, or a zeroed-out `struct timespec` on error.
 */
static force_inline struct timespec
clk_now_or_1970 (clockid_t const id)
{
	struct timespec t = {0};
	return !clock_gettime(id, &t)
	    && clk_timespec_ok(t) ? t : (typeof(t)){0};
}

/** Time now in nanoseconds, or 0 on error. */
static force_inline uint64_t
clk_now_or_1970_ns (clockid_t const id)
{
	struct timespec t = clk_now_or_1970(id);
	return 1000000000 * (uint64_t)t.tv_sec + (uint64_t)t.tv_nsec;
}

/** Boot clock time now, or 0 on error. */
static force_inline struct timespec
clk_boottime (void)
{
	return clk_now_or_1970(CLOCK_BOOTTIME);
}

/** Coarse monotonic time now, or 0 on error. */
static force_inline struct timespec
clk_monotonic_coarse (void)
{
	return clk_now_or_1970(CLOCK_MONOTONIC_COARSE);
}

/** Monotonic time now, or 0 on error. */
static force_inline struct timespec
clk_monotonic (void)
{
	return clk_now_or_1970(CLOCK_MONOTONIC);
}

/** Monotonic time now in nanoseconds, or 0 on error. */
static force_inline uint64_t
clk_monotonic_ns (void)
{
	return clk_now_or_1970_ns(CLOCK_MONOTONIC);
}

/** Wall clock time now, or 0 on error. */
static force_inline struct timespec
clk_realtime (void)
{
	return clk_now_or_1970(CLOCK_REALTIME);
}

/** Wall clock time now in nanoseconds, or 0 on error. */
static force_inline uint64_t
clk_realtime_ns (void)
{
	return clk_now_or_1970_ns(CLOCK_REALTIME);
}

#endif /* TWINCEPTION_SRC_CLK_H_ */
