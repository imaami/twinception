/** @file Tasks (task.h). */
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

struct tasks {
	struct tasks_metrics metrics;  /* owners and runners add */
	size_t               n;
	struct task_lane     lane[];   /* client loop i + 1's at i */
};

static uint64_t
load_of (struct task_lane const *lane)
{
	return atomic_load_explicit(&lane->load, memory_order_relaxed);
}

/* The lighter of two lanes picked at random by @p self's loop, none of them
 * @p self's own if @p other; @p self's if there is no other to pick. */
static struct task_lane *
choose (struct tasks     *tasks,
        struct task_lane *self,
        bool              other)
{
	size_t n = tasks->n;
	size_t me = (size_t)(self - tasks->lane);
	if (n == 1 || (other && n == 2))
		return n == 1 ? self : &tasks->lane[!me];
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
	struct task_lane *x = &tasks->lane[a];
	struct task_lane *y = &tasks->lane[b];
	return load_of(y) < load_of(x) ? y : x;
}

/* @p task has come to loop @p loop's lane @p lane: queue it. */
static void
take (struct task_lane *lane,
      struct task      *task,
      struct loop      *loop)
{
	list_append(&lane->held, &task->hook);
	hist_add(&lane->metrics.held, 1);
	loop_ready_bulk(loop, &task->step);
}

/* A task moved to this loop; once the loop has stopped, it never runs, and
 * the loop lets go of it. */
static void
task_arrive (struct loop *loop,
             struct msg  *msg)
{
	struct task *task = container_of(msg, struct task, msg);
	if (loop->exit) {
		atomic_fetch_add_explicit(&task->tasks->metrics.abandoned, 1,
		                          memory_order_relaxed);
		task_put(task, 1);
		return;
	}
	take(&task->tasks->lane[loop->id - 1], task, loop);
}

/* A slice of a task, unless it is cancelled: then, or once it is done, it
 * goes back to its owner; else it stays, or moves while this loop holds
 * more than TASK_BACKLOG to a loop that holds two fewer at least. */
static void
task_step (struct loop      *loop,
           struct loop_step *step)
{
	struct task *task = container_of(step, struct task, step);
	struct tasks *tasks = task->tasks;
	struct task_lane *self = &tasks->lane[loop->id - 1];
	bool cancelled = atomic_load_explicit(&task->cancel, memory_order_acquire);
	enum task_state state = TASK_DONE;
	if (!cancelled) {
		state = task->slice(task, loop, loop_clock() + TASK_SLICE_NS);
		loop_work(loop, LOOP_TASKS, 1);
		hist_add(&self->metrics.slices, 1);
	}
	struct task_lane *to = state == TASK_DONE ? nullptr
	                     : load_of(self) > TASK_BACKLOG ? choose(tasks, self, true)
	                     : self;
	if (to && to != self && load_of(to) + 2 > load_of(self))
		to = self;
	if (to == self) {
		loop_ready_bulk(loop, step);
		return;
	}
	list_del(&task->hook);
	hist_add(&self->metrics.held, -1ULL);
	atomic_fetch_sub_explicit(&self->load, 1, memory_order_relaxed);
	if (!to) {
		atomic_fetch_add_explicit(cancelled ? &tasks->metrics.cancelled
		                                    : &tasks->metrics.done, 1,
		                          memory_order_relaxed);
		task->msg.fn = task->back;
		loop_post(task->owner, &task->msg);
		return;
	}
	atomic_fetch_add_explicit(&to->load, 1, memory_order_relaxed);
	hist_add(&self->metrics.moved, 1);
	++task->moves;
	task->msg.fn = task_arrive;
	loop_post(to->loop, &task->msg);
}

struct tasks *
tasks_new (size_t loops)
{
	struct tasks *tasks = calloc(1, sizeof *tasks + loops * sizeof *tasks->lane);
	if (!tasks)
		return nullptr;
	struct tasks_metrics *m = &tasks->metrics;
	atomic_init(&m->submitted, 0);
	atomic_init(&m->done, 0);
	atomic_init(&m->cancelled, 0);
	atomic_init(&m->abandoned, 0);
	tasks->n = loops;
	for (size_t i = 0; i < loops; ++i) {
		struct task_lane *lane = &tasks->lane[i];
		atomic_init(&lane->metrics.held, 0);
		atomic_init(&lane->metrics.slices, 0);
		atomic_init(&lane->metrics.moved, 0);
		atomic_init(&lane->load, 0);
	}
	return tasks;
}

void
tasks_free (struct tasks **p_tasks)
{
	free(*p_tasks);
	*p_tasks = nullptr;
}

void
tasks_attach (struct tasks *tasks,
              struct loop  *loop)
{
	struct task_lane *lane = &tasks->lane[loop->id - 1];
	lane->loop = loop;
	list_init(&lane->held);
	lane->rng = loop_clock() * 0x9e3779b97f4a7c15 | 1;
}

void
tasks_detach (struct tasks *tasks,
              struct loop  *loop)
{
	struct task_lane *lane = &tasks->lane[loop->id - 1];
	while (!list_is_empty(&lane->held)) {
		struct task *task = container_of(lane->held.next, struct task, hook);
		loop_unready(&task->step);
		list_del(&task->hook);
		hist_add(&lane->metrics.held, -1ULL);
		atomic_fetch_add_explicit(&tasks->metrics.abandoned, 1, memory_order_relaxed);
		task_put(task, 1);
	}
	/* A placer that read the load before goes on, and its task is
	 * abandoned; the count stays far from wrapping. */
	atomic_store_explicit(&lane->load, UINT64_MAX / 2, memory_order_relaxed);
}

struct task_metrics const *
tasks_loop_metrics (struct tasks const *tasks,
                    size_t              i)
{
	return &tasks->lane[i].metrics;
}

struct tasks_metrics const *
tasks_metrics (struct tasks const *tasks)
{
	return &tasks->metrics;
}

void
task_init (struct task      *task,
           struct tasks     *tasks,
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
	task->tasks = tasks;
	task->owner = loop;
	task->moves = 0;
	atomic_init(&task->cancel, 0);
	atomic_init(&task->refs, 0);
}

void
task_submit (struct task *task,
             struct loop *loop)
{
	struct tasks *tasks = task->tasks;
	struct task_lane *self = &tasks->lane[loop->id - 1];
	atomic_store_explicit(&task->refs, 2, memory_order_relaxed);
	atomic_fetch_add_explicit(&tasks->metrics.submitted, 1, memory_order_relaxed);
	struct task_lane *to = choose(tasks, self, false);
	atomic_fetch_add_explicit(&to->load, 1, memory_order_relaxed);
	if (to == self) {
		take(self, task, loop);
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
