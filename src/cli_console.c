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
#include "cli_state.h"
#include "compat.h"
#include "dstr.h"
#include "jb.h"
#include "list.h"
#include "loop.h"
#include "util.h"

enum {
	CLI_CONSOLE_INPUT_LIMIT   = AUTOCOMPLETE_BYTES,
	CLI_CONSOLE_INPUT_COUNT   = 32,
	CLI_CONSOLE_HISTORY_LIMIT = CLI_STATE_INPUT_BYTES,
	CLI_CONSOLE_OUTPUT_LIMIT  = 32 * 1024 * 1024,
	CLI_CONSOLE_TAB_INTERVAL  = 400 * MS,
	CLI_CONSOLE_ROWS          = 256,
	CLI_CONSOLE_COLUMNS       = 512,
};

/* The bits of the flags of a console, besides CLI_CONSOLE_SIMPLE. */
enum {
	CLI_CONSOLE_FULLSCREEN        = 1u << 2,
	CLI_CONSOLE_SUSPENDED         = 1u << 3,
	CLI_CONSOLE_TERMINAL_SAVED    = 1u << 4,
	CLI_CONSOLE_RESIZE_PENDING    = 1u << 5,
	CLI_CONSOLE_INTERRUPT_PENDING = 1u << 6,
	CLI_CONSOLE_TERMINATED        = 1u << 7,
	CLI_CONSOLE_BUSY              = 1u << 8,
	CLI_CONSOLE_INPUT_EOF         = 1u << 9,
	CLI_CONSOLE_MULTILINE         = 1u << 10,
	CLI_CONSOLE_CALLBACK_ACTIVE   = 1u << 11,
	CLI_CONSOLE_EOF_PENDING       = 1u << 12,
	CLI_CONSOLE_INPUT_MACRO       = 1u << 13,
	CLI_CONSOLE_INPUT_NOTICE      = 1u << 14,
	CLI_CONSOLE_INPUT_YIELD       = 1u << 15,
	CLI_CONSOLE_TAB_PENDING       = 1u << 16,
	CLI_CONSOLE_TAB_ACTIVE        = 1u << 17,
	CLI_CONSOLE_TAB_LISTING       = 1u << 18,
	CLI_CONSOLE_TAB_COMPLETED     = 1u << 19,
	CLI_CONSOLE_IN_REGULAR        = 1u << 20,  /* stdin is a regular file */
	CLI_CONSOLE_IN_WORKING        = 1u << 21,  /* a blocker runs in.job */
	CLI_CONSOLE_OUT_REGULAR       = 1u << 22,  /* stdout is a regular file */
	CLI_CONSOLE_OUT_WORKING       = 1u << 23,  /* a blocker runs out.job */
	CLI_CONSOLE_TAB = CLI_CONSOLE_TAB_PENDING | CLI_CONSOLE_TAB_ACTIVE |
	                  CLI_CONSOLE_TAB_LISTING | CLI_CONSOLE_TAB_COMPLETED,
};

struct cli_console_output {
	uint32_t       value;
	unsigned       used;
	unsigned       expected;
	unsigned char  pending[4];
};

/* The blocker job of a stream, which reads stdin or writes stdout. */
struct cli_console_io {
	struct cli_io_job job;
	ssize_t           count;
	unsigned char     bytes[4096];
};

/* The state of one standard stream of the console. Member in of struct
 * cli_console is stdin, and member out is stdout.
 */
struct cli_console_stream {
	struct loop_ref       ref;      /* The watch of the descriptor. ref.fd is
	                                   -1 while it is not watched. */
	struct cli_console_io job;      /* The blocker job for a regular file. */
	int                   flags;    /* The file status flags before the console
	                                   set O_NONBLOCK, or -1. */
	int                   error;
};

struct cli_console_completion {
	uint32_t               begin;
	uint32_t               length;
	enum autocomplete_kind kind;
};

enum cli_console_pane_id : uint32_t {
	CLI_CONSOLE_CONVERSATION,
	CLI_CONSOLE_REASONING,
	CLI_CONSOLE_PANES,
};

struct cli_console_pane {
	struct cli_scrollback     scrollback;
	struct cli_console_output output;
	struct dstr               rows[CLI_CONSOLE_ROWS];
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
	struct cli_console_stream        in;
	struct cli_console_stream        out;
	struct cli_console_pane          panes[CLI_CONSOLE_PANES];
	struct buf                       output_queue;
	struct dstr                      draft;
	struct dstr                      prompt;
	struct dstr                      completion_labels;
	struct dstr                      decorations[4];
	struct dstr                      clean;  /**< sanitized output */
	struct dstr                      screen; /**< a frame of the full-screen UI */
	struct cli_console_lines         queue;  /**< the accepted lines */
	char                            *restored_line;
	char                            *immediate_line;
	struct autocomplete_match const *candidates;
	struct cli_style_entry const    *palette;
	uint64_t                         completion_time;
	size_t                           candidate_count;
	size_t                           completion_count;
	size_t                           draft_start;
	size_t                           immediate_length;
	size_t                           input_offset;
	size_t                           input_size;
	struct cli_state_ring            history;
	size_t                           history_sizes[CLI_STATE_INPUTS];  /**< the bytes of each */
	struct dstr                      history_last;
	struct termios                   initial_state;
	struct termios                   resume_state;
	struct cli_console_completion    completions[AUTOCOMPLETE_MATCHES];
	enum cli_style                   display;
	enum cli_console_pane_id         focused;
	int                              pending_input;
	int                              restored_point;
	int                              draft_point;
	unsigned                         rows;
	unsigned                         columns;
	unsigned                         frame;
	unsigned                         cursor_column;
	uint32_t                         dirty;
	uint64_t                         flags;
};

/* Only Readline's callbacks use this bridge. All owned state lives in the
 * console, and the pointer is cleared before its storage is released.
 */
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
	if (console->flags & CLI_CONSOLE_CALLBACK_ACTIVE) {
		rl_callback_handler_remove();
		console->flags &= ~CLI_CONSOLE_CALLBACK_ACTIVE;
	}
}

/* Drop the line that Readline edits, and remove its callback. */
static void
cli_console_abort_line (struct cli_console *console)
{
	rl_free_line_state();
	rl_callback_sigcleanup();
	cli_console_remove_callback(console);
}

/* Returns true if bytes of the input or its end are ready to be read. */
static bool
cli_console_has_input (struct cli_console const *console)
{
	return console->input_offset < console->input_size || (console->flags & CLI_CONSOLE_INPUT_EOF);
}

/* Make the count bytes in in.job.bytes the input of the console. A count of
 * 0 marks the end of the input.
 */
static void
cli_console_input_set (struct cli_console *console,
                       ssize_t             count)
{
	console->flags = (console->flags & ~CLI_CONSOLE_INPUT_EOF) | (count ? 0 : CLI_CONSOLE_INPUT_EOF);
	console->input_offset = 0;
	console->input_size = (size_t)count;
}

static void
cli_console_input_done (struct cli_io_job *job)
{
	struct cli_console *console = container_of(job, struct cli_console, in.job.job);
	console->flags &= ~CLI_CONSOLE_IN_WORKING;
	console->in.error = (console->flags & CLI_CONSOLE_TERMINATED) ? EINTR : job->blocker.error;
	if (!job->blocker.error)
		cli_console_input_set(console, console->in.job.count);
}

static int
cli_console_input_work (struct cli_io_job *job)
{
	struct cli_console_io *input = container_of(job, struct cli_console_io, job);
	do {
		input->count = read(STDIN_FILENO, input->bytes, sizeof input->bytes);
	} while (input->count < 0 && errno == EINTR);
	return input->count < 0 ? errno : 0;
}

static void
cli_console_input_ready (struct loop     *loop,
                         struct loop_ref *ref,
                         uint32_t         events)
{
	(void)events;
	struct cli_console *console = container_of(ref, struct cli_console, in.ref);
	ssize_t count = read(ref->fd, console->in.job.bytes, sizeof console->in.job.bytes);
	int e = count < 0 ? errno : 0;
	if (e == EAGAIN || e == EINTR)
		return;
	loop_unwatch(loop, ref);
	if (e)
		console->in.error = e;
	else
		cli_console_input_set(console, count);
}

static int
cli_console_input_arm (struct cli_console *console)
{
	if (cli_console_has_input(console) || console->in.error || console->in.ref.fd >= 0 ||
	    (console->flags & (CLI_CONSOLE_SUSPENDED | CLI_CONSOLE_IN_WORKING)))
		return 0;
	if (!(console->flags & CLI_CONSOLE_IN_REGULAR)) {
		int e = loop_watch(&console->loop->loop, &console->in.ref, STDIN_FILENO,
		                   EPOLLIN, cli_console_input_ready, nullptr);
		if (e != EPERM)
			return e;
		console->flags |= CLI_CONSOLE_IN_REGULAR;
	}
	if (console->io) {
		int e = cli_io_submit(console->io, &console->in.job.job);
		if (!e)
			console->flags |= CLI_CONSOLE_IN_WORKING;
		return e;
	}
	/* Console-only tests may omit a worker for a regular file. */
	console->in.job.job.blocker.error = cli_console_input_work(&console->in.job.job);
	cli_console_input_done(&console->in.job.job);
	return console->in.error;
}

static int
cli_console_wait (struct cli_console *console,
                  uint64_t            deadline)
{
	while (!console->in.error && !cli_console_has_input(console)) {
		int e = cli_console_input_arm(console);
		if (e)
			return e;
		if (cli_console_has_input(console))
			break;
		if (deadline != UINT64_MAX && loop_clock() >= deadline)
			return 0;
		e = cli_loop_turn(console->loop, deadline);
		if (!e && !console->frame)
			e = cli_console_service(console);
		if (e)
			return e;
	}
	return console->in.error;
}

static int
cli_console_byte (struct cli_console *console)
{
	if (console->in.error)
		return EOF;
	if (console->input_offset == console->input_size) {
		console->in.error = cli_console_wait(console, UINT64_MAX);
		if (console->in.error || (console->flags & CLI_CONSOLE_INPUT_EOF))
			return EOF;
	}
	return console->in.job.bytes[console->input_offset++];
}

static int
cli_console_getc (FILE *stream)
{
	(void)stream;
	struct cli_console *console = cli_console_active;
	int byte = cli_console_byte(console);
	if (byte == EOF && RL_ISSTATE(RL_STATE_MOREINPUT) && !console->in.error)
		console->in.error = EIO;
	/* Readline 8.2 cannot consume EOF midway through a bracketed paste. */
	if (console->in.error)
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
	console->in.error = cli_console_wait(console, deadline);
	return console->in.error || cli_console_has_input(console);
}

static bool
cli_console_is_immediate (struct cli_console *console,
                          struct cstr_view    line)
{
	return (console->flags & CLI_CONSOLE_FULLSCREEN) && console->immediate &&
	       console->immediate(console->immediate_context, line);
}

static void
cli_console_resume_keys (struct cli_console *console)
{
	if (console->immediate_line || rl_pending_input)
		return;
	if (console->flags & CLI_CONSOLE_INPUT_MACRO) {
		if (rl_executing_macro)
			RL_SETSTATE(RL_STATE_MACROINPUT);
		console->flags &= ~CLI_CONSOLE_INPUT_MACRO;
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
		console->flags |= CLI_CONSOLE_INPUT_MACRO;
	rl_clear_pending_input();
	RL_UNSETSTATE(RL_STATE_MACROINPUT);
}

static bool
cli_console_input_full (struct cli_console *console,
                        size_t              length,
                        bool                immediate)
{
	return immediate ? console->immediate_line != nullptr :
	       !tally_fits(&console->queue.used,
	                   (struct tally){ CLI_CONSOLE_INPUT_COUNT, CLI_CONSOLE_INPUT_LIMIT }, length);
}

int
cli_console_lines_push (struct cli_console_lines *queue,
                        struct dstr              *text,
                        size_t                    flags)
{
	struct cli_console_line *line = malloc(sizeof *line);
	if (!line)
		return ENOMEM;
	*line = (struct cli_console_line){.text = dstr_move(text), .flags = flags};
	list_append(&queue->lines, &line->hook);
	tally_add(&queue->used, dstr_len(&line->text));
	return 0;
}

size_t
cli_console_lines_pop (struct cli_console_lines *queue,
                       struct dstr              *text)
{
	struct cli_console_line *line = container_of(queue->lines.next, struct cli_console_line, hook);
	list_del(&line->hook);
	tally_sub(&queue->used, dstr_len(&line->text));
	if (text) {
		dstr_cleanup(text);
		*text = dstr_move(&line->text);
	}
	size_t flags = line->flags;
	dstr_cleanup(&line->text);
	free(line);
	return flags;
}

void
cli_console_lines_fini (struct cli_console_lines *queue)
{
	while (!list_is_empty(&queue->lines))
		(void)cli_console_lines_pop(queue, nullptr);
}

static void
cli_console_accept (char *line)
{
	struct cli_console *console = cli_console_active;
	if (!line) {
		console->flags |= CLI_CONSOLE_EOF_PENDING;
		cli_console_remove_callback(console);
		return;
	}
	struct cstr_view text = cstr_view(line);
	if (!text.len && (console->flags & CLI_CONSOLE_BUSY)) {
		free(line);
		return;
	}
	bool immediate = cli_console_is_immediate(console, text);
	if (cli_console_input_full(console, text.len, immediate)) {
		/* A custom inputrc binding may bypass our submit command. Restore
		 * its rejected submission after Readline prepares the next line. */
		console->restored_line = line;
		console->restored_point = rl_point;
		console->flags |= CLI_CONSOLE_INPUT_NOTICE;
		cli_console_pause_keys(console);
		console->dirty |= CLI_CONSOLE_PAINT_INPUT;
		return;
	}
	if (immediate) {
		console->immediate_line = line;
		console->immediate_length = text.len;
		cli_loop_wake(console->loop);
	} else {
		/* The line is copied into the queue, and text is set to a view
		 * of the copy. */
		dstr_auto copy = dstr(nullptr);
		int e = cstr_view_cat(text, &copy);
		if (!e)
			e = cli_console_lines_push(&console->queue, &copy, 0);
		free(line);
		if (e) {
			console->in.error = e;
			return;
		}
		text = cstr_view_dstr(&container_of(console->queue.lines.prev, struct cli_console_line, hook)->text);
	}
	cli_console_pause_keys(console);
	if (text.len >= CLI_CONSOLE_INPUT_LIMIT)
		console->in.error = EFBIG;
	else if (jb_utf8(text.data, text.len) != text.len)
		console->in.error = EILSEQ;
	else if (text.len <= CLI_CONSOLE_HISTORY_LIMIT)
		/* Record before Readline positions the new draft's history cursor. */
		console->in.error = cli_console_remember(console, text);
	/* While a request runs, Readline prepares a fresh draft itself. Keep
	 * its callback installed so editing never returns to canonical input. */
	uint64_t const working = CLI_CONSOLE_FULLSCREEN | CLI_CONSOLE_BUSY;
	if (!immediate && (console->flags & working) != working)
		cli_console_remove_callback(console);
	console->dirty |= CLI_CONSOLE_PAINT_INPUT | CLI_CONSOLE_PAINT_STATUS;
}

static int
cli_console_submit (int count,
                    int key)
{
	struct cli_console *console = cli_console_active;
	bool immediate = cli_console_is_immediate(console, (struct cstr_view){rl_line_buffer, (size_t)rl_end});
	char last = !immediate && rl_end ? rl_line_buffer[rl_end - 1] : 0;
	bool marked = last == '/' || last == '\\';
	bool multiline = console->flags & CLI_CONSOLE_MULTILINE;
	bool more = marked ? last == '\\' && !multiline : !immediate && multiline;
	if (!more && (rl_end || !(console->flags & CLI_CONSOLE_BUSY)) &&
	    cli_console_input_full(console, (size_t)rl_end, immediate)) {
		console->flags = (console->flags & ~CLI_CONSOLE_INPUT_MACRO) | CLI_CONSOLE_INPUT_NOTICE;
		console->pending_input = 0;
		/* Clearing MACROINPUT alone would replay the remaining macro
		 * bytes when the user next presses a physical key. */
		return rl_abort(count, key);
	}
	if (marked) {
		rl_delete_text(rl_end - 1, rl_end);
		if (rl_point > rl_end)
			rl_point = rl_end;
	}
	if (!more)
		return rl_newline(count, key);
	if (rl_end >= CLI_CONSOLE_INPUT_LIMIT - 1) {
		console->in.error = EFBIG;
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
	char *copy = strndup(candidate->line.data, candidate->cursor);
	if (!copy)
		console->in.error = ENOMEM;
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
	console->in.error = console->complete(console->complete_context, line, (size_t)end,
	                                      &console->candidates, &console->candidate_count);
	if (console->in.error || !console->candidate_count)
		return nullptr;
	if (!console->candidates || console->candidate_count > array_size(console->completions)) {
		console->in.error = EINVAL;
		return nullptr;
	}
	struct cstr_view suffix = {};
	for (size_t i = 0; i < console->candidate_count; ++i) {
		struct autocomplete_match const *candidate = &console->candidates[i];
		struct cstr_view replacement = candidate->line;
		if (!replacement.data || !candidate->cursor || candidate->cursor > replacement.len ||
		    candidate->label_begin >= candidate->cursor || candidate->kind >= AUTOCOMPLETE_KIND_COUNT ||
		    replacement.len >= CLI_CONSOLE_INPUT_LIMIT || memchr(replacement.data, 0, replacement.len) ||
		    util_char_start(replacement.data, replacement.len, candidate->cursor) != candidate->cursor)
			return nullptr;
		struct cstr_view tail = cstr_view_slice(replacement, candidate->cursor, SIZE_MAX);
		if (!i)
			suffix = tail;
		if (!cstr_view_equal(tail, suffix))
			return nullptr;
	}
	/* The text after the cursor is the same in each candidate, and it must
	 * end the line. */
	if (!cstr_view_ends(line, suffix))
		return nullptr;
	console->in.error = cli_console_completion_cache(console);
	if (console->in.error)
		return nullptr;
	char **matches = rl_completion_matches(text, cli_console_candidate);
	console->candidates = nullptr;
	console->candidate_count = 0;
	if (!matches)
		return nullptr;
	uint64_t const tab = CLI_CONSOLE_TAB_ACTIVE | CLI_CONSOLE_TAB_PENDING;
	if (console->completion_count == 1 && (console->flags & tab) == tab)
		console->flags = (console->flags & ~CLI_CONSOLE_TAB_PENDING) | CLI_CONSOLE_TAB_COMPLETED;
	if (rl_completion_type != '?') {
		rl_point = (int)(line.len - suffix.len);
	} else if (!matches[1]) {
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
			free(matches[0]);
			matches[0] = nullptr;
			console->in.error = ENOMEM;
		}
		free(matches);
		matches = listing;
	}
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
	if ((console->flags & (CLI_CONSOLE_TAB_ACTIVE | CLI_CONSOLE_TAB_LISTING)) ==
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
		if (column) {
			bool wrap = column >= columns || cells + 1 > columns - column;
			e = cli_scrollback_append(scrollback, wrap ? CSTR_VIEW("\n") : CSTR_VIEW(" "));
			column = wrap ? 0 : column + 1;
		}
		if (!e)
			e = cli_scrollback_append_style(scrollback, text, styles[completion->kind]);
		column += cells;
	}
	if (!e)
		e = cli_scrollback_append(scrollback, CSTR_VIEW("\n"));
	console->in.error = e;
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
	if (repeated && (console->flags & CLI_CONSOLE_TAB_COMPLETED)) {
		/* The first Tab already finished this word. Do not start completing
		 * its argument with the second keypress of the same pair. */
		console->flags &= ~CLI_CONSOLE_TAB;
		return 0;
	}
	bool listing = repeated && (console->flags & CLI_CONSOLE_TAB_PENDING);
	console->flags = (console->flags & ~CLI_CONSOLE_TAB) | CLI_CONSOLE_TAB_ACTIVE |
	                 (listing ? CLI_CONSOLE_TAB_LISTING : CLI_CONSOLE_TAB_PENDING);
	/* Each pair starts a new completion attempt. Readline otherwise treats
	 * every subsequent Tab as another request to print the same matches. */
	rl_last_func = nullptr;
	/* '!' completes unique matches and displays ambiguous ones. Explicit
	 * possible-completions bindings retain Readline's listing-only '?'. */
	int e = listing ? rl_complete_internal('!') : rl_complete(count, key);
	console->flags &= ~(CLI_CONSOLE_TAB_ACTIVE | CLI_CONSOLE_TAB_LISTING);
	return e;
}

static void
cli_console_prepare (int meta)
{
	rl_prep_terminal(meta);
	struct cli_console *console = cli_console_active;
	struct termios state;
	if (tcgetattr(STDIN_FILENO, &state) < 0) {
		console->in.error = errno;
		return;
	}
	state.c_lflag |= NOFLSH;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &state) < 0)
		console->in.error = errno;
}

static void
cli_console_output_done (struct cli_io_job *job)
{
	struct cli_console *console = container_of(job, struct cli_console, out.job.job);
	console->flags &= ~CLI_CONSOLE_OUT_WORKING;
	console->out.error = job->blocker.error;
	if (!job->blocker.error)
		buf_consume(&console->output_queue, (size_t)console->out.job.count);
}

static int
cli_console_output_work (struct cli_io_job *job)
{
	struct cli_console_io *output = container_of(job, struct cli_console_io, job);
	ssize_t count;
	do {
		count = write(STDOUT_FILENO, output->bytes, (size_t)output->count);
	} while (count < 0 && errno == EINTR);
	output->count = count;
	return count < 0 ? errno : count ? 0 : EIO;
}

static void
cli_console_output_ready (struct loop     *loop,
                          struct loop_ref *ref,
                          uint32_t         events)
{
	(void)loop;
	(void)events;
	struct cli_console *console = container_of(ref, struct cli_console, out.ref);
	console->out.error = cli_console_output_drain(console);
}

static int
cli_console_output_drain (struct cli_console *console)
{
	if (console->out.error || (console->flags & (CLI_CONSOLE_OUT_WORKING | CLI_CONSOLE_SUSPENDED)))
		return console->out.error;
	struct span bytes = buf_peek(&console->output_queue);
	if (!bytes.len) {
		loop_unwatch(&console->loop->loop, &console->out.ref);
		return 0;
	}
	if ((console->flags & CLI_CONSOLE_OUT_REGULAR) && console->io) {
		size_t length = bytes.len < sizeof console->out.job.bytes
		                ? bytes.len : sizeof console->out.job.bytes;
		memcpy(console->out.job.bytes, bytes.ptr, length);
		console->out.job.count = (ssize_t)length;
		int e = cli_io_submit(console->io, &console->out.job.job);
		if (!e)
			console->flags |= CLI_CONSOLE_OUT_WORKING;
		return e;
	}
	/* A turn writes at most one bounded block. EPOLLOUT drives the rest. */
	size_t length = bytes.len < 65536 ? bytes.len : 65536;
	ssize_t count = write(STDOUT_FILENO, bytes.ptr, length);
	int e = count < 0 ? errno : 0;
	if (e && e != EAGAIN && e != EINTR)
		return e;
	if (count > 0)
		buf_consume(&console->output_queue, (size_t)count);
	if (console->output_queue.head == console->output_queue.tail) {
		loop_unwatch(&console->loop->loop, &console->out.ref);
		return 0;
	}
	if (!(console->flags & CLI_CONSOLE_OUT_REGULAR) && console->out.ref.fd < 0)
		return loop_watch(&console->loop->loop, &console->out.ref, STDOUT_FILENO,
		                  EPOLLOUT, cli_console_output_ready, nullptr);
	return 0;
}

static int
cli_console_queue (struct cli_console *console,
                   struct cstr_view    text)
{
	int e = buf_put(&console->output_queue, text.data, text.len, CLI_CONSOLE_OUTPUT_LIMIT);
	return e ? e : cli_console_output_drain(console);
}

static ssize_t
cli_console_terminal_write (void       *cookie,
                            char const *data,
                            size_t      length)
{
	struct cli_console *console = cookie;
	int e = cli_console_queue(console, (struct cstr_view){data, length});
	if (e) {
		console->out.error = e;
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
		console->in.error = ENOMEM;
	console->draft_point = rl_point;
	console->dirty |= CLI_CONSOLE_PAINT_INPUT;
}

/* Scroll the focused pane a page back (-1) or forward (1), or to its
 * bottom (0).
 */
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
	if (console->rows > array_size(console->panes[0].rows))
		console->rows = array_size(console->panes[0].rows);
	if (console->columns > CLI_CONSOLE_COLUMNS)
		console->columns = CLI_CONSOLE_COLUMNS;
	console->dirty = CLI_CONSOLE_PAINT_ALL;
	return 0;
}

static int
cli_console_editor_start (struct cli_console *console)
{
	if (console->flags & (CLI_CONSOLE_SIMPLE | CLI_CONSOLE_CALLBACK_ACTIVE | CLI_CONSOLE_SUSPENDED |
	                      CLI_CONSOLE_TERMINATED | CLI_CONSOLE_INPUT_EOF | CLI_CONSOLE_EOF_PENDING))
		return 0;
	console->flags |= CLI_CONSOLE_CALLBACK_ACTIVE;
	++console->frame;
	rl_callback_handler_install(dstr_get(&console->prompt), cli_console_accept);
	if (rl_executing_macro)
		RL_SETSTATE(RL_STATE_MACROINPUT);
	if (console->pending_input) {
		rl_execute_next(console->pending_input);
		console->pending_input = 0;
	}
	int e = console->in.error;
	--console->frame;
	if (!e)
		e = cli_console_input_arm(console);
	cli_loop_wake(console->loop);
	return e;
}

/* Set the file status flags of stdin and stdout to the flags that
 * cli_console_create() read, plus the flags in added. A stream whose flags
 * are -1 is skipped. Returns 0, or the error of the last fcntl() that
 * failed.
 */
static int
cli_console_streams_set (struct cli_console *console,
                         int                 added)
{
	int e = 0;
	if (console->in.flags >= 0 && fcntl(STDIN_FILENO, F_SETFL, console->in.flags | added) < 0)
		e = errno;
	if (console->out.flags >= 0 && fcntl(STDOUT_FILENO, F_SETFL, console->out.flags | added) < 0)
		e = errno;
	return e;
}

/* Bind the keys of the console in the keymaps of Readline. */
static void
cli_console_bind_keys (struct cli_console *console)
{
	static char const *const names[] = {"emacs-standard", "vi-insertion", "vi-move"};
	for (size_t i = 0; i < array_size(names); ++i) {
		Keymap map = rl_get_keymap_by_name(names[i]);
		if (!map)
			continue;
		rl_bind_keyseq_in_map("\033[5~", rl_history_search_backward, map);
		rl_bind_keyseq_in_map("\033[6~", rl_history_search_forward, map);
		if (console->flags & CLI_CONSOLE_FULLSCREEN) {
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
			rl_bind_key_in_map('\t', cli_console_tab, map);
		}
		rl_bind_keyseq_in_map("\033[1;5D", rl_backward_word, map);
		rl_bind_keyseq_in_map("\033[1;5C", rl_forward_word, map);
		rl_bind_key_in_map('\r', cli_console_submit, map);
		rl_bind_key_in_map('\n', cli_console_submit, map);
		rl_bind_key_in_map(0x1f, rl_undo_command, map);
	}
}

/* Set up Readline for the console, and enter the full-screen mode if the
 * terminal supports it. Returns 0 or an error number.
 */
static int
cli_console_setup_readline (struct cli_console *console)
{
	/* Readline edits UTF-8 only in a UTF-8 locale. Its initialization
	 * resets the global locale, so the console sets the locale of this
	 * thread only. */
	console->locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
	if (!console->locale)
		return errno;
	console->previous_locale = uselocale(console->locale);
	if (!console->previous_locale)
		return errno;
	if (tcgetattr(STDIN_FILENO, &console->initial_state) < 0)
		return errno;
	console->flags |= CLI_CONSOLE_TERMINAL_SAVED;
	console->terminal = fopencookie(console, "w", (cookie_io_functions_t){
		.write = cli_console_terminal_write,
	});
	if (!console->terminal)
		return errno;
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
	if (!rl_get_termcap("up") || !rl_get_termcap("cl"))
		console->flags &= ~CLI_CONSOLE_FULLSCREEN;
	rl_add_defun("askai-accept-line", cli_console_submit, -1);
	rl_add_defun("askai-scroll-up", cli_console_page_up, -1);
	rl_add_defun("askai-scroll-down", cli_console_page_down, -1);
	cli_console_bind_keys(console);
	rl_variable_bind("enable-bracketed-paste", "on");
	char const *inputrc = getenv("INPUTRC");
	(void)rl_read_init_file(inputrc && *inputrc ? inputrc : "~/.inputrc");
	rl_prep_term_function = cli_console_prepare;
	rl_getc_function = cli_console_getc;
	rl_input_available_hook = cli_console_available;
	rl_attempted_completion_function = cli_console_complete;
	rl_completer_word_break_characters = "";
	rl_completer_quote_characters = "";
	using_history();
	stifle_history(array_size(console->history_sizes));
	if (!(console->flags & CLI_CONSOLE_FULLSCREEN))
		return 0;
	rl_completion_display_matches_hook = cli_console_matches;
	rl_redisplay_function = cli_console_redisplay;
	int e = cli_console_dimensions(console);
	if (!e)
		e = cli_console_queue(console, CSTR_VIEW("\033[?1049h\033[?7l"));
	return e ? e : cli_console_flush(console);
}

struct cli_console *
cli_console_create (uint32_t         flags,
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
	list_init(&console->queue.lines);
	console->in = (struct cli_console_stream){.ref.fd = -1, .flags = -1,
	                                          .job.job.work = cli_console_input_work,
	                                          .job.job.done = cli_console_input_done};
	console->out = (struct cli_console_stream){.ref.fd = -1, .flags = -1,
	                                           .job.job.work = cli_console_output_work,
	                                           .job.job.done = cli_console_output_done};
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
	for (size_t i = 0; i < array_size(console->decorations); ++i)
		console->decorations[i] = dstr(nullptr);
	for (size_t i = 0; i < array_size(console->panes); ++i) {
		cli_scrollback_init(&console->panes[i].scrollback);
		for (size_t row = 0; row < array_size(console->panes[i].rows); ++row)
			console->panes[i].rows[row] = dstr(nullptr);
	}
	console->palette = cli_style_palette(flags & CLI_CONSOLE_COLOR);
	char const *term = getenv("TERM");
	console->flags = (flags & CLI_CONSOLE_SIMPLE) || !isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO)
	               ? CLI_CONSOLE_SIMPLE : term && *term && strcmp(term, "dumb") ? CLI_CONSOLE_FULLSCREEN : 0;
	struct stat input, output;
	*e = fstat(STDIN_FILENO, &input) < 0 || fstat(STDOUT_FILENO, &output) < 0 ? errno : 0;
	if (*e)
		goto fail;
	console->flags |= (S_ISREG(input.st_mode) ? CLI_CONSOLE_IN_REGULAR : 0) |
	                  (S_ISREG(output.st_mode) ? CLI_CONSOLE_OUT_REGULAR : 0);
	console->in.flags = fcntl(STDIN_FILENO, F_GETFL);
	console->out.flags = fcntl(STDOUT_FILENO, F_GETFL);
	*e = console->in.flags < 0 || console->out.flags < 0 ? errno
	   : cli_console_streams_set(console, O_NONBLOCK);
	if (*e)
		goto fail;
	cli_console_active = console;
	*e = (console->flags & CLI_CONSOLE_SIMPLE) ? 0 : cli_console_setup_readline(console);
	if (*e)
		goto fail;
	return console;
fail:
	cli_console_destroy(&console);
	return nullptr;
}

/* Save the terminal state for cli_console_resume(), leave the full-screen
 * mode, and restore the terminal state that cli_console_create() saved.
 * Returns 0, or the error of the last call that failed.
 */
static int
cli_console_terminal_restore (struct cli_console *console)
{
	int e = tcgetattr(STDIN_FILENO, &console->resume_state) < 0 ? errno : 0;
	/* These short best-effort writes must never wait for a slow terminal. */
	static char const restore[] = "\033[?2004l\033[?7h\033[0m\033[?1049l";
	if (console->flags & CLI_CONSOLE_FULLSCREEN)
		(void)write(STDOUT_FILENO, restore, sizeof restore - 1);
	while (tcsetattr(STDIN_FILENO, TCSANOW, &console->initial_state) < 0)
		if (errno != EINTR)
			return errno;
	return e;
}

int
cli_console_suspend (struct cli_console *console)
{
	if (console->flags & CLI_CONSOLE_SUSPENDED)
		return 0;
	int e = (console->flags & CLI_CONSOLE_TERMINAL_SAVED) ? cli_console_terminal_restore(console) : 0;
	console->flags |= CLI_CONSOLE_SUSPENDED;
	loop_unwatch(&console->loop->loop, &console->in.ref);
	loop_unwatch(&console->loop->loop, &console->out.ref);
	int flags_error = cli_console_streams_set(console, 0);
	return flags_error ? flags_error : e;
}

int
cli_console_resume (struct cli_console *console)
{
	if (!(console->flags & CLI_CONSOLE_SUSPENDED))
		return 0;
	int e = cli_console_streams_set(console, O_NONBLOCK);
	if (e) {
		/* The console stays suspended, so restore the flags that
		 * cli_console_create() read. */
		(void)cli_console_streams_set(console, 0);
		return e;
	}
	console->flags = (console->flags & ~CLI_CONSOLE_SUSPENDED) | CLI_CONSOLE_RESIZE_PENDING;
	console->dirty = CLI_CONSOLE_PAINT_ALL;
	cli_loop_wake(console->loop);
	if ((console->flags & CLI_CONSOLE_TERMINAL_SAVED) &&
	    tcsetattr(STDIN_FILENO, TCSANOW, &console->resume_state) < 0)
		return errno;
	if (console->flags & CLI_CONSOLE_FULLSCREEN)
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
	if (console->flags & CLI_CONSOLE_CALLBACK_ACTIVE)
		cli_console_abort_line(console);
	(void)cli_console_suspend(console);
	cli_console_lines_fini(&console->queue);
	free(console->restored_line);
	console->restored_line = nullptr;
	free(console->immediate_line);
	console->immediate_line = nullptr;
	if (!(console->flags & CLI_CONSOLE_SIMPLE) && console->terminal) {
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
	dstr_cleanup(&console->history_last);
	dstr_cleanup(&console->draft);
	dstr_cleanup(&console->prompt);
	dstr_cleanup(&console->completion_labels);
	dstr_cleanup(&console->clean);
	dstr_cleanup(&console->screen);
	for (size_t i = 0; i < array_size(console->decorations); ++i)
		dstr_cleanup(&console->decorations[i]);
	for (size_t i = 0; i < array_size(console->panes); ++i) {
		cli_scrollback_fini(&console->panes[i].scrollback);
		for (size_t row = 0; row < array_size(console->panes[i].rows); ++row)
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
	if (!(console->flags & CLI_CONSOLE_FULLSCREEN) && console->display != display && sequence->length)
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
	if ((console->flags & CLI_CONSOLE_SIMPLE) || !text.len ||
	    cstr_view_equal(text, cstr_view_dstr(&console->history_last)))
		return 0;
	if (cstr_view_store(text, &console->history_last))
		return ENOMEM;
	/* The console counts the bytes of the submitted entries itself, because
	 * Readline changes recalled entries in place and offers no function for
	 * their length. */
	while (cli_state_ring_full(&console->history, text.len)) {
		free_history_entry(remove_history(0));
		cli_state_ring_drop(&console->history, console->history_sizes[console->history.first]);
	}
	add_history(dstr_get(&console->history_last));
	console->history_sizes[cli_state_ring_add(&console->history, text.len)] = text.len;
	return 0;
}

/* Append the bytes of the input up to the next newline, which is read
 * too, or up to its end.
 */
static int
cli_console_read_bytes (struct cli_console *console,
                        struct buf         *bytes)
{
	int byte;
	while ((byte = cli_console_byte(console)) != EOF && byte != '\n') {
		int e = buf_put(bytes, &(unsigned char){ (unsigned char)byte }, 1, CLI_CONSOLE_INPUT_LIMIT - 1);
		if (e)
			return e == ENOBUFS ? EFBIG : e;
	}
	return 0;
}

static int cli_console_read_simple(struct cli_console *console, struct buf *bytes,
                                   bool multiline, struct cstr_view prompt)
{
	int error = cli_console_queue(console, prompt);
	if (error)
		return error;
	bool more;
	do {
		size_t start = bytes->tail;
		int e = cli_console_read_bytes(console, bytes);
		if (e)
			return e;
		if (console->in.error || ((console->flags & CLI_CONSOLE_INPUT_EOF) && bytes->tail == start))
			return console->in.error;
		if (bytes->tail > start && bytes->data[bytes->tail - 1] == '\r')
			--bytes->tail;
		/* A trailing / ends the input. A trailing \ continues it, or
		 * ends it in multiline input. */
		unsigned char last = bytes->tail > start ? bytes->data[bytes->tail - 1] : 0;
		bool marked = last == '/' || last == '\\';
		more = marked ? last == '\\' && !multiline : multiline;
		bytes->tail -= marked;
		e = buf_put(bytes, "\n", 1, CLI_CONSOLE_INPUT_LIMIT);
		if (e)
			return e == ENOBUFS ? EFBIG : e;
	} while (more && !(console->flags & CLI_CONSOLE_INPUT_EOF));
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
		size_t previous = util_clip(draft.data, draft.len, next - 1);
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
	while (i < draft.len) {
		int cells;
		size_t bytes = cli_render_character(cstr_view_slice(draft, i, SIZE_MAX), &cells);
		unsigned width = cells < 0 ? 1 : (unsigned)cells;
		if (column + width >= console->columns)
			break;
		i += bytes;
		column += width;
		if (cells >= 0)
			continue;
		/* A character without a width is shown as a space. */
		if (!dstr_cat_n(output, draft.data + run, i - bytes - run) || !dstr_cat(output, " "))
			return ENOMEM;
		run = i;
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
	for (unsigned pane = 0; pane < array_size(console->panes); ++pane) {
		struct cli_scrollback const *scrollback = &console->panes[pane].scrollback;
		bool focused = console->focused == pane;
		char buffer[64];
		struct cstr_view caption = edge
			? util_format_view(buffer, sizeof buffer, " %s | %zu unread ",
			                   scrollback->offset ? "scrolling" : "live", scrollback->unread)
			: util_format_view(buffer, sizeof buffer, " %c %s ", focused ? '*' : ' ', names[pane]);
		unsigned width = cli_console_pane_width(console, pane);
		if (caption.len > width)
			caption.len = width;
		char const *title_style = cli_console_style(console, focused ? CLI_STYLE_TITLE : CLI_STYLE_TITLE_INACTIVE);
		if (!dstr_printf(output, "%s%s%s%.*s%s%.*s", frame_style, border[pane], title_style,
		                 (int)caption.len, caption.data, frame_style, (int)(width - caption.len) * 3, rule))
			return ENOMEM;
	}
	return dstr_cat(output, border[CLI_CONSOLE_PANES]) ? 0 : ENOMEM;
}

static int
cli_console_panels (struct cli_console *console,
                    struct dstr        *output)
{
	for (unsigned i = 0; i < array_size(console->panes); ++i) {
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
	char buffer[128];
	struct cstr_view status = console->queue.used.n
		? util_format_view(buffer, sizeof buffer, " askai | %zu queued%s", console->queue.used.n, hint)
		: util_format_view(buffer, sizeof buffer, " askai | %s%s",
		                   console->flags & CLI_CONSOLE_BUSY ? "working" : "ready", hint);
	return dstr_printf(output, "%s%-*.*s", cli_console_style(console, CLI_STYLE_STATUS),
	                   (int)console->columns, (int)console->columns, status.data) ? 0 : ENOMEM;
}

/* Clear the stored rows and decorations of the screen, and mark all parts
 * dirty, so that the next frame paints the whole screen.
 */
static void
cli_console_reset (struct cli_console *console)
{
	for (size_t i = 0; i < array_size(console->decorations); ++i)
		dstr_clear(&console->decorations[i]);
	for (size_t i = 0; i < array_size(console->panes); ++i)
		for (size_t row = 0; row < array_size(console->panes[i].rows); ++row)
			dstr_clear(&console->panes[i].rows[row]);
	console->dirty = CLI_CONSOLE_PAINT_ALL;
	console->cursor_column = 0;
}

static int
cli_console_render (struct cli_console *console)
{
	if ((console->flags & (CLI_CONSOLE_FULLSCREEN | CLI_CONSOLE_SUSPENDED)) != CLI_CONSOLE_FULLSCREEN ||
	    !console->dirty || console->output_queue.head != console->output_queue.tail)
		return 0;
	bool reset = console->dirty & CLI_CONSOLE_PAINT_RESET;
	if (reset)
		cli_console_reset(console);
	struct dstr *screen = &console->screen;
	(void)dstr_clear(screen);
	bool framed = console->rows >= 6 && console->columns >= 8;
	int e = dstr_cat(screen, "\0337") ? 0 : ENOMEM;
	if (!e && framed)
		e = cli_console_panels(console, screen);
	if (!e && !framed && reset && !dstr_cat(screen, "\033[2J"))
		e = ENOMEM;
	unsigned cursor = console->cursor_column;
	unsigned const flags[] = {CLI_CONSOLE_PAINT_FRAME, CLI_CONSOLE_PAINT_FRAME,
	                          CLI_CONSOLE_PAINT_STATUS, CLI_CONSOLE_PAINT_INPUT};
	unsigned const rows[] = {1, console->rows - 2, console->rows - 1, console->rows};
	static_assert(array_size(flags) == array_size(console->decorations));
	for (unsigned i = framed ? 0 : 3; !e && i < array_size(flags); ++i) {
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
	bool moved = cursor != console->cursor_column;
	if (!e && (dstr_len(screen) > 2 || moved)) {
		if (!dstr_cat(screen, "\0338") ||
		    (moved && !dstr_printf(screen, "\033[%u;%uH", console->rows, cursor)))
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

void
cli_console_interrupt (struct cli_console *console,
                       bool                terminate)
{
	if (terminate || (console->flags & CLI_CONSOLE_SIMPLE)) {
		console->flags |= CLI_CONSOLE_TERMINATED;
		console->in.error = EINTR;
	} else if (!(console->flags & CLI_CONSOLE_BUSY)) {
		console->flags |= CLI_CONSOLE_INTERRUPT_PENDING;
	}
}

void
cli_console_resize (struct cli_console *console)
{
	console->flags |= CLI_CONSOLE_RESIZE_PENDING;
}

int
cli_console_busy (struct cli_console *console,
                  bool                busy)
{
	console->flags = (console->flags & ~CLI_CONSOLE_BUSY) | (busy ? CLI_CONSOLE_BUSY : 0);
	console->dirty |= CLI_CONSOLE_PAINT_STATUS;
	if (busy && (console->flags & CLI_CONSOLE_FULLSCREEN))
		return cli_console_editor_start(console);
	return 0;
}

/* Pass the next key of the input to Readline. Then restore a draft that a
 * full input queue rejected, and report the full queue. Returns 0 or an
 * error number.
 */
static int
cli_console_read_key (struct cli_console *console)
{
	++console->frame;
	rl_callback_read_char();
	--console->frame;
	if (console->restored_line) {
		/* Readline's own accept-line ended the line and set up a new
		 * one. Stop the keyboard macro only now, and put back the line
		 * that cli_console_accept() did not queue. */
		rl_free_line_state();
		console->flags &= ~CLI_CONSOLE_INPUT_MACRO;
		console->pending_input = 0;
		rl_replace_line(console->restored_line, 0);
		rl_point = console->restored_point < rl_end ? console->restored_point : rl_end;
		free(console->restored_line);
		console->restored_line = nullptr;
		rl_redisplay_function();
	}
	if (console->flags & CLI_CONSOLE_INPUT_NOTICE) {
		console->flags &= ~CLI_CONSOLE_INPUT_NOTICE;
		int e = cli_console_error(console, CSTR_VIEW("Input queue full; draft kept.\n"));
		if (e)
			return e;
	}
	cli_console_resume_keys(console);
	if (rl_end >= CLI_CONSOLE_INPUT_LIMIT)
		console->in.error = EFBIG;
	return 0;
}

/* Returns true if the editor may read keys. That is the case while the
 * callback of Readline is installed, no input error occurred, and no
 * immediate command is pending.
 */
static bool
cli_console_editing (struct cli_console const *console)
{
	return (console->flags & CLI_CONSOLE_CALLBACK_ACTIVE) && !console->in.error && !console->immediate_line;
}

/* Apply the pending resize and interrupt, and pass up to 64 keys to
 * Readline. If more input is ready, wake the loop for another turn. Returns
 * 0 or an error number.
 */
static int
cli_console_edit (struct cli_console *console)
{
	int e = 0;
	if (console->flags & CLI_CONSOLE_RESIZE_PENDING) {
		console->flags &= ~CLI_CONSOLE_RESIZE_PENDING;
		rl_resize_terminal();
		if (console->flags & CLI_CONSOLE_FULLSCREEN)
			e = cli_console_dimensions(console);
		else if (console->flags & CLI_CONSOLE_CALLBACK_ACTIVE)
			rl_redisplay();
	}
	if (e)
		return e;
	uint64_t const interrupt = CLI_CONSOLE_INTERRUPT_PENDING | CLI_CONSOLE_CALLBACK_ACTIVE;
	if ((console->flags & interrupt) == interrupt) {
		console->flags &= ~CLI_CONSOLE_INTERRUPT_PENDING;
		/* Clear the draft. rl_delete_text() records the deletion in the
		 * undo list, so Ctrl-_ restores the draft. */
		rl_begin_undo_group();
		rl_delete_text(0, rl_end);
		rl_end_undo_group();
		rl_point = 0;
		rl_redisplay_function();
	}
	for (unsigned count = 0; count < 64 && cli_console_editing(console); ++count) {
		e = cli_console_input_arm(console);
		if (e)
			return e;
		if (!cli_console_has_input(console) && !rl_pending_input && !RL_ISSTATE(RL_STATE_MACROINPUT))
			break;
		e = cli_console_read_key(console);
		if (e)
			return e;
	}
	if (!cli_console_editing(console))
		return 0;
	e = cli_console_input_arm(console);
	if (e)
		return e;
	if (console->input_offset < console->input_size || rl_pending_input ||
	    RL_ISSTATE(RL_STATE_MACROINPUT))
		cli_loop_wake(console->loop);
	return 0;
}

int
cli_console_service (struct cli_console *console)
{
	if (console->flags & CLI_CONSOLE_SUSPENDED)
		return 0;
	if (console->loop->dispatching)
		return EBUSY;
	int e = cli_console_output_drain(console);
	if (!e && !console->frame && !(console->flags & CLI_CONSOLE_SIMPLE))
		e = cli_console_edit(console);
	if (!e)
		e = cli_console_render(console);
	return e ? e : console->in.error;
}

static int
cli_console_readline (struct cli_console *console,
                      struct cstr_view    prompt)
{
	if (!cstr_view_equal(prompt, cstr_view_dstr(&console->prompt))) {
		if (cstr_view_store(prompt, &console->prompt))
			return ENOMEM;
		if ((console->flags & CLI_CONSOLE_CALLBACK_ACTIVE) && rl_set_prompt(dstr_get(&console->prompt)))
			return EIO;
		console->dirty |= CLI_CONSOLE_PAINT_INPUT;
	}
	if (!(console->flags & CLI_CONSOLE_CALLBACK_ACTIVE) && !cli_console_queued(console)) {
		int e = cli_console_editor_start(console);
		if (e)
			return e;
	}
	while (!cli_console_queued(console) && !console->in.error) {
		int e = cli_console_service(console);
		if (e)
			return e;
		if (cli_console_queued(console))
			break;
		if (console->flags & CLI_CONSOLE_INPUT_YIELD) {
			console->flags &= ~CLI_CONSOLE_INPUT_YIELD;
			return EAGAIN;
		}
		e = cli_loop_turn(console->loop, UINT64_MAX);
		if (e)
			return e;
	}
	if (console->in.error)
		cli_console_abort_line(console);
	return console->in.error;
}

/* Read a line of the simple mode into text. Returns 0 or an error number. */
static int
cli_console_read_text_simple (struct cli_console *console,
                              struct dstr        *text,
                              bool                multiline,
                              struct cstr_view    prompt)
{
	struct buf bytes = {};
	int e = cli_console_read_simple(console, &bytes, multiline, prompt);
	struct cstr_view line = {(char const *)bytes.data, bytes.tail};
	if (!e && !jb_is_text(line))
		e = EILSEQ;
	if (!e && cstr_view_cat(line, text))
		e = ENOMEM;
	buf_fini(&bytes);
	return e;
}

/* Wait until Readline accepts a line, and copy the line and a newline into
 * text. In the full-screen mode, echo it in the conversation. Then drop the
 * line from the queue, unless an error occurred. Returns 0 or an error
 * number.
 */
static int
cli_console_read_text_readline (struct cli_console *console,
                                struct dstr        *text,
                                struct cstr_view    prompt)
{
	int e = cli_console_readline(console, prompt);
	if (!console->queue.used.n) {
		if (!e)
			console->flags |= CLI_CONSOLE_INPUT_EOF;
		return e;
	}
	if (e)
		return e;
	struct cli_console_line *accepted = container_of(console->queue.lines.next, struct cli_console_line, hook);
	if (cstr_view_cat(cstr_view_dstr(&accepted->text), text) || !dstr_cat(text, "\n"))
		return ENOMEM;
	if (console->flags & CLI_CONSOLE_FULLSCREEN) {
		struct cli_console_span const echo[] = {
			{cstr_view_dstr(&console->prompt), CLI_STYLE_PROMPT},
			{cstr_view_dstr(text), CLI_STYLE_USER_INPUT},
		};
		e = cli_console_print(console, echo, array_size(echo));
		if (e)
			return e;
	}
	(void)cli_console_lines_pop(&console->queue, nullptr);
	return 0;
}

int
cli_console_read (struct cli_console *console,
                  struct dstr        *text,
                  bool                multiline,
                  bool               *eof,
                  struct cstr_view    prompt)
{
	dstr_clear(text);
	*eof = false;
	if ((console->flags & (CLI_CONSOLE_INPUT_EOF | CLI_CONSOLE_EOF_PENDING)) && !console->queue.used.n) {
		*eof = true;
		return 0;
	}
	if (!(console->flags & CLI_CONSOLE_TERMINATED) && !console->queue.used.n)
		console->in.error = 0;
	console->flags = (console->flags & ~CLI_CONSOLE_MULTILINE) | (multiline ? CLI_CONSOLE_MULTILINE : 0);
	int e = cli_console_set_display(console, CLI_STYLE_USER_INPUT);
	if (!e)
		e = console->flags & CLI_CONSOLE_SIMPLE ? cli_console_read_text_simple(console, text, multiline, prompt)
		                                        : cli_console_read_text_readline(console, text, prompt);
	console->dirty |= CLI_CONSOLE_PAINT_STATUS;
	if ((console->flags & CLI_CONSOLE_EOF_PENDING) && !console->queue.used.n) {
		console->flags |= CLI_CONSOLE_INPUT_EOF;
		cli_console_remove_callback(console);
	}
	if (console->flags & CLI_CONSOLE_CALLBACK_ACTIVE) {
		cli_console_resume_keys(console);
		cli_loop_wake(console->loop);
	} else {
		dstr_clear(&console->draft);
		console->draft_point = 0;
		console->dirty |= CLI_CONSOLE_PAINT_INPUT;
	}
	if (e)
		dstr_clear(text);
	*eof = (console->flags & CLI_CONSOLE_INPUT_EOF) && !dstr_len(text);
	int reset = cli_console_set_display(console, CLI_STYLE_RESET);
	return e ? e : reset;
}


/* Add the continuation byte to the character in state. Once the character
 * is complete, append it to dest, or "?" if it is unsafe or overlong.
 */
static int
cli_console_output_next (struct cli_console_output *state,
                         unsigned char              byte,
                         struct dstr               *dest)
{
	static uint32_t const minimum[] = {[2] = 0x80, [3] = 0x800, [4] = 0x10000};
	state->pending[state->used++] = byte;
	state->value = (state->value << 6) | (byte & 0x3f);
	if (state->used < state->expected)
		return 0;
	uint32_t value = state->value;
	bool unsafe = value < minimum[state->expected] || value > 0x10ffff ||
	              (value >= 0xd800 && value <= 0xdfff) || (value >= 0x0080 && value <= 0x009f);
	struct cstr_view bytes = unsafe ? CSTR_VIEW("?") : (struct cstr_view){(char const *)state->pending, state->used};
	state->used = 0;
	return cstr_view_cat(bytes, dest);
}

/* Filter the first bytes of text, which is not empty, into dest. A byte
 * that continues the character in state is added to it, and a complete
 * character is appended, or "?" if it is unsafe. A byte that cuts the
 * character short appends "?" and is not used. A run of printable ASCII
 * is appended as it is, and an unsafe ASCII byte as "?". A lead byte
 * starts a character, or appends "?" if it is not valid. Sets *used to the
 * number of bytes used. Returns 0 or ENOMEM.
 */
static int
cli_console_output_step (struct cli_console_output *state,
                         struct dstr               *dest,
                         struct cstr_view           text,
                         size_t                    *used)
{
	unsigned char byte = (unsigned char)text.data[0];
	*used = 1;
	if (state->used) {
		if ((byte & 0xc0) == 0x80)
			return cli_console_output_next(state, byte, dest);
		/* A byte that does not continue the character cuts it short. */
		state->used = 0;
		*used = 0;
		return cstr_view_cat(CSTR_VIEW("?"), dest);
	}
	if (byte >= 0x80) {
		state->expected = byte >= 0xc2 && byte <= 0xdf ? 2 :
		                  byte >= 0xe0 && byte <= 0xef ? 3 :
		                  byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
		state->pending[0] = byte;
		state->used = state->expected != 0;
		state->value = byte & (0x7f >> state->expected);
		return state->expected ? 0 : cstr_view_cat(CSTR_VIEW("?"), dest);
	}
	if ((byte < 0x20 || byte == 0x7f) && byte != '\n' && byte != '\t')
		return cstr_view_cat(CSTR_VIEW("?"), dest);
	while (*used < text.len && (unsigned char)text.data[*used] - 0x20u < 0x5f)
		++*used;
	return cstr_view_cat(cstr_view_slice(text, 0, *used), dest);
}

static int
cli_console_output_write (struct cli_console_output *state,
                          struct dstr               *dest,
                          struct cstr_view           text)
{
	/* UTF-8 code points may straddle provider deltas. C1 controls are unsafe. */
	for (size_t i = 0; i < text.len;) {
		size_t used;
		int e = cli_console_output_step(state, dest, cstr_view_slice(text, i, SIZE_MAX), &used);
		if (e)
			return e;
		i += used;
	}
	return 0;
}

int
cli_console_write (struct cli_console *console,
                   bool                reasoning,
                   struct cstr_view    text)
{
	enum cli_console_pane_id target = reasoning ? CLI_CONSOLE_REASONING : CLI_CONSOLE_CONVERSATION;
	struct cli_console_pane *pane = &console->panes[target];
	struct dstr *clean = &console->clean;
	(void)dstr_clear(clean);
	int e = cli_console_output_write(&pane->output, clean, text);
	if (e)
		return e;
	if (!(console->flags & CLI_CONSOLE_FULLSCREEN))
		return cli_console_queue(console, cstr_view_dstr(clean));
	e = cli_scrollback_append_style(&pane->scrollback, cstr_view_dstr(clean), console->display);
	console->dirty |= 1u << target;
	if (pane->scrollback.offset)
		console->dirty |= CLI_CONSOLE_PAINT_FRAME;
	cli_loop_wake(console->loop);
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
	if (console->flags & CLI_CONSOLE_FULLSCREEN) {
		/* The full-screen UI keeps the style of each span with its text. */
		int e = 0;
		for (size_t i = 0; !e && i < count; ++i) {
			console->display = spans[i].style;
			e = cli_console_write(console, false, spans[i].text);
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
	return (console->flags & CLI_CONSOLE_OUT_WORKING) || console->output_queue.head != console->output_queue.tail;
}

bool
cli_console_queued (struct cli_console const *console)
{
	return console->queue.used.n || (console->flags & CLI_CONSOLE_EOF_PENDING);
}

void
cli_console_yield (struct cli_console *console)
{
	if (!(console->flags & CLI_CONSOLE_SIMPLE)) {
		console->flags |= CLI_CONSOLE_INPUT_YIELD;
		cli_loop_wake(console->loop);
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

/* Print the spans as cli_console_print() does, and then restore the
 * display style. Returns the first error.
 */
static int
cli_console_print_kept (struct cli_console            *console,
                        struct cli_console_span const *spans,
                        size_t                         count)
{
	enum cli_style saved = console->display;
	int e = cli_console_print(console, spans, count);
	int reset = cli_console_set_display(console, saved);
	return e ? e : reset;
}

int
cli_console_error (struct cli_console *console,
                   struct cstr_view    text)
{
	if (!console || !(console->flags & CLI_CONSOLE_FULLSCREEN))
		return cli_console_write_error(&text, 1);
	return cli_console_print_kept(console, &(struct cli_console_span){text, CLI_STYLE_ERROR}, 1);
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
	e = cli_console_print_kept(console, echo, array_size(echo));
	if (e)
		return e;
	free(console->immediate_line);
	console->immediate_line = nullptr;
	console->immediate_length = 0;
	cli_console_resume_keys(console);
	cli_loop_wake(console->loop);
	*ready = true;
	return 0;
}
