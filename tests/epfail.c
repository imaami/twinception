/**
 * @file LD_PRELOAD shim for tests/askai.py: epoll_ctl(EPOLL_CTL_ADD) fails with
 * ENOMEM for every TCP socket bound to a port other than that of the first one
 * watched, which is askai's listener. So askai cannot watch the connections
 * it makes upstream, while it can watch those of its clients. With
 * EPFAIL_ACCEPT set, every other accept4() fails with EPROTO, as when Linux
 * passes on a pending connection's network error. With EPFAIL_CLEANUP_MS
 * set, curl_multi_cleanup() takes that many milliseconds longer, as a
 * teardown may on a busy machine. With EPFAIL_EPOLL set to N, the Nth
 * epoll_create1() fails with EMFILE, as when an event loop cannot start.
 * With EPFAIL_TOKENER set, json-c cannot make a tokener, as when memory
 * runs out.
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

/* Store the address of the next definition of @p name after this library
 * (RTLD_NEXT) in the @p size bytes of the function pointer at @p real.
 * POSIX requires the same representation for void * and function
 * pointers, so the memcpy() works. ISO C does not require this.
 */
static void
epfail_next (void       *real,
             size_t      size,
             char const *name)
{
	void *sym = dlsym(RTLD_NEXT, name);
	memcpy(real, &sym, size);
}

int
accept4 (int              fd,
         struct sockaddr *addr,
         socklen_t       *len,
         int              flags)
{
	static int (*real) (int, struct sockaddr *, socklen_t *, int);
	static int fail;
	if (!real)
		epfail_next(&real, sizeof real, "accept4");
	if (getenv("EPFAIL_ACCEPT") && (fail = !fail)) {
		errno = EPROTO;
		return -1;
	}
	return real(fd, addr, len, flags);
}

int
epoll_create1 (int flags)
{
	static int (*real) (int);
	static long calls;
	if (!real)
		epfail_next(&real, sizeof real, "epoll_create1");
	char const *n = getenv("EPFAIL_EPOLL");
	if (n && ++calls == atol(n)) {
		errno = EMFILE;
		return -1;
	}
	return real(flags);
}

int
epoll_ctl (int                 epfd,
           int                 op,
           int                 fd,
           struct epoll_event *event)
{
	static int (*real) (int, int, int, struct epoll_event *);
	static in_port_t listener;
	if (!real)
		epfail_next(&real, sizeof real, "epoll_ctl");

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
	if (!real)
		epfail_next(&real, sizeof real, "curl_multi_cleanup");
	char const *ms = getenv("EPFAIL_CLEANUP_MS");
	if (ms) {
		long n = atol(ms);
		struct timespec ts = { n / 1000, n % 1000 * 1000000 };
		while (nanosleep(&ts, &ts) && errno == EINTR)
			;
	}
	return real(multi);
}

/* Wrappers of json-c's tokener constructors. struct json_tokener is
 * declared but not defined here, because the wrappers only pass pointers
 * to it.
 */
struct json_tokener;

struct json_tokener *
json_tokener_new_ex (int depth)
{
	static struct json_tokener *(*real) (int);
	if (getenv("EPFAIL_TOKENER"))
		return nullptr;
	if (!real)
		epfail_next(&real, sizeof real, "json_tokener_new_ex");
	return real(depth);
}

struct json_tokener *
json_tokener_new (void)
{
	static struct json_tokener *(*real) (void);
	if (getenv("EPFAIL_TOKENER"))
		return nullptr;
	if (!real)
		epfail_next(&real, sizeof real, "json_tokener_new");
	return real();
}
