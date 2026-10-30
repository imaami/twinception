/** @file cli_loop.h
 *
 * CLI reactor: explicit descriptor ownership and complete event turns.
 */
#ifndef TWINCEPTION_SRC_CLI_LOOP_H_
#define TWINCEPTION_SRC_CLI_LOOP_H_

#include <stdint.h>

#include "list.h"

struct cli_loop;
struct cli_loop_ref;
struct cli_loop_step;

typedef void cli_loop_cb (struct cli_loop     *loop,
                         struct cli_loop_ref *ref,
                         uint32_t             events);
typedef void cli_loop_step_cb (struct cli_loop      *loop,
                              struct cli_loop_step *step);
typedef void cli_loop_turn_cb (struct cli_loop *loop);

/** Embed this in the object owning a watched descriptor. The descriptor is
 * borrowed: unwatching does not close it.
 */
struct cli_loop_ref {
	struct list  hook;
	cli_loop_cb *cb;
	uint32_t     events;
	int          fd;
};

/** A bounded piece of owner-thread work, initialized with its fn set and
 * an empty (zeroed) hook. Requeueing runs it in a later turn.
 */
struct cli_loop_step {
	struct list       hook;
	cli_loop_step_cb *fn;
};

/** One thread owns a loop. Descriptor callbacks must not enter Readline;
 * after_turn runs after dispatch and ready work.
 */
struct cli_loop {
	struct list          refs;
	struct list          ready;
	struct cli_loop_ref  timer;
	cli_loop_turn_cb    *after_turn;
	uint64_t             deadline;
	uint64_t             until;       /**< owner-imposed absolute wait limit */
	uint64_t             dispatching;
	int                  epfd;
	int                  error;
};

extern int
cli_loop_init (struct cli_loop *loop);

/** Unwatch borrowed descriptors. Call only outside a turn, after owners
 * have stopped using the loop.
 */
extern void
cli_loop_fini (struct cli_loop *loop);

extern int
cli_loop_watch (struct cli_loop     *loop,
                struct cli_loop_ref *ref,
                int                  fd,
                uint32_t             events,
                cli_loop_cb         *cb);

extern void
cli_loop_unwatch (struct cli_loop     *loop,
                  struct cli_loop_ref *ref);

/** Unwatch the descriptor of ref, if it has one, and close it. */
extern void
cli_loop_close (struct cli_loop     *loop,
                struct cli_loop_ref *ref);

/** Read the 8-byte counter of the eventfd or timerfd of ref. Returns the
 * count, or 0 if there was none or the read failed; a failure other than
 * EAGAIN becomes the loop error unless one is already set.
 */
extern uint64_t
cli_loop_counter (struct cli_loop     *loop,
                  struct cli_loop_ref *ref);

extern void
cli_loop_ready (struct cli_loop      *loop,
                struct cli_loop_step *step);

extern void
cli_loop_unready (struct cli_loop_step *step);

/** Wait for events, pending work, or an absolute CLOCK_MONOTONIC deadline
 * in nanoseconds. UINT64_MAX means no deadline. No periodic tick is used.
 * Calling from dispatch returns EBUSY; after_turn may start another turn.
 */
extern int
cli_loop_turn (struct cli_loop *loop,
               uint64_t         deadline);

#endif /* TWINCEPTION_SRC_CLI_LOOP_H_ */
