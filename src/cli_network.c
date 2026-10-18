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
#include "compat.h"
#include "loop.h"

enum {
	CLI_NETWORK_BYTES = 256 * 1024,
	CLI_NETWORK_MESSAGES = 64,
	CLI_NETWORK_REQUEST = 1,
};

struct cli_network_call {
	cli_network_receive_fn    *header;
	cli_network_receive_fn    *body;
	void                      *context;
	struct cli_network_result *result;
	uint64_t                   id;
	int                        error;
	bool                       interruptible;
	bool                       done;
	bool                       abandoned;
};

struct cli_network_request {
	struct msg                 start;
	struct cli_mailbox_msg      done;
	struct cli_network        *network;
	struct cli_network_call   *call;
	struct curl_slist         *headers;
	CURL                      *easy;
	char                      *socket;
	char                      *wire;
	struct handle              handle;
	struct cli_network_result  result;
	uint64_t                   id;
	size_t                     bytes;
	size_t                     messages;
	int                        error;
	bool                       attached;
	bool                       paused;
};

struct cli_network_chunk {
	struct cli_mailbox_msg    mail;
	struct msg                returned;
	struct cli_network       *network;
	struct cli_network_call  *call;
	struct handle             handle;
	size_t                    length;
	bool                      header;
	char                      bytes[];
};

struct cli_network_socket {
	struct list      hook;
	struct loop_ref  ref;
	struct loop_step step;
	curl_socket_t    fd;
	int              events;
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
	struct list                 received; /* UI thread only. */
	struct cli_loop            *ui;
	struct cli_mailbox         *mailbox;
	struct cli_network_request *request;
	struct cli_network_request *completion;
	struct cli_network_call    *waiting; /* UI thread only. */
	struct cli_network_request *received_done; /* UI thread only. */
	cli_network_ready_fn       *ready;
	void                       *ready_context;
	CURLM                      *multi;
	pthread_t                   thread;
	_Atomic uint64_t            cancel_id;
	uint64_t                    sequence; /* UI thread only. */
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

static void
cli_network_request_destroy (struct cli_network_request **ptr)
{
	struct cli_network_request *request = *ptr;
	if (!request)
		return;
	curl_slist_free_all(request->headers);
	request->headers = nullptr;
	if (request->easy)
		curl_easy_cleanup(request->easy);
	request->easy = nullptr;
	free(request->socket);
	request->socket = nullptr;
	free(request->wire);
	request->wire = nullptr;
	free(request);
	*ptr = nullptr;
}

static void
cli_network_request_done (struct cli_mailbox     *mailbox,
                          struct cli_mailbox_msg *message)
{
	(void)mailbox;
	struct cli_network_request *request = container_of(message, struct cli_network_request, done);
	if (!list_is_empty(&request->network->received)) {
		request->network->received_done = request;
		return;
	}
	struct cli_network_call *call = request->call;
	if (call->result)
		*call->result = request->result;
	if (!call->error)
		call->error = request->error;
	call->done = true;
	if (call->abandoned) {
		free(call);
		call = nullptr;
	}
	cli_network_request_destroy(&request);
}

static void
cli_network_publish (struct cli_network *network)
{
	struct cli_network_request *request = network->completion;
	if (request && !cli_mailbox_post(network->mailbox, &request->done, true))
		network->completion = nullptr;
}

static void
cli_network_finish (struct cli_network *network,
                    int                 error,
                    char const         *detail)
{
	struct cli_network_request *request = network->request;
	if (!request)
		return;
	if (error && !request->error)
		request->error = error;
	if (detail && !request->result.message[0]) {
		size_t length = strlen(detail);
		if (length >= sizeof request->result.message)
			length = sizeof request->result.message - 1;
		memcpy(request->result.message, detail, length);
		request->result.message[length] = '\0';
	}
	if (request->easy && curl_easy_getinfo(request->easy, CURLINFO_RESPONSE_CODE,
	                                     &request->result.status) != CURLE_OK && !request->error)
		request->error = EIO;
	if (request->attached) {
		(void)curl_multi_remove_handle(network->multi, request->easy);
		request->attached = false;
	}
	loop_forget(&network->loop, request->handle);
	curl_slist_free_all(request->headers);
	request->headers = nullptr;
	if (request->easy)
		curl_easy_cleanup(request->easy);
	request->easy = nullptr;
	free(request->socket);
	request->socket = nullptr;
	free(request->wire);
	request->wire = nullptr;
	network->request = nullptr;
	network->completion = request;
	cli_network_publish(network);
}

static void
cli_network_check (struct cli_network *network)
{
	int left;
	CURLMsg *message;
	while ((message = curl_multi_info_read(network->multi, &left)))
		if (message->msg == CURLMSG_DONE && network->request &&
		    message->easy_handle == network->request->easy)
			cli_network_finish(network, message->data.result == CURLE_OK ? 0 : EIO,
			                    message->data.result == CURLE_OK ? nullptr :
			                    curl_easy_strerror(message->data.result));
}

static void
cli_network_action (struct cli_network *network,
                    curl_socket_t       fd,
                    int                 events)
{
	int running;
	CURLMcode code = curl_multi_socket_action(network->multi, fd, events, &running);
	if (code != CURLM_OK)
		cli_network_finish(network, EIO, curl_multi_strerror(code));
	else
		cli_network_check(network);
}

static void
cli_network_chunk_returned (struct loop *loop,
                            struct msg  *message)
{
	struct cli_network_chunk *chunk = container_of(message, struct cli_network_chunk, returned);
	struct cli_network_request *request = loop_lookup(loop, chunk->handle, CLI_NETWORK_REQUEST);
	if (request) {
		request->bytes -= chunk->length;
		--request->messages;
	}
	free(chunk);
	chunk = nullptr;
	if (!loop->exit)
		cli_network_progress(container_of(loop, struct cli_network, loop));
}

static void
cli_network_deliver (struct cli_network *network)
{
	size_t bytes = 0;
	while (!list_is_empty(&network->received)) {
		struct cli_network_chunk *chunk = container_of(network->received.next,
		                                               struct cli_network_chunk, mail.hook);
		struct cli_network_call *call = chunk->call;
		if (!call->error && !network->mailbox->closed) {
			if (bytes >= 64 * 1024 || (network->ready && !network->ready(network->ready_context)))
				return;
			cli_network_receive_fn *receive = chunk->header ? call->header : call->body;
			if (receive && receive(chunk->bytes, 1, chunk->length, call->context) != chunk->length) {
				call->error = EIO;
				cli_network_cancel(network, true);
			}
		}
		bytes += chunk->length;
		list_del(&chunk->mail.hook);
		cli_network_chunk_return(chunk);
	}
	if (network->received_done) {
		struct cli_network_request *request = network->received_done;
		network->received_done = nullptr;
		cli_network_request_done(network->mailbox, &request->done);
	}
}

static void
cli_network_chunk_received (struct cli_mailbox     *mailbox,
                            struct cli_mailbox_msg *message)
{
	(void)mailbox;
	struct cli_network_chunk *chunk = container_of(message, struct cli_network_chunk, mail);
	if (chunk->call->abandoned)
		cli_network_chunk_return(chunk);
	else {
		list_append(&chunk->network->received, &chunk->mail.hook);
		cli_loop_ready(chunk->network->ui, &chunk->network->delivery);
	}
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
	if (request->messages == CLI_NETWORK_MESSAGES ||
	    length > CLI_NETWORK_BYTES - request->bytes) {
		request->paused = true;
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
		.call = request->call,
		.handle = request->handle,
		.length = length,
		.header = header,
	};
	memcpy(chunk->bytes, bytes, length);
	int e = cli_mailbox_post(request->network->mailbox, &chunk->mail, false);
	if (e) {
		free(chunk);
		chunk = nullptr;
		if (e == EAGAIN) {
			request->paused = true;
			return CURL_WRITEFUNC_PAUSE;
		}
		request->error = e;
		return CURL_WRITEFUNC_ERROR;
	}
	request->bytes += length;
	++request->messages;
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

static void
cli_network_socket_step (struct loop      *loop,
                         struct loop_step *step)
{
	struct cli_network_socket *socket = container_of(step, struct cli_network_socket, step);
	int events = socket->events;
	socket->events = 0;
	cli_network_action(container_of(loop, struct cli_network, loop), socket->fd, events);
}

static void
cli_network_socket_event (struct loop     *loop,
                          struct loop_ref *ref,
                          uint32_t         events)
{
	struct cli_network_socket *socket = container_of(ref, struct cli_network_socket, ref);
	if (events & EPOLLIN)
		socket->events |= CURL_CSELECT_IN;
	if (events & EPOLLOUT)
		socket->events |= CURL_CSELECT_OUT;
	if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
		socket->events |= CURL_CSELECT_ERR;
	loop_ready_bulk(loop, &socket->step);
}

static void
cli_network_socket_release (struct loop_ref *ref)
{
	struct cli_network_socket *socket = container_of(ref, struct cli_network_socket, ref);
	free(socket);
	socket = nullptr;
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
	struct cli_network_socket *socket = assigned;
	if (action == CURL_POLL_REMOVE) {
		if (socket) {
			(void)curl_multi_assign(network->multi, fd, nullptr);
			list_del(&socket->hook);
			loop_unready(&socket->step);
			loop_release(&network->loop, &socket->ref, cli_network_socket_release);
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
	socket->step.fn = cli_network_socket_step;
	int e = loop_watch(&network->loop, &socket->ref, fd, events,
	                   cli_network_socket_event, nullptr);
	if (e) {
		free(socket);
		socket = nullptr;
		return -1;
	}
	list_append(&network->sockets, &socket->hook);
	if (curl_multi_assign(network->multi, fd, socket) == CURLM_OK)
		return 0;
	list_del(&socket->hook);
	loop_release(&network->loop, &socket->ref, cli_network_socket_release);
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
	struct cli_network_request *request = network->request;
	if (!request || !request->paused || request->messages == CLI_NETWORK_MESSAGES ||
	    request->bytes == CLI_NETWORK_BYTES)
		return;
	request->paused = false;
	CURLcode code = curl_easy_pause(request->easy, CURLPAUSE_CONT);
	if (code != CURLE_OK)
		cli_network_finish(network, EIO, curl_easy_strerror(code));
	else
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
	if (!network || network->delivering)
		return;
	network->delivering = true;
	cli_network_deliver(network);
	network->delivering = false;
	if (!list_is_empty(&network->received)) {
		struct cli_network_chunk *chunk = container_of(network->received.next,
		                                               struct cli_network_chunk, mail.hook);
		if (chunk->call->error || !network->ready || network->ready(network->ready_context))
			cli_loop_ready(network->ui, &network->delivery);
	}
	cli_network_kick(network);
}

static void
cli_network_cancel_message (struct loop *loop,
                            struct msg  *message)
{
	struct cli_network *network = container_of(message, struct cli_network, cancel);
	atomic_store(&network->cancel_pending, false);
	uint64_t id = atomic_load(&network->cancel_id);
	if (!loop->exit && network->request && network->request->id == id)
		cli_network_finish(network, EINTR, nullptr);
}

void
cli_network_cancel (struct cli_network *network,
                    bool                force)
{
	if (!network || !network->waiting || network->waiting->done || network->joined ||
	    (!force && !network->waiting->interruptible))
		return;
	if (!network->waiting->error)
		network->waiting->error = EINTR;
	atomic_store(&network->cancel_id, network->waiting->id);
	if (!atomic_exchange(&network->cancel_pending, true))
		(void)loop_post(&network->loop, &network->cancel);
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
	cli_network_finish(network, e ? e : ECANCELED, nullptr);
	(void)curl_multi_cleanup(network->multi);
	network->multi = nullptr;
	struct list *node, *next;
	list_foreach_safe (node, next, &network->sockets) {
		struct cli_network_socket *socket = container_of(node, struct cli_network_socket, hook);
		list_del(&socket->hook);
		loop_unready(&socket->step);
		loop_release(&network->loop, &socket->ref, cli_network_socket_release);
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
	atomic_init(&network->cancel_id, 0);
	atomic_init(&network->progress_pending, false);
	atomic_init(&network->cancel_pending, false);
	atomic_init(&network->failure, 0);
	atomic_init(&network->exited, false);
	list_init(&network->sockets);
	list_init(&network->received);
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
	CLI_NETWORK_MULTI(CURLMOPT_MAX_TOTAL_CONNECTIONS, 4L);
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
	cli_network_deliver(network);
	cli_loop_unready(&network->delivery);
	if (network->completion) {
		struct cli_network_request *request = network->completion;
		network->completion = nullptr;
		cli_network_request_done(network->mailbox, &request->done);
	}
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
	return network && network->waiting && !network->waiting->done;
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
	network->request = request;
	if (loop->exit) {
		cli_network_finish(network, ECANCELED, nullptr);
		return;
	}
	int e = loop_handle(loop, request, CLI_NETWORK_REQUEST, &request->handle);
	if (e) {
		cli_network_finish(network, e, nullptr);
		return;
	}
	CURLMcode code = curl_multi_add_handle(network->multi, request->easy);
	if (code != CURLM_OK) {
		cli_network_finish(network, EIO, curl_multi_strerror(code));
		return;
	}
	request->attached = true;
	cli_network_action(network, CURL_SOCKET_TIMEOUT, 0);
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
	if (network->waiting)
		return EBUSY;
	if (network->stopping || network->joined)
		return ESHUTDOWN;
	int stopped = atomic_load(&network->failure);
	if (stopped)
		return stopped;
	if (network->sequence == UINT64_MAX)
		return EOVERFLOW;
	struct cli_network_call *call = malloc(sizeof *call);
	if (!call)
		return ENOMEM;
	*call = (struct cli_network_call){
		.header = header, .body = body, .context = context, .result = result,
		.id = ++network->sequence, .interruptible = interruptible,
	};
	struct cli_network_request *request = calloc(1, sizeof *request);
	if (!request) {
		free(call);
		call = nullptr;
		return ENOMEM;
	}
	request->network = network;
	request->call = call;
	request->id = call->id;
	request->start.fn = cli_network_start;
	request->done.fn = cli_network_request_done;
	request->done.bytes = sizeof *request;
	request->easy = curl_easy_init();
	request->socket = strndup(socket.data, socket.len);
	request->wire = malloc(wire.len ? wire.len : 1);
	int e = request->easy && request->socket && request->wire ? 0 : ENOMEM;
	if (e)
		goto fail;
	memcpy(request->wire, wire.data, wire.len);
	e = cli_network_add_header(request, "Content-Type: application/json");
	if (!e)
		e = cli_network_add_header(request, "Accept: application/json, text/event-stream");
	if (!e)
		e = cli_network_add_header(request, "MCP-Protocol-Version: 2025-11-25");
	if (!e && session.len) {
		static char const prefix[] = "Mcp-Session-Id: ";
		if (session.len > SIZE_MAX - sizeof prefix) {
			e = EOVERFLOW;
			goto fail;
		}
		char *line = malloc(sizeof prefix + session.len);
		if (!line) {
			e = ENOMEM;
			goto fail;
		}
		memcpy(line, prefix, sizeof prefix - 1);
		memcpy(line + sizeof prefix - 1, session.data, session.len);
		line[sizeof prefix - 1 + session.len] = '\0';
		e = cli_network_add_header(request, line);
		free(line);
		line = nullptr;
	}
	if (e)
		goto fail;
#define CLI_NETWORK_OPTION(option, value) \
	do { \
		CURLcode code = curl_easy_setopt(request->easy, option, value); \
		if (code != CURLE_OK) { \
			e = EIO; \
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
#undef CLI_NETWORK_OPTION
	network->waiting = call;
	e = loop_post(&network->loop, &request->start);
	if (e) {
		network->waiting = nullptr;
		goto fail;
	}
	while (!call->done) {
		cli_network_service(network);
		if (call->done)
			break;
		e = cli_loop_turn(network->ui, UINT64_MAX);
		if (e) {
			call->error = e;
			cli_network_cancel(network, true);
			cli_network_deliver(network);
			if (!call->done) {
				call->abandoned = true;
				call->context = nullptr;
				call->result = nullptr;
				network->waiting = nullptr;
				return e;
			}
			break;
		}
		cli_network_kick(network);
	}
	network->waiting = nullptr;
	e = call->error;
	free(call);
	call = nullptr;
	return e;
fail:
	cli_network_request_destroy(&request);
	free(call);
	call = nullptr;
	return e;
}
