/** @file cli_loop.h
 *
 * The loop of the UI thread of askai-cli: a struct loop (src/loop.h) that
 * the UI runs one turn at a time, each with a deadline of its own.
 */
#ifndef TWINCEPTION_SRC_CLI_LOOP_H_
#define TWINCEPTION_SRC_CLI_LOOP_H_

#include <stdint.h>

#include "loop.h"

struct cli_loop;

typedef void cli_loop_turn_cb (struct cli_loop *loop);

/** One thread owns a loop. Descriptor callbacks must not enter Readline;
 * after_turn runs after the events, messages and steps of a turn.
 */
struct cli_loop {
	struct loop       loop;
	cli_loop_turn_cb *after_turn;
	uint64_t          until;        /**< the latest deadline of a turn */
	uint64_t          dispatching;  /**< bool: a turn is under way */
};

/** Initializes @p loop and calls loop_init() on loop->loop, so the atomic
 *  members of loop->loop must be valid as loop_init() requires. Returns 0
 *  or an error number. After a failure too, the caller must call
 *  cli_loop_fini() to free what loop_init() allocated.
 */
extern int
cli_loop_init (struct cli_loop *loop);

/** Calls loop_fini() on loop->loop. Call it exactly once after each
 *  cli_loop_init(), also after a failure. Call it outside a turn, after the
 *  other modules make their last call on loop->loop.
 */
extern void
cli_loop_fini (struct cli_loop *loop);

/** Wait for events, pending work, or an absolute CLOCK_MONOTONIC deadline
 * in nanoseconds. UINT64_MAX means no deadline. No periodic tick is used.
 * Calling from dispatch returns EBUSY; after_turn may start another turn.
 * Returns the error of epoll_pwait2() or the error that loop_fail() stored
 * in loop->loop.
 */
extern int
cli_loop_turn (struct cli_loop *loop,
               uint64_t         deadline);

#endif /* TWINCEPTION_SRC_CLI_LOOP_H_ */
