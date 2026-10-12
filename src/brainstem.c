/** @file The brainstem: signals, threads, watchdog, resources, shutdown
 *  (brainstem.h). */
#define _GNU_SOURCE /* CPU_COUNT(), getdents64(), ppoll(), pthread_setname_np(),
                       sched_getaffinity(), sigorset() */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

#include "blocker.h"
#include "brainstem.h"
#include "buf.h"
#include "log.h"
#include "loop.h"
#include "util.h"

static constexpr uint64_t MS = 1'000'000;

/* The loops' threads start in the order of their loops' numbers: home
 * first, as the client loops post jobs and connections to it. */
enum { HOME };

enum {
	START_MS  = 10'000,   /* for a loop to start */
	STUCK_MS  = 1'000,    /* one turn this long while draining: stuck */
	JOIN_MS   = 2'000,    /* for the blockers to finish what they do */
	FLUSH_MS  = 1'000,    /* for the logger to write what it holds */
	SAMPLE_MS = 1'000,    /* between resource samples */
	IDLE_MS   = 100,      /* between the watchdog's looks, with no threshold */
	FD_TABLE  = 65'536,   /* descriptors the table is grown to at the start */
	STACK     = 1 << 20,  /* a loop thread's stack: N of them count against
	                         RLIMIT_DATA, where the default, RLIMIT_STACK's
	                         8 MiB, would not fit many */
};

/* A loop thread's state; JOINED too before it starts, and if it cannot. */
enum { JOINED, STARTING, RUNNING, EXITED };

/* Orders to a loop: the bits of one message (design section 2). */
enum {
	DRAIN = 1, STATS = 2, STALL = 4, RELIEVE = 8, ALONE = 16, OPEN = 32,
	RELOAD = 64
};

/* Resources found over their threshold, as bits: descriptors, then PSI. */
enum { HIGH_FDS = 1, HIGH_PSI = 2 };

/* Where cgroup v2 is mounted. */
static char const cgroup_root[] = "/sys/fs/cgroup";

/*
 * Where each input's memory pressure level rises to high and to critical,
 * and where it falls back below each: the input's fullness, in hundredths of
 * a percent of what it is measured against (design section 5).
 */
static struct {
	uint32_t up[2];
	uint32_t down[2];
} const marks[PRESSURE_INPUTS] = {
	[PRESSURE_RESIDENT] = { { 10'000, 20'000 }, { 7'500, 15'000 } },  /* of memory_high */
	[PRESSURE_SYSTEM]   = { {  9'000,  9'500 }, { 8'500,  9'000 } },  /* of RAM */
	[PRESSURE_CGROUP]   = { {  9'000,  9'500 }, { 8'500,  9'000 } },  /* of the limit */
	[PRESSURE_PSI]      = { {  1'000,  4'000 }, {   500,  2'000 } },  /* of the time */
	[PRESSURE_TEST]     = { {      1,      2 }, {     1,      2 } },  /* the level */
};

struct brainstem;

/* A loop's thread, which the brainstem starts, watches, orders and stops. */
struct brainstem_thread {
	struct loop                 loop;
	struct msg                  order;    /* to the loop, with orders */
	struct brainstem           *bs;
	struct brainstem_app const *app;
	void                       *ctx;      /* the app's, for this loop */
	pthread_t                   thread;
	uint64_t                    flagged;  /* the turn the watchdog last
	                                         reported */
	_Atomic uint32_t            orders;
	_Atomic uint32_t            state;
	int                         error;    /* its own, set before state */
	uint16_t                    created;  /* bool: its loop needs loop_fini() */
	uint16_t                    drained;  /* bool: it was told to drain */
	char                        name[8];  /* the thread's: home, loop1 to
	                                         loop255 */
};

struct brainstem {
	uint64_t                    drain_by;  /* for the loops, set before DRAIN */
	uint64_t                    deadline;  /* of the grace period */
	uint64_t                    tick;      /* the watchdog's next look */
	uint64_t                    resumed;   /* the watchdog vouches for time
	                                          since then only */
	uint64_t                    sample;    /* when to sample resources next */
	uint64_t                    relief;    /* when the loops are to relieve
	                                          memory pressure again, if it
	                                          lasts */
	struct brainstem_thread    *threads;   /* home's, then the client loops' */
	struct brainstem_cfg const *cfg;
	struct blocker             *blockers;
	char                       *cgroup;    /* the process's cgroup v2
	                                          directory, with room after it
	                                          for a file's name */
	size_t                      n;         /* threads: home and the client
	                                          loops */
	size_t                      cg_len;    /* of cgroup; 0: none */
	int                         sig_fd;
	int                         wake_fd;   /* threads report through it */
	uint32_t                    high;      /* HIGH_* bits */
	uint32_t                    level;     /* memory pressure, as published */
	uint8_t                     levels[PRESSURE_INPUTS];  /* each input's */
	bool                        stopping;  /* shutting down */
};

struct brainstem_metrics brainstem_metrics;

/* The loops of the relief under way that have yet to end theirs, plus one
 * for its trim; 0 while none is under way.  And the bytes they gave back. */
static _Atomic uint32_t relieving;
static _Atomic uint64_t relieved;

/* Set @p p, of brainstem_metrics, which the brainstem alone writes. */
static void
brainstem_metric_set (_Atomic uint64_t *p,
                      uint64_t          v)
{
	atomic_store_explicit(p, v, memory_order_relaxed);
}

static uint32_t
brainstem_thread_state (struct brainstem_thread const *t)
{
	return atomic_load_explicit(&t->state, memory_order_acquire);
}

/* On the loop's thread: say how it stands. */
static void
brainstem_thread_report (struct brainstem_thread *t,
                         uint32_t                 state,
                         int                      error)
{
	t->error = error;
	atomic_store_explicit(&t->state, state, memory_order_release);
	(void)eventfd_write(t->bs->wake_fd, 1);
}

/* On a loop: carry out the brainstem's orders, unless the loop is done. */
static void
brainstem_thread_obey (struct loop *loop,
                       struct msg  *msg)
{
	struct brainstem_thread *t = container_of(msg, struct brainstem_thread, order);
	struct brainstem_app const *app = t->app;
	void *ctx = t->ctx;
	uint32_t orders = atomic_exchange(&t->orders, 0);
	if (loop->exit)
		return;
	if (orders & STATS) {
		char label[sizeof t->name + 1];
		util_format(label, sizeof label, "%s ", t->name);
		loop_log_stats(loop, label);
	}
	if (orders & STALL) {
		/* The test aid: hold the loop in this turn. */
		uint64_t ms = t->bs->cfg->stall_ms;
		struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1'000'000 };
		while (nanosleep(&ts, &ts) && errno == EINTR)
			;
	}
	if (orders & RELIEVE)
		app->relieve(ctx, loop);
	if (orders & DRAIN)
		app->drain(ctx, loop, t->bs->drain_by);
	if (orders & ALONE)
		app->alone(ctx, loop);
	if (orders & OPEN)
		app->open(ctx, loop);
	if (orders & RELOAD)
		app->reload(ctx, loop);
}

/* Post an order, unless the message is on its way already. */
static void
brainstem_thread_order (struct brainstem_thread *t,
                        uint32_t                 bit)
{
	if (!atomic_fetch_or(&t->orders, bit))
		loop_post(&t->loop, &t->order);
}

/* Give each running loop the order @p bit. */
static void
brainstem_order_all (struct brainstem *bs,
                     uint32_t          bit)
{
	for (size_t i = 0; i < bs->n; ++i)
		if (brainstem_thread_state(&bs->threads[i]) == RUNNING)
			brainstem_thread_order(&bs->threads[i], bit);
}

/* Have every running loop relieve memory pressure, unless the last relief
 * is still under way; EAGAIN if it is, or no loop runs, or one starts, as it
 * might run by the time the orders go out, ordered but not counted.  A loop
 * that exits before it ends its share holds up the reliefs after, which only
 * shutdown does. */
static int
brainstem_relieve (struct brainstem *bs)
{
	uint32_t n = 0;
	for (size_t i = 0; i < bs->n; ++i) {
		uint32_t s = brainstem_thread_state(&bs->threads[i]);
		if (s == STARTING)
			return EAGAIN;
		n += s == RUNNING;
	}
	if (!n || atomic_load(&relieving))
		return EAGAIN;
	atomic_store(&relieving, n + 1);
	brainstem_order_all(bs, RELIEVE);
	return 0;
}

bool
brainstem_relieved (uint64_t bytes)
{
	atomic_fetch_add_explicit(&relieved, bytes, memory_order_relaxed);
	return atomic_fetch_sub(&relieving, 1) == 2;
}

uint64_t
brainstem_trimmed (void)
{
	uint64_t bytes = atomic_exchange_explicit(&relieved, 0, memory_order_relaxed);
	atomic_store(&relieving, 0);
	return bytes;
}

/* On the loop's thread, its loop started: start the app, then run the loop
 * until it exits; 0 or the error that ended it. */
static int
brainstem_thread_exec (struct brainstem_thread *t)
{
	int e = t->app->start(t->ctx, &t->loop, t->bs->blockers);
	if (e)
		return e;
	brainstem_thread_report(t, RUNNING, 0);
	e = loop_exec(&t->loop);
	if (e)
		LOG(LOG_ERROR, "%s's event loop: %s", t->name, strerror(e));
	return e;
}

static void *
brainstem_thread_main (void *arg)
{
	struct brainstem_thread *t = arg;
	log_attach();
	(void)pthread_setname_np(pthread_self(), t->name);
	int e = loop_init(&t->loop);
	if (e) {
		LOG(LOG_ERROR, "cannot start %s's event loop: %s", t->name, strerror(e));
	} else {
		e = brainstem_thread_exec(t);
		t->app->stop(t->ctx, &t->loop);
	}
	brainstem_thread_report(t, EXITED, e);
	return nullptr;
}

/* Sleep until @p until at most, or until a thread reports or a signal comes;
 * returns whether a signal came. */
static bool
brainstem_nap (struct brainstem *bs,
               uint64_t          until)
{
	uint64_t now = loop_clock();
	uint64_t ns = until > now ? until - now : 0;
	struct timespec ts = {
		(time_t)(ns / 1'000'000'000),
		(long)(ns % 1'000'000'000)
	};
	struct pollfd fds[] = {
		{ .fd = bs->wake_fd, .events = POLLIN },
		{ .fd = bs->sig_fd, .events = POLLIN },
	};
	if (ppoll(fds, ARRAY_SIZE(fds), until == UINT64_MAX ? nullptr : &ts,
	          nullptr) <= 0)
		return false;
	if (fds[0].revents) {
		eventfd_t n;
		(void)eventfd_read(bs->wake_fd, &n);
	}
	return fds[1].revents;
}

/* End the process now, once the logger has had a moment.  No signal matters
 * any more, and one left unread would end every wait at once.  A loop thread
 * not joined is let go, to end with the process. */
[[noreturn]] static void
brainstem_force (struct brainstem *bs,
                 char const       *why)
{
	LOG(LOG_ERROR, "shutting down at once: %s", why);
	log_stop();
	close(bs->sig_fd);
	bs->sig_fd = -1;
	uint64_t until = loop_clock() + FLUSH_MS * MS;
	while (!log_done() && loop_clock() < until)
		(void)brainstem_nap(bs, until);
	for (size_t i = 0; i < bs->n; ++i)
		if (bs->threads[i].created && brainstem_thread_state(&bs->threads[i]) != JOINED)
			(void)pthread_detach(bs->threads[i].thread);
	_exit(BRAINSTEM_FORCED);
}

/* The same, for why the loop of @p t fails. */
[[noreturn]] static void
brainstem_thread_force (struct brainstem_thread *t,
                        char const              *why)
{
	char text[64];
	util_format(text, sizeof text, "the event loop %s (%s)", why, t->name);
	brainstem_force(t->bs, text);
}

/* Have each running loop drain, once, by the end of four fifths of the grace
 * period, which leaves the rest to the threads that wind down after them. */
static void
brainstem_drain (struct brainstem *bs)
{
	if (!bs->drain_by)
		bs->drain_by = bs->deadline - bs->cfg->grace_ms * MS / 5;
	for (size_t i = 0; i < bs->n; ++i) {
		struct brainstem_thread *t = &bs->threads[i];
		if (brainstem_thread_state(t) == RUNNING && !t->drained) {
			t->drained = true;
			brainstem_thread_order(t, DRAIN);
		}
	}
}

/* Shut down: the loops drain within the grace period. */
static void
brainstem_stop (struct brainstem *bs)
{
	bs->stopping = true;
	bs->deadline = loop_clock() + bs->cfg->grace_ms * MS;
	brainstem_drain(bs);
}

/* Have home reload the configuration, unless shutting down. */
static void
brainstem_reload (struct brainstem *bs)
{
	if (!bs->stopping && brainstem_thread_state(&bs->threads[HOME]) == RUNNING)
		brainstem_thread_order(&bs->threads[HOME], RELOAD);
}

/* Take signal @p signo: SIGUSR1 logs the loops' statistics, SIGUSR2 holds
 * them in a turn (a test aid), and SIGHUP has home reload, if it has a
 * reload; any other, SIGHUP without one included, shuts down, and a second
 * shutdown signal ends the process at once. */
static void
brainstem_signal (struct brainstem *bs,
                  uint32_t          signo)
{
	switch (signo) {
	case SIGUSR1:
		brainstem_order_all(bs, STATS);
		return;
	case SIGUSR2:
		brainstem_order_all(bs, STALL);
		return;
	case SIGHUP:
		if (!bs->cfg->home.reload)
			break;
		brainstem_reload(bs);
		return;
	}
	if (bs->stopping)
		brainstem_force(bs, "a second signal");
	brainstem_stop(bs);
}

static void
brainstem_signals (struct brainstem *bs)
{
	struct signalfd_siginfo si;
	while (read(bs->sig_fd, &si, sizeof si) == (ssize_t)sizeof si)
		brainstem_signal(bs, si.ssi_signo);
}

/* Report @p t's turn under way once it has gone on longer than @p threshold,
 * and while draining, end the process once it has gone on for STUCK_MS; of a
 * turn, the time since bs->resumed counts only. */
static void
brainstem_thread_watch (struct brainstem_thread *t,
                        uint64_t                 now,
                        uint64_t                 threshold)
{
	if (brainstem_thread_state(t) != RUNNING)
		return;
	uint64_t start = atomic_load_explicit(&t->loop.turn, memory_order_relaxed);
	if (!start || start >= now)
		return;
	struct brainstem *bs = t->bs;
	uint64_t seen = now - (start > bs->resumed ? start : bs->resumed);
	if (seen > threshold && start != t->flagged) {
		t->flagged = start;
		hist_add(&brainstem_metrics.reports, 1);
		LOG(LOG_WARN, "watchdog: the event loop has been in one turn for %.1f ms "
		    "(%s)", (double)(now - start) / 1e6, t->name);
	}
	if (bs->stopping && seen > STUCK_MS * MS)
		brainstem_thread_force(t, "is stuck");
}

/*
 * Every half threshold: report a turn that has gone on longer than the
 * threshold, once, and while draining end the process once one has gone on
 * for STUCK_MS.  Woken more than a threshold (two periods) past its time, the
 * brainstem was held up itself, likely with the rest of the process or the
 * machine, so it vouches for no turn's time before then.  With no threshold,
 * it looks every IDLE_MS, for stuck turns only.
 */
static void
brainstem_watch (struct brainstem *bs,
                 uint64_t          now)
{
	uint64_t threshold = UINT64_MAX;
	uint64_t period = IDLE_MS * MS;
	if (bs->cfg->watchdog_ms) {
		threshold = bs->cfg->watchdog_ms * MS;
		period = threshold / 2;
	}
	if (now - bs->tick > 2 * period) {
		bs->resumed = now;
		hist_add(&brainstem_metrics.late, 1);
	}
	bs->tick = now + period;
	for (size_t i = 0; i < bs->n; ++i)
		brainstem_thread_watch(&bs->threads[i], now, threshold);
}

/* The start of the file at @p path, as a string in the @p size bytes at
 * @p buf, empty if unreadable; 0 or the error number. */
static int
brainstem_read (char const *path,
                char       *buf,
                size_t      size)
{
	*buf = '\0';
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno;
	ssize_t n = read(fd, buf, size - 1);
	int e = n < 0 ? errno : 0;
	close(fd);
	buf[n > 0 ? n : 0] = '\0';
	return e;
}

/* Whether a gauge went over its threshold (1) or back under it (-1), as bit
 * @p bit of bs->high keeps track of. */
static int
brainstem_crossed (struct brainstem *bs,
                   uint32_t          bit,
                   bool              over,
                   bool              under)
{
	if (bs->high & bit) {
		if (!under)
			return 0;
		bs->high &= ~bit;
		return -1;
	}
	if (!over)
		return 0;
	bs->high |= bit;
	return 1;
}

/* The number the file at @p path starts with, or @p none if it starts with
 * none, as a cgroup's "max" does, or is unreadable. */
static uint64_t
brainstem_number (char const *path,
                  char       *buf,
                  size_t      size,
                  uint64_t    none)
{
	unsigned long long n;
	return brainstem_read(path, buf, size) || sscanf(buf, "%llu", &n) != 1 ? none : n;
}

/* The number after field @p name in @p text, or @p none if it has none. */
static uint64_t
brainstem_field (char const *text,
                 char const *name,
                 uint64_t    none)
{
	char const *at = strstr(text, name);
	unsigned long long n;
	return at && sscanf(at, "%*s %llu", &n) == 1 ? n : none;
}

/* The length of the path of the cgroup above the one whose directory's path
 * is the first @p len bytes of @p dir; 0 above cgroup_root. */
static size_t
brainstem_cgroup_up (char const *dir,
                     size_t      len)
{
	if (len == sizeof cgroup_root - 1)
		return 0;
	while (dir[--len] != '/')
		;
	return len;
}

/*
 * Of the process's cgroup and its ancestors, the one whose memory use is
 * nearest its limit, the lower of memory.high and memory.max: its use, less
 * the inactive file cache the kernel reclaims as it needs, and that limit go
 * to @p *usage and @p *limit, the lowest limit of all to @p *least, 0 if none
 * has a limit, and its fullness, in hundredths of a percent, is returned.
 */
static uint64_t
brainstem_cgroup (struct brainstem *bs,
                  char             *buf,
                  size_t            size,
                  uint64_t         *usage,
                  uint64_t         *limit,
                  uint64_t         *least)
{
	static char const high_file[] = "/memory.high";
	static char const max_file[] = "/memory.max";
	static char const stat_file[] = "/memory.stat";
	static char const current_file[] = "/memory.current";
	/* The files' names go after each directory's, which bs->cgroup has room
	 * for, and which the walk up does not look at. */
	char *path = bs->cgroup;
	uint64_t most = 0;
	*usage = *limit = *least = 0;
	for (size_t len = bs->cg_len; len; len = brainstem_cgroup_up(path, len)) {
		memcpy(path + len, high_file, sizeof high_file);
		uint64_t high = brainstem_number(path, buf, size, UINT64_MAX);
		memcpy(path + len, max_file, sizeof max_file);
		uint64_t cap = brainstem_number(path, buf, size, UINT64_MAX);
		cap = high < cap ? high : cap;
		if (!cap || cap == UINT64_MAX)
			continue;
		*least = *least && *least < cap ? *least : cap;
		memcpy(path + len, stat_file, sizeof stat_file);
		uint64_t cache = brainstem_read(path, buf, size)
		               ? 0 : brainstem_field(buf, "\ninactive_file ", 0);
		memcpy(path + len, current_file, sizeof current_file);
		uint64_t use = brainstem_number(path, buf, size, 0);
		use -= cache < use ? cache : use;
		uint64_t full = use * 10'000 / cap;
		if (!use || full < most)
			continue;
		most = full;
		*usage = use;
		*limit = cap;
	}
	return most;
}

/* Move each input's memory pressure level with its @p fullness, with
 * hysteresis, and publish it; returns the highest. */
static uint32_t
brainstem_inputs (struct brainstem *bs,
                  uint64_t const   *fullness)
{
	uint32_t level = BUF_EASY;
	for (size_t i = 0; i < PRESSURE_INPUTS; ++i) {
		uint32_t l = bs->levels[i];
		while (l < BUF_CRITICAL && fullness[i] >= marks[i].up[l])
			++l;
		while (l && fullness[i] < marks[i].down[l - 1])
			--l;
		bs->levels[i] = (uint8_t)l;
		brainstem_metric_set(&brainstem_metrics.inputs[i], l);
		level = l > level ? l : level;
	}
	return level;
}

/*
 * Publish the memory pressure @p level, and log a change, with the resident
 * bytes @p rss, the threshold @p high and RAM's fullness @p ram; when it
 * rises, and every BRAINSTEM_RELIEF_MS while it lasts, have the loops relieve
 * it, as soon as the last relief has ended.
 */
static void
brainstem_level (struct brainstem *bs,
                 uint64_t          now,
                 uint32_t          level,
                 uint64_t          rss,
                 uint64_t          high,
                 uint64_t          ram)
{
	static char const *const names[] = { "none", "high", "critical" };
	brainstem_metric_set(&brainstem_metrics.pressure, level);
	atomic_store_explicit(&buf_pressure, level, memory_order_relaxed);
	if (level != bs->level) {
		enum log_level said = LOG_INFO;
		if (level > bs->level) {
			said = LOG_WARN;
			bs->relief = now;
		}
		LOG(said, "memory pressure %s: resident %" PRIu64 " MiB, high from %" PRIu64
		    " MiB; RAM %" PRIu64 " %% in use%s", names[level],
		    rss >> 20, high >> 20, ram / 100,
		    bs->levels[PRESSURE_TEST] ? "; the test aid's level" : "");
	}
	if (level && now >= bs->relief && !brainstem_relieve(bs))
		bs->relief = now + BRAINSTEM_RELIEF_MS * MS;
	bs->level = level;
}

/* Work out the memory pressure level from each input's, and act on it. */
static void
brainstem_pressure (struct brainstem *bs,
                    uint64_t          now,
                    char             *buf,
                    size_t            size)
{
	(void)brainstem_read("/proc/meminfo", buf, size);
	uint64_t total = brainstem_field(buf, "MemTotal:", 0) << 10;
	uint64_t avail = brainstem_field(buf, "MemAvailable:", 0) << 10;
	uint64_t usage;
	uint64_t limit;
	uint64_t least;
	uint64_t cg = brainstem_cgroup(bs, buf, size, &usage, &limit, &least);
	uint64_t room = least && least < total ? least : total;
	uint64_t high = bs->cfg->memory_high ? bs->cfg->memory_high
	              : room / 16 > MEMORY_HIGH_MIN ? room / 16 : MEMORY_HIGH_MIN;
	uint64_t rss = hist_get(&brainstem_metrics.rss);
	uint64_t const fullness[PRESSURE_INPUTS] = {
		[PRESSURE_RESIDENT] = rss * 10'000 / high,
		/* MemAvailable reads 0 where it is absent, as before Linux 3.14. */
		[PRESSURE_SYSTEM] = avail && avail < total ? (total - avail) * 10'000 / total
		                                           : 0,
		[PRESSURE_CGROUP] = cg,
		[PRESSURE_PSI] = hist_get(&brainstem_metrics.psi[1]),
		[PRESSURE_TEST] = bs->cfg->pressure ? brainstem_number(bs->cfg->pressure, buf,
		                                                       size, 0) : 0,
	};
	brainstem_metric_set(&brainstem_metrics.mem_high, high);
	brainstem_metric_set(&brainstem_metrics.mem_total, total);
	brainstem_metric_set(&brainstem_metrics.mem_avail, avail);
	brainstem_metric_set(&brainstem_metrics.cg_usage, usage);
	brainstem_metric_set(&brainstem_metrics.cg_limit, limit);
	brainstem_level(bs, now, brainstem_inputs(bs, fullness), rss, high,
	                fullness[PRESSURE_SYSTEM]);
}

/* Count into @p *fds each entry of /proc/self/fd but ".", ".." and the
 * directory's own descriptor, read through @p buf without malloc(), whose
 * arena lock a trim on a loop may hold. */
static int
brainstem_fd_count (char     *buf,
                    size_t    size,
                    uint64_t *fds)
{
	int dir = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dir < 0)
		return errno;
	uint64_t n = 0;
	for (ssize_t got; (got = getdents64(dir, buf, size)) > 0; )
		for (ssize_t at = 0; at < got; ++n) {
			typeof(((struct dirent64 *)nullptr)->d_reclen) len;
			memcpy(&len, buf + at + offsetof(struct dirent64, d_reclen), sizeof len);
			at += len ? len : got;
		}
	close(dir);
	*fds = n > 3 ? n - 3 : 0;
	return 0;
}

/* Descriptors open, against their limit. */
static void
brainstem_fds (struct brainstem *bs,
               char             *buf,
               size_t            size)
{
	struct rlimit nofile;
	uint64_t fds;
	if (getrlimit(RLIMIT_NOFILE, &nofile) || brainstem_fd_count(buf, size, &fds))
		return;
	uint64_t limit = nofile.rlim_cur;
	brainstem_metric_set(&brainstem_metrics.fds, fds);
	brainstem_metric_set(&brainstem_metrics.fd_limit, limit);
	int c = brainstem_crossed(bs, HIGH_FDS, fds >= limit / 10 * 9,
	                          fds < limit / 10 * 8);
	if (c > 0)
		LOG(LOG_WARN, "descriptors open: %" PRIu64 " of %" PRIu64, fds, limit);
	if (c < 0)
		LOG(LOG_INFO, "descriptors open: %" PRIu64 " of %" PRIu64 ", fewer again",
		    fds, limit);
}

/* The pressure stall information, where the kernel has it. */
static void
brainstem_psi (struct brainstem *bs,
               char             *buf,
               size_t            size)
{
#define PSI "/proc/pressure/"
	static char const *const files[] = { PSI "cpu", PSI "memory", PSI "io" };
	for (size_t i = 0; i < ARRAY_SIZE(files); ++i) {
		char const *name = files[i] + sizeof PSI - 1;
		double avg10;
		unsigned long long total;
		if (brainstem_read(files[i], buf, size) ||
		    sscanf(buf, "some avg10=%lf avg60=%*f avg300=%*f total=%llu", &avg10,
		           &total) != 2)
			continue;
		uint64_t permyriad = (uint64_t)(avg10 * 100 + 0.5);
		brainstem_metric_set(&brainstem_metrics.psi_ok, 1);
		brainstem_metric_set(&brainstem_metrics.psi[i], permyriad);
		brainstem_metric_set(&brainstem_metrics.psi_total[i], total);
		int c = brainstem_crossed(bs, HIGH_PSI << i, permyriad >= 2000,
		                          permyriad < 1000);
		if (c > 0)
			LOG(LOG_WARN, "pressure on %s: some task stalled %.2f %% of the last "
			    "10 s", name, avg10);
		if (c < 0)
			LOG(LOG_INFO, "pressure on %s: some task stalled %.2f %% of the last "
			    "10 s, less again", name, avg10);
	}
#undef PSI
}

/* Once a second: resident memory, descriptors against their limit, the
 * pressure stall information where the kernel has it, and memory pressure. */
static void
brainstem_sample (struct brainstem *bs,
                  uint64_t          now)
{
	bs->sample = now + SAMPLE_MS * MS;
	char buf[16 << 10];
	long page = sysconf(_SC_PAGESIZE);
	unsigned long long resident;
	if (page > 0 && !brainstem_read("/proc/self/statm", buf, sizeof buf) &&
	    sscanf(buf, "%*s %llu", &resident) == 1)
		brainstem_metric_set(&brainstem_metrics.rss, resident * (uint64_t)page);
	brainstem_fds(bs, buf, sizeof buf);
	brainstem_psi(bs, buf, sizeof buf);
	brainstem_pressure(bs, now, buf, sizeof buf);
}

/* Wait at most until @p until, then mind signals, the watchdog, resources
 * and the grace period. */
static void
brainstem_mind (struct brainstem *bs,
                uint64_t          until)
{
	uint64_t next = until;
	uint64_t const due[] = { bs->tick, bs->sample, bs->deadline };
	for (size_t i = 0; i < ARRAY_SIZE(due); ++i)
		next = due[i] < next ? due[i] : next;
	if (brainstem_nap(bs, next))
		brainstem_signals(bs);
	uint64_t now = loop_clock();
	if (now >= bs->tick)
		brainstem_watch(bs, now);
	if (now >= bs->sample) {
		brainstem_sample(bs, now);
		/* Busy with that, the brainstem was not held up. */
		bs->tick += loop_clock() - now;
	}
	if (now >= bs->deadline)
		brainstem_force(bs, "the grace period is over");
	/* A loop that exits on its own shuts the rest down. */
	for (size_t i = 0; i < bs->n && !bs->stopping; ++i)
		if (brainstem_thread_state(&bs->threads[i]) == EXITED)
			brainstem_stop(bs);
}

/* Create @p t's thread, which inherits the mask that blocks every signal, on
 * a stack of STACK bytes. */
static int
brainstem_thread_create (struct brainstem_thread *t)
{
	pthread_attr_t attr;
	int e = pthread_attr_init(&attr);
	if (e)
		return e;
	(void)pthread_attr_setstacksize(&attr, STACK);
	e = pthread_create(&t->thread, &attr, brainstem_thread_main, t);
	(void)pthread_attr_destroy(&attr);
	return e;
}

/* Start @p t's thread, and wait while its loop starts; 0 or the error that
 * kept the thread from starting. */
static int
brainstem_thread_start (struct brainstem_thread *t)
{
	atomic_store_explicit(&t->state, STARTING, memory_order_relaxed);
	int e = brainstem_thread_create(t);
	if (e) {
		atomic_store_explicit(&t->state, JOINED, memory_order_relaxed);
		LOG(LOG_ERROR, "cannot start %s: %s", t->name, strerror(e));
		return e;
	}
	t->created = true;
	uint64_t until = loop_clock() + START_MS * MS;
	while (brainstem_thread_state(t) == STARTING) {
		if (loop_clock() >= until)
			brainstem_thread_force(t, "did not start in time");
		brainstem_mind(t->bs, until);
	}
	return 0;
}

/*
 * Grow the descriptor table to FD_TABLE, or the limit, while the process has
 * one thread: once threads share it, the kernel waits for an RCU grace period
 * each time it grows, 7-11 ms here, in the call that needed a descriptor.
 * The table stays grown after the copy of wake_fd that grew it is closed.
 */
static void
brainstem_fd_table (struct brainstem const *bs)
{
	struct rlimit nofile;
	if (getrlimit(RLIMIT_NOFILE, &nofile))
		return;
	int last = (int)(nofile.rlim_cur < FD_TABLE ? nofile.rlim_cur : FD_TABLE) - 1;
	if (last > bs->wake_fd && fcntl(last, F_GETFD) < 0 &&
	    dup2(bs->wake_fd, last) == last)
		close(last);
}

/* Find the process's cgroup v2 directory, for its limits: the path of
 * /proc/self/cgroup's "0::" line, under where cgroup v2 is mounted, in
 * memory of its own with @p room bytes after it, its NUL among them; its
 * length goes to @p *len, 0 if out of memory, when nullptr is returned. */
static char *
brainstem_cgroup_dir (size_t  room,
                      size_t *len)
{
	/* A line break first, as if a line came before the file's first. */
	char buf[1 + 4096];
	buf[0] = '\n';
	(void)brainstem_read("/proc/self/cgroup", buf + 1, sizeof buf - 1);
	char const *line = strstr(buf, "\n0::/");
	char const *path = line ? line + 4 : "";
	/* "/", the root's path, adds nothing. */
	size_t n = strcspn(path, "\n");
	n = n > 1 ? n : 0;
	size_t at = sizeof cgroup_root - 1;
	char *dir = malloc(at + n + room);
	if (!dir) {
		*len = 0;
		return nullptr;
	}
	memcpy(dir, cgroup_root, at);
	memcpy(dir + at, path, n);
	dir[at + n] = '\0';
	*len = at + n;
	return dir;
}

size_t
brainstem_cpus (void)
{
	cpu_set_t set;
	long online = sysconf(_SC_NPROCESSORS_ONLN);
	size_t n = !sched_getaffinity(0, sizeof set, &set) ? (size_t)CPU_COUNT(&set)
	         : online > 0 ? (size_t)online : 1;
	/* A quota and its period, or "max": no quota.  A fraction of a CPU
	 * still needs one. */
	static char const max_file[] = "/cpu.max";
	size_t len;
	char *path = brainstem_cgroup_dir(sizeof max_file, &len);
	char buf[64];
	for (; len; len = brainstem_cgroup_up(path, len)) {
		unsigned long long quota;
		unsigned long long period;
		memcpy(path + len, max_file, sizeof max_file);
		if (brainstem_read(path, buf, sizeof buf) ||
		    sscanf(buf, "%llu %llu", &quota, &period) != 2 || !period)
			continue;
		uint64_t cpus = (quota + period - 1) / period;
		n = cpus && cpus < n ? (size_t)cpus : n;
	}
	free(path);
	return n ? n : 1;
}

/* Join @p t's thread, if it has exited; one that exited on its own, as the
 * brainstem did not shut down, starts the shutdown. */
static void
brainstem_thread_join (struct brainstem_thread *t)
{
	if (brainstem_thread_state(t) != EXITED)
		return;
	if (!t->bs->stopping)
		brainstem_stop(t->bs);
	(void)pthread_join(t->thread, nullptr);
	atomic_store_explicit(&t->state, JOINED, memory_order_relaxed);
}

/* Lay @p t, storage just allocated, as the thread of @p bs that runs @p app
 * with the context @p ctx; its place in bs->threads numbers its loop.  Its
 * atomics are initialized in place, once, as loop_place() does its loop's. */
static void
brainstem_thread_place (struct brainstem_thread    *t,
                        struct brainstem           *bs,
                        struct brainstem_app const *app,
                        void                       *ctx)
{
	size_t id = (size_t)(t - bs->threads);
	loop_place(&t->loop);
	t->loop.id = id;
	t->order.fn = brainstem_thread_obey;
	t->bs = bs;
	t->app = app;
	t->ctx = ctx;
	atomic_init(&t->orders, 0);
	atomic_init(&t->state, JOINED);
	util_format(t->name, sizeof t->name, id ? "loop%zu" : "home", id);
}

/* Allocate the threads, home's, then the client loops', each with the
 * context of its own. */
static int
brainstem_place_threads (struct brainstem *bs)
{
	struct brainstem_cfg const *cfg = bs->cfg;
	bs->threads = calloc(cfg->loops + 1, sizeof *bs->threads);
	if (!bs->threads)
		return ENOMEM;
	bs->n = cfg->loops + 1;
	brainstem_thread_place(&bs->threads[HOME], bs, &cfg->home, cfg->home.ctx);
	char *ctx = cfg->client.ctx;
	for (size_t i = HOME + 1; i < bs->n; ++i, ctx += cfg->client.stride)
		brainstem_thread_place(&bs->threads[i], bs, &cfg->client, ctx);
	return 0;
}

/* Block every signal but those faults raise, in the threads to come too, as
 * they inherit the mask, and ignore SIGPIPE; returns the mask before. */
static sigset_t
brainstem_block (void)
{
	/* Asynchronous signals go to the signalfd alone. */
	static int const faults[] = {
		SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGSYS, SIGTRAP
	};
	sigset_t all;
	sigset_t old;
	sigfillset(&all);
	for (size_t i = 0; i < ARRAY_SIZE(faults); ++i)
		sigdelset(&all, faults[i]);
	pthread_sigmask(SIG_BLOCK, &all, &old);
	/* libcurl leaves SIGPIPE to us because CURLOPT_NOSIGNAL is set. */
	signal(SIGPIPE, SIG_IGN);
	return old;
}

/*
 * The signals the signalfd takes for @p cfg.  Shutdown signals the process
 * started with ignored, as nohup and shells leave them to spare a job, stay
 * so; SIGHUP is taken all the same where it reloads.  The signalfd would take
 * them all: Linux keeps a blocked signal, whatever its action.
 */
static sigset_t
brainstem_mine (struct brainstem_cfg const *cfg)
{
	static int const stops[] = { SIGHUP, SIGINT, SIGTERM };
	sigset_t mine;
	sigemptyset(&mine);
	for (size_t i = 0; i < ARRAY_SIZE(stops); ++i) {
		struct sigaction sa = {};
		(void)sigaction(stops[i], nullptr, &sa);
		if (sa.sa_handler != SIG_IGN || (stops[i] == SIGHUP && cfg->home.reload))
			sigaddset(&mine, stops[i]);
	}
	sigaddset(&mine, SIGUSR1);
	if (cfg->stall_ms)
		sigaddset(&mine, SIGUSR2);
	return mine;
}

/* Set up @p bs, as brainstem_run() declares it, for @p cfg: the threads, the
 * signalfd that takes @p mine, the eventfd the threads report through, the
 * cgroup's paths, the logger and the blockers. */
static int
brainstem_init (struct brainstem           *bs,
                struct brainstem_cfg const *cfg,
                sigset_t const             *mine)
{
	uint64_t now = loop_clock();
	bs->cfg = cfg;
	bs->tick = now;
	bs->resumed = now;
	bs->sample = now;
	int e = brainstem_place_threads(bs);
	if (e)
		return e;
	bs->sig_fd = signalfd(-1, mine, SFD_NONBLOCK | SFD_CLOEXEC);
	if (bs->sig_fd < 0)
		return errno;
	bs->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (bs->wake_fd < 0)
		return errno;
	brainstem_fd_table(bs);
	size_t len;
	bs->cgroup = brainstem_cgroup_dir(sizeof "/memory.current", &len);
	if (!bs->cgroup)
		return ENOMEM;
	bs->cg_len = len;
	/* Rings for the brainstem, the loops and the blockers. */
	e = log_start(1 + bs->n + BLOCKER_THREADS, bs->wake_fd);
	if (e)
		return e;
	log_attach();
	bs->blockers = blocker_create(bs->wake_fd, &e);
	if (e)
		return e;
	return blocker_start(bs->blockers);
}

/* Start home's thread, then the client loops', until a signal comes or one
 * does not run; returns whether all run, the error that kept one from
 * starting in @p *e. */
static bool
brainstem_start_all (struct brainstem *bs,
                     int              *e)
{
	for (size_t i = 0; i < bs->n; ++i) {
		struct brainstem_thread *t = &bs->threads[i];
		if (bs->stopping || (*e = brainstem_thread_start(t)) ||
		    brainstem_thread_state(t) != RUNNING)
			return false;
	}
	return true;
}

/*
 * Start home's thread, then the client loops', unless a signal comes
 * meanwhile; until all run, the brainstem keeps every signal blocked, as they
 * inherit its mask, and takes its own through the signalfd.  A loop that
 * could not start shuts the others down; once every one runs, home opens its
 * door to clients.  Returns 0 or the error that kept a thread from starting.
 */
static int
brainstem_start (struct brainstem *bs)
{
	int e = 0;
	bool all = brainstem_start_all(bs, &e);
	if (bs->stopping)
		brainstem_drain(bs);
	else if (!all)
		brainstem_stop(bs);
	else
		brainstem_thread_order(&bs->threads[HOME], OPEN);
	return e;
}

/* Tell @p t's loop, if it runs, that it is alone, and wait for it to
 * exit. */
static void
brainstem_alone (struct brainstem        *bs,
                 struct brainstem_thread *t)
{
	if (brainstem_thread_state(t) != RUNNING)
		return;
	brainstem_thread_order(t, ALONE);
	do brainstem_mind(bs, UINT64_MAX);
	while (brainstem_thread_state(t) == RUNNING);
}

/* Join the client loops' threads that have exited; returns whether a client
 * loop runs. */
static bool
brainstem_clients_run (struct brainstem *bs)
{
	bool running = false;
	for (size_t i = HOME + 1; i < bs->n; ++i) {
		struct brainstem_thread *t = &bs->threads[i];
		brainstem_thread_join(t);
		running |= brainstem_thread_state(t) == RUNNING;
	}
	return running;
}

/* The client loops exit first, in any order; then home, told it is alone,
 * once its jobs end.  Returns the first error a loop ended with, 0 if none. */
static int
brainstem_wait (struct brainstem *bs)
{
	while (brainstem_clients_run(bs))
		brainstem_mind(bs, UINT64_MAX);
	int e = 0;
	for (size_t i = 0; i < bs->n; ++i) {
		struct brainstem_thread *t = &bs->threads[i];
		brainstem_alone(bs, t);
		brainstem_thread_join(t);
		e = e ? e : t->error;
	}
	return e;
}

/*
 * Stop the blockers, and once they are done, within JOIN_MS, free them and
 * the loops; returns whether they were done.  The blockers may still post to
 * home, whose loop stays until they are done, as do the client loops, which
 * home's may post to; a blocker stuck in a call is left to the process's
 * exit.  Home's loop goes first: what its inbox still holds posts to the
 * client loops, whose own handlers post to nobody.
 */
static bool
brainstem_stop_blockers (struct brainstem *bs)
{
	blocker_stop(bs->blockers);
	uint64_t until = loop_clock() + JOIN_MS * MS;
	while (!blocker_done(bs->blockers) && loop_clock() < until)
		brainstem_mind(bs, until);
	if (!blocker_done(bs->blockers)) {
		LOG(LOG_WARN, "a blocker is still busy");
		return false;
	}
	blocker_destroy(&bs->blockers);
	for (size_t i = 0; i < bs->n; ++i)
		if (bs->threads[i].created)
			loop_fini(&bs->threads[i].loop);
	free(bs->threads);
	bs->threads = nullptr;
	bs->n = 0;
	return true;
}

/* Free what @p bs holds, once the threads that use it are done, and let the
 * logger write what it holds, within FLUSH_MS. */
static void
brainstem_fini (struct brainstem *bs)
{
	bool freed = brainstem_stop_blockers(bs);
	log_stop();
	uint64_t until = loop_clock() + FLUSH_MS * MS;
	while (!log_done() && loop_clock() < until)
		brainstem_mind(bs, until);
	/* The threads write wake_fd as they exit, the last of them perhaps
	 * still to come, so it stays open until the process exits. */
	if (freed && log_done())
		log_fini();
	if (bs->sig_fd >= 0)
		close(bs->sig_fd);
	bs->sig_fd = -1;
	free(bs->cgroup);
	bs->cgroup = nullptr;
	bs->cg_len = 0;
}

int
brainstem_run (struct brainstem_cfg const *cfg)
{
	sigset_t old = brainstem_block();
	sigset_t mine = brainstem_mine(cfg);

	/* Static, as a blocker stuck in a call may post to home until the
	 * process exits; so its loops stay too then. */
	static struct brainstem bs = {
		.deadline = UINT64_MAX,
		.sig_fd = -1,
		.wake_fd = -1
	};
	int e = brainstem_init(&bs, cfg, &mine);
	if (e)
		LOG(LOG_ERROR, "cannot start: %s", strerror(e));
	else
		e = brainstem_start(&bs);
	/* What the signalfd does not take, the brainstem takes as before. */
	sigorset(&mine, &mine, &old);
	pthread_sigmask(SIG_SETMASK, &mine, nullptr);
	int lost = brainstem_wait(&bs);
	brainstem_fini(&bs);
	return e || lost ? 1 : 0;
}
