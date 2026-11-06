/** @file loop.h
 *
 * Event loop: epoll, a timer heap, two budgeted ready lists and an
 * inbox that other threads post to.
 */
#ifndef ASKAI_SRC_LOOP_H_
#define ASKAI_SRC_LOOP_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "cstr_view.h"
#include "hist.h"
#include "list.h"
#include "loop_export.h"

struct loop;
struct loop_ref;
struct loop_slot;
struct loop_stats;

/** The bound of each unit per step, LOOP_STEP_ID for LOOP_ID. */
#define LOOP_BOUND(id, name, bound) [LOOP_##id] = bound,
static constexpr uint64_t loop_bound[LOOP_UNITS] = { LOOP_UNIT_LIST(LOOP_BOUND) };
#undef LOOP_BOUND

/**
 * Reports @p events on a watched descriptor. A callback should only note
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
 * This structure is an entry on a ready list of the loop. A turn of the
 * loop calls the function fn of the entry to run one step, which is a
 * bounded piece of work. A turn first runs the callbacks of events,
 * messages and timers. Then it starts interactive steps, at most one for
 * each queued interactive entry, until 500 µs have passed since the turn
 * began. If an interactive entry is queued, at least one interactive step
 * runs, even when the callbacks ran for 500 µs or more. Then, if bulk
 * entries are queued, it runs at least one bulk step, and it starts more
 * bulk steps while they can end within 1 ms.
 *
 * loop_ready() appends an entry to the fresh list when a callback calls it,
 * and to the ongoing list when a step calls it, for example when a step
 * queues itself again. While both lists have entries, the steps alternate
 * between the two lists. A turn begins with the list after the list of the
 * last step in the previous turn, unless that list is empty. So the
 * alternation continues from one turn to the next, even when a turn has
 * time for only one interactive step.
 *
 * Entries that a turn does not run are at the head of their lists in the
 * next turn. An interactive entry that a step queues runs in the next turn
 * at the earliest.
 */
struct loop_step {
	struct list hook;  /**< on a ready list; zeroed when off it */
	void      (*fn) (struct loop      *loop,
	                 struct loop_step *step);
};

/** What a loop has done since it began: its one writer is the loop, and any
 *  thread may read it (src/hist.h). Times are in ns.
 */
struct loop_metrics {
	struct hist      turn;
	struct hist      step[2];           /**< interactive, bulk */
	struct hist      late;              /**< how late timers fired */
	struct hist      inbox;             /**< from a post to its handler */
	_Atomic uint64_t work[LOOP_UNITS];  /**< the most of each unit a step did */
};

/** The bits of loop.flags: loop_exit() or loop_fini() was called, and
 *  steps run, whose loop_ready() calls queue steps that are not fresh.
 */
enum { LOOP_EXIT = 1, LOOP_STEPPING = 2 };

/**
 * An event loop. Only its own thread touches it, but for loop_post(), which
 * any thread may call, and the fields marked as read elsewhere.
 */
struct loop {
	_Atomic uint64_t       turn;              /**< when the turn under way began, or 0
	                                               while waiting for events: read by
	                                               a watchdog */
	_Atomic uint64_t       asleep;            /**< bool, widened over the padding:
	                                               waiting with the inbox empty */
	_Atomic(struct msg *)  inbox;             /**< posted, newest first */
	uint64_t               id;                /**< which loop of the process, < 256,
	                                               that its handles name: set
	                                               before loop_init() */
	/* The atomic members and the id come first: loop_init() and loop_fini()
	 * blank the rest, from here on, with one memset(). */
	struct list            refs;
	struct list            dead;
	struct list            ready[3];          /**< interactive fresh and ongoing, bulk */
	struct loop_ref        wake;              /**< the inbox's eventfd */
	struct loop_timer    **heap;              /**< 4-ary min-heap by due time */
	struct loop_stats     *stats;
	struct loop_metrics   *metrics;           /**< read elsewhere */
	struct loop_slot      *slots;             /**< what its handles name */
	struct msg            *mail;              /**< taken from the inbox, oldest first */
	struct msg            *mail_last;
	size_t                 timers;            /**< armed, the heap's length */
	size_t                 heap_cap;
	size_t                 slots_cap;
	size_t                 cursor;            /**< the index in ready of the
	                                               interactive list of the
	                                               first step in the next turn,
	                                               unless that list is empty */
	uint64_t               now;               /**< when the turn began, CLOCK_MONOTONIC ns */
	uint64_t               work[LOOP_UNITS];  /**< done by the step under way */
	uint64_t               flags;             /**< LOOP_* bits, widened over the
	                                               padding */
	int                    epfd;
	int                    error;
	uint32_t               used;              /**< slots ever used */
	uint32_t               vacant;            /**< the last slot freed + 1, whose
	                                               own vacant links the next;
	                                               0: none */
};

/**
 * A loop at rest, to declare where it is to stay, `struct loop l = loop();`,
 * or as a member of a struct so declared: its atomic members are then valid
 * (hist.h), as loop_init() needs.
 */
extern struct loop
loop (void);

/**
 * Lay a loop at rest in @p loop, storage just allocated, as loop() would
 * give it: its atomic members are initialized in place (hist.h), once, so
 * that loop_init() may follow.
 */
extern void
loop_place (struct loop *loop);

/**
 * Start @p loop, whose atomic members are valid: it was declared from loop(),
 * laid with loop_place(), is of static or thread storage duration, or
 * loop_fini() ended it. It resets them by atomic stores. On failure,
 * loop_fini() releases what it has.
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
 * Stores in @p *h a handle that names @p obj for other threads. The handle
 * names a slot of the loop's table, which also stores @p kind. The kind is
 * a number that the caller chooses for each type of object that it names,
 * and loop_lookup() returns the object only for the same kind. The
 * function takes the slot that was freed last, if there is one, so the
 * table grows only past the largest number of objects named at once.
 * Returns 0, or ENOMEM if the table cannot grow, which it never does past
 * 2^24 slots.
 */
extern int
loop_handle (struct loop   *loop,
             void          *obj,
             uint32_t       kind,
             struct handle *h);

/** Returns the object that @p h names on @p loop if loop_handle() stored
 *  it with @p kind. Returns nullptr if the slot of @p h has been freed
 *  since the handle was made, if the handle is another loop's, or if the
 *  object has another kind.
 */
extern void *
loop_lookup (struct loop const *loop,
             struct handle      h,
             uint32_t           kind);

/** Free the slot @p h names, if it still does: its generation advances, so no
 *  copy of @p h names anything any more. A slot whose generation wraps to
 *  0 is retired: loop_handle() never uses it again.
 */
extern void
loop_forget (struct loop   *loop,
             struct handle  h);

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

/** Unwatch @p ref and close its descriptor, unless ref->fd is -1. */
extern void
loop_close (struct loop     *loop,
            struct loop_ref *ref);

/**
 * Unwatch @p ref and call @p release on it once the current batch of events
 * has been dispatched. Events already fetched for @p ref are skipped, so its
 * owner may be freed by @p release without racing the dispatcher.
 */
extern void
loop_release (struct loop     *loop,
              struct loop_ref *ref,
              loop_release_cb *release);

/** Call the release callbacks loop_release() deferred, now: only outside the
 *  dispatch of events, as each turn's end and loop_fini() do.
 */
extern void
loop_reap (struct loop *loop);

/** Queue @p step for an interactive step, unless it is queued already. */
extern void
loop_ready (struct loop      *loop,
            struct loop_step *step);

/** Queue @p step for a bulk step, unless it is queued already. */
extern void
loop_ready_bulk (struct loop      *loop,
                 struct loop_step *step);

/** Returns false if loop_room() for the unit @p u is not 0. Otherwise
 *  queues @p step for a bulk step and returns true, and the caller returns
 *  so that its work continues in that later step.
 */
static inline bool
loop_spent (struct loop      *loop,
            struct loop_step *step,
            enum loop_unit    u)
{
	if (loop_room(loop, u))
		return false;
	loop_ready_bulk(loop, step);
	return true;
}

/** Returns whether the step that is running may do @p n units of the work
 *  @p u now: loop_room() has room for them, or the step has done none of
 *  that work yet, so that work over a whole bound runs in a step of its
 *  own.
 */
static inline bool
loop_fits (struct loop const *loop,
           enum loop_unit     u,
           uint64_t           n)
{
	uint64_t const room = loop_room(loop, u);
	return room >= n || ((size_t)u < array_size(loop_bound) && room == loop_bound[u]);
}

/** Where loop_lines_next() is in a text that it splits into lines, the
 *  same text in each step. A zeroed one is at the start of the text.
 */
struct loop_lines {
	size_t at;     /**< the bytes of the text looked at */
	size_t start;  /**< where the next line begins */
};

/**
 * Stores in @p *line the next line of @p text, without its line feed, if
 * the LOOP_PARSE room of the step on @p loop reaches the end of the line,
 * and counts the bytes looked at as LOOP_PARSE work. A last line without a
 * line feed ends at the end of @p text. Returns true for a line, and false
 * once the room or the lines have run out; all of @p text is split once
 * l->start is text.len.
 */
extern bool
loop_lines_next (struct loop_lines *l,
                 struct loop       *loop,
                 struct cstr_view   text,
                 struct cstr_view  *line);

/** Take @p step off its list, if it is on one. */
extern void
loop_unready (struct loop_step *step);

/** CLOCK_MONOTONIC in nanoseconds, or 0 if it cannot be read. */
extern uint64_t
loop_clock (void);

/**
 * Log a line each for the turn times, the interactive and bulk step times
 * and the timers' lateness since the previous call, then one of the work
 * done: the most events, timers and steps in a turn, and the most of each
 * unit in a step against its bound, as "name=most/bound". Each line starts
 * with "stats " and @p label; counting then starts afresh.
 */
extern void
loop_log_stats (struct loop *loop,
                char const  *label);

/**
 * Run one turn. The turn waits for events, messages, timers or ready steps,
 * but not past @p deadline, an absolute loop_clock() time in nanoseconds,
 * and then handles them. UINT64_MAX means no deadline. Returns 0, or the
 * error of epoll_pwait2(). If a signal interrupts the wait, the function
 * returns 0 at once and handles nothing.
 */
extern int
loop_turn (struct loop *loop,
           uint64_t     deadline);

/**
 * Run turns until loop_exit() is called or a wait fails. Returns the error
 * of epoll_pwait2() if a wait failed. Otherwise returns the first error that
 * loop_fail() stored, for example the error of epoll_ctl() in
 * loop_unwatch(), or 0.
 */
extern int
loop_exec (struct loop *loop);

/** Store @p e as the error of @p loop, unless @p e is 0 or an error is
 *  stored already. loop_exec() returns that error.
 */
extern void
loop_fail (struct loop *loop,
           int          e);

extern void
loop_exit (struct loop *loop);

#endif /* ASKAI_SRC_LOOP_H_ */
