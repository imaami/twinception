/** @file cli_loop.c
 *
 * CLI reactor implementation.
 */
#include <errno.h>
#include <stddef.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "cli_loop.h"
#include "compat.h"
#include "loop.h"

uint64_t
cli_loop_counter (struct cli_loop     *loop,
                  struct cli_loop_ref *ref)
{
	uint64_t count;
	ssize_t n;
	do {
		n = read(ref->fd, &count, sizeof count);
	} while (n < 0 && errno == EINTR);
	if (n == (ssize_t)sizeof count)
		return count;
	if ((n >= 0 || errno != EAGAIN) && !loop->error)
		loop->error = n < 0 ? errno : EIO;
	return 0;
}

static void
cli_loop_timer_read (struct cli_loop     *loop,
                     struct cli_loop_ref *ref,
                     uint32_t             events)
{
	(void)events;
	(void)cli_loop_counter(loop, ref);
}

int
cli_loop_init (struct cli_loop *loop)
{
	*loop = (struct cli_loop){
		.timer.fd = -1,
		.deadline = UINT64_MAX,
		.until = UINT64_MAX,
		.epfd = -1,
	};
	list_init(&loop->refs);
	list_init(&loop->ready);
	loop->epfd = epoll_create1(EPOLL_CLOEXEC);
	if (loop->epfd < 0)
		return errno;
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (fd < 0) {
		int e = errno;
		cli_loop_fini(loop);
		return e;
	}
	int e = cli_loop_watch(loop, &loop->timer, fd, EPOLLIN, cli_loop_timer_read);
	if (e) {
		close(fd);
		cli_loop_fini(loop);
	}
	return e;
}

int
cli_loop_watch (struct cli_loop     *loop,
                struct cli_loop_ref *ref,
                int                  fd,
                uint32_t             events,
                cli_loop_cb         *cb)
{
	if (!ref || !cb || fd < 0 || loop->epfd < 0)
		return EINVAL;
	struct epoll_event event = { .events = events, .data.ptr = ref };
	if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &event))
		return errno;
	*ref = (struct cli_loop_ref){ .cb = cb, .events = events, .fd = fd };
	list_init(&ref->hook);
	list_append(&loop->refs, &ref->hook);
	return 0;
}

void
cli_loop_unwatch (struct cli_loop     *loop,
                  struct cli_loop_ref *ref)
{
	if (!ref || ref->fd < 0)
		return;
	if (epoll_ctl(loop->epfd, EPOLL_CTL_DEL, ref->fd, nullptr) &&
	    errno != ENOENT && errno != EBADF && !loop->error)
		loop->error = errno;
	list_del(&ref->hook);
	*ref = (struct cli_loop_ref){ .fd = -1 };
}

void
cli_loop_close (struct cli_loop     *loop,
                struct cli_loop_ref *ref)
{
	int fd = ref->fd;
	if (fd < 0)
		return;
	cli_loop_unwatch(loop, ref);
	(void)close(fd);
}

void
cli_loop_ready (struct cli_loop      *loop,
                struct cli_loop_step *step)
{
	if (!step->hook.next)
		list_append(&loop->ready, &step->hook);
}

void
cli_loop_unready (struct cli_loop_step *step)
{
	if (step->hook.next) {
		list_del(&step->hook);
		step->hook = (struct list){};
	}
}

static void
cli_loop_steps (struct cli_loop *loop)
{
	struct list pending;
	list_init(&pending);
	list_transplant(&pending, &loop->ready);
	for (size_t count = 0; count < 64 && !list_is_empty(&pending); ++count) {
		struct cli_loop_step *step = container_of(pending.next, struct cli_loop_step, hook);
		cli_loop_unready(step);
		step->fn(loop, step);
	}
	list_transplant(&pending, &loop->ready);
	list_transplant(&loop->ready, &pending);
}

static int
cli_loop_timer_set (struct cli_loop *loop,
                    uint64_t         deadline)
{
	if (loop->deadline == deadline)
		return 0;
	struct itimerspec timer = {};
	if (deadline != UINT64_MAX) {
		uint64_t seconds = deadline / 1000000000;
		timer.it_value.tv_sec = (time_t)seconds;
		if ((uint64_t)timer.it_value.tv_sec != seconds)
			return EOVERFLOW;
		timer.it_value.tv_nsec = (long)(deadline % 1000000000);
	}
	if (timerfd_settime(loop->timer.fd, TFD_TIMER_ABSTIME, &timer, nullptr))
		return errno;
	loop->deadline = deadline;
	return 0;
}

int
cli_loop_turn (struct cli_loop *loop,
               uint64_t         deadline)
{
	if (loop->dispatching)
		return EBUSY;
	if (loop->error)
		return loop->error;
	if (deadline > loop->until)
		deadline = loop->until;
	bool expired = deadline != UINT64_MAX && deadline <= loop_clock();
	int e = cli_loop_timer_set(loop, expired ? UINT64_MAX : deadline);
	if (e)
		return e;
	int timeout = expired || !list_is_empty(&loop->ready) ? 0 : -1;
	struct epoll_event events[64];
	int n;
	do {
		n = epoll_wait(loop->epfd, events, (int)array_size(events), timeout);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return errno;
	loop->dispatching = 1;
	for (int i = 0; i < n; ++i) {
		struct cli_loop_ref *ref = events[i].data.ptr;
		if (ref->cb)
			ref->cb(loop, ref, events[i].events);
	}
	cli_loop_steps(loop);
	loop->dispatching = 0;
	if (!loop->error && loop->after_turn)
		loop->after_turn(loop);
	return loop->error;
}

void
cli_loop_fini (struct cli_loop *loop)
{
	cli_loop_close(loop, &loop->timer);
	while (!list_is_empty(&loop->refs)) {
		struct cli_loop_ref *ref = container_of(loop->refs.next, struct cli_loop_ref, hook);
		cli_loop_unwatch(loop, ref);
	}
	while (!list_is_empty(&loop->ready)) {
		struct cli_loop_step *step = container_of(loop->ready.next, struct cli_loop_step, hook);
		cli_loop_unready(step);
	}
	if (loop->epfd >= 0)
		close(loop->epfd);
	loop->epfd = -1;
	loop->after_turn = nullptr;
}
