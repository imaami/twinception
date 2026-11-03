/** @file findings.c
 *
 * The findings tool.
 */
#include <errno.h>
#include <stdlib.h>

#include "dstr.h"
#include "findings.h"
#include "jb.h"
#include "loop_export.h"
#include "mcp.h"
#include "notice.h"
#include "review.h"

/* The own word of a findings call. */
struct findings_call {
	struct json_object *args;  /* a reference to the arguments */
	struct dstr         out;   /* what review_call() wrote */
};

/* A JSON array of the @p n names at @p names; nullptr if out of memory. */
static struct json_object *
findings_names (struct tool_text const *names,
                size_t                  n)
{
	struct json_object *arr = json_object_new_array();
	for (size_t i = 0; arr && i < n; ++i)
		if (jb_push(arr, json_object_new_string_len(names[i].ptr, (int)names[i].len))) {
			json_object_put(arr);
			arr = nullptr;
		}
	return arr;
}

struct json_object *
findings_definition (void const *service)
{
	(void)service;
	static_assert(REVIEW_MAX == 4 << 20, "the description says");
	return jb_object(
		"name", json_object_new_string("findings"),
		"description", json_object_new_string(
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
			"holds at most 4 MiB."),
		"inputSchema", jb_object(
			"type", json_object_new_string("object"),
			"properties", jb_object(
				"action", jb_object(
					"type", json_object_new_string("string"),
					"enum", findings_names(review_action_names, REVIEW_ACTIONS),
					"description", json_object_new_string(
						"add a finding, update one, or show the record."),
					nullptr),
				"claim", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"Required for add, and for update if the claim was "
						"wrong: what you found."),
					nullptr),
				"location", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"For add, and for update if the location was wrong: "
						"the lines that the claim is about, as path:line or "
						"path:first-last, such as src/mcp.c:120-140. A commit "
						"and a colon may come first, as git grep prints them; "
						"HEAD and a colon are as no commit."),
					nullptr),
				"state", jb_object(
					"type", json_object_new_string("string"),
					"enum", findings_names(review_state_names, REVIEW_STATES),
					"description", json_object_new_string(
						"For add: candidate (the default), verified or "
						"disproved. Required for update: the new state, or "
						"the same."),
					nullptr),
				"evidence", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"Required for update, and for add with verified or "
						"disproved: what you read that shows the state, or "
						"why you retract the finding."),
					nullptr),
				"id", jb_object(
					"type", json_object_new_string("integer"),
					"minimum", json_object_new_int(1),
					"description", json_object_new_string(
						"Required for update, and for show to give one "
						"finding's history: the number of the finding."),
					nullptr),
				"offset", jb_object(
					"type", json_object_new_string("integer"),
					"minimum", json_object_new_int(0),
					"description", json_object_new_string(
						"For show: the byte of the output to start from, to "
						"read on past where it was cut."),
					nullptr),
				nullptr),
			"required", jb_list(json_object_new_string("action")),
			"additionalProperties", json_object_new_boolean(0),
			nullptr),
		nullptr);
}

void
findings_drop (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	json_object_put(call->args);
	dstr_cleanup(&call->out);
	free(call);
	*mcp_slot_own(slot) = nullptr;
}

void
findings_step (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct loop *loop = mcp_slot_loop(slot);
	size_t had = dstr_len(&call->out);
	/* review_call() copies the claim and the evidence of an add or an
	 * update. If less LOOP_COPY room is left in this step than they need,
	 * and the step has used part of its share, wait for the next step. */
	if (!had) {
		size_t claim = 0;
		size_t evidence = 0;
		(void)jb_str(jb_get(call->args, "claim"), &claim);
		(void)jb_str(jb_get(call->args, "evidence"), &evidence);
		uint64_t room = loop_room(loop, LOOP_COPY);
		if (room < claim + evidence && room < LOOP_STEP_COPY) {
			mcp_slot_ready(slot);
			return;
		}
		loop_work(loop, LOOP_COPY, claim + evidence);
	}
	/* A show writes as much as the room allows and goes on in later steps
	 * (EAGAIN). */
	int e = review_call(mcp_slot_review(slot), call->args, mcp_slot_turn(slot), notice_kept,
	                    had, loop_room(loop, LOOP_COPY), &call->out);
	loop_work(loop, LOOP_COPY, dstr_len(&call->out) - had);
	struct tool_text out = { dstr_get(&call->out), dstr_len(&call->out) };
	if (e == EAGAIN) {
		mcp_slot_ready(slot);
	} else if (e && e != EINVAL) {
		mcp_slot_fail(slot, e);
	} else {
		/* A refusal (EINVAL) has no offset. mcp_slot_offset() without
		 * args sets the offset to 0. */
		if (e)
			(void)mcp_slot_offset(slot, nullptr);
		mcp_slot_lend(slot, out);
		mcp_slot_finish(slot, e ? TOOL_FAILED : 0);
	}
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
	*call = (struct findings_call){ .args = json_object_get(args), .out = DSTR_INIT };
	*mcp_slot_own(slot) = call;
	findings_step(slot);
}
