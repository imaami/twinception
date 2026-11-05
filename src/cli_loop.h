/** @file cli_loop.h
 *
 * The loop of the UI thread of askai-cli. It is a struct loop (src/loop.h)
 * that the UI thread runs one turn at a time, each turn with its own
 * deadline.
 */
#ifndef TWINCEPTION_SRC_CLI_LOOP_H_
#define TWINCEPTION_SRC_CLI_LOOP_H_

#include <stdint.h>

#include "loop.h"

struct cli_loop;

typedef void cli_loop_turn_cb (struct cli_loop *loop);

/** The loop of the UI thread. Only the UI thread may access it. The
 *  callbacks of watched descriptors must not call Readline. after_turn runs
 *  after the events, messages and steps of each turn.
 */
struct cli_loop {
	struct loop       loop;
	struct loop_step  wake;         /**< the step that cli_loop_wake() queues */
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

/** Queues a step that does nothing on @p loop. The next turn then does not
 *  wait for events, and after_turn runs at its end. A module calls it after
 *  it queues work for after_turn, because only after_turn may call Readline
 *  or the callbacks of the protocol.
 */
extern void
cli_loop_wake (struct cli_loop *loop);

/** Runs one turn of loop->loop, and then after_turn unless an error is
 *  stored in loop->loop. The turn waits for events, messages, timers or
 *  ready steps, but not past @p deadline or until, which are absolute
 *  CLOCK_MONOTONIC times in nanoseconds. UINT64_MAX means no deadline. A
 *  call during a turn returns EBUSY, but after_turn may start another
 *  turn. Returns the error of epoll_pwait2(), or the error that
 *  loop_fail() stored in loop->loop.
 */
extern int
cli_loop_turn (struct cli_loop *loop,
               uint64_t         deadline);

#endif /* TWINCEPTION_SRC_CLI_LOOP_H_ */
