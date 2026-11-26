/*
 * Adapted from llama.cpp tools/cli/cli-ui.h.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#ifndef ASKAI_SRC_CLI_UI_H_
#define ASKAI_SRC_CLI_UI_H_

#include "compat.h"
#include "cstr_view.h"
#include "dstr.h"
#include "flag.h"

struct cli_console;

/** The flags of struct cli_ui. */
enum {
	/** the start mark of the reasoning is written, and its end mark is not
	 */
	CLI_UI_REASONING_OPEN = FLAG_OWN(0),

	/** the output ends a line */
	CLI_UI_TRAILING_NEWLINE = FLAG_OWN(1),
};

/** A streamed turn borrows its console. It owns the name of the tool that
 *  the last calls in a row called, and their count. Their line is written
 *  when the row ends.
 */
struct cli_ui {
	struct cli_console *console;
	struct dstr         tool;
	size_t              calls;  /**< the calls in the row, 0 if none */
	struct flag64       flags;
};

static force_inline struct cli_ui
cli_ui (struct cli_console *console)
{
	return (struct cli_ui){
		.console = console,
		.tool = DSTR_INIT,
		.flags = { CLI_UI_TRAILING_NEWLINE },
	};
}

extern void
cli_ui_fini (struct cli_ui *ui);

extern int
cli_ui_push (struct cli_ui    *ui,
             bool              thinking,
             struct cstr_view  text);

/** Ends the output of the turn: the reasoning and the line of the calls in
 *  a row.
 */
extern int
cli_ui_finish (struct cli_ui *ui);

/** Counts a call of @p tool. Calls of one tool in a row share a line, "* git"
 *  for one call and "* git x5" for five. The line is written when a call of
 *  another tool, text in the conversation or cli_ui_finish() ends the row.
 */
extern int
cli_ui_action (struct cli_ui    *ui,
               struct cstr_view  tool);

extern int
cli_ui_message (struct cli_console *console,
                struct cstr_view    text);

extern int
cli_ui_info (struct cli_console *console,
             struct cstr_view    text);

extern int
cli_ui_error (struct cli_console *console,
              struct cstr_view    title,
              struct cstr_view    detail);

#endif /* ASKAI_SRC_CLI_UI_H_ */
