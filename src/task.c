/** @file Tasks (task.h). */
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "hist.h"
#include "list.h"
#include "loop.h"
#include "task.h"
#include "util.h"

/* A client loop's part. */
struct task_lane {
	struct loop        *loop;     /* the loop's, as it attaches */
	struct list         held;     /* the loop's: the tasks it holds */
	uint64_t            rng;      /* the loop's: its choices' xorshift64 */
	struct task_metrics metrics;  /* the loop's */
	_Atomic uint64_t    load;     /* tasks placed on the loop and not gone
	                                 from it: placers add, the loop takes
	                                 away; once it has stopped, so many
	                                 that none chooses it */
};

struct task_pool {
	struct task_pool_metrics metrics;  /* owners and runners add */
	size_t                   n;
	struct task_lane         lane[];   /* client loop i + 1's at i */
};

static uint64_t
task_lane_load (struct task_lane const *lane)
{
	return atomic_load_explicit(&lane->load, memory_order_relaxed);
}

/* @p task has come to loop @p loop's lane @p lane: queue it. */
static void
task_lane_take (struct task_lane *lane,
                struct task      *task,
                struct loop      *loop)
{
	list_append(&lane->held, &task->hook);
	hist_add(&lane->metrics.held, 1);
	loop_ready_bulk(loop, &task->step);
}

/* @p task leaves @p lane, which held it. */
static void
task_lane_leave (struct task_lane *lane,
                 struct task      *task)
{
	list_del(&task->hook);
	hist_add(&lane->metrics.held, -1ULL);
	atomic_fetch_sub_explicit(&lane->load, 1, memory_order_relaxed);
}

/* The lighter of two lanes picked at random by @p self's loop, none of them
 * @p self's own if @p other; @p self's if there is no other to pick. */
static struct task_lane *
task_pool_choose (struct task_pool *pool,
                  struct task_lane *self,
                  bool              other)
{
	size_t n = pool->n;
	if (n == 1)
		return self;
	size_t me = (size_t)(self - pool->lane);
	if (other && n == 2)
		return &pool->lane[!me];
	uint64_t r = self->rng;
	r ^= r << 13;
	r ^= r >> 7;
	r ^= r << 17;
	self->rng = r;
	/* Two lanes apart, of those to pick from: all, or all but this one. */
	size_t m = n - other;
	size_t a = r % m;
	size_t b = (r >> 32) % (m - 1);
	b += b >= a;
	if (other) {
		a += a >= me;
		b += b >= me;
	}
	struct task_lane *x = &pool->lane[a];
	struct task_lane *y = &pool->lane[b];
	return task_lane_load(y) < task_lane_load(x) ? y : x;
}

/* A task moved to this loop; once the loop has stopped, it never runs, and
 * the loop lets go of it. */
static void
task_arrive (struct loop *loop,
             struct msg  *msg)
{
	struct task *task = container_of(msg, struct task, msg);
	if (loop->exit) {
		atomic_fetch_add_explicit(&task->pool->metrics.abandoned, 1,
		                          memory_order_relaxed);
		task_put(task, 1);
		return;
	}
	task_lane_take(&task->pool->lane[loop->id - 1], task, loop);
}

/* After a slice of @p task, which @p lane holds on @p loop: queue it for the
 * next, or move it, while the loop holds more than TASK_BACKLOG, to a loop
 * that holds two fewer at least. */
static void
task_lane_next (struct task_lane *lane,
                struct task      *task,
                struct loop      *loop)
{
	uint64_t load = task_lane_load(lane);
	struct task_lane *to = load > TASK_BACKLOG ? task_pool_choose(task->pool, lane, true)
	                                           : lane;
	if (to == lane || task_lane_load(to) + 2 > load) {
		loop_ready_bulk(loop, &task->step);
		return;
	}
	task_lane_leave(lane, task);
	atomic_fetch_add_explicit(&to->load, 1, memory_order_relaxed);
	hist_add(&lane->metrics.moved, 1);
	++task->moves;
	task->msg.fn = task_arrive;
	loop_post(to->loop, &task->msg);
}

/* A slice of a task, unless it is cancelled: then, or once it is done, it
 * goes back to its owner. */
static void
task_step (struct loop      *loop,
           struct loop_step *step)
{
	struct task *task = container_of(step, struct task, step);
	struct task_pool *pool = task->pool;
	struct task_lane *self = &pool->lane[loop->id - 1];
	_Atomic uint64_t *ended = &pool->metrics.cancelled;
	if (!atomic_load_explicit(&task->cancel, memory_order_acquire)) {
		enum task_state state = task->slice(task, loop, loop_clock() + TASK_SLICE_NS);
		loop_work(loop, LOOP_TASKS, 1);
		hist_add(&self->metrics.slices, 1);
		if (state == TASK_MORE) {
			task_lane_next(self, task, loop);
			return;
		}
		ended = &pool->metrics.done;
	}
	task_lane_leave(self, task);
	atomic_fetch_add_explicit(ended, 1, memory_order_relaxed);
	task->msg.fn = task->back;
	loop_post(task->owner, &task->msg);
}

struct task_pool *
task_pool_create (size_t  loops,
                  int    *e)
{
	struct task_pool *pool = calloc(1, sizeof *pool + loops * sizeof *pool->lane);
	if (!pool) {
		*e = ENOMEM;
		return nullptr;
	}
	struct task_pool_metrics *m = &pool->metrics;
	atomic_init(&m->submitted, 0);
	atomic_init(&m->done, 0);
	atomic_init(&m->cancelled, 0);
	atomic_init(&m->abandoned, 0);
	pool->n = loops;
	for (size_t i = 0; i < loops; ++i) {
		struct task_lane *lane = &pool->lane[i];
		atomic_init(&lane->metrics.held, 0);
		atomic_init(&lane->metrics.slices, 0);
		atomic_init(&lane->metrics.moved, 0);
		atomic_init(&lane->load, 0);
	}
	*e = 0;
	return pool;
}

void
task_pool_destroy (struct task_pool **p_pool)
{
	if (!p_pool)
		return;
	free(*p_pool);
	*p_pool = nullptr;
}

void
task_pool_attach (struct task_pool *pool,
                  struct loop      *loop)
{
	struct task_lane *lane = &pool->lane[loop->id - 1];
	lane->loop = loop;
	list_init(&lane->held);
	lane->rng = loop_clock() * 0x9e3779b97f4a7c15 | 1;
}

void
task_pool_detach (struct task_pool *pool,
                  struct loop      *loop)
{
	struct task_lane *lane = &pool->lane[loop->id - 1];
	while (!list_is_empty(&lane->held)) {
		struct task *task = container_of(lane->held.next, struct task, hook);
		loop_unready(&task->step);
		list_del(&task->hook);
		hist_add(&lane->metrics.held, -1ULL);
		atomic_fetch_add_explicit(&pool->metrics.abandoned, 1, memory_order_relaxed);
		task_put(task, 1);
	}
	/* A placer that read the load before goes on, and its task is
	 * abandoned; the count stays far from wrapping. */
	atomic_store_explicit(&lane->load, UINT64_MAX / 2, memory_order_relaxed);
}

struct task_metrics const *
task_pool_loop_metrics (struct task_pool const *pool,
                        size_t                  i)
{
	return &pool->lane[i].metrics;
}

struct task_pool_metrics const *
task_pool_metrics (struct task_pool const *pool)
{
	return &pool->metrics;
}

void
task_init (struct task      *task,
           struct task_pool *pool,
           struct loop      *loop,
           enum task_state (*slice) (struct task *task,
                                     struct loop *loop,
                                     uint64_t     until),
           void           (*back)  (struct loop *loop,
                                    struct msg  *msg))
{
	task->step = (struct loop_step){ .fn = task_step };
	task->msg = (struct msg){};
	task->hook = (struct list){};
	task->slice = slice;
	task->back = back;
	task->pool = pool;
	task->owner = loop;
	task->moves = 0;
	atomic_init(&task->cancel, 0);
	atomic_init(&task->refs, 0);
}

void
task_submit (struct task *task,
             struct loop *loop)
{
	struct task_pool *pool = task->pool;
	struct task_lane *self = &pool->lane[loop->id - 1];
	atomic_store_explicit(&task->refs, 2, memory_order_relaxed);
	atomic_fetch_add_explicit(&pool->metrics.submitted, 1, memory_order_relaxed);
	struct task_lane *to = task_pool_choose(pool, self, false);
	atomic_fetch_add_explicit(&to->load, 1, memory_order_relaxed);
	if (to == self) {
		task_lane_take(self, task, loop);
		return;
	}
	task->msg.fn = task_arrive;
	loop_post(to->loop, &task->msg);
}

void
task_drop (struct task *task)
{
	atomic_store_explicit(&task->cancel, 1, memory_order_release);
	task_put(task, 1);
}

void
task_put (struct task *task,
          uint32_t     n)
{
	if (atomic_fetch_sub_explicit(&task->refs, n, memory_order_acq_rel) == n)
		free(task);
}
