/** @file cli_io.c
 *
 * Bounded blocking work using the existing blocker pool.
 */
#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "cli_io.h"
#include "compat.h"

enum { CLI_IO_JOBS = 32 };

struct cli_io {
	struct blocker  *blocker;
	struct cli_loop *ui;
	struct loop_ref  notify;   /**< the eventfd that each blocker thread
	                                writes 1 to as it exits */
	size_t           pending;
	uint64_t         stopped;
	uint32_t         exited;
	uint32_t         started;
};

static void
cli_io_notify (struct loop     *loop,
               struct loop_ref *ref,
               uint32_t         events)
{
	(void)events;
	struct cli_io *io = container_of(ref, struct cli_io, notify);
	uint64_t count;
	loop_fail(loop, cli_loop_ref_counter(ref, &count));
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
cli_io_create (struct cli_loop *ui,
               int             *e)
{
	struct cli_io *io = calloc(1, sizeof *io);
	if (!io) {
		*e = ENOMEM;
		return nullptr;
	}
	io->ui = ui;
	io->notify.fd = -1;
	int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	*e = fd < 0 ? errno : 0;
	if (!*e)
		*e = loop_watch(&ui->loop, &io->notify, fd, EPOLLIN, cli_io_notify, nullptr);
	if (*e) {
		if (fd >= 0)
			(void)close(fd);
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
	job->blocker = (struct blocker_job){
		.done = {.fn = cli_io_complete}, .work = cli_io_work, .loop = &io->ui->loop
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
	/* blocker_done() precedes each worker's final write. A normal start
	 * accounts for both writes above. After a partial start the opaque
	 * blocker does not expose how many workers exist: retain that fd
	 * until process exit, as blocker_create() requires. */
	if (io->started || notify_unused)
		cli_loop_close(io->ui, &io->notify);
	else
		loop_unwatch(&io->ui->loop, &io->notify);
	free(io);
	*p_io = nullptr;
}
