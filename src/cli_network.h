/** @file cli_network.h
 *
 * Network ownership and asynchronous Unix-socket HTTP transport.
 */
#ifndef ASKAI_SRC_CLI_NETWORK_H_
#define ASKAI_SRC_CLI_NETWORK_H_

#include <stddef.h>
#include <stdint.h>

#include "cstr_view.h"

/** The MCP protocol version that each request names and a session negotiates. */
#define CLI_VERSION "2025-11-25"

struct cli_loop;
struct cli_network;

/** Runs on the UI thread. Bytes are borrowed until the callback returns.
 * Returns 0, or an error number that fails and cancels the call.
 */
typedef int cli_network_receive_fn (struct cstr_view  bytes,
                                    void             *context);

typedef bool cli_network_ready_fn (void *context);

struct cli_network_result {
	long status;
	char message[256];
};

/** A call that a cancel without force ends. Other calls are for control,
 *  and only a forced cancel ends them.
 */
enum { CLI_NETWORK_INTERRUPTIBLE = 1u << 0 };

/** A request to AskAI. Admission copies the views, and the call passes
 *  context to header and body. socket ends in a NUL.
 */
struct cli_network_post {
	struct cstr_view        socket;
	struct cstr_view        session;
	struct cstr_view        wire;
	cli_network_receive_fn *header;
	cli_network_receive_fn *body;
	void                   *context;
	long                    timeout;  /**< seconds, 0 for none */
	uint64_t                flags;    /**< CLI_NETWORK_INTERRUPTIBLE or 0 */
};

/** Called once on the UI thread after this call's response bytes. The result
 * is borrowed until return. Callbacks may submit, but must not wait.
 */
typedef void cli_network_done_fn (void                            *context,
                                  int                              error,
                                  struct cli_network_result const *result);

/** The network posts its replies to @p ui, which outlives it. */
extern struct cli_network *
cli_network_create (struct cli_loop *ui,
                    int             *e);

extern void
cli_network_destroy (struct cli_network **network);

/** Stop admission and the network thread, which ends its calls and then
 * wakes the UI loop.
 */
extern void
cli_network_stop (struct cli_network *network);

/** UI-thread query: true once the thread has ended and the UI loop has
 * handled every message of the network. Destroy only after this becomes
 * true.
 */
extern bool
cli_network_done (struct cli_network const *network);

/** UI-thread query, including transfer completion still waiting in its inbox. */
extern bool
cli_network_active (struct cli_network const *network);

/** UI-thread output backpressure predicate; nullptr always accepts data. */
extern void
cli_network_set_ready (struct cli_network   *network,
                       cli_network_ready_fn *ready,
                       void                 *context);

/** Deliver bounded response work after a complete UI turn, including turns
 * serviced from Readline input hooks. Never call from event dispatch.
 */
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
 * done, including on shutdown. An admission error does not call done.
 */
extern int
cli_network_submit (struct cli_network            *network,
                    struct cli_network_post const *post,
                    cli_network_done_fn           *done,
                    uint64_t                      *id);

/**
 * Submit copied request data, then service UI turns until completion. All
 * response callbacks run on this calling thread. Only one synchronous call
 * may be outstanding; nested waits return EBUSY. Cancellation returns
 * EINTR.
 */
extern int
cli_network_perform (struct cli_network            *network,
                     struct cli_network_post const *post,
                     struct cli_network_result     *result);

#endif /* ASKAI_SRC_CLI_NETWORK_H_ */
