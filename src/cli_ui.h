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

	/** text was shown */
	CLI_UI_TEXT_SHOWN = FLAG_OWN(2),

	/** a progress line was shown */
	CLI_UI_PROGRESS_SHOWN = FLAG_OWN(3),

	/** the newest progress line is stored and is shown when the text ends
	 */
	CLI_UI_PROGRESS_PENDING = FLAG_OWN(4),
};

/** A streamed turn borrows its console and owns its pending status text.
 */
struct cli_ui {
	struct cli_console *console;
	struct dstr         actor;
	struct dstr         status;
	struct flag64       flags;
};

static force_inline struct cli_ui
cli_ui (struct cli_console *console)
{
	return (struct cli_ui){
		.console = console,
		.actor = DSTR_INIT,
		.status = DSTR_INIT,
		.flags = { CLI_UI_TRAILING_NEWLINE },
	};
}

extern void
cli_ui_fini (struct cli_ui *ui);

extern int
cli_ui_push (struct cli_ui    *ui,
             bool              thinking,
             struct cstr_view  text);

extern int
cli_ui_finish (struct cli_ui *ui);

extern int
cli_ui_progress (struct cli_ui    *ui,
                 struct cstr_view  actor,
                 struct cstr_view  text);

extern int
cli_ui_action (struct cli_ui    *ui,
               struct cstr_view  actor,
               struct cstr_view  text);

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
