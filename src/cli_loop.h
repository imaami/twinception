/** @file CLI reactor: explicit descriptor ownership and complete event turns. */
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
typedef void cli_loop_release_cb (struct cli_loop_ref *ref);
typedef void cli_loop_step_cb (struct cli_loop      *loop,
                              struct cli_loop_step *step);
typedef void cli_loop_turn_cb (struct cli_loop *loop);

/** Embed this in the object owning a watched descriptor. The descriptor is
 * borrowed: unwatching does not close it. */
struct cli_loop_ref {
	struct list          hook;
	cli_loop_cb         *cb;
	cli_loop_release_cb *release;
	uint32_t             events;
	int                  fd;
};

/** A bounded piece of owner-thread work, initialized with its fn set and
 * an empty (zeroed) hook. Requeueing runs it in a later turn. */
struct cli_loop_step {
	struct list       hook;
	cli_loop_step_cb *fn;
};

/** One thread owns a loop. Descriptor callbacks must not enter Readline;
 * after_turn runs after dispatch, ready work and deferred reclamation. */
struct cli_loop {
	struct list          refs;
	struct list          dead;
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

/** Unwatch borrowed descriptors and release deferred objects. Call only
 * outside a turn, after owners have stopped using the loop. */
extern void
cli_loop_fini (struct cli_loop *loop);

extern int
cli_loop_watch (struct cli_loop     *loop,
                struct cli_loop_ref *ref,
                int                  fd,
                uint32_t             events,
                cli_loop_cb         *cb);

extern int
cli_loop_modify (struct cli_loop     *loop,
                 struct cli_loop_ref *ref,
                 uint32_t             events);

extern void
cli_loop_unwatch (struct cli_loop     *loop,
                  struct cli_loop_ref *ref);

/** Unwatch now; release after the event batch and ready callbacks finish.
 * An unwatched ref must remain alive until that point even when it has no
 * release callback. */
extern void
cli_loop_release (struct cli_loop     *loop,
                  struct cli_loop_ref *ref,
                  cli_loop_release_cb *release);

extern void
cli_loop_ready (struct cli_loop      *loop,
                struct cli_loop_step *step);

extern void
cli_loop_unready (struct cli_loop_step *step);

/** Wait for events, pending work, or an absolute CLOCK_MONOTONIC deadline
 * in nanoseconds. UINT64_MAX means no deadline. No periodic tick is used.
 * Calling from dispatch returns EBUSY; after_turn may start another turn. */
extern int
cli_loop_turn (struct cli_loop *loop,
               uint64_t         deadline);

/** CLOCK_MONOTONIC in nanoseconds. */
extern uint64_t
cli_loop_clock (void);

#endif /* TWINCEPTION_SRC_CLI_LOOP_H_ */
