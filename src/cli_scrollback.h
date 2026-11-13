/** @file cli_scrollback.h
 *
 * Bounded transcript storage, independent of terminal and connections.
 */
#ifndef ASKAI_SRC_CLI_SCROLLBACK_H_
#define ASKAI_SRC_CLI_SCROLLBACK_H_

#include <stddef.h>

#include "cli_style.h"
#include "compat.h"
#include "cstr_view.h"
#include "dstr.h"
#include "list.h"
#include "tally.h"

struct cli_scrollback {
	struct list  lines;
	struct tally used;  /**< the lines and the bytes of their text */
	size_t       offset;
	size_t       unread;
	size_t       rows;
	size_t       columns;
	size_t       discarded;
	size_t       styles;
};

/** Returns an empty scrollback for the object at @p self. The empty list
 *  of lines stores the address &self->lines. So @p self must be the
 *  address of the object that the result initializes, and that object
 *  must not be copied afterwards.
 */
static force_inline struct cli_scrollback
cli_scrollback (struct cli_scrollback *self)
{
	return (struct cli_scrollback){.lines = list(&self->lines)};
}

extern void
cli_scrollback_init (struct cli_scrollback *dest);

extern void
cli_scrollback_fini (struct cli_scrollback *scrollback);

/** Append terminal-safe UTF-8; storage evicts only transcript content. */
extern int
cli_scrollback_append (struct cli_scrollback *scrollback,
                       struct cstr_view       text);

/** Retain a palette index with the text; adjacent equal styles coalesce. */
extern int
cli_scrollback_append_style (struct cli_scrollback *scrollback,
                             struct cstr_view       text,
                             enum cli_style         style);

extern void
cli_scrollback_scroll (struct cli_scrollback *scrollback,
                       size_t                 rows,
                       bool                   backward);

extern void
cli_scrollback_bottom (struct cli_scrollback *scrollback);

/** Paint a rectangle at 1-based terminal coordinates. Every row is explicitly
 * positioned and padded to columns cells; neighboring panes are not erased.
 * The palette (cli_style_palette()) colors the text, with TEXT as the
 * pane's default style.
 * An optional caller-owned cache contains height initialized strings. Only
 * changed spans are emitted and the cache is updated; clear it after terminal
 * content is invalidated. Each emitted span establishes its own color.
 */
extern int
cli_scrollback_render_pane (struct cli_scrollback        *scrollback,
                            size_t                        left,
                            size_t                        top,
                            size_t                        columns,
                            size_t                        height,
                            struct cli_style_entry const *palette,
                            struct dstr                  *cache,
                            struct dstr                  *output);

#endif /* ASKAI_SRC_CLI_SCROLLBACK_H_ */
