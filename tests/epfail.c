/**
 * @file LD_PRELOAD shim for tests/askai.py: epoll_ctl(EPOLL_CTL_ADD) fails with
 * ENOMEM for every TCP socket bound to a port other than that of the first one
 * watched, which is askai's listener.  So askai cannot watch the connections
 * it makes upstream, while it can watch those of its clients.  With
 * EPFAIL_ACCEPT set, every other accept4() fails with EPROTO, as when Linux
 * passes on a pending connection's network error.  With EPFAIL_CLEANUP_MS
 * set, curl_multi_cleanup() takes that many milliseconds longer, as a
 * teardown may on a busy machine.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>

int
accept4 (int              fd,
         struct sockaddr *addr,
         socklen_t       *len,
         int              flags)
{
	static int (*real) (int, struct sockaddr *, socklen_t *, int);
	static int fail;
	if (!real) {
		void *sym = dlsym(RTLD_NEXT, "accept4");
		memcpy(&real, &sym, sizeof real);
	}
	if (getenv("EPFAIL_ACCEPT") && (fail = !fail)) {
		errno = EPROTO;
		return -1;
	}
	return real(fd, addr, len, flags);
}

int
epoll_ctl (int                 epfd,
           int                 op,
           int                 fd,
           struct epoll_event *event)
{
	static int (*real) (int, int, int, struct epoll_event *);
	static in_port_t listener;
	if (!real) {
		/* POSIX makes function and object pointers alike; ISO C does not. */
		void *sym = dlsym(RTLD_NEXT, "epoll_ctl");
		memcpy(&real, &sym, sizeof real);
	}

	/* The port lies at the same offset in either address family. */
	struct sockaddr_in6 addr;
	socklen_t len = sizeof addr;
	if (op == EPOLL_CTL_ADD && !getsockname(fd, (struct sockaddr *)&addr, &len) &&
	    (addr.sin6_family == AF_INET || addr.sin6_family == AF_INET6)) {
		if (!listener) {
			listener = addr.sin6_port;
		} else if (addr.sin6_port != listener) {
			errno = ENOMEM;
			return -1;
		}
	}
	return real(epfd, op, fd, event);
}

/* libcurl's, its CURLM * taken as a void * and its CURLMcode as an int. */
int
curl_multi_cleanup (void *multi)
{
	static int (*real) (void *);
	if (!real) {
		void *sym = dlsym(RTLD_NEXT, "curl_multi_cleanup");
		memcpy(&real, &sym, sizeof real);
	}
	char const *ms = getenv("EPFAIL_CLEANUP_MS");
	if (ms) {
		long n = atol(ms);
		struct timespec ts = { n / 1000, n % 1000 * 1000000 };
		while (nanosleep(&ts, &ts) && errno == EINTR)
			;
	}
	return real(multi);
}
