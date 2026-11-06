/** @file notes_tool.h
 *
 * The notes tool. Its calls write and read notes in the shared git
 * repository of src/notes.h. A caller writes only to its own notebook. A
 * caller can read every notebook in the repository. The tool is a built-in
 * plugin with a part on each client loop. The part also runs the git
 * commands that read the notes for the prompt of an ask call
 * (notes_tool_loop_prompt()).
 */
#ifndef TWINCEPTION_SRC_NOTES_TOOL_H_
#define TWINCEPTION_SRC_NOTES_TOOL_H_

#include <stddef.h>
#include <stdint.h>

#include "mcp.h"

struct notes_tool_loop;
struct plugin;

/** The service of the notes tool. All loops share it. mcp_static_create()
 *  fills it before mcp_static_build() reads it.
 */
struct notes_tool {
	struct notes const *notes;         /**< nullptr without --notes */
	size_t              files_inline;  /**< the most bytes of a file that a
	                                        client hands askai inline */
	uint64_t            attach;        /**< nonzero if the upstream takes
	                                        files (ai_upload()). It is wider
	                                        than a bool to fill the padding. */
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
 *  kind and of the ask kind.
 */
extern tool_drop_fn notes_tool_drop;

/**
 * Reads the notes of the caller for the prompt of the ask call in @p slot,
 * the ask call's own slot. git runs on the loop of the part @p loop. The
 * list of the notes and the newest note go into @p prompt, the text that
 * askai adds to the prompt. The prompt shows at most @p most bytes of the
 * newest note. @p what says whether the ask call starts or compacts its
 * context. Once the notes are in @p prompt, the function calls @p then,
 * which goes on with the ask call. If git cannot list the notes for
 * NOTES_TOOL_RESUME, the ask call ends as failed.
 */
extern void
notes_tool_loop_prompt (struct notes_tool_loop *loop,
                        struct mcp_slot        *slot,
                        struct dstr            *prompt,
                        enum notes_tool_prompt  what,
                        size_t                  most,
                        tool_step_fn           *then);

#endif /* TWINCEPTION_SRC_NOTES_TOOL_H_ */
