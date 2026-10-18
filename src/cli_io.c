/** @file Bounded blocking work using the existing blocker pool. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "cli_io.h"
#include "compat.h"

enum { CLI_IO_JOBS = 32 };

struct cli_io {
	pthread_mutex_t     mutex;
	struct list         completed;
	struct blocker     *blocker;
	struct loop        *network;
	struct cli_mailbox *mailbox;
	struct cli_loop_ref notify;
	size_t              pending;
	uint64_t            stopped;
	uint32_t            exited;
	uint32_t            started;
};

static void
cli_io_result (struct cli_mailbox     *mailbox,
               struct cli_mailbox_msg *message);

static void
cli_io_notify (struct cli_loop     *loop,
               struct cli_loop_ref *ref,
               uint32_t             events)
{
	(void)events;
	struct cli_io *io = container_of(ref, struct cli_io, notify);
	uint64_t count;
	int e;
	do {
		e = eventfd_read(ref->fd, &count) ? errno : 0;
	} while (e == EINTR);
	if (!e)
		io->exited += (uint32_t)count;
	else if (e != EAGAIN && !loop->error)
		loop->error = e;
	struct list completed;
	list_init(&completed);
	pthread_mutex_lock(&io->mutex);
	list_transplant(&completed, &io->completed);
	pthread_mutex_unlock(&io->mutex);
	while (!list_is_empty(&completed)) {
		struct cli_mailbox_msg *message = container_of(completed.next, struct cli_mailbox_msg, hook);
		list_del(&message->hook);
		cli_io_result(io->mailbox, message);
	}
}

static void
cli_io_result (struct cli_mailbox     *mailbox,
               struct cli_mailbox_msg *message)
{
	(void)mailbox;
	struct cli_io_job *job = container_of(message, struct cli_io_job, result);
	--job->io->pending;
	job->done(job);
}

static void
cli_io_complete (struct loop *loop,
                 struct msg  *message)
{
	(void)loop;
	struct blocker_job *blocker = container_of(message, struct blocker_job, done);
	struct cli_io_job *job = container_of(blocker, struct cli_io_job, blocker);
	if (blocker->error)
		job->error = blocker->error;
	int e = cli_mailbox_post(job->io->mailbox, &job->result, true);
	if (!e)
		return;
	/* Admission already bounds this fallback queue to CLI_IO_JOBS. Even
	 * a full or closed shared mailbox must return every accepted job. */
	struct cli_io *io = job->io;
	pthread_mutex_lock(&io->mutex);
	list_append(&io->completed, &job->result.hook);
	/* Blockers write one on exit. Completion wakes occupy the high word,
	 * so the receiver can distinguish them without another descriptor. */
	do {
		e = eventfd_write(io->notify.fd, UINT64_C(1) << 32) ? errno : 0;
	} while (e == EINTR);
	/* EAGAIN means the descriptor is already readable. Other failures
	 * leave the job owned by completed for shutdown, never abandoned. */
	pthread_mutex_unlock(&io->mutex);
}

static void
cli_io_work (struct blocker_job *blocker)
{
	struct cli_io_job *job = container_of(blocker, struct cli_io_job, blocker);
	job->error = job->work(job);
}

struct cli_io *
cli_io_create (struct loop        *network,
               struct cli_mailbox *mailbox,
               int                *e)
{
	struct cli_io *io = calloc(1, sizeof *io);
	if (!io) {
		*e = ENOMEM;
		return nullptr;
	}
	io->network = network;
	io->mailbox = mailbox;
	io->notify.fd = -1;
	list_init(&io->completed);
	*e = pthread_mutex_init(&io->mutex, nullptr);
	if (*e) {
		free(io);
		io = nullptr;
		return nullptr;
	}
	int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	*e = fd < 0 ? errno : 0;
	if (!*e)
		*e = cli_loop_watch(mailbox->loop, &io->notify, fd, EPOLLIN, cli_io_notify);
	if (*e) {
		if (fd >= 0)
			(void)close(fd);
		pthread_mutex_destroy(&io->mutex);
		free(io);
		io = nullptr;
		return nullptr;
	}
	io->blocker = blocker_create(fd, e);
	if (!*e)
		*e = blocker_start(io->blocker);
	if (*e)
		cli_io_stop(io);
	else
		io->started = BLOCKER_THREADS;
	/* Even a partial start owns threads; the caller must drain them. */
	return io;
}

int
cli_io_submit (struct cli_io     *io,
               struct cli_io_job *job)
{
	if (!io || !job || !job->work || !job->done)
		return EINVAL;
	if (io->stopped)
		return ESHUTDOWN;
	if (io->pending == CLI_IO_JOBS)
		return EAGAIN;
	job->io = io;
	job->error = 0;
	job->blocker = (struct blocker_job){
		.done = {.fn = cli_io_complete}, .work = cli_io_work, .loop = io->network
	};
	job->result = (struct cli_mailbox_msg){.fn = cli_io_result, .bytes = sizeof job->result};
	int e = blocker_submit(io->blocker, &job->blocker);
	if (!e)
		++io->pending;
	return e;
}

void
cli_io_stop (struct cli_io *io)
{
	if (io) {
		io->stopped = true;
		blocker_stop(io->blocker);
	}
}

bool
cli_io_done (struct cli_io const *io)
{
	return !io || (!io->pending && blocker_done(io->blocker) && io->exited >= io->started);
}

void
cli_io_destroy (struct cli_io **p_io)
{
	if (!p_io || !*p_io || !cli_io_done(*p_io))
		return;
	struct cli_io *io = *p_io;
	bool notify_unused = !io->blocker;
	blocker_destroy(&io->blocker);
	int fd = io->notify.fd;
	cli_loop_unwatch(io->mailbox->loop, &io->notify);
	/* blocker_done() precedes each worker's final write. A normal start
	 * accounts for both writes above. After a partial start the opaque
	 * blocker does not expose how many workers exist: retain that fd
	 * until process exit, as blocker_create() requires. */
	if (fd >= 0 && (io->started || notify_unused))
		(void)close(fd);
	io->notify.fd = -1;
	pthread_mutex_destroy(&io->mutex);
	free(io);
	*p_io = nullptr;
}
