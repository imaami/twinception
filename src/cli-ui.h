/*
 * Adapted from llama.cpp tools/cli/cli-ui.h.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#ifndef TWINCEPTION_SRC_CLI_UI_H_
#define TWINCEPTION_SRC_CLI_UI_H_

#include "cli-console.h"

/** A streamed turn borrows its console and owns its pending status text. */
struct cli_ui {
	struct cli_console *console;
	struct dstr actor, status;
	bool reasoning, trailing_newline, streaming, shown, pending;
};

struct cli_ui cli_ui(struct cli_console *console);
void cli_ui_fini(struct cli_ui *ui);
int cli_ui_push(struct cli_ui *ui, bool thinking, struct cli_text text);
int cli_ui_finish(struct cli_ui *ui);
int cli_ui_progress(struct cli_ui *ui, struct cli_text actor, struct cli_text text);
int cli_ui_action(struct cli_ui *ui, struct cli_text actor, struct cli_text text);
int cli_ui_message(struct cli_console *console, struct cli_text text);
int cli_ui_info(struct cli_console *console, struct cli_text text);
int cli_ui_error(struct cli_text title, struct cli_text detail);

#endif /* TWINCEPTION_SRC_CLI_UI_H_ */
