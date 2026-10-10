/** @file Per-thread log rings, and the logger thread that writes them to
 *  stderr (log.h). */
#define _GNU_SOURCE /* program_invocation_short_name, pthread_setname_np() */

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

enum {
	RING      = 256 << 10,  /* bytes a thread's ring holds: a power of two */
	RINGS_MAX = 16,
	LINE      = 1 << 10,    /* the longest line; the rest is cut */
	BURST     = 32,         /* lines a call site may log a second */
};

/* The logger's state. */
enum { RUNNING, STOPPING, DONE };

/* The owner's half and the logger's half each have a cache line. */
struct ring {
	alignas(64) _Atomic size_t head;  /* bytes put, by the thread it is for */
	unsigned char             *data;
	alignas(64) _Atomic size_t tail;  /* bytes written out, by the logger */
	size_t                     seen;  /* the head that its batch took */
};

static struct {
	struct ring      *rings;
	size_t            n;
	_Atomic size_t    attached;
	int               wake_fd;
	int               notify_fd;
	_Atomic uint32_t  asleep;
	_Atomic uint32_t  state;
} lg = { .wake_fd = -1, .notify_fd = -1 };

static thread_local struct ring *self;

/* For a thread that found no ring left: always full, so its lines are dropped
 * and counted, and it never blocks on stderr. */
static struct ring full = { .head = RING };

struct log_metrics log_metrics;
enum log_level log_threshold = LOG_INFO;

/* Whether the site may log a line now; if so, @p *missed is how many it held
 * back since its last.  Racing threads may miscount by a line or two. */
static bool
site_allow (struct log_site *site,
            uint32_t        *missed)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	uint64_t now = (uint64_t)ts.tv_sec;
	uint64_t second = atomic_load_explicit(&site->second, memory_order_relaxed);
	if (second != now &&
	    atomic_compare_exchange_strong_explicit(&site->second, &second, now,
	                                            memory_order_relaxed,
	                                            memory_order_relaxed))
		atomic_store_explicit(&site->count, 0, memory_order_relaxed);
	if (atomic_fetch_add_explicit(&site->count, 1, memory_order_relaxed) >= BURST) {
		atomic_fetch_add_explicit(&site->missed, 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&log_metrics.suppressed, 1, memory_order_relaxed);
		return false;
	}
	*missed = atomic_exchange_explicit(&site->missed, 0, memory_order_relaxed);
	return true;
}

/* Put a whole line into the calling thread's ring, or drop it if it does not
 * fit; without a ring, write it out. */
static void
emit (char const *line,
      size_t      len)
{
	struct ring *r = self;
	if (!r) {
		ssize_t n = write(STDERR_FILENO, line, len);
		(void)n;
		return;
	}
	size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
	if (RING - (head - atomic_load_explicit(&r->tail, memory_order_acquire)) < len) {
		atomic_fetch_add_explicit(&log_metrics.dropped, 1, memory_order_relaxed);
		return;
	}
	size_t at = head % RING;
	size_t first = len < RING - at ? len : RING - at;
	memcpy(r->data + at, line, first);
	memcpy(r->data, line + first, len - first);
	/* Sequentially consistent with the look at asleep: either the logger
	 * sees this line before it sleeps, or this sees it asleep. */
	atomic_store(&r->head, head + len);
	atomic_fetch_add_explicit(&log_metrics.lines, 1, memory_order_relaxed);
	if (atomic_load(&lg.asleep) && atomic_exchange(&lg.asleep, 0))
		(void)eventfd_write(lg.wake_fd, 1);
}

/* Length @p len of a line after a vsnprintf() into it returned @p n. */
static size_t
grown (size_t len,
       int    n)
{
	return n < 0 ? len : len + (size_t)n < LINE - 1 ? len + (size_t)n : LINE - 1;
}

void
log_put (struct log_site *site,
         char const      *fmt,
         ...)
{
	uint32_t missed = 0;
	if (site && !site_allow(site, &missed))
		return;

	/* The last byte is kept for the line break. */
	char line[LINE];
	size_t len = grown(0, snprintf(line, LINE - 1, "%s: ",
	                               program_invocation_short_name));
	va_list ap;
	va_start(ap, fmt);
	len = grown(len, vsnprintf(line + len, LINE - 1 - len, fmt, ap));
	va_end(ap);
	if (missed)
		len = grown(len, snprintf(line + len, LINE - 1 - len,
		                          " (%" PRIu32 " more held back)", missed));
	if (len == LINE - 1)
		memcpy(line + len - 3, "...", 3);
	line[len++] = '\n';
	emit(line, len);
}

/* Write all @p n buffers at @p iov to stderr, waiting as long as it takes for
 * room; on an error, drop the rest. */
static void
write_all (struct iovec *iov,
           size_t        n)
{
	while (n) {
		ssize_t w = writev(STDERR_FILENO, iov, (int)n);
		if (w < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				(void)poll(&(struct pollfd){ .fd = STDERR_FILENO,
				                             .events = POLLOUT }, 1, -1);
			else if (errno != EINTR)
				return;
			continue;
		}
		size_t left = (size_t)w;
		for (; n && left >= iov->iov_len; ++iov, --n)
			left -= iov->iov_len;
		if (n) {
			iov->iov_base = (char *)iov->iov_base + left;
			iov->iov_len -= left;
		}
	}
}

/* Whether the logger has something to do: lines, or the order to stop. */
static bool
pending (void)
{
	if (atomic_load(&lg.state) != RUNNING)
		return true;
	for (size_t i = 0; i < lg.n; ++i)
		if (atomic_load(&lg.rings[i].head) !=
		    atomic_load_explicit(&lg.rings[i].tail, memory_order_relaxed))
			return true;
	return false;
}

static void *
logger (void *arg)
{
	(void)arg;
	(void)pthread_setname_np(pthread_self(), "logger");
	uint64_t reported = 0;
	for (;;) {
		struct iovec iov[2 * RINGS_MAX + 1];
		size_t k = 0;
		char note[160];
		uint64_t dropped = atomic_load_explicit(&log_metrics.dropped,
		                                        memory_order_relaxed);
		if (dropped != reported) {
			int n = snprintf(note, sizeof note, "%s: log: %" PRIu64 " lines "
			                 "dropped so far\n", program_invocation_short_name,
			                 dropped);
			if (n > 0 && (size_t)n < sizeof note)
				iov[k++] = (struct iovec){ note, (size_t)n };
			reported = dropped;
		}
		for (size_t i = 0; i < lg.n; ++i) {
			struct ring *r = &lg.rings[i];
			size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
			r->seen = atomic_load_explicit(&r->head, memory_order_acquire);
			size_t len = r->seen - tail;
			size_t at = tail % RING;
			size_t first = len < RING - at ? len : RING - at;
			if (first)
				iov[k++] = (struct iovec){ r->data + at, first };
			if (len > first)
				iov[k++] = (struct iovec){ r->data, len - first };
		}
		if (k) {
			write_all(iov, k);
			for (size_t i = 0; i < lg.n; ++i)
				atomic_store_explicit(&lg.rings[i].tail, lg.rings[i].seen,
				                      memory_order_release);
			continue;
		}
		if (atomic_load(&lg.state) != RUNNING)
			break;

		/* Say so before the last look, so that a line put after it, or
		 * the order to stop, wakes this. */
		atomic_store(&lg.asleep, 1);
		if (!pending()) {
			eventfd_t v;
			(void)poll(&(struct pollfd){ .fd = lg.wake_fd, .events = POLLIN }, 1, -1);
			(void)eventfd_read(lg.wake_fd, &v);
		}
		atomic_store_explicit(&lg.asleep, 0, memory_order_relaxed);
	}
	/* Once done, the logger may be freed: it touches nothing after but
	 * notify_fd, which its owner keeps open. */
	int notify_fd = lg.notify_fd;
	atomic_store(&lg.state, DONE);
	(void)eventfd_write(notify_fd, 1);
	return nullptr;
}

int
log_start (size_t rings,
           int    notify_fd)
{
	if (!rings || rings > RINGS_MAX)
		return EINVAL;
	lg.rings = aligned_alloc(alignof(struct ring), rings * sizeof *lg.rings);
	if (!lg.rings)
		return ENOMEM;
	memset(lg.rings, 0, rings * sizeof *lg.rings);
	int e = 0;
	for (size_t i = 0; !e && i < rings; ++i, ++lg.n)
		e = (lg.rings[i].data = malloc(RING)) ? 0 : ENOMEM;
	if (!e && (lg.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0)
		e = errno;
	lg.notify_fd = notify_fd;
	pthread_attr_t attr;
	if (!e && !(e = pthread_attr_init(&attr))) {
		(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_t thread;
		e = pthread_create(&thread, &attr, logger, nullptr);
		pthread_attr_destroy(&attr);
	}
	if (e) {
		for (size_t i = 0; i < lg.n; ++i)
			free(lg.rings[i].data);
		free(lg.rings);
		if (lg.wake_fd >= 0)
			close(lg.wake_fd);
		lg.rings = nullptr;
		lg.n = 0;
		lg.wake_fd = -1;
	}
	return e;
}

void
log_attach (void)
{
	size_t i = atomic_fetch_add(&lg.attached, 1);
	self = i < lg.n ? &lg.rings[i] : &full;
}

void
log_stop (void)
{
	uint32_t running = RUNNING;
	if (!lg.rings || !atomic_compare_exchange_strong(&lg.state, &running, STOPPING))
		return;
	if (atomic_exchange(&lg.asleep, 0))
		(void)eventfd_write(lg.wake_fd, 1);
}

bool
log_done (void)
{
	return !lg.rings || atomic_load(&lg.state) == DONE;
}

void
log_fini (void)
{
	if (!lg.rings)
		return;
	for (size_t i = 0; i < lg.n; ++i)
		free(lg.rings[i].data);
	free(lg.rings);
	close(lg.wake_fd);
	lg.rings = nullptr;
	lg.n = 0;
	lg.wake_fd = -1;
	atomic_store(&lg.attached, 0);
	atomic_store(&lg.state, RUNNING);
	self = nullptr;
}
