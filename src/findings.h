/** @file findings.h
 *
 * The findings tool, which askai lends only to the turns of an ask call.
 * A call adds a finding to the review record of the ask call (src/review.h),
 * updates one, or shows the record. The row of the tool in mcp_rows
 * (src/mcp.c) gives its kind these functions.
 */
#ifndef TWINCEPTION_SRC_FINDINGS_H_
#define TWINCEPTION_SRC_FINDINGS_H_

#include "tool.h"

/** Returns the definition of the findings tool. Its enums are the names
 *  in review_action_names and review_state_names.
 */
extern tool_definition_fn findings_definition;

/** Takes the offset of the call, then runs its first step. */
extern tool_start_fn findings_start;

/** Runs review_call() on the record of mcp_slot_review(). A refusal fails
 *  the call with the whole refusal as its output, without an offset.
 */
extern tool_step_fn findings_step;

/** Frees the own word of the call. */
extern tool_drop_fn findings_drop;

#endif /* TWINCEPTION_SRC_FINDINGS_H_ */
