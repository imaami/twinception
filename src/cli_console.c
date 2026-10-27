/*
 * Display helpers adapted from llama.cpp common/console.cpp, commit
 * b76b13379d3b7d67ab0bc424516bb6fd2522bd8e.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "buf.h"
#include "cli_console.h"
#include "cli_io.h"
#include "cli_loop.h"
#include "cli_render.h"
#include "cli_scrollback.h"
#include "compat.h"
#include "dstr.h"
#include "jb.h"
#include "list.h"
#include "loop.h"

enum {
	CLI_CONSOLE_INPUT_LIMIT   = 1024 * 1024 * 16,
	CLI_CONSOLE_INPUT_COUNT   = 32,
	CLI_CONSOLE_HISTORY_LIMIT = 1024 * 1024,
	CLI_CONSOLE_HISTORY_COUNT =  256,
	CLI_CONSOLE_OUTPUT_LIMIT  = 32 * 1024 * 1024,
	CLI_CONSOLE_TAB_INTERVAL  = 400 * 1000 * 1000,
	CLI_CONSOLE_COMPLETIONS   = 256,
	CLI_CONSOLE_ROWS          = 256,
	CLI_CONSOLE_COLUMNS       = 512,
};

enum {
	CLI_CONSOLE_TAB_PENDING   = 1u << 0,
	CLI_CONSOLE_TAB_ACTIVE    = 1u << 1,
	CLI_CONSOLE_TAB_LISTING   = 1u << 2,
	CLI_CONSOLE_TAB_COMPLETED = 1u << 3,
};

struct cli_console_output {
	uint32_t       value;
	uint32_t       minimum;
	unsigned       used;
	unsigned       expected;
	unsigned char  pending[4];
};

struct cli_console_io {
	struct cli_io_job job;
	ssize_t          count;
	int              fd;
	unsigned char    bytes[4096];
};

struct cli_console_completion {
	uint32_t               begin;
	uint32_t               length;
	enum autocomplete_kind kind;
};

struct cli_console_line {
	struct list hook;
	char       *text;
	size_t      length;
};

enum {
	CLI_CONSOLE_INPUT_MACRO  = 1u << 0,
	CLI_CONSOLE_INPUT_NOTICE = 1u << 1,
	CLI_CONSOLE_INPUT_YIELD  = 1u << 2,
};

enum cli_console_pane_id : uint32_t {
	CLI_CONSOLE_CONVERSATION,
	CLI_CONSOLE_REASONING,
	CLI_CONSOLE_PANES,
};

struct cli_console_pane {
	struct cli_scrollback     scrollback;
	struct cli_console_output output;
	struct dstr              rows[CLI_CONSOLE_ROWS];
};

enum {
	CLI_CONSOLE_PAINT_FRAME  = 1u << CLI_CONSOLE_PANES,
	CLI_CONSOLE_PAINT_STATUS = CLI_CONSOLE_PAINT_FRAME << 1,
	CLI_CONSOLE_PAINT_INPUT  = CLI_CONSOLE_PAINT_STATUS << 1,
	CLI_CONSOLE_PAINT_RESET  = CLI_CONSOLE_PAINT_INPUT << 1,
	CLI_CONSOLE_PAINT_ALL    = (CLI_CONSOLE_PAINT_RESET << 1) - 1,
};

struct cli_console {
	cli_console_complete_fn         *complete;
	cli_console_immediate_fn        *immediate;
	void                            *immediate_context;
	void                            *complete_context;
	struct cli_loop                 *loop;
	struct cli_io                   *io;
	FILE                            *terminal;
	locale_t                         locale;
	locale_t                         previous_locale;
	struct cli_loop_ref              input_ref;
	struct cli_loop_step             input_step;
	struct cli_loop_ref              output_ref;
	struct cli_console_io            input_job;
	struct cli_console_io            output_job;
	struct cli_console_pane          panes[CLI_CONSOLE_PANES];
	struct buf                       output_queue;
	struct dstr                      draft;
	struct dstr                      prompt;
	struct dstr                      completion_labels;
	struct dstr                      decorations[4];
	struct dstr                      clean;  /**< sanitized output */
	struct dstr                      screen; /**< a frame of the full-screen UI */
	struct list                      lines;
	char                            *restored_line;
	char                            *immediate_line;
	struct autocomplete_match const *candidates;
	struct cli_style_entry const    *palette;
	uint64_t                         completion_time;
	size_t                           candidate_count;
	size_t                           completion_count;
	size_t                           draft_start;
	size_t                           line_count;
	size_t                           line_bytes;
	size_t                           immediate_length;
	size_t                           input_offset;
	size_t                           input_size;
	size_t                           history_first;
	size_t                           history_count;
	size_t                           history_bytes;
	size_t                           history_sizes[CLI_CONSOLE_HISTORY_COUNT];
	struct dstr                      history_last;
	struct termios                   initial_state;
	struct termios                   resume_state;
	struct cli_console_completion    completions[CLI_CONSOLE_COMPLETIONS];
	enum cli_style                   display;
	enum cli_console_pane_id         focused;
	int                              input_error;
	int                              pending_input;
	int                              restored_point;
	int                              input_flags;
	int                              output_flags;
	int                              draft_point;
	int                              output_error;
	unsigned                         rows;
	unsigned                         columns;
	unsigned                         frame;
	unsigned                         cursor_column;
	uint32_t                         dirty;
	uint32_t                         input_state;
	bool                             fullscreen;
	bool                             suspended;
	bool                             terminal_saved;
	bool                             resize_pending;
	bool                             interrupt_pending;
	bool                             terminated;
	bool                             busy;
	bool                             input_regular;
	bool                             output_regular;
	bool                             input_working;
	bool                             output_working;
	bool                             simple;
	bool                             input_eof;
	bool                             multiline;
	bool                             callback_active;
	bool                             eof_pending;
	uint8_t                          completion_flags;
	unsigned char                    input_buffer[4096];
};

/* Only Readline's callbacks use this bridge. All owned state lives in the
 * console, and the pointer is cleared before its storage is released. */
static struct cli_console *cli_console_active;

static int cli_console_input_arm (struct cli_console *console);
static int cli_console_output_drain (struct cli_console *console);
static int cli_console_render (struct cli_console *console);
static int cli_console_output_write (struct cli_console_output *state, struct dstr *dest, struct cstr_view text);

static unsigned
cli_console_pane_width (struct cli_console const *console,
                        enum cli_console_pane_id  pane)
{
	if (console->columns < 5)
		return 1;
	unsigned available = console->columns - 3;
	unsigned left = available * 3 / 5;
	return pane == CLI_CONSOLE_CONVERSATION ? left : available - left;
}

static int
cli_console_completion_cache (struct cli_console *console)
{
	console->completion_count = 0;
	(void)dstr_clear(&console->completion_labels);
	for (size_t i = 0; i < console->candidate_count; ++i) {
		struct autocomplete_match const *candidate = &console->candidates[i];
		size_t end = candidate->cursor;
		if (end > candidate->label_begin && candidate->line.data[end - 1] == ' ')
			--end;
		struct cstr_view label = cstr_view_slice(candidate->line, candidate->label_begin, end - candidate->label_begin);
		size_t begin = dstr_len(&console->completion_labels);
		if (label.len >= CLI_CONSOLE_INPUT_LIMIT - begin)
			return E2BIG;
		struct cli_console_output state = {};
		int e = cli_console_output_write(&state, &console->completion_labels, label);
		if (e)
			return e;
		console->completions[i] = (struct cli_console_completion){
			.begin = (uint32_t)begin,
			.length = (uint32_t)(dstr_len(&console->completion_labels) - begin),
			.kind = candidate->kind,
		};
	}
	console->completion_count = console->candidate_count;
	return 0;
}

static void
cli_console_remove_callback (struct cli_console *console)
{
	if (console->callback_active) {
		rl_callback_handler_remove();
		console->callback_active = false;
	}
}

static void
cli_console_input_done (struct cli_io_job *job)
{
	struct cli_console *console = container_of(job, struct cli_console, input_job.job);
	console->input_working = false;
	console->input_error = console->terminated ? EINTR : job->error;
	if (!job->error) {
		ssize_t count = console->input_job.count;
		console->input_eof = !count;
		console->input_offset = 0;
		console->input_size = (size_t)count;
		memcpy(console->input_buffer, console->input_job.bytes, (size_t)count);
	}
}

static int
cli_console_input_work (struct cli_io_job *job)
{
	struct cli_console_io *input = container_of(job, struct cli_console_io, job);
	do {
		input->count = read(input->fd, input->bytes, sizeof input->bytes);
	} while (input->count < 0 && errno == EINTR);
	return input->count < 0 ? errno : 0;
}

static void
cli_console_input_ready (struct cli_loop     *loop,
                         struct cli_loop_ref *ref,
                         uint32_t             events)
{
	(void)events;
	struct cli_console *console = container_of(ref, struct cli_console, input_ref);
	ssize_t count = read(ref->fd, console->input_buffer, sizeof console->input_buffer);
	if (count < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	cli_loop_unwatch(loop, ref);
	if (count < 0) {
		console->input_error = errno;
		return;
	}
	console->input_eof = !count;
	console->input_offset = 0;
	console->input_size = (size_t)count;
}

static void
cli_console_input_continue (struct cli_loop      *loop,
                            struct cli_loop_step *step)
{
	(void)loop;
	(void)step;
}

static int
cli_console_input_arm (struct cli_console *console)
{
	if (console->input_offset < console->input_size || console->input_eof ||
	    console->input_error || console->suspended || console->input_working ||
	    console->input_ref.fd >= 0)
		return 0;
	if (!console->input_regular) {
		int e = cli_loop_watch(console->loop, &console->input_ref, STDIN_FILENO,
		                       EPOLLIN, cli_console_input_ready);
		if (e != EPERM)
			return e;
		console->input_regular = true;
	}
	if (console->io) {
		console->input_job.job.work = cli_console_input_work;
		console->input_job.job.done = cli_console_input_done;
		console->input_job.fd = STDIN_FILENO;
		int e = cli_io_submit(console->io, &console->input_job.job);
		if (!e)
			console->input_working = true;
		return e;
	}
	/* Console-only tests may omit a worker for a regular file. */
	console->input_job.fd = STDIN_FILENO;
	console->input_job.job.error = cli_console_input_work(&console->input_job.job);
	cli_console_input_done(&console->input_job.job);
	return console->input_error;
}

static int
cli_console_wait (struct cli_console *console,
                  uint64_t            deadline)
{
	while (!console->input_eof && !console->input_error &&
	       console->input_offset == console->input_size) {
		int e = cli_console_input_arm(console);
		if (e)
			return e;
		if (console->input_offset < console->input_size || console->input_eof)
			break;
		if (deadline != UINT64_MAX && loop_clock() >= deadline)
			return 0;
		e = cli_loop_turn(console->loop, deadline);
		if (e)
			return e;
		if (!console->frame) {
			e = cli_console_service(console);
			if (e)
				return e;
		}
	}
	return console->input_error;
}

static int
cli_console_byte (struct cli_console *console)
{
	if (console->input_error)
		return EOF;
	if (console->input_offset == console->input_size) {
		console->input_error = cli_console_wait(console, UINT64_MAX);
		if (console->input_error || console->input_eof)
			return EOF;
	}
	return console->input_buffer[console->input_offset++];
}

static int
cli_console_getc (FILE *stream)
{
	(void)stream;
	struct cli_console *console = cli_console_active;
	int byte = cli_console_byte(console);
	if (byte == EOF && RL_ISSTATE(RL_STATE_MOREINPUT) && !console->input_error)
		console->input_error = EIO;
	/* Readline 8.2 cannot consume EOF midway through a bracketed paste. */
	if (console->input_error)
		rl_abort(0, 0);
	return byte;
}

static int
cli_console_available (void)
{
	struct cli_console *console = cli_console_active;
	int timeout = rl_set_keyboard_input_timeout(-1);
	uint64_t deadline = timeout < 0 ? UINT64_MAX :
	                    loop_clock() + (uint64_t)timeout * 1000;
	console->input_error = cli_console_wait(console, deadline);
	return console->input_error || console->input_eof ||
	       console->input_offset < console->input_size;
}

static bool
cli_console_is_immediate (struct cli_console *console,
                          struct cstr_view    line)
{
	return console->fullscreen && console->immediate &&
	       console->immediate(console->immediate_context, line);
}

static void
cli_console_resume_keys (struct cli_console *console)
{
	if (console->immediate_line || rl_pending_input)
		return;
	if (console->input_state & CLI_CONSOLE_INPUT_MACRO) {
		if (rl_executing_macro)
			RL_SETSTATE(RL_STATE_MACROINPUT);
		console->input_state &= ~CLI_CONSOLE_INPUT_MACRO;
	}
	if (console->pending_input) {
		rl_execute_next(console->pending_input);
		console->pending_input = 0;
	}
}

static void
cli_console_pause_keys (struct cli_console *console)
{
	if (!console->pending_input)
		console->pending_input = rl_pending_input;
	if (rl_executing_macro)
		console->input_state |= CLI_CONSOLE_INPUT_MACRO;
	rl_clear_pending_input();
	RL_UNSETSTATE(RL_STATE_MACROINPUT);
}

static bool
cli_console_input_full (struct cli_console *console,
                        size_t              length,
                        bool                immediate)
{
	return immediate ? console->immediate_line != nullptr :
	       console->line_count >= CLI_CONSOLE_INPUT_COUNT ||
	       length > CLI_CONSOLE_INPUT_LIMIT - console->line_bytes;
}

static void
cli_console_line_destroy (struct cli_console_line **ptr)
{
	struct cli_console_line *line = *ptr;
	if (!line)
		return;
	free(line->text);
	line->text = nullptr;
	free(line);
	*ptr = nullptr;
}

/** Takes ownership of text, including on allocation failure. */
static struct cli_console_line *
cli_console_line_create (char   *text,
                         size_t  length,
                         int    *e)
{
	struct cli_console_line *line = malloc(sizeof *line);
	*e = line ? 0 : ENOMEM;
	if (line)
		*line = (struct cli_console_line){.text = text, .length = length};
	else {
		free(text);
		text = nullptr;
	}
	return line;
}

static void
cli_console_drop_line (struct cli_console *console)
{
	struct cli_console_line *line = container_of(console->lines.next, struct cli_console_line, hook);
	list_del(&line->hook);
	--console->line_count;
	console->line_bytes -= line->length;
	cli_console_line_destroy(&line);
}

static void
cli_console_accept (char *line)
{
	struct cli_console *console = cli_console_active;
	if (!line) {
		console->eof_pending = true;
		cli_console_remove_callback(console);
		return;
	}
	size_t length = strlen(line);
	if (!length && console->busy) {
		free(line);
		line = nullptr;
		return;
	}
	bool immediate = cli_console_is_immediate(console, (struct cstr_view){line, length});
	if (cli_console_input_full(console, length, immediate)) {
		/* A custom inputrc binding may bypass our submit command. Restore
		 * its rejected submission after Readline prepares the next line. */
		console->restored_line = line;
		console->restored_point = rl_point;
		console->input_state |= CLI_CONSOLE_INPUT_NOTICE;
		cli_console_pause_keys(console);
		console->dirty |= CLI_CONSOLE_PAINT_INPUT;
		return;
	}
	if (immediate) {
		console->immediate_line = line;
		console->immediate_length = length;
		cli_loop_ready(console->loop, &console->input_step);
	} else {
		int e;
		struct cli_console_line *input = cli_console_line_create(line, length, &e);
		if (!input) {
			console->input_error = e;
			return;
		}
		list_append(&console->lines, &input->hook);
		++console->line_count;
		console->line_bytes += length;
	}
	cli_console_pause_keys(console);
	if (length >= CLI_CONSOLE_INPUT_LIMIT)
		console->input_error = EFBIG;
	else if (jb_utf8(line, length) != length)
		console->input_error = EILSEQ;
	else if (length <= CLI_CONSOLE_HISTORY_LIMIT)
		/* Record before Readline positions the new draft's history cursor. */
		console->input_error = cli_console_remember(console, (struct cstr_view){line, length});
	/* While a request runs, Readline prepares a fresh draft itself. Keep
	 * its callback installed so editing never returns to canonical input. */
	if (!immediate && (!console->fullscreen || !console->busy))
		cli_console_remove_callback(console);
	console->dirty |= CLI_CONSOLE_PAINT_INPUT | CLI_CONSOLE_PAINT_STATUS;
}

static int
cli_console_submit (int count,
                    int key)
{
	struct cli_console *console = cli_console_active;
	bool immediate = cli_console_is_immediate(console, (struct cstr_view){rl_line_buffer, (size_t)rl_end});
	bool more = !immediate && console->multiline;
	char last = !immediate && rl_end ? rl_line_buffer[rl_end - 1] : 0;
	if (last == '/' || last == '\\')
		more = last == '\\' && !more;
	if (!more && (rl_end || !console->busy) &&
	    cli_console_input_full(console, (size_t)rl_end, immediate)) {
		console->input_state &= ~CLI_CONSOLE_INPUT_MACRO;
		console->input_state |= CLI_CONSOLE_INPUT_NOTICE;
		console->pending_input = 0;
		/* Clearing MACROINPUT alone would replay the remaining macro
		 * bytes when the user next presses a physical key. */
		return rl_abort(count, key);
	}
	if (last == '/' || last == '\\') {
		rl_delete_text(rl_end - 1, rl_end);
		if (rl_point > rl_end)
			rl_point = rl_end;
	}
	if (!more)
		return rl_newline(count, key);
	if (rl_end >= CLI_CONSOLE_INPUT_LIMIT - 1) {
		console->input_error = EFBIG;
		return rl_newline(count, key);
	}
	rl_point = rl_end;
	rl_insert_text("\n");
	return 0;
}

static char *
cli_console_candidate (char const *text,
                       int         state)
{
	(void)text;
	struct cli_console *console = cli_console_active;
	if (state < 0 || (size_t)state >= console->candidate_count)
		return nullptr;
	struct autocomplete_match const *candidate = &console->candidates[state];
	char *copy = malloc(candidate->cursor + 1);
	if (!copy) {
		console->input_error = ENOMEM;
		return nullptr;
	}
	memcpy(copy, candidate->line.data, candidate->cursor);
	copy[candidate->cursor] = '\0';
	return copy;
}

static char **
cli_console_complete (char const *text,
                      int         start,
                      int         end)
{
	struct cli_console *console = cli_console_active;
	rl_attempted_completion_over = 1;
	rl_completion_suppress_append = 1;
	rl_completion_suppress_quote = 1;
	rl_filename_completion_desired = 0;
	console->completion_count = 0;
	if (!console->complete || start || end < 0 || rl_end < 0)
		return nullptr;
	struct cstr_view line = {rl_line_buffer, (size_t)rl_end};
	console->input_error = console->complete(console->complete_context, line, (size_t)end,
	                                         &console->candidates, &console->candidate_count);
	if (console->input_error || !console->candidate_count)
		return nullptr;
	if (!console->candidates || console->candidate_count > CLI_CONSOLE_COMPLETIONS) {
		console->input_error = EINVAL;
		return nullptr;
	}
	struct cstr_view suffix = {};
	for (size_t i = 0; i < console->candidate_count; ++i) {
		struct autocomplete_match const *candidate = &console->candidates[i];
		struct cstr_view replacement = candidate->line;
		if (!replacement.data || !candidate->cursor || candidate->cursor > replacement.len ||
		    candidate->label_begin >= candidate->cursor || candidate->kind >= AUTOCOMPLETE_KIND_COUNT ||
		    replacement.len >= CLI_CONSOLE_INPUT_LIMIT || memchr(replacement.data, 0, replacement.len) ||
		    (candidate->cursor < replacement.len &&
		     ((unsigned char)replacement.data[candidate->cursor] & 0xc0) == 0x80))
			return nullptr;
		struct cstr_view tail = cstr_view_slice(replacement, candidate->cursor, SIZE_MAX);
		if (!i) {
			suffix = tail;
			if (suffix.len <= line.len &&
			    cstr_view_equal(cstr_view_slice(line, line.len - suffix.len, SIZE_MAX), suffix))
				continue;
		} else if (cstr_view_equal(tail, suffix)) {
			continue;
		}
		return nullptr;
	}
	console->input_error = cli_console_completion_cache(console);
	if (console->input_error)
		return nullptr;
	char **matches = rl_completion_matches(text, cli_console_candidate);
	if (matches && rl_completion_type == '?' && !matches[1]) {
		/* A possible-completions request must never insert a unique match. */
		char **listing = calloc(3, sizeof *listing);
		char *empty = calloc(1, 1);
		if (listing && empty) {
			listing[0] = empty;
			listing[1] = matches[0];
		} else {
			free(listing);
			listing = nullptr;
			free(empty);
			empty = nullptr;
			free(matches[0]);
			matches[0] = nullptr;
			console->input_error = ENOMEM;
		}
		free(matches);
		matches = listing;
	}

	if (matches && rl_completion_type != '?')
		rl_point = (int)(line.len - suffix.len);
	if (matches && console->candidate_count == 1 &&
	    (console->completion_flags & (CLI_CONSOLE_TAB_ACTIVE | CLI_CONSOLE_TAB_PENDING)) ==
	    (CLI_CONSOLE_TAB_ACTIVE | CLI_CONSOLE_TAB_PENDING)) {
		console->completion_flags &= ~CLI_CONSOLE_TAB_PENDING;
		console->completion_flags |= CLI_CONSOLE_TAB_COMPLETED;
	}
	console->candidates = nullptr;
	console->candidate_count = 0;
	return matches;
}

static size_t
cli_console_text_width (struct cstr_view text)
{
	size_t width = 0;
	while (text.len) {
		int cells;
		size_t length = cli_render_character(text, &cells);
		width += cells < 0 ? 1 : (size_t)cells;
		text = cstr_view_slice(text, length, SIZE_MAX);
	}
	return width;
}

static void
cli_console_matches (char **matches,
                     int    count,
                     int    width)
{
	(void)matches;
	(void)count;
	(void)width;
	struct cli_console *console = cli_console_active;
	if ((console->completion_flags & (CLI_CONSOLE_TAB_ACTIVE | CLI_CONSOLE_TAB_LISTING)) ==
	    CLI_CONSOLE_TAB_ACTIVE)
		return;
	static enum cli_style const styles[AUTOCOMPLETE_KIND_COUNT] = {
		[AUTOCOMPLETE_COMMAND] = CLI_STYLE_COMPLETION_COMMAND,
		[AUTOCOMPLETE_TOOL] = CLI_STYLE_COMPLETION_TOOL,
		[AUTOCOMPLETE_PARAMETER] = CLI_STYLE_COMPLETION_PARAMETER,
		[AUTOCOMPLETE_VALUE] = CLI_STYLE_COMPLETION_VALUE,
		[AUTOCOMPLETE_NAME] = CLI_STYLE_COMPLETION_NAME,
		[AUTOCOMPLETE_FILE] = CLI_STYLE_COMPLETION_FILE,
	};
	struct cli_scrollback *scrollback = &console->panes[CLI_CONSOLE_CONVERSATION].scrollback;
	size_t columns = cli_console_pane_width(console, CLI_CONSOLE_CONVERSATION);
	struct cstr_view labels = cstr_view_dstr(&console->completion_labels);
	int e = cli_scrollback_append(scrollback, CSTR_VIEW("\n"));
	size_t column = 0;
	for (size_t i = 0; i < console->completion_count && !e; ++i) {
		struct cli_console_completion const *completion = &console->completions[i];
		struct cstr_view text = cstr_view_slice(labels, completion->begin, completion->length);
		size_t cells = cli_console_text_width(text);
		if (column && (column >= columns || cells + 1 > columns - column)) {
			e = cli_scrollback_append(scrollback, CSTR_VIEW("\n"));
			column = 0;
		} else if (column)
			e = cli_scrollback_append(scrollback, CSTR_VIEW(" "));
		if (!e)
			e = cli_scrollback_append_style(scrollback, text, styles[completion->kind]);
		column += cells + (column ? 1 : 0);
	}
	if (!e)
		e = cli_scrollback_append(scrollback, CSTR_VIEW("\n"));
	console->input_error = e;
	cli_scrollback_bottom(scrollback);
	console->dirty |= (1u << CLI_CONSOLE_CONVERSATION) | CLI_CONSOLE_PAINT_FRAME;
}

static int
cli_console_tab (int count,
                 int key)
{
	struct cli_console *console = cli_console_active;
	uint64_t now = loop_clock();
	bool repeated = rl_last_func == cli_console_tab && now - console->completion_time <= CLI_CONSOLE_TAB_INTERVAL;
	console->completion_time = now;
	if (repeated && (console->completion_flags & CLI_CONSOLE_TAB_COMPLETED)) {
		/* The first Tab already finished this word. Do not start completing
		 * its argument with the second keypress of the same pair. */
		console->completion_flags = 0;
		return 0;
	}
	bool listing = repeated && (console->completion_flags & CLI_CONSOLE_TAB_PENDING);
	console->completion_flags = CLI_CONSOLE_TAB_ACTIVE |
	                            (listing ? CLI_CONSOLE_TAB_LISTING : CLI_CONSOLE_TAB_PENDING);
	/* Each pair starts a new completion attempt. Readline otherwise treats
	 * every subsequent Tab as another request to print the same matches. */
	rl_last_func = nullptr;
	/* '!' completes unique matches and displays ambiguous ones. Explicit
	 * possible-completions bindings retain Readline's listing-only '?'. */
	int e = listing ? rl_complete_internal('!') : rl_complete(count, key);
	console->completion_flags &= CLI_CONSOLE_TAB_PENDING | CLI_CONSOLE_TAB_COMPLETED;
	return e;
}

static void
cli_console_prepare (int meta)
{
	rl_prep_terminal(meta);
	struct cli_console *console = cli_console_active;
	struct termios state;
	if (tcgetattr(STDIN_FILENO, &state) < 0) {
		console->input_error = errno;
		return;
	}
	state.c_lflag |= NOFLSH;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &state) < 0)
		console->input_error = errno;
}

static void
cli_console_output_done (struct cli_io_job *job)
{
	struct cli_console *console = container_of(job, struct cli_console, output_job.job);
	console->output_working = false;
	console->output_error = job->error;
	if (!job->error)
		buf_consume(&console->output_queue, (size_t)console->output_job.count);
}

static int
cli_console_output_work (struct cli_io_job *job)
{
	struct cli_console_io *output = container_of(job, struct cli_console_io, job);
	ssize_t count;
	do {
		count = write(output->fd, output->bytes, (size_t)output->count);
	} while (count < 0 && errno == EINTR);
	output->count = count;
	return count < 0 ? errno : count ? 0 : EIO;
}

static void
cli_console_output_ready (struct cli_loop     *loop,
                          struct cli_loop_ref *ref,
                          uint32_t             events)
{
	(void)loop;
	(void)events;
	struct cli_console *console = container_of(ref, struct cli_console, output_ref);
	console->output_error = cli_console_output_drain(console);
}

static int
cli_console_output_drain (struct cli_console *console)
{
	if (console->output_error || console->output_working || console->suspended)
		return console->output_error;
	struct span bytes = buf_peek(&console->output_queue);
	if (!bytes.len) {
		cli_loop_unwatch(console->loop, &console->output_ref);
		return 0;
	}
	if (console->output_regular && console->io) {
		size_t length = bytes.len < sizeof console->output_job.bytes
		                ? bytes.len : sizeof console->output_job.bytes;
		memcpy(console->output_job.bytes, bytes.ptr, length);
		console->output_job.count = (ssize_t)length;
		console->output_job.fd = STDOUT_FILENO;
		console->output_job.job.work = cli_console_output_work;
		console->output_job.job.done = cli_console_output_done;
		int e = cli_io_submit(console->io, &console->output_job.job);
		if (!e)
			console->output_working = true;
		return e;
	}
	/* A turn writes at most one bounded block. EPOLLOUT drives the rest. */
	size_t length = bytes.len < 65536 ? bytes.len : 65536;
	ssize_t count = write(STDOUT_FILENO, bytes.ptr, length);
	if (count < 0 && errno != EAGAIN && errno != EINTR)
		return errno;
	if (count > 0)
		buf_consume(&console->output_queue, (size_t)count);
	if (console->output_queue.head == console->output_queue.tail) {
		cli_loop_unwatch(console->loop, &console->output_ref);
		return 0;
	}
	if (!console->output_regular && console->output_ref.fd < 0)
		return cli_loop_watch(console->loop, &console->output_ref, STDOUT_FILENO,
		                      EPOLLOUT, cli_console_output_ready);
	return 0;
}

static int
cli_console_queue (struct cli_console *console,
                   struct cstr_view    text)
{
	struct span room;
	int e = buf_reserve(&console->output_queue, text.len, CLI_CONSOLE_OUTPUT_LIMIT, &room);
	if (e)
		return e;
	if (text.len)
		memcpy(room.ptr, text.data, text.len);
	buf_commit(&console->output_queue, text.len);
	return cli_console_output_drain(console);
}

static ssize_t
cli_console_terminal_write (void       *cookie,
                            char const *data,
                            size_t      length)
{
	struct cli_console *console = cookie;
	int e = cli_console_queue(console, (struct cstr_view){data, length});
	if (e) {
		console->output_error = e;
		return -1;
	}
	/* Bell and bracketed-paste mode changes leave the display intact. */
	if ((length == 1 && *data == '\a') || (length == 8 && !memcmp(data, "\033[?2004", 7)))
		return (ssize_t)length;
	console->cursor_column = 0;
	console->dirty |= length == 1 && *data == '\r' ? CLI_CONSOLE_PAINT_INPUT : CLI_CONSOLE_PAINT_RESET;
	return (ssize_t)length;
}

static void
cli_console_redisplay (void)
{
	struct cli_console *console = cli_console_active;
	struct cstr_view text = {rl_line_buffer, (size_t)rl_end};
	if (!cstr_view_equal(text, cstr_view_dstr(&console->draft)) &&
	    cstr_view_store(text, &console->draft))
		console->input_error = ENOMEM;
	console->draft_point = rl_point;
	console->dirty |= CLI_CONSOLE_PAINT_INPUT;
}

/* Scroll the focused pane a page back (-1) or forward (1), or to its
 * bottom (0). */
static int
cli_console_scroll (int direction)
{
	struct cli_console *console = cli_console_active;
	struct cli_scrollback *scrollback = &console->panes[console->focused].scrollback;
	if (direction)
		cli_scrollback_scroll(scrollback, console->rows > 4 ? console->rows - 4 : 1, direction < 0);
	else
		cli_scrollback_bottom(scrollback);
	console->dirty |= (1u << console->focused) | CLI_CONSOLE_PAINT_FRAME;
	return 0;
}

static int
cli_console_page_up (int count,
                     int key)
{
	(void)count;
	(void)key;
	return cli_console_scroll(-1);
}

static int
cli_console_page_down (int count,
                       int key)
{
	(void)count;
	(void)key;
	return cli_console_scroll(1);
}

static int
cli_console_bottom (int count,
                    int key)
{
	(void)count;
	(void)key;
	return cli_console_scroll(0);
}

static int
cli_console_focus (int count,
                   int key)
{
	(void)count;
	struct cli_console *console = cli_console_active;
	console->focused = key == 'D' ? CLI_CONSOLE_CONVERSATION : key == 'C' ? CLI_CONSOLE_REASONING
	                   : console->focused == CLI_CONSOLE_CONVERSATION ? CLI_CONSOLE_REASONING : CLI_CONSOLE_CONVERSATION;
	console->dirty |= CLI_CONSOLE_PAINT_FRAME;
	return 0;
}

static int
cli_console_dimensions (struct cli_console *console)
{
	struct winsize size = {};
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0)
		return errno;
	console->rows = size.ws_row ? size.ws_row : 24;
	console->columns = size.ws_col ? size.ws_col : 80;
	console->draft_start = 0;
	if (console->rows > CLI_CONSOLE_ROWS)
		console->rows = CLI_CONSOLE_ROWS;
	if (console->columns > CLI_CONSOLE_COLUMNS)
		console->columns = CLI_CONSOLE_COLUMNS;
	console->dirty = CLI_CONSOLE_PAINT_ALL;
	return 0;
}

static int
cli_console_editor_start (struct cli_console *console)
{
	if (console->simple || console->callback_active || console->suspended || console->terminated ||
	    console->input_eof || console->eof_pending)
		return 0;
	console->callback_active = true;
	++console->frame;
	rl_callback_handler_install(dstr_get(&console->prompt), cli_console_accept);
	if (rl_executing_macro)
		RL_SETSTATE(RL_STATE_MACROINPUT);
	if (console->pending_input) {
		rl_execute_next(console->pending_input);
		console->pending_input = 0;
	}
	int e = console->input_error;
	--console->frame;
	if (!e)
		e = cli_console_input_arm(console);
	cli_loop_ready(console->loop, &console->input_step);
	return e;
}

struct cli_console *
cli_console_create (bool             simple,
                    bool             color,
                    struct cli_loop *loop,
                    struct cli_io   *io,
                    int             *e)
{
	if (cli_console_active || !loop) {
		*e = cli_console_active ? EBUSY : EINVAL;
		return nullptr;
	}
	struct cli_console *console = calloc(1, sizeof *console);
	if (!console) {
		*e = ENOMEM;
		return nullptr;
	}
	console->input_step.fn = cli_console_input_continue;
	list_init(&console->lines);
	console->input_ref.fd = -1;
	console->output_ref.fd = -1;
	console->input_job.fd = -1;
	console->output_job.fd = -1;
	console->input_flags = -1;
	console->output_flags = -1;
	console->rows = 24;
	console->columns = 80;
	console->loop = loop;
	console->io = io;
	console->history_last = dstr(nullptr);
	console->draft = dstr(nullptr);
	console->prompt = dstr("\n> ");
	console->completion_labels = dstr(nullptr);
	console->clean = dstr(nullptr);
	console->screen = dstr(nullptr);
	for (size_t i = 0; i < 4; ++i)
		console->decorations[i] = dstr(nullptr);
	for (size_t i = 0; i < CLI_CONSOLE_PANES; ++i) {
		cli_scrollback_init(&console->panes[i].scrollback);
		for (size_t row = 0; row < CLI_CONSOLE_ROWS; ++row)
			console->panes[i].rows[row] = dstr(nullptr);
	}
	bool tty_stdout = isatty(STDOUT_FILENO);
	console->simple = simple || !tty_stdout || !isatty(STDIN_FILENO);
	console->palette = cli_style_palette(color && tty_stdout);
	char const *term = getenv("TERM");
	console->fullscreen = !console->simple && term && *term && strcmp(term, "dumb");
	struct stat input, output;
	*e = fstat(STDIN_FILENO, &input) < 0 || fstat(STDOUT_FILENO, &output) < 0 ? errno : 0;
	if (*e)
		goto fail;
	console->input_regular = S_ISREG(input.st_mode);
	console->output_regular = S_ISREG(output.st_mode);
	console->input_flags = fcntl(STDIN_FILENO, F_GETFL);
	console->output_flags = fcntl(STDOUT_FILENO, F_GETFL);
	if (console->input_flags < 0 || console->output_flags < 0 ||
	    fcntl(STDIN_FILENO, F_SETFL, console->input_flags | O_NONBLOCK) < 0 ||
	    fcntl(STDOUT_FILENO, F_SETFL, console->output_flags | O_NONBLOCK) < 0) {
		*e = errno;
		goto fail;
	}
	cli_console_active = console;
	if (!console->simple) {
		/* Readline edits UTF-8 only in a UTF-8 locale; its initialization
		 * resets the global locale, so keep the override in this thread. */
		console->locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
		if (!console->locale) {
			*e = errno;
			goto fail;
		}
		console->previous_locale = uselocale(console->locale);
		if (!console->previous_locale) {
			*e = errno;
			goto fail;
		}
		if (tcgetattr(STDIN_FILENO, &console->initial_state) < 0) {
			*e = errno;
			goto fail;
		}
		console->terminal_saved = true;
		console->terminal = fopencookie(console, "w", (cookie_io_functions_t){
			.write = cli_console_terminal_write,
		});
		if (!console->terminal) {
			*e = errno;
			goto fail;
		}
		setbuf(console->terminal, nullptr);
		rl_readline_name = "askai-cli";
		rl_catch_signals = 0;
		rl_catch_sigwinch = 0;
		rl_instream = stdin;
		rl_outstream = console->terminal;
		rl_initialize();
		rl_variable_bind("input-meta", "on");
		rl_variable_bind("output-meta", "on");
		rl_variable_bind("convert-meta", "off");
		console->fullscreen = console->fullscreen && rl_get_termcap("up") && rl_get_termcap("cl");
		rl_add_defun("askai-accept-line", cli_console_submit, -1);
		rl_add_defun("askai-scroll-up", cli_console_page_up, -1);
		rl_add_defun("askai-scroll-down", cli_console_page_down, -1);
		char const *names[] = {"emacs-standard", "vi-insertion", "vi-move"};
		for (size_t i = 0; i < array_size(names); ++i) {
			Keymap map = rl_get_keymap_by_name(names[i]);
			if (!map)
				continue;
			rl_bind_keyseq_in_map("\033[5~", rl_history_search_backward, map);
			rl_bind_keyseq_in_map("\033[6~", rl_history_search_forward, map);
			if (console->fullscreen) {
				rl_bind_keyseq_in_map("\033[5;5~", cli_console_page_up, map);
				rl_bind_keyseq_in_map("\033[6;5~", cli_console_page_down, map);
				rl_bind_keyseq_in_map("\033[5^", cli_console_page_up, map);
				rl_bind_keyseq_in_map("\033[6^", cli_console_page_down, map);
				rl_bind_keyseq_in_map("\033[1;3A", cli_console_page_up, map);
				rl_bind_keyseq_in_map("\033[1;3B", cli_console_page_down, map);
				rl_bind_keyseq_in_map("\033[1;5F", cli_console_bottom, map);
				rl_bind_keyseq_in_map("\033[4;5~", cli_console_bottom, map);
				rl_bind_keyseq_in_map("\033[8;5~", cli_console_bottom, map);
				rl_bind_keyseq_in_map("\033[8^", cli_console_bottom, map);
				rl_bind_keyseq_in_map("\033[17~", cli_console_focus, map);
				rl_bind_keyseq_in_map("\033[1;3D", cli_console_focus, map);
				rl_bind_keyseq_in_map("\033[1;3C", cli_console_focus, map);
				rl_bind_keyseq_in_map("\033\033[A", cli_console_page_up, map);
				rl_bind_keyseq_in_map("\033\033[B", cli_console_page_down, map);
				rl_bind_keyseq_in_map("\033\033[D", cli_console_focus, map);
				rl_bind_keyseq_in_map("\033\033[C", cli_console_focus, map);
				rl_bind_keyseq_in_map("\033\033OA", cli_console_page_up, map);
				rl_bind_keyseq_in_map("\033\033OB", cli_console_page_down, map);
				rl_bind_keyseq_in_map("\033\033OD", cli_console_focus, map);
				rl_bind_keyseq_in_map("\033\033OC", cli_console_focus, map);
			}
			rl_bind_keyseq_in_map("\033[1;5D", rl_backward_word, map);
			rl_bind_keyseq_in_map("\033[1;5C", rl_forward_word, map);
			rl_bind_key_in_map('\r', cli_console_submit, map);
			rl_bind_key_in_map('\n', cli_console_submit, map);
			rl_bind_key_in_map(0x1f, rl_undo_command, map);
			if (console->fullscreen)
				rl_bind_key_in_map('\t', cli_console_tab, map);
		}
		rl_variable_bind("enable-bracketed-paste", "on");
		char const *inputrc = getenv("INPUTRC");
		(void)rl_read_init_file(inputrc && *inputrc ? inputrc : "~/.inputrc");
		rl_prep_term_function = cli_console_prepare;
		rl_completion_display_matches_hook = console->fullscreen ? cli_console_matches : nullptr;
		rl_getc_function = cli_console_getc;
		rl_input_available_hook = cli_console_available;
		rl_attempted_completion_function = cli_console_complete;
		rl_completer_word_break_characters = "";
		rl_completer_quote_characters = "";
		if (console->fullscreen)
			rl_redisplay_function = cli_console_redisplay;
		using_history();
		stifle_history(CLI_CONSOLE_HISTORY_COUNT);
		if (console->fullscreen) {
			*e = cli_console_dimensions(console);
			if (!*e)
				*e = cli_console_queue(console, CSTR_VIEW("\033[?1049h\033[?7l"));
			if (!*e)
				*e = cli_console_flush(console);
			if (*e)
				goto fail;
		}
	}
	*e = 0;
	return console;
fail:
	cli_console_destroy(&console);
	return nullptr;
}

int
cli_console_suspend (struct cli_console *console)
{
	if (console->suspended)
		return 0;
	int e = 0;
	if (console->terminal_saved) {
		if (tcgetattr(STDIN_FILENO, &console->resume_state) < 0)
			e = errno;
		/* These short best-effort writes must never wait for a slow terminal. */
		char const restore[] = "\033[?2004l\033[?7h\033[0m\033[?1049l";
		if (console->fullscreen)
			(void)write(STDOUT_FILENO, restore, sizeof restore - 1);
		while (tcsetattr(STDIN_FILENO, TCSANOW, &console->initial_state) < 0) {
			if (errno != EINTR) {
				e = errno;
				break;
			}
		}
	}
	console->suspended = true;
	cli_loop_unwatch(console->loop, &console->input_ref);
	cli_loop_unwatch(console->loop, &console->output_ref);
	if (console->input_flags >= 0 && fcntl(STDIN_FILENO, F_SETFL, console->input_flags) < 0)
		e = errno;
	if (console->output_flags >= 0 && fcntl(STDOUT_FILENO, F_SETFL, console->output_flags) < 0)
		e = errno;
	return e;
}

int
cli_console_resume (struct cli_console *console)
{
	if (!console->suspended)
		return 0;
	if ((console->input_flags >= 0 && fcntl(STDIN_FILENO, F_SETFL, console->input_flags | O_NONBLOCK) < 0) ||
	    (console->output_flags >= 0 && fcntl(STDOUT_FILENO, F_SETFL, console->output_flags | O_NONBLOCK) < 0))
		return errno;
	console->suspended = false;
	cli_loop_ready(console->loop, &console->input_step);
	console->resize_pending = true;
	console->dirty = CLI_CONSOLE_PAINT_ALL;
	if (console->terminal_saved && tcsetattr(STDIN_FILENO, TCSANOW, &console->resume_state) < 0)
		return errno;
	if (console->fullscreen)
		return cli_console_queue(console, CSTR_VIEW("\033[?1049h\033[?7l\033[?2004h"));
	return 0;
}

void
cli_console_destroy (struct cli_console **ptr)
{
	if (!ptr || !*ptr)
		return;
	struct cli_console *console = *ptr;
	*ptr = nullptr;
	if (console->callback_active) {
		rl_free_line_state();
		rl_callback_sigcleanup();
	}
	cli_console_remove_callback(console);
	cli_loop_unready(&console->input_step);
	(void)cli_console_suspend(console);
	while (!list_is_empty(&console->lines))
		cli_console_drop_line(console);
	free(console->restored_line);
	console->restored_line = nullptr;
	free(console->immediate_line);
	console->immediate_line = nullptr;
	if (!console->simple && console->terminal) {
		rl_clear_history();
		rl_prep_term_function = rl_prep_terminal;
		rl_completion_display_matches_hook = nullptr;
		rl_getc_function = rl_getc;
		rl_input_available_hook = nullptr;
		rl_attempted_completion_function = nullptr;
		rl_redisplay_function = rl_redisplay;
		rl_outstream = stdout;
		(void)fclose(console->terminal);
		console->terminal = nullptr;
	}
	if (console->locale) {
		(void)uselocale(console->previous_locale);
		freelocale(console->locale);
		console->locale = nullptr;
	}
	if (console->input_flags >= 0)
		(void)fcntl(STDIN_FILENO, F_SETFL, console->input_flags);
	if (console->output_flags >= 0)
		(void)fcntl(STDOUT_FILENO, F_SETFL, console->output_flags);
	dstr_cleanup(&console->history_last);
	dstr_cleanup(&console->draft);
	dstr_cleanup(&console->prompt);
	dstr_cleanup(&console->completion_labels);
	dstr_cleanup(&console->clean);
	dstr_cleanup(&console->screen);
	for (size_t i = 0; i < 4; ++i)
		dstr_cleanup(&console->decorations[i]);
	for (size_t i = 0; i < CLI_CONSOLE_PANES; ++i) {
		cli_scrollback_fini(&console->panes[i].scrollback);
		for (size_t row = 0; row < CLI_CONSOLE_ROWS; ++row)
			dstr_cleanup(&console->panes[i].rows[row]);
	}
	buf_fini(&console->output_queue);
	if (cli_console_active == console)
		cli_console_active = nullptr;
	free(console);
}

int
cli_console_set_display (struct cli_console *console,
                         enum cli_style      display)
{
	if (display >= CLI_STYLE_COUNT)
		return EINVAL;
	struct cli_style_entry const *sequence = &console->palette[display];
	int e = 0;
	if (!console->fullscreen && console->display != display && sequence->length)
		e = cli_console_queue(console, (struct cstr_view){sequence->bytes, sequence->length});
	if (!e)
		console->display = display;
	return e;
}

static char const *
cli_console_style (struct cli_console *console, enum cli_style display)
{
	return console->palette[display].bytes;
}

int cli_console_remember(struct cli_console *console, struct cstr_view text)
{
	if (!console || (text.len && (!text.data || memchr(text.data, 0, text.len))))
		return EINVAL;
	if (text.len > CLI_CONSOLE_HISTORY_LIMIT)
		return EFBIG;
	if (console->simple || !text.len || cstr_view_equal(text, cstr_view_dstr(&console->history_last)))
		return 0;
	dstr_auto copy = dstr(nullptr);
	if (!dstr_cat_n(&copy, text.data, text.len))
		return ENOMEM;
	/* Count submitted entries. Readline's mutable snapshots of recalled
	 * entries have no public length API. */
	while (console->history_count &&
	       (console->history_count == CLI_CONSOLE_HISTORY_COUNT ||
	        console->history_bytes + text.len > CLI_CONSOLE_HISTORY_LIMIT)) {
		free_history_entry(remove_history(0));
		console->history_bytes -= console->history_sizes[console->history_first];
		console->history_first = (console->history_first + 1) % CLI_CONSOLE_HISTORY_COUNT;
		--console->history_count;
	}
	dstr_cleanup(&console->history_last);
	console->history_last = dstr_move(&copy);
	add_history(dstr_get(&console->history_last));
	console->history_sizes[(console->history_first + console->history_count) % CLI_CONSOLE_HISTORY_COUNT] = text.len;
	console->history_bytes += text.len;
	++console->history_count;
	return 0;
}

static int cli_console_read_simple(struct cli_console *console, struct buf *bytes,
                                   bool multiline, char const *prompt)
{
	int error = cli_console_queue(console, cstr_view(prompt));
	if (error)
		return error;
	bool more;
	do {
		size_t start = bytes->tail;
		int byte;
		while ((byte = cli_console_byte(console)) != EOF && byte != '\n') {
			struct span room;
			int e = buf_reserve(bytes, 1, CLI_CONSOLE_INPUT_LIMIT - 1, &room);
			if (e)
				return e == ENOBUFS ? EFBIG : e;
			room.ptr[0] = (unsigned char)byte;
			buf_commit(bytes, 1);
		}
		if (console->input_error || (console->input_eof && bytes->tail == start))
			return console->input_error;
		if (bytes->tail > start && bytes->data[bytes->tail - 1] == '\r')
			--bytes->tail;
		more = multiline;
		if (bytes->tail > start) {
			unsigned char last = bytes->data[bytes->tail - 1];
			if (last == '/' || last == '\\') {
				--bytes->tail;
				more = last == '\\' && !more;
			}
		}
		struct span room;
		int e = buf_reserve(bytes, 1, CLI_CONSOLE_INPUT_LIMIT, &room);
		if (e)
			return e == ENOBUFS ? EFBIG : e;
		room.ptr[0] = '\n';
		buf_commit(bytes, 1);
	} while (more && !console->input_eof);
	return 0;
}

static int
cli_console_render_draft (struct cli_console *console,
                          struct dstr        *output,
                          unsigned           *cursor)
{
	struct cstr_view draft = cstr_view_dstr(&console->draft);
	/* The input row scrolls horizontally; the complete draft remains owned
	 * by Readline, including embedded newlines and its undo list. */
	size_t point = console->draft_point < 0 ? 0 : (size_t)console->draft_point;
	if (point > draft.len)
		point = draft.len;
	unsigned column = console->columns > 2 ? 2 : 0;
	size_t available = console->columns - column - 1;
	size_t retained = console->draft_start <= point ? console->draft_start : 0;
	size_t scanned = 0;
	size_t before = 0;
	size_t start = point;
	for (size_t next = point; next;) {
		size_t previous = next - 1;
		while (previous && ((unsigned char)draft.data[previous] & 0xc0) == 0x80)
			--previous;
		size_t width = cli_console_text_width(cstr_view_slice(draft, previous, next - previous));
		if (width > available - scanned)
			break;
		scanned += width;
		next = previous;
		if (width && (previous >= retained || !previous)) {
			start = previous;
			before = scanned;
		}
	}
	console->draft_start = start;
	*cursor = column + (unsigned)before + 1;
	if (!dstr_printf(output, "%s%.*s%s", cli_console_style(console, CLI_STYLE_PROMPT),
	                 (int)column, start ? "< " : "> ", cli_console_style(console, CLI_STYLE_USER_INPUT)))
		return ENOMEM;
	size_t run = start;
	size_t i = start;
	for (; i < draft.len;) {
		int cells;
		size_t bytes = cli_render_character(cstr_view_slice(draft, i, SIZE_MAX), &cells);
		unsigned width = cells < 0 ? 1 : (unsigned)cells;
		if (column + width >= console->columns)
			break;
		if (cells < 0) {
			if (!dstr_cat_n(output, draft.data + run, i - run) || !dstr_cat(output, " "))
				return ENOMEM;
			run = i + bytes;
		}
		i += bytes;
		column += width;
	}
	return dstr_cat_n(output, draft.data + run, i - run) &&
	       dstr_printf(output, "%*s", (int)(console->columns - column), "") ? 0 : ENOMEM;
}

static int
cli_console_frame (struct cli_console *console,
                   struct dstr        *output,
                   unsigned            edge)
{
#define CLI_CONSOLE_REPEAT8(x) x x x x x x x x
	static char const rule[] = CLI_CONSOLE_REPEAT8(CLI_CONSOLE_REPEAT8(CLI_CONSOLE_REPEAT8("─")));
#undef CLI_CONSOLE_REPEAT8
	static_assert(sizeof rule == CLI_CONSOLE_COLUMNS * 3 + 1);
	static char const *const corners[][3] = {{"┌", "┬", "┐"}, {"└", "┴", "┘"}};
	static char const *const names[] = {"Conversation", "Reasoning"};
	char const *const *border = corners[edge];
	char const *frame_style = cli_console_style(console, CLI_STYLE_FRAME);
	for (unsigned pane = 0; pane < CLI_CONSOLE_PANES; ++pane) {
		struct cli_scrollback const *scrollback = &console->panes[pane].scrollback;
		bool focused = console->focused == pane;
		char caption[64];
		int length = edge
			? snprintf(caption, sizeof caption, " %s | %zu unread ",
			           scrollback->offset ? "scrolling" : "live", scrollback->unread)
			: snprintf(caption, sizeof caption, " %c %s ", focused ? '*' : ' ', names[pane]);
		if (length < 0 || length >= (int)sizeof caption)
			return EOVERFLOW;
		unsigned width = cli_console_pane_width(console, pane);
		if ((unsigned)length > width)
			length = (int)width;
		char const *title_style = cli_console_style(console, focused ? CLI_STYLE_TITLE : CLI_STYLE_TITLE_INACTIVE);
		if (!dstr_printf(output, "%s%s%s%.*s%s%.*s", frame_style, border[pane], title_style,
		                 length, caption, frame_style, (int)(width - (unsigned)length) * 3, rule))
			return ENOMEM;
	}
	return dstr_cat(output, border[CLI_CONSOLE_PANES]) ? 0 : ENOMEM;
}

static int
cli_console_panels (struct cli_console *console,
                     struct dstr        *output)
{
	for (unsigned i = 0; i < CLI_CONSOLE_PANES; ++i) {
		if (!(console->dirty & (1u << i)))
			continue;
		struct cli_console_pane *pane = &console->panes[i];
		unsigned left = i ? cli_console_pane_width(console, CLI_CONSOLE_CONVERSATION) + 3 : 2;
		int e = cli_scrollback_render_pane(&pane->scrollback, left, 2,
		                                   cli_console_pane_width(console, i), console->rows - 4,
		                                   console->palette, pane->rows, output);
		if (e)
			return e;
	}
	if (!(console->dirty & CLI_CONSOLE_PAINT_RESET))
		return 0;
	if (!dstr_cat(output, cli_console_style(console, CLI_STYLE_FRAME)))
		return ENOMEM;
	unsigned divider = cli_console_pane_width(console, CLI_CONSOLE_CONVERSATION) + 2;
	for (unsigned row = 2; row < console->rows - 2; ++row) {
		if (!dstr_printf(output, "\033[%u;1H│\033[%u;%uH│\033[%u;%uH│",
		                 row, row, divider, row, console->columns))
			return ENOMEM;
	}
	return 0;
}

static int
cli_console_status (struct cli_console *console,
                    struct dstr        *output)
{
	static char const hint[] = " | Alt-Left/Right pane  Alt-Up/Down scroll  Ctrl-End follow";
	char status[128];
	int length = console->line_count
	           ? snprintf(status, sizeof status, " askai | %zu queued%s", console->line_count, hint)
	           : snprintf(status, sizeof status, " askai | %s%s", console->busy ? "working" : "ready", hint);
	if (length < 0)
		return EIO;
	/* The longest status, with a count of 20 digits, has 95 bytes. */
	return dstr_printf(output, "%s%-*.*s", cli_console_style(console, CLI_STYLE_STATUS),
	                   (int)console->columns, (int)console->columns, status) ? 0 : ENOMEM;
}

static int
cli_console_render (struct cli_console *console)
{
	if (!console->fullscreen || !console->dirty || console->suspended ||
	    console->output_queue.head != console->output_queue.tail)
		return 0;
	if (console->dirty & CLI_CONSOLE_PAINT_RESET) {
		for (size_t i = 0; i < 4; ++i)
			dstr_clear(&console->decorations[i]);
		for (size_t i = 0; i < CLI_CONSOLE_PANES; ++i)
			for (size_t row = 0; row < CLI_CONSOLE_ROWS; ++row)
				dstr_clear(&console->panes[i].rows[row]);
		console->dirty = CLI_CONSOLE_PAINT_ALL;
		console->cursor_column = 0;
	}
	struct dstr *screen = &console->screen;
	(void)dstr_clear(screen);
	bool framed = console->rows >= 6 && console->columns >= 8;
	int e = dstr_cat(screen, "\0337") ? 0 : ENOMEM;
	if (!e && framed)
		e = cli_console_panels(console, screen);
	if (!e && !framed && (console->dirty & CLI_CONSOLE_PAINT_RESET) && !dstr_cat(screen, "\033[2J"))
		e = ENOMEM;
	unsigned cursor = console->cursor_column;
	unsigned const flags[] = {CLI_CONSOLE_PAINT_FRAME, CLI_CONSOLE_PAINT_FRAME,
	                          CLI_CONSOLE_PAINT_STATUS, CLI_CONSOLE_PAINT_INPUT};
	unsigned const rows[] = {1, console->rows - 2, console->rows - 1, console->rows};
	for (unsigned i = framed ? 0 : 3; !e && i < 4; ++i) {
		if (!(console->dirty & flags[i]))
			continue;
		size_t begin = dstr_len(screen);
		if (i < 2)
			e = cli_console_frame(console, screen, i);
		else if (i == 3)
			e = cli_console_render_draft(console, screen, &cursor);
		else
			e = cli_console_status(console, screen);
		if (!e)
			e = cli_render_row(&console->decorations[i], screen, begin, rows[i], 1);
	}
	if (!e && (dstr_len(screen) > 2 || cursor != console->cursor_column)) {
		if (!dstr_cat(screen, "\0338") || (cursor != console->cursor_column &&
		    !dstr_printf(screen, "\033[%u;%uH", console->rows, cursor)))
			e = ENOMEM;
		if (!e)
			e = cli_console_queue(console, cstr_view_dstr(screen));
	}
	if (!e) {
		console->cursor_column = cursor;
		console->dirty = 0;
	}
	return e;
}

int
cli_console_interrupt (struct cli_console *console,
                       bool                terminate)
{
	if (terminate || console->simple) {
		console->terminated = true;
		console->input_error = EINTR;
	} else if (!console->busy) {
		console->interrupt_pending = true;
	}
	return 0;
}

int
cli_console_resize (struct cli_console *console)
{
	console->resize_pending = true;
	return 0;
}

int
cli_console_busy (struct cli_console *console,
                  bool                busy)
{
	console->busy = busy;
	console->dirty |= CLI_CONSOLE_PAINT_STATUS;
	if (busy && console->fullscreen)
		return cli_console_editor_start(console);
	return 0;
}

int
cli_console_service (struct cli_console *console)
{
	if (console->suspended)
		return 0;
	if (console->loop->dispatching)
		return EBUSY;
	int e = cli_console_output_drain(console);
	if (e)
		return e;
	if (!console->frame && !console->simple) {
		if (console->resize_pending) {
			console->resize_pending = false;
			rl_resize_terminal();
			if (console->fullscreen) {
				e = cli_console_dimensions(console);
				if (e)
					return e;
			} else if (console->callback_active) {
				rl_redisplay();
			}
		}
		if (console->interrupt_pending && console->callback_active) {
			console->interrupt_pending = false;
			/* rl_delete_text records the entire deletion in Readline's undo
			 * list before mutating it. Ctrl-_ restores the draft. */
			rl_begin_undo_group();
			rl_delete_text(0, rl_end);
			rl_end_undo_group();
			rl_point = 0;
			rl_redisplay_function();
		}
		for (unsigned count = 0; count < 64 && console->callback_active && !console->input_error &&
		                         !console->immediate_line; ++count) {
			e = cli_console_input_arm(console);
			if (e)
				return e;
			if (console->input_offset == console->input_size && !console->input_eof &&
			    !rl_pending_input && !RL_ISSTATE(RL_STATE_MACROINPUT))
				break;
			++console->frame;
			rl_callback_read_char();
			--console->frame;
			if (console->restored_line) {
				/* Native accept-line has already finished the old editor.
				 * Cancel its macro only after the new editor is prepared. */
				rl_free_line_state();
				console->input_state &= ~CLI_CONSOLE_INPUT_MACRO;
				console->pending_input = 0;
				rl_replace_line(console->restored_line, 0);
				rl_point = console->restored_point < rl_end ? console->restored_point : rl_end;
				free(console->restored_line);
				console->restored_line = nullptr;
				rl_redisplay_function();
			}
			if (console->input_state & CLI_CONSOLE_INPUT_NOTICE) {
				console->input_state &= ~CLI_CONSOLE_INPUT_NOTICE;
				e = cli_console_error(console, CSTR_VIEW("Input queue full; draft kept.\n"));
				if (e)
					return e;
			}
			cli_console_resume_keys(console);
			if (rl_end >= CLI_CONSOLE_INPUT_LIMIT)
				console->input_error = EFBIG;
		}
		if (console->callback_active && !console->input_error && !console->immediate_line) {
			e = cli_console_input_arm(console);
			if (e)
				return e;
			if (console->input_offset < console->input_size || rl_pending_input ||
			    RL_ISSTATE(RL_STATE_MACROINPUT))
				cli_loop_ready(console->loop, &console->input_step);
		}
	}
	e = cli_console_render(console);
	return e ? e : console->input_error;
}

static int
cli_console_readline (struct cli_console *console,
                      char const         *prompt)
{
	struct cstr_view prefix = cstr_view(prompt);
	if (!cstr_view_equal(prefix, cstr_view_dstr(&console->prompt))) {
		if (!dstr_reserve(&console->prompt, prefix.len))
			return ENOMEM;
		dstr_clear(&console->prompt);
		if (!dstr_cat_n(&console->prompt, prefix.data, prefix.len))
			return ENOMEM;
		if (console->callback_active && rl_set_prompt(dstr_get(&console->prompt)))
			return EIO;
		console->dirty |= CLI_CONSOLE_PAINT_INPUT;
	}
	if (!console->callback_active && !console->line_count && !console->eof_pending) {
		int e = cli_console_editor_start(console);
		if (e)
			return e;
	}
	while (!console->line_count && !console->eof_pending && !console->input_error) {
		int e = cli_console_service(console);
		if (e)
			return e;
		if (console->line_count || console->eof_pending)
			break;
		if (console->input_state & CLI_CONSOLE_INPUT_YIELD) {
			console->input_state &= ~CLI_CONSOLE_INPUT_YIELD;
			return EAGAIN;
		}
		e = cli_loop_turn(console->loop, UINT64_MAX);
		if (e)
			return e;
	}
	if (console->input_error) {
		rl_free_line_state();
		rl_callback_sigcleanup();
		cli_console_remove_callback(console);
	}
	return console->input_error;
}

int
cli_console_read (struct cli_console *console,
                  struct dstr        *text,
                  bool                multiline,
                  bool               *eof,
                  char const         *prompt)
{
	dstr_clear(text);
	*eof = false;
	if ((console->input_eof || console->eof_pending) && !console->line_count) {
		*eof = true;
		return 0;
	}
	if (!console->terminated && !console->line_count)
		console->input_error = 0;
	console->multiline = multiline;
	int e = cli_console_set_display(console, CLI_STYLE_USER_INPUT);
	struct buf bytes = {};
	struct cstr_view line = {};
	struct cli_console_line *accepted = nullptr;
	if (!e && console->simple) {
		e = cli_console_read_simple(console, &bytes, multiline, prompt);
		line = (struct cstr_view){(char const *)bytes.data, bytes.tail};
	} else if (!e) {
		e = cli_console_readline(console, prompt);
		if (console->line_count) {
			accepted = container_of(console->lines.next, struct cli_console_line, hook);
			line = (struct cstr_view){accepted->text, accepted->length};
		} else if (!e) {
			console->input_eof = true;
		}
	}
	if (!e && line.len) {
		if (console->simple && (memchr(line.data, 0, line.len) || jb_utf8(line.data, line.len) != line.len))
			e = EILSEQ;
		else if (!dstr_cat_n(text, line.data, line.len))
			e = ENOMEM;
	}
	if (!e && accepted) {
		if (!dstr_cat(text, "\n"))
			e = ENOMEM;
		if (!e && console->fullscreen) {
			struct cli_console_span const echo[] = {
				{cstr_view_dstr(&console->prompt), CLI_STYLE_PROMPT},
				{cstr_view_dstr(text), CLI_STYLE_USER_INPUT},
			};
			e = cli_console_print(console, echo, array_size(echo));
		}
	}
	buf_fini(&bytes);
	if (!e && accepted)
		cli_console_drop_line(console);
	console->dirty |= CLI_CONSOLE_PAINT_STATUS;
	if (console->eof_pending && !console->line_count) {
		console->input_eof = true;
		cli_console_remove_callback(console);
	}
	if (console->callback_active) {
		cli_console_resume_keys(console);
		cli_loop_ready(console->loop, &console->input_step);
	}
	if (!console->callback_active) {
		dstr_clear(&console->draft);
		console->draft_point = 0;
		console->dirty |= CLI_CONSOLE_PAINT_INPUT;
	}
	if (e)
		dstr_clear(text);
	*eof = console->input_eof && !dstr_len(text);
	int reset = cli_console_set_display(console, CLI_STYLE_RESET);
	return e ? e : reset;
}


static int
cli_console_output_write (struct cli_console_output *state,
                          struct dstr               *dest,
                          struct cstr_view           text)
{
	/* UTF-8 code points may straddle provider deltas. C1 controls are unsafe. */
	for (size_t i = 0; i < text.len; ++i) {
		unsigned char byte = (unsigned char)text.data[i];
		if (state->used) {
			if ((byte & 0xc0) == 0x80) {
				state->pending[state->used++] = byte;
				state->value = (state->value << 6) | (byte & 0x3f);
				if (state->used == state->expected) {
					uint32_t value = state->value;
					bool unsafe = value < state->minimum || value > 0x10ffff ||
					              (value >= 0xd800 && value <= 0xdfff) ||
					              (value >= 0x0080 && value <= 0x009f);
					struct cstr_view bytes = unsafe ? CSTR_VIEW("?") :
					                        (struct cstr_view){(char const *)state->pending, state->used};
					if (!dstr_cat_n(dest, bytes.data, bytes.len))
						return ENOMEM;
					state->used = 0;
				}
				continue;
			}
			if (!dstr_cat(dest, "?"))
				return ENOMEM;
			state->used = 0;
		}
		if (byte < 0x80) {
			bool safe = (byte >= 0x20 && byte != 0x7f) || byte == '\n' || byte == '\t';
			size_t end = i + 1;
			if (safe)
				while (end < text.len && (unsigned char)text.data[end] - 0x20u < 0x5f)
					++end;
			if (!dstr_cat_n(dest, safe ? text.data + i : "?", end - i))
				return ENOMEM;
			i = end - 1;
		} else {
			state->expected = byte >= 0xc2 && byte <= 0xdf ? 2 :
			                  byte >= 0xe0 && byte <= 0xef ? 3 :
			                  byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
			if (!state->expected) {
				if (!dstr_cat(dest, "?"))
					return ENOMEM;
				continue;
			}
			state->pending[0] = byte;
			state->used = 1;
			state->value = byte & (0x7f >> state->expected);
			state->minimum = state->expected == 2 ? 0x080 :
			                 state->expected == 3 ? 0x800 : 0x10000;
		}
	}
	return 0;
}

static int
cli_console_write_pane (struct cli_console      *console,
                        enum cli_console_pane_id target,
                        struct cstr_view         text)
{
	struct cli_console_pane *pane = &console->panes[target];
	struct dstr *clean = &console->clean;
	(void)dstr_clear(clean);
	int e = cli_console_output_write(&pane->output, clean, text);
	if (e)
		return e;
	if (!console->fullscreen)
		return cli_console_queue(console, cstr_view_dstr(clean));
	e = cli_scrollback_append_style(&pane->scrollback, cstr_view_dstr(clean), console->display);
	console->dirty |= 1u << target;
	if (pane->scrollback.offset)
		console->dirty |= CLI_CONSOLE_PAINT_FRAME;
	cli_loop_ready(console->loop, &console->input_step);
	return e;
}

int
cli_console_print (struct cli_console            *console,
                   struct cli_console_span const *spans,
                   size_t                         count)
{
	for (size_t i = 0; i < count; ++i)
		if (spans[i].style >= CLI_STYLE_COUNT)
			return EINVAL;
	if (console->fullscreen) {
		/* The full-screen UI keeps the style of each span with its text. */
		int e = 0;
		for (size_t i = 0; !e && i < count; ++i) {
			console->display = spans[i].style;
			e = cli_console_write_pane(console, CLI_CONSOLE_CONVERSATION, spans[i].text);
		}
		console->display = CLI_STYLE_RESET;
		return e;
	}
	/* A stream changes its style only where the style changes. */
	struct cli_console_output *state = &console->panes[CLI_CONSOLE_CONVERSATION].output;
	struct dstr *clean = &console->clean;
	enum cli_style display = console->display;
	(void)dstr_clear(clean);
	for (size_t i = 0; i <= count; ++i) {
		enum cli_style style = i < count ? spans[i].style : CLI_STYLE_RESET;
		struct cli_style_entry const *sequence = &console->palette[style];
		if (style != display && !dstr_cat_n(clean, sequence->bytes, sequence->length))
			return ENOMEM;
		display = style;
		int e = i < count ? cli_console_output_write(state, clean, spans[i].text) : 0;
		if (e)
			return e;
	}
	int e = cli_console_queue(console, cstr_view_dstr(clean));
	if (!e)
		console->display = CLI_STYLE_RESET;
	return e;
}

int
cli_console_write (struct cli_console *console,
                   struct cstr_view    text)
{
	return cli_console_write_pane(console, CLI_CONSOLE_CONVERSATION, text);
}

int
cli_console_reasoning (struct cli_console *console,
                       struct cstr_view    text)
{
	return cli_console_write_pane(console, CLI_CONSOLE_REASONING, text);
}

int
cli_console_write_error (struct cstr_view const *parts,
                         size_t                  count)
{
	struct cli_console_output state = {};
	dstr_auto clean = dstr(nullptr);
	for (size_t i = 0; i < count; ++i) {
		int e = cli_console_output_write(&state, &clean, parts[i]);
		if (e)
			return e;
	}
	size_t length = dstr_len(&clean);
	return fwrite(dstr_get(&clean), 1, length, stderr) != length || fflush(stderr) ? EIO : 0;
}

int
cli_console_flush (struct cli_console *console)
{
	int e = cli_console_output_drain(console);
	return e ? e : cli_console_render(console);
}


bool
cli_console_pending (struct cli_console const *console)
{
	return console->output_working || console->output_queue.head != console->output_queue.tail;
}

bool
cli_console_queued (struct cli_console const *console)
{
	return console->line_count || console->eof_pending;
}

void
cli_console_yield (struct cli_console *console)
{
	if (!console->simple) {
		console->input_state |= CLI_CONSOLE_INPUT_YIELD;
		cli_loop_ready(console->loop, &console->input_step);
	}
}

bool
cli_console_blocked (struct cli_console const *console)
{
	return console->output_queue.tail - console->output_queue.head >= 1024 * 1024;
}

void
cli_console_set_completion (struct cli_console      *console,
                            cli_console_complete_fn *callback,
                            void                    *context)
{
	console->complete = callback;
	console->complete_context = context;
}

int
cli_console_error (struct cli_console *console,
                   struct cstr_view    text)
{
	if (!console || !console->fullscreen)
		return cli_console_write_error(&text, 1);
	enum cli_style saved = console->display;
	int e = cli_console_print(console, &(struct cli_console_span){text, CLI_STYLE_ERROR}, 1);
	int reset = cli_console_set_display(console, saved);
	return e ? e : reset;
}

void
cli_console_set_immediate (struct cli_console       *console,
                           cli_console_immediate_fn *callback,
                           void                     *context)
{
	console->immediate = callback;
	console->immediate_context = context;
}

int
cli_console_take_immediate (struct cli_console *console,
                            struct dstr        *text,
                            bool               *ready)
{
	*ready = false;
	if (console->frame || !console->immediate_line)
		return 0;
	struct cstr_view line = {console->immediate_line, console->immediate_length};
	int e = cstr_view_store(line, text);
	if (e)
		return e;
	struct cli_console_span const echo[] = {
		{cstr_view_dstr(&console->prompt), CLI_STYLE_USER_INPUT},
		{line, CLI_STYLE_USER_INPUT},
		{CSTR_VIEW("\n"), CLI_STYLE_USER_INPUT},
	};
	enum cli_style saved = console->display;
	e = cli_console_print(console, echo, array_size(echo));
	int reset = cli_console_set_display(console, saved);
	if (!e)
		e = reset;
	if (e)
		return e;
	free(console->immediate_line);
	console->immediate_line = nullptr;
	console->immediate_length = 0;
	cli_console_resume_keys(console);
	cli_loop_ready(console->loop, &console->input_step);
	*ready = true;
	return 0;
}
