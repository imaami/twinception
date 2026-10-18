/*
 * Adapted from llama.cpp tools/cli/cli-ui.h.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#ifndef TWINCEPTION_SRC_CLI_UI_H_
#define TWINCEPTION_SRC_CLI_UI_H_

#include "cstr_view.h"
#include "dstr.h"

struct cli_console;

/** A streamed turn borrows its console and owns its pending status text.
 */
struct cli_ui {
	struct cli_console *console;
	struct dstr         actor;
	struct dstr         status;
	bool                reasoning;
	bool                trailing_newline;
	bool                streaming;
	bool                shown;
	bool                pending;
};

struct cli_ui
cli_ui (struct cli_console *console);

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
cli_ui_error (struct cstr_view title,
              struct cstr_view detail);

#endif /* TWINCEPTION_SRC_CLI_UI_H_ */
