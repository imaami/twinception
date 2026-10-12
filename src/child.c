/** @file Child processes (child.h). */
#define _GNU_SOURCE /* pipe2(), memfd_create(), POSIX_SPAWN_SETSID */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/pidfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "child.h"
#include "util.h"

/* What a child's next step is to look at. */
enum {
	CHILD_OUT  = 1 << 0,  /* its output can be read, or has ended */
	CHILD_EXIT = 1 << 1,  /* it has exited */
	CHILD_LATE = 1 << 2,  /* its deadline has come */
};

/* The children spawned and not yet reaped, of every loop. */
static _Atomic uint32_t alive;

/* Stop reading the child's output. */
static void
child_close (struct child *c)
{
	int fd = c->pipe.fd;
	if (fd < 0)
		return;
	loop_unwatch(c->loop, &c->pipe);
	close(fd);
}

/* Kill the child's group, unless the child is reaped, and stop reading its
 * output. */
static void
child_kill (struct child *c)
{
	if (c->proc.fd >= 0)
		(void)kill(-c->pid, SIGKILL);
	child_close(c);
}

static void
child_free (struct child *c)
{
	list_del(&c->hook);
	loop_unready(&c->step);
	loop_timer_stop(c->loop, &c->timer);
	child_kill(c);
	int fd = c->proc.fd;
	if (fd >= 0) {
		loop_unwatch(c->loop, &c->proc);
		close(fd);
		atomic_fetch_sub_explicit(&alive, 1, memory_order_relaxed);
	}
	buf_free(&c->out);
	free(c);
}

/* Read as much of the child's output as the step has the share for, the rest
 * in its next step, up to the cap and a byte more, which says the output
 * goes on past it.  Its end, or that byte, ends the reading. */
static void
child_read (struct child *c)
{
	struct loop *loop = c->loop;
	for (;;) {
		size_t len = buf_peek(&c->out).len;
		size_t room = loop_room(loop, LOOP_RECV);
		size_t want = c->max + 1 - len < room ? c->max + 1 - len : room;
		if (!want) {
			c->events |= CHILD_OUT;
			loop_ready_bulk(loop, &c->step);
			return;
		}
		struct span s = buf_reserve(&c->out, want, c->max + 1);
		if (!s.len) {
			c->cut = true;
			child_kill(c);
			return;
		}
		ssize_t n = read(c->pipe.fd, s.ptr, want);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return;
		if (n <= 0) {
			child_close(c);
			return;
		}
		buf_commit(&c->out, (size_t)n);
		loop_work(loop, LOOP_RECV, (uint64_t)n);
		if (len + (size_t)n > c->max) {
			buf_cut(&c->out, c->max, len + (size_t)n - c->max);
			c->cut = true;
			child_kill(c);
			return;
		}
	}
}

/* Reap the child, if it has exited; should waitid() fail, it has gone all
 * the same, how unknown. */
static void
child_reap (struct child *c)
{
	siginfo_t info = {};
	int fd = c->proc.fd;
	int e = waitid(P_PIDFD, (id_t)fd, &info, WEXITED | WNOHANG) ? errno : 0;
	if (!e && !info.si_pid)
		return;
	c->code = e ? 0 : info.si_code;
	c->status = e ? 0 : info.si_status;
	loop_unwatch(c->loop, &c->proc);
	close(fd);
	atomic_fetch_sub_explicit(&alive, 1, memory_order_relaxed);
}

/* Once the child is reaped and its output is all in, hand it to its owner,
 * or free it if the owner has let go. */
static void
child_step (struct loop      *loop,
            struct loop_step *step)
{
	struct child *c = container_of(step, struct child, step);
	uint16_t events = c->events;
	c->events = 0;
	if (events & CHILD_LATE && c->grace && !c->late && c->proc.fd >= 0) {
		/* Its grace starts on the timer, which stays in the heap. */
		c->late = true;
		(void)kill(-c->pid, SIGTERM);
		(void)loop_timer_set(loop, &c->timer, loop->now + c->grace);
	} else if (events & CHILD_LATE) {
		c->late = true;
		child_kill(c);
	}
	if (events & CHILD_OUT && c->pipe.fd >= 0)
		child_read(c);
	if (events & CHILD_EXIT && c->proc.fd >= 0)
		child_reap(c);
	if (c->proc.fd >= 0 || c->pipe.fd >= 0)
		return;
	loop_timer_stop(loop, &c->timer);
	if (c->done)
		c->done(c);
	else
		child_free(c);
}

static void
child_event (struct loop     *loop,
             struct loop_ref *ref,
             uint32_t         events)
{
	(void)events;
	struct child *c = ref->owner;
	c->events |= ref == &c->proc ? CHILD_EXIT : CHILD_OUT;
	loop_ready_bulk(loop, &c->step);
}

static void
child_late (struct loop       *loop,
            struct loop_timer *timer)
{
	struct child *c = container_of(timer, struct child, timer);
	c->events |= CHILD_LATE;
	loop_ready_bulk(loop, &c->step);
}

/* Spawn @p file, its input @p in if not -1, its output, and its error with
 * CHILD_STDERR in @p flags, the pipe end @p out, into @p pidfd. */
static int
spawn (int        *pidfd,
       int         in,
       int         out,
       uint32_t    flags,
       char const *file,
       char *const argv[],
       char *const envp[])
{
	sigset_t none;
	sigset_t all;
	sigemptyset(&none);
	sigfillset(&all);
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t attr;
	int e = posix_spawn_file_actions_init(&fa);
	if (e)
		return e;
	e = posix_spawnattr_init(&attr);
	if (!e) {
		e = posix_spawn_file_actions_adddup2(&fa, out, STDOUT_FILENO);
		if (!e)
			e = in >= 0 ? posix_spawn_file_actions_adddup2(&fa, in, STDIN_FILENO)
			  : posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null",
			                                     O_RDONLY, 0);
		if (!e)
			e = flags & CHILD_STDERR
			  ? posix_spawn_file_actions_adddup2(&fa, out, STDERR_FILENO)
			  : posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null",
			                                     O_WRONLY, 0);
		if (!e)
			e = posix_spawn_file_actions_addclosefrom_np(&fa, STDERR_FILENO + 1);
		/* askai blocks every signal but the faults' and ignores SIGPIPE,
		 * which a child would inherit. */
		if (!e)
			e = posix_spawnattr_setsigmask(&attr, &none);
		if (!e)
			e = posix_spawnattr_setsigdefault(&attr, &all);
		if (!e)
			e = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID |
			                                    POSIX_SPAWN_SETSIGMASK |
			                                    POSIX_SPAWN_SETSIGDEF);
		if (!e)
			e = pidfd_spawnp(pidfd, file, &fa, &attr, argv, envp);
		posix_spawnattr_destroy(&attr);
	}
	posix_spawn_file_actions_destroy(&fa);
	return e;
}

/* A file in memory that holds the @p len bytes at @p data, read from its
 * start; -1, with errno, if that cannot be. */
static int
input (unsigned char const *data,
       size_t               len)
{
	int fd = memfd_create("askai-input", MFD_CLOEXEC);
	for (size_t done = 0; fd >= 0 && done < len; ) {
		ssize_t n = write(fd, data + done, len - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			int e = n ? errno : EIO;
			close(fd);
			errno = e;
			return -1;
		}
		done += (size_t)n;
	}
	if (fd >= 0 && lseek(fd, 0, SEEK_SET)) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	return fd;
}

int
child_spawn (struct child **p_child,
             struct loop   *loop,
             struct list   *list,
             char const    *file,
             char *const    argv[],
             char *const    envp[],
             struct span    in,
             size_t         max,
             uint64_t       deadline,
             uint64_t       grace,
             uint32_t       flags,
             void         (*done) (struct child *c),
             void          *owner)
{
	if (atomic_fetch_add_explicit(&alive, 1, memory_order_relaxed) >= CHILD_MAX) {
		atomic_fetch_sub_explicit(&alive, 1, memory_order_relaxed);
		return EBUSY;
	}
	/* A spawn costs far more than an allocation, so children are not kept
	 * for reuse; their output's storage goes back to the stash. */
	struct child *c = malloc(sizeof *c);
	if (!c) {
		atomic_fetch_sub_explicit(&alive, 1, memory_order_relaxed);
		return ENOMEM;
	}
	*c = (struct child){
		.proc.fd = -1,
		.pipe.fd = -1,
		.step.fn = child_step,
		.timer.fn = child_late,
		.loop = loop,
		.done = done,
		.owner = owner,
		.max = max,
		.grace = grace
	};
	int fds[2] = { -1, -1 };
	int pidfd = -1;
	int in_fd = in.len ? input(in.ptr, in.len) : -1;
	int e = in.len && in_fd < 0 ? errno : pipe2(fds, O_CLOEXEC) ? errno : 0;
	/* Only the read end: the child writes its output as it would any pipe. */
	if (!e && fcntl(fds[0], F_SETFL, O_NONBLOCK))
		e = errno;
	if (!e)
		e = loop_watch(loop, &c->pipe, fds[0], EPOLLIN, child_event, c);
	if (!e && deadline)
		e = loop_timer_set(loop, &c->timer, deadline);
	if (!e)
		e = spawn(&pidfd, in_fd, fds[1], flags, file, argv, envp);
	if (fds[1] >= 0)
		close(fds[1]);
	if (in_fd >= 0)
		close(in_fd);
	if (!e) {
		c->pid = pidfd_getpid(pidfd);
		e = c->pid <= 0 ? ESRCH : loop_watch(loop, &c->proc, pidfd, EPOLLIN,
		                                     child_event, c);
		if (e) {
			/* Unwatched, it can only be reaped here, at once. */
			if (c->pid > 0)
				(void)kill(-c->pid, SIGKILL);
			else
				(void)pidfd_send_signal(pidfd, SIGKILL, nullptr, 0);
			siginfo_t info;
			(void)waitid(P_PIDFD, (id_t)pidfd, &info, WEXITED);
			close(pidfd);
		}
	}
	if (e) {
		loop_timer_stop(loop, &c->timer);
		loop_unwatch(loop, &c->pipe);
		if (fds[0] >= 0)
			close(fds[0]);
		free(c);
		atomic_fetch_sub_explicit(&alive, 1, memory_order_relaxed);
		return e;
	}
	list_append(list, &c->hook);
	*p_child = c;
	return 0;
}

void
child_drop (struct child *c)
{
	c->done = nullptr;
	child_kill(c);
	if (c->proc.fd < 0) {
		child_free(c);
		return;
	}
	loop_timer_stop(c->loop, &c->timer);
	buf_free(&c->out);
}

void
child_free_all (struct list *list)
{
	while (!list_is_empty(list))
		child_free(container_of(list->next, struct child, hook));
}
