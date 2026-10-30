/** @file cli_io.h
 *
 * Blocking CLI work, returned to its UI owner through a mailbox.
 */
#ifndef TWINCEPTION_SRC_CLI_IO_H_
#define TWINCEPTION_SRC_CLI_IO_H_

#include "blocker.h"
#include "cli_mailbox.h"

struct cli_io;

/** Embed in the object containing a job's arguments and result. Only work
 * runs on a blocker; done runs on the UI. From successful submission until
 * done, the caller must not access the job or its mutable arguments.
 */
struct cli_io_job {
	struct blocker_job     blocker;
	struct cli_mailbox_msg result;
	struct cli_io         *io;
	int                 (*work) (struct cli_io_job *job);
	void                (*done) (struct cli_io_job *job);
	int                   error;
};

extern struct cli_io *
cli_io_create (struct loop        *network,
               struct cli_mailbox *mailbox,
               int                *e);

/** UI thread only. A refused submission leaves ownership with its caller. */
extern int
cli_io_submit (struct cli_io     *io,
               struct cli_io_job *job);

extern void
cli_io_stop (struct cli_io *io);

/** True only after workers exited and the UI received all completions. */
extern bool
cli_io_done (struct cli_io const *io);

/** Requires cli_io_done(). Keep the network loop and mailbox alive until
 * then. A stuck syscall is left to process exit, never pthread_cancel().
 */
extern void
cli_io_destroy (struct cli_io **p_io);

#endif /* TWINCEPTION_SRC_CLI_IO_H_ */
