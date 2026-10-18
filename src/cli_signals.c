/** @file Signal delivery without asynchronous handlers or shared flags. */
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "cli_signals.h"
#include "compat.h"

static void
cli_signals_read (struct cli_loop     *loop,
                  struct cli_loop_ref *ref,
                  uint32_t             events)
{
	(void)events;
	struct cli_signals *signals = container_of(ref, struct cli_signals, ref);
	for (unsigned i = 0; i < 64; ++i) {
		struct signalfd_siginfo info;
		ssize_t n = read(ref->fd, &info, sizeof info);
		if (n == (ssize_t)sizeof info) {
			signals->fn(signals, info.ssi_signo);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return;
		loop->error = n < 0 ? errno : EIO;
		return;
	}
}

int
cli_signals_init (struct cli_signals *signals,
                  struct cli_loop    *loop,
                  cli_signals_cb     *fn)
{
	*signals = (struct cli_signals){.ref.fd = -1, .loop = loop, .fn = fn};
	sigset_t mask;
	if (sigemptyset(&mask))
		return errno;
	int const numbers[] = {SIGINT, SIGTERM, SIGHUP, SIGWINCH, SIGTSTP, SIGCONT};
	for (size_t i = 0; i < sizeof numbers / sizeof *numbers; ++i)
		if (sigaddset(&mask, numbers[i]))
			return errno;
	int e = pthread_sigmask(SIG_BLOCK, &mask, &signals->previous);
	if (e)
		return e;
	signals->masked = true;
	struct sigaction ignore = {.sa_handler = SIG_IGN};
	if (sigemptyset(&ignore.sa_mask) || sigaction(SIGPIPE, &ignore, &signals->pipe))
		return errno;
	signals->ignored = true;
	int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	if (fd < 0)
		return errno;
	e = cli_loop_watch(loop, &signals->ref, fd, EPOLLIN, cli_signals_read);
	if (e)
		(void)close(fd);
	return e;
}

void
cli_signals_fini (struct cli_signals *signals)
{
	int fd = signals->ref.fd;
	if (signals->loop)
		cli_loop_unwatch(signals->loop, &signals->ref);
	if (fd >= 0)
		(void)close(fd);
	signals->ref.fd = -1;
	if (signals->ignored)
		(void)sigaction(SIGPIPE, &signals->pipe, nullptr);
	if (signals->masked)
		(void)pthread_sigmask(SIG_SETMASK, &signals->previous, nullptr);
	signals->ignored = false;
	signals->masked = false;
}
