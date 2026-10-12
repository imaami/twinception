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
	buf_fini(&c->out);
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
		size_t left = c->max + 1 - len;
		size_t room = loop_room(loop, LOOP_RECV);
		size_t want = left < room ? left : room;
		if (!want) {
			c->events |= CHILD_OUT;
			loop_ready_bulk(loop, &c->step);
			return;
		}
		struct span s;
		if (buf_reserve(&c->out, want, c->max + 1, &s)) {
			c->cut = true;
			child_kill(c);
			return;
		}
		ssize_t n = read(c->pipe.fd, s.ptr, want);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			/* EAGAIN waits for more; anything else ends the output. */
			if (!n || errno != EAGAIN)
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
	if (!waitid(P_PIDFD, (id_t)fd, &info, WEXITED | WNOHANG)) {
		if (!info.si_pid)
			return;
		c->code = info.si_code;
		c->status = info.si_status;
	}
	loop_unwatch(c->loop, &c->proc);
	close(fd);
	atomic_fetch_sub_explicit(&alive, 1, memory_order_relaxed);
}

/* Its deadline has come: SIGTERM, and SIGKILL once its grace has passed too,
 * or SIGKILL at once without one. */
static void
child_due (struct child *c)
{
	bool term = c->grace && !c->late && c->proc.fd >= 0;
	c->late = true;
	if (!term) {
		child_kill(c);
		return;
	}
	/* Its grace starts on the timer, which stays in the heap. */
	(void)kill(-c->pid, SIGTERM);
	(void)loop_timer_set(c->loop, &c->timer, c->loop->now + c->grace);
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
	if (events & CHILD_LATE)
		child_due(c);
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

/* Have @p fa give the child @p fd as its descriptor @p to, or /dev/null,
 * opened with @p oflag, if @p fd is -1. */
static int
child_fd (posix_spawn_file_actions_t *fa,
          int                         fd,
          int                         to,
          int                         oflag)
{
	return fd >= 0 ? posix_spawn_file_actions_adddup2(fa, fd, to)
	               : posix_spawn_file_actions_addopen(fa, to, "/dev/null", oflag, 0);
}

/* Have @p fa give the child @p out as its output, @p in, or /dev/null, as
 * its input, and @p out as its error too with CHILD_STDERR in @p flags, else
 * /dev/null, and no other descriptor. */
static int
child_fds (posix_spawn_file_actions_t *fa,
           int                         in,
           int                         out,
           uint32_t                    flags)
{
	int e = child_fd(fa, out, STDOUT_FILENO, O_WRONLY);
	if (e)
		return e;
	if ((e = child_fd(fa, in, STDIN_FILENO, O_RDONLY)))
		return e;
	if ((e = child_fd(fa, flags & CHILD_STDERR ? out : -1, STDERR_FILENO, O_WRONLY)))
		return e;
	return posix_spawn_file_actions_addclosefrom_np(fa, STDERR_FILENO + 1);
}

/* Have @p attr give the child a session of its own, an empty signal mask
 * and every signal's default action: askai blocks every signal but the
 * faults' and ignores SIGPIPE, which a child would inherit. */
static int
child_attr (posix_spawnattr_t *attr)
{
	sigset_t none;
	sigset_t all;
	sigemptyset(&none);
	sigfillset(&all);
	int e = posix_spawnattr_setsigmask(attr, &none);
	if (e)
		return e;
	if ((e = posix_spawnattr_setsigdefault(attr, &all)))
		return e;
	return posix_spawnattr_setflags(attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK |
	                                      POSIX_SPAWN_SETSIGDEF);
}

/* Spawn @p cmd's file into @p pidfd, its input @p in if not -1 and its
 * output the pipe end @p out, through @p fa and @p attr, fresh. */
static int
child_spawn (struct child_cmd const     *cmd,
             posix_spawn_file_actions_t *fa,
             posix_spawnattr_t          *attr,
             int                         in,
             int                         out,
             int                        *pidfd)
{
	int e = child_fds(fa, in, out, cmd->flags);
	if (e)
		return e;
	if ((e = child_attr(attr)))
		return e;
	/* POSIX declares the vectors without const. */
	return pidfd_spawnp(pidfd, cmd->file, fa, attr, (char *const *)cmd->argv,
	                    (char *const *)cmd->envp);
}

/* As child_spawn(), with file actions and attributes of its own. */
static int
child_exec (struct child_cmd const *cmd,
            int                     in,
            int                     out,
            int                    *pidfd)
{
	posix_spawn_file_actions_t fa;
	int e = posix_spawn_file_actions_init(&fa);
	if (e)
		return e;
	posix_spawnattr_t attr;
	if (!(e = posix_spawnattr_init(&attr))) {
		e = child_spawn(cmd, &fa, &attr, in, out, pidfd);
		posix_spawnattr_destroy(&attr);
	}
	posix_spawn_file_actions_destroy(&fa);
	return e;
}

/* A file in memory that holds the bytes of @p in, read from its start, in
 * @p *fd; 0 or an error. */
static int
child_input (struct span  in,
             int         *fd)
{
	*fd = memfd_create("askai-input", MFD_CLOEXEC);
	if (*fd < 0)
		return errno;
	int e = 0;
	for (size_t done = 0; !e && done < in.len; ) {
		ssize_t n = write(*fd, in.ptr + done, in.len - done);
		if (n > 0)
			done += (size_t)n;
		else if (!n)
			e = EIO;
		else if (errno != EINTR)
			e = errno;
	}
	if (!e && lseek(*fd, 0, SEEK_SET))
		e = errno;
	if (e) {
		close(*fd);
		*fd = -1;
	}
	return e;
}

/* Watch @p pidfd, @p c's process, which has run: if it cannot be, kill and
 * reap it at once. */
static int
child_watch (struct child *c,
             int           pidfd)
{
	c->pid = pidfd_getpid(pidfd);
	int e = c->pid > 0 ? loop_watch(c->loop, &c->proc, pidfd, EPOLLIN, child_event, c)
	                   : ESRCH;
	if (!e)
		return 0;
	if (c->pid > 0)
		(void)kill(-c->pid, SIGKILL);
	else
		(void)pidfd_send_signal(pidfd, SIGKILL, nullptr, 0);
	siginfo_t info;
	(void)waitid(P_PIDFD, (id_t)pidfd, &info, WEXITED);
	close(pidfd);
	return e;
}

/* A pipe for @p c's output: its read end watched, and the write end in
 * @p *out. */
static int
child_pipe (struct child *c,
            int          *out)
{
	int fds[2];
	if (pipe2(fds, O_CLOEXEC))
		return errno;
	/* Only the read end: the child writes its output as it would any pipe. */
	int e = fcntl(fds[0], F_SETFL, O_NONBLOCK) ? errno
	      : loop_watch(c->loop, &c->pipe, fds[0], EPOLLIN, child_event, c);
	if (e) {
		close(fds[0]);
		close(fds[1]);
		return e;
	}
	*out = fds[1];
	return 0;
}

/* Time @p c, and spawn its process as @p cmd says, its input @p in and its
 * output @p out. */
static int
child_run (struct child           *c,
           struct child_cmd const *cmd,
           int                     in,
           int                     out)
{
	int e = cmd->deadline ? loop_timer_set(c->loop, &c->timer, cmd->deadline) : 0;
	if (e)
		return e;
	int pidfd = -1;
	if ((e = child_exec(cmd, in, out, &pidfd)))
		return e;
	return child_watch(c, pidfd);
}

/* Spawn @p c's process as @p cmd says, reading its output and timing it;
 * on an error, its pipe and timer may be left for child_free(). */
static int
child_start (struct child           *c,
             struct child_cmd const *cmd)
{
	int in = -1;
	int e = cmd->in.len ? child_input(cmd->in, &in) : 0;
	if (e)
		return e;
	int out = -1;
	if (!(e = child_pipe(c, &out))) {
		e = child_run(c, cmd, in, out);
		close(out);
	}
	if (in >= 0)
		close(in);
	return e;
}

struct child *
child_create (struct loop            *loop,
              struct list            *list,
              struct child_cmd const *cmd,
              void                  (*done) (struct child *c),
              void                   *owner,
              int                    *e)
{
	if (atomic_fetch_add_explicit(&alive, 1, memory_order_relaxed) >= CHILD_MAX) {
		atomic_fetch_sub_explicit(&alive, 1, memory_order_relaxed);
		*e = EBUSY;
		return nullptr;
	}
	/* A spawn costs far more than an allocation, so children are not kept
	 * for reuse; their output's storage goes back to the stash. */
	struct child *c = malloc(sizeof *c);
	if (!c) {
		atomic_fetch_sub_explicit(&alive, 1, memory_order_relaxed);
		*e = ENOMEM;
		return nullptr;
	}
	*c = (struct child){
		.proc.fd = -1,
		.pipe.fd = -1,
		.step.fn = child_step,
		.timer.fn = child_late,
		.loop = loop,
		.done = done,
		.owner = owner,
		.max = cmd->max,
		.grace = cmd->grace
	};
	list_init(&c->hook);
	if ((*e = child_start(c, cmd))) {
		atomic_fetch_sub_explicit(&alive, 1, memory_order_relaxed);
		child_free(c);
		return nullptr;
	}
	list_append(list, &c->hook);
	return c;
}

void
child_destroy (struct child **p_c)
{
	struct child *c = *p_c;
	if (!c)
		return;
	*p_c = nullptr;
	c->done = nullptr;
	child_kill(c);
	if (c->proc.fd < 0) {
		child_free(c);
		return;
	}
	loop_timer_stop(c->loop, &c->timer);
	buf_fini(&c->out);
}

void
child_destroy_all (struct list *list)
{
	while (!list_is_empty(list))
		child_free(container_of(list->next, struct child, hook));
}
