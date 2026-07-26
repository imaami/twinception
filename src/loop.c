/** @file Small epoll dispatcher with intrusive references. */
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "loop.h"
#include "util.h"

int
loop_init (struct loop *loop)
{
	int epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0)
		return errno;

	*loop = (struct loop){
		.epfd = epfd
	};
	list_init(&loop->refs);
	return 0;
}

void
loop_fini (struct loop *loop)
{
	struct list *node;
	struct list *tmp;
	list_foreach_safe (node, tmp, &loop->refs) {
		struct loop_ref *ref = container_of(node, struct loop_ref, hook);
		loop_unwatch(loop, ref);
	}

	if (loop->epfd >= 0)
		close(loop->epfd);
	*loop = (struct loop){ .epfd = -1 };
}

int
loop_watch (struct loop     *loop,
            struct loop_ref *ref,
            int              fd,
            uint32_t         events,
            loop_cb         *cb,
            void            *owner)
{
	if (!ref || !cb || fd < 0)
		return EINVAL;

	struct epoll_event event = {
		.events = events,
		.data.ptr = ref
	};
	if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &event))
		return errno;

	*ref = (struct loop_ref){
		.cb = cb,
		.owner = owner,
		.events = events,
		.fd = fd
	};
	list_init(&ref->hook);
	list_append(&loop->refs, &ref->hook);
	return 0;
}

int
loop_modify (struct loop     *loop,
             struct loop_ref *ref,
             uint32_t         events)
{
	if (!ref || ref->fd < 0)
		return EINVAL;
	if (ref->events == events)
		return 0;

	struct epoll_event event = {
		.events = events,
		.data.ptr = ref
	};
	if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, ref->fd, &event))
		return errno;

	ref->events = events;
	return 0;
}

void
loop_unwatch (struct loop     *loop,
              struct loop_ref *ref)
{
	if (!ref || ref->fd < 0)
		return;

	if (epoll_ctl(loop->epfd, EPOLL_CTL_DEL, ref->fd, nullptr) &&
	    errno != ENOENT && errno != EBADF)
		loop->error = errno;

	if (ref->hook.next && ref->hook.prev)
		list_del(&ref->hook);
	*ref = (struct loop_ref){ .fd = -1 };
}

int
loop_exec (struct loop *loop)
{
	struct epoll_event events[16];

	while (!loop->exit) {
		int n = epoll_wait(loop->epfd, events, ARRAY_SIZE(events), -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}

		for (int i = 0; i < n; ++i) {
			struct loop_ref *ref = events[i].data.ptr;
			if (!ref || !ref->cb)
				continue;

			int e = ref->cb(loop, ref, events[i].events);
			if (e)
				return e;
			if (loop->exit)
				break;
		}
	}

	return loop->error;
}

void
loop_exit (struct loop *loop)
{
	loop->exit = 1;
}
