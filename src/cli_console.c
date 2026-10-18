/*
 * Display helpers adapted from llama.cpp common/console.cpp, commit
 * b76b13379d3b7d67ab0bc424516bb6fd2522bd8e.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "buf.h"
#include "cli_console.h"
#include "dstr.h"
#include "jb.h"

enum {
	CLI_CONSOLE_INPUT_LIMIT   = 1024 * 1024 * 16,
	CLI_CONSOLE_HISTORY_LIMIT = 1024 * 1024,
	CLI_CONSOLE_HISTORY_COUNT =  256
};

struct cli_console_output {
	uint32_t       value;
	uint32_t       minimum;
	unsigned       used;
	unsigned       expected;
	unsigned char  pending[4];
};

struct cli_console {
	cli_console_complete_fn         *complete;
	void                            *complete_context;
	volatile sig_atomic_t           *interrupted;
	char                            *accepted_line;
	struct autocomplete_match const *candidates;
	size_t                           candidate_count;
	size_t                           accepted_length;
	size_t                           input_offset;
	size_t                           input_size;
	size_t                           history_first;
	size_t                           history_count;
	size_t                           history_bytes;
	size_t                           history_sizes[CLI_CONSOLE_HISTORY_COUNT];
	struct dstr                      history_last;
	struct termios                   initial_state;
	struct cli_console_output        output;
	int                              input_error;
	int                              pending_input;
	int                              wake_fd;
	enum cli_console_display         display;
	bool                             color;
	bool                             simple;
	bool                             input_eof;
	bool                             multiline;
	bool                             callback_active;
	bool                             line_ready;
	unsigned char                    input_buffer[4096];
};

/* Only Readline's callbacks use this bridge. All owned state lives in the
 * console, and the pointer is cleared before its storage is released. */
static struct cli_console *cli_console_active;

static bool
cli_console_stopped (struct cli_console const *console)
{
	return console->interrupted && *console->interrupted;
}

static void
cli_console_remove_callback (struct cli_console *console)
{
	if (console->callback_active) {
		rl_callback_handler_remove();
		console->callback_active = false;
	}
}

static int
cli_console_wait (struct cli_console *console)
{
	for (;;) {
		if (cli_console_stopped(console))
			return EINTR;
		struct pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {console->wake_fd, POLLIN, 0}};
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (cli_console_stopped(console))
			return EINTR;
		if (fds[1].revents & POLLIN) {
			char events[64];
			while (read(console->wake_fd, events, sizeof events) > 0) {}
			if (cli_console_stopped(console))
				return EINTR;
			if (console->callback_active) {
				rl_resize_terminal();
				rl_redisplay();
			}
		}
		if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLNVAL))
			return EIO;
		if (fds[1].revents & POLLHUP)
			return EPIPE;
		if (fds[0].revents & (POLLIN | POLLHUP))
			return 0;
	}
}

static int
cli_console_byte (struct cli_console *console)
{
	if (cli_console_stopped(console))
		console->input_error = EINTR;
	if (console->input_eof || console->input_error)
		return EOF;
	if (console->input_offset < console->input_size)
		return console->input_buffer[console->input_offset++];
	for (;;) {
		console->input_error = cli_console_wait(console);
		if (console->input_error)
			return EOF;
		ssize_t size = read(STDIN_FILENO, console->input_buffer, sizeof console->input_buffer);
		if (size > 0) {
			console->input_size = (size_t)size;
			console->input_offset = 1;
			return console->input_buffer[0];
		}
		if (!size) {
			console->input_eof = true;
			return EOF;
		}
		if (errno != EINTR && errno != EAGAIN) {
			console->input_error = errno;
			return EOF;
		}
	}
}

static int
cli_console_getc (FILE *stream)
{
	(void)stream;
	struct cli_console *console = cli_console_active;
	int byte = cli_console_byte(console);
	if (byte == EOF && RL_ISSTATE(RL_STATE_MOREINPUT) && !console->input_error)
		console->input_error = EIO;
	/* Abort inside Readline's callback frame: EOF during a partial paste
	 * makes Readline 8.2 use an unterminated buffer. */
	if (console->input_error)
		rl_abort(0, 0);
	return byte;
}

static int
cli_console_available (void)
{
	struct cli_console *console = cli_console_active;
	if (cli_console_stopped(console) ||
	    console->input_error         ||
	    console->input_offset < console->input_size)
		return 1;
	int timeout = rl_set_keyboard_input_timeout(-1);
	struct timespec deadline = {timeout / 1000000, (timeout % 1000000) * 1000L};
	struct pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {console->wake_fd, POLLIN, 0}};
	int ready = ppoll(fds, 2, &deadline, nullptr);
	return cli_console_stopped(console) ||
	       (ready > 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)));
}

static void
cli_console_accept (char *line)
{
	struct cli_console *console = cli_console_active;
	console->accepted_line = line;
	console->accepted_length = line ? strlen(line) : 0;
	console->line_ready = true;
	/* Pause macros and pending keys until the next prompt is installed. */
	console->pending_input = rl_pending_input;
	rl_clear_pending_input();
	RL_UNSETSTATE(RL_STATE_MACROINPUT);
	cli_console_remove_callback(console);
}

static int
cli_console_submit (int count,
                    int key)
{
	struct cli_console *console = cli_console_active;
	bool more = console->multiline;
	if (rl_end) {
		char last = rl_line_buffer[rl_end - 1];
		if (last == '/' || last == '\\') {
			rl_delete_text(rl_end - 1, rl_end);
			if (rl_point > rl_end)
				rl_point = rl_end;
			more = last == '\\' && !more;
		}
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
	if (!console->complete || start || end < 0 || rl_end < 0)
		return nullptr;
	struct cstr_view line = {rl_line_buffer, (size_t)rl_end};
	console->input_error = console->complete(console->complete_context, line, (size_t)end,
	                                         &console->candidates, &console->candidate_count);
	if (console->input_error || !console->candidate_count)
		return nullptr;
	if (!console->candidates) {
		console->input_error = EINVAL;
		return nullptr;
	}
	struct cstr_view suffix = {};
	for (size_t i = 0; i < console->candidate_count; ++i) {
		struct autocomplete_match const *candidate = &console->candidates[i];
		struct cstr_view replacement = candidate->line;
		if (!replacement.data || !candidate->cursor || candidate->cursor > replacement.len ||
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
	char **matches = rl_completion_matches(text, cli_console_candidate);
	if (matches && rl_completion_type == '?' && !matches[1]) {
		/* A possible-completions request must never insert a unique match. */
		char **listing = calloc(3, sizeof *listing);
		do {
			if (listing) {
				listing[0] = calloc(1, 1);
				if (listing[0]) {
					listing[1] = matches[0];
					break;
				}

				free(listing);
				listing = nullptr;
			}

			free(matches[0]);
			matches[0] = nullptr;
			console->input_error = ENOMEM;
		} while (0);

		free(matches);
		matches = listing;
	}

	if (matches && rl_completion_type != '?')
		rl_point = (int)(line.len - suffix.len);
	console->candidates = nullptr;
	console->candidate_count = 0;
	return matches;
}

struct cli_console *
cli_console_create (bool                   simple,
                    bool                   color,
                    volatile sig_atomic_t *interrupted,
                    int                    wake_fd,
                    int                   *e)
{
	if (cli_console_active) {
		*e = EBUSY;
		return nullptr;
	}

	struct cli_console *console = calloc(1, sizeof *console);
	if (!console) {
		*e = ENOMEM;
		return nullptr;
	}

	bool const tty_stdout = isatty(STDOUT_FILENO);
	console->history_last = dstr(nullptr);
	console->simple = simple || !tty_stdout ||
	                  !isatty(STDIN_FILENO);
	console->color = color && tty_stdout;
	console->interrupted = interrupted;
	console->wake_fd = wake_fd;

	if (!console->simple) {
		if (tcgetattr(STDIN_FILENO,
		              &console->initial_state) < 0) {
			*e = errno;
			free(console);
			console = nullptr;
			return nullptr;
		}
		(void)setlocale(LC_CTYPE, "");
		rl_readline_name = "askai-cli";
		rl_catch_signals = 0;
		rl_catch_sigwinch = 0;
		rl_instream = stdin;
		rl_outstream = stdout;
		rl_initialize();
		rl_add_defun("askai-accept-line", cli_console_submit, -1);
		char const *names[] = {"emacs-standard", "vi-insertion", "vi-move"};
		for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
			Keymap map = rl_get_keymap_by_name(names[i]);
			if (!map)
				continue;
			rl_bind_keyseq_in_map("\033[5~", rl_history_search_backward, map);
			rl_bind_keyseq_in_map("\033[6~", rl_history_search_forward, map);
			rl_bind_keyseq_in_map("\033[1;5D", rl_backward_word, map);
			rl_bind_keyseq_in_map("\033[1;5C", rl_forward_word, map);
			rl_bind_key_in_map('\r', cli_console_submit, map);
			rl_bind_key_in_map('\n', cli_console_submit, map);
		}
		rl_variable_bind("enable-bracketed-paste", "on");
		/* User bindings, including $if askai-cli, override our defaults. */
		(void)rl_read_init_file(nullptr);
		rl_getc_function = cli_console_getc;
		rl_input_available_hook = cli_console_available;
		rl_attempted_completion_function = cli_console_complete;
		rl_completer_word_break_characters = "";
		rl_completer_quote_characters = "";
		using_history();
		stifle_history(CLI_CONSOLE_HISTORY_COUNT);
	}

	cli_console_active = console;
	*e = 0;
	return console;
}

void
cli_console_destroy (struct cli_console **ptr)
{
	if (!ptr || !*ptr)
		return;

	struct cli_console *console = *ptr;
	*ptr = nullptr;

	cli_console_remove_callback(console);
	free(console->accepted_line);
	console->accepted_line = nullptr;
	(void)cli_console_set_display(console, CLI_CONSOLE_RESET);

	if (!console->simple) {
		rl_clear_history();
		rl_getc_function = rl_getc;
		rl_input_available_hook = nullptr;
		rl_attempted_completion_function = nullptr;
		while (tcsetattr(STDIN_FILENO, TCSANOW,
		                 &console->initial_state) < 0
		       && errno == EINTR) {}
	}
	dstr_cleanup(&console->history_last);
	cli_console_active = nullptr;
	free(console);
}

int
cli_console_set_display (struct cli_console       *console,
                         enum cli_console_display  display)
{
	if (console->color && console->display != display) {
		static char const *const colors[] = {
			"\x1b[0m", "\x1b[35m", "\x1b[33m", "\x1b[90m",
			"\x1b[1m\x1b[32m", "\x1b[1m\x1b[31m"
		};
		if ((unsigned)display >= sizeof colors / sizeof *colors)
			return EINVAL;
		if (fputs(colors[display], stdout) == EOF || fflush(stdout))
			return EIO;
		console->display = display;
	}
	return 0;
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
	if ((prompt && fputs(prompt, stdout) == EOF) || fflush(stdout))
		return EIO;
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

static int cli_console_readline(struct cli_console *console, char const *prompt)
{
	console->line_ready = false;
	console->callback_active = true;
	rl_callback_handler_install(prompt ? prompt : "", cli_console_accept);
	if (rl_executing_macro)
		RL_SETSTATE(RL_STATE_MACROINPUT);
	if (console->pending_input) {
		rl_execute_next(console->pending_input);
		console->pending_input = 0;
	}
	while (!console->line_ready && !console->input_error) {
		console->input_error = console->input_offset < console->input_size || rl_pending_input ||
		                       RL_ISSTATE(RL_STATE_MACROINPUT) ? 0 : cli_console_wait(console);
		if (console->input_error)
			break;
		if (console->wake_fd < 0)
			rl_resize_terminal();
		rl_callback_read_char();
		if (cli_console_stopped(console))
			console->input_error = EINTR;
		else if (rl_end >= CLI_CONSOLE_INPUT_LIMIT)
			console->input_error = EFBIG;
	}
	if (console->input_error) {
		rl_free_line_state();
		rl_callback_sigcleanup();
	}
	cli_console_remove_callback(console);
	return console->input_error;
}

int cli_console_read(struct cli_console *console, struct dstr *text,
                     bool multiline, bool *eof, char const *prompt)
{
	dstr_clear(text);
	*eof = false;
	console->input_error = 0;
	console->multiline = multiline;
	int e = cli_console_set_display(console, CLI_CONSOLE_USER_INPUT);
	struct buf bytes = {};
	struct cstr_view line = {};
	if (!e && console->simple) {
		e = cli_console_read_simple(console, &bytes, multiline, prompt);
		line = (struct cstr_view){(char const *)bytes.data, bytes.tail};
	} else if (!e) {
		e = cli_console_readline(console, prompt);
		line = (struct cstr_view){console->accepted_line, console->accepted_length};
		if (!console->accepted_line)
			console->input_eof = true;
	}
	if (!e && line.len) {
		if (line.len >= CLI_CONSOLE_INPUT_LIMIT && !console->simple)
			e = EFBIG;
		else if (memchr(line.data, 0, line.len) || jb_utf8(line.data, line.len) != line.len)
			e = EILSEQ;
		else if (!dstr_cat_n(text, line.data, line.len))
			e = ENOMEM;
	}
	if (!e && !console->simple && console->accepted_line) {
		if (line.len <= CLI_CONSOLE_HISTORY_LIMIT)
			e = cli_console_remember(console, line);
		if (!e && !dstr_cat(text, "\n"))
			e = ENOMEM;
	}
	buf_fini(&bytes);
	free(console->accepted_line);
	console->accepted_line = nullptr;
	console->accepted_length = 0;
	if (e) {
		dstr_clear(text);
		if (!console->simple)
			(void)fputc('\n', stdout);
	}
	*eof = console->input_eof && !dstr_len(text);
	int reset = cli_console_set_display(console, CLI_CONSOLE_RESET);
	return e ? e : reset;
}

static int
cli_console_output_write (struct cli_console_output *state,
                          FILE                      *dest,
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
					if (value < state->minimum ||
					    value > 0x10ffff       ||
					    (value >= 0xd800 && value <= 0xdfff) ||
					    (value >= 0x0080 && value <= 0x009f))
						(void)fputc('?', dest);
					else
						(void)fwrite(state->pending, 1,
						             state->used, dest);
					state->used = 0;
				}
				continue;
			}
			(void)fputc('?', dest);
			state->used = 0;
		}
		if (byte < 0x80) {
			(void)fputc((byte >= 0x20 && byte != 0x7f) ||
			            byte == '\n' || byte == '\t'
			            ? byte : '?', dest);
		} else {
			state->expected = byte >= 0xc2 && byte <= 0xdf ? 2 :
			                  byte >= 0xe0 && byte <= 0xef ? 3 :
			                  byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
			if (!state->expected) {
				(void)fputc('?', dest);
				continue;
			}
			state->pending[0] = byte;
			state->used = 1;
			state->value = byte & (0x7f >> state->expected);
			state->minimum = state->expected == 2 ? 0x080 :
			                 state->expected == 3 ? 0x800 : 0x10000;
		}
	}
	return ferror(dest) ? EIO : 0;
}

int
cli_console_write (struct cli_console *console,
                   struct cstr_view    text)
{
	return cli_console_output_write(&console->output, stdout, text);
}

int
cli_console_write_error (struct cstr_view text)
{
	static struct cli_console_output state;
	int e = cli_console_output_write(&state, stderr, text);
	return fflush(stderr) || e ? EIO : 0;
}

int
cli_console_flush (struct cli_console *console)
{
	(void)console;
	return fflush(stdout) ? EIO : 0;
}

void
cli_console_set_completion (struct cli_console      *console,
                            cli_console_complete_fn *callback,
                            void                    *context)
{
	console->complete = callback;
	console->complete_context = context;
}
