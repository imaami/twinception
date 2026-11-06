/** @file cli_signals.h
 *
 * Process signals delivered to one explicitly owned UI descriptor.
 */
#ifndef ASKAI_SRC_CLI_SIGNALS_H_
#define ASKAI_SRC_CLI_SIGNALS_H_

#include <signal.h>

#include "loop.h"

struct cli_signals;
typedef void cli_signals_cb (struct cli_signals *signals, unsigned number);

/** The flags of struct cli_signals. cli_signals_fini() restores the state
 *  of each flag that is set.
 */
enum {
	CLI_SIGNALS_MASKED  = 1u << 0,  /**< the signal mask, from previous */
	CLI_SIGNALS_IGNORED = 1u << 1,  /**< the action of SIGPIPE, from pipe */
};

struct cli_signals {
	struct loop_ref   ref;
	struct loop      *loop;
	cli_signals_cb   *fn;
	sigset_t          previous;
	struct sigaction  pipe;
	uint64_t          flags;
};

/** Call before creating threads or initializing libraries that create them. */
extern int
cli_signals_init (struct cli_signals *signals,
                  struct loop        *loop,
                  cli_signals_cb     *fn);

/** Call after every managed thread has stopped. */
extern void
cli_signals_fini (struct cli_signals *signals);

#endif /* ASKAI_SRC_CLI_SIGNALS_H_ */
