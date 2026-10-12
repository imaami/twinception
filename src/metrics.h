/** @file The core's metrics in Prometheus's text format, for GET /metrics.
 *
 * Every histogram and gauge has one writer thread, as do most counters; those
 * of log lines and of blocker jobs, which several threads count into, are
 * atomic sums of rare events.  All are read with relaxed atomics, so
 * rendering takes no lock and stops no one: per loop, each client loop's and
 * home's, histograms of turn and step times, timer lateness and inbox delay,
 * the most work a step did, and the storage its stash keeps and what it gave
 * back; per client loop the storage it keeps besides, its tool calls'
 * cancellations, its request times by MCP method and its connections; home's
 * jobs, its scheduler's queue and slots, the upstream breaker, streamed
 * answers' chunks, and its upstream wait and run times and host lookups;
 * blocker jobs; the man tool's manual, its pages, hits and runs of man;
 * tasks (src/task.h), those each client loop holds, its slices and moves,
 * and how they ended;
 * log lines dropped; watchdog reports; the brainstem's resource gauges, and
 * memory pressure and its inputs.  Histograms have fixed buckets, from 1 µs
 * to 300 s.
 */
#ifndef TWINCEPTION_SRC_METRICS_H_
#define TWINCEPTION_SRC_METRICS_H_

#include <stddef.h>

#include "buf.h"
#include "hist.h"

struct blocker_metrics;
struct home_metrics;
struct http_metrics;
struct loop_metrics;
struct manual_metrics;
struct task_metrics;
struct tasks_metrics;

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
	_Atomic uint64_t jobs;      /**< jobs let go of */
	_Atomic uint64_t bytes[2];  /**< what the requests hold, and the pooled
	                                 tokeners' string buffers */
};

/** What a client loop counts of its tool calls' jobs: the loop writes, any
 *  thread reads. */
struct metrics_calls {
	_Atomic uint64_t cancelled;  /**< calls it cancelled while their job ran */
	_Atomic uint64_t dropped;    /**< completions it dropped, their call gone:
	                                  a stale handle */
};

/** What a client loop's MCP counts: the loop writes, any thread reads.  It
 *  lasts until every loop has finished, as any loop may render it. */
struct metrics_mcp {
	struct metrics_kept  kept;
	struct metrics_calls calls;
	struct hist          methods[METHOD_N];  /**< by enum metrics_method */
};

/** Initialize @p m, empty, in storage just allocated (hist.h on atomics). */
static inline void
metrics_mcp_init (struct metrics_mcp *m)
{
	atomic_init(&m->kept.spares, 0);
	atomic_init(&m->kept.jobs, 0);
	atomic_init(&m->kept.bytes[0], 0);
	atomic_init(&m->kept.bytes[1], 0);
	atomic_init(&m->calls.cancelled, 0);
	atomic_init(&m->calls.dropped, 0);
	for (size_t i = 0; i < METHOD_N; ++i)
		hist_init(&m->methods[i]);
}

/** What a client loop counts of its connections: the loop writes, any
 *  thread reads. */
struct metrics_conns {
	_Atomic uint64_t open;   /**< connections it serves */
	_Atomic uint64_t taken;  /**< connections home placed on it */
};

/** A loop's metrics, labelled with its name; those it lacks are nullptr.  Each
 *  loop sets its own as it starts, before home opens its door to clients. */
struct metrics_loop {
	char                            name[8];
	struct loop_metrics const      *loop;
	struct buf_stash_metrics const *stash;
	struct metrics_kept const      *kept;
	struct metrics_calls const     *calls;
	struct hist const              *methods;  /**< METHOD_N, by enum
	                                               metrics_method */
	struct metrics_conns const     *conns;
	struct task_metrics const      *tasks;    /**< if askai offers test_task */
};

/** Where the metrics are; the brainstem's and the logger's are global. */
struct metrics_src {
	struct metrics_loop          *loops;     /**< the client loops', which
	                                              have all, then home's */
	struct http_metrics const    *http;
	struct blocker_metrics const *blockers;
	struct home_metrics const    *jobs;
	struct manual_metrics const  *man;       /**< if askai offers man */
	struct tasks_metrics const   *tasks;     /**< if askai offers test_task */
	size_t                        n_loops;
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
