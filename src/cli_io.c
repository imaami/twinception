/** @file cli_io.c
 *
 * Bounded blocking work using the existing blocker pool.
 */
#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "cli_io.h"
#include "compat.h"
#include "util.h"

enum { CLI_IO_JOBS = 32 };

struct cli_io {
	struct blocker *blocker;
	struct loop    *ui;
	struct loop_ref notify;   /**< the eventfd that each blocker thread
	                               writes 1 to as it exits */
	size_t          pending;
	uint64_t        stopped;
	uint32_t        exited;
	uint32_t        started;
};

static void
cli_io_notify (struct loop     *loop,
               struct loop_ref *ref,
               uint32_t         events)
{
	(void)events;
	struct cli_io *io = container_of(ref, struct cli_io, notify);
	eventfd_t count = 0;
	if (eventfd_read(ref->fd, &count) && errno != EAGAIN)
		loop_fail(loop, errno);
	io->exited += (uint32_t)count;
}

/* Runs on the UI loop, to which the blocker posts the job when it is done. */
static void
cli_io_complete (struct loop *loop,
                 struct msg  *message)
{
	(void)loop;
	struct blocker_job *blocker = container_of(message, struct blocker_job, done);
	struct cli_io_job *job = container_of(blocker, struct cli_io_job, blocker);
	--job->io->pending;
	job->done(job);
}

static void
cli_io_work (struct blocker_job *blocker)
{
	struct cli_io_job *job = container_of(blocker, struct cli_io_job, blocker);
	blocker->error = job->work(job);
}

struct cli_io *
cli_io_create (struct loop *ui,
               int         *e)
{
	struct cli_io *io = malloc(sizeof *io);
	if (!io) {
		*e = ENOMEM;
		return nullptr;
	}
	*io = (struct cli_io){
		.ui        = ui,
		.notify.fd = -1,
	};
	int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	*e = fd < 0 ? errno : loop_watch(ui, &io->notify, fd, EPOLLIN, cli_io_notify, nullptr);
	if (*e) {
		util_close(&fd);
		free(io);
		return nullptr;
	}
	io->blocker = blocker_create(fd, e);
	if (!*e)
		*e = blocker_start(io->blocker);
	if (*e)
		cli_io_stop(io);
	else
		io->started = BLOCKER_THREADS;
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
	job->blocker = (struct blocker_job){
		.done = {.fn = cli_io_complete}, .work = cli_io_work, .loop = io->ui
	};
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
	/* Each blocker thread increments the exit count that blocker_done()
	 * reads, and then writes the eventfd once. After a normal start,
	 * io->exited counts the writes of all threads, so the eventfd can be
	 * closed. Without a blocker, no thread exists, and the eventfd can be
	 * closed too. After a partial start, io->started is 0, because the
	 * blocker does not report how many threads started. A thread may still
	 * write the eventfd, so it stays open until the process exits, as
	 * blocker_create() requires. */
	if (io->started || notify_unused)
		loop_close(io->ui, &io->notify);
	else
		loop_unwatch(io->ui, &io->notify);
	free(io);
	*p_io = nullptr;
}
