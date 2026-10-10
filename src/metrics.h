/** @file The core's metrics in Prometheus's text format, for GET /metrics.
 *
 * Every histogram and gauge has one writer thread, as do most counters; those
 * of log lines and of blocker jobs, which several threads count into, are
 * atomic sums of rare events.  All are read with relaxed atomics, so
 * rendering takes no lock and stops no one: per-loop histograms of turn and
 * step times, timer lateness and inbox delay, the most work a step did,
 * request times by MCP method, upstream wait and run times, host lookups,
 * blocker jobs, log lines dropped, watchdog reports, the brainstem's
 * resource gauges, memory pressure and its inputs, and per loop the storage
 * its stash and its requests keep and what it gave back.  Histograms have
 * fixed buckets, from 1 µs to 300 s.
 */
#ifndef TWINCEPTION_SRC_METRICS_H_
#define TWINCEPTION_SRC_METRICS_H_

#include <stddef.h>

#include "buf.h"
#include "hist.h"

struct blocker_metrics;
struct http_metrics;
struct loop_metrics;

/** MCP methods, whose request times are kept apart. */
enum metrics_method {
	METHOD_OTHER,       /**< not one below, or no valid message */
	METHOD_INITIALIZE,
	METHOD_PING,
	METHOD_TOOLS_LIST,
	METHOD_TOOLS_CALL,
	METHOD_DISCOVER,
	METHOD_NOTIFY,      /**< notifications, and responses */
	METHOD_METRICS,     /**< GET /metrics */
	METHOD_N
};

/** What a loop keeps for reuse besides its stash's storage: the loop
 *  writes, any thread reads (hist.h). */
struct metrics_kept {
	_Atomic uint64_t spares;    /**< finished requests */
	_Atomic uint64_t bytes[2];  /**< what those hold, and the pooled
	                                 tokeners' string buffers */
};

/** Where a loop's metrics are; the brainstem's and the logger's are global. */
struct metrics_src {
	struct loop_metrics const      *loop;
	struct http_metrics const      *http;
	struct blocker_metrics const   *blockers;
	struct buf_stash_metrics const *stash;
	struct metrics_kept const      *kept;
	struct hist const              *methods;   /**< METHOD_N, by enum metrics_method */
};

/**
 * Render the metrics from entry @p *pos on into @p out, a bounded share of
 * them a call; returns 0 once all are, EAGAIN while some are left, or ENOMEM.
 */
extern int
metrics_render (struct metrics_src const *src,
                size_t                   *pos,
                struct buf               *out);

#endif /* TWINCEPTION_SRC_METRICS_H_ */
