/** @file findings.c
 *
 * The findings tool.
 */
#include <errno.h>
#include <stdlib.h>

#include "dstr.h"
#include "findings.h"
#include "jb.h"
#include "loop.h"
#include "mcp.h"
#include "notice.h"
#include "review.h"
#include "schema.h"

/* The state of a findings call, which mcp_slot_own() stores in the slot. */
struct findings_call {
	struct json_object *args;    /* a reference to the arguments */
	struct dstr         out;     /* what review_call() wrote */
	struct tool_cursor  cursor;  /* the position of a show in its text */
	size_t              texts;   /* the bytes of the claim and the evidence */
};

struct json_object *
findings_definition (void const *service)
{
	(void)service;
	static_assert(TOOL_TEXT_MAX == 4U << 20, "update the description");
	return schema_tool("findings",
		"Record your findings in the review record of this ask call. askai "
		"shows the record again after every compaction. Change the state of a "
		"finding with update: verified if source that you read shows the claim "
		"true, disproved if the source shows the claim false, retracted if you "
		"withdraw the claim, or candidate again. verified and disproved require "
		"the lines of the location in the read log. The read log lists the "
		"lines that the read command of the git tool and the describe action "
		"of the code tool showed you. Verify or disprove a finding in a later "
		"turn than the read of the lines. The history of a finding lists every "
		"change. A record can start with findings from an earlier ask call. "
		"The read log of each ask call starts empty. The record is limited to "
		"4 MiB.",
		jb_object(
			"action", schema_enum(
				schema_names(review_action_names, REVIEW_ACTIONS),
				"add a finding, update a finding, or show the record."),
			"claim", schema_property("string",
				"What you found. Required for add, and for update to "
				"correct the claim."),
			"location", schema_property("string",
				"The lines of a committed file that show the claim, as "
				"path:line or path:first-last, such as src/mcp.c:120-140. "
				"A commit ID and a colon can come first, as in git grep "
				"output. HEAD: is the same as no commit. For update, "
				"give location only to correct the location. Leave out "
				"location if no lines of a file show the claim."),
			"state", schema_enum(
				schema_names(review_state_names, REVIEW_STATES),
				"For add: candidate (the default), verified or disproved. "
				"Required for update."),
			"evidence", schema_property("string",
				"What you read that shows the state. For retracted, the "
				"reason you withdraw the finding. Required for update, "
				"and for add with verified or disproved."),
			"id", schema_count(1,
				"The number of a finding. Required for update. show with "
				"id prints the history of the finding."),
			"offset", schema_count(0, TOOL_OFFSET_ABOUT),
			nullptr),
		CSTR_VIEW("action"));
}

void
findings_drop (struct mcp_slot *slot)
{
	void **own = mcp_slot_own(slot);
	struct findings_call *call = *own;
	json_object_put(call->args);
	call->args = nullptr;
	dstr_cleanup(&call->out);
	free(*own);
	*own = nullptr;
}

void
findings_step (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct loop *loop = mcp_slot_loop(slot);
	/* review_call() copies the claim and the evidence of an add or an
	 * update. If less LOOP_COPY room is left in this step than they need,
	 * and the step has used part of its share, the call runs in the next
	 * step. A show writes at most the room of the step, and the rest of its
	 * text in later steps (EAGAIN). */
	if (!loop_fits(loop, LOOP_COPY, call->texts)) {
		mcp_slot_ready(slot);
		return;
	}
	size_t const had = dstr_len(&call->out);
	int e = review_call(mcp_slot_review(slot), call->args, mcp_slot_turn(slot), notice_kept,
	                    &call->cursor, loop_room(loop, LOOP_COPY), &call->out);
	struct cstr_view const out = cstr_view_dstr(&call->out);
	loop_work(loop, LOOP_COPY, call->texts + out.len - had);
	switch (e) {
	case 0:
		break;
	case EAGAIN:
		mcp_slot_ready(slot);
		return;
	case EINVAL:
		/* mcp_slot_offset() without arguments sets the offset of a refusal
		 * to 0. */
		(void)mcp_slot_offset(slot, nullptr);
		break;
	default:
		mcp_slot_fail(slot, e);
		return;
	}
	mcp_slot_lend(slot, out);
	mcp_slot_finish(slot, e ? TOOL_FAILED : 0);
}

void
findings_start (struct mcp_slot    *slot,
                struct json_object *args)
{
	if (mcp_slot_offset(slot, args))
		return;
	struct findings_call *call = malloc(sizeof *call);
	if (!call) {
		mcp_slot_fail(slot, ENOMEM);
		return;
	}
	*call = (struct findings_call){
		.args  = json_object_get(args),
		.out   = DSTR_INIT,
		.texts = jb_view(jb_get(args, "claim")).len + jb_view(jb_get(args, "evidence")).len,
	};
	*mcp_slot_own(slot) = call;
	findings_step(slot);
}
