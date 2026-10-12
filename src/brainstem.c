/** @file The brainstem: signals, threads, watchdog, resources, shutdown
 *  (brainstem.h). */
#define _GNU_SOURCE /* CPU_COUNT(), getdents64(), ppoll(), pthread_setname_np(),
                       sched_getaffinity(), sigorset() */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
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
struct loop_thread {
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
	struct loop_thread         *threads;   /* home's, then the client loops' */
	struct brainstem_cfg const *cfg;
	struct blocker             *blockers;
	uint64_t                    drain_by;  /* for the loops, set before DRAIN */
	uint64_t                    deadline;  /* of the grace period */
	uint64_t                    tick;      /* the watchdog's next look */
	uint64_t                    resumed;   /* the watchdog vouches for time
	                                          since then only */
	uint64_t                    sample;    /* when to sample resources next */
	uint64_t                    relief;    /* when the loops are to relieve
	                                          memory pressure again, if it
	                                          lasts */
	uint64_t                    stopping;  /* bool, widened over the padding:
	                                          shutting down */
	size_t                      n;         /* threads: home and the client
	                                          loops */
	size_t                      cg_len;    /* of cgroup */
	int                         sig_fd;
	int                         wake_fd;   /* threads report through it */
	uint32_t                    high;      /* HIGH_* bits */
	uint32_t                    level;     /* memory pressure, as published */
	uint8_t                     levels[PRESSURE_INPUTS];  /* each input's */
	char                        cgroup[PATH_MAX];  /* the process's cgroup v2
	                                                  directory */
};

struct brainstem_metrics brainstem_metrics;

/* The loops of the relief under way that have yet to end theirs, plus one
 * for its trim; 0 while none is under way.  And the bytes they gave back. */
static _Atomic uint32_t relieving;
static _Atomic uint64_t relieved;

static void
set (_Atomic uint64_t *p,
     uint64_t          v)
{
	atomic_store_explicit(p, v, memory_order_relaxed);
}

static uint32_t
state (struct loop_thread const *t)
{
	return atomic_load_explicit(&t->state, memory_order_acquire);
}

/* On the loop's thread: say how it stands. */
static void
report (struct loop_thread *t,
        uint32_t            state,
        int                 error)
{
	t->error = error;
	atomic_store_explicit(&t->state, state, memory_order_release);
	(void)eventfd_write(t->bs->wake_fd, 1);
}

/* On a loop: carry out the brainstem's orders, unless the loop is done. */
static void
loop_order (struct loop *loop,
            struct msg  *msg)
{
	struct loop_thread *t = container_of(msg, struct loop_thread, order);
	struct brainstem_app const *app = t->app;
	void *ctx = t->ctx;
	uint32_t orders = atomic_exchange(&t->orders, 0);
	if (loop->exit)
		return;
	if (orders & STATS) {
		char label[sizeof t->name + 1];
		snprintf(label, sizeof label, "%s ", t->name);
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
order (struct loop_thread *t,
       uint32_t            bit)
{
	if (!atomic_fetch_or(&t->orders, bit))
		loop_post(&t->loop, &t->order);
}

/* Give each running loop the order @p bit. */
static void
order_all (struct brainstem *bs,
           uint32_t          bit)
{
	for (size_t i = 0; i < bs->n; ++i)
		if (state(&bs->threads[i]) == RUNNING)
			order(&bs->threads[i], bit);
}

/* Have every running loop relieve memory pressure, unless the last relief
 * is still under way; false if it is, or no loop runs, or one starts, as it
 * might run by the time the orders go out, ordered but not counted.  A loop
 * that exits before it ends its share holds up the reliefs after, which only
 * shutdown does. */
static bool
relieve (struct brainstem *bs)
{
	uint32_t n = 0;
	for (size_t i = 0; i < bs->n; ++i) {
		uint32_t s = state(&bs->threads[i]);
		if (s == STARTING)
			return false;
		n += s == RUNNING;
	}
	if (!n || atomic_load(&relieving))
		return false;
	atomic_store(&relieving, n + 1);
	order_all(bs, RELIEVE);
	return true;
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

static void *
loop_main (void *arg)
{
	struct loop_thread *t = arg;
	struct brainstem_app const *app = t->app;
	log_attach();
	(void)pthread_setname_np(pthread_self(), t->name);
	int e = loop_init(&t->loop);
	if (e) {
		LOG(LOG_ERROR, "cannot start %s's event loop: %s", t->name, strerror(e));
	} else {
		if (!(e = app->start(t->ctx, &t->loop, t->bs->blockers))) {
			report(t, RUNNING, 0);
			if ((e = loop_exec(&t->loop)))
				LOG(LOG_ERROR, "%s's event loop: %s", t->name, strerror(e));
		}
		app->stop(t->ctx, &t->loop);
	}
	report(t, EXITED, e);
	return nullptr;
}

/* Sleep until @p until at most, or until a thread reports or a signal comes;
 * returns whether a signal came. */
static bool
nap (struct brainstem *bs,
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
force (struct brainstem *bs,
       char const       *why)
{
	LOG(LOG_ERROR, "shutting down at once: %s", why);
	log_stop();
	close(bs->sig_fd);
	bs->sig_fd = -1;
	uint64_t until = loop_clock() + FLUSH_MS * MS;
	while (!log_done() && loop_clock() < until)
		(void)nap(bs, until);
	for (size_t i = 0; i < bs->n; ++i)
		if (bs->threads[i].created && state(&bs->threads[i]) != JOINED)
			pthread_detach(bs->threads[i].thread);
	_exit(BRAINSTEM_FORCED);
}

/* The same, for why the loop of @p t fails. */
[[noreturn]] static void
force_for (struct brainstem   *bs,
           struct loop_thread *t,
           char const         *why)
{
	char text[64];
	snprintf(text, sizeof text, "the event loop %s (%s)", why, t->name);
	force(bs, text);
}

/* Have each running loop drain, once, by the end of four fifths of the grace
 * period, which leaves the rest to the threads that wind down after them. */
static void
drain (struct brainstem *bs)
{
	if (!bs->drain_by)
		bs->drain_by = bs->deadline - bs->cfg->grace_ms * MS / 5;
	for (size_t i = 0; i < bs->n; ++i) {
		struct loop_thread *t = &bs->threads[i];
		if (state(t) == RUNNING && !t->drained) {
			t->drained = true;
			order(t, DRAIN);
		}
	}
}

/* Shut down: the loops drain within the grace period. */
static void
stop (struct brainstem *bs)
{
	bs->stopping = true;
	bs->deadline = loop_clock() + bs->cfg->grace_ms * MS;
	drain(bs);
}

/* Have home reload the configuration, unless shutting down. */
static void
reload (struct brainstem *bs)
{
	if (!bs->stopping && state(&bs->threads[HOME]) == RUNNING)
		order(&bs->threads[HOME], RELOAD);
}

static void
signals (struct brainstem *bs)
{
	struct signalfd_siginfo si;
	while (read(bs->sig_fd, &si, sizeof si) == (ssize_t)sizeof si) {
		if (si.ssi_signo == SIGUSR1)
			order_all(bs, STATS);
		else if (si.ssi_signo == SIGUSR2)
			order_all(bs, STALL);
		else if (si.ssi_signo == SIGHUP && bs->cfg->home.reload)
			reload(bs);
		else if (bs->stopping)
			force(bs, "a second signal");
		else
			stop(bs);
	}
}

/*
 * Every half threshold: report a turn that has gone on longer than the
 * threshold, once, and while draining end the process once one has gone on
 * for STUCK_MS.  Woken more than a threshold (two periods) past its time, the
 * brainstem was held up itself, likely with the rest of the process or the
 * machine, so it vouches for no turn's time before then.
 */
static void
watch (struct brainstem *bs,
       uint64_t          now)
{
	uint64_t threshold = bs->cfg->watchdog_ms * MS;
	uint64_t period = threshold ? threshold / 2 : IDLE_MS * MS;
	if (now - bs->tick > 2 * period) {
		bs->resumed = now;
		hist_add(&brainstem_metrics.late, 1);
	}
	bs->tick = now + period;

	for (size_t i = 0; i < bs->n; ++i) {
		struct loop_thread *t = &bs->threads[i];
		uint64_t start = state(t) == RUNNING
		               ? atomic_load_explicit(&t->loop.turn, memory_order_relaxed) : 0;
		if (!start || start >= now)
			continue;
		uint64_t seen = now - (start > bs->resumed ? start : bs->resumed);
		if (threshold && seen > threshold && start != t->flagged) {
			t->flagged = start;
			hist_add(&brainstem_metrics.reports, 1);
			LOG(LOG_WARN, "watchdog: the event loop has been in one turn for %.1f ms "
			    "(%s)", (double)(now - start) / 1e6, t->name);
		}
		if (bs->stopping && seen > STUCK_MS * MS)
			force_for(bs, t, "is stuck");
	}
}

/* The start of the file at @p path, as a string; false if unreadable. */
static bool
slurp (char const *path,
       char       *buf,
       size_t      size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n = fd < 0 ? -1 : read(fd, buf, size - 1);
	if (fd >= 0)
		close(fd);
	buf[n > 0 ? n : 0] = '\0';
	return n > 0;
}

/* Whether a gauge went over its threshold (1) or back under it (-1), as bit
 * @p bit of bs->high keeps track of. */
static int
crossed (struct brainstem *bs,
         uint32_t          bit,
         bool              over,
         bool              under)
{
	if (over && !(bs->high & bit)) {
		bs->high |= bit;
		return 1;
	}
	if (under && (bs->high & bit)) {
		bs->high &= ~bit;
		return -1;
	}
	return 0;
}

/* The number the file at @p path starts with, or @p none if it starts with
 * none, as a cgroup's "max" does, or is unreadable. */
static uint64_t
number (char const *path,
        char       *buf,
        size_t      size,
        uint64_t    none)
{
	unsigned long long n;
	return slurp(path, buf, size) && sscanf(buf, "%llu", &n) == 1 ? n : none;
}

/* The number after field @p name in @p text, or @p none if it has none. */
static uint64_t
field (char const *text,
       char const *name,
       uint64_t    none)
{
	char const *at = strstr(text, name);
	unsigned long long n;
	return at && sscanf(at, "%*s %llu", &n) == 1 ? n : none;
}

/*
 * Of the process's cgroup and its ancestors, the one whose memory use is
 * nearest its limit, the lower of memory.high and memory.max: its use, less
 * the inactive file cache the kernel reclaims as it needs, and that limit go
 * to @p *usage and @p *limit, the lowest limit of all to @p *least, 0 if none
 * has a limit, and its fullness, in hundredths of a percent, is returned.
 */
static uint64_t
cgroup (struct brainstem *bs,
        char             *buf,
        size_t            size,
        uint64_t         *usage,
        uint64_t         *limit,
        uint64_t         *least)
{
	char path[sizeof bs->cgroup + sizeof "/memory.current"];
	uint64_t most = 0;
	*usage = *limit = *least = 0;
	for (size_t len = bs->cg_len; len >= sizeof cgroup_root - 1; ) {
		memcpy(path, bs->cgroup, len);
		strcpy(path + len, "/memory.high");
		uint64_t high = number(path, buf, size, UINT64_MAX);
		strcpy(path + len, "/memory.max");
		uint64_t max = number(path, buf, size, UINT64_MAX);
		uint64_t cap = high < max ? high : max;
		uint64_t use = 0;
		if (cap && cap < UINT64_MAX) {
			strcpy(path + len, "/memory.stat");
			uint64_t cache = slurp(path, buf, size)
			               ? field(buf, "\ninactive_file ", 0) : 0;
			strcpy(path + len, "/memory.current");
			use = number(path, buf, size, 0);
			use -= cache < use ? cache : use;
			*least = *least && *least < cap ? *least : cap;
		}
		if (use && use * 10'000 / cap >= most) {
			most = use * 10'000 / cap;
			*usage = use;
			*limit = cap;
		}
		if (len == sizeof cgroup_root - 1)
			break;
		while (bs->cgroup[--len] != '/')
			;
	}
	return most;
}

/*
 * Work out the memory pressure level from each input's, which moves with
 * hysteresis, and publish it; when it rises, and every BRAINSTEM_RELIEF_MS
 * while it lasts, have the loops relieve it, as soon as the last relief has
 * ended.
 */
static void
pressure (struct brainstem *bs,
          uint64_t          now,
          char             *buf,
          size_t            size)
{
	static char const *const names[] = { "none", "high", "critical" };
	uint64_t total = 0;
	uint64_t avail = 0;
	if (slurp("/proc/meminfo", buf, size)) {
		total = field(buf, "MemTotal:", 0) << 10;
		avail = field(buf, "MemAvailable:", 0) << 10;
	}
	uint64_t usage;
	uint64_t limit;
	uint64_t least;
	uint64_t cg = cgroup(bs, buf, size, &usage, &limit, &least);
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
		[PRESSURE_TEST] = bs->cfg->pressure ? number(bs->cfg->pressure, buf, size, 0)
		                                    : 0,
	};
	uint32_t level = BUF_EASY;
	for (size_t i = 0; i < PRESSURE_INPUTS; ++i) {
		uint32_t l = bs->levels[i];
		while (l < BUF_CRITICAL && fullness[i] >= marks[i].up[l])
			++l;
		while (l && fullness[i] < marks[i].down[l - 1])
			--l;
		bs->levels[i] = (uint8_t)l;
		set(&brainstem_metrics.inputs[i], l);
		level = l > level ? l : level;
	}
	set(&brainstem_metrics.mem_high, high);
	set(&brainstem_metrics.mem_total, total);
	set(&brainstem_metrics.mem_avail, avail);
	set(&brainstem_metrics.cg_usage, usage);
	set(&brainstem_metrics.cg_limit, limit);
	set(&brainstem_metrics.pressure, level);
	atomic_store_explicit(&buf_pressure, level, memory_order_relaxed);
	if (level != bs->level)
		LOG(level > bs->level ? LOG_WARN : LOG_INFO, "memory pressure %s: resident "
		    "%" PRIu64 " MiB, high from %" PRIu64 " MiB; RAM %" PRIu64 " %% in use%s",
		    names[level], rss >> 20, high >> 20, fullness[PRESSURE_SYSTEM] / 100,
		    bs->levels[PRESSURE_TEST] ? "; the test aid's level" : "");
	if (level > bs->level)
		bs->relief = now;
	if (level && now >= bs->relief && relieve(bs))
		bs->relief = now + BRAINSTEM_RELIEF_MS * MS;
	bs->level = level;
}

/* Once a second: resident memory, descriptors against their limit, the
 * pressure stall information where the kernel has it, and memory pressure. */
static void
sample (struct brainstem *bs,
        uint64_t          now)
{
	bs->sample = now + SAMPLE_MS * MS;
	char buf[16 << 10];
	unsigned long long resident;
	if (slurp("/proc/self/statm", buf, sizeof buf) &&
	    sscanf(buf, "%*s %llu", &resident) == 1)
		set(&brainstem_metrics.rss, resident * (uint64_t)sysconf(_SC_PAGESIZE));

	/* Each entry but ".", ".." and the directory's own descriptor, read
	 * without malloc(), whose arena lock a trim on the loop may hold. */
	int dir = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	struct rlimit nofile;
	if (dir >= 0 && !getrlimit(RLIMIT_NOFILE, &nofile)) {
		uint64_t fds = 0;
		for (ssize_t n; (n = getdents64(dir, buf, sizeof buf)) > 0; )
			for (ssize_t at = 0; at < n; ++fds) {
				typeof(((struct dirent64 *)nullptr)->d_reclen) len;
				memcpy(&len, buf + at + offsetof(struct dirent64, d_reclen),
				       sizeof len);
				at += len ? len : n;
			}
		fds = fds > 3 ? fds - 3 : 0;
		uint64_t limit = nofile.rlim_cur;
		set(&brainstem_metrics.fds, fds);
		set(&brainstem_metrics.fd_limit, limit);
		int c = crossed(bs, HIGH_FDS, fds >= limit / 10 * 9, fds < limit / 10 * 8);
		if (c)
			LOG(c > 0 ? LOG_WARN : LOG_INFO, "descriptors open: %" PRIu64 " of %"
			    PRIu64 "%s", fds, limit, c > 0 ? "" : ", fewer again");
	}
	if (dir >= 0)
		close(dir);

	static char const *const psi[] = { "cpu", "memory", "io" };
	for (size_t i = 0; i < ARRAY_SIZE(psi); ++i) {
		char path[32];
		double avg10;
		unsigned long long total;
		snprintf(path, sizeof path, "/proc/pressure/%s", psi[i]);
		if (!slurp(path, buf, sizeof buf) ||
		    sscanf(buf, "some avg10=%lf avg60=%*f avg300=%*f total=%llu", &avg10,
		           &total) != 2)
			continue;
		uint64_t permyriad = (uint64_t)(avg10 * 100 + 0.5);
		set(&brainstem_metrics.psi_ok, 1);
		set(&brainstem_metrics.psi[i], permyriad);
		set(&brainstem_metrics.psi_total[i], total);
		int c = crossed(bs, HIGH_PSI << i, permyriad >= 2000, permyriad < 1000);
		if (c)
			LOG(c > 0 ? LOG_WARN : LOG_INFO, "pressure on %s: some task stalled "
			    "%.2f %% of the last 10 s%s", psi[i], avg10,
			    c > 0 ? "" : ", less again");
	}
	pressure(bs, now, buf, sizeof buf);
}

/* Wait at most until @p until, then mind signals, the watchdog, resources
 * and the grace period. */
static void
mind (struct brainstem *bs,
      uint64_t          until)
{
	uint64_t next = until;
	uint64_t const due[] = { bs->tick, bs->sample, bs->deadline };
	for (size_t i = 0; i < ARRAY_SIZE(due); ++i)
		next = due[i] < next ? due[i] : next;
	if (nap(bs, next))
		signals(bs);
	uint64_t now = loop_clock();
	if (now >= bs->tick)
		watch(bs, now);
	if (now >= bs->sample) {
		sample(bs, now);
		/* Busy with that, the brainstem was not held up. */
		bs->tick += loop_clock() - now;
	}
	if (now >= bs->deadline)
		force(bs, "the grace period is over");
	/* A loop that exits on its own shuts the rest down. */
	for (size_t i = 0; i < bs->n && !bs->stopping; ++i)
		if (state(&bs->threads[i]) == EXITED)
			stop(bs);
}

/* Start @p t's thread, which inherits the mask that blocks every signal, on
 * a stack of @p attr's size, and wait for its loop to run; false if it does
 * not. */
static bool
start (struct brainstem     *bs,
       struct loop_thread   *t,
       pthread_attr_t const *attr)
{
	atomic_store_explicit(&t->state, STARTING, memory_order_relaxed);
	int e = pthread_create(&t->thread, attr, loop_main, t);
	if (e) {
		atomic_store_explicit(&t->state, JOINED, memory_order_relaxed);
		t->error = e;
		LOG(LOG_ERROR, "cannot start %s: %s", t->name, strerror(e));
		return false;
	}
	t->created = true;
	uint64_t until = loop_clock() + START_MS * MS;
	while (state(t) == STARTING) {
		if (loop_clock() >= until)
			force_for(bs, t, "did not start in time");
		mind(bs, until);
	}
	return state(t) == RUNNING;
}

/*
 * Grow the descriptor table to FD_TABLE, or the limit, while the process has
 * one thread: once threads share it, the kernel waits for an RCU grace period
 * each time it grows, 7-11 ms here, in the call that needed a descriptor.
 * The table stays grown; @p fd is any open descriptor.
 */
static void
fd_table (int fd)
{
	struct rlimit nofile;
	if (fd < 0 || getrlimit(RLIMIT_NOFILE, &nofile))
		return;
	int last = (int)(nofile.rlim_cur < FD_TABLE ? nofile.rlim_cur : FD_TABLE) - 1;
	if (last > fd && fcntl(last, F_GETFD) < 0 && dup2(fd, last) == last)
		close(last);
}

/* Find the process's cgroup v2 directory, for its limits: the path of
 * /proc/self/cgroup's "0::" line, under where cgroup v2 is mounted, into
 * @p dir; returns its length, 0 if it does not fit. */
static size_t
cgroup_dir (char   *dir,
            size_t  size)
{
	char buf[4096];
	char const *line = slurp("/proc/self/cgroup", buf, sizeof buf) ? buf : "";
	if (strncmp(line, "0::/", 4) && (line = strstr(line, "\n0::/")))
		++line;
	size_t n = line && *line ? strcspn(line + 3, "\n") : 0;
	int len = snprintf(dir, size, "%s%.*s", cgroup_root, n > 1 ? (int)n : 0,
	                   line ? line + 3 : "");
	return len > 0 && (size_t)len < size ? (size_t)len : 0;
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
	char path[PATH_MAX + sizeof "/cpu.max"];
	char buf[64];
	for (size_t len = cgroup_dir(path, PATH_MAX); len >= sizeof cgroup_root - 1; ) {
		unsigned long long quota;
		unsigned long long period;
		strcpy(path + len, "/cpu.max");
		if (slurp(path, buf, sizeof buf) &&
		    sscanf(buf, "%llu %llu", &quota, &period) == 2 && period) {
			uint64_t cpus = (quota + period - 1) / period;
			n = cpus && cpus < n ? (size_t)cpus : n;
		}
		if (len == sizeof cgroup_root - 1)
			break;
		while (path[--len] != '/')
			;
	}
	return n ? n : 1;
}

/* Join @p t's thread, if it has exited; one that exited on its own, as the
 * brainstem did not shut down, starts the shutdown. */
static void
join (struct brainstem   *bs,
      struct loop_thread *t)
{
	if (state(t) != EXITED)
		return;
	if (!bs->stopping)
		stop(bs);
	pthread_join(t->thread, nullptr);
	atomic_store_explicit(&t->state, JOINED, memory_order_relaxed);
}

int
brainstem_run (struct brainstem_cfg const *cfg)
{
	/* Threads inherit the mask: asynchronous signals go to the signalfd
	 * alone.  Signals that faults raise stay unblocked. */
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

	/* Shutdown signals the process started with ignored, as nohup and
	 * shells leave them to spare a job, stay so; SIGHUP is taken all the
	 * same where it reloads.  The signalfd would take them all: Linux keeps
	 * a blocked signal, whatever its action. */
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

	/* Static, as a blocker stuck in a call may post to home until the
	 * process exits; so its loops stay too then. */
	uint64_t now = loop_clock();
	static struct brainstem bs = {
		.deadline = UINT64_MAX,
		.sig_fd = -1,
		.wake_fd = -1
	};
	bs.threads = calloc(cfg->loops + 1, sizeof *bs.threads);
	bs.n = bs.threads ? cfg->loops + 1 : 0;
	for (size_t i = 0; i < bs.n; ++i) {
		struct loop_thread *t = &bs.threads[i];
		loop_place(&t->loop);
		t->loop.id = i;
		t->order.fn = loop_order;
		t->bs = &bs;
		t->app = i ? &cfg->client : &cfg->home;
		t->ctx = (char *)t->app->ctx + (i ? i - 1 : 0) * t->app->stride;
		atomic_init(&t->orders, 0);
		atomic_init(&t->state, JOINED);
		snprintf(t->name, sizeof t->name, i ? "loop%zu" : "home", i);
	}
	bs.cfg = cfg;
	bs.tick = now;
	bs.resumed = now;
	bs.sample = now;
	bs.sig_fd = signalfd(-1, &mine, SFD_NONBLOCK | SFD_CLOEXEC);
	bs.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	int e = bs.sig_fd < 0 || bs.wake_fd < 0 ? errno : !bs.threads ? ENOMEM : 0;
	fd_table(bs.wake_fd);
	bs.cg_len = cgroup_dir(bs.cgroup, sizeof bs.cgroup);
	/* Rings for the brainstem, the loops and the blockers. */
	if (!e && !(e = log_start(1 + bs.n + BLOCKER_THREADS, bs.wake_fd)))
		log_attach();
	if (!e)
		bs.blockers = blocker_create(bs.wake_fd, &e);
	if (!e)
		e = blocker_start(bs.blockers);
	if (e)
		LOG(LOG_ERROR, "cannot start: %s", strerror(e));
	/* Home first, then the client loops, unless a signal came meanwhile;
	 * until all run, the brainstem keeps every signal blocked, as they
	 * inherit its mask, and takes its own through the signalfd. */
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, STACK);
	size_t started = 0;
	while (!e && !bs.stopping && started < bs.n &&
	       start(&bs, &bs.threads[started], &attr))
		++started;
	pthread_attr_destroy(&attr);
	/* What the signalfd does not take, the brainstem takes as before. */
	sigorset(&mine, &mine, &old);
	pthread_sigmask(SIG_SETMASK, &mine, nullptr);
	/* A loop that could not start shuts the others down; once every one
	 * runs, home opens its door to clients. */
	if (!e && started < bs.n && !bs.stopping)
		stop(&bs);
	if (bs.stopping)
		drain(&bs);
	else if (!e)
		order(&bs.threads[HOME], OPEN);

	/* The client loops exit first, in any order; then home, told it is
	 * alone, once its jobs end. */
	for (bool running = true; running; ) {
		running = false;
		for (size_t i = HOME + 1; i < bs.n; ++i) {
			join(&bs, &bs.threads[i]);
			running |= state(&bs.threads[i]) == RUNNING;
		}
		if (running)
			mind(&bs, UINT64_MAX);
	}
	for (size_t i = 0; i < bs.n; ++i) {
		struct loop_thread *t = &bs.threads[i];
		if (state(t) == RUNNING)
			order(t, ALONE);
		while (state(t) == RUNNING)
			mind(&bs, UINT64_MAX);
		join(&bs, t);
		e = e ? e : t->error;
	}

	/* The blockers may still post to home, whose loop stays until they are
	 * done, as do the client loops, which home's may post to; a blocker
	 * stuck in a call is left to the process's exit.  Home's loop goes
	 * first: what its inbox still holds posts to the client loops, whose
	 * own handlers post to nobody. */
	blocker_stop(bs.blockers);
	uint64_t until = loop_clock() + JOIN_MS * MS;
	while (!blocker_done(bs.blockers) && loop_clock() < until)
		mind(&bs, until);
	bool done = blocker_done(bs.blockers);
	if (done) {
		blocker_destroy(&bs.blockers);
		for (size_t i = 0; i < bs.n; ++i)
			if (bs.threads[i].created)
				loop_fini(&bs.threads[i].loop);
		free(bs.threads);
		bs.threads = nullptr;
		bs.n = 0;
	} else {
		LOG(LOG_WARN, "a blocker is still busy");
	}

	log_stop();
	until = loop_clock() + FLUSH_MS * MS;
	while (!log_done() && loop_clock() < until)
		mind(&bs, until);
	/* The threads write wake_fd as they exit, the last of them perhaps
	 * still to come, so it stays open until the process exits. */
	if (done && log_done())
		log_fini();
	if (bs.sig_fd >= 0)
		close(bs.sig_fd);
	return e ? 1 : 0;
}
