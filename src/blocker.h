/** @file blocker.h
 *
 * Blockers: threads for calls that can only block, such as regular-file
 * I/O, getaddrinfo() and malloc_trim(), off the loops (design sections 1,
 * 2, and 5).
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

struct blocker;

/** The blockers, not started yet, which write @p notify_fd, an eventfd, as
 *  each exits, so the caller keeps that open until the process exits;
 *  nullptr, with the error in @p *e, if they cannot be made.
 */
extern struct blocker *
blocker_create (int  notify_fd,
                int *e);

/** Start the blocker threads.  Returns 0, or why one could not start: those
 *  started run on, for blocker_stop() to end.
 */
extern int
blocker_start (struct blocker *blocker);

/** Queue @p job, its done.fn, work and loop set; EAGAIN if the queue is full
 *  or the blockers stop.
 */
extern int
blocker_submit (struct blocker     *blocker,
                struct blocker_job *job);

/** Tell the blockers to finish the work under way, cancel the rest and exit. */
extern void
blocker_stop (struct blocker *blocker);

/** Whether every blocker started has exited. */
extern bool
blocker_done (struct blocker const *blocker);

/** Free the blockers, once they are done. */
extern void
blocker_destroy (struct blocker **p_blocker);

extern struct blocker_metrics const *
blocker_metrics (struct blocker const *blocker);

#endif /* TWINCEPTION_SRC_BLOCKER_H_ */
