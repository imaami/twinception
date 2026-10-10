/** @file Event loop: epoll, a 4-ary timer heap, budgeted ready lists and an
 *  inbox. */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "hist.h"
#include "log.h"
#include "loop.h"
#include "util.h"

/* The ready lists; a turn takes the two interactive ones together. */
enum { FRESH, ONGOING, BULK };

enum {
	EVENTS         = 128,      /* events taken per wait */
	MAIL           = 256,      /* inbox handlers per turn */
	TIMERS         = 64,       /* timers fired per turn */
	INTERACTIVE_NS = 500'000,  /* interactive steps start until then */
	BULK_NS        = 650'000,  /* further bulk steps: a step of up to 250 µs
	                              then ends by 1 ms, with time to spare for
	                              page faults and preemption */
	HEAP_MIN       = 64,       /* timer slots the heap begins with; it keeps
	                              those it grows to (design section 5) */
};

/* Times in ns; those over 1 ms are also counted exactly, as the histogram's
 * bucket around 1 ms straddles it. */
struct timing {
	struct hist hist;
	uint64_t    over;
};

struct loop_stats {
	struct timing turn;              /* from epoll returning to the end of the turn */
	struct timing step[2];           /* interactive, bulk */
	struct timing late;              /* how long after its due time a timer fired */
	uint64_t      work[LOOP_UNITS];  /* the most a step did */
	uint64_t      events;            /* the most a turn had */
	uint64_t      timers;
	uint64_t      steps;
};

static constexpr uint64_t bound[LOOP_UNITS] = {
	[LOOP_REQS]    = LOOP_STEP_REQS,
	[LOOP_RECV]    = LOOP_STEP_RECV,
	[LOOP_SEND]    = LOOP_STEP_SEND,
	[LOOP_BODY]    = LOOP_STEP_BODY,
	[LOOP_PARSE]   = LOOP_STEP_PARSE,
	[LOOP_VALUES]  = LOOP_STEP_VALUES,
	[LOOP_ESCAPE]  = LOOP_STEP_ESCAPE,
	[LOOP_COPY]    = LOOP_STEP_COPY,
	[LOOP_ACCEPTS] = LOOP_STEP_ACCEPTS,
	[LOOP_CLOSES]  = LOOP_STEP_CLOSES,
	[LOOP_RELEASE] = LOOP_STEP_RELEASE,
};

static char const *const unit_name[LOOP_UNITS] = {
	[LOOP_REQS]    = "reqs",
	[LOOP_RECV]    = "recv",
	[LOOP_SEND]    = "send",
	[LOOP_BODY]    = "body",
	[LOOP_PARSE]   = "parse",
	[LOOP_VALUES]  = "values",
	[LOOP_ESCAPE]  = "escape",
	[LOOP_COPY]    = "copy",
	[LOOP_ACCEPTS] = "accepts",
	[LOOP_CLOSES]  = "closes",
	[LOOP_RELEASE] = "release",
};

/* Record @p ns in the statistics since the last report, and in @p metric. */
static void
timing_record (struct timing *t,
               struct hist   *metric,
               uint64_t       ns)
{
	hist_record(&t->hist, ns);
	t->over += ns > 1'000'000;
	hist_record(metric, ns);
}

static void
most (uint64_t *max,
      uint64_t  n)
{
	if (n > *max)
		*max = n;
}

uint64_t
loop_room (struct loop const *loop,
           enum loop_unit     u)
{
	return loop->work[u] < bound[u] ? bound[u] - loop->work[u] : 0;
}

uint64_t
loop_clock (void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1'000'000'000 + (uint64_t)ts.tv_nsec;
}

static void
loop_reap (struct loop *loop)
{
	while (!list_is_empty(&loop->dead)) {
		struct loop_ref *ref = container_of(loop->dead.next, struct loop_ref,
		                                    hook);
		list_del(&ref->hook);
		ref->release(ref);
	}
}

/* The inbox's eventfd: what was posted is taken every turn anyway. */
static void
loop_wake (struct loop     *loop,
           struct loop_ref *ref,
           uint32_t         events)
{
	(void)loop;
	(void)events;
	eventfd_t n;
	(void)eventfd_read(ref->fd, &n);
}

int
loop_init (struct loop *loop)
{
	*loop = (struct loop){
		.wake.fd = -1,
		.now = loop_clock(),
		.epfd = epoll_create1(EPOLL_CLOEXEC)
	};
	list_init(&loop->refs);
	list_init(&loop->dead);
	for (size_t i = 0; i < ARRAY_SIZE(loop->ready); ++i)
		list_init(&loop->ready[i]);
	if (loop->epfd < 0)
		return errno;

	int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (fd < 0)
		return errno;
	int e = loop_watch(loop, &loop->wake, fd, EPOLLIN, loop_wake, nullptr);
	if (e) {
		close(fd);
		return e;
	}
	loop->stats = calloc(1, sizeof *loop->stats);
	loop->metrics = calloc(1, sizeof *loop->metrics);
	return loop->stats && loop->metrics ? 0 : ENOMEM;
}

void
loop_post (struct loop *loop,
           struct msg  *msg)
{
	msg->posted = loop_clock();
	struct msg *head = atomic_load_explicit(&loop->inbox, memory_order_relaxed);
	do msg->next = head;
	while (!atomic_compare_exchange_weak(&loop->inbox, &head, msg));
	/* Sequentially consistent with the loop's own store to asleep and its
	 * look at the inbox: either it sees this message, or this sees it
	 * asleep (design section 2). */
	if (atomic_exchange(&loop->asleep, 0))
		(void)eventfd_write(loop->wake.fd, 1);
}

/* Take what was posted, oldest first, behind what earlier turns left. */
static void
loop_collect (struct loop *loop)
{
	struct msg *msg = atomic_load_explicit(&loop->inbox, memory_order_relaxed)
	                ? atomic_exchange(&loop->inbox, nullptr) : nullptr;
	if (msg) {
		/* The newest comes first: reversed, the first is the last. */
		struct msg *last = msg;
		struct msg *fifo = nullptr;
		while (msg) {
			struct msg *next = msg->next;
			msg->next = fifo;
			fifo = msg;
			msg = next;
		}
		if (loop->mail_last)
			loop->mail_last->next = fifo;
		else
			loop->mail = fifo;
		loop->mail_last = last;
	}
}

/* Hand the oldest message taken to its handler. */
static void
loop_deliver (struct loop *loop)
{
	struct msg *msg = loop->mail;
	if (!(loop->mail = msg->next))
		loop->mail_last = nullptr;
	msg->fn(loop, msg);
}

void
loop_fini (struct loop *loop)
{
	loop->exit = true;
	for (loop_collect(loop); loop->mail; loop_collect(loop))
		loop_deliver(loop);

	int wake = loop->wake.fd;
	struct list *node;
	struct list *tmp;
	list_foreach_safe (node, tmp, &loop->refs) {
		struct loop_ref *ref = container_of(node, struct loop_ref, hook);
		loop_unwatch(loop, ref);
	}
	loop_reap(loop);
	for (size_t i = 0; i < ARRAY_SIZE(loop->ready); ++i)
		while (!list_is_empty(&loop->ready[i]))
			loop_unready(container_of(loop->ready[i].next, struct loop_step,
			                          hook));
	for (size_t i = 0; i < loop->timers; ++i)
		loop->heap[i]->slot = 0;

	if (wake >= 0)
		close(wake);
	if (loop->epfd >= 0)
		close(loop->epfd);
	free(loop->heap);
	free(loop->stats);
	free(loop->metrics);
	*loop = (struct loop){ .wake.fd = -1, .epfd = -1 };
}

int
loop_watch (struct loop     *loop,
            struct loop_ref *ref,
            int              fd,
            uint32_t         events,
            loop_cb         *cb,
            void            *owner)
{
	if (!ref || !cb || fd < 0)
		return EINVAL;

	struct epoll_event event = {
		.events = events,
		.data.ptr = ref
	};
	if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &event))
		return errno;

	*ref = (struct loop_ref){
		.cb = cb,
		.owner = owner,
		.events = events,
		.fd = fd
	};
	list_init(&ref->hook);
	list_append(&loop->refs, &ref->hook);
	return 0;
}

int
loop_modify (struct loop     *loop,
             struct loop_ref *ref,
             uint32_t         events)
{
	if (!ref || ref->fd < 0)
		return EINVAL;
	if (ref->events == events)
		return 0;

	struct epoll_event event = {
		.events = events,
		.data.ptr = ref
	};
	if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, ref->fd, &event))
		return errno;

	ref->events = events;
	return 0;
}

void
loop_unwatch (struct loop     *loop,
              struct loop_ref *ref)
{
	if (!ref || ref->fd < 0)
		return;

	if (epoll_ctl(loop->epfd, EPOLL_CTL_DEL, ref->fd, nullptr) &&
	    errno != ENOENT && errno != EBADF && !loop->error)
		loop->error = errno;

	if (ref->hook.next && ref->hook.prev)
		list_del(&ref->hook);
	*ref = (struct loop_ref){ .fd = -1 };
}

void
loop_release (struct loop     *loop,
              struct loop_ref *ref,
              loop_release_cb *release)
{
	loop_unwatch(loop, ref);
	ref->release = release;
	list_append(&loop->dead, &ref->hook);
}

void
loop_ready (struct loop      *loop,
            struct loop_step *step)
{
	if (!step->hook.next)
		list_append(&loop->ready[loop->stepping ? ONGOING : FRESH], &step->hook);
}

void
loop_ready_bulk (struct loop      *loop,
                 struct loop_step *step)
{
	if (!step->hook.next)
		list_append(&loop->ready[BULK], &step->hook);
}

void
loop_unready (struct loop_step *step)
{
	if (step->hook.next) {
		list_del(&step->hook);
		step->hook = (struct list){};
	}
}

static void
heap_put (struct loop       *loop,
          struct loop_timer *timer,
          size_t             i)
{
	loop->heap[i] = timer;
	timer->slot = i + 1;
}

/* Restore the heap order around @p timer, which is in the heap. */
static void
heap_fix (struct loop       *loop,
          struct loop_timer *timer)
{
	size_t i = timer->slot - 1;
	while (i && loop->heap[(i - 1) / 4]->due > timer->due) {
		heap_put(loop, loop->heap[(i - 1) / 4], i);
		i = (i - 1) / 4;
	}
	for (size_t c; (c = 4 * i + 1) < loop->timers; i = c) {
		size_t end = c + 4 < loop->timers ? c + 4 : loop->timers;
		for (size_t k = c + 1; k < end; ++k)
			if (loop->heap[k]->due < loop->heap[c]->due)
				c = k;
		if (loop->heap[c]->due >= timer->due)
			break;
		heap_put(loop, loop->heap[c], i);
	}
	heap_put(loop, timer, i);
}

int
loop_timer_set (struct loop       *loop,
                struct loop_timer *timer,
                uint64_t           due)
{
	if (!timer->slot) {
		if (loop->timers == loop->heap_cap) {
			size_t cap = loop->heap_cap ? 2 * loop->heap_cap : HEAP_MIN;
			struct loop_timer **heap = realloc(loop->heap, cap * sizeof *heap);
			if (!heap)
				return ENOMEM;
			loop->heap = heap;
			loop->heap_cap = cap;
		}
		heap_put(loop, timer, loop->timers++);
	}
	timer->due = due;
	heap_fix(loop, timer);
	return 0;
}

void
loop_timer_stop (struct loop       *loop,
                 struct loop_timer *timer)
{
	if (!timer->slot)
		return;
	size_t i = timer->slot - 1;
	timer->slot = 0;
	struct loop_timer *last = loop->heap[--loop->timers];
	if (i < loop->timers) {
		heap_put(loop, last, i);
		heap_fix(loop, last);
	}
}

/* Fire up to TIMERS timers due by the start of the turn; returns how many. */
static unsigned
loop_fire (struct loop *loop)
{
	unsigned n = 0;
	for (; n < TIMERS && loop->timers && !loop->exit; ++n) {
		struct loop_timer *timer = loop->heap[0];
		if (timer->due > loop->now)
			break;
		timing_record(&loop->stats->late, &loop->metrics->late,
		              loop->now - timer->due);
		timer->due = UINT64_MAX;
		heap_fix(loop, timer);
		timer->fn(loop, timer);
	}
	return n;
}

/*
 * Give the entries queued on the @p n lists from @p lists one step each, a
 * step from each list in turn, while the clock is short of @p until, or at
 * least one if @p bulk.  Entries not reached go back to the head of their
 * lists, ahead of steps queued meanwhile.  Returns how many steps ran.
 */
static uint64_t
loop_run (struct loop *loop,
          struct list *lists,
          size_t       n,
          uint64_t     until,
          bool         bulk)
{
	struct list batch[2];
	for (size_t i = 0; i < n; ++i) {
		list_init(&batch[i]);
		list_transplant(&batch[i], &lists[i]);
	}

	uint64_t t = loop_clock();
	uint64_t steps = 0;
	bool first = bulk;
	for (size_t i = 0, empty = 0; empty < n && !loop->exit && (first || t < until);
	     i = (i + 1) % n) {
		if (list_is_empty(&batch[i])) {
			++empty;
			continue;
		}
		empty = 0;
		first = false;
		struct loop_step *step = container_of(batch[i].next, struct loop_step,
		                                      hook);
		loop_unready(step);
		for (size_t u = 0; u < LOOP_UNITS; ++u)
			loop->work[u] = 0;
		step->fn(loop, step);
		uint64_t end = loop_clock();
		timing_record(&loop->stats->step[bulk], &loop->metrics->step[bulk], end - t);
		for (size_t u = 0; u < LOOP_UNITS; ++u) {
			most(&loop->stats->work[u], loop->work[u]);
			if (loop->work[u] > hist_get(&loop->metrics->work[u]))
				atomic_store_explicit(&loop->metrics->work[u], loop->work[u],
				                      memory_order_relaxed);
		}
		t = end;
		++steps;
	}
	for (size_t i = 0; i < n; ++i) {
		list_transplant(&batch[i], &lists[i]);
		list_transplant(&lists[i], &batch[i]);
	}
	return steps;
}

/* Do not sleep while steps or messages are queued; otherwise until the next
 * timer. */
static struct timespec *
loop_timeout (struct loop const *loop,
              struct timespec   *ts)
{
	uint64_t ns = 0;
	if (list_is_empty(&loop->ready[FRESH]) && list_is_empty(&loop->ready[ONGOING]) &&
	    list_is_empty(&loop->ready[BULK]) && !loop->mail) {
		if (!loop->timers || loop->heap[0]->due == UINT64_MAX)
			return nullptr;
		uint64_t now = loop_clock();
		ns = loop->heap[0]->due > now ? loop->heap[0]->due - now : 0;
	}
	*ts = (struct timespec){
		.tv_sec = (time_t)(ns / 1'000'000'000),
		.tv_nsec = (long)(ns % 1'000'000'000)
	};
	return ts;
}

void
loop_log_stats (struct loop *loop)
{
	struct loop_stats *s = loop->stats;
	struct {
		char const          *name;
		struct timing const *timing;
	} const lines[] = {
		{ "turn",        &s->turn },
		{ "interactive", &s->step[0] },
		{ "bulk",        &s->step[1] },
		{ "timer_late",  &s->late },
	};
	for (size_t i = 0; i < ARRAY_SIZE(lines); ++i) {
		struct hist const *h = &lines[i].timing->hist;
		log_put(nullptr, "stats %s n=%" PRIu64 " over_1ms=%" PRIu64 " p50_us=%.1f "
		        "p90_us=%.1f p99_us=%.1f p99_9_us=%.1f max_us=%.1f", lines[i].name,
		        hist_get(&h->count), lines[i].timing->over,
		        (double)hist_pct(h, 0.5) / 1e3, (double)hist_pct(h, 0.9) / 1e3,
		        (double)hist_pct(h, 0.99) / 1e3, (double)hist_pct(h, 0.999) / 1e3,
		        (double)hist_get(&h->max) / 1e3);
	}
	char work[512];
	int n = snprintf(work, sizeof work, "events=%" PRIu64 "/%u timers=%" PRIu64
	                 "/%u steps=%" PRIu64, s->events, (unsigned)EVENTS, s->timers,
	                 (unsigned)TIMERS, s->steps);
	for (size_t u = 0; u < LOOP_UNITS && n > 0 && (size_t)n < sizeof work; ++u)
		n += snprintf(work + n, sizeof work - (size_t)n, " %s=%" PRIu64 "/%" PRIu64,
		              unit_name[u], s->work[u], bound[u]);
	log_put(nullptr, "stats work %s", work);
	memset(s, 0, sizeof *s);
}

int
loop_exec (struct loop *loop)
{
	/* Wake on time: the default 50 µs of timer slack would be latency. */
	(void)prctl(PR_SET_TIMERSLACK, 1UL);

	while (!loop->exit) {
		struct epoll_event events[EVENTS];
		struct timespec ts;
		struct timespec *timeout = loop_timeout(loop, &ts);
		/* About to sleep: say so, then look at the inbox once more
		 * (design section 2). */
		if (!timeout || ts.tv_sec || ts.tv_nsec) {
			atomic_store(&loop->asleep, 1);
			if (atomic_load(&loop->inbox)) {
				ts = (struct timespec){};
				timeout = &ts;
			}
		}
		atomic_store_explicit(&loop->turn, 0, memory_order_relaxed);
		int n = epoll_pwait2(loop->epfd, events, EVENTS, timeout, nullptr);
		/* A poster that still sees it asleep writes the eventfd for
		 * nothing, which costs a turn. */
		atomic_store_explicit(&loop->asleep, 0, memory_order_relaxed);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}

		uint64_t start = loop->now = loop_clock();
		atomic_store_explicit(&loop->turn, start, memory_order_relaxed);
		for (int i = 0; i < n && !loop->exit; ++i) {
			struct loop_ref *ref = events[i].data.ptr;
			if (ref && ref->cb)
				ref->cb(loop, ref, events[i].events);
		}
		loop_collect(loop);
		for (unsigned i = 0; i < MAIL && loop->mail && !loop->exit; ++i) {
			uint64_t posted = loop->mail->posted;
			uint64_t now = loop_clock();
			hist_record(&loop->metrics->inbox, now > posted ? now - posted : 0);
			loop_deliver(loop);
		}
		struct loop_stats *s = loop->stats;
		most(&s->events, (uint64_t)n);
		most(&s->timers, loop_fire(loop));

		loop->stepping = true;
		uint64_t steps = loop_run(loop, &loop->ready[FRESH], 2,
		                          start + INTERACTIVE_NS, false);
		steps += loop_run(loop, &loop->ready[BULK], 1, start + BULK_NS, true);
		loop->stepping = false;
		most(&s->steps, steps);
		loop_reap(loop);
		timing_record(&s->turn, &loop->metrics->turn, loop_clock() - start);
	}

	/* What follows the last turn is no turn to a watchdog. */
	atomic_store_explicit(&loop->turn, 0, memory_order_relaxed);
	return loop->error;
}

void
loop_exit (struct loop *loop)
{
	loop->exit = true;
}
