/** @file libcurl multi-socket integration for the epoll loop. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#include "http.h"
#include "list.h"
#include "util.h"

struct http_socket {
	struct list     hook;
	struct loop_ref ref;
	struct http    *http;
	curl_socket_t   socket;
};

struct http_request {
	struct list       hook;
	struct curl_slist *headers;
	http_write_cb     *write_cb;
	http_done_cb      *done_cb;
	void              *user;
	char              *body;
	CURL              *easy;
	char               error[CURL_ERROR_SIZE];
};

struct http {
	struct loop_ref timer_ref;
	struct list     requests;
	struct list     sockets;
	struct loop    *loop;
	CURLM          *multi;
	int             timerfd;
	int             running;
};

static void
http_check_done (struct http *http)
{
	int left;
	CURLMsg *msg;
	while ((msg = curl_multi_info_read(http->multi, &left))) {
		if (msg->msg != CURLMSG_DONE)
			continue;

		struct http_request *request = nullptr;
		curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &request);
		if (!request)
			continue;

		CURLcode result = msg->data.result;
		long status = 0;
		curl_easy_getinfo(request->easy, CURLINFO_RESPONSE_CODE, &status);
		curl_multi_remove_handle(http->multi, request->easy);
		curl_easy_cleanup(request->easy);
		request->easy = nullptr;
		list_del(&request->hook);

		http_done_cb *done = request->done_cb;
		void *user = request->user;
		char error[CURL_ERROR_SIZE];
		snprintf(error, sizeof error, "%s", request->error);

		curl_slist_free_all(request->headers);
		free(request->body);
		free(request);
		done(result, status, error, user);
	}
}

static int
http_action (struct http *http,
             curl_socket_t socket,
             int           ev_bitmask)
{
	CURLMcode mc = curl_multi_socket_action(http->multi, socket, ev_bitmask,
	                                         &http->running);
	if (mc != CURLM_OK)
		return EIO;
	http_check_done(http);
	return 0;
}

static int
http_socket_event (struct loop     *loop,
                   struct loop_ref *ref,
                   uint32_t         events)
{
	(void)loop;
	struct http_socket *socket = ref->owner;
	int ev = 0;
	if (events & EPOLLIN)
		ev |= CURL_CSELECT_IN;
	if (events & EPOLLOUT)
		ev |= CURL_CSELECT_OUT;
	if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
		ev |= CURL_CSELECT_ERR;
	return http_action(socket->http, socket->socket, ev);
}

static int
http_timer_event (struct loop     *loop,
                  struct loop_ref *ref,
                  uint32_t         events)
{
	(void)loop;
	(void)events;
	struct http *http = ref->owner;
	uint64_t expirations;
	while (read(http->timerfd, &expirations, sizeof expirations) < 0 &&
	       errno == EINTR) {}
	return http_action(http, CURL_SOCKET_TIMEOUT, 0);
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
http_socket_cb (CURL   *easy,
                curl_socket_t socket,
                int     what,
                void   *clientp,
                void   *socketp)
{
	(void)easy;
	struct http *http = clientp;
	struct http_socket *hs = socketp;

	if (what == CURL_POLL_REMOVE) {
		if (hs) {
			loop_unwatch(http->loop, &hs->ref);
			curl_multi_assign(http->multi, socket, nullptr);
			hs->socket = CURL_SOCKET_BAD;
		}
		return 0;
	}

	uint32_t events = curl_events(what);
	if (!events)
		return 0;

	if (hs)
		return loop_modify(http->loop, &hs->ref, events) ? -1 : 0;

	hs = calloc(1, sizeof *hs);
	if (!hs)
		return -1;
	list_init(&hs->hook);
	hs->ref.fd = -1;
	hs->http = http;
	hs->socket = socket;

	int e = loop_watch(http->loop, &hs->ref, socket, events,
	                   http_socket_event, hs);
	if (e) {
		free(hs);
		return -1;
	}
	list_append(&http->sockets, &hs->hook);

	CURLMcode mc = curl_multi_assign(http->multi, socket, hs);
	if (mc != CURLM_OK) {
		loop_unwatch(http->loop, &hs->ref);
		list_del(&hs->hook);
		free(hs);
		return -1;
	}
	return 0;
}

static int
http_timer_cb (CURLM *multi,
               long   timeout_ms,
               void  *user)
{
	(void)multi;
	struct http *http = user;
	struct itimerspec its = {0};

	if (timeout_ms >= 0) {
		if (!timeout_ms) {
			its.it_value.tv_nsec = 1;
		} else {
			its.it_value.tv_sec = timeout_ms / 1000;
			its.it_value.tv_nsec = (timeout_ms % 1000) * 1'000'000L;
		}
	}

	return timerfd_settime(http->timerfd, 0, &its, nullptr) ? -1 : 0;
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

int
http_init (struct http **p_http,
           struct loop  *loop)
{
	if (!p_http || !loop)
		return EFAULT;

	CURLcode ce = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (ce != CURLE_OK)
		return EIO;

	struct http *http = calloc(1, sizeof *http);
	if (!http) {
		curl_global_cleanup();
		return errno ? errno : ENOMEM;
	}
	http->timerfd = -1;
	http->timer_ref.fd = -1;
	http->loop = loop;
	list_init(&http->requests);
	list_init(&http->sockets);

	http->multi = curl_multi_init();
	if (!http->multi) {
		free(http);
		curl_global_cleanup();
		return ENOMEM;
	}

	http->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (http->timerfd < 0) {
		int e = errno;
		curl_multi_cleanup(http->multi);
		free(http);
		curl_global_cleanup();
		return e;
	}

	int e = loop_watch(loop, &http->timer_ref, http->timerfd, EPOLLIN,
	                   http_timer_event, http);
	if (e) {
		close(http->timerfd);
		curl_multi_cleanup(http->multi);
		free(http);
		curl_global_cleanup();
		return e;
	}

	if (curl_multi_setopt(http->multi, CURLMOPT_SOCKETFUNCTION, http_socket_cb) != CURLM_OK ||
	    curl_multi_setopt(http->multi, CURLMOPT_SOCKETDATA, http) != CURLM_OK ||
	    curl_multi_setopt(http->multi, CURLMOPT_TIMERFUNCTION, http_timer_cb) != CURLM_OK ||
	    curl_multi_setopt(http->multi, CURLMOPT_TIMERDATA, http) != CURLM_OK) {
		http_fini(&http);
		return EIO;
	}

	*p_http = http;
	return 0;
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
	curl_slist_free_all(request->headers);
	free(request->body);
	free(request);
}

void
http_fini (struct http **p_http)
{
	if (!p_http || !*p_http)
		return;

	struct http *http = *p_http;
	*p_http = nullptr;

	struct list *node;
	struct list *tmp;
	list_foreach_safe (node, tmp, &http->requests) {
		struct http_request *request = container_of(node, struct http_request,
		                                           hook);
		http_cancel(http, &request);
	}

	if (http->timer_ref.fd >= 0)
		loop_unwatch(http->loop, &http->timer_ref);

	list_foreach_safe (node, tmp, &http->sockets) {
		struct http_socket *socket = container_of(node, struct http_socket, hook);
		if (socket->ref.fd >= 0)
			loop_unwatch(http->loop, &socket->ref);
		list_del(&socket->hook);
		free(socket);
	}
	if (http->timerfd >= 0)
		close(http->timerfd);
	if (http->multi)
		curl_multi_cleanup(http->multi);
	free(http);
	curl_global_cleanup();
}

int
http_post_json (struct http       *http,
                char const        *url,
                char const        *body,
                http_write_cb     *write_cb,
                http_done_cb      *done_cb,
                void              *user,
                struct http_request **p_request)
{
	if (!http || !url || !body || !write_cb || !done_cb || !p_request)
		return EFAULT;

	struct http_request *request = calloc(1, sizeof *request);
	if (!request)
		return errno ? errno : ENOMEM;
	list_init(&request->hook);

	request->body = strdup(body);
	request->easy = curl_easy_init();
	request->write_cb = write_cb;
	request->done_cb = done_cb;
	request->user = user;
	if (!request->body || !request->easy) {
		http_cancel(http, &request);
		return ENOMEM;
	}

	request->headers = curl_slist_append(nullptr,
	                                     "Content-Type: application/json");
	if (!request->headers) {
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
	SETOPT(CURLOPT_POST, 1L);
	SETOPT(CURLOPT_POSTFIELDS, request->body);
	SETOPT(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(request->body));
	SETOPT(CURLOPT_WRITEFUNCTION, http_write_adapter);
	SETOPT(CURLOPT_WRITEDATA, request);
	SETOPT(CURLOPT_PRIVATE, request);
	SETOPT(CURLOPT_ERRORBUFFER, request->error);
	SETOPT(CURLOPT_NOSIGNAL, 1L);
	SETOPT(CURLOPT_CONNECTTIMEOUT_MS, 10'000L);
	SETOPT(CURLOPT_TCP_KEEPALIVE, 1L);

#undef SETOPT

	CURLMcode mc = curl_multi_add_handle(http->multi, request->easy);
	if (mc != CURLM_OK) {
		http_cancel(http, &request);
		return EIO;
	}

	list_append(&http->requests, &request->hook);
	*p_request = request;
	return 0;
}
