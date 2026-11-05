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

/** Initializes @p loop. The atomic members of loop->loop must be valid: the
 *  object is declared with an initializer, or loop_place() initialized
 *  them. Returns 0 or an error number.
 */
extern int
cli_loop_init (struct cli_loop *loop);

/** Unwatch borrowed descriptors. Call only outside a turn, after owners
 * have stopped using the loop.
 */
extern void
cli_loop_fini (struct cli_loop *loop);

/** Unwatch the descriptor of ref, if it has one, and close it. */
extern void
cli_loop_close (struct cli_loop *loop,
                struct loop_ref *ref);

/** Read the 8-byte counter of the eventfd or timerfd of @p ref into
 * @p *count, and return 0. For an empty counter, read() fails with
 * EAGAIN, and the function stores 0 and returns 0. After any other
 * error, it stores 0 and returns the error number of read(), or EIO for a
 * short read.
 */
extern int
cli_loop_ref_counter (struct loop_ref const *ref,
                      uint64_t              *count);

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
