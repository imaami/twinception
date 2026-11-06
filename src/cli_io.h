/** @file cli_io.h
 *
 * Blocking CLI work on the blocker threads. A blocker posts each job back
 * to the UI loop when the work is done.
 */
#ifndef ASKAI_SRC_CLI_IO_H_
#define ASKAI_SRC_CLI_IO_H_

#include "blocker.h"
#include "loop.h"

struct cli_io;

/** Embed in the object containing a job's arguments and result. Only work
 * runs on a blocker; done runs on the UI. From successful submission until
 * done, the caller must not access the job or its mutable arguments. When
 * done runs, blocker.error is the value that work returned, or ECANCELED
 * if work did not run.
 */
struct cli_io_job {
	struct blocker_job  blocker;
	struct cli_io      *io;
	int              (*work) (struct cli_io_job *job);
	void             (*done) (struct cli_io_job *job);
};

/** Starts the blocker threads, which post the jobs back to @p ui. Returns
 *  nullptr, with the error in @p *e, if the eventfd for the threads cannot
 *  be made or watched. If the blocker cannot be made or not all threads
 *  start, it stops the threads that started and returns the object with the
 *  error in @p *e. The caller then waits until cli_io_done() returns true,
 *  as after a normal start.
 */
extern struct cli_io *
cli_io_create (struct loop *ui,
               int         *e);

/** UI thread only. A refused submission leaves ownership with its caller. */
extern int
cli_io_submit (struct cli_io     *io,
               struct cli_io_job *job);

extern void
cli_io_stop (struct cli_io *io);

/** True only after workers exited and the UI received all completions. */
extern bool
cli_io_done (struct cli_io const *io);

/** Requires cli_io_done(). Keep the UI loop alive until then. A stuck
 * syscall is left to process exit, never pthread_cancel().
 */
extern void
cli_io_destroy (struct cli_io **p_io);

#endif /* ASKAI_SRC_CLI_IO_H_ */
