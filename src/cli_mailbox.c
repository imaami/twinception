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
cli_mailbox_wake (struct cli_loop     *loop,
                   struct cli_loop_ref *ref,
                   uint32_t             events)
{
	(void)events;
	struct cli_mailbox *mailbox = container_of(ref, struct cli_mailbox, wake);
	(void)cli_loop_counter(loop, ref);
	cli_loop_ready(loop, &mailbox->drain);
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
		list_del(&msg->hook);
		--mailbox->messages[msg->control];
		mailbox->bytes[msg->control] -= msg->bytes;
		pthread_mutex_unlock(&mailbox->mutex);
		msg->fn(mailbox, msg);
	}
	int e = pthread_mutex_lock(&mailbox->mutex);
	if (e)
		return e;
	bool pending = !list_is_empty(&mailbox->mail);
	pthread_mutex_unlock(&mailbox->mutex);
	if (pending)
		cli_loop_ready(mailbox->loop, &mailbox->drain);
	return 0;
}

static void
cli_mailbox_drain (struct cli_loop      *loop,
                    struct cli_loop_step *step)
{
	struct cli_mailbox *mailbox = container_of(step, struct cli_mailbox, drain);
	int e = cli_mailbox_dispatch(mailbox, 32);
	if (e && !loop->error)
		loop->error = e;
}

int
cli_mailbox_init (struct cli_mailbox       *mailbox,
                   struct cli_loop          *loop,
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
	e = cli_loop_watch(loop, &mailbox->wake, fd, EPOLLIN, cli_mailbox_wake);
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
	size_t count_limit = control ? mailbox->limits.control_messages : mailbox->limits.messages;
	size_t bytes_limit = control ? mailbox->limits.control_bytes : mailbox->limits.bytes;
	if (mailbox->closed)
		e = ESHUTDOWN;
	else if (mailbox->messages[control] >= count_limit ||
	         msg->bytes > bytes_limit - mailbox->bytes[control])
		e = EAGAIN;
	else {
		bool empty = list_is_empty(&mailbox->mail);
		msg->control = control;
		list_append(&mailbox->mail, &msg->hook);
		++mailbox->messages[control];
		mailbox->bytes[control] += msg->bytes;
		if (empty) {
			uint64_t value = 1;
			ssize_t n;
			do {
				n = write(mailbox->wake.fd, &value, sizeof(value));
			} while (n < 0 && errno == EINTR);
			if (n < 0 && errno != EAGAIN)
				e = errno;
			else if (n >= 0 && n != (ssize_t)sizeof(value))
				e = EIO;
			if (e) {
				list_del(&msg->hook);
				--mailbox->messages[control];
				mailbox->bytes[control] -= msg->bytes;
			}
		}
	}
	pthread_mutex_unlock(&mailbox->mutex);
	return e;
}

void
cli_mailbox_close (struct cli_mailbox *mailbox)
{
	if (!mailbox->mutex_ready)
		return;
	int e = pthread_mutex_lock(&mailbox->mutex);
	if (e) {
		if (!mailbox->loop->error)
			mailbox->loop->error = e;
		return;
	}
	mailbox->closed = 1;
	pthread_mutex_unlock(&mailbox->mutex);
}

void
cli_mailbox_fini (struct cli_mailbox *mailbox)
{
	if (!mailbox->mutex_ready)
		return;
	cli_mailbox_close(mailbox);
	int e = cli_mailbox_dispatch(mailbox, SIZE_MAX);
	if (e && !mailbox->loop->error)
		mailbox->loop->error = e;
	cli_loop_unready(&mailbox->drain);
	cli_loop_close(mailbox->loop, &mailbox->wake);
	e = pthread_mutex_destroy(&mailbox->mutex);
	if (e && !mailbox->loop->error)
		mailbox->loop->error = e;
	mailbox->mutex_ready = 0;
}
