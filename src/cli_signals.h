/** @file Process signals delivered to one explicitly owned UI descriptor. */
#ifndef TWINCEPTION_SRC_CLI_SIGNALS_H_
#define TWINCEPTION_SRC_CLI_SIGNALS_H_

#include <signal.h>

#include "cli_loop.h"

struct cli_signals;
typedef void cli_signals_cb (struct cli_signals *signals, unsigned number);

struct cli_signals {
	struct cli_loop_ref ref;
	struct cli_loop    *loop;
	cli_signals_cb     *fn;
	sigset_t           previous;
	struct sigaction   pipe;
	uint32_t           masked;
	uint32_t           ignored;
};

/** Call before creating threads or initializing libraries that create them. */
extern int
cli_signals_init (struct cli_signals *signals,
                  struct cli_loop    *loop,
                  cli_signals_cb     *fn);

/** Call after every managed thread has stopped. */
extern void
cli_signals_fini (struct cli_signals *signals);

#endif /* TWINCEPTION_SRC_CLI_SIGNALS_H_ */
