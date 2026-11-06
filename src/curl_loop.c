/** @file curl_loop.c
 *
 * libcurl's multi-socket interface on an event loop.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

#include "clk.h"
#include "compat.h"
#include "curl_loop.h"

struct curl_loop_socket {
	struct list      hook;  /* on the curl_loop's sockets */
	struct loop_ref  ref;   /* owner: the curl_loop */
	struct loop_step step;  /* reports what to curl */
	uint64_t         what;  /* CURL_CSELECT_* bits seen since the step */
};

void
curl_loop_action (struct curl_loop *cl,
                  curl_socket_t     fd,
                  int               what)
{
	int running;
	CURLMcode mc = curl_multi_socket_action(cl->multi, fd, what, &running);
	if (mc != CURLM_OK)
		cl->fail(cl, nullptr, mc == CURLM_OUT_OF_MEMORY ? ENOMEM : EIO, curl_multi_strerror(mc));
	int left;
	CURLMsg *msg;
	while ((msg = curl_multi_info_read(cl->multi, &left))) {
		void *transfer = nullptr;
		if (msg->msg == CURLMSG_DONE &&
		    !curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &transfer) && transfer)
			cl->done(cl, transfer, msg->data.result);
	}
}

static void
curl_loop_socket_event (struct loop     *loop,
                        struct loop_ref *ref,
                        uint32_t         events)
{
	struct curl_loop_socket *s = container_of(ref, struct curl_loop_socket, ref);
	s->what |= (events & EPOLLIN ? CURL_CSELECT_IN : 0) |
	           (events & EPOLLOUT ? CURL_CSELECT_OUT : 0) |
	           (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP) ? CURL_CSELECT_ERR : 0);
	loop_ready_bulk(loop, &s->step);
}

static void
curl_loop_socket_step (struct loop      *loop,
                       struct loop_step *step)
{
	(void)loop;
	struct curl_loop_socket *s = container_of(step, struct curl_loop_socket, step);
	int what = (int)s->what;
	s->what = 0;
	curl_loop_action(s->ref.owner, s->ref.fd, what);
}

static void
curl_loop_socket_release (struct loop_ref *ref)
{
	free(container_of(ref, struct curl_loop_socket, ref));
}

/* Watch @p fd, which curl asks for @p events. */
static int
curl_loop_watch (struct curl_loop *cl,
                 curl_socket_t     fd,
                 uint32_t          events)
{
	struct curl_loop_socket *s = malloc(sizeof *s);
	if (!s)
		return ENOMEM;
	*s = (struct curl_loop_socket){.ref.fd = -1, .step.fn = curl_loop_socket_step};
	int e = loop_watch(cl->loop, &s->ref, fd, events, curl_loop_socket_event, cl);
	if (e) {
		free(s);
		return e;
	}
	list_append(&cl->sockets, &s->hook);
	(void)curl_multi_assign(cl->multi, fd, s);
	return 0;
}

/* An error returned to curl would fail every transfer, so a socket that
 * cannot be watched fails its own transfer through cl->fail.
 */
static int
curl_loop_socket_cb (CURL          *easy,
                     curl_socket_t  fd,
                     int            what,
                     void          *user,
                     void          *assigned)
{
	struct curl_loop *cl = user;
	struct curl_loop_socket *s = assigned;
	if (what == CURL_POLL_REMOVE) {
		if (s) {
			(void)curl_multi_assign(cl->multi, fd, nullptr);
			list_del(&s->hook);
			loop_unready(&s->step);
			loop_release(cl->loop, &s->ref, curl_loop_socket_release);
		}
		return 0;
	}

	static uint32_t const events[] = {
		[CURL_POLL_IN]    = EPOLLIN | EPOLLRDHUP,
		[CURL_POLL_OUT]   = EPOLLOUT | EPOLLRDHUP,
		[CURL_POLL_INOUT] = EPOLLIN | EPOLLOUT | EPOLLRDHUP
	};
	if (what <= CURL_POLL_NONE || what >= (int)array_size(events))
		return 0;
	int e = s ? loop_modify(cl->loop, &s->ref, events[what])
	          : curl_loop_watch(cl, fd, events[what]);
	void *transfer = nullptr;
	if (e && !curl_easy_getinfo(easy, CURLINFO_PRIVATE, &transfer) && transfer)
		cl->fail(cl, transfer, e, strerror(e));
	return 0;
}

static void
curl_loop_timer_event (struct loop       *loop,
                       struct loop_timer *timer)
{
	loop_ready_bulk(loop, &container_of(timer, struct curl_loop, timer)->timeout);
}

static void
curl_loop_timeout_step (struct loop      *loop,
                        struct loop_step *step)
{
	(void)loop;
	curl_loop_action(container_of(step, struct curl_loop, timeout), CURL_SOCKET_TIMEOUT, 0);
}

/* The timer is in the heap from init to fini, so setting it cannot fail. A
 * timeout too far off to count in ns never expires.
 */
static int
curl_loop_timer_cb (CURLM *multi,
                    long   timeout_ms,
                    void  *user)
{
	(void)multi;
	struct curl_loop *cl = user;
	uint64_t now = loop_clock();
	(void)loop_timer_set(cl->loop, &cl->timer,
	                     timeout_ms >= 0 && (uint64_t)timeout_ms < (UINT64_MAX - now) / MS
	                     ? now + (uint64_t)timeout_ms * MS : UINT64_MAX);
	return 0;
}

int
curl_loop_init (struct curl_loop  *cl,
                struct loop       *loop,
                curl_loop_done_cb *done,
                curl_loop_fail_cb *fail)
{
	*cl = (struct curl_loop){
		.timeout.fn = curl_loop_timeout_step,
		.timer.fn = curl_loop_timer_event,
		.loop = loop,
		.multi = curl_multi_init(),
		.done = done,
		.fail = fail
	};
	list_init(&cl->sockets);
	int e = !cl->multi || loop_timer_set(loop, &cl->timer, UINT64_MAX) ? ENOMEM : 0;
	/* Each callback's data first, as a cleanup after a refused option may
	 * call the callbacks set before it. */
	if (!e &&
	    (curl_multi_setopt(cl->multi, CURLMOPT_SOCKETDATA, cl) != CURLM_OK ||
	     curl_multi_setopt(cl->multi, CURLMOPT_SOCKETFUNCTION, curl_loop_socket_cb) != CURLM_OK ||
	     curl_multi_setopt(cl->multi, CURLMOPT_TIMERDATA, cl) != CURLM_OK ||
	     curl_multi_setopt(cl->multi, CURLMOPT_TIMERFUNCTION, curl_loop_timer_cb) != CURLM_OK))
		e = EIO;
	if (e)
		curl_loop_fini(cl);
	return e;
}

void
curl_loop_fini (struct curl_loop *cl)
{
	if (!cl->multi)
		return;
	/* Cleanup may still call the socket and timer callbacks. */
	curl_multi_cleanup(cl->multi);
	cl->multi = nullptr;
	struct list *node;
	struct list *tmp;
	list_foreach_safe (node, tmp, &cl->sockets) {
		struct curl_loop_socket *s = container_of(node, struct curl_loop_socket, hook);
		loop_unready(&s->step);
		loop_unwatch(cl->loop, &s->ref);
		list_del(&s->hook);
		free(s);
	}
	loop_unready(&cl->timeout);
	(void)loop_timer_stop(cl->loop, &cl->timer);
}
