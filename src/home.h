/** @file Home: the sidekick thread that runs every client loop's jobs, and
 *  serves no client (design sections 1 and 4).
 *
 * Home owns the curl bridge (src/http.h), with its timers and its host
 * lookups on the blockers, the registry of jobs in flight, and their
 * scheduler: admission, a cap on the jobs in flight beyond which a job ends
 * at once as BUSY, and first-in first-out starts.  It runs a job's request
 * upstream, gathers the answer into the job's result, which it never parses,
 * a step's share of receiving at a time, and completes the job (src/job.h),
 * at its deadline if not before.  The messages of jobs only admit, refuse or
 * note them; bulk steps start and cancel them, a share at a time.  The
 * registry finds the jobs a legacy notifications/cancelled names by their
 * keys, in O(1).  Each job admitted counts against the descriptors, which
 * home's door (src/httpd.h) budgets.  A home that drains cancels the jobs in
 * flight and refuses new ones; once the client loops have all exited and no
 * job is in flight, it has its loop exit.
 */
#ifndef TWINCEPTION_SRC_HOME_H_
#define TWINCEPTION_SRC_HOME_H_

#include <stdatomic.h>
#include <stdint.h>

struct ai_endpoint;
struct blockers;
struct home;
struct http_metrics;
struct httpd_home;
struct loop;

/** What home counts of its jobs: home writes, any thread reads (hist.h).
 *  submitted = completed + cancelled + rejected + in_flight. */
struct home_metrics {
	_Atomic uint64_t submitted;  /**< SUBMITs taken */
	_Atomic uint64_t completed;  /**< ended with the upstream's answer or
	                                  error */
	_Atomic uint64_t cancelled;  /**< by their front, or by a drain */
	_Atomic uint64_t rejected;   /**< refused: too many in flight, or
	                                  draining */
	_Atomic uint64_t in_flight;  /**< admitted, not ended */
};

/**
 * Home on @p loop, asking @p ep through a curl bridge that looks host names
 * up on @p blockers, with at most @p max jobs in flight, each of which
 * @p door makes room for.  It counts into @p metrics and @p http, whose
 * atomic members are valid, and which outlive every loop that renders them.
 */
extern int
home_init (struct home               **p_home,
           struct loop                *loop,
           struct blockers            *blockers,
           struct httpd_home          *door,
           struct ai_endpoint const   *ep,
           uint32_t                    max,
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

#endif /* TWINCEPTION_SRC_HOME_H_ */
