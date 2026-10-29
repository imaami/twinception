#include <curl/curl.h>
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
#include "dstr.h"
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

struct cli_network_call {
	struct list                hook;
	struct list                received;
	cli_network_receive_fn    *header;
	cli_network_receive_fn    *body;
	cli_network_done_fn       *complete;
	void                      *context;
	struct cli_network_result *result;
	uint64_t                   id;
	size_t                     bytes;
	int                        error;
	uint32_t                   flags;
};

struct cli_network_request {
	struct cli_network_call    call; /* UI-owned; transferred separately from hook. */
	struct list                hook; /* Network-owned requests, then completions. */
	struct msg                 start;
	struct cli_mailbox_msg      done;
	struct cli_network        *network;
	struct curl_slist         *headers;
	CURL                      *easy;
	char                      *socket;
	char                      *wire;
	struct cli_network_result  result;
	int                        error;
	_Atomic uint16_t           cancelled;
	uint16_t                   flags;
};

struct cli_network_chunk {
	struct cli_mailbox_msg    mail;
	struct msg                returned;
	struct cli_network       *network;
	struct cli_network_call  *call;
	size_t                    length;
	bool                      header;
	char                      bytes[];
};

struct cli_network {
	struct loop                 loop;
	struct loop_timer           timer;
	struct loop_step            timeout;
	struct cli_loop_step        delivery;
	struct msg                  progress;
	struct msg                  cancel;
	struct msg                  stop;
	struct cli_mailbox_msg      ended;
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
cli_network_chunk_return (struct cli_network_chunk *chunk)
{
	if (chunk->network->joined) {
		free(chunk);
		chunk = nullptr;
	} else {
		(void)loop_post(&chunk->network->loop, &chunk->returned);
	}
}

/* Free the transfer and the copies of the request, but not the request. */
static void
cli_network_request_release (struct cli_network_request *request)
{
	curl_slist_free_all(request->headers);
	request->headers = nullptr;
	curl_easy_cleanup(request->easy);
	request->easy = nullptr;
	free(request->socket);
	request->socket = nullptr;
	free(request->wire);
	request->wire = nullptr;
}

static void
cli_network_request_destroy (struct cli_network_request **ptr)
{
	struct cli_network_request *request = *ptr;
	if (!request)
		return;
	cli_network_request_release(request);
	free(request);
	*ptr = nullptr;
}

static void
cli_network_request_done (struct cli_mailbox     *mailbox,
                          struct cli_mailbox_msg *message)
{
	(void)mailbox;
	struct cli_network_request *request = container_of(message, struct cli_network_request, done);
	request->call.flags |= CLI_NETWORK_FINISHED;
	cli_loop_ready(request->network->ui, &request->network->delivery);
}

static void
cli_network_publish (struct cli_network *network)
{
	while (!list_is_empty(&network->completions)) {
		struct cli_network_request *request = container_of(network->completions.next,
		                                                   struct cli_network_request, hook);
		list_del(&request->hook);
		if (cli_mailbox_post(network->mailbox, &request->done, true)) {
			list_append(&network->completions, &request->hook);
			break;
		}
	}
}

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
cli_network_action (struct cli_network *network,
                    curl_socket_t       fd,
                    int                 events)
{
	int running;
	CURLMcode code = curl_multi_socket_action(network->multi, fd, events, &running);
	if (code != CURLM_OK) {
		while (!list_is_empty(&network->requests))
			cli_network_finish(container_of(network->requests.next, struct cli_network_request, hook),
			                    EIO, curl_multi_strerror(code));
	} else {
		cli_network_check(network);
	}
}

static void
cli_network_chunk_returned (struct loop *loop,
                            struct msg  *message)
{
	struct cli_network_chunk *chunk = container_of(message, struct cli_network_chunk, returned);
	struct cli_network *network = container_of(loop, struct cli_network, loop);
	network->bytes -= chunk->length;
	--network->messages;
	free(chunk);
	chunk = nullptr;
	if (!loop->exit)
		cli_network_progress(network);
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

static size_t
cli_network_receive (struct cli_network_request *request,
                     char                       *bytes,
                     size_t                      length,
                     bool                        header)
{
	if (!length)
		return 0;
	if (length > CLI_NETWORK_BYTES) {
		request->error = EFBIG;
		return CURL_WRITEFUNC_ERROR;
	}
	struct cli_network *network = request->network;
	if (network->messages == CLI_NETWORK_MESSAGES ||
	    length > CLI_NETWORK_BYTES - network->bytes) {
		request->flags |= CLI_NETWORK_PAUSED;
		return CURL_WRITEFUNC_PAUSE;
	}
	struct cli_network_chunk *chunk = malloc(sizeof *chunk + length);
	if (!chunk) {
		request->error = ENOMEM;
		return CURL_WRITEFUNC_ERROR;
	}
	*chunk = (struct cli_network_chunk){
		.mail = {.fn = cli_network_chunk_received, .bytes = sizeof *chunk + length},
		.returned.fn = cli_network_chunk_returned,
		.network = request->network,
		.call = &request->call,
		.length = length,
		.header = header,
	};
	memcpy(chunk->bytes, bytes, length);
	int e = cli_mailbox_post(request->network->mailbox, &chunk->mail, false);
	if (e) {
		free(chunk);
		chunk = nullptr;
		if (e == EAGAIN) {
			request->flags |= CLI_NETWORK_PAUSED;
			return CURL_WRITEFUNC_PAUSE;
		}
		request->error = e;
		return CURL_WRITEFUNC_ERROR;
	}
	network->bytes += length;
	++network->messages;
	return length;
}

static size_t
cli_network_body (char   *bytes,
                  size_t  size,
                  size_t  count,
                  void   *context)
{
	struct cli_network_request *request = context;
	if (size && count > SIZE_MAX / size) {
		request->error = EOVERFLOW;
		return CURL_WRITEFUNC_ERROR;
	}
	return cli_network_receive(request, bytes, size * count, false);
}

static size_t
cli_network_header (char   *bytes,
                    size_t  size,
                    size_t  count,
                    void   *context)
{
	struct cli_network_request *request = context;
	if (size && count > SIZE_MAX / size) {
		request->error = EOVERFLOW;
		return CURL_WRITEFUNC_ERROR;
	}
	return cli_network_receive(request, bytes, size * count, true);
}

static int
cli_network_socket (CURL          *easy,
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

static void
cli_network_timeout_step (struct loop      *loop,
                          struct loop_step *step)
{
	(void)step;
	cli_network_action(container_of(loop, struct cli_network, loop), CURL_SOCKET_TIMEOUT, 0);
}

static void
cli_network_timer_fired (struct loop       *loop,
                         struct loop_timer *timer)
{
	struct cli_network *network = container_of(timer, struct cli_network, timer);
	loop_ready_bulk(loop, &network->timeout);
}

static int
cli_network_timer (CURLM *multi,
                   long   milliseconds,
                   void  *context)
{
	(void)multi;
	struct cli_network *network = context;
	uint64_t due = UINT64_MAX;
	if (milliseconds >= 0) {
		uint64_t now = loop_clock();
		uint64_t limit = (UINT64_MAX - now) / 1'000'000;
		if ((unsigned long)milliseconds < limit)
			due = now + (uint64_t)milliseconds * 1'000'000;
	}
	return loop_timer_set(&network->loop, &network->timer, due) ? -1 : 0;
}

static void
cli_network_progress (struct cli_network *network)
{
	cli_network_publish(network);
	struct list pending;
	list_init(&pending);
	list_transplant(&pending, &network->requests);
	while (!list_is_empty(&pending) && network->messages < CLI_NETWORK_MESSAGES &&
	       network->bytes < CLI_NETWORK_BYTES) {
		struct cli_network_request *request = container_of(pending.next, struct cli_network_request, hook);
		list_del(&request->hook);
		list_append(&network->requests, &request->hook);
		if (!(request->flags & CLI_NETWORK_PAUSED))
			continue;
		request->flags &= ~CLI_NETWORK_PAUSED;
		CURLcode code = curl_easy_pause(request->easy, CURLPAUSE_CONT);
		if (code != CURLE_OK)
			cli_network_finish(request, EIO, curl_easy_strerror(code));
	}
	list_transplant(&pending, &network->requests);
	list_transplant(&network->requests, &pending);
	cli_network_check(network);
}

static void
cli_network_progress_message (struct loop *loop,
                              struct msg  *message)
{
	struct cli_network *network = container_of(message, struct cli_network, progress);
	atomic_store(&network->progress_pending, false);
	if (!loop->exit)
		cli_network_progress(network);
}

static void
cli_network_kick (struct cli_network *network)
{
	if (!network->joined && !atomic_exchange(&network->progress_pending, true))
		(void)loop_post(&network->loop, &network->progress);
}

static void
cli_network_delivery (struct cli_loop      *loop,
                      struct cli_loop_step *step)
{
	(void)loop;
	(void)step;
	/* Keep a turn runnable; only after_turn may enter protocol callbacks. */
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
		struct cli_socket *socket = to_cli_socket(node);
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
#define CLI_NETWORK_MULTI(option, value) \
        do { \
                if (curl_multi_setopt(network->multi, option, value) != CURLM_OK) { \
                        *e = EIO; \
                        goto fail; \
                } \
        } while (0)
	CLI_NETWORK_MULTI(CURLMOPT_SOCKETFUNCTION, cli_network_socket);
	CLI_NETWORK_MULTI(CURLMOPT_SOCKETDATA, network);
	CLI_NETWORK_MULTI(CURLMOPT_TIMERFUNCTION, cli_network_timer);
	CLI_NETWORK_MULTI(CURLMOPT_TIMERDATA, network);
	CLI_NETWORK_MULTI(CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)(CLI_NETWORK_CALLS + CLI_NETWORK_CONTROL_CALLS));
	CLI_NETWORK_MULTI(CURLMOPT_MAXCONNECTS, 4L);
#undef CLI_NETWORK_MULTI
	*e = pthread_create(&network->thread, nullptr, cli_network_run, network);
	if (*e)
		goto fail;
	network->started = true;
	return network;
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

struct loop *
cli_network_loop (struct cli_network *network)
{
	return &network->loop;
}

bool
cli_network_active (struct cli_network const *network)
{
	return network && !list_is_empty(&network->calls);
}

void
cli_network_set_ready (struct cli_network  *network,
                       cli_network_ready_fn *ready,
                       void                *context)
{
	network->ready = ready;
	network->ready_context = context;
}

static int
cli_network_add_header (struct cli_network_request *request,
                        char const                 *text)
{
	struct curl_slist *headers = curl_slist_append(request->headers, text);
	if (!headers)
		return ENOMEM;
	request->headers = headers;
	return 0;
}

static void
cli_network_start (struct loop *loop,
                   struct msg  *message)
{
	struct cli_network_request *request = container_of(message, struct cli_network_request, start);
	struct cli_network *network = container_of(loop, struct cli_network, loop);
	list_append(&network->requests, &request->hook);
	if (loop->exit || atomic_load(&request->cancelled)) {
		cli_network_finish(request, ECANCELED, nullptr);
		return;
	}
	CURLMcode code = curl_multi_add_handle(network->multi, request->easy);
	if (code != CURLM_OK) {
		cli_network_finish(request, EIO, curl_multi_strerror(code));
		return;
	}
	request->flags |= CLI_NETWORK_ATTACHED;
	cli_network_action(network, CURL_SOCKET_TIMEOUT, 0);
}

static struct cli_network_request *
cli_network_request_create (struct cli_network        *network,
                            struct cstr_view           socket,
                            struct cstr_view           session,
                            struct cstr_view           wire,
                            long                       timeout,
                            bool                       interruptible,
                            cli_network_receive_fn    *header,
                            cli_network_receive_fn    *body,
                            void                      *context,
                            cli_network_done_fn       *done,
                            struct cli_network_result *result,
                            size_t                     bytes,
                            int                       *e)
{
	struct cli_network_request *request = calloc(1, sizeof *request);
	*e = request ? 0 : ENOMEM;
	if (!request)
		return nullptr;
	atomic_init(&request->cancelled, false);
	request->network = network;
	request->call = (struct cli_network_call){
		.header = header, .body = body, .complete = done, .context = context, .result = result,
		.id = ++network->sequence, .bytes = bytes,
		.flags = interruptible ? CLI_NETWORK_INTERRUPTIBLE : 0,
	};
	list_init(&request->call.received);
	request->start.fn = cli_network_start;
	request->done.fn = cli_network_request_done;
	request->done.bytes = sizeof *request;
	request->easy = curl_easy_init();
	request->socket = strndup(socket.data, socket.len);
	request->wire = malloc(wire.len ? wire.len : 1);
	*e = request->easy && request->socket && request->wire ? 0 : ENOMEM;
	if (*e)
		goto fail;
	if (wire.len)
		memcpy(request->wire, wire.data, wire.len);
	*e = cli_network_add_header(request, "Content-Type: application/json");
	if (!*e)
		*e = cli_network_add_header(request, "Accept: application/json, text/event-stream");
	if (!*e)
		*e = cli_network_add_header(request, "MCP-Protocol-Version: " CLI_VERSION);
	if (!*e && session.len) {
		/* Admission bounds the session to far less than INT_MAX bytes. */
		dstr_auto line = dstr(nullptr);
		*e = dstr_printf(&line, "Mcp-Session-Id: %.*s", (int)session.len, session.data)
		   ? cli_network_add_header(request, dstr_get(&line)) : ENOMEM;
	}
	if (*e)
		goto fail;
#define CLI_NETWORK_OPTION(option, value) \
        do { \
                CURLcode code = curl_easy_setopt(request->easy, option, value); \
                if (code != CURLE_OK) { \
                        *e = EIO; \
                        goto fail; \
                } \
        } while (0)
	CLI_NETWORK_OPTION(CURLOPT_URL, "http://localhost/mcp");
	CLI_NETWORK_OPTION(CURLOPT_UNIX_SOCKET_PATH, request->socket);
	CLI_NETWORK_OPTION(CURLOPT_PROXY, "");
	CLI_NETWORK_OPTION(CURLOPT_NOPROXY, "*");
	CLI_NETWORK_OPTION(CURLOPT_FOLLOWLOCATION, 0L);
	CLI_NETWORK_OPTION(CURLOPT_NOSIGNAL, 1L);
	CLI_NETWORK_OPTION(CURLOPT_CONNECTTIMEOUT, 10L);
	CLI_NETWORK_OPTION(CURLOPT_TIMEOUT, timeout);
	CLI_NETWORK_OPTION(CURLOPT_POST, 1L);
	CLI_NETWORK_OPTION(CURLOPT_POSTFIELDS, request->wire);
	CLI_NETWORK_OPTION(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)wire.len);
	CLI_NETWORK_OPTION(CURLOPT_HTTPHEADER, request->headers);
	CLI_NETWORK_OPTION(CURLOPT_WRITEFUNCTION, cli_network_body);
	CLI_NETWORK_OPTION(CURLOPT_WRITEDATA, request);
	CLI_NETWORK_OPTION(CURLOPT_HEADERFUNCTION, cli_network_header);
	CLI_NETWORK_OPTION(CURLOPT_HEADERDATA, request);
	CLI_NETWORK_OPTION(CURLOPT_ERRORBUFFER, request->result.message);
	CLI_NETWORK_OPTION(CURLOPT_PRIVATE, request);
#undef CLI_NETWORK_OPTION
	return request;
fail:
	cli_network_request_destroy(&request);
	return nullptr;
}

static int
cli_network_admit (struct cli_network         *network,
                   struct cstr_view            socket,
                   struct cstr_view            session,
                   struct cstr_view            wire,
                   long                        timeout,
                   bool                        interruptible,
                   cli_network_receive_fn     *header,
                   cli_network_receive_fn     *body,
                   void                       *context,
                   cli_network_done_fn        *done,
                   struct cli_network_result  *result,
                   struct cli_network_request **out)
{
	*out = nullptr;
	if (!network || !socket.data || !socket.len || (session.len && !session.data) ||
	    (wire.len && !wire.data))
		return EINVAL;
	if (network->stopping || network->joined)
		return ESHUTDOWN;
	int stopped = atomic_load(&network->failure);
	if (stopped)
		return stopped;
	if (network->sequence == UINT64_MAX)
		return EOVERFLOW;
	size_t limit = CLI_NETWORK_REQUEST_BYTES + (interruptible ? 0 : 64 * 1024);
	if (network->call_count >= CLI_NETWORK_CALLS + (interruptible ? 0 : CLI_NETWORK_CONTROL_CALLS))
		return EAGAIN;
	size_t bytes = sizeof(struct cli_network_request) + 1;
	size_t const lengths[] = {socket.len, session.len, wire.len};
	for (size_t i = 0; i < array_size(lengths); ++i) {
		if (lengths[i] > limit - bytes)
			return EFBIG;
		bytes += lengths[i];
	}
	if (network->request_bytes > limit || bytes > limit - network->request_bytes)
		return EAGAIN;
	int e;
	struct cli_network_request *request = cli_network_request_create(network, socket, session, wire,
	                                       timeout, interruptible, header, body, context, done, result, bytes, &e);
	if (!request)
		return e;
	list_append(&network->calls, &request->call.hook);
	++network->call_count;
	network->request_bytes += bytes;
	e = loop_post(&network->loop, &request->start);
	if (e) {
		list_del(&request->call.hook);
		--network->call_count;
		network->request_bytes -= bytes;
		cli_network_request_destroy(&request);
		return e;
	}
	*out = request;
	return 0;
}

int
cli_network_submit (struct cli_network     *network,
                    struct cstr_view        socket,
                    struct cstr_view        session,
                    struct cstr_view        wire,
                    long                    timeout,
                    bool                    interruptible,
                    cli_network_receive_fn *header,
                    cli_network_receive_fn *body,
                    void                   *context,
                    cli_network_done_fn    *done,
                    uint64_t               *id)
{
	if (!done || !id)
		return EINVAL;
	*id = 0;
	struct cli_network_request *request;
	int e = cli_network_admit(network, socket, session, wire, timeout, interruptible,
	                          header, body, context, done, nullptr, &request);
	if (!e)
		*id = request->call.id;
	return e;
}

int
cli_network_perform (struct cli_network        *network,
                     struct cstr_view           socket,
                     struct cstr_view           session,
                     struct cstr_view           wire,
                     long                       timeout,
                     bool                       interruptible,
                     cli_network_receive_fn    *header,
                     cli_network_receive_fn    *body,
                     void                      *context,
                     struct cli_network_result *result)
{
	*result = (struct cli_network_result){};
	if (network->waiting || network->delivering || network->ui->dispatching)
		return EBUSY;
	struct cli_network_request *request;
	int e = cli_network_admit(network, socket, session, wire, timeout, interruptible,
	                          header, body, context, nullptr, result, &request);
	if (e)
		return e;
	struct cli_network_call *call = &request->call;
	network->waiting = call;
	while (!(call->flags & CLI_NETWORK_DONE)) {
		cli_network_service(network);
		if (call->flags & CLI_NETWORK_DONE)
			break;
		e = cli_loop_turn(network->ui, UINT64_MAX);
		if (e) {
			call->error = e;
			cli_network_call_cancel(network, call, true);
			cli_network_service(network);
			if (!(call->flags & CLI_NETWORK_DONE)) {
				call->flags |= CLI_NETWORK_ABANDONED;
				call->context = nullptr;
				call->result = nullptr;
				network->waiting = nullptr;
				return e;
			}
			break;
		}
	}
	network->waiting = nullptr;
	e = call->error;
	cli_network_request_destroy(&request);
	return e;
}
