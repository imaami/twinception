/** @file cli_mailbox.h
 *
 * Bounded, object-owned mailbox for messages sent to the CLI loop.
 */
#ifndef TWINCEPTION_SRC_CLI_MAILBOX_H_
#define TWINCEPTION_SRC_CLI_MAILBOX_H_

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "cli_loop.h"
#include "list.h"

struct cli_mailbox;
struct cli_mailbox_msg;

typedef void cli_mailbox_cb (struct cli_mailbox     *mailbox,
                            struct cli_mailbox_msg *msg);

/** Embed in an owned message. bytes charges the complete allocation and
 * any owned payload against the corresponding admission limit.
 */
struct cli_mailbox_msg {
	struct list     hook;
	cli_mailbox_cb *fn;
	size_t          bytes;
	uint64_t        control;
};

/** Data and control have independent capacity, but share one FIFO so a
 * completion cannot overtake the data preceding it.
 */
struct cli_mailbox_limits {
	size_t messages;
	size_t bytes;
	size_t control_messages;
	size_t control_bytes;
};

struct cli_mailbox {
	pthread_mutex_t           mutex;
	struct list               mail;
	struct cli_loop_ref       wake;
	struct cli_loop_step      drain;
	struct cli_loop          *loop;
	struct cli_mailbox_limits  limits;
	size_t                    messages[2];
	size_t                    bytes[2];
	uint32_t                  closed;
	uint32_t                  mutex_ready;
};

extern int
cli_mailbox_init (struct cli_mailbox       *mailbox,
                   struct cli_loop          *loop,
                   struct cli_mailbox_limits limits);

/** Thread-safe. Success transfers ownership until fn receives the message.
 * EAGAIN means full; ESHUTDOWN means closed. On every error the caller keeps
 * ownership. A posted message must not be touched or posted again.
 */
extern int
cli_mailbox_post (struct cli_mailbox     *mailbox,
                   struct cli_mailbox_msg *msg,
                   bool                    control);

/** Owner-thread delivery, also usable outside a turn during error cleanup.
 * Deliver at most limit messages; leave any remainder runnable.
 */
extern int
cli_mailbox_dispatch (struct cli_mailbox *mailbox,
                      size_t              limit);

/** Owner thread only. Reject future posts, leaving accepted messages queued. */
extern void
cli_mailbox_close (struct cli_mailbox *mailbox);

/** Owner thread, after every producer has stopped. Calls fn for remaining
 * messages with closed set, then releases the descriptor and mutex.
 */
extern void
cli_mailbox_fini (struct cli_mailbox *mailbox);

#endif /* TWINCEPTION_SRC_CLI_MAILBOX_H_ */
