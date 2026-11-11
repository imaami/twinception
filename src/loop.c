/** @file loop.c
 *
 * Event loop: epoll, a 4-ary timer heap, budgeted ready lists and an
 * inbox.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "clk.h"
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
	INTERACTIVE_NS = 500000,  /* interactive steps start until then */
	BULK_NS        = 650000,  /* further bulk steps: a step of up to 250 µs
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
	uint32_t gen;         /* the generation its handles carry, 0 if retired */
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

/* The statistics since the last report, after the metrics, which share
 * their allocation. The histograms come first and the plain counts after
 * them, so that a new window wipes the counts with one memset() and clears
 * the histograms by atomic stores (loop_stats_empty()).
 */
struct loop_stats {
	struct loop_metrics metrics;           /* loop->metrics points here */
	struct hist         time[TIMINGS];
	uint64_t            over[TIMINGS];     /* those over 1 ms, counted exactly,
	                                          as the histogram's bucket around
	                                          1 ms straddles it */
	uint64_t            work[LOOP_UNITS];  /* the most a step did */
	uint64_t            events;            /* the most a turn had */
	uint64_t            timers;
	uint64_t            steps;
};

#define LOOP_UNIT_NAME(id, name, bound) [LOOP_##id] = name,
static char const *const unit_name[LOOP_UNITS] = { LOOP_UNIT_LIST(LOOP_UNIT_NAME) };
#undef LOOP_UNIT_NAME

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
	s->over[t] += ns > MS;
	hist_record(metric, ns);
}

/* Empty @p s for a window of statistics: if it was just allocated, its
 * histograms are initialized, and otherwise only cleared.
 */
static void
loop_stats_empty (struct loop_stats *s,
                  bool               fresh)
{
	for (size_t t = 0; t < array_size(s->time); ++t)
		(fresh ? hist_init : hist_clear)(&s->time[t]);
	size_t at = offsetof(struct loop_stats, over);
	memset((unsigned char *)s + at, 0, sizeof *s - at);
}

static void
loop_metrics_init (struct loop_metrics *m)
{
	hist_init(&m->turn);
	for (size_t i = 0; i < array_size(m->step); ++i)
		hist_init(&m->step[i]);
	hist_init(&m->late);
	hist_init(&m->inbox);
	for (size_t u = 0; u < array_size(m->work); ++u)
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
	if ((size_t)u >= array_size(loop->work))
		return EINVAL;
	loop->work[u] += n;
	return 0;
}

uint64_t
loop_room (struct loop const *loop,
           enum loop_unit     u)
{
	if (!loop || (size_t)u >= array_size(loop_bound))
		return 0;
	return loop->work[u] < loop_bound[u] ? loop_bound[u] - loop->work[u] : 0;
}

bool
loop_lines_next (struct loop_lines *l,
                 struct loop       *loop,
                 struct cstr_view   text,
                 struct cstr_view  *line)
{
	uint64_t const room = loop_room(loop, LOOP_PARSE);
	size_t const n = text.len - l->at < room ? text.len - l->at : (size_t)room;
	char const *nl = n ? memchr(text.data + l->at, '\n', n) : nullptr;
	size_t const end = nl ? (size_t)(nl - text.data) : l->at + n;
	size_t const next = end + (nl != nullptr);
	loop_work(loop, LOOP_PARSE, next - l->at);
	l->at = next;
	if (!nl && (end < text.len || l->start == end))
		return false;
	*line = (struct cstr_view){ text.data + l->start, end - l->start };
	l->start = next;
	return true;
}

uint64_t
loop_clock (void)
{
	return clk_monotonic_ns();
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
	return (struct loop){ .wake.fd = -1, .epfd = -1 };
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
	for (size_t i = 0; i < array_size(loop->ready); ++i)
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
	if (!loop->stats)
		return ENOMEM;
	loop->metrics = &loop->stats->metrics;
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
		/* The newest message of the inbox is posted already. Posting it
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
	loop->flags |= LOOP_EXIT;
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
	for (size_t i = 0; i < array_size(loop->ready); ++i)
		while (!list_is_empty(&loop->ready[i]))
			loop_unready(container_of(loop->ready[i].next, struct loop_step,
			                          hook));
	for (size_t i = 0; i < loop->timers; ++i)
		loop->heap[i]->slot = 0;

	util_close(&wake);
	util_close(&loop->epfd);
	free(loop->heap);
	free(loop->slots);
	free(loop->stats);
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

/* Make room in the table of handles for a slot that was never used.
 * Returns 0 or ENOMEM.
 */
static int
loop_slots_grow (struct loop *loop)
{
	if (loop->used >= SLOTS_MAX)
		return ENOMEM;
	size_t n = loop->used < SLOTS_MIN ? SLOTS_MIN : loop->used + 1;
	return util_grow(loop->slots, loop->slots_cap, n);
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
	if (!++slot->gen)
		return;
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

	if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd,
	              &(struct epoll_event){ .events = events, .data.ptr = ref }))
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

	if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, ref->fd,
	              &(struct epoll_event){ .events = events, .data.ptr = ref }))
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
	    errno != ENOENT && errno != EBADF)
		loop_fail(loop, errno);

	if (ref->hook.next && ref->hook.prev)
		list_del(&ref->hook);
	*ref = (struct loop_ref){ .fd = -1 };
}

void
loop_close (struct loop     *loop,
            struct loop_ref *ref)
{
	int fd = ref->fd;
	if (fd < 0)
		return;
	loop_unwatch(loop, ref);
	(void)close(fd);
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
		list_append(&loop->ready[loop->flags & LOOP_STEPPING ? ONGOING : FRESH],
		            &step->hook);
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
	if (timer->slot > loop->timers)
		return EINVAL;
	if (!timer->slot) {
		size_t n = loop->timers < HEAP_MIN ? HEAP_MIN : loop->timers + 1;
		if (util_grow(loop->heap, loop->heap_cap, n))
			return ENOMEM;
		loop_heap_put(loop, timer, loop->timers++);
	} else if (loop->heap[timer->slot - 1] != timer) {
		/* timer->slot does not index this timer in the heap, so
		 * loop_heap_fix() would overwrite the entry of another timer. */
		return EINVAL;
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
	loop->heap[loop->timers] = nullptr;
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
	for (; n < TIMERS && loop->timers && !(loop->flags & LOOP_EXIT); ++n) {
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
 * @p lists. While more than one list has entries, the steps alternate
 * between the lists. The first step is from the list at index @p *cursor,
 * unless that list is empty. If an entry is queued, at least one step
 * runs, and the function starts more steps only while the clock is before
 * @p until. @p bulk selects the histogram in which the function records
 * the times of the steps. The function puts the entries that did not run
 * back at the head of their lists, before the entries that were queued
 * during the call. On return, @p *cursor is the index of the list after
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
	for (size_t empty = 0;
	     empty < n && !(loop->flags & LOOP_EXIT) && (!steps || t < until);
	     i = (i + 1) % n) {
		if (list_is_empty(&batch[i])) {
			++empty;
			continue;
		}
		empty = 0;
		struct loop_step *step = container_of(batch[i].next, struct loop_step,
		                                      hook);
		loop_unready(step);
		memset(loop->work, 0, sizeof loop->work);
		step->fn(loop, step);
		uint64_t end = loop_clock();
		loop_stats_time(loop->stats, TIME_STEP + bulk, &loop->metrics->step[bulk],
		                end - t);
		for (size_t u = 0; u < array_size(loop->work); ++u) {
			loop_most(&loop->stats->work[u], loop->work[u]);
			hist_max(&loop->metrics->work[u], loop->work[u]);
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

/* Returns ts with the time to wait for events, or nullptr for a wait without
 * a time limit. The time is 0 while steps or messages are queued. Otherwise
 * it ends when the next timer is due or at deadline, whichever comes first.
 */
static struct timespec *
loop_timeout (struct loop const *loop,
              uint64_t           deadline,
              struct timespec   *ts)
{
	uint64_t ns = 0;
	if (list_is_empty(&loop->ready[FRESH]) && list_is_empty(&loop->ready[ONGOING]) &&
	    list_is_empty(&loop->ready[BULK]) && !loop->mail) {
		uint64_t due = loop->timers && loop->heap[0]->due < deadline ? loop->heap[0]->due
		                                                             : deadline;
		if (due == UINT64_MAX)
			return nullptr;
		uint64_t now = loop_clock();
		ns = due > now ? due - now : 0;
	}
	*ts = clk_timespec(ns);
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
	for (size_t t = 0; t < array_size(s->time); ++t) {
		struct hist const *h = &s->time[t];
		log_put(nullptr, "stats %s%s n=%" PRIu64 " over_1ms=%" PRIu64 " p50_us=%.1f "
		        "p90_us=%.1f p99_us=%.1f p99_9_us=%.1f max_us=%.1f", label, name[t],
		        hist_get(&h->count), s->over[t],
		        (double)hist_pct(h, 0.5) / 1e3, (double)hist_pct(h, 0.9) / 1e3,
		        (double)hist_pct(h, 0.99) / 1e3, (double)hist_pct(h, 0.999) / 1e3,
		        (double)hist_get(&h->max) / 1e3);
	}
	/* A text cut short ends in "...", and util_text_printf() appends nothing
	 * after that. */
	UTIL_TEXT(512) work = {};
	(void)util_text_printf(&work, "events=%" PRIu64 "/%u timers=%" PRIu64 "/%u steps=%"
	                       PRIu64, s->events, (unsigned)EVENTS, s->timers,
	                       (unsigned)TIMERS, s->steps);
	for (size_t u = 0; u < array_size(s->work); ++u)
		(void)util_text_printf(&work, " %s=%" PRIu64 "/%" PRIu64, unit_name[u],
		                       s->work[u], loop_bound[u]);
	log_put(nullptr, "stats %swork %s", label, work.data);
	loop_stats_empty(s, false);
}

int
loop_turn (struct loop *loop,
           uint64_t     deadline)
{
	struct epoll_event events[EVENTS];
	struct timespec ts;
	struct timespec *timeout = loop_timeout(loop, deadline, &ts);
	/* Before a wait that can sleep, set asleep and check the inbox again. A
	 * post made before asleep was set did not write the eventfd (design
	 * section 2). */
	if (!timeout || ts.tv_sec || ts.tv_nsec) {
		atomic_store(&loop->asleep, 1);
		if (atomic_load(&loop->inbox)) {
			ts = (struct timespec){};
			timeout = &ts;
		}
	}
	atomic_store_explicit(&loop->turn, 0, memory_order_relaxed);
	int n = epoll_pwait2(loop->epfd, events, EVENTS, timeout, nullptr);
	/* A post between the end of the wait and this store still writes the
	 * eventfd, and the next wait then returns at once. */
	atomic_store_explicit(&loop->asleep, 0, memory_order_relaxed);
	if (n < 0)
		return errno == EINTR ? 0 : errno;

	uint64_t start = loop->now = loop_clock();
	atomic_store_explicit(&loop->turn, start, memory_order_relaxed);
	for (int i = 0; i < n && !(loop->flags & LOOP_EXIT); ++i) {
		struct loop_ref *ref = events[i].data.ptr;
		if (ref && ref->cb)
			ref->cb(loop, ref, events[i].events);
	}
	loop_collect(loop);
	for (unsigned i = 0; i < MAIL && loop->mail && !(loop->flags & LOOP_EXIT); ++i) {
		uint64_t posted = loop->mail->posted;
		uint64_t now = loop_clock();
		hist_record(&loop->metrics->inbox, now > posted ? now - posted : 0);
		loop_deliver(loop);
	}
	struct loop_stats *s = loop->stats;
	loop_most(&s->events, (uint64_t)n);
	loop_most(&s->timers, loop_fire(loop));

	loop->flags |= LOOP_STEPPING;
	uint64_t steps = loop_run(loop, &loop->ready[FRESH], 2, &loop->cursor,
	                          start + INTERACTIVE_NS, false);
	steps += loop_run(loop, &loop->ready[BULK], 1, &(size_t){}, start + BULK_NS, true);
	loop->flags &= ~LOOP_STEPPING;
	loop_most(&s->steps, steps);
	loop_reap(loop);
	loop_stats_time(s, TIME_TURN, &loop->metrics->turn, loop_clock() - start);
	return 0;
}

int
loop_exec (struct loop *loop)
{
	/* Set the timer slack of the thread to 1 ns. The default slack of 50 µs
	 * delays each timed wake-up. */
	(void)prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
	while (!(loop->flags & LOOP_EXIT)) {
		int e = loop_turn(loop, UINT64_MAX);
		if (e)
			return e;
	}
	/* Clear turn, so that a watchdog does not count the time after the last
	 * turn as a turn. */
	atomic_store_explicit(&loop->turn, 0, memory_order_relaxed);
	return loop->error;
}

void
loop_fail (struct loop *loop,
           int          e)
{
	if (e && !loop->error)
		loop->error = e;
}

void
loop_exit (struct loop *loop)
{
	loop->flags |= LOOP_EXIT;
}
