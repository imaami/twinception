/** @file agenda.c
 *
 * Parts of the client side of jobs and wait, and texts that the turns'
 * agenda in src/mcp.c shares.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>

#include "agenda.h"
#include "clk.h"
#include "dstr.h"
#include "hexid.h"
#include "jb.h"
#include "ledger.h"
#include "loop.h"
#include "mcp.h"
#include "util.h"

struct cstr_view const agenda_ended[LEDGER_STATUSES] = {
	[LEDGER_COMPLETED] = CSTR_VIEW_INIT("The job had already completed."),
	[LEDGER_FAILED]    = CSTR_VIEW_INIT("The job had already failed."),
	[LEDGER_CANCELLED] = CSTR_VIEW_INIT("The job was cancelled."),
};

/* The answer to a client's call of background, timer or wait that the
 * ledger refuses for want of room.
 */
static struct cstr_view const no_room = CSTR_VIEW_INIT("The server is at the limit of jobs, "
                                                       "timers and waits. Try again later.");

int
agenda_notice_timer (struct dstr               *d,
                     struct ledger_timer const *t)
{
	return dstr_printf(d, "[askai: timer %" PRIu32 " fired%s%s.]\n", t->id,
	                   t->note.len ? ": " : "", t->note.data) ? 0 : ENOMEM;
}

int
agenda_timer_lines (struct dstr               *d,
                    struct ledger_timer const *timers,
                    size_t                     n,
                    uint64_t                   now)
{
	bool ok = true;
	for (struct ledger_timer const *t = timers; ok && t < timers + n; ++t)
		ok = !t->due ||
		     ((t->due > now
		       ? dstr_printf(d, "timer %" PRIu32 ": fires in %" PRIu64 " s", t->id,
		                     (t->due - now + NS - 1) / NS)
		       : dstr_printf(d, "timer %" PRIu32 ": fired %" PRIu64 " s ago, unread",
		                     t->id, (now - t->due) / NS)) &&
		      dstr_printf(d, "%s%s\n", t->note.len ? ": " : "", t->note.data));
	return ok ? 0 : ENOMEM;
}

void
agenda_refused (struct mcp_slot  *slot,
                int               e,
                struct cstr_view  busy)
{
	if (e == EBUSY || e == ENOSPC)
		mcp_slot_tell(slot, e == EBUSY ? busy : no_room, TOOL_FAILED);
	else
		mcp_slot_fail(slot, e);
}

/* Returns the whole seconds from @p from to @p to, or 0 if @p to is not
 * later.
 */
static uint64_t
agenda_seconds (uint64_t from,
                uint64_t to)
{
	return to > from ? (to - from) / NS : 0;
}

struct ledger_task *
agenda_find (struct mcp_slot    *slot,
             struct json_object *args,
             uint32_t            tool)
{
	struct hexid id;
	if (hexid_init(&id, jb_view(jb_get(args, "job")))) {
		mcp_slot_tell(slot, CSTR_VIEW("The job argument must be the ID of a job that you "
		                              "started."), TOOL_FAILED);
		return nullptr;
	}
	struct ledger_task *task = ledger_find(mcp_slot_ledger(slot), mcp_slot_owner(slot), &id);
	if (task && tool != TOOL_MAX && task->tool != tool) {
		ledger_task_put(task);
		task = nullptr;
	}
	if (!task)
		mcp_slot_tell(slot, tool != TOOL_MAX
		              ? CSTR_VIEW("You have no job with the given ID from this tool. "
		                          "askai stores a job for up to an hour after the job "
		                          "ends.")
		              : CSTR_VIEW("You have no job with the given ID. askai stores a job "
		                          "for up to an hour after the job ends."), TOOL_FAILED);
	return task;
}

int
agenda_list (struct mcp_slot *slot)
{
	struct ledger_look look;
	int e = ledger_list(mcp_slot_ledger(slot), mcp_slot_owner(slot), &look);
	if (e)
		return e;
	dstr_auto d = DSTR_INIT;
	uint64_t day = clk_realtime_ns();
	bool ok = true;
	for (uint32_t i = 0; ok && i < look.n_tasks; ++i) {
		struct ledger_task const *t = look.tasks[i];
		uint64_t state = atomic_load_explicit(&t->state, memory_order_acquire);
		enum ledger_status status = state & LEDGER_STATUS;
		bool told = state & LEDGER_TOLD;
		ok = dstr_printf(&d, "%s %s: %s: ", ledger_kinds[t->kind], t->id.hex,
		                 mcp_slot_tool_name(slot, t->tool)) &&
		     (status == LEDGER_WORKING
		      ? dstr_printf(&d, "running for %" PRIu64 " s\n",
		                    agenda_seconds(t->created, day))
		      : dstr_printf(&d, "%s %" PRIu64 " s ago%s\n", ledger_statuses[status].data,
		                    agenda_seconds(t->ended, day), told ? "" : ", unread"));
	}
	if (ok && look.more)
		ok = dstr_printf(&d, "and %" PRIu64 " older records\n", look.more);
	ok = ok && !agenda_timer_lines(&d, look.timers, look.n_timers, loop_clock());
	if (ok && !look.n_tasks && !look.n_timers)
		ok = !cstr_view_cat(CSTR_VIEW("You have no jobs and no timers."), &d);
	ledger_look_put(&look);
	size_t taken;
	return ok ? mcp_slot_write(slot, cstr_view_dstr(&d), 0, &taken) : ENOMEM;
}

void
agenda_cancel (struct mcp_slot    *slot,
               struct json_object *args,
               uint32_t            tool)
{
	struct ledger_task *task = agenda_find(slot, args, tool);
	if (!task)
		return;
	enum ledger_status const status = ledger_task_status(task);
	bool const runs = status == LEDGER_WORKING;
	struct cstr_view said = agenda_ended[status];
	char text[192];
	if (runs) {
		ledger_task_cancel(task);
		said = util_format_view(text, sizeof text, "askai is cancelling %s %s. %s",
		                        ledger_kinds[task->kind], task->id.hex, tool != TOOL_MAX
		                        ? "Call the same tool with the job ID to check the status."
		                        : "Call wait to wait for the end of the job.");
	}
	ledger_task_put(task);
	mcp_slot_tell(slot, said, runs ? 0 : TOOL_FAILED);
}

/* A client's wait: the own word of the slot of its call of wait. */
struct agenda_wait {
	struct loop_timer   alarm;  /* the wait's end, or the time of the first
	                               timer of the owner that has not fired, if
	                               that is sooner */
	struct ledger_wait *wait;   /* the wait in the ledger */
	struct mcp_slot    *slot;   /* the slot of the call */
	uint64_t            end;    /* the loop's time when the wait ends */
	uint32_t            secs;   /* the seconds that the call asked for */
	uint32_t            n;      /* the ids in ids */
	struct hexid        ids[AGENDA_WAIT_IDS];  /* the ids of the jobs that
	                                              the call names, copied,
	                                              because the call's message
	                                              is freed as the call
	                                              parks */
};

/* Writes the events in @p look, of the client's wait @p w, into the output
 * of the wait's call: each job that ended and each timer that fired, or
 * else that nothing happened. Returns 0, or the error of mcp_slot_write(),
 * or ENOMEM.
 */
static int
agenda_wait_report (struct agenda_wait const *w,
                    struct ledger_look const *look)
{
	dstr_auto d = DSTR_INIT;
	bool ok = true;
	for (uint32_t i = 0; ok && i < look->n_tasks; ++i) {
		struct ledger_task const *t = look->tasks[i];
		enum ledger_status status = ledger_task_status(t);
		char const *kind = ledger_kinds[t->kind];
		char const *tool = mcp_slot_tool_name(w->slot, t->tool);
		ok = status == LEDGER_CANCELLED
		   ? dstr_printf(&d, "[askai: %s %s, %s, cancelled.]\n", kind, t->id.hex, tool)
		   : dstr_printf(&d, "[askai: %s %s, %s, %s. Call jobs with action result and "
		                 "job %s for the result.]\n", kind, t->id.hex, tool,
		                 ledger_statuses[status].data, t->id.hex);
	}
	for (uint32_t i = 0; ok && i < look->n_timers; ++i)
		ok = !agenda_notice_timer(&d, &look->timers[i]);
	if (ok && !look->n_tasks && !look->n_timers)
		ok = dstr_printf(&d, "Nothing happened in %" PRIu32 " s.", w->secs);
	size_t taken;
	return ok ? mcp_slot_write(w->slot, cstr_view_dstr(&d), 0, &taken) : ENOMEM;
}

/* Checks the client's wait @p w. If ledger_tell() returns jobs that ended
 * or timers that fired, or the wait's time is up, the wait's call answers
 * with what happened. Else the wait goes on: its alarm is set for its end,
 * or for the time of the owner's first timer, if that is sooner. On the
 * first check, @p first, the call then parks.
 */
static void
agenda_wait_check (struct agenda_wait *w,
                   bool                first)
{
	struct mcp_slot *slot = w->slot;
	struct loop *loop = mcp_slot_loop(slot);
	struct ledger_look look;
	int e = ledger_tell(mcp_slot_ledger(slot), w->wait, w->ids, w->n, &look);
	if (!e && !look.n_tasks && !look.n_timers && loop->now < w->end) {
		e = loop_timer_set(loop, &w->alarm, look.due < w->end ? look.due : w->end);
		if (!e && first)
			e = mcp_slot_park(slot);
		if (!e)
			return;
	}
	loop_timer_stop(loop, &w->alarm);
	if (!e)
		e = agenda_wait_report(w, &look);
	ledger_look_put(&look);
	if (e)
		mcp_slot_fail(slot, e);
	else
		mcp_slot_finish(slot, 0);
}

/* The alarm of a client's wait: its end, or a timer of its owner. */
static void
agenda_wait_alarm (struct loop       *loop,
                   struct loop_timer *timer)
{
	(void)loop;
	(void)mcp_slot_ready(container_of(timer, struct agenda_wait, alarm)->slot);
}

/* The message of a client's wait, @p msg, which the ledger posts when a
 * record of the wait's owner ends or the owner sets a timer. If the wait's
 * call still runs on @p loop, its step checks the wait again.
 */
static void
agenda_wait_wake (struct loop *loop,
                  struct msg  *msg)
{
	struct ledger_wait *w = container_of(msg, struct ledger_wait, wake);
	ledger_wait_heard(w);
	struct mcp_slot *slot = mcp_slot_named(loop, w->who);
	ledger_wait_put(w);
	/* mcp_slot_ready() refuses nullptr, for a call that is gone, and a call
	 * that has ended. */
	mcp_slot_ready(slot);
}

void
agenda_wait_start (struct mcp_slot    *slot,
                   struct json_object *args,
                   uint64_t            secs)
{
	static_assert(AGENDA_WAIT_IDS == 16 && LEDGER_WAITS == 4, "update the problem texts");
	static struct cstr_view const bad_list = CSTR_VIEW_INIT("The jobs argument must be a list "
	                                                        "of at most 16 IDs of your jobs.");
	struct ledger *ledger = mcp_slot_ledger(slot);
	uint64_t owner = mcp_slot_owner(slot);
	struct json_object *list = jb_get(args, "jobs");
	size_t n = !list ? 0 : json_object_is_type(list, json_type_array)
	         ? json_object_array_length(list) : SIZE_MAX;
	if (n > AGENDA_WAIT_IDS) {
		mcp_slot_tell(slot, bad_list, TOOL_FAILED);
		return;
	}
	struct loop *loop = mcp_slot_loop(slot);
	struct agenda_wait *w = malloc(sizeof *w);
	if (!w) {
		mcp_slot_fail(slot, ENOMEM);
		return;
	}
	*w = (struct agenda_wait){
		.alarm.fn = agenda_wait_alarm,
		.slot     = slot,
		.end      = loop->now + secs * NS,
		.secs     = (uint32_t)secs,
		.n        = (uint32_t)n,
	};
	char why[160];
	struct cstr_view problem = {};
	for (size_t i = 0; !problem.data && i < n; ++i) {
		struct cstr_view const id = jb_view(jb_at(list, i));
		struct ledger_task *task = hexid_init(&w->ids[i], id) ? nullptr
		                         : ledger_find(ledger, owner, &w->ids[i]);
		if (task) {
			ledger_task_put(task);
			continue;
		}
		problem = !id.data ? bad_list
		        : util_format_view(why, sizeof why, "You have no job with the ID %.*s. "
		                           "askai stores a job for up to an hour after the job "
		                           "ends.",
		                           (int)util_clip(id.data, id.len, HEXID_DIGITS), id.data);
	}
	if (problem.data) {
		free(w);
		mcp_slot_tell(slot, problem, TOOL_FAILED);
		return;
	}
	struct handle who;
	int e = mcp_slot_name(slot, &who);
	w->wait = e ? nullptr : ledger_wait_enter(ledger, owner, loop, who, agenda_wait_wake, &e);
	if (!w->wait) {
		free(w);
		agenda_refused(slot, e, CSTR_VIEW("You have 4 waits under way. The limit is 4."));
		return;
	}
	*mcp_slot_own(slot) = w;
	agenda_wait_check(w, true);
}

void
agenda_wait_step (struct mcp_slot *slot)
{
	struct agenda_wait *w = *mcp_slot_own(slot);
	if (w)
		agenda_wait_check(w, false);
}

void
agenda_wait_drop (struct mcp_slot *slot)
{
	struct agenda_wait *w = *mcp_slot_own(slot);
	loop_timer_stop(mcp_slot_loop(slot), &w->alarm);
	ledger_wait_leave(mcp_slot_ledger(slot), &w->wait);
	free(w);
}
