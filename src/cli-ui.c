/*
 * Adapted from llama.cpp tools/cli/cli-ui.h, commit
 * b76b13379d3b7d67ab0bc424516bb6fd2522bd8e.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#include <errno.h>

#include "cli-ui.h"

struct cli_ui cli_ui(struct cli_console *console)
{
	return (struct cli_ui){.console = console, .actor = DSTR_INIT,
	                       .status = DSTR_INIT, .trailing_newline = true};
}

static int cli_ui_newline(struct cli_ui *ui)
{
	if (!ui->trailing_newline) {
		int e = cli_console_write(ui->console, CLI_TEXT("\n"));
		if (e)
			return e;
		ui->trailing_newline = true;
	}
	return 0;
}

int cli_ui_push(struct cli_ui *ui, bool thinking, struct cli_text text)
{
	if (!text.len)
		return 0;
	ui->streaming = true;
	int e = 0;
	if (thinking != ui->reasoning) {
		e = cli_ui_newline(ui);
		if (!e && thinking)
			e = cli_console_set_display(ui->console, CLI_CONSOLE_REASONING);
		if (!e)
			e = cli_console_write(ui->console, thinking ? CLI_TEXT("[Start thinking]\n\n")
			                                          : CLI_TEXT("[End thinking]\n\n"));
		if (!e && !thinking)
			e = cli_console_set_display(ui->console, CLI_CONSOLE_RESET);
		if (e)
			return e;
		ui->reasoning = thinking;
	}
	e = cli_console_write(ui->console, text);
	ui->trailing_newline = text.data[text.len - 1] == '\n';
	return e ? e : cli_console_flush(ui->console);
}

static int cli_ui_write_action(struct cli_ui *ui, struct cli_text actor, struct cli_text text)
{
	int e = cli_console_set_display(ui->console, CLI_CONSOLE_INFO);
	struct cli_text parts[] = {CLI_TEXT("* "), actor, CLI_TEXT(" "), text, CLI_TEXT("\n")};
	for (size_t i = 0; !e && i < sizeof parts / sizeof *parts; ++i)
		e = cli_console_write(ui->console, parts[i]);
	int reset = cli_console_set_display(ui->console, CLI_CONSOLE_RESET);
	int flush = cli_console_flush(ui->console);
	return e ? e : reset ? reset : flush;
}

int cli_ui_finish(struct cli_ui *ui)
{
	int e = cli_ui_newline(ui);
	if (!e && ui->reasoning)
		e = cli_console_write(ui->console, CLI_TEXT("[End thinking]\n"));
	ui->reasoning = false;
	int reset = cli_console_set_display(ui->console, CLI_CONSOLE_RESET);
	if (!e && !reset && ui->pending)
		e = cli_ui_write_action(ui, cli_text_dstr(&ui->actor), cli_text_dstr(&ui->status));
	dstr_clear(&ui->actor);
	dstr_clear(&ui->status);
	ui->streaming = ui->shown = ui->pending = false;
	int flush = cli_console_flush(ui->console);
	return e ? e : reset ? reset : flush;
}

void cli_ui_fini(struct cli_ui *ui)
{
	(void)cli_ui_finish(ui);
	dstr_cleanup(&ui->actor);
	dstr_cleanup(&ui->status);
}

int cli_ui_progress(struct cli_ui *ui, struct cli_text actor, struct cli_text text)
{
	if (!text.len || (cli_text_equal(actor, cli_text_dstr(&ui->actor)) &&
	                 cli_text_equal(text, cli_text_dstr(&ui->status))))
		return 0;
	/* Reserve both before replacing either, leaving a valid status on failure. */
	size_t actor_len = dstr_len(&ui->actor), status_len = dstr_len(&ui->status);
	if (!dstr_reserve(&ui->actor, actor.len > actor_len ? actor.len - actor_len : 0) ||
	    !dstr_reserve(&ui->status, text.len > status_len ? text.len - status_len : 0))
		return ENOMEM;
	dstr_clear(&ui->actor);
	dstr_clear(&ui->status);
	if (!dstr_cat_n(&ui->actor, actor.data, actor.len) || !dstr_cat_n(&ui->status, text.data, text.len))
		return ENOMEM;
	ui->pending = ui->streaming || ui->shown;
	if (ui->pending)
		return 0;
	ui->shown = true;
	return cli_ui_write_action(ui, actor, text);
}

int cli_ui_action(struct cli_ui *ui, struct cli_text actor, struct cli_text text)
{
	int e = cli_ui_finish(ui);
	return e ? e : cli_ui_write_action(ui, actor, text);
}

int cli_ui_message(struct cli_console *console, struct cli_text text)
{
	int e = cli_console_write(console, text);
	if (!e)
		e = cli_console_write(console, CLI_TEXT("\n"));
	return e ? e : cli_console_flush(console);
}

int cli_ui_info(struct cli_console *console, struct cli_text text)
{
	int e = cli_console_set_display(console, CLI_CONSOLE_INFO);
	if (!e)
		e = cli_ui_message(console, text);
	int reset = cli_console_set_display(console, CLI_CONSOLE_RESET);
	return e ? e : reset;
}

int cli_ui_error(struct cli_text title, struct cli_text detail)
{
	struct cli_text parts[] = {CLI_TEXT("Error: "), title, CLI_TEXT("\n"), detail, CLI_TEXT("\n")};
	size_t count = detail.len ? sizeof parts / sizeof *parts : 3;
	for (size_t i = 0; i < count; ++i) {
		int e = cli_console_write_error(parts[i]);
		if (e)
			return e;
	}
	return 0;
}
