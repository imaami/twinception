/** @file loop.c
 *
 * Event loop: epoll, a 4-ary timer heap, budgeted ready lists and an
 * inbox.
 */
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
	SLOTS_MIN      = 64,       /* the same for the table of handles */
	SLOTS_MAX      = 1 << 24,  /* as many as a handle's slot field holds */
};

/* A slot of the table of handles. */
struct loop_slot {
	void    *obj;         /* what it names; nullptr while free */
	uint32_t gen;         /* the generation its handles carry, never 0 */
	union {
		uint32_t kind;    /* while in use: the kind of obj, a number that
		                     loop_handle() stores and loop_lookup()
		                     compares */
		uint32_t vacant;  /* while free: the slot freed before it + 1, or
		                     0 */
	};
};

/* What the loop times, in ns: a turn from epoll returning to its end, an
 * interactive step and a bulk one (TIME_STEP + bulk), and how long after its
 * due time a timer fired.
 */
enum { TIME_TURN, TIME_STEP, TIME_BULK, TIME_LATE, TIMINGS };

/* The statistics since the last report.  The histograms come first and the
 * plain counts after them, so that a new window wipes the counts with one
 * memset() and clears the histograms by atomic stores (loop_stats_empty()).
 */
struct loop_stats {
	struct hist time[TIMINGS];
	uint64_t    over[TIMINGS];     /* those over 1 ms, counted exactly, as the
	                                  histogram's bucket around 1 ms
	                                  straddles it */
	uint64_t    work[LOOP_UNITS];  /* the most a step did */
	uint64_t    events;            /* the most a turn had */
	uint64_t    timers;
	uint64_t    steps;
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
	[LOOP_TASKS]   = LOOP_STEP_TASKS,
	[LOOP_FLIGHTS] = LOOP_STEP_FLIGHTS,
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
	[LOOP_TASKS]   = "tasks",
	[LOOP_FLIGHTS] = "flights",
};

/* Record @p ns as timing @p t in the statistics since the last report, and in
 * @p metric.
 */
static void
loop_stats_time (struct loop_stats *s,
                 size_t             t,
                 struct hist       *metric,
                 uint64_t           ns)
{
	hist_record(&s->time[t], ns);
	s->over[t] += ns > 1'000'000;
	hist_record(metric, ns);
}

/* Empty @p s for a window of statistics: if it was just allocated, its
 * histograms are initialized, and otherwise only cleared.
 */
static void
loop_stats_empty (struct loop_stats *s,
                  bool               fresh)
{
	for (size_t t = 0; t < TIMINGS; ++t)
		(fresh ? hist_init : hist_clear)(&s->time[t]);
	size_t at = offsetof(struct loop_stats, over);
	memset((unsigned char *)s + at, 0, sizeof *s - at);
}

static void
loop_metrics_init (struct loop_metrics *m)
{
	hist_init(&m->turn);
	for (size_t i = 0; i < ARRAY_SIZE(m->step); ++i)
		hist_init(&m->step[i]);
	hist_init(&m->late);
	hist_init(&m->inbox);
	for (size_t u = 0; u < LOOP_UNITS; ++u)
		atomic_init(&m->work[u], 0);
}

static void
loop_most (uint64_t *max,
           uint64_t  n)
{
	if (n > *max)
		*max = n;
}

uint64_t
loop_now (struct loop const *loop)
{
	return loop ? loop->now : 0;
}

int
loop_work (struct loop    *loop,
           enum loop_unit  u,
           uint64_t        n)
{
	if (!loop)
		return EFAULT;
	if ((size_t)u >= LOOP_UNITS)
		return EINVAL;
	loop->work[u] += n;
	return 0;
}

uint64_t
loop_room (struct loop const *loop,
           enum loop_unit     u)
{
	if (!loop || (size_t)u >= LOOP_UNITS)
		return 0;
	return loop->work[u] < bound[u] ? bound[u] - loop->work[u] : 0;
}

uint64_t
loop_clock (void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1'000'000'000 + (uint64_t)ts.tv_nsec;
}

void
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

struct loop
loop (void)
{
	struct loop ret = {
		.wake.fd = -1,
		.epfd = -1
	};
	return ret;
}

/* Blank all but @p loop's atomic members, which come first (loop.h). */
static void
loop_blank (struct loop *loop)
{
	size_t at = offsetof(struct loop, refs);
	memset((unsigned char *)loop + at, 0, sizeof *loop - at);
	loop->wake.fd = -1;
	loop->epfd = -1;
}

void
loop_place (struct loop *loop)
{
	atomic_init(&loop->turn, 0);
	atomic_init(&loop->asleep, 0);
	atomic_init(&loop->inbox, nullptr);
	loop->id = 0;
	loop_blank(loop);
}

int
loop_init (struct loop *loop)
{
	atomic_store_explicit(&loop->turn, 0, memory_order_relaxed);
	atomic_store_explicit(&loop->asleep, 0, memory_order_relaxed);
	atomic_store_explicit(&loop->inbox, nullptr, memory_order_relaxed);
	loop_blank(loop);
	loop->now = loop_clock();
	list_init(&loop->refs);
	list_init(&loop->dead);
	for (size_t i = 0; i < ARRAY_SIZE(loop->ready); ++i)
		list_init(&loop->ready[i]);
	loop->epfd = epoll_create1(EPOLL_CLOEXEC);
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
	loop->stats = malloc(sizeof *loop->stats);
	loop->metrics = malloc(sizeof *loop->metrics);
	if (!loop->stats || !loop->metrics)
		return ENOMEM;
	loop_stats_empty(loop->stats, true);
	loop_metrics_init(loop->metrics);
	return 0;
}

int
loop_post (struct loop *loop,
           struct msg  *msg)
{
	if (!loop || !msg || !msg->fn)
		return EFAULT;
	uint64_t const posted = loop_clock();
	struct msg *head = atomic_load_explicit(&loop->inbox, memory_order_relaxed);
	do {
		/* The newest message of the inbox is posted already.  Posting it
		 * again would make it its own next message, and the inbox a
		 * cycle. */
		if (head == msg)
			return EALREADY;
		msg->next = head;
		msg->posted = posted;
	} while (!atomic_compare_exchange_weak(&loop->inbox, &head, msg));
	/* Sequentially consistent with the loop's own store to asleep and its
	 * look at the inbox: either it sees this message, or this sees it
	 * asleep (design section 2). */
	if (atomic_exchange(&loop->asleep, 0))
		(void)eventfd_write(loop->wake.fd, 1);
	return 0;
}

/* Take what was posted, oldest first, behind what earlier turns left. */
static void
loop_collect (struct loop *loop)
{
	/* Only this loop empties the inbox: once seen full, it stays so. */
	if (!atomic_load_explicit(&loop->inbox, memory_order_relaxed))
		return;
	struct msg *msg = atomic_exchange(&loop->inbox, nullptr);
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
	free(loop->slots);
	free(loop->stats);
	free(loop->metrics);
	loop_blank(loop);
}

/* The slot @p h names, if it names one of @p loop's; else nullptr. */
static struct loop_slot *
loop_slot_of (struct loop const *loop,
              struct handle      h)
{
	uint32_t i = (uint32_t)(h.bits >> 32) & (SLOTS_MAX - 1);
	if (h.bits >> 56 != loop->id || i >= loop->used)
		return nullptr;
	struct loop_slot *slot = &loop->slots[i];
	return slot->gen == (uint32_t)h.bits && slot->obj ? slot : nullptr;
}

/* Room in the table of handles for a slot never used; 0 or ENOMEM. */
static int
loop_slots_grow (struct loop *loop)
{
	if (loop->used < loop->slots_cap)
		return 0;
	size_t cap = loop->slots_cap ? 2 * loop->slots_cap : SLOTS_MIN;
	struct loop_slot *slots = cap > SLOTS_MAX ? nullptr
	                        : realloc(loop->slots, cap * sizeof *slots);
	if (!slots)
		return ENOMEM;
	loop->slots = slots;
	loop->slots_cap = cap;
	return 0;
}

int
loop_handle (struct loop   *loop,
             void          *obj,
             uint32_t       kind,
             struct handle *h)
{
	uint32_t i = loop->vacant;
	if (i--) {
		loop->vacant = loop->slots[i].vacant;
	} else {
		int e = loop_slots_grow(loop);
		if (e)
			return e;
		i = loop->used++;
		loop->slots[i].gen = 1;
	}
	loop->slots[i].obj = obj;
	loop->slots[i].kind = kind;
	*h = (struct handle){ loop->id << 56 | (uint64_t)i << 32 | loop->slots[i].gen };
	return 0;
}

void *
loop_lookup (struct loop const *loop,
             struct handle      h,
             uint32_t           kind)
{
	struct loop_slot const *slot = loop_slot_of(loop, h);
	return slot && slot->kind == kind ? slot->obj : nullptr;
}

void
loop_forget (struct loop   *loop,
             struct handle  h)
{
	struct loop_slot *slot = loop_slot_of(loop, h);
	if (!slot)
		return;
	slot->obj = nullptr;
	slot->gen += 1 + (slot->gen == UINT32_MAX);
	slot->vacant = loop->vacant;
	loop->vacant = (uint32_t)(slot - loop->slots) + 1;
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
loop_heap_put (struct loop       *loop,
               struct loop_timer *timer,
               size_t             i)
{
	loop->heap[i] = timer;
	timer->slot = i + 1;
}

/* Restore the heap order around @p timer, which is in the heap. */
static void
loop_heap_fix (struct loop       *loop,
               struct loop_timer *timer)
{
	size_t i = timer->slot - 1;
	while (i && loop->heap[(i - 1) / 4]->due > timer->due) {
		loop_heap_put(loop, loop->heap[(i - 1) / 4], i);
		i = (i - 1) / 4;
	}
	for (size_t c; (c = 4 * i + 1) < loop->timers; i = c) {
		size_t end = c + 4 < loop->timers ? c + 4 : loop->timers;
		for (size_t k = c + 1; k < end; ++k)
			if (loop->heap[k]->due < loop->heap[c]->due)
				c = k;
		if (loop->heap[c]->due >= timer->due)
			break;
		loop_heap_put(loop, loop->heap[c], i);
	}
	loop_heap_put(loop, timer, i);
}

int
loop_timer_set (struct loop       *loop,
                struct loop_timer *timer,
                uint64_t           due)
{
	if (!loop || !timer || !timer->fn)
		return EFAULT;
	/* A timer with a stale slot would take another timer's place. */
	if (timer->slot > loop->timers ||
	    (timer->slot && loop->heap[timer->slot - 1] != timer))
		return EINVAL;
	if (!timer->slot) {
		if (loop->timers == loop->heap_cap) {
			size_t cap = loop->heap_cap ? 2 * loop->heap_cap : HEAP_MIN;
			struct loop_timer **heap = realloc(loop->heap, cap * sizeof *heap);
			if (!heap)
				return ENOMEM;
			loop->heap = heap;
			loop->heap_cap = cap;
		}
		loop_heap_put(loop, timer, loop->timers++);
	}
	timer->due = due;
	loop_heap_fix(loop, timer);
	return 0;
}

int
loop_timer_stop (struct loop       *loop,
                 struct loop_timer *timer)
{
	if (!loop || !timer)
		return EFAULT;
	if (!timer->slot)
		return 0;
	/* A timer with a stale slot would take another timer out of the
	 * heap. */
	if (timer->slot > loop->timers || loop->heap[timer->slot - 1] != timer)
		return EINVAL;
	size_t i = timer->slot - 1;
	timer->slot = 0;
	struct loop_timer *last = loop->heap[--loop->timers];
	if (i < loop->timers) {
		loop_heap_put(loop, last, i);
		loop_heap_fix(loop, last);
	}
	return 0;
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
		loop_stats_time(loop->stats, TIME_LATE, &loop->metrics->late,
		                loop->now - timer->due);
		timer->due = UINT64_MAX;
		loop_heap_fix(loop, timer);
		timer->fn(loop, timer);
	}
	return n;
}

/*
 * Run one step of each entry that is queued on the @p n lists in the array
 * @p lists.  While more than one list has entries, the steps alternate
 * between the lists.  The first step is from the list at index @p *cursor,
 * unless that list is empty.  If an entry is queued, at least one step
 * runs, and the function starts more steps only while the clock is before
 * @p until.  @p bulk selects the histogram in which the function records
 * the times of the steps.  The function puts the entries that did not run
 * back at the head of their lists, before the entries that were queued
 * during the call.  On return, @p *cursor is the index of the list after
 * the list of the last step, so the next call continues the alternation.
 * Returns the number of steps that ran.
 */
static uint64_t
loop_run (struct loop *loop,
          struct list *lists,
          size_t       n,
          size_t      *cursor,
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
	size_t i = *cursor;
	for (size_t empty = 0; empty < n && !loop->exit && (!steps || t < until);
	     i = (i + 1) % n) {
		if (list_is_empty(&batch[i])) {
			++empty;
			continue;
		}
		empty = 0;
		struct loop_step *step = container_of(batch[i].next, struct loop_step,
		                                      hook);
		loop_unready(step);
		for (size_t u = 0; u < LOOP_UNITS; ++u)
			loop->work[u] = 0;
		step->fn(loop, step);
		uint64_t end = loop_clock();
		loop_stats_time(loop->stats, TIME_STEP + bulk, &loop->metrics->step[bulk],
		                end - t);
		for (size_t u = 0; u < LOOP_UNITS; ++u) {
			loop_most(&loop->stats->work[u], loop->work[u]);
			if (loop->work[u] > hist_get(&loop->metrics->work[u]))
				atomic_store_explicit(&loop->metrics->work[u], loop->work[u],
				                      memory_order_relaxed);
		}
		t = end;
		++steps;
	}
	*cursor = i;
	for (i = 0; i < n; ++i) {
		list_transplant(&batch[i], &lists[i]);
		list_transplant(&lists[i], &batch[i]);
	}
	return steps;
}

/* Do not sleep while steps or messages are queued; otherwise until the next
 * timer.
 */
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
loop_log_stats (struct loop *loop,
                char const  *label)
{
	static char const *const name[TIMINGS] = {
		[TIME_TURN] = "turn",
		[TIME_STEP] = "interactive",
		[TIME_BULK] = "bulk",
		[TIME_LATE] = "timer_late",
	};
	struct loop_stats *s = loop->stats;
	for (size_t t = 0; t < TIMINGS; ++t) {
		struct hist const *h = &s->time[t];
		log_put(nullptr, "stats %s%s n=%" PRIu64 " over_1ms=%" PRIu64 " p50_us=%.1f "
		        "p90_us=%.1f p99_us=%.1f p99_9_us=%.1f max_us=%.1f", label, name[t],
		        hist_get(&h->count), s->over[t],
		        (double)hist_pct(h, 0.5) / 1e3, (double)hist_pct(h, 0.9) / 1e3,
		        (double)hist_pct(h, 0.99) / 1e3, (double)hist_pct(h, 0.999) / 1e3,
		        (double)hist_get(&h->max) / 1e3);
	}
	char work[512];
	size_t len = util_format(work, sizeof work, "events=%" PRIu64 "/%u timers=%" PRIu64
	                         "/%u steps=%" PRIu64, s->events, (unsigned)EVENTS,
	                         s->timers, (unsigned)TIMERS, s->steps);
	/* Once cut short, with "...", the line says no more. */
	for (size_t u = 0; u < LOOP_UNITS && len < sizeof work - 1; ++u)
		len += util_format(work + len, sizeof work - len, " %s=%" PRIu64 "/%" PRIu64,
		                   unit_name[u], s->work[u], bound[u]);
	log_put(nullptr, "stats %swork %s", label, work);
	loop_stats_empty(s, false);
}

int
loop_exec (struct loop *loop)
{
	/* Wake on time: the default 50 µs of timer slack would be latency. */
	(void)prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);

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
		loop_most(&s->events, (uint64_t)n);
		loop_most(&s->timers, loop_fire(loop));

		loop->stepping = true;
		uint64_t steps = loop_run(loop, &loop->ready[FRESH], 2, &loop->cursor,
		                          start + INTERACTIVE_NS, false);
		steps += loop_run(loop, &loop->ready[BULK], 1, &(size_t){},
		                  start + BULK_NS, true);
		loop->stepping = false;
		loop_most(&s->steps, steps);
		loop_reap(loop);
		loop_stats_time(s, TIME_TURN, &loop->metrics->turn, loop_clock() - start);
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
