/** @file loop_export.h
 *
 * The part of the event loop (src/loop.h) that a plugin (src/plugin.h) uses.
 * struct loop is an incomplete type in this header, so a plugin does not
 * depend on the members of struct loop.
 *
 * Each exported function checks its inputs before it uses them, because a
 * plugin calls it across the boundary of a shared object. A function that
 * returns an error number returns EFAULT for a pointer that is nullptr,
 * also for a message or a timer without a function, and EINVAL for a value
 * out of range, also for a timer whose member slot is not its place in the
 * loop's heap. loop_now() and loop_room() return 0 for such inputs.
 */
#ifndef ASKAI_SRC_LOOP_EXPORT_H_
#define ASKAI_SRC_LOOP_EXPORT_H_

#include <stddef.h>
#include <stdint.h>

#include "export.h"

struct loop;

/**
 * A handle is the name that another thread uses for an object of a loop. A
 * thread never passes a pointer or a file descriptor of a loop's object to
 * another thread (design section 1, R2). A handle names a slot of the
 * loop's handle table. The slot has a generation number, which increases
 * each time the slot is freed. A handle from before that no longer names
 * an object. The value 0 is not a handle.
 */
struct handle {
	uint64_t bits;  /**< from the highest bit: 8 bits for the loop, 24 bits
	                     for the slot, 32 bits for the generation */
};

/**
 * The units in which a step counts its work. Each unit has a bound per
 * step (design section 1). The bounds make a turn short on a slow machine
 * too. A step counts its work with loop_work(). It can call loop_room()
 * to learn how much of a unit it may still do, and it leaves the rest of
 * its work to a later step. Work that cannot be split, for example a copy
 * that a library makes, is counted in full in the step where it happens.
 * The loop's statistics record the largest count of each unit in one step.
 * That count must be within the unit's bound.
 *
 * LOOP_UNIT_LIST(X) lists each unit as X(ID, name, bound): LOOP_ID in enum
 * loop_unit, its name in the log and the metrics, and its bound per step,
 * LOOP_STEP_ID. The units count:
 * - REQS: requests that began, or tool calls that a drain ended
 * - RECV: bytes received
 * - SEND: bytes sent, or bytes that an application queued for sending
 * - BODY: bytes of a request body that were read and pre-scanned
 * - PARSE: bytes of JSON parsed, or bytes of text split into lines, such
 *   as an SSE stream or a file that the read command of the git tool
 *   numbers
 * - VALUES: JSON values parsed or freed
 * - ESCAPE: bytes escaped into JSON
 * - COPY: bytes copied in one piece, for example a string that json-c
 *   parsed, or the text of an answer
 * - ACCEPTS: connections accepted
 * - CLOSES: connections closed
 * - RELEASE: blocks or objects that were allocated for reuse and are freed
 *   under memory pressure. A block counts one unit for each 128 KiB
 *   (buf_stash_release()).
 * - TASKS: task slices (task.h)
 * - FLIGHTS: TLS handshake flights (tls.h)
 */
#define LOOP_UNIT_LIST(X)                \
        X(REQS,    "reqs",    8)         \
        X(RECV,    "recv",    64 << 10)  \
        X(SEND,    "send",    256 << 10) \
        X(BODY,    "body",    64 << 10)  \
        X(PARSE,   "parse",   16 << 10)  \
        X(VALUES,  "values",  256)       \
        X(ESCAPE,  "escape",  64 << 10)  \
        X(COPY,    "copy",    1 << 20)   \
        X(ACCEPTS, "accepts", 32)        \
        X(CLOSES,  "closes",  32)        \
        X(RELEASE, "release", 256)       \
        X(TASKS,   "tasks",   1)         \
        X(FLIGHTS, "flights", 1)

#define LOOP_UNIT(id, name, bound) LOOP_##id,
enum loop_unit {
	LOOP_UNIT_LIST(LOOP_UNIT)
	LOOP_UNITS
};
#undef LOOP_UNIT

/** The bound of each unit per step. */
#define LOOP_UNIT(id, name, bound) LOOP_STEP_##id = bound,
enum {
	LOOP_UNIT_LIST(LOOP_UNIT)
};
#undef LOOP_UNIT

/**
 * A message that another thread posts to a loop (design section 2). The
 * loop calls the message's function fn in a later turn, on the loop's
 * thread. From the post until that call, the poster must not read or
 * write the message, and must not post it again. From that call on, the
 * message belongs to fn. A message that can be posted repeatedly has a
 * word of pending bits, and it is posted only when that word changes from
 * zero to non-zero.
 */
struct msg {
	struct msg *next;
	void      (*fn) (struct loop *loop,
	                 struct msg  *msg);
	uint64_t    posted;  /**< the value of loop_clock() when the message
	                          was posted */
};

/** A deadline in the loop's timer heap. An object has one timer and sets
 *  it again for each of its deadlines. It does not have one timer for
 *  each event.
 */
struct loop_timer {
	uint64_t due;   /**< the deadline in nanoseconds of CLOCK_MONOTONIC. A
	                     timer that is due at UINT64_MAX never fires. */
	void   (*fn) (struct loop       *loop,
	              struct loop_timer *timer);
	size_t   slot;  /**< the timer's index in the heap plus 1. It is 0
	                     while the timer is not in the heap. */
};

/**
 * Posts @p msg to @p loop. Any thread can call it. It pushes the message
 * onto the loop's list of posted messages with an atomic operation, and it
 * writes to the loop's eventfd only if the loop is asleep. Returns 0, or
 * EFAULT if @p loop or @p msg is nullptr or @p msg has no function. It
 * returns EALREADY if @p msg is the newest message in the loop's list,
 * which means that it is posted already. The function does not detect a
 * message that is further down the list, or that the loop has taken from
 * the list and not yet handled. So a poster still must not post a message
 * again before the loop has called the message's function.
 */
EXPORT
extern int
loop_post (struct loop *loop,
           struct msg  *msg);

/**
 * Sets @p timer to fire at @p due. If the timer is already set, the call
 * changes its deadline. A timer stays in the heap until loop_timer_stop().
 * After it has fired, it is due at UINT64_MAX, which means never, until it
 * is set again. Returns 0. Returns EFAULT if @p loop or @p timer is
 * nullptr or @p timer has no function, EINVAL if the member slot of
 * @p timer is not 0 and is not the timer's place in the heap, and ENOMEM if
 * the timer is not in the heap and the heap cannot grow.
 */
EXPORT
extern int
loop_timer_set (struct loop       *loop,
                struct loop_timer *timer,
                uint64_t           due);

/** Removes @p timer from the heap of @p loop. It does nothing if the timer
 *  is not in the heap. Returns 0, EFAULT if @p loop or @p timer is
 *  nullptr, or EINVAL if the member slot of @p timer is not 0 and is not the
 *  timer's place in the heap.
 */
EXPORT
extern int
loop_timer_stop (struct loop       *loop,
                 struct loop_timer *timer);

/** Returns the time at which the current turn of @p loop began, in
 *  nanoseconds of CLOCK_MONOTONIC. loop_timer_set() uses the same clock.
 *  Returns 0 if @p loop is nullptr.
 */
EXPORT
extern uint64_t
loop_now (struct loop const *loop);

/** Counts @p n units of the work @p u for the step that is running.
 *  Returns 0, EFAULT if @p loop is nullptr, or EINVAL if @p u is out of
 *  range.
 */
EXPORT
extern int
loop_work (struct loop    *loop,
           enum loop_unit  u,
           uint64_t        n);

/** Returns the number of units of the work @p u that the step that is
 *  running may still do. Returns 0 if @p loop is nullptr or @p u is out of
 *  range.
 */
EXPORT
extern uint64_t
loop_room (struct loop const *loop,
           enum loop_unit     u);

#endif /* ASKAI_SRC_LOOP_EXPORT_H_ */
