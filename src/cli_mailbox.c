/** @file cli_mailbox.c
 *
 * Object-owned CLI mailbox implementation.
 */
#include <errno.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "cli_mailbox.h"
#include "compat.h"

static void
cli_mailbox_wake (struct loop     *loop,
                  struct loop_ref *ref,
                  uint32_t         events)
{
	(void)events;
	struct cli_mailbox *mailbox = container_of(ref, struct cli_mailbox, wake);
	eventfd_t count;
	if (eventfd_read(ref->fd, &count) && errno != EAGAIN)
		loop_fail(loop, errno);
	loop_ready(loop, &mailbox->drain);
}

int
cli_mailbox_dispatch (struct cli_mailbox *mailbox,
                      size_t              limit)
{
	for (size_t count = 0; count < limit; ++count) {
		int e = pthread_mutex_lock(&mailbox->mutex);
		if (e)
			return e;
		if (list_is_empty(&mailbox->mail)) {
			pthread_mutex_unlock(&mailbox->mutex);
			return 0;
		}
		struct cli_mailbox_msg *msg = container_of(mailbox->mail.next, struct cli_mailbox_msg, hook);
		struct cli_mailbox_budget *used = &mailbox->used[msg->control];
		list_del(&msg->hook);
		--used->messages;
		used->bytes -= msg->bytes;
		pthread_mutex_unlock(&mailbox->mutex);
		msg->fn(mailbox, msg);
	}
	int e = pthread_mutex_lock(&mailbox->mutex);
	if (e)
		return e;
	bool pending = !list_is_empty(&mailbox->mail);
	pthread_mutex_unlock(&mailbox->mutex);
	if (pending)
		loop_ready(mailbox->loop, &mailbox->drain);
	return 0;
}

static void
cli_mailbox_drain (struct loop      *loop,
                   struct loop_step *step)
{
	struct cli_mailbox *mailbox = container_of(step, struct cli_mailbox, drain);
	loop_fail(loop, cli_mailbox_dispatch(mailbox, 32));
}

int
cli_mailbox_init (struct cli_mailbox       *mailbox,
                  struct loop              *loop,
                  struct cli_mailbox_limits limits)
{
	*mailbox = (struct cli_mailbox){
		.wake.fd = -1,
		.drain.fn = cli_mailbox_drain,
		.loop = loop,
		.limits = limits,
	};
	list_init(&mailbox->mail);
	int e = pthread_mutex_init(&mailbox->mutex, nullptr);
	if (e)
		return e;
	mailbox->mutex_ready = 1;
	int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (fd < 0) {
		e = errno;
		cli_mailbox_fini(mailbox);
		return e;
	}
	e = loop_watch(loop, &mailbox->wake, fd, EPOLLIN, cli_mailbox_wake, nullptr);
	if (e) {
		close(fd);
		cli_mailbox_fini(mailbox);
	}
	return e;
}

int
cli_mailbox_post (struct cli_mailbox     *mailbox,
                  struct cli_mailbox_msg *msg,
                  bool                    control)
{
	if (!msg || !msg->fn)
		return EINVAL;
	int e = pthread_mutex_lock(&mailbox->mutex);
	if (e)
		return e;
	struct cli_mailbox_budget const *limit = &mailbox->limits.budget[control];
	struct cli_mailbox_budget *used = &mailbox->used[control];
	if (mailbox->closed)
		e = ESHUTDOWN;
	else if (used->messages >= limit->messages || msg->bytes > limit->bytes - used->bytes)
		e = EAGAIN;
	else if (list_is_empty(&mailbox->mail) && eventfd_write(mailbox->wake.fd, 1) && errno != EAGAIN)
		e = errno;
	if (!e) {
		msg->control = control;
		list_append(&mailbox->mail, &msg->hook);
		++used->messages;
		used->bytes += msg->bytes;
	}
	pthread_mutex_unlock(&mailbox->mutex);
	return e;
}

int
cli_mailbox_close (struct cli_mailbox *mailbox)
{
	if (!mailbox->mutex_ready)
		return 0;
	int e = pthread_mutex_lock(&mailbox->mutex);
	if (e)
		return e;
	mailbox->closed = 1;
	pthread_mutex_unlock(&mailbox->mutex);
	return 0;
}

void
cli_mailbox_fini (struct cli_mailbox *mailbox)
{
	if (!mailbox->mutex_ready)
		return;
	struct loop *loop = mailbox->loop;
	loop_fail(loop, cli_mailbox_close(mailbox));
	loop_fail(loop, cli_mailbox_dispatch(mailbox, SIZE_MAX));
	loop_unready(&mailbox->drain);
	loop_close(loop, &mailbox->wake);
	loop_fail(loop, pthread_mutex_destroy(&mailbox->mutex));
	mailbox->mutex_ready = 0;
}
