/** @file home.h
 *
 * Home is a thread with its own loop.  It runs the jobs (src/job.h), which
 * are the upstream calls that the client loops hand over to it, and it
 * serves no client itself (design sections 1 and 4).  Home's door
 * (src/httpd.h) accepts the clients and places each on a client loop.
 *
 * Home owns the curl bridge (src/http.h) and the scheduler of the jobs.
 * The bridge's host lookups run on the blocker threads.
 *
 * Home ends a new job at once as BUSY if --max-calls jobs are in flight,
 * or if JOB_QUEUED jobs of the same class are already queued for the job's
 * principal.  The principal is the client's identity, else the call's
 * session, else the client's address.  A job that arrives while an
 * upstream slot is free for it, and while no job that would start before
 * it waits, starts without queueing.  The other jobs wait in their
 * principal's queue.  Home gives the free slots in turn to the principals
 * that have jobs queued, one job each.
 *
 * Home sends a job's request upstream, and ends the job at its deadline if
 * the job has not ended by then.  It parses the events of a streamed answer
 * into the job's text, tool calls and thinking, or collects a plain answer
 * without parsing it.  While the job waits, home publishes its place in its
 * principal's queue, and while the job runs, how far its answer has come
 * (src/job.h).  Then home completes the job.
 *
 * A circuit breaker opens after several upstream failures in a row, or
 * when the answer to a failed request says when to retry.  While the
 * breaker is open, home refuses new jobs and ends the jobs that wait.
 * After the time that the answer gave, or else after a backoff that
 * doubles each time the breaker opens, home lets one job through as a
 * probe.  If the probe succeeds, the breaker closes.
 *
 * The messages from the client loops admit or refuse new jobs, cancel jobs
 * that wait, and move running jobs to the lists of the bulk steps.  Bulk
 * steps of home's loop start the jobs, read their answers and cancel them,
 * with a limited amount of work in each step, because each call into
 * libcurl takes some tens of microseconds.  An admitted job may need a
 * descriptor for its upstream connection.  If the client connections and
 * the jobs in flight already hold as many descriptors as they may, the
 * door has the client loop with the least recently active idle connection
 * close that connection, if any connection is idle.
 *
 * When home drains, it cancels the jobs in flight and refuses new jobs.
 * It does not cancel durable background work at its provider, so that
 * askai can recover the work later.  Once all client loops have exited, no
 * job is in flight, and no provider cancellation or journal callback is
 * pending, home makes its loop exit.
 */
#ifndef TWINCEPTION_SRC_HOME_H_
#define TWINCEPTION_SRC_HOME_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "buf.h"

struct ai_endpoint;
struct blocker;
struct config;
struct home;
struct http_metrics;
struct httpd_home;
struct loop;
struct job;
struct journal_record;

/** Restore a provider turn from its journal records, after the turn's
 *  request checkpoint. Copies all retained bytes; SUBMIT later replays
 *  them without sending the original POST, then reattaches if possible.
 */
extern int
home_job_restore (struct job                  *job,
                  struct journal_record const *records);

/** Free a job's recovery state after its last home reference is gone. */
extern void
home_job_recovery_fini (struct job *job);

/** What home counts of its jobs: home writes, any thread reads (hist.h).
 *  submitted = completed + cancelled + rejected + in_flight.
 */
struct home_metrics {
	_Atomic uint64_t submitted;   /**< SUBMITs taken */
	_Atomic uint64_t completed;   /**< ended with the upstream's answer or
	                                   error */
	_Atomic uint64_t cancelled;   /**< by their front, or by a drain */
	_Atomic uint64_t rejected;    /**< ended without going upstream: busy,
	                                   the upstream unavailable, timed out
	                                   in the queue, or draining */
	_Atomic uint64_t in_flight;   /**< admitted, not ended */
	_Atomic uint64_t queued;      /**< of those, waiting for a slot */
	_Atomic uint64_t principals;  /**< with jobs queued */
	_Atomic uint64_t running;     /**< upstream calls under way */
	_Atomic uint64_t slots;       /**< upstream calls at once, at most */
	_Atomic uint64_t dispatched;  /**< jobs started upstream */
	_Atomic uint64_t aborted;     /**< upstream calls cut once their request
	                                   had gone: of jobs cancelled, or
	                                   preempted */
	_Atomic uint64_t chunks;      /**< events of streamed answers */
	_Atomic uint64_t stalls;      /**< answers ended by the chunk timeout */
	_Atomic uint64_t breaker;     /**< 0 closed, 1 open, 2 half-open */
	_Atomic uint64_t opens;       /**< times the breaker opened */
	_Atomic uint64_t probes;      /**< jobs it let through half-open */
	_Atomic uint64_t background;  /**< upstream calls of background jobs
	                                   under way */
	_Atomic uint64_t preempted;   /**< background jobs whose upstream call an
	                                   interactive one preempted */
};

/**
 * Home on @p loop, asking @p ep through a curl bridge that looks host names
 * up on @p blockers, with at most @p cfg's calls jobs in flight, each of
 * which @p door makes room for, and with CFG_TEST_FIFO, as a test aid, in
 * one queue for all, first in first out.  It counts into @p metrics and
 * @p http, whose atomic members are valid, and which outlive every loop that
 * renders them.  nullptr, with the error in @p *e, if it cannot be made.
 */
extern struct home *
home_create (struct loop              *loop,
             struct blocker           *blockers,
             struct httpd_home        *door,
             struct ai_endpoint const *ep,
             struct config const      *cfg,
             struct home_metrics      *metrics,
             struct http_metrics      *http,
             int                      *e);

/** Stop the jobs in flight, which complete, then free home. */
extern void
home_destroy (struct home **p_home);

/** Stop local transfers, a share a step, and refuse new ones. Durable
 *  provider jobs are suspended, not cancelled.
 */
extern void
home_drain (struct home *home);

/** The client loops have all exited: drain, and have the loop exit once no
 *  job is in flight.
 */
extern void
home_alone (struct home *home);

/** Begin a window of reuse of what home keeps, as buf_stash_age() does for
 *  a stash.
 */
extern void
home_age (struct home *home);

/** Free the principals' entries and the tokeners kept for reuse, up to
 *  @p max units of release work, at BUF_HIGH those unused since the last
 *  window began, and tokeners whose string buffer outgrew a slice; returns
 *  the units, 0 once none are left to free.
 */
extern size_t
home_relieve (struct home       *home,
              enum buf_pressure  level,
              size_t             max);

#endif /* TWINCEPTION_SRC_HOME_H_ */
