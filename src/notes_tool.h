/** @file notes_tool.h
 *
 * The notes tool. Its calls write and read notes in the shared git
 * repository of src/notes.h. A caller writes only to its own notebook. A
 * caller can read every notebook in the repository. The tool is a built-in
 * plugin with a part on each client loop. The part also runs the git
 * commands that read the notes for the prompt of an ask call
 * (notes_tool_loop_prompt()) and that save the review record of an ask
 * call (notes_tool_loop_save()).
 */
#ifndef ASKAI_SRC_NOTES_TOOL_H_
#define ASKAI_SRC_NOTES_TOOL_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "mcp.h"

struct hexid;
struct notes_tool_loop;
struct plugin;

/** The service of the notes tool. All loops share it. mcp_static_create()
 *  fills it before mcp_static_build() reads it.
 */
struct notes_tool {
	struct notes const *notes;         /**< nullptr without --notes */
	size_t              files_inline;  /**< the most bytes of a file that a
	                                        client hands askai inline */
	size_t              actions;       /**< the number of leading entries of
	                                        notes_action_names that are valid
	                                        actions for the upstream. It is
	                                        NOTES_ACTION_ATTACH, or all of
	                                        them if the upstream supports
	                                        file uploads (ai_upload()). */
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
 *  kind and of the ask kind.
 */
extern tool_drop_fn notes_tool_drop;

/**
 * Reads the notes of the caller for the prompt of the ask call in @p slot,
 * the ask call's own slot. git runs on the loop of the part @p loop. The
 * list of the notes and the newest note go into @p prompt, the text that
 * askai adds to the prompt. The prompt shows at most @p most bytes of the
 * newest note. @p what says whether the ask call starts or compacts its
 * context. If @p thread is not nullptr, git then reads the review record of
 * the thread into the record of the ask call (mcp_slot_review()), and the
 * text of the record is appended to @p prompt after the notes. Once the
 * notes are in @p prompt, the function calls @p then, which goes on with
 * the ask call. If git cannot list the notes for NOTES_TOOL_RESUME, or
 * cannot read the review record, the ask call ends as failed.
 */
extern void
notes_tool_loop_prompt (struct notes_tool_loop *loop,
                        struct mcp_slot        *slot,
                        struct dstr            *prompt,
                        enum notes_tool_prompt  what,
                        size_t                  most,
                        struct hexid const     *thread,
                        tool_step_fn           *then);

/**
 * Commits the findings of the review record of the ask call in @p slot,
 * the ask call's own slot, on the ref of the review record of @p thread.
 * git runs on the loop of the part @p loop. The function appends a
 * sentence about the save to @p told, the trace of the ask call, and then
 * calls @p then. Only ENOMEM ends the ask call.
 */
extern void
notes_tool_loop_save (struct notes_tool_loop *loop,
                      struct mcp_slot        *slot,
                      struct dstr            *told,
                      struct hexid const     *thread,
                      tool_step_fn           *then);

#endif /* ASKAI_SRC_NOTES_TOOL_H_ */
