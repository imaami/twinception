#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cli_loop.h"
#include "cli_network.h"
#include "compat.h"
#include "curl_loop.h"
#include "dstr.h"
#include "loop.h"
#include "tally.h"
#include "util.h"

/* Calls for control are admitted past the bounds of the other calls, by
 * CLI_NETWORK_CONTROL_CALLS calls and CLI_NETWORK_CONTROL_BYTES bytes.
 */
enum {
	CLI_NETWORK_BYTES = 256 * 1024,
	CLI_NETWORK_DELIVERY = 64 * 1024,
	CLI_NETWORK_CALLS = 16,
	CLI_NETWORK_CONTROL_CALLS = 2,
	CLI_NETWORK_REQUEST_BYTES = 32 * 1024 * 1024,
	CLI_NETWORK_CONTROL_BYTES = 64 * 1024,
};

/* The bounds of the chunks that the network thread has posted to the UI
 * and not got back yet.
 */
static constexpr struct tally cli_network_credit = { 64, CLI_NETWORK_BYTES };

/* The bits of a call's flags, after CLI_NETWORK_INTERRUPTIBLE. */
enum {
	CLI_NETWORK_FINISHED = 1u << 1,
	CLI_NETWORK_DONE = 1u << 2,
	CLI_NETWORK_ABANDONED = 1u << 3,
};

enum {
	CLI_NETWORK_ATTACHED = 1u << 0,
	CLI_NETWORK_PAUSED = 1u << 1,
};

/* The bits of a network's shared word, which both threads use. */
enum {
	CLI_NETWORK_PROGRESS_QUEUED = 1u << 0,  /* progress is posted to the thread */
	CLI_NETWORK_CANCEL_QUEUED   = 1u << 1,  /* cancel is posted to the thread */
	CLI_NETWORK_JOINED          = 1u << 2,  /* the thread has been joined */
};

/* The bits of a network's flags, which only the UI thread uses. */
enum {
	CLI_NETWORK_STOPPING   = 1u << 0,
	CLI_NETWORK_DELIVERING = 1u << 1,
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
	struct list                hook; /* Network-owned requests. */
	struct msg                 start;
	struct msg                 done;
	struct cli_network        *network;
	struct curl_slist         *headers;
	CURL                      *easy;
	struct cli_network_result  result;
	int                        error;
	_Atomic uint16_t           cancelled;
	uint16_t                   flags;
};

struct cli_network_chunk {
	struct list               hook;    /* on call->received */
	struct msg                msg;     /* to the UI, then back */
	struct cli_network       *network;
	struct cli_network_call  *call;
	size_t                    length;
	uint64_t                  header;  /* bool, widened over the padding */
	char                      bytes[];
};

struct cli_network {
	struct loop                 loop;
	struct curl_loop            curl;
	struct msg                  progress;
	struct msg                  cancel;
	struct msg                  stop;
	struct msg                  ended;
	struct list                 requests;
	struct list                 calls; /* UI thread only. */
	struct cli_loop            *ui;
	struct cli_network_call    *waiting; /* UI thread only. */
	cli_network_ready_fn       *ready;
	void                       *ready_context;
	pthread_t                   thread;
	uint64_t                    sequence; /* UI thread only. */
	struct tally                load; /* UI thread only: the calls and the bytes of their requests. */
	struct tally                credit; /* Network thread's: the chunks posted to the UI. */
	_Atomic int                 failure;
	_Atomic uint32_t            posted; /* Unhandled messages to the UI, and 1 for the thread. */
	_Atomic uint32_t            shared;
	uint32_t                    flags;
};

static void cli_network_progress(struct cli_network *network);
static void cli_network_call_cancel(struct cli_network *network, struct cli_network_call *call, bool force);

/* Returns true once the network thread has been joined. */
static bool
cli_network_joined (struct cli_network const *network)
{
	return atomic_load(&network->shared) & CLI_NETWORK_JOINED;
}

/* Post @p message, the progress or the cancel message, to the network
 * thread, unless its bit @p queued is set already in the shared word, or
 * the thread has been joined.
 */
static void
cli_network_queue (struct cli_network *network,
                   struct msg         *message,
                   uint32_t            queued)
{
	if (!(atomic_fetch_or(&network->shared, queued) & (queued | CLI_NETWORK_JOINED)))
		(void)loop_post(&network->loop, message);
}

/* Free the transfer and its headers, but not the request. */
static void
cli_network_request_release (struct cli_network_request *request)
{
	curl_slist_free_all(request->headers);
	request->headers = nullptr;
	curl_easy_cleanup(request->easy);
	request->easy = nullptr;
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
cli_network_request_done (struct loop *loop,
                          struct msg  *message)
{
	(void)loop;
	struct cli_network_request *request = container_of(message, struct cli_network_request, done);
	atomic_fetch_sub(&request->network->posted, 1);
	request->call.flags |= CLI_NETWORK_FINISHED;
	cli_loop_wake(request->network->ui);
}

/* Count @p message as posted to the UI loop, and post it from the network
 * thread, or once that has been joined, handle it at once.
 */
static void
cli_network_post (struct cli_network *network,
                  struct msg         *message)
{
	atomic_fetch_add(&network->posted, 1);
	if (cli_network_joined(network))
		message->fn(&network->ui->loop, message);
	else
		(void)loop_post(&network->ui->loop, message);
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
		(void)curl_multi_remove_handle(network->curl.multi, request->easy);
		request->flags &= ~CLI_NETWORK_ATTACHED;
	}
	cli_network_request_release(request);
	list_del(&request->hook);
	cli_network_post(network, &request->done);
}

/* curl ended @p transfer, a request, with @p result. */
static void
cli_network_curl_done (struct curl_loop *curl,
                       void             *transfer,
                       CURLcode          result)
{
	(void)curl;
	if (result)
		cli_network_finish(transfer, EIO, curl_easy_strerror(result));
	else
		cli_network_finish(transfer, 0, nullptr);
}

/* Fail @p transfer, a request whose socket cannot be watched, with @p e
 * through the cancel message, as curl's socket callback cannot remove it.
 * Without a transfer, curl could not act on an event, and every request
 * fails now with @p e and curl's text of it, @p why.
 */
static void
cli_network_curl_fail (struct curl_loop *curl,
                       void             *transfer,
                       int               e,
                       char const       *why)
{
	struct cli_network *network = container_of(curl, struct cli_network, curl);
	struct cli_network_request *request = transfer;
	if (!request) {
		while (!list_is_empty(&network->requests))
			cli_network_finish(container_of(network->requests.next, struct cli_network_request, hook),
			                    e, why);
		return;
	}
	if (!request->error)
		request->error = e;
	atomic_store(&request->cancelled, true);
	cli_network_queue(network, &network->cancel, CLI_NETWORK_CANCEL_QUEUED);
}

static void
cli_network_chunk_returned (struct loop *loop,
                            struct msg  *message)
{
	struct cli_network_chunk *chunk = container_of(message, struct cli_network_chunk, msg);
	struct cli_network *network = container_of(loop, struct cli_network, loop);
	tally_sub(&network->credit, chunk->length);
	free(chunk);
	chunk = nullptr;
	if (!(loop->flags & LOOP_EXIT))
		cli_network_progress(network);
}

static void
cli_network_chunk_return (struct cli_network_chunk *chunk)
{
	if (cli_network_joined(chunk->network))
		free(chunk);
	else {
		chunk->msg.fn = cli_network_chunk_returned;
		(void)loop_post(&chunk->network->loop, &chunk->msg);
	}
}

/* Pass the chunks in call->received to call->header or call->body, and add
 * their lengths to *delivered. Stop before a chunk if *delivered is at least
 * CLI_NETWORK_DELIVERY or the output is not ready. The chunks of a failed
 * call are returned without a callback.
 */
static void
cli_network_deliver_chunks (struct cli_network      *network,
                            struct cli_network_call *call,
                            size_t                  *delivered)
{
	while (!list_is_empty(&call->received)) {
		struct cli_network_chunk *chunk = container_of(call->received.next,
		                                               struct cli_network_chunk, hook);
		bool live = !call->error;
		if (live && (*delivered >= CLI_NETWORK_DELIVERY ||
		             (network->ready && !network->ready(network->ready_context))))
			return;
		cli_network_receive_fn *receive = chunk->header ? call->header : call->body;
		int e = live && receive ? receive((struct cstr_view){chunk->bytes, chunk->length},
		                                  call->context) : 0;
		if (e) {
			call->error = e;
			cli_network_call_cancel(network, call, true);
		}
		*delivered += chunk->length;
		list_del(&chunk->hook);
		cli_network_chunk_return(chunk);
	}
}

/* Deliver the chunks of the calls in turn, up to CLI_NETWORK_DELIVERY bytes,
 * and finish each call whose request is done and whose chunks are all
 * delivered.
 */
static void
cli_network_deliver (struct cli_network *network)
{
	size_t delivered = 0;
	for (size_t count = network->load.n;
	     count && delivered < CLI_NETWORK_DELIVERY && !list_is_empty(&network->calls); --count) {
		struct cli_network_call *call = container_of(network->calls.next, struct cli_network_call, hook);
		list_move(&network->calls, &call->hook);
		cli_network_deliver_chunks(network, call, &delivered);
		if (!(call->flags & CLI_NETWORK_FINISHED) || !list_is_empty(&call->received))
			continue;
		struct cli_network_request *request = container_of(call, struct cli_network_request, call);
		if (!call->error)
			call->error = request->error;
		if (call->result)
			*call->result = request->result;
		call->flags |= CLI_NETWORK_DONE;
		list_del(&call->hook);
		tally_sub(&network->load, call->bytes);
		if (call->complete)
			call->complete(call->context, call->error, &request->result);
		else if (!(call->flags & CLI_NETWORK_ABANDONED))
			continue;
		cli_network_request_destroy(&request);
	}
}

static void
cli_network_chunk_received (struct loop *loop,
                            struct msg  *message)
{
	(void)loop;
	struct cli_network_chunk *chunk = container_of(message, struct cli_network_chunk, msg);
	atomic_fetch_sub(&chunk->network->posted, 1);
	list_append(&chunk->call->received, &chunk->hook);
	cli_loop_wake(chunk->network->ui);
}

/* Queue the size * count bytes that curl received for the request as a
 * chunk of the header or of the body. Pause the transfer while the budget
 * of the network is used up.
 */
static size_t
cli_network_receive (struct cli_network_request *request,
                     char                       *bytes,
                     size_t                      size,
                     size_t                      count,
                     bool                        header)
{
	if (size && count > CLI_NETWORK_BYTES / size) {
		request->error = EFBIG;
		return CURL_WRITEFUNC_ERROR;
	}
	size_t length = size * count;
	if (!length)
		return 0;
	struct cli_network *network = request->network;
	if (!tally_fits(&network->credit, cli_network_credit, length)) {
		request->flags |= CLI_NETWORK_PAUSED;
		return CURL_WRITEFUNC_PAUSE;
	}
	struct cli_network_chunk *chunk = malloc(sizeof *chunk + length);
	if (!chunk) {
		request->error = ENOMEM;
		return CURL_WRITEFUNC_ERROR;
	}
	*chunk = (struct cli_network_chunk){
		.msg.fn = cli_network_chunk_received,
		.network = network,
		.call = &request->call,
		.length = length,
		.header = header,
	};
	memcpy(chunk->bytes, bytes, length);
	tally_add(&network->credit, length);
	cli_network_post(network, &chunk->msg);
	return length;
}

static size_t
cli_network_body (char   *bytes,
                  size_t  size,
                  size_t  count,
                  void   *context)
{
	return cli_network_receive(context, bytes, size, count, false);
}

static size_t
cli_network_header (char   *bytes,
                    size_t  size,
                    size_t  count,
                    void   *context)
{
	return cli_network_receive(context, bytes, size, count, true);
}

static void
cli_network_progress (struct cli_network *network)
{
	struct list pending = list(&pending);
	list_transplant(&pending, &network->requests);
	while (!list_is_empty(&pending) && tally_fits(&network->credit, cli_network_credit, 1)) {
		struct cli_network_request *request = container_of(pending.next, struct cli_network_request, hook);
		list_move(&network->requests, &request->hook);
		if (!(request->flags & CLI_NETWORK_PAUSED))
			continue;
		request->flags &= ~CLI_NETWORK_PAUSED;
		CURLcode code = curl_easy_pause(request->easy, CURLPAUSE_CONT);
		if (code != CURLE_OK)
			cli_network_finish(request, EIO, curl_easy_strerror(code));
	}
	list_transplant(&pending, &network->requests);
	list_transplant(&network->requests, &pending);
	curl_loop_action(&network->curl, CURL_SOCKET_TIMEOUT, 0);
}

static void
cli_network_progress_message (struct loop *loop,
                              struct msg  *message)
{
	struct cli_network *network = container_of(message, struct cli_network, progress);
	atomic_fetch_and(&network->shared, ~CLI_NETWORK_PROGRESS_QUEUED);
	if (!(loop->flags & LOOP_EXIT))
		cli_network_progress(network);
}

void
cli_network_service (struct cli_network *network)
{
	if (!network || (network->flags & CLI_NETWORK_DELIVERING) || network->ui->dispatching)
		return;
	network->flags |= CLI_NETWORK_DELIVERING;
	cli_network_deliver(network);
	network->flags &= ~CLI_NETWORK_DELIVERING;
	bool ready = !network->ready || network->ready(network->ready_context);
	struct list *node;
	list_foreach (node, &network->calls) {
		struct cli_network_call *call = container_of(node, struct cli_network_call, hook);
		if (list_is_empty(&call->received) ? call->flags & CLI_NETWORK_FINISHED
		                                   : call->error || ready) {
			cli_loop_wake(network->ui);
			break;
		}
	}
	cli_network_queue(network, &network->progress, CLI_NETWORK_PROGRESS_QUEUED);
}

static void
cli_network_cancel_message (struct loop *loop,
                            struct msg  *message)
{
	struct cli_network *network = container_of(message, struct cli_network, cancel);
	atomic_fetch_and(&network->shared, ~CLI_NETWORK_CANCEL_QUEUED);
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
	uint32_t need = force ? 0 : CLI_NETWORK_INTERRUPTIBLE;
	if ((call->flags & (CLI_NETWORK_DONE | need)) != need)
		return;
	if (!call->error)
		call->error = EINTR;
	struct cli_network_request *request = container_of(call, struct cli_network_request, call);
	atomic_store(&request->cancelled, true);
	cli_network_queue(network, &network->cancel, CLI_NETWORK_CANCEL_QUEUED);
	cli_loop_wake(network->ui);
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
	curl_loop_fini(&network->curl);
	/* The thread's own count goes to its last message. */
	(void)loop_post(&network->ui->loop, &network->ended);
	return nullptr;
}

static void
cli_network_ended (struct loop *loop,
                   struct msg  *message)
{
	struct cli_network *network = container_of(message, struct cli_network, ended);
	atomic_fetch_sub(&network->posted, 1);
	int e = atomic_load(&network->failure);
	if (e != ESHUTDOWN)
		loop_fail(loop, e);
}

void
cli_network_stop (struct cli_network *network)
{
	if (!network || (network->flags & CLI_NETWORK_STOPPING))
		return;
	network->flags |= CLI_NETWORK_STOPPING;
	(void)loop_post(&network->loop, &network->stop);
}

bool
cli_network_done (struct cli_network const *network)
{
	return !network || !atomic_load(&network->posted);
}

static void
cli_network_join (struct cli_network *network)
{
	if (cli_network_joined(network))
		return;
	cli_network_stop(network);
	(void)pthread_join(network->thread, nullptr);
	atomic_fetch_or(&network->shared, CLI_NETWORK_JOINED);
}

struct cli_network *
cli_network_create (struct cli_loop *ui,
                    int             *e)
{
	struct cli_network *network = calloc(1, sizeof *network);
	*e = ENOMEM;
	if (!network)
		return nullptr;
	loop_place(&network->loop);
	atomic_init(&network->failure, 0);
	atomic_init(&network->posted, 1);
	atomic_init(&network->shared, 0);
	list_init(&network->requests);
	list_init(&network->calls);
	network->ui = ui;
	network->progress.fn = cli_network_progress_message;
	network->cancel.fn = cli_network_cancel_message;
	network->stop.fn = cli_network_stop_message;
	network->ended.fn = cli_network_ended;
	*e = loop_init(&network->loop);
	if (!*e)
		*e = curl_loop_init(&network->curl, &network->loop, cli_network_curl_done, cli_network_curl_fail);
	if (!*e && (curl_multi_setopt(network->curl.multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
	                              (long)(CLI_NETWORK_CALLS + CLI_NETWORK_CONTROL_CALLS)) != CURLM_OK ||
	            curl_multi_setopt(network->curl.multi, CURLMOPT_MAXCONNECTS, 4L) != CURLM_OK))
		*e = EIO;
	if (!*e)
		*e = pthread_create(&network->thread, nullptr, cli_network_run, network);
	if (!*e)
		return network;
	/* No request or call exists before the thread runs. */
	curl_loop_fini(&network->curl);
	loop_fini(&network->loop);
	free(network);
	return nullptr;
}

void
cli_network_destroy (struct cli_network **ptr)
{
	struct cli_network *network = *ptr;
	if (!network)
		return;
	cli_network_join(network);
	curl_loop_fini(&network->curl);
	loop_fini(&network->loop);
	cli_network_cancel(network, true);
	while (!list_is_empty(&network->calls))
		cli_network_service(network);
	free(network);
	*ptr = nullptr;
}

bool
cli_network_active (struct cli_network const *network)
{
	return network && !list_is_empty(&network->calls);
}

void
cli_network_set_ready (struct cli_network   *network,
                       cli_network_ready_fn *ready,
                       void                 *context)
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
	if (loop->flags & LOOP_EXIT || atomic_load(&request->cancelled)) {
		cli_network_finish(request, ECANCELED, nullptr);
		return;
	}
	CURLMcode code = curl_multi_add_handle(network->curl.multi, request->easy);
	if (code != CURLM_OK) {
		cli_network_finish(request, EIO, curl_multi_strerror(code));
		return;
	}
	request->flags |= CLI_NETWORK_ATTACHED;
	curl_loop_action(&network->curl, CURL_SOCKET_TIMEOUT, 0);
}

static struct cli_network_request *
cli_network_request_create (struct cli_network            *network,
                            struct cli_network_post const *post,
                            cli_network_done_fn           *done,
                            struct cli_network_result     *result,
                            size_t                         bytes,
                            int                           *e)
{
	struct cli_network_request *request = calloc(1, sizeof *request);
	if (!request) {
		*e = ENOMEM;
		return nullptr;
	}
	atomic_init(&request->cancelled, false);
	request->network = network;
	request->call = (struct cli_network_call){
		.received = list(&request->call.received), .header = post->header, .body = post->body,
		.complete = done, .context = post->context, .result = result, .id = ++network->sequence,
		.bytes = bytes, .flags = post->flags & CLI_NETWORK_INTERRUPTIBLE,
	};
	request->start.fn = cli_network_start;
	request->done.fn = cli_network_request_done;
	request->easy = curl_easy_init();
	*e = request->easy ? cli_network_add_header(request, "Content-Type: application/json") : ENOMEM;
	if (!*e)
		*e = cli_network_add_header(request, "Accept: application/json, text/event-stream");
	if (!*e)
		*e = cli_network_add_header(request, "MCP-Protocol-Version: " CLI_VERSION);
	if (!*e && post->session.len) {
		/* Admission bounds the session to far less than INT_MAX bytes. */
		dstr_auto line = dstr(nullptr);
		*e = dstr_printf(&line, "Mcp-Session-Id: %.*s", (int)post->session.len, post->session.data)
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
	/* libcurl copies the socket path and, with COPYPOSTFIELDS, the body. */
	CLI_NETWORK_OPTION(CURLOPT_UNIX_SOCKET_PATH, post->socket.data);
	CLI_NETWORK_OPTION(CURLOPT_PROXY, "");
	CLI_NETWORK_OPTION(CURLOPT_NOPROXY, "*");
	CLI_NETWORK_OPTION(CURLOPT_FOLLOWLOCATION, 0L);
	CLI_NETWORK_OPTION(CURLOPT_NOSIGNAL, 1L);
	CLI_NETWORK_OPTION(CURLOPT_CONNECTTIMEOUT, 10L);
	CLI_NETWORK_OPTION(CURLOPT_TIMEOUT, post->timeout);
	CLI_NETWORK_OPTION(CURLOPT_POST, 1L);
	CLI_NETWORK_OPTION(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)post->wire.len);
	CLI_NETWORK_OPTION(CURLOPT_COPYPOSTFIELDS, post->wire.len ? post->wire.data : "");
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
cli_network_admit (struct cli_network            *network,
                   struct cli_network_post const *post,
                   cli_network_done_fn           *done,
                   struct cli_network_result     *result,
                   struct cli_network_request   **out)
{
	*out = nullptr;
	if (!network || !post || !post->socket.data || !post->socket.len ||
	    (post->session.len && !post->session.data) || (post->wire.len && !post->wire.data))
		return EINVAL;
	/* A network is stopping before it is joined. */
	if (network->flags & CLI_NETWORK_STOPPING)
		return ESHUTDOWN;
	int stopped = atomic_load(&network->failure);
	if (stopped)
		return stopped;
	if (network->sequence == UINT64_MAX)
		return EOVERFLOW;
	bool control = !(post->flags & CLI_NETWORK_INTERRUPTIBLE);
	struct tally const max = {
		CLI_NETWORK_CALLS + control * CLI_NETWORK_CONTROL_CALLS,
		CLI_NETWORK_REQUEST_BYTES + control * CLI_NETWORK_CONTROL_BYTES,
	};
	size_t bytes = sizeof(struct cli_network_request) + 1;
	size_t const lengths[] = {post->socket.len, post->session.len, post->wire.len};
	for (size_t i = 0; i < array_size(lengths); ++i) {
		if (lengths[i] > max.bytes - bytes)
			return EFBIG;
		bytes += lengths[i];
	}
	if (!tally_fits(&network->load, max, bytes))
		return EAGAIN;
	int e;
	struct cli_network_request *request = cli_network_request_create(network, post, done, result, bytes, &e);
	if (!request)
		return e;
	list_append(&network->calls, &request->call.hook);
	tally_add(&network->load, bytes);
	e = loop_post(&network->loop, &request->start);
	if (e) {
		list_del(&request->call.hook);
		tally_sub(&network->load, bytes);
		cli_network_request_destroy(&request);
		return e;
	}
	*out = request;
	return 0;
}

int
cli_network_submit (struct cli_network            *network,
                    struct cli_network_post const *post,
                    cli_network_done_fn           *done,
                    uint64_t                      *id)
{
	if (!done || !id)
		return EINVAL;
	*id = 0;
	struct cli_network_request *request;
	int e = cli_network_admit(network, post, done, nullptr, &request);
	if (!e)
		*id = request->call.id;
	return e;
}

int
cli_network_perform (struct cli_network            *network,
                     struct cli_network_post const *post,
                     struct cli_network_result     *result)
{
	*result = (struct cli_network_result){};
	if (network->waiting || (network->flags & CLI_NETWORK_DELIVERING) || network->ui->dispatching)
		return EBUSY;
	struct cli_network_request *request;
	int e = cli_network_admit(network, post, nullptr, result, &request);
	if (e)
		return e;
	struct cli_network_call *call = &request->call;
	network->waiting = call;
	for (;;) {
		cli_network_service(network);
		if (call->flags & CLI_NETWORK_DONE)
			break;
		e = cli_loop_turn(network->ui, UINT64_MAX);
		if (e)
			break;
	}
	/* After a failed turn, cancel the call. A call that does not end now
	 * is abandoned, and it ends later without a waiter. */
	if (e) {
		call->error = e;
		cli_network_call_cancel(network, call, true);
		cli_network_service(network);
	}
	network->waiting = nullptr;
	if (!(call->flags & CLI_NETWORK_DONE)) {
		call->flags |= CLI_NETWORK_ABANDONED;
		call->context = nullptr;
		call->result = nullptr;
		return e;
	}
	e = call->error;
	cli_network_request_destroy(&request);
	return e;
}
