/*
 * Adapted from llama.cpp tools/cli/cli-ui.h, commit
 * b76b13379d3b7d67ab0bc424516bb6fd2522bd8e.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#include <errno.h>

#include "cli_console.h"
#include "cli_ui.h"

struct cli_ui
cli_ui (struct cli_console *console)
{
	return (struct cli_ui){
		.console = console,
		.actor = DSTR_INIT,
		.status = DSTR_INIT,
		.trailing_newline = true,
	};
}

static int
cli_ui_newline (struct cli_ui *ui)
{
	if (!ui->trailing_newline) {
		int e = cli_console_write(ui->console, CSTR_VIEW("\n"));
		if (e)
			return e;
		ui->trailing_newline = true;
	}
	return 0;
}

int
cli_ui_push (struct cli_ui    *ui,
             bool              thinking,
             struct cstr_view  text)
{
	if (!text.len)
		return 0;
	ui->streaming = true;
	int e = cli_console_set_display(ui->console, ui->reasoning ? CLI_STYLE_REASONING : CLI_STYLE_RESET);
	if (e)
		return e;
	if (thinking != ui->reasoning) {
		e = cli_ui_newline(ui);
		if (!e && thinking)
			e = cli_console_set_display(ui->console, CLI_STYLE_REASONING);
		if (!e)
			e = cli_console_write(ui->console, thinking ? CSTR_VIEW("[Start thinking]\n\n")
			                                            : CSTR_VIEW("[End thinking]\n\n"));
		if (!e && !thinking)
			e = cli_console_set_display(ui->console, CLI_STYLE_RESET);
		if (e)
			return e;
		ui->reasoning = thinking;
	}
	e = cli_console_write(ui->console, text);
	ui->trailing_newline = text.data[text.len - 1] == '\n';
	return e ? e : cli_console_flush(ui->console);
}

static int
cli_ui_write_action (struct cli_ui    *ui,
                     struct cstr_view  actor,
                     struct cstr_view  text)
{
	int e = cli_console_set_display(ui->console, CLI_STYLE_INFO);
	struct cstr_view parts[] = {CSTR_VIEW("* "), actor, CSTR_VIEW(" "), text, CSTR_VIEW("\n")};
	for (size_t i = 0; !e && i < sizeof parts / sizeof *parts; ++i)
		e = cli_console_write(ui->console, parts[i]);
	int reset = cli_console_set_display(ui->console, CLI_STYLE_RESET);
	int flush = cli_console_flush(ui->console);
	return e ? e : reset ? reset : flush;
}

int
cli_ui_finish (struct cli_ui *ui)
{
	int e = cli_console_set_display(ui->console, ui->reasoning ? CLI_STYLE_REASONING : CLI_STYLE_RESET);
	if (!e)
		e = cli_ui_newline(ui);
	if (!e && ui->reasoning)
		e = cli_console_write(ui->console, CSTR_VIEW("[End thinking]\n"));
	ui->reasoning = false;
	int reset = cli_console_set_display(ui->console, CLI_STYLE_RESET);
	if (!e && !reset && ui->pending)
		e = cli_ui_write_action(ui, cstr_view_dstr(&ui->actor), cstr_view_dstr(&ui->status));
	dstr_clear(&ui->actor);
	dstr_clear(&ui->status);
	ui->streaming = ui->shown = ui->pending = false;
	int flush = cli_console_flush(ui->console);
	return e ? e : reset ? reset : flush;
}

void
cli_ui_fini (struct cli_ui *ui)
{
	(void)cli_ui_finish(ui);
	dstr_cleanup(&ui->actor);
	dstr_cleanup(&ui->status);
}

int
cli_ui_progress (struct cli_ui    *ui,
                 struct cstr_view  actor,
                 struct cstr_view  text)
{
	if (!text.len || (cstr_view_equal(actor, cstr_view_dstr(&ui->actor)) &&
	                  cstr_view_equal(text, cstr_view_dstr(&ui->status))))
		return 0;
	/* Reserve both before replacing either, leaving a valid status on failure. */
	size_t actor_len = dstr_len(&ui->actor);
	size_t status_len = dstr_len(&ui->status);
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

int
cli_ui_action (struct cli_ui    *ui,
               struct cstr_view  actor,
               struct cstr_view  text)
{
	int e = cli_ui_finish(ui);
	return e ? e : cli_ui_write_action(ui, actor, text);
}

static int
cli_ui_write_message (struct cli_console *console,
                      struct cstr_view    text,
                      enum cli_style      display)
{
	int e = cli_console_set_display(console, display);
	if (!e)
		e = cli_console_write(console, text);
	if (!e)
		e = cli_console_write(console, CSTR_VIEW("\n"));
	int reset = cli_console_set_display(console, CLI_STYLE_RESET);
	return e ? e : reset ? reset : cli_console_flush(console);
}

int
cli_ui_message (struct cli_console *console,
                struct cstr_view    text)
{
	return cli_ui_write_message(console, text, CLI_STYLE_RESET);
}

int
cli_ui_info (struct cli_console *console,
             struct cstr_view    text)
{
	return cli_ui_write_message(console, text, CLI_STYLE_INFO);
}

int
cli_ui_error (struct cli_console *console,
              struct cstr_view    title,
              struct cstr_view    detail)
{
	struct cstr_view parts[] = {CSTR_VIEW("Error: "), title, CSTR_VIEW("\n"), detail, CSTR_VIEW("\n")};
	size_t count = detail.len ? sizeof parts / sizeof *parts : 3;
	for (size_t i = 0; i < count; ++i) {
		int e = cli_console_error(console, parts[i]);
		if (e)
			return e;
	}
	return cli_console_flush(console);
}
