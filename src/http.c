/**
 * @file http.c
 * libcurl multi-socket integration for the event loop.
 *
 * Socket events and curl's timer, which lives on the loop's heap, only queue
 * bulk steps, and the steps drive curl.  An error fails at most the transfer
 * it belongs to, never the loop or the other transfers.  A done callback may
 * ask what Retry-After its answer said.
 *
 * With a resolver set, host names are looked up on a blocker thread rather
 * than by libcurl, whose resolver would start a thread of its own for each
 * lookup.  A request to a host waits for its addresses, while one lookup per
 * host is under way; the addresses are kept for LOOKUP_TTL_NS, and a lookup
 * that fails leaves those it had in use.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "blocker.h"
#include "http.h"
#include "list.h"
#include "log.h"
#include "util.h"

struct http_socket {
	struct list      hook;
	struct loop_ref  ref;
	struct loop_step step;   /* reports @p what to curl */
	struct http     *http;
	curl_socket_t    socket;
	int              what;   /* CURL_CSELECT_* bits seen since the step */
};

/* Requests handed to curl per step: each one connects, so a burst of them
 * would make one long step. */
enum { START_BATCH = 2 };

/* How long looked-up addresses serve new requests. */
static constexpr uint64_t LOOKUP_TTL_NS = 60'000'000'000;

struct http_request {
	struct list        hook;     /* on a host's waiting list, then on
	                                http->queued, then http->requests */
	struct curl_slist *headers;
	struct curl_slist *resolve;  /* the host's addresses, for CURLOPT_RESOLVE */
	http_write_cb     *write_cb;
	http_done_cb      *done_cb;
	void              *user;
	CURL              *easy;
	uint64_t           since;    /* when it was made */
	uint64_t           started;  /* when curl got it; 0 until then */
	size_t             len;      /* the body's bytes */
	CURLcode           fail;     /* to fail it with, from http->fail */
	char               error[CURL_ERROR_SIZE];
};

struct http {
	struct loop_timer    timer;
	struct loop_step     timeout;   /* tells curl its timer expired */
	struct loop_step     start;     /* hands queued requests to curl */
	struct loop_step     fail;      /* fails requests that curl cannot run */
	struct list          queued;    /* requests not yet handed to curl */
	struct list          requests;
	struct list          sockets;
	struct list          hosts;     /* looked up, or being looked up */
	struct loop         *loop;
	struct blockers     *blockers;  /* that look host names up, or nullptr */
	struct http_metrics *metrics;
	CURLM               *multi;
	long                 retry;     /* the Retry-After of the request that
	                                   ended last, in seconds */
};

/* An upstream host by name and port, and its addresses. */
struct http_host {
	struct list         hook;     /* on http->hosts */
	struct list         waiting;  /* requests that wait for its addresses */
	struct http        *http;
	struct http_lookup *lookup;   /* under way, or nullptr */
	char               *entry;    /* "name:port:address,...", or nullptr */
	uint64_t            expires;  /* when entry stops serving new requests */
	size_t              name;     /* the name's length */
	size_t              len;      /* the key's, its NULs included */
	char                key[];    /* the name, then the port, each ending in NUL */
};

/* A lookup on a blocker, which reads only what the lookup holds. */
struct http_lookup {
	struct blocker_job  job;
	struct http_host   *host;     /* nullptr once http has gone */
	char               *entry;    /* as in the host, or nullptr */
	size_t              name;     /* as in the host */
	size_t              len;
	int                 gai;      /* else getaddrinfo()'s error */
	char                key[];    /* the host's, copied */
};

/* Take @p request out of curl and free it, then report @p result. */
static void
http_finish (struct http         *http,
             struct http_request *request,
             CURLcode             result)
{
	long status = 0;
	curl_off_t retry = 0;
	curl_easy_getinfo(request->easy, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_getinfo(request->easy, CURLINFO_RETRY_AFTER, &retry);
	http->retry = retry > 0 && retry < LONG_MAX ? (long)retry : 0;
	curl_multi_remove_handle(http->multi, request->easy);
	curl_easy_cleanup(request->easy);
	list_del(&request->hook);
	if (request->started && http->metrics)
		hist_record(&http->metrics->run, loop_clock() - request->started);

	http_done_cb *done = request->done_cb;
	void *user = request->user;
	char error[CURL_ERROR_SIZE];
	snprintf(error, sizeof error, "%s", request->error);

	curl_slist_free_all(request->resolve);
	curl_slist_free_all(request->headers);
	free(request);
	done(result, status, error, user);
}

static void
http_check_done (struct http *http)
{
	int left;
	CURLMsg *msg;
	while ((msg = curl_multi_info_read(http->multi, &left))) {
		struct http_request *request = nullptr;
		if (msg->msg == CURLMSG_DONE &&
		    !curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &request) &&
		    request)
			http_finish(http, request, msg->data.result);
	}
}

/* Have the request of @p easy fail with @p e on a later step.  The socket
 * callback cannot remove it, and an error returned there would fail every
 * transfer. */
static void
http_doom (struct http *http,
           CURL        *easy,
           int          e)
{
	struct http_request *request = nullptr;
	curl_easy_getinfo(easy, CURLINFO_PRIVATE, &request);
	if (request && !request->fail) {
		request->fail = e == ENOMEM ? CURLE_OUT_OF_MEMORY : CURLE_COULDNT_CONNECT;
		snprintf(request->error, sizeof request->error,
		         "cannot watch the socket: %s", strerror(e));
		loop_ready_bulk(http->loop, &http->fail);
	}
}

/* Fail one doomed request a step, as it may start or cancel others. */
static void
http_fail_step (struct loop      *loop,
                struct loop_step *step)
{
	struct http *http = container_of(step, struct http, fail);
	struct list *node;
	list_foreach (node, &http->requests) {
		struct http_request *request = container_of(node, struct http_request,
		                                           hook);
		if (request->fail) {
			loop_ready_bulk(loop, step);
			http_finish(http, request, request->fail);
			return;
		}
	}
}

/* Transfers that curl failed end through http_check_done() like any other. */
static void
http_action (struct http  *http,
             curl_socket_t socket,
             int           what)
{
	int running;
	CURLMcode mc = curl_multi_socket_action(http->multi, socket, what, &running);
	if (mc != CURLM_OK)
		LOG(LOG_ERROR, "curl: %s", curl_multi_strerror(mc));
	http_check_done(http);
}

/* Hand curl up to START_BATCH queued requests, and have it start them. */
static void
http_start_step (struct loop      *loop,
                 struct loop_step *step)
{
	struct http *http = container_of(step, struct http, start);
	for (unsigned n = 0; n < START_BATCH && !list_is_empty(&http->queued); ++n) {
		struct http_request *request = container_of(http->queued.next,
		                                           struct http_request, hook);
		list_del(&request->hook);
		list_append(&http->requests, &request->hook);
		request->started = loop_clock();
		if (http->metrics)
			hist_record(&http->metrics->wait, request->started - request->since);
		if (curl_multi_add_handle(http->multi, request->easy) != CURLM_OK) {
			request->started = 0;
			request->fail = CURLE_FAILED_INIT;
			loop_ready_bulk(loop, &http->fail);
		}
	}
	if (!list_is_empty(&http->queued))
		loop_ready_bulk(loop, step);
	http_action(http, CURL_SOCKET_TIMEOUT, 0);
}

static void
http_socket_event (struct loop     *loop,
                   struct loop_ref *ref,
                   uint32_t         events)
{
	struct http_socket *hs = ref->owner;
	if (events & EPOLLIN)
		hs->what |= CURL_CSELECT_IN;
	if (events & EPOLLOUT)
		hs->what |= CURL_CSELECT_OUT;
	if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
		hs->what |= CURL_CSELECT_ERR;
	loop_ready_bulk(loop, &hs->step);
}

static void
http_socket_step (struct loop      *loop,
                  struct loop_step *step)
{
	(void)loop;
	struct http_socket *hs = container_of(step, struct http_socket, step);
	int what = hs->what;
	hs->what = 0;
	http_action(hs->http, hs->socket, what);
}

static void
http_timer_event (struct loop       *loop,
                  struct loop_timer *timer)
{
	loop_ready_bulk(loop, &container_of(timer, struct http, timer)->timeout);
}

static void
http_timeout_step (struct loop      *loop,
                   struct loop_step *step)
{
	(void)loop;
	http_action(container_of(step, struct http, timeout), CURL_SOCKET_TIMEOUT, 0);
}

static void
http_socket_release (struct loop_ref *ref)
{
	free(container_of(ref, struct http_socket, ref));
}

static uint32_t
curl_events (int what)
{
	switch (what) {
	case CURL_POLL_IN:
		return EPOLLIN | EPOLLRDHUP;
	case CURL_POLL_OUT:
		return EPOLLOUT | EPOLLRDHUP;
	case CURL_POLL_INOUT:
		return EPOLLIN | EPOLLOUT | EPOLLRDHUP;
	default:
		return 0;
	}
}

static int
http_socket_cb (CURL          *easy,
                curl_socket_t  socket,
                int            what,
                void          *clientp,
                void          *socketp)
{
	struct http *http = clientp;
	struct http_socket *hs = socketp;

	if (what == CURL_POLL_REMOVE) {
		if (hs) {
			curl_multi_assign(http->multi, socket, nullptr);
			list_del(&hs->hook);
			loop_unready(&hs->step);
			loop_release(http->loop, &hs->ref, http_socket_release);
		}
		return 0;
	}

	uint32_t events = curl_events(what);
	int e;
	if (!events || hs) {
		e = events ? loop_modify(http->loop, &hs->ref, events) : 0;
	} else if (!(hs = malloc(sizeof *hs))) {
		e = ENOMEM;
	} else {
		*hs = (struct http_socket){
			.step.fn = http_socket_step,
			.http = http,
			.socket = socket
		};
		e = loop_watch(http->loop, &hs->ref, socket, events, http_socket_event,
		               hs);
		if (e) {
			free(hs);
		} else {
			list_append(&http->sockets, &hs->hook);
			(void)curl_multi_assign(http->multi, socket, hs);
		}
	}
	if (e)
		http_doom(http, easy, e);
	return 0;
}

/* The timer is in the heap from the start, so setting it cannot fail. */
static int
http_timer_cb (CURLM *multi,
               long   timeout_ms,
               void  *user)
{
	(void)multi;
	struct http *http = user;
	(void)loop_timer_set(http->loop, &http->timer, timeout_ms < 0 ? UINT64_MAX
	                     : loop_clock() + (uint64_t)timeout_ms * 1'000'000);
	return 0;
}

static size_t
http_write_adapter (char   *ptr,
                    size_t  size,
                    size_t  nmemb,
                    void   *user)
{
	struct http_request *request = user;
	if (size && nmemb > SIZE_MAX / size)
		return 0;
	return request->write_cb(ptr, size * nmemb, request->user);
}

/* Give the transfer of @p request the addresses in @p entry. */
static int
http_pin (struct http_request *request,
          char const          *entry)
{
	request->resolve = curl_slist_append(nullptr, entry);
	return !request->resolve ? ENOMEM
	     : curl_easy_setopt(request->easy, CURLOPT_RESOLVE, request->resolve)
	     ? EIO : 0;
}

/* On a blocker: look the host up, and write its addresses as libcurl takes
 * them, IPv6 ones in brackets. */
static void
http_look_up (struct blocker_job *job)
{
	struct http_lookup *l = container_of(job, struct http_lookup, job);
	char const *port = l->key + l->name + 1;
	struct addrinfo const hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	struct addrinfo *list;
	if ((l->gai = getaddrinfo(l->key, port, &hints, &list)))
		return;

	size_t n = 0;
	for (struct addrinfo const *ai = list; ai; ai = ai->ai_next)
		++n;
	/* "name:port:" and, for each address, "[address]," at most. */
	size_t cap = l->len + 1 + n * (INET6_ADDRSTRLEN + 3);
	char *entry = malloc(cap);
	size_t len = entry ? (size_t)snprintf(entry, cap, "%s:%s:", l->key, port) : 0;
	for (struct addrinfo const *ai = list; entry && ai; ai = ai->ai_next) {
		bool v6 = ai->ai_family == AF_INET6;
		void const *addr = v6
		        ? (void const *)&((struct sockaddr_in6 const *)(void const *)
		                          ai->ai_addr)->sin6_addr
		        : (void const *)&((struct sockaddr_in const *)(void const *)
		                          ai->ai_addr)->sin_addr;
		char text[INET6_ADDRSTRLEN];
		if ((v6 || ai->ai_family == AF_INET) &&
		    inet_ntop(ai->ai_family, addr, text, sizeof text))
			len += (size_t)snprintf(entry + len, cap - len, "%s%s%s%s",
			                        entry[len - 1] == ':' ? "" : ",",
			                        v6 ? "[" : "", text, v6 ? "]" : "");
	}
	freeaddrinfo(list);
	if (entry && entry[len - 1] != ':') {
		l->entry = entry;
	} else {
		free(entry);
		l->gai = entry ? EAI_NONAME : EAI_MEMORY;
	}
}

/* On the loop: start the requests that waited for the host's addresses, or
 * fail them.  A lookup that fails leaves the addresses the host had. */
static void
http_looked_up (struct loop *loop,
                struct msg  *msg)
{
	struct http_lookup *l = container_of(msg, struct http_lookup, job.done);
	struct http_host *host = l->host;
	if (host) {
		struct http *http = host->http;
		char const *why = l->job.error ? strerror(l->job.error)
		                : l->entry ? "" : gai_strerror(l->gai);
		host->lookup = nullptr;
		if (http->metrics)
			hist_add(&http->metrics->lookups[l->entry != nullptr], 1);
		if (l->entry) {
			free(host->entry);
			host->entry = l->entry;
			l->entry = nullptr;
			host->expires = loop->now + LOOKUP_TTL_NS;
		} else if (host->entry) {
			LOG(LOG_WARN, "cannot look up %s, so its old addresses serve: %s",
			    host->key, why);
		}
		while (!list_is_empty(&host->waiting)) {
			struct http_request *request = container_of(host->waiting.next,
			                                           struct http_request,
			                                           hook);
			list_del(&request->hook);
			int e = host->entry ? http_pin(request, host->entry) : 0;
			if (host->entry && !e) {
				list_append(&http->queued, &request->hook);
				loop_ready_bulk(loop, &http->start);
				continue;
			}
			request->fail = CURLE_COULDNT_RESOLVE_HOST;
			snprintf(request->error, sizeof request->error, "cannot look up %s: %s",
			         host->key, e ? strerror(e) : why);
			list_append(&http->requests, &request->hook);
			loop_ready_bulk(loop, &http->fail);
		}
	}
	free(l->entry);
	free(l);
}

/* Have a blocker look @p host up. */
static int
http_lookup (struct http_host *host)
{
	struct http_lookup *l = malloc(sizeof *l + host->len);
	if (!l)
		return ENOMEM;
	*l = (struct http_lookup){
		.job = {
			.done.fn = http_looked_up,
			.work = http_look_up,
			.loop = host->http->loop
		},
		.host = host,
		.name = host->name,
		.len = host->len
	};
	memcpy(l->key, host->key, host->len);
	int e = blockers_submit(host->http->blockers, &l->job);
	if (e)
		free(l);
	else
		host->lookup = l;
	return e;
}

/* The host that @p url names, if the bridge looks names up and it is one,
 * rather than an address; else, and when out of memory, nullptr, which
 * leaves the lookup to libcurl. */
static struct http_host *
http_host (struct http *http,
           char const  *url)
{
	CURLU *u = http->blockers ? curl_url() : nullptr;
	char *name = nullptr;
	char *port = nullptr;
	struct http_host *host = nullptr;
	unsigned char addr[sizeof (struct in_addr)];
	if (u && !curl_url_set(u, CURLUPART_URL, url, 0) &&
	    !curl_url_get(u, CURLUPART_HOST, &name, 0) &&
	    !curl_url_get(u, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) &&
	    *name != '[' && inet_pton(AF_INET, name, addr) != 1) {
		size_t n = strlen(name) + 1;
		size_t p = strlen(port) + 1;
		struct list *node;
		list_foreach (node, &http->hosts) {
			struct http_host *h = container_of(node, struct http_host, hook);
			if (!strcmp(h->key, name) && !strcmp(h->key + n, port)) {
				host = h;
				break;
			}
		}
		if (!host && (host = malloc(sizeof *host + n + p))) {
			*host = (struct http_host){
				.http = http,
				.name = n - 1,
				.len = n + p
			};
			list_init(&host->waiting);
			memcpy(host->key, name, n);
			memcpy(host->key + n, port, p);
			list_append(&http->hosts, &host->hook);
		}
	}
	curl_free(port);
	curl_free(name);
	curl_url_cleanup(u);
	return host;
}

int
http_init (struct http         **p_http,
           struct loop          *loop,
           struct http_metrics  *metrics)
{
	if (!p_http || !loop)
		return EFAULT;
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
		return EIO;

	struct http *http = malloc(sizeof *http);
	if (!http) {
		curl_global_cleanup();
		return ENOMEM;
	}
	*http = (struct http){
		.timer.fn = http_timer_event,
		.timeout.fn = http_timeout_step,
		.start.fn = http_start_step,
		.fail.fn = http_fail_step,
		.loop = loop,
		.metrics = metrics,
		.multi = curl_multi_init()
	};
	list_init(&http->queued);
	list_init(&http->requests);
	list_init(&http->sockets);
	list_init(&http->hosts);
	*p_http = http;

	int e = !http->multi || loop_timer_set(loop, &http->timer, UINT64_MAX)
	      ? ENOMEM : 0;
	if (!e &&
	    (curl_multi_setopt(http->multi, CURLMOPT_SOCKETFUNCTION, http_socket_cb) != CURLM_OK ||
	     curl_multi_setopt(http->multi, CURLMOPT_SOCKETDATA, http) != CURLM_OK ||
	     curl_multi_setopt(http->multi, CURLMOPT_TIMERFUNCTION, http_timer_cb) != CURLM_OK ||
	     curl_multi_setopt(http->multi, CURLMOPT_TIMERDATA, http) != CURLM_OK))
		e = EIO;
	if (e)
		http_fini(p_http);
	return e;
}

void
http_cancel (struct http         *http,
             struct http_request **p_request)
{
	if (!http || !p_request || !*p_request)
		return;

	struct http_request *request = *p_request;
	*p_request = nullptr;
	if (request->easy) {
		curl_multi_remove_handle(http->multi, request->easy);
		curl_easy_cleanup(request->easy);
	}
	if (request->hook.next && request->hook.prev)
		list_del(&request->hook);
	curl_slist_free_all(request->resolve);
	curl_slist_free_all(request->headers);
	free(request);
}

void
http_end (struct http         *http,
          struct http_request **p_request,
          CURLcode              result)
{
	struct http_request *request = *p_request;
	*p_request = nullptr;
	http_finish(http, request, result);
}

CURLcode
http_resume (struct http_request *request)
{
	return curl_easy_pause(request->easy, CURLPAUSE_CONT);
}

bool
http_sent (struct http_request const *request)
{
	curl_off_t body = 0;
	long head = 0;
	return request->started &&
	       !curl_easy_getinfo(request->easy, CURLINFO_REQUEST_SIZE, &head) && head &&
	       !curl_easy_getinfo(request->easy, CURLINFO_SIZE_UPLOAD_T, &body) &&
	       body >= (curl_off_t)request->len;
}

void
http_response (struct http_request const  *request,
               long                       *status,
               char const                **type)
{
	*status = 0;
	*type = nullptr;
	curl_easy_getinfo(request->easy, CURLINFO_RESPONSE_CODE, status);
	curl_easy_getinfo(request->easy, CURLINFO_CONTENT_TYPE, type);
}

long
http_retry_after (struct http const *http)
{
	return http->retry;
}

void
http_connections (struct http *http,
                  long         n)
{
	(void)curl_multi_setopt(http->multi, CURLMOPT_MAX_HOST_CONNECTIONS, n);
	(void)curl_multi_setopt(http->multi, CURLMOPT_MAXCONNECTS, n + 1);
}

void
http_resolver (struct http     *http,
               struct blockers *blockers)
{
	http->blockers = blockers;
}

void
http_fini (struct http **p_http)
{
	if (!p_http || !*p_http)
		return;

	struct http *http = *p_http;
	*p_http = nullptr;

	/* A lookup under way comes back to nobody, and frees itself. */
	while (!list_is_empty(&http->hosts)) {
		struct http_host *host = container_of(http->hosts.next, struct http_host,
		                                      hook);
		list_transplant(&http->queued, &host->waiting);
		if (host->lookup)
			host->lookup->host = nullptr;
		list_del(&host->hook);
		free(host->entry);
		free(host);
	}
	struct list *node;
	struct list *tmp;
	struct list *const lists[] = { &http->queued, &http->requests };
	for (size_t i = 0; i < ARRAY_SIZE(lists); ++i) {
		list_foreach_safe (node, tmp, lists[i]) {
			struct http_request *request = container_of(node,
			                                           struct http_request,
			                                           hook);
			http_cancel(http, &request);
		}
	}

	/* Cleanup may still invoke the socket and timer callbacks. */
	if (http->multi)
		curl_multi_cleanup(http->multi);

	list_foreach_safe (node, tmp, &http->sockets) {
		struct http_socket *socket = container_of(node, struct http_socket, hook);
		loop_unready(&socket->step);
		loop_unwatch(http->loop, &socket->ref);
		list_del(&socket->hook);
		free(socket);
	}
	loop_unready(&http->timeout);
	loop_unready(&http->start);
	loop_unready(&http->fail);
	loop_timer_stop(http->loop, &http->timer);
	free(http);
	curl_global_cleanup();
}

int
http_post_json (struct http         *http,
                char const          *url,
                char const          *body,
                size_t               len,
                bool                 copy,
                char const *const   *headers,
                long                 timeout_ms,
                http_write_cb       *write_cb,
                http_done_cb        *done_cb,
                void                *user,
                struct http_request **p_request)
{
	if (!http || !url || !write_cb || !done_cb || !p_request)
		return EFAULT;

	struct http_request *request = calloc(1, sizeof *request);
	if (!request)
		return errno ? errno : ENOMEM;
	list_init(&request->hook);

	request->easy = curl_easy_init();
	request->write_cb = write_cb;
	request->done_cb = done_cb;
	request->user = user;
	request->len = body ? len : 0;
	/* A body is JSON.  Appending keeps the list's head, the request's. */
	bool ok = request->easy &&
	          (!body || (request->headers = curl_slist_append(
	                     nullptr, "Content-Type: application/json")));
	for (; ok && headers && *headers; ++headers) {
		struct curl_slist *list = curl_slist_append(request->headers, *headers);
		ok = list;
		if (!request->headers)
			request->headers = list;
	}
	if (!ok) {
		http_cancel(http, &request);
		return ENOMEM;
	}

#define SETOPT(opt, value) \
	do { \
		if (curl_easy_setopt(request->easy, (opt), (value)) != CURLE_OK) { \
			http_cancel(http, &request); \
			return EIO; \
		} \
	} while (0)

	SETOPT(CURLOPT_URL, url);
	SETOPT(CURLOPT_HTTPHEADER, request->headers);
	if (body) {
		SETOPT(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
		SETOPT(copy ? CURLOPT_COPYPOSTFIELDS : CURLOPT_POSTFIELDS, body);
	}
	SETOPT(CURLOPT_WRITEFUNCTION, http_write_adapter);
	SETOPT(CURLOPT_WRITEDATA, request);
	SETOPT(CURLOPT_PRIVATE, request);
	SETOPT(CURLOPT_ERRORBUFFER, request->error);
	SETOPT(CURLOPT_NOSIGNAL, 1L);
	SETOPT(CURLOPT_CONNECTTIMEOUT_MS, 10'000L);
	SETOPT(CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 0L);
	SETOPT(CURLOPT_TCP_KEEPALIVE, 1L);

#undef SETOPT

	/* A host by name waits for its addresses, unless they are fresh. */
	request->since = loop_clock();
	struct http_host *host = http_host(http, url);
	bool fresh = host && host->entry && host->expires > request->since;
	int e = fresh ? http_pin(request, host->entry) : 0;
	if (host && !fresh) {
		list_append(&host->waiting, &request->hook);
		e = host->lookup ? 0 : http_lookup(host);
	}
	if (e) {
		http_cancel(http, &request);
		return e;
	}
	if (!host || fresh) {
		list_append(&http->queued, &request->hook);
		loop_ready_bulk(http->loop, &http->start);
	}
	*p_request = request;
	return 0;
}
