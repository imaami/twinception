/** @file Bounded transcript storage, independent of terminal and connections. */
#ifndef TWINCEPTION_SRC_CLI_SCROLLBACK_H_
#define TWINCEPTION_SRC_CLI_SCROLLBACK_H_

#include <stddef.h>

#include "cstr_view.h"
#include "dstr.h"
#include "list.h"

struct cli_scrollback {
	struct list lines;
	size_t      bytes;
	size_t      count;
	size_t      offset;
	size_t      unread;
	size_t      rows;
	size_t      columns;
	size_t      discarded;
};

extern void
cli_scrollback_init (struct cli_scrollback *scrollback);

extern void
cli_scrollback_fini (struct cli_scrollback *scrollback);

/** Append terminal-safe UTF-8; storage evicts only transcript content. */
extern int
cli_scrollback_append (struct cli_scrollback *scrollback,
                       struct cstr_view       text);

extern void
cli_scrollback_scroll (struct cli_scrollback *scrollback,
                       size_t                 rows,
                       bool                   backward);

extern void
cli_scrollback_bottom (struct cli_scrollback *scrollback);

/** Append at most height wrapped rows, with CRLF separators, to output. */
extern int
cli_scrollback_render (struct cli_scrollback *scrollback,
                       size_t                 columns,
                       size_t                 height,
                       struct dstr           *output);

#endif /* TWINCEPTION_SRC_CLI_SCROLLBACK_H_ */
