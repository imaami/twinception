/** @file Logging that never blocks the caller (design section 6).
 *
 * Each thread that logs formats its lines whole into its own ring, a single-
 * producer single-consumer byte ring, and one logger thread writes the rings
 * to stderr.  The logger alone writes stderr, so it alone may block on it: a
 * reader of stderr that stalls fills the rings, after which lines are dropped
 * and counted, and nobody else waits.  The logger sleeps while the rings are
 * empty and is woken through an eventfd, which a producer writes only when
 * the logger has said it sleeps.
 *
 * A thread without a ring, such as one logging before the logger starts or in
 * a program that has none, writes its lines to stderr itself.
 */
#ifndef TWINCEPTION_SRC_LOG_H_
#define TWINCEPTION_SRC_LOG_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/** Levels, the most severe first. */
enum log_level {
	LOG_ERROR,
	LOG_WARN,
	LOG_INFO,
	LOG_VERBOSE,
};

/** A call site's rate limit: at most LOG_BURST lines a second, after which
 *  the site's next line says how many it held back. */
struct log_site {
	_Atomic uint64_t second;  /**< of CLOCK_MONOTONIC_COARSE, the window's */
	_Atomic uint32_t count;   /**< lines in the window */
	_Atomic uint32_t missed;  /**< held back since the last line */
};

/** Lines logged, for every thread to count into. */
struct log_metrics {
	_Atomic uint64_t lines;       /**< put into a ring */
	_Atomic uint64_t dropped;     /**< lost to a full ring */
	_Atomic uint64_t suppressed;  /**< held back by their site's rate limit */
};

extern struct log_metrics log_metrics;

/** Lines of a level above this are skipped; set before threads start. */
extern enum log_level log_threshold;

/**
 * Log a line at @p level, formatted as by printf(), and add a line break.
 * A level above log_threshold skips the call, arguments and all, and each
 * call site has its own rate limit.
 */
#define LOG(level, ...) \
        do { \
                if ((level) <= log_threshold) { \
                        static struct log_site log_site_; \
                        log_put(&log_site_, __VA_ARGS__); \
                } \
        } while (0)

/** As LOG(), but without the rate limit of a call site: for lines that
 *  must all appear, such as those that describe the configuration as askai
 *  starts. */
#define LOG_ALL(level, ...) \
        do { \
                if ((level) <= log_threshold) \
                        log_put(nullptr, __VA_ARGS__); \
        } while (0)

/** The work of LOG(): with no @p site, the line has no rate limit, which
 *  suits what an operator asked for, such as statistics. */
[[gnu::format(printf, 2, 3)]]
extern void
log_put (struct log_site *site,
         char const      *fmt,
         ...);

/**
 * Start the logger with @p rings rings, one for each thread that will
 * log_attach(); it writes @p notify_fd, an eventfd, once it has stopped, so
 * the caller keeps that open until the process exits.
 */
extern int
log_start (size_t rings,
           int    notify_fd);

/** Give the calling thread a ring of its own; without one left, it drops its
 *  lines, counted. */
extern void
log_attach (void);

/** Ask the logger to write what the rings hold, then stop. */
extern void
log_stop (void);

/** Whether the logger has stopped. */
extern bool
log_done (void);

/** Free the logger once it is done, and no thread but the caller logs any
 *  more; the caller then logs to stderr itself. */
extern void
log_fini (void);

#endif /* TWINCEPTION_SRC_LOG_H_ */
