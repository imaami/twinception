/** @file Event loop: epoll, a timer heap, two budgeted ready lists and an
 *  inbox that other threads post to. */
#ifndef TWINCEPTION_SRC_LOOP_H_
#define TWINCEPTION_SRC_LOOP_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "hist.h"
#include "list.h"

struct loop;
struct loop_ref;
struct loop_stats;

/**
 * The units of work a step may do, and at most how many of each (design
 * section 1): what keeps a turn short however slow the machine.  Steps count
 * their work with loop_work(), may ask loop_room() what is left of the
 * step's share, and leave the rest to a later step.  Work that cannot be
 * split, such as a copy a library makes, counts in full where it happens.
 * The statistics keep the most of each unit any step did, which must stay
 * within its bound.
 */
enum loop_unit {
	LOOP_REQS,     /**< requests begun, or tool calls a drain ends */
	LOOP_RECV,     /**< bytes received */
	LOOP_SEND,     /**< bytes sent, or queued to send by an application */
	LOOP_BODY,     /**< request body bytes taken in and pre-scanned */
	LOOP_PARSE,    /**< JSON bytes parsed */
	LOOP_VALUES,   /**< JSON values parsed or freed */
	LOOP_ESCAPE,   /**< bytes escaped into JSON */
	LOOP_COPY,     /**< bytes copied whole, as json-c does a string it
	                    parsed, or an answer's text */
	LOOP_ACCEPTS,  /**< connections accepted */
	LOOP_CLOSES,   /**< connections closed */
	LOOP_RELEASE,  /**< blocks or objects kept for reuse that are given back
	                    to the allocator under memory pressure, a block a
	                    unit per 128 KiB (buf_stash_release()) */
	LOOP_UNITS
};

/** The bounds of the units per step. */
enum {
	LOOP_STEP_REQS    = 8,
	LOOP_STEP_RECV    = 64 << 10,
	LOOP_STEP_SEND    = 256 << 10,
	LOOP_STEP_BODY    = 64 << 10,
	LOOP_STEP_PARSE   = 16 << 10,
	LOOP_STEP_VALUES  = 256,
	LOOP_STEP_ESCAPE  = 64 << 10,
	LOOP_STEP_COPY    = 1 << 20,
	LOOP_STEP_ACCEPTS = 32,
	LOOP_STEP_CLOSES  = 32,
	LOOP_STEP_RELEASE = 256,
};

/**
 * Reports @p events on a watched descriptor.  A callback should only note
 * what happened and queue a step: the turn's budget bounds steps, and work
 * done in callbacks escapes it.
 */
typedef void loop_cb (struct loop     *loop,
                      struct loop_ref *ref,
                      uint32_t         events);

typedef void loop_release_cb (struct loop_ref *ref);

struct loop_ref {
	struct list      hook;
	loop_cb         *cb;
	loop_release_cb *release;
	void            *owner;
	uint32_t         events;
	int              fd;
};

/**
 * A bounded piece of work for a turn.  After its events and timers, a turn
 * gives each queued interactive entry one step until 500 µs have passed since
 * it began, then at least one bulk step, and more as long as they can end
 * within 1 ms.  Interactive entries queued by event and timer callbacks and
 * those queued by steps, such as a step queuing itself again, take turns, a
 * step each, starting with the former: fresh work does not wait behind
 * ongoing work, nor ongoing work behind a flood of fresh.  Entries a turn
 * does not reach go first in the next; a step queued during the turn's steps
 * waits for the next.
 */
struct loop_step {
	struct list hook;  /**< on a ready list; zeroed when off it */
	void      (*fn) (struct loop      *loop,
	                 struct loop_step *step);
};

/**
 * A message another thread posts to a loop (design section 2).  Its handler
 * runs on the loop in a later turn, and owns the message from then on.  The
 * poster must not touch it meanwhile, nor post it again: a message that may
 * be posted repeatedly carries a word of pending bits, and is posted only
 * when that word turns non-zero.
 */
struct msg {
	struct msg *next;
	void      (*fn) (struct loop *loop,
	                 struct msg  *msg);
	uint64_t    posted;  /**< loop_clock() as it was posted */
};

/** What a loop has done since it began: its one writer is the loop, and any
 *  thread may read it (src/hist.h).  Times are in ns. */
struct loop_metrics {
	struct hist      turn;
	struct hist      step[2];           /**< interactive, bulk */
	struct hist      late;              /**< how late timers fired */
	struct hist      inbox;             /**< from a post to its handler */
	_Atomic uint64_t work[LOOP_UNITS];  /**< the most of each unit a step did */
};

/** A deadline on the loop's timer heap: one per object, not one per event. */
struct loop_timer {
	uint64_t due;   /**< CLOCK_MONOTONIC ns; UINT64_MAX never fires */
	void   (*fn) (struct loop       *loop,
	              struct loop_timer *timer);
	size_t   slot;  /**< heap index + 1; 0 while not in the heap */
};

/**
 * An event loop.  Only its own thread touches it, but for loop_post(), which
 * any thread may call, and the fields marked as read elsewhere.
 */
struct loop {
	_Atomic uint64_t       turn;              /**< when the turn under way began, or 0
	                                               while waiting for events: read by
	                                               a watchdog */
	_Atomic uint64_t       asleep;            /**< bool, widened over the padding:
	                                               waiting with the inbox empty */
	_Atomic(struct msg *)  inbox;             /**< posted, newest first */
	/* The atomic members come first: loop_init() and loop_fini() blank the
	 * rest, from here on, with one memset(). */
	struct list            refs;
	struct list            dead;
	struct list            ready[3];          /**< interactive fresh and ongoing, bulk */
	struct loop_ref        wake;              /**< the inbox's eventfd */
	struct loop_timer    **heap;              /**< 4-ary min-heap by due time */
	struct loop_stats     *stats;
	struct loop_metrics   *metrics;           /**< read elsewhere */
	struct msg            *mail;              /**< taken from the inbox, oldest first */
	struct msg            *mail_last;
	size_t                 timers;            /**< armed, the heap's length */
	size_t                 heap_cap;
	uint64_t               now;               /**< when the turn began, CLOCK_MONOTONIC ns */
	uint64_t               work[LOOP_UNITS];  /**< done by the step under way */
	int                    epfd;
	int                    error;
	uint32_t               stepping;          /**< nonzero while steps run: theirs
	                                               are not fresh */
	uint32_t               exit;
};

/**
 * A loop at rest, to declare where it is to stay, `struct loop l = loop();`,
 * or as a member of a struct so declared: its atomic members are then valid
 * (hist.h), as loop_init() needs.
 */
extern struct loop
loop (void);

/**
 * Start @p loop, whose atomic members are valid: it was declared from loop(),
 * is of static or thread storage duration, or loop_fini() ended it.  It resets
 * them by atomic stores.  On failure, loop_fini() releases what it has.
 */
extern int
loop_init (struct loop *loop);

/**
 * Free the loop, after handling what its inbox still holds, its exit flag
 * set: a handler takes its message back then, and leaves the rest alone.
 * Its atomic members stay valid, and the rest is blanked.
 */
extern void
loop_fini (struct loop *loop);

/**
 * Post @p msg to @p loop, from any thread: an atomic push, and a write to the
 * loop's eventfd only if the loop sleeps.  It cannot fail.
 */
extern void
loop_post (struct loop *loop,
           struct msg  *msg);

extern int
loop_watch (struct loop     *loop,
            struct loop_ref *ref,
            int              fd,
            uint32_t         events,
            loop_cb         *cb,
            void            *owner);

extern int
loop_modify (struct loop     *loop,
             struct loop_ref *ref,
             uint32_t         events);

extern void
loop_unwatch (struct loop     *loop,
              struct loop_ref *ref);

/**
 * Unwatch @p ref and call @p release on it once the current batch of events
 * has been dispatched.  Events already fetched for @p ref are skipped, so its
 * owner may be freed by @p release without racing the dispatcher.
 */
extern void
loop_release (struct loop     *loop,
              struct loop_ref *ref,
              loop_release_cb *release);

/** Queue @p step for an interactive step, unless it is queued already. */
extern void
loop_ready (struct loop      *loop,
            struct loop_step *step);

/** Queue @p step for a bulk step, unless it is queued already. */
extern void
loop_ready_bulk (struct loop      *loop,
                 struct loop_step *step);

/** Take @p step off its list, if it is on one. */
extern void
loop_unready (struct loop_step *step);

/**
 * Arm @p timer to fire at @p due, or move it there.  A timer stays in the
 * heap until it is stopped; once it fires it is due at UINT64_MAX (never)
 * until set again.  So only arming a timer that is not in the heap can fail,
 * when the heap cannot grow (ENOMEM).
 */
extern int
loop_timer_set (struct loop       *loop,
                struct loop_timer *timer,
                uint64_t           due);

extern void
loop_timer_stop (struct loop       *loop,
                 struct loop_timer *timer);

/** Count @p n units of work @p u done by the step under way. */
static inline void
loop_work (struct loop    *loop,
           enum loop_unit  u,
           uint64_t        n)
{
	loop->work[u] += n;
}

/** How many units of work @p u the step under way has left. */
extern uint64_t
loop_room (struct loop const *loop,
           enum loop_unit     u);

/** CLOCK_MONOTONIC in nanoseconds. */
extern uint64_t
loop_clock (void);

/**
 * Log a line each for the turn times, the interactive and bulk step times
 * and the timers' lateness since the previous call, then one of the work
 * done: the most events, timers and steps in a turn, and the most of each
 * unit in a step against its bound, as "name=most/bound".  Each line starts
 * with "stats "; counting then starts afresh.
 */
extern void
loop_log_stats (struct loop *loop);

/**
 * Run turns until loop_exit().  Returns the error that stopped it, which only
 * epoll_pwait2() can cause, or else the first that epoll_ctl() reported.
 */
extern int
loop_exec (struct loop *loop);

extern void
loop_exit (struct loop *loop);

#endif /* TWINCEPTION_SRC_LOOP_H_ */
