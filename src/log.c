/** @file log.c
 * Logging.
 * @note The design of this entire thing is bizarre. When I mentioned
 *       ring buffers I didn't imagine it somehow turning into this.
 */
#define _GNU_SOURCE /* pthread_setname_np() */

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <unistd.h>

#include "clk.h"
#include "compat.h"
#include "hist.h"
#include "log.h"
#include "util.h"

enum {
	RING      = 256 << 10,  /* size of each thread's ring in bytes, a power
	                           of two */
	RINGS_MAX = 511,        /* largest number of rings, because one
	                           writev() gets up to two iovecs per ring and
	                           one for the dropped-lines note, and IOV_MAX
	                           is 1024 */
	LINE      = 1 << 10,    /* bytes in the buffer that a line is formatted
	                           in; longer lines are cut and end in "..." */
	BURST     = 32,         /* lines one call site may log per second */
};

/* The logger's state. */
enum { RUNNING, STOPPING, DONE };

/* A thread's ring buffer. data points to its RING bytes. The thread that
 * uses the ring writes head and the bytes, and the logger thread writes
 * tail and seen. alignas(64) puts head and data on one cache line, and
 * tail and seen on the next, so that a write by one of the two threads does
 * not invalidate the cache line that the other thread writes.
 */
struct log_ring {
	alignas(64) _Atomic size_t head;  /* bytes put in so far */
	unsigned char             *data;
	alignas(64) _Atomic size_t tail;  /* bytes written out or dropped */
	size_t                     seen;  /* head as read for the batch */
};

static struct {
	struct log_ring  *rings;
	size_t            n;
	_Atomic size_t    attached;
	int               wake_fd;
	int               notify_fd;
	_Atomic uint32_t  asleep;
	_Atomic uint32_t  state;
} lg = { .wake_fd = -1, .notify_fd = -1 };

/* The calling thread's ring. log_attach() sets it to one of lg.rings or
 * to &full, and log_fini() sets it to nullptr in the thread that calls
 * log_fini(). It is also nullptr in a thread that has not called
 * log_attach(). While it is nullptr, log_emit() writes the thread's lines
 * straight to stderr.
 */
static thread_local struct log_ring *self;

/* The ring of the threads that call log_attach() after all rings are taken.
 * Its head is RING bytes ahead of its tail, so the check for free space in
 * log_emit() always fails, and log_emit() drops each line and counts it in
 * log_metrics.dropped. These threads do not write to stderr themselves,
 * because a write to stderr could block them.
 */
static struct log_ring full = { .head = RING };

struct log_metrics log_metrics;
enum log_level log_threshold = LOG_INFO;

/* Returns true if @p site may log a line now, which is when it has logged
 * fewer than BURST lines in the current second. Then @p *missed is the
 * number of lines of the site that were suppressed since its last logged
 * line. When threads use the site at the same time around the start of a
 * new second, the count can be reset late or more than once, so the site
 * may log more or fewer than BURST lines in that second.
 */
static bool
log_site_allow (struct log_site *site,
                uint32_t        *missed)
{
	uint64_t now = (uint64_t)clk_monotonic_coarse().tv_sec;
	uint64_t second = atomic_load_explicit(&site->second, memory_order_relaxed);
	if (second != now &&
	    atomic_compare_exchange_strong_explicit(&site->second, &second, now,
	                                            memory_order_relaxed,
	                                            memory_order_relaxed))
		atomic_store_explicit(&site->count, 0, memory_order_relaxed);
	if (atomic_fetch_add_explicit(&site->count, 1, memory_order_relaxed) >= BURST) {
		atomic_fetch_add_explicit(&site->missed, 1, memory_order_relaxed);
		hist_count(&log_metrics.suppressed, 1);
		return false;
	}
	*missed = atomic_exchange_explicit(&site->missed, 0, memory_order_relaxed);
	return true;
}

/* Put a whole line into the calling thread's ring, or drop it if it does not
 * fit; without a ring, write it out.
 */
static void
log_emit (char const *line,
          size_t      len)
{
	struct log_ring *r = self;
	if (!r) {
		ssize_t n = write(STDERR_FILENO, line, len);
		(void)n;
		return;
	}
	size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
	if (RING - (head - atomic_load_explicit(&r->tail, memory_order_acquire)) < len) {
		hist_count(&log_metrics.dropped, 1);
		return;
	}
	size_t at = head % RING;
	size_t first = len < RING - at ? len : RING - at;
	memcpy(r->data + at, line, first);
	memcpy(r->data, line + first, len - first);
	/* The store to r->head and the load of lg.asleep after it are
	 * sequentially consistent, and so are the logger's store to
	 * lg.asleep and its loads of the heads in log_pending(). So either
	 * the logger's last check before it sleeps reads the new head, or
	 * this thread reads lg.asleep as 1 and wakes the logger. */
	atomic_store(&r->head, head + len);
	hist_count(&log_metrics.lines, 1);
	if (atomic_load(&lg.asleep) && atomic_exchange(&lg.asleep, 0))
		(void)eventfd_write(lg.wake_fd, 1);
}

void
log_put (struct log_site *site,
         char const      *fmt,
         ...)
{
	uint32_t missed = 0;
	if (site && !log_site_allow(site, &missed))
		return;

	/* The line break takes the place of the text's NUL. */
	char line[LINE];
	va_list ap;
	va_start(ap, fmt);
	size_t len = util_vformat(line, sizeof line, fmt, ap);
	va_end(ap);
	if (missed)
		len += util_format(line + len, sizeof line - len, " (%" PRIu32 " skipped)",
		                   missed);
	line[len++] = '\n';
	log_emit(line, len);
}

/* Write all @p n buffers at @p iov to stderr, waiting as long as it takes for
 * room; on an error, drop the rest.
 */
static void
log_write_all (struct iovec *iov,
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
log_pending (void)
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
log_thread (void *arg)
{
	(void)arg;
	(void)pthread_setname_np(pthread_self(), "logger");
	uint64_t reported = 0;
	for (;;) {
		struct iovec iov[2 * RINGS_MAX + 1];
		size_t k = 0;
		char note[160];
		uint64_t dropped = hist_get(&log_metrics.dropped);
		if (dropped != reported) {
			size_t n = util_format(note, sizeof note, "log: %" PRIu64
			                       " lines dropped so far\n", dropped);
			if (n)
				iov[k++] = (struct iovec){ note, n };
			reported = dropped;
		}
		for (size_t i = 0; i < lg.n; ++i) {
			struct log_ring *r = &lg.rings[i];
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
			log_write_all(iov, k);
			for (size_t i = 0; i < lg.n; ++i)
				atomic_store_explicit(&lg.rings[i].tail, lg.rings[i].seen,
				                      memory_order_release);
			continue;
		}
		if (atomic_load(&lg.state) != RUNNING)
			break;

		/* Set lg.asleep before the last check of the state and the
		 * heads in log_pending(). A thread that puts a line after
		 * that check reads lg.asleep as 1 and wakes the logger, and so
		 * does log_stop() if it changes the state after that check. */
		atomic_store(&lg.asleep, 1);
		if (!log_pending()) {
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

/* Make @p n rings, and the logger's eventfd. */
static int
log_alloc (size_t n)
{
	lg.rings = aligned_alloc(alignof(struct log_ring), n * sizeof *lg.rings);
	if (!lg.rings)
		return ENOMEM;
	for (; lg.n < n; ++lg.n) {
		struct log_ring *r = &lg.rings[lg.n];
		if (!(r->data = malloc(RING)))
			return ENOMEM;
		atomic_init(&r->head, 0);
		atomic_init(&r->tail, 0);
		r->seen = 0;
	}
	lg.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	return lg.wake_fd < 0 ? errno : 0;
}

int
log_start (size_t rings,
           int    notify_fd)
{
	if (!rings || rings > RINGS_MAX)
		return EINVAL;
	lg.notify_fd = notify_fd;
	int e = log_alloc(rings);
	pthread_t thread;
	if (e || (e = pthread_create(&thread, nullptr, log_thread, nullptr)))
		log_fini();
	else
		(void)pthread_detach(thread);
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
	for (size_t i = 0; i < lg.n; ++i) {
		free(lg.rings[i].data);
		lg.rings[i].data = nullptr;
	}
	free(lg.rings);
	lg.rings = nullptr;
	lg.n = 0;
	util_close(&lg.wake_fd);
	atomic_store(&lg.attached, 0);
	atomic_store(&lg.state, RUNNING);
	self = nullptr;
}
