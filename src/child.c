/** @file Child processes (child.h). */
#define _GNU_SOURCE /* pipe2(), memfd_create(), POSIX_SPAWN_SETSID */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
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

/* Stop watching @p ref, which is the pidfd or the output pipe of the child,
 * and close its descriptor. */
static void
child_close (struct child    *c,
             struct loop_ref *ref)
{
	int fd = ref->fd;
	if (fd < 0)
		return;
	loop_unwatch(c->loop, ref);
	close(fd);
}

/* Signal the child's group with @p sig, unless the child is reaped, and
 * stop reading its output. */
static void
child_signal (struct child *c,
              int           sig)
{
	if (c->pid > 0)
		(void)kill(-c->pid, sig);
	child_close(c, &c->pipe);
}

/* Stop the child: a child with a grace with SIGTERM, which lets it clean
 * up (git removes its lock files), and any other with SIGKILL. */
static void
child_kill (struct child *c)
{
	child_signal(c, c->grace ? SIGTERM : SIGKILL);
}

/* Reap the child if it has exited, and set its pid to 0.  After the reap,
 * the kernel can reuse the id, so no signal goes to the group of the child
 * after this call.  A child that has not exited yet stays unreaped until
 * askai exits.  Apart from child_exited() and the error paths of
 * child_create(), only this function decrements alive. */
static void
child_reap (struct child *c)
{
	if (c->pid <= 0)
		return;
	(void)waitpid(c->pid, nullptr, WNOHANG);
	c->pid = 0;
	atomic_fetch_sub_explicit(&alive, 1, memory_order_relaxed);
}

static void
child_dispose (struct child *c)
{
	list_del(&c->hook);
	loop_unready(&c->step);
	loop_timer_stop(c->loop, &c->timer);
	child_kill(c);
	child_close(c, &c->proc);
	child_reap(c);
	buf_fini(&c->out);
	free(c);
}

/* read() @p len bytes into @p buf, from @p fd, again after a signal: the
 * bytes, or a negative error. */
static ssize_t
child_pipe_read (int     fd,
                 void   *buf,
                 size_t  len)
{
	for (;;) {
		ssize_t n = read(fd, buf, len);
		if (n >= 0)
			return n;
		if (errno != EINTR)
			return -errno;
	}
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
		ssize_t n = child_pipe_read(c->pipe.fd, s.ptr, want);
		/* EAGAIN waits for more; anything else ends the output. */
		if (n == -EAGAIN)
			return;
		if (n <= 0) {
			child_close(c, &c->pipe);
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

/* If the child has exited, store how it ended, and close its pidfd, which
 * stays readable after the exit.  With WNOWAIT, waitid() does not reap the
 * child.  The kernel does not reuse the id of a zombie, and the id of the
 * child is also the id of its group.  So until child_reap(), a kill still
 * reaches a process of the group that has the output pipe open after the
 * child has exited.  If waitid() fails, the child was reaped elsewhere, and
 * the kernel can already have reused its id for another process.  So this
 * function then sets the pid to 0 without a call of waitpid(). */
static void
child_exited (struct child *c)
{
	siginfo_t info = {};
	if (waitid(P_PIDFD, (id_t)c->proc.fd, &info, WEXITED | WNOHANG | WNOWAIT)) {
		c->pid = 0;
		atomic_fetch_sub_explicit(&alive, 1, memory_order_relaxed);
	} else if (!info.si_pid) {
		return;
	}
	c->code = info.si_code;
	c->status = info.si_status;
	child_close(c, &c->proc);
}

/* Its deadline has come: SIGTERM, and SIGKILL once its grace has passed too,
 * or SIGKILL at once without one. */
static void
child_due (struct child *c)
{
	bool term = c->grace && !c->late && c->pid > 0;
	c->late = true;
	if (!term) {
		child_signal(c, SIGKILL);
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
		child_exited(c);
	if (c->proc.fd >= 0 || c->pipe.fd >= 0)
		return;
	child_reap(c);
	loop_timer_stop(loop, &c->timer);
	if (c->done)
		c->done(c);
	else
		child_dispose(c);
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

/* Have @p fa give the child the descriptors @p fds: its directory first, as
 * a later action may replace that descriptor; its output, its input, or
 * /dev/null for either, and its output as its error too with CHILD_STDERR in
 * @p flags, else /dev/null; the descriptor it keeps, and no other. */
static int
child_fds_actions (struct child_fds const     *fds,
                   posix_spawn_file_actions_t *fa,
                   uint32_t                    flags)
{
	int e = fds->dir < 0 ? 0 : posix_spawn_file_actions_addfchdir_np(fa, fds->dir);
	if (!e)
		e = child_fd(fa, fds->out, STDOUT_FILENO, O_WRONLY);
	if (!e)
		e = child_fd(fa, fds->in, STDIN_FILENO, O_RDONLY);
	if (!e)
		e = child_fd(fa, flags & CHILD_STDERR ? fds->out : -1, STDERR_FILENO, O_WRONLY);
	int first = STDERR_FILENO + 1;  /* the first descriptor it does not keep */
	if (!e && fds->keep >= 0)
		e = posix_spawn_file_actions_adddup2(fa, fds->keep, first++);
	return e ? e : posix_spawn_file_actions_addclosefrom_np(fa, first);
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

/* Spawn @p cmd's file into @p pidfd, with the descriptors @p fds, through
 * @p fa and @p attr, fresh. */
static int
child_cmd_spawn (struct child_cmd const     *cmd,
                 posix_spawn_file_actions_t *fa,
                 posix_spawnattr_t          *attr,
                 struct child_fds const     *fds,
                 int                        *pidfd)
{
	int e = child_fds_actions(fds, fa, cmd->flags);
	if (e)
		return e;
	if ((e = child_attr(attr)))
		return e;
	/* POSIX declares the vectors without const. */
	return pidfd_spawnp(pidfd, cmd->file, fa, attr, (char *const *)cmd->argv,
	                    (char *const *)cmd->envp);
}

/* As child_cmd_spawn(), with file actions and attributes of its own. */
static int
child_cmd_exec (struct child_cmd const *cmd,
                struct child_fds const *fds,
                int                    *pidfd)
{
	posix_spawn_file_actions_t fa;
	int e = posix_spawn_file_actions_init(&fa);
	if (e)
		return e;
	posix_spawnattr_t attr;
	if (!(e = posix_spawnattr_init(&attr))) {
		e = child_cmd_spawn(cmd, &fa, &attr, fds, pidfd);
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
	int e = util_write_all(*fd, in.ptr, in.len);
	if (!e && lseek(*fd, 0, SEEK_SET))
		e = errno;
	if (e) {
		close(*fd);
		*fd = -1;
	}
	return e;
}

/* Kill and reap @p pidfd's process, which could not be watched, at once;
 * returns @p e. */
static int
child_unwatched (int pidfd,
                 int e)
{
	siginfo_t info;
	(void)waitid(P_PIDFD, (id_t)pidfd, &info, WEXITED);
	close(pidfd);
	return e;
}

/* Watch @p pidfd, @p c's process, which has run: if it cannot be, kill and
 * reap it at once. */
static int
child_watch (struct child *c,
             int           pidfd)
{
	pid_t pid = pidfd_getpid(pidfd);
	if (pid <= 0) {
		(void)pidfd_send_signal(pidfd, SIGKILL, nullptr, 0);
		return child_unwatched(pidfd, ESRCH);
	}
	int e = loop_watch(c->loop, &c->proc, pidfd, EPOLLIN, child_event, c);
	if (e) {
		(void)kill(-pid, SIGKILL);
		return child_unwatched(pidfd, e);
	}
	c->pid = pid;
	return 0;
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

/* Time @p c, and spawn its process as @p cmd says, with the descriptors
 * @p fds. */
static int
child_launch (struct child           *c,
              struct child_cmd const *cmd,
              struct child_fds const *fds)
{
	int e = cmd->deadline ? loop_timer_set(c->loop, &c->timer, cmd->deadline) : 0;
	if (e)
		return e;
	int pidfd = -1;
	if ((e = child_cmd_exec(cmd, fds, &pidfd)))
		return e;
	return child_watch(c, pidfd);
}

/* Spawn @p c's process as @p cmd says, reading its output and timing it;
 * on an error, its pipe and timer may be left for child_dispose(). */
static int
child_start (struct child           *c,
             struct child_cmd const *cmd)
{
	struct child_fds fds = { .in = -1, .out = -1, .dir = -1, .keep = -1 };
	int e = cmd->in.len ? child_input(cmd->in, &fds.in) : 0;
	if (e)
		return e;
	if (!(e = child_pipe(c, &fds.out))) {
		e = child_launch(c, cmd, &fds);
		close(fds.out);
	}
	if (fds.in >= 0)
		close(fds.in);
	return e;
}

/* Wait for @p pidfd's process, which runs @p cmd, to exit: at its deadline,
 * SIGTERM, and SIGKILL once its grace has passed too, or at once without
 * one.  Returns 0, ETIMEDOUT if the deadline came, or why the wait failed,
 * which kills the process. */
static int
child_await (struct child_cmd const *cmd,
             int                     pidfd)
{
	struct pollfd watch = { .fd = pidfd, .events = POLLIN };
	uint64_t deadline = cmd->deadline;
	int e = 0;
	for (;;) {
		int timeout = -1;
		if (deadline) {
			uint64_t now = loop_clock();
			uint64_t ms = now < deadline ? (deadline - now + 999'999) / 1'000'000 : 0;
			timeout = ms < INT_MAX ? (int)ms : INT_MAX;
		}
		int ready = poll(&watch, 1, timeout);
		if (ready > 0)
			return e;
		if (ready < 0 && errno == EINTR)
			continue;
		bool term = !ready && !e && cmd->grace;
		if (!e)
			e = ready ? errno : ETIMEDOUT;
		(void)pidfd_send_signal(pidfd, term ? SIGTERM : SIGKILL, nullptr, 0);
		if (!term)
			return e;
		deadline = loop_clock() + cmd->grace;
	}
}

int
child_run (struct child_cmd const *cmd,
           struct child_fds        fds,
           int                    *status)
{
	int own = -1;
	int e = fds.in < 0 && cmd->in.len ? child_input(cmd->in, &own) : 0;
	if (own >= 0)
		fds.in = own;
	int pidfd = -1;
	if (!e)
		e = child_cmd_exec(cmd, &fds, &pidfd);
	if (own >= 0)
		close(own);
	if (e)
		return e;
	e = child_await(cmd, pidfd);
	siginfo_t info = {};
	while (waitid(P_PIDFD, (id_t)pidfd, &info, WEXITED))
		if (errno != EINTR) {
			e = e ? e : errno;
			break;
		}
	close(pidfd);
	if (!e && info.si_code != CLD_EXITED)
		e = EIO;
	if (!e)
		*status = info.si_status;
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
		child_dispose(c);
		return nullptr;
	}
	list_append(list, &c->hook);
	return c;
}

void
child_destroy (struct child **p_c)
{
	if (!p_c || !*p_c)
		return;
	struct child *c = *p_c;
	*p_c = nullptr;
	c->done = nullptr;
	if (c->proc.fd < 0) {
		child_dispose(c);
		return;
	}
	/* The deadline still ends a child that SIGTERM did not. */
	child_kill(c);
	if (!c->grace)
		loop_timer_stop(c->loop, &c->timer);
	buf_fini(&c->out);
}

void
child_release (struct child **p_c)
{
	if (!p_c || !*p_c)
		return;
	struct child *c = *p_c;
	*p_c = nullptr;
	c->done = nullptr;
	c->owner = nullptr;
}

void
child_destroy_all (struct list *list)
{
	while (!list_is_empty(list))
		child_dispose(container_of(list->next, struct child, hook));
}
