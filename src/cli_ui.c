/*
 * Adapted from llama.cpp tools/cli/cli-ui.h, commit
 * b76b13379d3b7d67ab0bc424516bb6fd2522bd8e.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#include <errno.h>

#include "cli_console.h"
#include "cli_ui.h"

/* The mark that is written when the output enters a state, and the style
 * of the output in that state. Index 0 is the state out of the reasoning,
 * and index 1 the state in the reasoning.
 */
static struct cli_console_span const cli_ui_modes[] = {
	{CSTR_VIEW_INIT("[End thinking]\n\n"), CLI_STYLE_RESET},
	{CSTR_VIEW_INIT("[Start thinking]\n\n"), CLI_STYLE_REASONING},
};

static int
cli_ui_newline (struct cli_ui *ui)
{
	if (ui->flags & CLI_UI_TRAILING_NEWLINE)
		return 0;
	int e = cli_console_write(ui->console, ui->flags & CLI_UI_REASONING, CSTR_VIEW("\n"));
	if (!e)
		ui->flags |= CLI_UI_TRAILING_NEWLINE;
	return e;
}

int
cli_ui_push (struct cli_ui    *ui,
             bool              thinking,
             struct cstr_view  text)
{
	if (!text.len)
		return 0;
	bool reasoning = ui->flags & CLI_UI_REASONING;
	ui->flags |= CLI_UI_STREAMING;
	int e = cli_console_set_display(ui->console, cli_ui_modes[reasoning].style);
	if (!e && thinking != reasoning) {
		/* The mark is in the reasoning style, and so is the newline that
		 * ends the reasoning. */
		e = cli_ui_newline(ui);
		if (!e)
			e = cli_console_set_display(ui->console, CLI_STYLE_REASONING);
		if (!e)
			e = cli_console_write(ui->console, true, cli_ui_modes[thinking].text);
		if (!e)
			e = cli_console_set_display(ui->console, cli_ui_modes[thinking].style);
		if (!e)
			ui->flags ^= CLI_UI_REASONING;
	}
	if (e)
		return e;
	ui->flags = (ui->flags & ~CLI_UI_TRAILING_NEWLINE) |
	            (text.data[text.len - 1] == '\n' ? CLI_UI_TRAILING_NEWLINE : 0);
	return cli_console_write(ui->console, thinking, text);
}

static int
cli_ui_write_action (struct cli_ui    *ui,
                     struct cstr_view  actor,
                     struct cstr_view  text,
                     enum cli_style    style)
{
	/* A tool status shows its actor in a style of its own. */
	enum cli_style lead = style == CLI_STYLE_TOOL_STATUS ? CLI_STYLE_TOOL_ACTOR : style;
	struct cli_console_span const spans[] = {
		{CSTR_VIEW("* "), lead}, {actor, lead},
		{CSTR_VIEW(" "), style}, {text, style}, {CSTR_VIEW("\n"), style},
	};
	return cli_console_print(ui->console, spans, array_size(spans));
}

int
cli_ui_finish (struct cli_ui *ui)
{
	bool reasoning = ui->flags & CLI_UI_REASONING;
	int e = cli_console_set_display(ui->console, cli_ui_modes[reasoning].style);
	if (!e)
		e = cli_ui_newline(ui);
	if (!e && reasoning)
		e = cli_console_write(ui->console, true, CSTR_VIEW("[End thinking]\n"));
	int reset = cli_console_set_display(ui->console, CLI_STYLE_RESET);
	if (!e && !reset && (ui->flags & CLI_UI_PENDING))
		e = cli_ui_write_action(ui, cstr_view_dstr(&ui->actor), cstr_view_dstr(&ui->status), CLI_STYLE_PROGRESS);
	dstr_clear(&ui->actor);
	dstr_clear(&ui->status);
	ui->flags &= CLI_UI_TRAILING_NEWLINE;
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
	if (cstr_view_cat(actor, &ui->actor) || cstr_view_cat(text, &ui->status))
		return ENOMEM;
	/* After a shown line or delta, the last line waits for the end. */
	if (ui->flags & (CLI_UI_STREAMING | CLI_UI_SHOWN)) {
		ui->flags |= CLI_UI_PENDING;
		return 0;
	}
	ui->flags |= CLI_UI_SHOWN;
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
	struct cli_console_span const spans[] = {{text, display}, {CSTR_VIEW("\n"), display}};
	return cli_console_print(console, spans, array_size(spans));
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
	if (cstr_view_cat(title, &line) || !dstr_cat(&line, "\n") ||
	    (detail.len && (cstr_view_cat(detail, &line) || !dstr_cat(&line, "\n"))))
		return ENOMEM;
	return cli_console_error(console, cstr_view_dstr(&line));
}
