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
		"Keep your findings in askai's review record. askai shows the record "
		"again after every compaction, with its log of the lines that you "
		"read in this call with the read command of the git tool, if you have "
		"it, so you need not copy them into notes or your summary. askai "
		"keeps your task as the user gave it through every compaction, so you "
		"need not copy it. add records a claim, a candidate until you check "
		"it. update changes its state: verified when source that you read "
		"shows it true, disproved when it shows it false, retracted when you "
		"withdraw it, or candidate again. Each update needs evidence, and can "
		"also change the claim or the location if they were wrong; the "
		"finding's history keeps the old text. verified and disproved need "
		"the lines of the location in the read log, so they need the git "
		"tool, and askai records the commit that the lines were read at. show "
		"gives the record, or with id one finding's history. The record "
		"holds at most 4 MiB.",
		jb_object(
			"action", schema_enum(
				schema_names(review_action_names, REVIEW_ACTIONS),
				"add a finding, update one, or show the record."),
			"claim", schema_property("string",
				"Required for add, and for update if the claim was "
				"wrong: what you found."),
			"location", schema_property("string",
				"For add, and for update if the location was wrong: "
				"the lines that the claim is about, as path:line or "
				"path:first-last, such as src/mcp.c:120-140. A commit "
				"and a colon may come first, as git grep prints them; "
				"HEAD and a colon are as no commit."),
			"state", schema_enum(
				schema_names(review_state_names, REVIEW_STATES),
				"For add: candidate (the default), verified or "
				"disproved. Required for update: the new state, or "
				"the same."),
			"evidence", schema_property("string",
				"Required for update, and for add with verified or "
				"disproved: what you read that shows the state, or "
				"why you retract the finding."),
			"id", schema_count(1,
				"Required for update, and for show to give one "
				"finding's history: the number of the finding."),
			"offset", schema_count(0,
				"For show: the byte of the output to start from, to "
				"read on past where it was cut."),
			nullptr),
		CSTR_VIEW("action"));
}

void
findings_drop (struct mcp_slot *slot)
{
	void **own = mcp_slot_own(slot);
	struct findings_call *call = *own;
	json_object_put(call->args);
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
