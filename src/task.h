/** @file task.h
 *
 * Tasks (design sections 1 and 4, S8): CPU work done a slice a step,
 * each slice TASK_SLICE_NS at most, on the bulk list of a client loop.
 *
 * Its owner, on a client loop, submits a task, which goes to the lighter of
 * two client loops, chosen at random: the one holding fewer tasks, each loop
 * counting the tasks placed on it as they are placed, so that placements in
 * a burst spread rather than herd. A loop that holds more than TASK_BACKLOG
 * tasks, more than 2 ms of slices waiting, moves a task at the end of its
 * slice to the lighter of two other loops, if that holds two fewer at least.
 * Once its slices are done, or it is cancelled, the task goes back to its
 * owner by message.
 *
 * A task is plain memory, the first member of a malloc()ed object, that one
 * thread holds at a time: its owner, or the loop that runs it, between which
 * it moves by message (R1, R3). One word in it is shared (R4): its
 * references, a bit for the owner's and a bit for the runner's. Either can
 * be dropped first as askai stops, and the drop of the last frees the task.
 * The owner's bit is the cancel order too: the loop that runs the task reads
 * the bit before each slice and runs no slice once the bit is clear. A
 * loop's count of the tasks placed on it is shared too: placers add, the
 * loop takes away.
 */
#ifndef ASKAI_SRC_TASK_H_
#define ASKAI_SRC_TASK_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "flag.h"
#include "list.h"
#include "loop.h"

enum {
	TASK_SLICE_NS = 250000,   /**< a slice, at most (loop.c's BULK_NS) */
	TASK_BACKLOG  = 8,        /**< tasks a loop holds before one moves:
	                               2 ms of slices */
};

/** The references to a task: the bits of task.refs. A task has exactly these
 *  two references, so a bit for each replaces a count, and the owner's bit
 *  replaces a cancel flag. A third reference needs a count again
 *  (src/ref.h).
 */
enum : uint64_t {
	/** The owner's reference. task_drop() clears the bit, which cancels the
	 *  task: the loop that runs it tests the bit before each slice.
	 */
	TASK_OWNER = FLAG_OWN(0),

	/** The reference of the loop that runs the task. When the task is back
	 *  on the loop of its owner, the function back drops it.
	 */
	TASK_RUNNER = FLAG_OWN(1),
};

/** What a slice leaves to do. */
enum task_state {
	TASK_MORE,
	TASK_DONE,
};

struct task_pool;

struct task {
	struct loop_step  step;    /**< on its runner's bulk list */
	struct msg        msg;     /**< to its next runner, or back to its owner */
	struct list       hook;    /**< its runner's: on the tasks it holds */
	/** Run a slice, until @p until (loop_clock()) at most, counting its work
	 *  on @p loop; TASK_DONE once nothing is left. */
	enum task_state (*slice) (struct task *task,
	                          struct loop *loop,
	                          uint64_t     until);
	/** The owner's handler of the task back, done or cancelled, on its loop;
	 *  it owns the runner's reference. */
	void            (*back)  (struct loop *loop,
	                          struct msg  *msg);
	struct task_pool *pool;
	struct loop      *owner;
	uint64_t          moves;   /**< times it moved between loops */
	_Atomic uint64_t  refs;    /**< TASK_OWNER and TASK_RUNNER, widened
	                                over the padding */
};

/** A client loop's tasks, as GET /metrics renders them: the loop writes, any
 *  thread reads (hist.h).
 */
struct task_metrics {
	_Atomic uint64_t held;    /**< tasks it holds */
	_Atomic uint64_t slices;  /**< slices it ran */
	_Atomic uint64_t moved;   /**< tasks it moved to another loop */
};

/** How tasks ended, which any client loop adds to: submitted = done +
 *  cancelled + abandoned + those under way.
 */
struct task_pool_metrics {
	_Atomic uint64_t submitted;  /**< owners count */
	_Atomic uint64_t done;       /**< their slices done; runners count */
	_Atomic uint64_t cancelled;  /**< found cancelled; runners count */
	_Atomic uint64_t abandoned;  /**< held, or moving to, a loop that stopped */
};

/**
 * The tasks of @p loops client loops, made before any loop starts, destroyed
 * once every loop is finished; nullptr, with ENOMEM in @p *e, if out of
 * memory.
 */
extern struct task_pool *
task_pool_create (size_t  loops,
                  int    *e);

extern void
task_pool_destroy (struct task_pool **p_pool);

/** Take client loop @p loop's part, as it starts: tasks may come to it from
 *  then on.
 */
extern void
task_pool_attach (struct task_pool *pool,
                  struct loop      *loop);

/** Let go of the tasks client loop @p loop holds, as it stops: they never run
 *  again, nor go back to their owners, who let go of them as they end the
 *  calls that wait for them; and have placers and movers pass it by.
 */
extern void
task_pool_detach (struct task_pool *pool,
                  struct loop      *loop);

/** Returns the counts of the tasks of the loop @p i of @p pool. */
extern struct task_metrics const *
task_pool_loop_counts (struct task_pool const *pool,
                       size_t                  i);

/** Returns the counts of the work of @p pool. */
extern struct task_pool_metrics const *
task_pool_counts (struct task_pool const *pool);

/** Initialize @p task, in storage just allocated, for its owner @p loop:
 *  slices in @p slice, back to @p back.
 */
extern void
task_init (struct task      *task,
           struct task_pool *pool,
           struct loop      *loop,
           enum task_state (*slice) (struct task *task,
                                     struct loop *loop,
                                     uint64_t     until),
           void           (*back)  (struct loop *loop,
                                    struct msg  *msg));

/** Submit @p task from its owner's loop, @p loop, placing it. */
extern void
task_submit (struct task *task,
             struct loop *loop);

/** The owner lets go of @p task: drops the owner's reference, which cancels
 *  the task if it is not back.
 */
extern void
task_drop (struct task *task);

/** Drop the references @p refs to @p task, TASK_OWNER, TASK_RUNNER or both.
 *  The drop of the last reference frees the task.
 */
extern void
task_put (struct task *task,
          uint64_t     refs);

#endif /* ASKAI_SRC_TASK_H_ */
