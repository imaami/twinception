/** @file agenda.h
 *
 * Parts of the client side of jobs and wait: the lookup, list and cancel
 * of a client's job and the client's wait, over the records and the timers
 * of the client's owner in the ledger (src/ledger.h); the answer to a
 * refusal of the ledger; and texts that the turns' agenda in src/mcp.c
 * shares. The functions use the call API of src/mcp.h. The rest of the
 * client side and the turns' side are in src/mcp.c.
 */
#ifndef ASKAI_SRC_AGENDA_H_
#define ASKAI_SRC_AGENDA_H_

#include <stdint.h>

#include "cstr_view.h"
#include "ledger.h"
#include "tool.h"

struct dstr;

/** The ids of jobs that a client's wait may name, at most. */
enum { AGENDA_WAIT_IDS = 16 };

/** The answer to a cancel of a job that has ended. */
extern struct cstr_view const agenda_runs_no_more;

/** Appends the notice that the timer @p t fired, with its note, to @p d.
 *  Returns 0, or ENOMEM.
 */
extern int
agenda_notice_timer (struct dstr               *d,
                     struct ledger_timer const *t);

/** Answers the client's call in @p slot that the ledger refused with
 *  @p e: with @p busy for EBUSY, and with the lack of room for ENOSPC. Any
 *  other error fails the call.
 */
extern void
agenda_refused (struct mcp_slot  *slot,
                int               e,
                struct cstr_view  busy);

/** Returns a reference to the record of the owner of the client's call in
 *  @p slot that the argument job of @p args names, and that is a call of
 *  the tool @p tool, unless @p tool is TOOL_MAX. If there is none, it
 *  returns nullptr, and the call answers why.
 */
extern struct ledger_task *
agenda_find (struct mcp_slot    *slot,
             struct json_object *args,
             uint32_t            tool);

/** Writes the records and the timers of the owner of the client's call in
 *  @p slot into its output: the records, the newest first, then the
 *  timers. Returns 0, ENOMEM, or the error of ledger_list() or
 *  mcp_slot_write().
 */
extern int
agenda_list (struct mcp_slot *slot);

/** Cancels the record of the owner of the client's call in @p slot that
 *  @p args names, as agenda_find() finds it with @p tool, if its call
 *  runs, and answers the call.
 */
extern void
agenda_cancel (struct mcp_slot    *slot,
               struct json_object *args,
               uint32_t            tool);

/** Starts the client's call of wait in @p slot, for @p secs seconds: until
 *  a job of the call's owner ends, of those that @p args names or any, or
 *  a timer of the owner fires. A job named that has ended, an ended job
 *  that no one has read, or a timer that fired before the call ends the
 *  call at once.
 */
extern void
agenda_wait_start (struct mcp_slot    *slot,
                   struct json_object *args,
                   uint64_t            secs);

/** The step of a client's call of wait, which the message and the alarm
 *  of its wait queue: checks the wait again.
 */
extern tool_step_fn agenda_wait_step;

/** Stops the alarm of the wait of a client's call of wait, removes the
 *  wait from the ledger and frees it.
 */
extern tool_drop_fn agenda_wait_drop;

#endif /* ASKAI_SRC_AGENDA_H_ */
