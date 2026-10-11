/** @file Home: the sidekick thread that runs every client loop's jobs, and
 *  serves no client (design sections 1 and 4).
 *
 * Home owns the curl bridge (src/http.h), with its timers and its host
 * lookups on the blockers, the registry of jobs in flight, and their
 * scheduler.  Admission ends a job at once as BUSY with --max-calls jobs in
 * flight, or with eight queued for its principal, the call's session or else
 * its client's address.  A job that a free slot takes at once, nobody
 * waiting, queues for none; the others wait in their principal's queue for
 * one of the S upstream slots, which home gives the principals with jobs
 * queued in turn, a job each, round-robin.  It runs a job's request upstream
 * until its deadline at most, reads a streamed answer's events a step's
 * share at a time into its text, or gathers a plain answer, which it never
 * parses, likewise, and completes the job (src/job.h), publishing its
 * progress meanwhile.  A breaker on the upstream's failures refuses jobs
 * while the upstream seems down, and lets a probe through now and then.  The
 * messages of jobs only admit, refuse or note them; bulk steps start, read
 * and cancel them, a share at a time.  The registry finds the jobs a legacy
 * notifications/cancelled names by their keys, in O(1).  Each job admitted
 * counts against the descriptors, which home's door (src/httpd.h) budgets.
 * A home that drains cancels the jobs in flight and refuses new ones; once
 * the client loops have all exited and no job is in flight, it has its loop
 * exit.
 */
#ifndef TWINCEPTION_SRC_HOME_H_
#define TWINCEPTION_SRC_HOME_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "buf.h"

struct ai_endpoint;
struct blockers;
struct home;
struct http_metrics;
struct httpd_home;
struct loop;

/** What home counts of its jobs: home writes, any thread reads (hist.h).
 *  submitted = completed + cancelled + rejected + in_flight. */
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
	_Atomic uint64_t aborted;     /**< upstream calls of jobs cancelled once
	                                   their request had gone */
	_Atomic uint64_t chunks;      /**< events of streamed answers */
	_Atomic uint64_t stalls;      /**< answers ended by the chunk timeout */
	_Atomic uint64_t breaker;     /**< 0 closed, 1 open, 2 half-open */
	_Atomic uint64_t opens;       /**< times the breaker opened */
	_Atomic uint64_t probes;      /**< jobs it let through half-open */
};

/**
 * Home on @p loop, asking @p ep through a curl bridge that looks host names
 * up on @p blockers, with at most @p max jobs in flight, each of which
 * @p door makes room for, and with @p fifo, as a test aid, in one queue for
 * all, first in first out.  It counts into @p metrics and @p http, whose
 * atomic members are valid, and which outlive every loop that renders them.
 */
extern int
home_init (struct home               **p_home,
           struct loop                *loop,
           struct blockers            *blockers,
           struct httpd_home          *door,
           struct ai_endpoint const   *ep,
           uint32_t                    max,
           bool                        fifo,
           struct home_metrics        *metrics,
           struct http_metrics        *http);

/** Cancel the jobs in flight, which complete, then free home. */
extern void
home_fini (struct home **p_home);

/** Cancel the jobs in flight, a share a step, and refuse new ones. */
extern void
home_drain (struct home *home);

/** The client loops have all exited: drain, and have the loop exit once no
 *  job is in flight. */
extern void
home_alone (struct home *home);

/** Begin a window of reuse of what home keeps, as buf_stash_age() does for
 *  a stash. */
extern void
home_age (struct home *home);

/** Free the principals' entries and the tokeners kept for reuse, up to
 *  @p max units of release work, at BUF_HIGH those unused since the last
 *  window began, and tokeners whose string buffer outgrew a slice; returns
 *  the units, 0 once none are left to free. */
extern size_t
home_relieve (struct home       *home,
              enum buf_pressure  level,
              size_t             max);

#endif /* TWINCEPTION_SRC_HOME_H_ */
