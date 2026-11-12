/** @file notes_tool.h
 *
 * The notes tool. Its calls write and read the notebooks of the members of
 * the session (src/notes.h, src/member.h). The turns of an ask call write
 * as the agent of askai. A client writes as the member of its login. A
 * caller writes only its own notebook, and can read every notebook. The
 * tool is a built-in plugin with a part on each client loop. A call uses
 * the table of the members on the loop of the call (mcp_slot_members()).
 * The part also runs the git commands that read the notes of the agent for
 * the prompt of an ask call (notes_tool_loop_prompt()).
 */
#ifndef ASKAI_SRC_NOTES_TOOL_H_
#define ASKAI_SRC_NOTES_TOOL_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "mcp.h"
#include "shelf.h"

struct member_registry;
struct plugin;

/** The service of the notes tool. All loops share it. mcp_static_create()
 *  fills it before mcp_static_build() reads it.
 */
struct notes_tool {
	struct member_registry const *members;       /**< the registry of the
	                                                  session, with its
	                                                  repository
	                                                  (member_registry_store()),
	                                                  nullptr without
	                                                  --notes */
	size_t                        files_inline;  /**< the most bytes of a
	                                                  file that a client
	                                                  hands askai inline */
	size_t                        attach;        /**< 1 if the upstream
	                                                  supports file uploads
	                                                  (ai_upload()), so
	                                                  that a turn can use
	                                                  the action attach,
	                                                  else 0. A bool would
	                                                  leave padding. */
};

/** The part of the notes tool on one loop. */
struct notes_tool_loop {
	struct shelf             spare;  /**< finished calls, kept for reuse
	                                      with the storage of their
	                                      output */
	struct notes_tool const *tool;   /**< the service */
};

/** The reason why notes_tool_loop_prompt() reads the notes. */
enum notes_tool_prompt : uint8_t {
	NOTES_TOOL_RESUME,   /**< the start of an ask call */
	NOTES_TOOL_COMPACT,  /**< a compaction, by compact or by a full context */
};

/** The plugin of the notes tool, which is built into askai. Its part on
 *  each loop is a struct notes_tool_loop. The flag TOOL_LENT lets the
 *  "upstream" member of a role name the notes tool. The turns always get
 *  the tool.
 */
extern struct plugin const notes_tool_plugin;

/** Creates the part of the notes tool for @p service, a struct notes_tool.
 *  Returns the part, or nullptr with ENOMEM in @p *e.
 */
extern mcp_part_create_fn notes_tool_loop_create;

/** Frees the spare calls and the part at @p *p_part, and sets @p *p_part
 *  to nullptr.
 */
extern mcp_part_destroy_fn notes_tool_loop_destroy;

/** Begins a window of reuse of the spare calls of @p loop, as
 *  buf_stash_age() does for a stash.
 */
extern void
notes_tool_loop_age (struct notes_tool_loop *loop);

/** Frees the spare calls of @p loop and the storage of their output, at
 *  BUF_HIGH those unused since the notes_tool_loop_age() call before the
 *  last one, and at BUF_CRITICAL all. Counts in units of release work
 *  (buf_units()), always frees the first call and no later one that would
 *  make the total greater than @p max. Returns the units, 0 once none are
 *  left to free.
 */
extern size_t
notes_tool_loop_relieve (struct notes_tool_loop *loop,
                         enum buf_pressure       level,
                         size_t                  max);

/** Returns the definition of the notes tool for the upstream. @p service
 *  is the struct notes_tool.
 */
extern tool_definition_fn notes_tool_upstream_definition;

/** Handles the output of git for the call in @p slot. For attach, it also
 *  handles the answer to the upload. This is the step() of the notes kind.
 *  mcp_slot_ask_step() calls it for the prompt notes.
 */
extern tool_step_fn notes_tool_step;

/** Releases the notes call in @p slot. Without memory pressure, the call
 *  object stays on its loop for reuse. This is the drop() of the notes
 *  kind, and of the ask kind while git reads the notes for the prompt.
 */
extern tool_drop_fn notes_tool_drop;

/**
 * Writes the notices of the notes for the prompt of the ask call in
 * @p slot, the ask call's own slot, to @p prompt, the text that askai adds
 * to the prompt. git runs on the loop of the part @p loop. The notices say
 * who the agent is and who made the ask call, list the members, and show
 * the note history of the agent and the newest note, at most @p most bytes
 * of it. @p what says whether the ask call starts or compacts its context.
 * As the ask call starts, the function claims the member of the login of
 * its client (member_table_claim()), and git lists the records of the
 * members (member_list). The member is the asker of the request
 * (mcp_slot_asker()), whom the commits of the turns name. Once the notices
 * are in @p prompt, the function calls @p then, which goes on with the ask
 * call. If git cannot list the notes for NOTES_TOOL_RESUME, the ask call
 * ends as failed.
 */
extern void
notes_tool_loop_prompt (struct notes_tool_loop *loop,
                        struct mcp_slot        *slot,
                        struct dstr            *prompt,
                        enum notes_tool_prompt  what,
                        size_t                  most,
                        tool_step_fn           *then);

#endif /* ASKAI_SRC_NOTES_TOOL_H_ */
