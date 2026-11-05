/** @file cli_mailbox.h
 *
 * Bounded, object-owned mailbox for messages sent to the CLI loop.
 */
#ifndef TWINCEPTION_SRC_CLI_MAILBOX_H_
#define TWINCEPTION_SRC_CLI_MAILBOX_H_

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "list.h"
#include "loop.h"

struct cli_mailbox;
struct cli_mailbox_msg;

typedef void cli_mailbox_cb (struct cli_mailbox     *mailbox,
                             struct cli_mailbox_msg *msg);

/** Embed this struct in each object that is posted to a mailbox. bytes is
 *  the size of that object and of the memory that is freed with it.
 *  cli_mailbox_post() adds bytes to the count of the class of the message,
 *  and cli_mailbox_dispatch() subtracts it.
 */
struct cli_mailbox_msg {
	struct list     hook;
	cli_mailbox_cb *fn;
	size_t          bytes;
	uint64_t        control;
};

/** messages and bytes are a number of messages of one class and their total
 *  size. They are the limits in struct cli_mailbox_limits and the counts of
 *  the queued messages in struct cli_mailbox.
 */
struct cli_mailbox_budget {
	size_t messages;
	size_t bytes;
};

/** The budgets of the classes of messages. budget[0] is the budget of data
 *  messages, and budget[1] the budget of control messages. The two classes
 *  share one FIFO, so a completion cannot overtake the data before it.
 */
struct cli_mailbox_limits {
	struct cli_mailbox_budget budget[2];
};

struct cli_mailbox {
	pthread_mutex_t           mutex;
	struct list               mail;
	struct loop_ref           wake;
	struct loop_step          drain;
	struct loop              *loop;
	struct cli_mailbox_limits limits;
	struct cli_mailbox_budget used[2];  /**< the messages and bytes queued in
	                                         each class */
	uint32_t                  closed;
	uint32_t                  mutex_ready;
};

extern int
cli_mailbox_init (struct cli_mailbox       *mailbox,
                  struct loop              *loop,
                  struct cli_mailbox_limits limits);

/** Posts @p msg as a control message if @p control is true, and otherwise
 *  as a data message. Any thread may call it. On success, the mailbox owns
 *  the message until it calls fn. Returns EAGAIN if the budget of the class
 *  is used up, and ESHUTDOWN after cli_mailbox_close(). After an error, the
 *  caller still owns the message. A posted message must not be changed or
 *  posted again.
 */
extern int
cli_mailbox_post (struct cli_mailbox     *mailbox,
                  struct cli_mailbox_msg *msg,
                  bool                    control);

/** Calls fn for at most @p limit queued messages in the order of posting,
 *  and queues the drain step if messages remain. Only the thread that owns
 *  the loop calls it, also outside a turn during cleanup. Returns 0 or the
 *  error of pthread_mutex_lock().
 */
extern int
cli_mailbox_dispatch (struct cli_mailbox *mailbox,
                      size_t              limit);

/** Call only on the thread that owns the loop. Rejects future posts and
 * leaves accepted messages queued. Returns 0 or the error of
 * pthread_mutex_lock().
 */
extern int
cli_mailbox_close (struct cli_mailbox *mailbox);

/** Owner thread, after every producer has stopped. Calls fn for remaining
 * messages with closed set, then releases the descriptor and mutex.
 */
extern void
cli_mailbox_fini (struct cli_mailbox *mailbox);

#endif /* TWINCEPTION_SRC_CLI_MAILBOX_H_ */
