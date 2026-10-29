#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

#include "cli_loop.h"
#include "cli_mailbox.h"
#include "cli_network.h"
#include "cli_socket.h"
#include "compat.h"
#include "loop.h"
#include "util.h"

enum {
	CLI_NETWORK_BYTES = 256 * 1024,
	CLI_NETWORK_MESSAGES = 64,
	CLI_NETWORK_CALLS = 16,
	CLI_NETWORK_CONTROL_CALLS = 2,
	CLI_NETWORK_REQUEST_BYTES = 32 * 1024 * 1024,
};

enum {
	CLI_NETWORK_INTERRUPTIBLE = 1u << 0,
	CLI_NETWORK_FINISHED = 1u << 1,
	CLI_NETWORK_DONE = 1u << 2,
	CLI_NETWORK_ABANDONED = 1u << 3,
};

enum {
	CLI_NETWORK_ATTACHED = 1u << 0,
	CLI_NETWORK_PAUSED = 1u << 1,
};

struct cli_network {
	struct loop                 loop;
	struct loop_timer           timer;
	struct loop_step            timeout;
	struct cli_loop_step        delivery;
	struct msg                  progress;
	struct msg                  cancel;
	struct msg                  stop;
	struct cli_mailbox_msg       ended;
	struct list                 sockets;
	struct list                 requests;
	struct list                 completions;
	struct list                 calls; /* UI thread only. */
	struct cli_loop            *ui;
	struct cli_mailbox         *mailbox;
	struct cli_network_call    *waiting; /* UI thread only. */
	cli_network_ready_fn       *ready;
	void                       *ready_context;
	CURLM                      *multi;
	pthread_t                   thread;
	uint64_t                    sequence; /* UI thread only. */
	size_t                      request_bytes; /* UI thread only. */
	size_t                      call_count; /* UI thread only. */
	size_t                      bytes; /* Network thread's aggregate response credit. */
	size_t                      messages;
	_Atomic int                 failure;
	_Atomic bool                progress_pending;
	_Atomic bool                cancel_pending;
	_Atomic bool                exited;
	bool                        started;
	bool                        joined;
	bool                        stopping;
	bool                        delivering;
};

static void cli_network_progress(struct cli_network *network);

static void
cli_network_finish (struct cli_network_request *request,
                    int                         error,
                    char const                 *detail)
{
	struct cli_network *network = request->network;
	if (error && !request->error)
		request->error = error;
	if (detail && !request->result.message[0])
		(void)util_format(request->result.message, sizeof request->result.message, "%s", detail);
	if (request->easy && curl_easy_getinfo(request->easy, CURLINFO_RESPONSE_CODE,
	                                     &request->result.status) != CURLE_OK && !request->error)
		request->error = EIO;
	if (request->flags & CLI_NETWORK_ATTACHED) {
		(void)curl_multi_remove_handle(network->multi, request->easy);
		request->flags &= ~CLI_NETWORK_ATTACHED;
	}
	cli_network_request_release(request);
	list_del(&request->hook);
	list_append(&network->completions, &request->hook);
	cli_network_publish(network);
}

static void
cli_network_check (struct cli_network *network)
{
	int left;
	CURLMsg *message;
	while ((message = curl_multi_info_read(network->multi, &left))) {
		if (message->msg != CURLMSG_DONE)
			continue;
		struct cli_network_request *request = nullptr;
		if (curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &request) == CURLE_OK && request) {
			CURLcode result = message->data.result;
			cli_network_finish(request, result == CURLE_OK ? 0 : EIO,
			                    result == CURLE_OK ? nullptr : curl_easy_strerror(result));
		}
	}
}

static void
cli_network_deliver (struct cli_network *network)
{
	size_t bytes = 0;
	for (size_t count = network->call_count; count && !list_is_empty(&network->calls); --count) {
		struct cli_network_call *call = container_of(network->calls.next, struct cli_network_call, hook);
		list_del(&call->hook);
		list_append(&network->calls, &call->hook);
		while (!list_is_empty(&call->received)) {
			struct cli_network_chunk *chunk = container_of(call->received.next,
			                                               struct cli_network_chunk, mail.hook);
			if (!call->error && !network->mailbox->closed) {
				if (bytes >= 64 * 1024 || (network->ready && !network->ready(network->ready_context)))
					break;
				cli_network_receive_fn *receive = chunk->header ? call->header : call->body;
				int e = receive ? receive(chunk->bytes, chunk->length, call->context) : 0;
				if (e) {
					call->error = e;
					cli_network_cancel_call(network, call->id, true);
				}
			}
			bytes += chunk->length;
			list_del(&chunk->mail.hook);
			cli_network_chunk_return(chunk);
		}
		if ((call->flags & CLI_NETWORK_FINISHED) && list_is_empty(&call->received)) {
			struct cli_network_request *request = container_of(call, struct cli_network_request, call);
			if (!call->error)
				call->error = request->error;
			if (call->result)
				*call->result = request->result;
			call->flags |= CLI_NETWORK_DONE;
			list_del(&call->hook);
			--network->call_count;
			network->request_bytes -= call->bytes;
			if (call->complete)
				call->complete(call->context, call->error, &request->result);
			if (call->complete || (call->flags & CLI_NETWORK_ABANDONED))
				cli_network_request_destroy(&request);
		}
		if (bytes >= 64 * 1024)
			break;
	}
}

static void
cli_network_chunk_received (struct cli_mailbox     *mailbox,
                            struct cli_mailbox_msg *message)
{
	(void)mailbox;
	struct cli_network_chunk *chunk = container_of(message, struct cli_network_chunk, mail);
	list_append(&chunk->call->received, &chunk->mail.hook);
	cli_loop_ready(chunk->network->ui, &chunk->network->delivery);
}

static void
cli_socket_step (struct loop      *loop,
                 struct loop_step *step)
{
	struct cli_socket *socket = to_cli_socket(step);
	int events = socket->events;
	socket->events = 0;
	cli_network_action(container_of(loop, struct cli_network, loop), socket->fd, events);
}

static void
cli_socket_event (struct loop     *loop,
                  struct loop_ref *ref,
                  uint32_t         events)
{
	struct cli_socket *socket = to_cli_socket(ref);
	if (events & EPOLLIN)
		socket->events |= CURL_CSELECT_IN;
	if (events & EPOLLOUT)
		socket->events |= CURL_CSELECT_OUT;
	if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
		socket->events |= CURL_CSELECT_ERR;
	loop_ready_bulk(loop, &socket->step);
}

static void
cli_socket_release (struct loop_ref *ref)
{
	struct cli_socket *socket = to_cli_socket(ref);
	free(socket);
}

static int
cli_socket_fn (CURL          *easy,
               curl_socket_t  fd,
               int            action,
               void          *context,
               void          *assigned)
{
	(void)easy;
	struct cli_network *network = context;
	struct cli_socket *socket = assigned;
	if (action == CURL_POLL_REMOVE) {
		if (socket) {
			(void)curl_multi_assign(network->multi, fd, nullptr);
			list_del(&socket->hook);
			loop_unready(&socket->step);
			loop_release(&network->loop, &socket->ref, cli_socket_release);
		}
		return 0;
	}
	uint32_t events = EPOLLRDHUP;
	if (action == CURL_POLL_IN || action == CURL_POLL_INOUT)
		events |= EPOLLIN;
	if (action == CURL_POLL_OUT || action == CURL_POLL_INOUT)
		events |= EPOLLOUT;
	if (socket)
		return loop_modify(&network->loop, &socket->ref, events) ? -1 : 0;
	socket = calloc(1, sizeof *socket);
	if (!socket)
		return -1;
	socket->fd = fd;
	socket->ref.fd = -1;
	socket->step.fn = cli_socket_step;
	int e = loop_watch(&network->loop, &socket->ref, fd, events,
	                   cli_socket_event, nullptr);
	if (e) {
		free(socket);
		socket = nullptr;
		return -1;
	}
	list_append(&network->sockets, &socket->hook);
	if (curl_multi_assign(network->multi, fd, socket) == CURLM_OK)
		return 0;
	list_del(&socket->hook);
	loop_release(&network->loop, &socket->ref, cli_socket_release);
	return -1;
}

void
cli_network_service (struct cli_network *network)
{
	if (!network || network->delivering || network->ui->dispatching)
		return;
	network->delivering = true;
	cli_network_deliver(network);
	network->delivering = false;
	struct list *node;
	list_foreach (node, &network->calls) {
		struct cli_network_call *call = container_of(node, struct cli_network_call, hook);
		if ((!list_is_empty(&call->received) &&
		     (call->error || !network->ready || network->ready(network->ready_context))) ||
		    ((call->flags & CLI_NETWORK_FINISHED) && list_is_empty(&call->received))) {
			cli_loop_ready(network->ui, &network->delivery);
			break;
		}
	}
	cli_network_kick(network);
}

static void
cli_network_cancel_message (struct loop *loop,
                            struct msg  *message)
{
	struct cli_network *network = container_of(message, struct cli_network, cancel);
	atomic_store(&network->cancel_pending, false);
	struct list *node, *next;
	list_foreach_safe (node, next, &network->requests) {
		struct cli_network_request *request = container_of(node, struct cli_network_request, hook);
		if (atomic_load(&request->cancelled))
			cli_network_finish(request, EINTR, nullptr);
	}
	(void)loop;
}

static void
cli_network_call_cancel (struct cli_network      *network,
                         struct cli_network_call *call,
                         bool                     force)
{
	if ((call->flags & CLI_NETWORK_DONE) ||
	    (!force && !(call->flags & CLI_NETWORK_INTERRUPTIBLE)))
		return;
	if (!call->error)
		call->error = EINTR;
	struct cli_network_request *request = container_of(call, struct cli_network_request, call);
	atomic_store(&request->cancelled, true);
	if (!network->joined && !atomic_exchange(&network->cancel_pending, true))
		(void)loop_post(&network->loop, &network->cancel);
	cli_loop_ready(network->ui, &network->delivery);
}

void
cli_network_cancel_call (struct cli_network *network,
                         uint64_t            id,
                         bool                force)
{
	if (!network)
		return;
	struct list *node;
	list_foreach (node, &network->calls) {
		struct cli_network_call *call = container_of(node, struct cli_network_call, hook);
		if (call->id == id) {
			cli_network_call_cancel(network, call, force);
			break;
		}
	}
}

void
cli_network_cancel (struct cli_network *network,
                    bool                force)
{
	if (!network)
		return;
	if (!force) {
		if (network->waiting)
			cli_network_call_cancel(network, network->waiting, false);
		return;
	}
	struct list *node;
	list_foreach (node, &network->calls)
		cli_network_call_cancel(network, container_of(node, struct cli_network_call, hook), true);
}

static void
cli_network_stop_message (struct loop *loop,
                          struct msg  *message)
{
	(void)message;
	loop_exit(loop);
}

static void *
cli_network_run (void *context)
{
	struct cli_network *network = context;
	int e = loop_exec(&network->loop);
	atomic_store(&network->failure, e ? e : ESHUTDOWN);
	while (!list_is_empty(&network->requests))
		cli_network_finish(container_of(network->requests.next, struct cli_network_request, hook),
		                    e ? e : ECANCELED, nullptr);
	(void)curl_multi_cleanup(network->multi);
	network->multi = nullptr;
	struct list *node, *next;
	list_foreach_safe (node, next, &network->sockets) {
		struct cli_socket *socket = container_of(node, struct cli_socket, hook);
		list_del(&socket->hook);
		loop_unready(&socket->step);
		loop_release(&network->loop, &socket->ref, cli_socket_release);
	}
	(void)loop_timer_stop(&network->loop, &network->timer);
	loop_unready(&network->timeout);
	loop_reap(&network->loop);
	atomic_store(&network->exited, true);
	(void)cli_mailbox_post(network->mailbox, &network->ended, true);
	return nullptr;
}

static void
cli_network_ended (struct cli_mailbox     *mailbox,
                   struct cli_mailbox_msg *message)
{
	(void)mailbox;
	struct cli_network *network = container_of(message, struct cli_network, ended);
	int e = atomic_load(&network->failure);
	if (e != ESHUTDOWN && !network->ui->error)
		network->ui->error = e;
}

void
cli_network_stop (struct cli_network *network)
{
	if (!network || network->stopping)
		return;
	network->stopping = true;
	if (network->started)
		(void)loop_post(&network->loop, &network->stop);
}

bool
cli_network_done (struct cli_network const *network)
{
	return !network || !network->started || atomic_load(&network->exited);
}

static void
cli_network_join (struct cli_network *network)
{
	if (network->started && !network->joined) {
		cli_network_stop(network);
		(void)pthread_join(network->thread, nullptr);
		network->joined = true;
	}
}

struct cli_network *
cli_network_create (struct cli_loop    *ui,
                    struct cli_mailbox *mailbox,
                    int                *e)
{
	struct cli_network *network = calloc(1, sizeof *network);
	*e = ENOMEM;
	if (!network)
		return nullptr;
	loop_place(&network->loop);
	atomic_init(&network->progress_pending, false);
	atomic_init(&network->cancel_pending, false);
	atomic_init(&network->failure, 0);
	atomic_init(&network->exited, false);
	list_init(&network->sockets);
	list_init(&network->requests);
	list_init(&network->completions);
	list_init(&network->calls);
	network->ui = ui;
	network->mailbox = mailbox;
	network->timer = (struct loop_timer){.due = UINT64_MAX, .fn = cli_network_timer_fired};
	network->timeout.fn = cli_network_timeout_step;
	network->delivery.fn = cli_network_delivery;
	network->progress.fn = cli_network_progress_message;
	network->cancel.fn = cli_network_cancel_message;
	network->stop.fn = cli_network_stop_message;
	network->ended = (struct cli_mailbox_msg){.fn = cli_network_ended, .bytes = sizeof network->ended};
	*e = loop_init(&network->loop);
	if (*e)
		goto fail;
	*e = loop_timer_set(&network->loop, &network->timer, UINT64_MAX);
	if (*e)
		goto fail;
	network->multi = curl_multi_init();
	if (!network->multi) {
		*e = ENOMEM;
		goto fail;
	}

#define CLI_NETWORK_OPT(opt, fn) do { \
	if (curl_multi_setopt(network->multi, CURLMOPT_##opt, fn) != CURLM_OK) \
		goto fail_setopt; \
} while (0)

	CLI_NETWORK_OPT(SOCKETFUNCTION, cli_socket_fn);
	CLI_NETWORK_OPT(SOCKETDATA, network);
	CLI_NETWORK_OPT(TIMERFUNCTION, cli_network_timer);
	CLI_NETWORK_OPT(TIMERDATA, network);
	CLI_NETWORK_OPT(MAX_TOTAL_CONNECTIONS, (long)(CLI_NETWORK_CALLS + CLI_NETWORK_CONTROL_CALLS));
	CLI_NETWORK_OPT(MAXCONNECTS, 4L);

#undef CLI_NETWORK_OPT

	*e = pthread_create(&network->thread, nullptr, cli_network_run, network);
	if (*e)
		goto fail;
	network->started = true;
	return network;

fail_setopt:
	*e = EIO;
fail:
	cli_network_destroy(&network);
	return nullptr;
}

void
cli_network_destroy (struct cli_network **ptr)
{
	struct cli_network *network = *ptr;
	if (!network)
		return;
	cli_network_join(network);
	if (network->multi)
		(void)curl_multi_cleanup(network->multi);
	network->multi = nullptr;
	loop_fini(&network->loop);
	(void)cli_mailbox_dispatch(network->mailbox, SIZE_MAX);
	while (!list_is_empty(&network->completions)) {
		struct cli_network_request *request = container_of(network->completions.next,
		                                                   struct cli_network_request, hook);
		list_del(&request->hook);
		cli_network_request_done(network->mailbox, &request->done);
	}
	cli_network_cancel(network, true);
	while (!list_is_empty(&network->calls))
		cli_network_service(network);
	cli_loop_unready(&network->delivery);
	free(network);
	*ptr = nullptr;
}
