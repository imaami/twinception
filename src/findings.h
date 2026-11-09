/** @file findings.h
 *
 * The findings tool, which askai offers with --notes. A call of a turn of
 * an ask call adds a finding of the agent (src/finding.h), updates one,
 * lists the findings of every member, shows one finding with its changes,
 * or expands the stored evidence of a finding, of one evidence item, or of
 * all open findings of the agent. Clients can list, show and expand. The
 * evidence of an add or an update comes from the calls of the ring of the
 * ask call and from the items that a result showed whole in an earlier
 * turn (src/evidence.h, src/seen.h). verified and disproved refuse an item
 * that the turns had not seen, and the refusal prints the item, which then
 * counts as shown in this turn. A finding that cites an item of another
 * finding again names the archive files of the item by their git IDs, so
 * that each branch of findings contains all of its evidence.
 *
 * The findings of an agent are the branch refs/heads/agent/NAME/findings
 * of the session repository (src/store.h). Each add and each update is one
 * commit on that branch, made at once with the file of the finding and the
 * archive files of its evidence. The number of a new finding is one more
 * than the highest number on the branch when the commit is made. If
 * another ask call of the agent changed the branch at the same time, the
 * call reads the branch again and makes the commit again, STORE_TRIES
 * commits at most. The result names the ID only after the commit. The
 * journal records no findings call (KF_RERUN), so the turn that askai
 * resumes after a restart makes its findings calls again. An add or an
 * update of that turn first lists the newest commits of the branch of the
 * agent, and a commit with the trailer lines Ask and Call of the call means
 * that the call made the commit before the restart. The result then names
 * that commit, and the call makes no second commit.
 *
 * git reads the files of findings in runs of cat-file whose output stays
 * within STORE_MAX bytes, so that any number of findings can be read. The
 * calls of the tool, the findings of the items that the evidence of an add
 * or an update cites again, and the prompt of an ask call
 * (findings_prompt()) are read the same way.
 *
 * The tool is a built-in plugin without a part. Its service is the struct
 * notes_tool of the notes tool (src/notes_tool.h), which contains the
 * store and the members.
 */
#ifndef ASKAI_SRC_FINDINGS_H_
#define ASKAI_SRC_FINDINGS_H_

#include <stddef.h>

#include "tool.h"

struct dstr;
struct json_object;
struct mcp_slot;
struct notes_tool;
struct plugin;

/** The plugin of the findings tool, which is built into askai. Its
 *  definition is the one for clients, which can list, show and expand.
 */
extern struct plugin const findings_plugin;

/** Returns the definition of the findings tool for the turns. @p service
 *  is the struct notes_tool. Returns nullptr if out of memory.
 */
extern struct json_object *
findings_upstream_definition (void const *service);

/** Handles the output of git for the call in @p slot. This is the step()
 *  of the findings kind. mcp_slot_ask_step() calls it while
 *  findings_prompt() reads the open findings for the prompt.
 */
extern tool_step_fn findings_step;

/** Frees the findings call in @p slot. This is the drop() of the findings
 *  kind, and of the ask kind while findings_prompt() reads.
 */
extern tool_drop_fn findings_drop;

/**
 * Appends to @p prompt, the text that askai adds to the prompt of the ask
 * call in @p slot, the ask call's own slot, the notices of the open findings
 * of the agent in the conversation of the ask call (finding_notice()), at
 * most @p most bytes of lines of findings, and then the notice that names
 * the calls of the ring of the ask call (seen_notice()). @p service is the
 * struct notes_tool. git reads the files of the findings of the agent as
 * for a call of the tool. The reading call is the own word of @p slot until
 * the notices are in @p prompt. The notices start a paragraph of their own
 * if @p prompt does not end with a line end. If git cannot read the
 * findings, the function appends the notice "[askai: Your findings could
 * not be read.]" instead of the open findings. Then the function calls
 * @p then, which goes on with the ask call. For ENOMEM, mcp_slot_fail()
 * ends the ask call instead.
 */
extern void
findings_prompt (struct notes_tool const *service,
                 struct mcp_slot         *slot,
                 struct dstr             *prompt,
                 size_t                   most,
                 tool_step_fn            *then);

#endif /* ASKAI_SRC_FINDINGS_H_ */
