/**
 * @file http.c
 * HTTP requests over libcurl on the event loop (src/curl_loop.h).
 *
 * An error fails at most the transfer it belongs to, never the loop or the
 * other transfers. A done callback may ask what Retry-After its answer said.
 * A write callback gets no more of an answer than the step's share of
 * receiving takes: curl holds the rest back until a later step lets it on.
 *
 * With a resolver set, host names are looked up on a blocker thread rather
 * than by libcurl, whose resolver would start a thread of its own for each
 * lookup. A request to a host waits for its addresses, while one lookup per
 * host is under way; the addresses are kept for LOOKUP_TTL_NS, and a lookup
 * that fails leaves those it had in use.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "blocker.h"
#include "curl_loop.h"
#include "http.h"
#include "list.h"
#include "log.h"
#include "util.h"

/* Requests handed to curl per step: each one connects, so a burst of them
 * would make one long step.
 */
enum { START_BATCH = 2 };

/* How long looked-up addresses serve new requests. */
static constexpr uint64_t LOOKUP_TTL_NS = 60 * NS;

struct http_request {
	struct list        hook;     /* on a host's waiting list, then on
	                                http->queued, then http->requests */
	struct list        held;     /* on http->held while curl holds the
	                                answer back */
	struct http       *http;
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
	struct curl_loop     curl;
	struct loop_step     start;     /* hands queued requests to curl */
	struct loop_step     fail;      /* fails requests that curl cannot run */
	struct loop_step     resume;    /* lets the answers held back on */
	struct list          queued;    /* requests not yet handed to curl */
	struct list          requests;
	struct list          held;      /* the requests whose answer curl holds
	                                   back, the first held first */
	struct list          hosts;     /* looked up, or being looked up */
	struct blocker      *blockers;  /* that look host names up, or nullptr */
	struct http_metrics *metrics;
	CURL                *ended;     /* the transfer whose done callback
	                                   runs, or nullptr */
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

/* Take @p transfer, a request, out of curl and free it, then report
 * @p result. The transfer's handle stays until the done callback returns, so
 * that http_type() can read the answer's Content-Type from it. A done
 * callback can end another request, so the handle of the outer callback is
 * restored after the callback.
 */
static void
http_finish (struct curl_loop *curl,
             void             *transfer,
             CURLcode          result)
{
	struct http *http = container_of(curl, struct http, curl);
	struct http_request *request = transfer;
	CURL *easy = request->easy;
	long status = 0;
	curl_off_t retry = 0;
	curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_getinfo(easy, CURLINFO_RETRY_AFTER, &retry);
	http->retry = retry > 0 && retry < LONG_MAX ? (long)retry : 0;
	if (request->started && http->metrics)
		hist_record(&http->metrics->run, loop_clock() - request->started);

	http_done_cb *done = request->done_cb;
	void *user = request->user;
	char error[CURL_ERROR_SIZE];
	memcpy(error, request->error, sizeof error);

	curl_multi_remove_handle(curl->multi, easy);
	request->easy = nullptr;
	http_cancel(http, &request);
	CURL *outer = http->ended;
	http->ended = easy;
	done(result, status, error, user);
	http->ended = outer;
	curl_easy_cleanup(easy);
}

/* Have @p transfer, a request whose socket cannot be watched, fail with
 * @p e on a later step, as the socket callback cannot remove it. Without a
 * transfer, curl could not act on an event, which only the log shows.
 */
static void
http_doom (struct curl_loop *curl,
           void             *transfer,
           int               e,
           char const       *why)
{
	struct http_request *request = transfer;
	if (!request) {
		LOG(LOG_ERROR, "curl: %s", why);
		return;
	}
	if (request->fail)
		return;
	request->fail = e == ENOMEM ? CURLE_OUT_OF_MEMORY : CURLE_COULDNT_CONNECT;
	util_format(request->error, sizeof request->error, "cannot watch the socket: %s", why);
	loop_ready_bulk(curl->loop, &container_of(curl, struct http, curl)->fail);
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
			http_finish(&http->curl, request, request->fail);
			return;
		}
	}
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
		list_move(&http->requests, &request->hook);
		request->started = loop_clock();
		if (http->metrics)
			hist_record(&http->metrics->wait, request->started - request->since);
		if (curl_multi_add_handle(http->curl.multi, request->easy) != CURLM_OK) {
			request->started = 0;
			request->fail = CURLE_FAILED_INIT;
			loop_ready_bulk(loop, &http->fail);
		}
	}
	if (!list_is_empty(&http->queued))
		loop_ready_bulk(loop, step);
	curl_loop_action(&http->curl, CURL_SOCKET_TIMEOUT, 0);
}

/* Let the answers held back on, the first held first, while the step's
 * share of receiving has room for a write of curl, which may hold one back
 * again. A request that curl cannot let on ends with curl's error.
 */
static void
http_resume_step (struct loop      *loop,
                  struct loop_step *step)
{
	struct http *http = container_of(step, struct http, resume);
	while (!list_is_empty(&http->held)) {
		if (loop_room(loop, LOOP_RECV) < CURL_MAX_WRITE_SIZE) {
			loop_ready_bulk(loop, step);
			return;
		}
		struct http_request *request = container_of(http->held.next,
		                                            struct http_request, held);
		list_del(&request->held);
		CURLcode result = curl_easy_pause(request->easy, CURLPAUSE_CONT);
		if (result)
			http_finish(&http->curl, request, result);
	}
}

/* Give the write callback what curl wrote, unless the step's share of
 * receiving has no room for it: then curl holds it back until the resume
 * step lets it on. A write over a whole share, which curl does not make,
 * counts in full.
 */
static size_t
http_write_adapter (char   *ptr,
                    size_t  size,
                    size_t  nmemb,
                    void   *user)
{
	struct http_request *request = user;
	struct loop *loop = request->http->curl.loop;
	if (size && nmemb > SIZE_MAX / size)
		return 0;
	size_t len = size * nmemb;
	if (!loop_fits(loop, LOOP_RECV, len)) {
		http_resume(request);
		return CURL_WRITEFUNC_PAUSE;
	}
	size_t n = request->write_cb(ptr, len, request->user);
	if (n == len)
		loop_work(loop, LOOP_RECV, len);
	return n;
}

/* Append the address of @p ai, if it is one, to the @p len bytes of
 * @p l's entry, as libcurl takes it: IPv6 in brackets, after a comma but for
 * the first, which follows the ':' that ends "name:port:". There is room
 * for INET6_ADDRSTRLEN + 3 bytes. Returns the new length.
 */
static size_t
http_lookup_put (struct http_lookup    *l,
                 size_t                 len,
                 struct addrinfo const *ai)
{
	void const *addr;
	size_t v6;
	switch (ai->ai_family) {
	case AF_INET6:
		addr = &((struct sockaddr_in6 const *)(void const *)ai->ai_addr)->sin6_addr;
		v6 = 1;
		break;
	case AF_INET:
		addr = &((struct sockaddr_in const *)(void const *)ai->ai_addr)->sin_addr;
		v6 = 0;
		break;
	default:
		return len;
	}
	char *p = l->entry + len;
	if (p[-1] != ':')
		*p++ = ',';
	/* The bracket, which an IPv4 address writes over. */
	*p = '[';
	if (!inet_ntop(ai->ai_family, addr, p + v6, INET6_ADDRSTRLEN))
		return len;
	p += v6 + strlen(p + v6);
	*p = ']';
	return (size_t)(p + v6 - l->entry);
}

/* On a blocker: look the host up, and write its addresses as libcurl takes
 * them: "name:port:address,...".
 */
static void
http_look_up (struct blocker_job *job)
{
	struct http_lookup *l = container_of(job, struct http_lookup, job);
	struct addrinfo const hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	struct addrinfo *list;
	if ((l->gai = getaddrinfo(l->key, l->key + l->name + 1, &hints, &list)))
		return;

	size_t n = 0;
	for (struct addrinfo const *ai = list; ai; ai = ai->ai_next)
		++n;
	/* The key, "name\0port\0", and for each address "[address]," at most,
	 * and the NUL. */
	l->entry = malloc(l->len + n * (INET6_ADDRSTRLEN + 3) + 1);
	if (!l->entry) {
		freeaddrinfo(list);
		l->gai = EAI_MEMORY;
		return;
	}
	memcpy(l->entry, l->key, l->len);
	l->entry[l->name] = ':';
	l->entry[l->len - 1] = ':';
	size_t len = l->len;
	for (struct addrinfo const *ai = list; ai; ai = ai->ai_next)
		len = http_lookup_put(l, len, ai);
	freeaddrinfo(list);
	l->entry[len] = '\0';
	if (len == l->len) {
		free(l->entry);
		l->entry = nullptr;
		l->gai = EAI_NONAME;
	}
}

/* Queue @p request for curl. */
static void
http_queue (struct http         *http,
            struct http_request *request)
{
	list_append(&http->queued, &request->hook);
	loop_ready_bulk(http->curl.loop, &http->start);
}

/* Sets CURLOPT_RESOLVE of the transfer of @p request to the address entry
 * of @p host, and queues the request. Returns 0, ENOMEM or EIO.
 */
static int
http_request_pin (struct http_request    *request,
                  struct http_host const *host)
{
	request->resolve = curl_slist_append(nullptr, host->entry);
	if (!request->resolve)
		return ENOMEM;
	if (curl_easy_setopt(request->easy, CURLOPT_RESOLVE, request->resolve))
		return EIO;
	http_queue(host->http, request);
	return 0;
}

/* Start @p request, which waited for @p host's addresses, or fail it, saying
 * @p why if the host has none.
 */
static void
http_host_resume (struct http_host    *host,
                  struct http_request *request,
                  char const          *why)
{
	struct http *http = host->http;
	list_del(&request->hook);
	if (host->entry) {
		int e = http_request_pin(request, host);
		if (!e)
			return;
		why = strerror(e);
	}
	request->fail = CURLE_COULDNT_RESOLVE_HOST;
	util_format(request->error, sizeof request->error, "cannot look up %s: %s",
	            host->key, why);
	list_append(&http->requests, &request->hook);
	loop_ready_bulk(http->curl.loop, &http->fail);
}

/* @p host's lookup @p l has come back: keep the addresses it found, and
 * start the requests that waited for them, or fail them. A lookup that
 * fails leaves the addresses the host had.
 */
static void
http_host_found (struct http_host   *host,
                 struct http_lookup *l)
{
	struct http *http = host->http;
	char const *why = "";
	host->lookup = nullptr;
	if (http->metrics)
		hist_add(&http->metrics->lookups[l->entry != nullptr], 1);
	if (l->entry) {
		free(host->entry);
		host->entry = l->entry;
		l->entry = nullptr;
		host->expires = http->curl.loop->now + LOOKUP_TTL_NS;
	} else {
		why = l->job.error ? strerror(l->job.error) : gai_strerror(l->gai);
		if (host->entry)
			LOG(LOG_WARN, "cannot look up %s, so its old addresses serve: %s",
			    host->key, why);
	}
	while (!list_is_empty(&host->waiting))
		http_host_resume(host, container_of(host->waiting.next, struct http_request,
		                                    hook), why);
}

/* On the loop: a lookup has come back, to its host unless http has gone. */
static void
http_looked_up (struct loop *loop,
                struct msg  *msg)
{
	(void)loop;
	struct http_lookup *l = container_of(msg, struct http_lookup, job.done);
	if (l->host)
		http_host_found(l->host, l);
	free(l->entry);
	l->entry = nullptr;
	free(l);
}

/* Have a blocker look @p host up. */
static int
http_host_lookup (struct http_host *host)
{
	struct http_lookup *l = malloc(sizeof *l + host->len);
	if (!l)
		return ENOMEM;
	*l = (struct http_lookup){
		.job = {
			.done.fn = http_looked_up,
			.work = http_look_up,
			.loop = host->http->curl.loop
		},
		.host = host,
		.name = host->name,
		.len = host->len
	};
	memcpy(l->key, host->key, host->len);
	int e = blocker_submit(host->http->blockers, &l->job);
	if (e)
		free(l);
	else
		host->lookup = l;
	return e;
}

/* The host @p name, of @p n bytes with its NUL, and @p port, of @p p:
 * found, or added; nullptr if out of memory.
 */
static struct http_host *
http_find_host (struct http *http,
                char const  *name,
                size_t       n,
                char const  *port,
                size_t       p)
{
	struct list *node;
	list_foreach (node, &http->hosts) {
		struct http_host *h = container_of(node, struct http_host, hook);
		if (h->len == n + p && !memcmp(h->key, name, n) && !memcmp(h->key + n, port, p))
			return h;
	}
	struct http_host *host = malloc(sizeof *host + n + p);
	if (!host)
		return nullptr;
	*host = (struct http_host){
		.http = http,
		.name = n - 1,
		.len = n + p
	};
	list_init(&host->waiting);
	memcpy(host->key, name, n);
	memcpy(host->key + n, port, p);
	list_append(&http->hosts, &host->hook);
	return host;
}

/* The host that @p url names, if the bridge looks names up and it is one,
 * rather than an address; else, and when out of memory, nullptr, which
 * leaves the lookup to libcurl.
 */
static struct http_host *
http_named_host (struct http *http,
                 char const  *url)
{
	if (!http->blockers)
		return nullptr;
	CURLU *u = curl_url();
	char *name = nullptr;
	char *port = nullptr;
	struct http_host *host = nullptr;
	unsigned char addr[sizeof (struct in_addr)];
	if (u && !curl_url_set(u, CURLUPART_URL, url, 0) &&
	    !curl_url_get(u, CURLUPART_HOST, &name, 0) &&
	    !curl_url_get(u, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) &&
	    *name != '[' && inet_pton(AF_INET, name, addr) != 1)
		host = http_find_host(http, name, strlen(name) + 1, port, strlen(port) + 1);
	curl_free(port);
	curl_free(name);
	curl_url_cleanup(u);
	return host;
}

/* Have @p request, to @p host, wait for its addresses, or with fresh ones,
 * queue it.
 */
static int
http_host_take (struct http_host    *host,
                struct http_request *request)
{
	if (host->entry && host->expires > request->since)
		return http_request_pin(request, host);
	list_append(&host->waiting, &request->hook);
	return host->lookup ? 0 : http_host_lookup(host);
}

struct http *
http_create (struct loop         *loop,
             struct http_metrics *metrics,
             int                 *e)
{
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		*e = EIO;
		return nullptr;
	}
	struct http *http = malloc(sizeof *http);
	if (!http) {
		curl_global_cleanup();
		*e = ENOMEM;
		return nullptr;
	}
	*http = (struct http){
		.start.fn = http_start_step,
		.fail.fn = http_fail_step,
		.resume.fn = http_resume_step,
		.metrics = metrics
	};
	list_init(&http->queued);
	list_init(&http->requests);
	list_init(&http->held);
	list_init(&http->hosts);
	if ((*e = curl_loop_init(&http->curl, loop, http_finish, http_doom)))
		http_destroy(&http);
	return http;
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
		curl_multi_remove_handle(http->curl.multi, request->easy);
		curl_easy_cleanup(request->easy);
		request->easy = nullptr;
	}
	list_del(&request->hook);
	list_del(&request->held);
	curl_slist_free_all(request->resolve);
	request->resolve = nullptr;
	curl_slist_free_all(request->headers);
	request->headers = nullptr;
	free(request);
}

void
http_end (struct http         *http,
          struct http_request **p_request,
          CURLcode              result)
{
	struct http_request *request = *p_request;
	*p_request = nullptr;
	http_finish(&http->curl, request, result);
}

void
http_resume (struct http_request *request)
{
	struct http *http = request->http;
	if (list_is_empty(&request->held))
		list_append(&http->held, &request->held);
	loop_ready_bulk(http->curl.loop, &http->resume);
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

char const *
http_type (struct http const *http)
{
	char const *type = nullptr;
	if (http->ended)
		curl_easy_getinfo(http->ended, CURLINFO_CONTENT_TYPE, &type);
	return type;
}

void
http_connections (struct http *http,
                  long         n)
{
	(void)curl_multi_setopt(http->curl.multi, CURLMOPT_MAX_HOST_CONNECTIONS, n);
	(void)curl_multi_setopt(http->curl.multi, CURLMOPT_MAXCONNECTS, n + 1);
}

void
http_resolver (struct http    *http,
               struct blocker *blockers)
{
	http->blockers = blockers;
}

void
http_destroy (struct http **p_http)
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
		host->entry = nullptr;
		free(host);
	}
	struct list *node;
	struct list *tmp;
	struct list *const lists[] = { &http->queued, &http->requests };
	for (size_t i = 0; i < array_size(lists); ++i) {
		list_foreach_safe (node, tmp, lists[i]) {
			struct http_request *request = container_of(node,
			                                           struct http_request,
			                                           hook);
			http_cancel(http, &request);
		}
	}

	curl_loop_fini(&http->curl);
	loop_unready(&http->start);
	loop_unready(&http->fail);
	loop_unready(&http->resume);
	free(http);
	curl_global_cleanup();
}

/* Appends the lines of @p headers to request->headers. @p headers is
 * nullptr or a nullptr-terminated array. If @p body is not nullptr and no
 * line of @p headers begins with "Content-Type:" in any letter case, the
 * function also appends "Content-Type: application/json". Returns 0 or
 * ENOMEM.
 */
static int
http_request_headers (struct http_request *request,
                      char const          *body,
                      char const *const   *headers)
{
	bool typed = !body;
	for (; headers && *headers; ++headers) {
		typed |= cstr_view_starts_case(cstr_view(*headers), CSTR_VIEW("Content-Type:"));
		struct curl_slist *list = curl_slist_append(request->headers, *headers);
		if (!list)
			return ENOMEM;
		request->headers = list;
	}
	if (typed)
		return 0;
	struct curl_slist *list = curl_slist_append(request->headers,
	                                            "Content-Type: application/json");
	if (!list)
		return ENOMEM;
	request->headers = list;
	return 0;
}

int
http_post_json (struct http         *http,
                char const          *url,
                char const          *body,
                size_t               len,
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
		return ENOMEM;
	list_init(&request->hook);
	list_init(&request->held);
	request->http = http;
	request->easy = curl_easy_init();
	request->write_cb = write_cb;
	request->done_cb = done_cb;
	request->user = user;
	request->len = body ? len : 0;
	if (!request->easy || http_request_headers(request, body, headers)) {
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
		SETOPT(CURLOPT_POSTFIELDS, body);
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
	struct http_host *host = http_named_host(http, url);
	int e = 0;
	if (!host)
		http_queue(http, request);
	else if ((e = http_host_take(host, request)))
		http_cancel(http, &request);
	*p_request = request;
	return e;
}

int
http_accept_encoding (struct http_request *request)
{
	if (!request)
		return EFAULT;
	return curl_easy_setopt(request->easy, CURLOPT_ACCEPT_ENCODING, "") == CURLE_OK
	       ? 0 : EIO;
}

int
http_method (struct http_request *request,
             char const          *method)
{
	if (!request || !method)
		return EFAULT;
	return curl_easy_setopt(request->easy, CURLOPT_CUSTOMREQUEST, method) ? EIO : 0;
}
