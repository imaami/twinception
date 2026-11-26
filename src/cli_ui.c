/*
 * Adapted from llama.cpp tools/cli/cli-ui.h, commit
 * b76b13379d3b7d67ab0bc424516bb6fd2522bd8e.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#include <errno.h>

#include "cli_console.h"
#include "cli_ui.h"
#include "util.h"

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
	if (flag_any(ui->flags, CLI_UI_TRAILING_NEWLINE))
		return 0;
	int e = cli_console_write(ui->console, flag_any(ui->flags, CLI_UI_REASONING_OPEN), CSTR_VIEW("\n"));
	if (!e)
		flag_set(&ui->flags, CLI_UI_TRAILING_NEWLINE);
	return e;
}

/* Write the line of the calls in a row, "* git" for one call and "* git x5"
 * for five, and forget the calls. The conversation is at the start of a
 * line: cli_ui_action() ended the line before it counted the first call.
 */
static int
cli_ui_calls (struct cli_ui *ui)
{
	if (!ui->calls)
		return 0;
	char count[32];
	struct cli_console_span const spans[] = {
		cli_console_span(CSTR_VIEW("* "), CLI_STYLE_TOOL),
		cli_console_span(cstr_view_dstr(&ui->tool), CLI_STYLE_TOOL),
		cli_console_span(ui->calls > 1 ? util_format_view(count, sizeof count, " x%zu\n", ui->calls)
		                               : CSTR_VIEW("\n"), CLI_STYLE_TOOL),
	};
	ui->calls = 0;
	return cli_console_print(ui->console, spans, array_size(spans));
}

int
cli_ui_push (struct cli_ui    *ui,
             bool              thinking,
             struct cstr_view  text)
{
	if (!text.len)
		return 0;
	/* Output in the conversation ends the calls in a row. The reasoning is
	 * output in the conversation too, unless the full-screen UI shows it in
	 * a pane of its own. */
	int e = !thinking || !cli_console_fullscreen(ui->console) ? cli_ui_calls(ui) : 0;
	bool reasoning = flag_any(ui->flags, CLI_UI_REASONING_OPEN);
	if (!e)
		e = cli_console_set_display(ui->console, cli_ui_modes[reasoning].style);
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
			flag_xor(&ui->flags, CLI_UI_REASONING_OPEN);
	}
	if (e)
		return e;
	flag_put(&ui->flags, CLI_UI_TRAILING_NEWLINE,
	         text.data[text.len - 1] == '\n' ? CLI_UI_TRAILING_NEWLINE : 0);
	return cli_console_write(ui->console, thinking, text);
}

/* End the line of the output, and the reasoning if its start mark is
 * written, so that the next output starts a line out of the reasoning.
 */
static int
cli_ui_close (struct cli_ui *ui)
{
	bool reasoning = flag_any(ui->flags, CLI_UI_REASONING_OPEN);
	int e = cli_console_set_display(ui->console, cli_ui_modes[reasoning].style);
	if (!e)
		e = cli_ui_newline(ui);
	if (!e && reasoning)
		e = cli_console_write(ui->console, true, CSTR_VIEW("[End thinking]\n"));
	flag_clr(&ui->flags, CLI_UI_REASONING_OPEN);
	int reset = cli_console_set_display(ui->console, CLI_STYLE_RESET);
	return e ? e : reset;
}

int
cli_ui_finish (struct cli_ui *ui)
{
	int e = cli_ui_close(ui);
	if (!e)
		e = cli_ui_calls(ui);
	ui->calls = 0;
	return e;
}

void
cli_ui_fini (struct cli_ui *ui)
{
	(void)cli_ui_finish(ui);
	dstr_cleanup(&ui->tool);
}

int
cli_ui_action (struct cli_ui    *ui,
               struct cstr_view  tool)
{
	int e = cli_ui_close(ui);
	if (e)
		return e;
	if (ui->calls && cstr_view_equal(tool, cstr_view_dstr(&ui->tool))) {
		++ui->calls;
		return 0;
	}
	e = cli_ui_calls(ui);
	if (e)
		return e;
	if (cstr_view_store(tool, &ui->tool))
		return ENOMEM;
	ui->calls = 1;
	return 0;
}

static int
cli_ui_write_message (struct cli_console *console,
                      struct cstr_view    text,
                      enum cli_style      display)
{
	struct cli_console_span const spans[] = {
		cli_console_span(text, display),
		cli_console_span(CSTR_VIEW("\n"), display),
	};
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
