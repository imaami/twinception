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
cli_ui_write (struct cli_ui    *ui,
              bool              reasoning,
              struct cstr_view  text)
{
	return reasoning ? cli_console_reasoning(ui->console, text) : cli_console_write(ui->console, text);
}

static int
cli_ui_newline (struct cli_ui *ui)
{
	if (!ui->trailing_newline) {
		int e = cli_ui_write(ui, ui->reasoning, CSTR_VIEW("\n"));
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
			e = cli_ui_write(ui, true, thinking ? CSTR_VIEW("[Start thinking]\n\n")
			                                    : CSTR_VIEW("[End thinking]\n\n"));
		if (!e && !thinking)
			e = cli_console_set_display(ui->console, CLI_STYLE_RESET);
		if (e)
			return e;
		ui->reasoning = thinking;
	}
	ui->trailing_newline = text.data[text.len - 1] == '\n';
	return cli_ui_write(ui, thinking, text);
}

static int
cli_ui_write_action (struct cli_ui    *ui,
                     struct cstr_view  actor,
                     struct cstr_view  text,
                     enum cli_style    style)
{
	dstr_auto line = dstr("* ");
	if (!dstr_cat_n(&line, actor.data, actor.len) || !dstr_cat(&line, " ") ||
	    !dstr_cat_n(&line, text.data, text.len) || !dstr_cat(&line, "\n"))
		return ENOMEM;
	struct cstr_view content = cstr_view_dstr(&line);
	size_t split = style == CLI_STYLE_TOOL_STATUS ? actor.len + 2 : content.len;
	int e = cli_console_set_display(ui->console, style == CLI_STYLE_TOOL_STATUS ? CLI_STYLE_TOOL_ACTOR : style);
	if (!e)
		e = cli_console_write(ui->console, cstr_view_slice(content, 0, split));
	if (!e && split < content.len) {
		e = cli_console_set_display(ui->console, style);
		if (!e)
			e = cli_console_write(ui->console, cstr_view_slice(content, split, SIZE_MAX));
	}
	int reset = cli_console_set_display(ui->console, CLI_STYLE_RESET);
	return e ? e : reset;
}

int
cli_ui_finish (struct cli_ui *ui)
{
	int e = cli_console_set_display(ui->console, ui->reasoning ? CLI_STYLE_REASONING : CLI_STYLE_RESET);
	if (!e)
		e = cli_ui_newline(ui);
	if (!e && ui->reasoning)
		e = cli_ui_write(ui, true, CSTR_VIEW("[End thinking]\n"));
	ui->reasoning = false;
	int reset = cli_console_set_display(ui->console, CLI_STYLE_RESET);
	if (!e && !reset && ui->pending)
		e = cli_ui_write_action(ui, cstr_view_dstr(&ui->actor), cstr_view_dstr(&ui->status), CLI_STYLE_PROGRESS);
	dstr_clear(&ui->actor);
	dstr_clear(&ui->status);
	ui->streaming = ui->shown = ui->pending = false;
	return e ? e : reset;
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
	return cli_ui_write_action(ui, actor, text, CLI_STYLE_PROGRESS);
}

int
cli_ui_action (struct cli_ui    *ui,
               struct cstr_view  actor,
               struct cstr_view  text)
{
	int e = cli_ui_finish(ui);
	return e ? e : cli_ui_write_action(ui, actor, text, CLI_STYLE_TOOL_STATUS);
}

static int
cli_ui_write_message (struct cli_console *console,
                      struct cstr_view    text,
                      enum cli_style      display)
{
	dstr_auto line = dstr(nullptr);
	if (!dstr_cat_n(&line, text.data, text.len) || !dstr_cat(&line, "\n"))
		return ENOMEM;
	int e = cli_console_set_display(console, display);
	if (!e)
		e = cli_console_write(console, cstr_view_dstr(&line));
	int reset = cli_console_set_display(console, CLI_STYLE_RESET);
	return e ? e : reset;
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
	dstr_auto line = dstr("Error: ");
	if (!dstr_cat_n(&line, title.data, title.len) || !dstr_cat(&line, "\n") ||
	    (detail.len && (!dstr_cat_n(&line, detail.data, detail.len) || !dstr_cat(&line, "\n"))))
		return ENOMEM;
	return cli_console_error(console, cstr_view_dstr(&line));
}
