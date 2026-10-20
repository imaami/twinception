/** @file Network ownership and asynchronous Unix-socket HTTP transport. */
#ifndef TWINCEPTION_SRC_CLI_NETWORK_H_
#define TWINCEPTION_SRC_CLI_NETWORK_H_

#include <stddef.h>
#include <stdint.h>

#include "cstr_view.h"

struct cli_loop;
struct cli_mailbox;
struct cli_network;
struct loop;

/** Runs on the UI thread. Bytes are borrowed until the callback returns. */
typedef size_t cli_network_receive_fn (char   *bytes,
                                       size_t  size,
                                       size_t  count,
                                       void   *context);

typedef bool cli_network_ready_fn (void *context);

struct cli_network_result {
	long status;
	char message[256];
};

/** Called once on the UI thread after this call's response bytes. The result
 * is borrowed until return. Callbacks may submit, but must not wait. */
typedef void cli_network_done_fn (void                            *context,
                                  int                              error,
                                  struct cli_network_result const *result);

/** The UI loop and its mailbox outlive the network and its queued replies. */
extern struct cli_network *
cli_network_create (struct cli_loop    *ui,
                    struct cli_mailbox *mailbox,
                    int                *e);

extern void
cli_network_destroy (struct cli_network **network);

/** Stop admission and wake the owner. Keep its mailbox alive until done. */
extern void
cli_network_stop (struct cli_network *network);

/** UI-thread query. Destroy only after this becomes true. */
extern bool
cli_network_done (struct cli_network const *network);

/** Worker completions may post here while the network is alive. */
extern struct loop *
cli_network_loop (struct cli_network *network);

/** UI-thread query, including transfer completion still waiting in its inbox. */
extern bool
cli_network_active (struct cli_network const *network);

/** UI-thread output backpressure predicate; nullptr always accepts data. */
extern void
cli_network_set_ready (struct cli_network  *network,
                       cli_network_ready_fn *ready,
                       void                *context);

/** Deliver bounded response work after a complete UI turn, including turns
 * serviced from Readline input hooks. Never call from event dispatch. */
extern void
cli_network_service (struct cli_network *network);

/** Cancel the foreground call, or every call when force is true. */
extern void
cli_network_cancel (struct cli_network *network,
                    bool                force);

/** Cancel one admitted call; force overrides its interruptible setting. */
extern void
cli_network_cancel_call (struct cli_network *network,
                         uint64_t            id,
                         bool                force);

/** Copy and admit a request. On success, publish its id and eventually call
 * done, including on shutdown. An admission error does not call done. */
extern int
cli_network_submit (struct cli_network      *network,
                    struct cstr_view         socket,
                    struct cstr_view         session,
                    struct cstr_view         wire,
                    long                     timeout,
                    bool                     interruptible,
                    cli_network_receive_fn  *header,
                    cli_network_receive_fn  *body,
                    void                    *context,
                    cli_network_done_fn     *done,
                    uint64_t                *id);

/**
 * Submit copied request data, then service UI turns until completion. All
 * response callbacks run on this calling thread. Only one synchronous call
 * may be outstanding; nested waits return EBUSY. A successful callback must
 * return size * count. Cancellation returns EINTR.
 */
extern int
cli_network_perform (struct cli_network       *network,
                     struct cstr_view          socket,
                     struct cstr_view          session,
                     struct cstr_view          wire,
                     long                      timeout,
                     bool                      interruptible,
                     cli_network_receive_fn   *header,
                     cli_network_receive_fn   *body,
                     void                     *context,
                     struct cli_network_result *result);

#endif /* TWINCEPTION_SRC_CLI_NETWORK_H_ */
