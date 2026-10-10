/** @file Blockers: threads for calls that can only block, such as regular-file
 *  I/O and getaddrinfo(), off the loops (design sections 1 and 2).
 *
 * A loop submits a job to a bounded queue, a mutex and a condition variable
 * over a list, which a full queue refuses.  One of the blockers runs the
 * job's work, then posts the job's message back to the loop's inbox, whose
 * handler takes the job back.  Every job submitted comes back exactly once;
 * once the blockers are told to stop, the jobs still queued come back with
 * error ECANCELED, their work not done.  The threads are detached: each counts
 * itself out as it exits, so one stuck in a call can be left to the process's
 * exit.
 */
#ifndef TWINCEPTION_SRC_BLOCKER_H_
#define TWINCEPTION_SRC_BLOCKER_H_

#include <stdatomic.h>
#include <stdint.h>

#include "list.h"
#include "loop.h"

/** Blocker threads, so that a slow call does not hold up every other. */
enum { BLOCKER_THREADS = 2 };

struct blocker_job {
	struct msg   done;   /**< posted to loop once done */
	struct list  hook;   /**< on the queue */
	/** Runs on a blocker. */
	void       (*work) (struct blocker_job *job);
	struct loop *loop;
	int          error;  /**< ECANCELED: work not done */
};

/** Counts the blockers keep, for any thread to read. */
struct blocker_metrics {
	_Atomic uint64_t done;     /**< jobs whose work was done */
	_Atomic uint64_t refused;  /**< jobs a full or stopped queue refused */
};

struct blockers;

/** Start the blockers; each writes @p notify_fd, an eventfd, as it exits,
 *  so the caller keeps that open until the process exits. */
extern int
blockers_start (struct blockers **p_blockers,
                int               notify_fd);

/** Queue @p job, its done.fn, work and loop set; EAGAIN if the queue is full
 *  or the blockers stop. */
extern int
blockers_submit (struct blockers    *blockers,
                 struct blocker_job *job);

/** Tell the blockers to finish the work under way, cancel the rest and exit. */
extern void
blockers_stop (struct blockers *blockers);

/** Whether every blocker has exited. */
extern bool
blockers_done (struct blockers const *blockers);

/** Free the blockers, once they are done. */
extern void
blockers_fini (struct blockers **p_blockers);

extern struct blocker_metrics const *
blockers_metrics (struct blockers const *blockers);

#endif /* TWINCEPTION_SRC_BLOCKER_H_ */
