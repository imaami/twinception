/** @file child.c
 *
 * Child processes.
 */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE /* pipe2(), memfd_create(), POSIX_SPAWN_SETSID */
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/pidfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "child.h"
#include "log.h"
#include "util.h"

/* The children spawned and not yet reaped, of every loop. */
static _Atomic uint32_t alive;

/* Signal the child's group with @p sig, unless the child is reaped, and
 * stop reading its output.
 */
static void
child_signal (struct child *c,
              int           sig)
{
	if (c->pid > 0)
		(void)kill(-c->pid, sig);
	loop_close(c->loop, &c->pipe);
}

/* Stop the child: a child with a grace with SIGTERM, which lets it clean
 * up (git removes its lock files), and any other with SIGKILL.
 */
static void
child_kill (struct child *c)
{
	child_signal(c, c->grace ? SIGTERM : SIGKILL);
}

/* Reap the child if it has exited, and set its pid to 0. After the reap,
 * the kernel can reuse the id, so no signal goes to the group of the child
 * after this call. A child that has not exited yet stays unreaped until
 * askai exits. Apart from child_exited() and the error paths of
 * child_create(), only this function decrements alive.
 */
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
	loop_close(c->loop, &c->proc);
	child_reap(c);
	buf_fini(&c->out);
	free(c);
}

/* Read as much of the child's output as the step has the share for, the rest
 * in its next step, up to the cap and a byte more, which says the output
 * goes on past it. Its end, or that byte, ends the reading.
 */
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
			c->end.flags |= CHILD_OUT;
			loop_ready_bulk(loop, &c->step);
			return;
		}
		struct span s;
		if (buf_reserve(&c->out, want, c->max + 1, &s)) {
			c->end.flags |= CHILD_CUT;
			child_kill(c);
			return;
		}
		size_t got;
		int e = util_read_all(c->pipe.fd, s.ptr, want, -1, &got);
		buf_commit(&c->out, got);
		loop_work(loop, LOOP_RECV, got);
		if (len + got > c->max) {
			buf_cut(&c->out, c->max, len + got - c->max);
			c->end.flags |= CHILD_CUT;
			child_kill(c);
			return;
		}
		/* EAGAIN waits for more; the end or another error ends the output. */
		if (e == EAGAIN)
			return;
		if (e || got < want) {
			loop_close(c->loop, &c->pipe);
			return;
		}
	}
}

/* If the child has exited, store how it ended, and close its pidfd, which
 * stays readable after the exit. With WNOWAIT, waitid() does not reap the
 * child. The kernel does not reuse the id of a zombie, and the id of the
 * child is also the id of its group. So until child_reap(), a kill still
 * reaches a process of the group that has the output pipe open after the
 * child has exited. If waitid() fails, the child was reaped elsewhere, and
 * the kernel can already have reused its id for another process. So this
 * function then sets the pid to 0 without a call of waitpid().
 */
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
	c->end.code = info.si_code;
	c->end.status = info.si_status;
	loop_close(c->loop, &c->proc);
}

/* Its deadline has come: SIGTERM, and SIGKILL once its grace has passed too,
 * or SIGKILL at once without one.
 */
static void
child_due (struct child *c)
{
	bool term = c->grace && !(c->end.flags & CHILD_LATE) && c->pid > 0;
	c->end.flags |= CHILD_LATE;
	if (!term) {
		child_signal(c, SIGKILL);
		return;
	}
	/* Its grace starts on the timer, which stays in the heap. */
	(void)kill(-c->pid, SIGTERM);
	(void)loop_timer_set(c->loop, &c->timer, c->loop->now + c->grace);
}

/* Once the child is reaped and its output is all in, hand it to its owner,
 * or free it if the owner has let go.
 */
static void
child_step (struct loop      *loop,
            struct loop_step *step)
{
	struct child *c = container_of(step, struct child, step);
	uint32_t events = c->end.flags & CHILD_EVENTS;
	c->end.flags &= ~CHILD_EVENTS;
	if (events & CHILD_DUE)
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
	c->end.flags |= ref == &c->proc ? CHILD_EXIT : CHILD_OUT;
	loop_ready_bulk(loop, &c->step);
}

static void
child_late (struct loop       *loop,
            struct loop_timer *timer)
{
	struct child *c = container_of(timer, struct child, timer);
	c->end.flags |= CHILD_DUE;
	loop_ready_bulk(loop, &c->step);
}

/* Have @p fa give the child @p fd as its descriptor @p to, or /dev/null,
 * opened with @p oflag, if @p fd is -1.
 */
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
 * @p flags, else /dev/null; the descriptor it keeps, and no other.
 */
static int
child_fds_actions (struct child_fds const     *fds,
                   posix_spawn_file_actions_t *fa,
                   uint64_t                    flags)
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
 * faults' and ignores SIGPIPE, which a child would inherit.
 */
static int
child_attr (posix_spawnattr_t *attr)
{
	sigset_t none;
	sigset_t all;
	sigemptyset(&none);
	sigfillset(&all);
	int e = posix_spawnattr_setsigmask(attr, &none);
	if (!e)
		e = posix_spawnattr_setsigdefault(attr, &all);
	return e ? e : posix_spawnattr_setflags(attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK |
	                                              POSIX_SPAWN_SETSIGDEF);
}

/* Spawn a process that runs cmd->file with cmd->argv and cmd->envp, set up
 * its descriptors from @p fds, and store its pidfd in @p *pidfd.
 */
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
	e = posix_spawnattr_init(&attr);
	if (!e) {
		e = child_fds_actions(fds, &fa, cmd->flags);
		if (!e)
			e = child_attr(&attr);
		/* POSIX declares the vectors without const. */
		if (!e)
			e = pidfd_spawnp(pidfd, cmd->file, &fa, &attr, (char *const *)cmd->argv,
			                 (char *const *)cmd->envp);
		posix_spawnattr_destroy(&attr);
	}
	posix_spawn_file_actions_destroy(&fa);
	return e;
}

/* A file in memory that holds the bytes of @p in, read from its start, in
 * @p *fd; 0 or an error.
 */
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
	if (e)
		util_close(fd);
	return e;
}

/* Kill and reap @p pidfd's process, which could not be watched, at once;
 * returns @p e.
 */
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
 * reap it at once.
 */
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
 * @p *out.
 */
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

/* Spawn @p c's process as @p cmd says, reading its output and timing it;
 * on an error, its pipe and timer may be left for child_dispose().
 */
static int
child_start (struct child           *c,
             struct child_cmd const *cmd)
{
	struct child_fds fds = child_fds();
	int e = cmd->in.len ? child_input(cmd->in, &fds.in) : 0;
	if (!e && cmd->deadline)
		e = loop_timer_set(c->loop, &c->timer, cmd->deadline);
	if (!e && !(e = child_pipe(c, &fds.out))) {
		int pidfd = -1;
		e = child_cmd_exec(cmd, &fds, &pidfd);
		util_close(&fds.out);
		if (!e)
			e = child_watch(c, pidfd);
	}
	util_close(&fds.in);
	return e;
}

/* Wait for @p pidfd's process, which runs @p cmd, to exit: at its deadline,
 * SIGTERM, and SIGKILL once its grace has passed too, or at once without
 * one. Returns 0, ETIMEDOUT if the deadline came, or why the wait failed,
 * which kills the process.
 */
static int
child_await (struct child_cmd const *cmd,
             int                     pidfd)
{
	struct pollfd watch = { .fd = pidfd, .events = POLLIN };
	uint64_t deadline = cmd->deadline;
	uint64_t grace = cmd->grace;
	int e = 0;
	for (;;) {
		int timeout = -1;
		if (deadline) {
			uint64_t now = loop_clock();
			uint64_t ms = now < deadline ? (deadline - now + MS - 1) / MS : 0;
			timeout = ms < INT_MAX ? (int)ms : INT_MAX;
		}
		int ready = poll(&watch, 1, timeout);
		if (ready > 0)
			return e;
		if (ready < 0 && errno == EINTR)
			continue;
		/* At the first timeout, send SIGTERM if the command has a grace. */
		if (!ready && grace) {
			(void)pidfd_send_signal(pidfd, SIGTERM, nullptr, 0);
			deadline = loop_clock() + grace;
			grace = 0;
			e = ETIMEDOUT;
			continue;
		}
		/* errno is read before pidfd_send_signal() can change it. */
		int why = e ? e : ready ? errno : ETIMEDOUT;
		(void)pidfd_send_signal(pidfd, SIGKILL, nullptr, 0);
		return why;
	}
}

/* Runs @p cmd to the end on the calling thread with the descriptors
 * @p fds, and writes how the child ended to @p *end. The comment of
 * child_capture() in src/child.h describes the spawn, the deadline and
 * @p *end. The output goes to fds.out, and the function does not read it.
 * Returns 0 if the child exited before the deadline, ETIMEDOUT if the
 * deadline came, EIO if a signal killed the child before the deadline, or
 * the error that stopped the spawn or the wait.
 */
static int
child_run (struct child_cmd const *cmd,
           struct child_fds        fds,
           struct child_end       *end)
{
	*end = child_end();
	int own = -1;
	if (fds.in < 0 && cmd->in.len) {
		int e = child_input(cmd->in, &own);
		if (e)
			return e;
		fds.in = own;
	}
	int pidfd = -1;
	int e = child_cmd_exec(cmd, &fds, &pidfd);
	util_close(&own);
	if (e)
		return e;
	int const waited = child_await(cmd, pidfd);
	siginfo_t info = {};
	int failed;
	do {
		failed = waitid(P_PIDFD, (id_t)pidfd, &info, WEXITED);
	} while (failed && errno == EINTR);
	e = waited ? waited : failed ? errno : info.si_code != CLD_EXITED ? EIO : 0;
	close(pidfd);
	/* info stays zero if waitid() fails, so end->code is 0 then. */
	*end = child_end(
		.code   = info.si_code,
		.status = info.si_status,
		.flags  = waited == ETIMEDOUT ? CHILD_LATE : 0
	);
	return e;
}

int
child_capture (struct child_cmd const *cmd,
               struct child_fds        fds,
               struct buf             *out,
               struct child_end       *end)
{
	*end = child_end();
	fds.out = memfd_create("askai-output", MFD_CLOEXEC);
	if (fds.out < 0)
		return errno;
	/* After the read, out contains at most max readable bytes. */
	size_t const had = buf_peek(out).len;
	size_t const max = cmd->max < SIZE_MAX - had ? had + cmd->max : SIZE_MAX;
	int e = child_run(cmd, fds, end);
	/* The child wrote through a copy of fds.out, and the copy shares the
	 * file offset. So the read starts after a seek to offset 0. */
	if (!e && lseek(fds.out, 0, SEEK_SET) < 0)
		e = errno;
	if (!e)
		e = buf_read(out, fds.out, 0, max);
	/* buf_read() reads one byte past max to detect a longer output, and
	 * then returns ENOBUFS. */
	if (e == ENOBUFS) {
		buf_cut(out, max, buf_peek(out).len - max);
		end->flags |= CHILD_CUT;
		e = EFBIG;
	}
	close(fds.out);
	return e;
}

int
child_cmd_run (struct child_cmd const *cmd,
               struct child_fds        fds,
               struct buf             *out,
               struct child_end       *end)
{
	int e;
	if (out) {
		buf_clear(out);
		e = child_capture(cmd, fds, out, end);
	} else {
		e = child_run(cmd, fds, end);
	}
	return e ? e : end->status ? EIO : 0;
}

int
child_argv (char const               **argv,
            size_t                     cap,
            char const *const *const  *parts,
            size_t                     n)
{
	if (!cap)
		return E2BIG;
	size_t k = 0;
	for (size_t i = 0; i < n; ++i) {
		for (char const *const *w = parts[i]; w && *w; ++w) {
			if (k + 1 >= cap)
				return E2BIG;
			argv[k++] = *w;
		}
	}
	argv[k] = nullptr;
	return 0;
}

struct cstr_view
child_end_why (struct child_end const *end,
               char const             *name,
               uint64_t                timeout,
               size_t                  max,
               char                   *buf,
               size_t                  size)
{
	if (end->flags & CHILD_LATE)
		return util_format_view(buf, size, "%s took longer than %" PRIu64 " s, and was "
		                        "stopped.", name, timeout / NS);
	if (end->flags & CHILD_CUT)
		return util_format_view(buf, size, "%s wrote more than %zu MiB.", name, max >> 20);
	switch (end->code) {
	case CLD_EXITED:
		return end->status ? util_format_view(buf, size, "%s exited with status %d.", name,
		                                      end->status)
		                   : (struct cstr_view){};
	case 0:
		return util_format_view(buf, size, "%s ended, and why is unknown.", name);
	default:
		return util_format_view(buf, size, "%s was killed by signal %d.", name, end->status);
	}
}

struct cstr_view
child_spawn_why (int         e,
                 char const *name,
                 char       *buf,
                 size_t      size)
{
	if (e == EBUSY)
		return CSTR_VIEW("Too many commands are running; try again in a few seconds.");
	struct cstr_view const why = util_format_view(buf, size, "Cannot run %s: %s.", name,
	                                              strerror(e));
	LOG(LOG_WARN, "%s", buf);
	return why;
}

struct child *
child_create (struct loop            *loop,
              struct list            *list,
              struct child_cmd const *cmd,
              void                  (*done) (struct child *c),
              void                   *owner,
              int                    *e)
{
	bool const room = atomic_fetch_add_explicit(&alive, 1, memory_order_relaxed) < CHILD_MAX;
	/* A spawn costs far more than an allocation, so children are not kept
	 * for reuse; their output's storage goes back to the stash. */
	struct child *c = room ? malloc(sizeof *c) : nullptr;
	*e = !room ? EBUSY : !c ? ENOMEM : 0;
	if (!*e) {
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
		list_append(list, &c->hook);
		if (!(*e = child_start(c, cmd)))
			return c;
		/* c->pid is still 0, so child_reap() does not decrement alive. */
		child_dispose(c);
	}
	atomic_fetch_sub_explicit(&alive, 1, memory_order_relaxed);
	return nullptr;
}

void
child_destroy (struct child **p_c)
{
	struct child *c = p_c ? *p_c : nullptr;
	child_release(p_c);
	if (!c)
		return;
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

int
child_std_open (void)
{
	for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
		if (fcntl(fd, F_GETFD) >= 0)
			continue;
		/* The lowest free number is fd, as those below it are open. It is
		 * open for the other direction than its use, so that a read of 0
		 * or a write of 1 or 2 still fails with EBADF, as if closed. */
		int got = open("/dev/null", fd == STDIN_FILENO ? O_WRONLY : O_RDONLY);
		if (got < 0)
			return errno;
		if (got != fd) {
			close(got);
			return EBADF;
		}
	}
	return 0;
}
