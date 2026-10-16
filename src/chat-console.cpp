/*
 * Display helpers adapted from llama.cpp common/console.cpp, commit
 * b76b13379d3b7d67ab0bc424516bb6fd2522bd8e.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */

#include <algorithm>
#include <cerrno>
#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

#include "chat-console.h"

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_GRAY    "\x1b[90m"
#define ANSI_COLOR_RESET   "\x1b[0m"
#define ANSI_BOLD          "\x1b[1m"

namespace chat_console {

	static constexpr size_t INPUT_LIMIT = 16 * 1024 * 1024;
	static constexpr size_t HISTORY_LIMIT = 1024 * 1024;
	static bool advanced_display = false;
	static bool simple_io = true;
	static bool initialized = false;
	static bool input_eof = false;
	static bool multiline_input = false;
	static bool callback_active = false;
	static bool line_ready = false;
	static int input_error = 0;
	static int pending_input = 0;
	static int wake_fd = -1;
	static display_type current_display = DISPLAY_TYPE_RESET;
	static FILE *out = stdout;
	static termios initial_state{};
	static volatile sig_atomic_t *interrupted = nullptr;
	static char *accepted_line = nullptr;
	static unsigned char input_buffer[4096];
	static size_t input_offset = 0;
	static size_t input_size = 0;
	static completion_callback completion_cb = nullptr;
	static std::vector<std::pair<std::string, size_t>> completion_matches;

	static bool stopped() {
		return interrupted && *interrupted;
	}

	static void remove_callback() {
		if (callback_active) {
			rl_callback_handler_remove();
			callback_active = false;
		}
	}

	static int wait_input() {
		for (;;) {
			if (stopped()) {
				return EINTR;
			}
			pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {wake_fd, POLLIN, 0}};
			if (poll(fds, 2, -1) < 0) {
				if (errno == EINTR && !stopped()) {
					continue;
				}
				return errno;
			}
			if (stopped()) {
				return EINTR;
			}
			if (fds[1].revents & POLLIN) {
				// The owner uses a nonblocking self-pipe for signals, including resize.
				char events[64];
				while (read(wake_fd, events, sizeof events) > 0) {}
				if (stopped()) {
					return EINTR;
				}
				if (callback_active) {
					rl_resize_terminal();
					rl_redisplay();
				}
			}
			if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLNVAL)) {
				return EIO;
			}
			if (fds[1].revents & POLLHUP) {
				return EPIPE;
			}
			if (fds[0].revents & (POLLIN | POLLHUP)) {
				return 0;
			}
		}
	}

	static int input_byte() {
		if (stopped()) {
			input_error = EINTR;
		}
		if (input_eof || input_error) {
			return EOF;
		}
		if (input_offset < input_size) {
			return input_buffer[input_offset++];
		}
		for (;;) {
			input_error = wait_input();
			if (input_error) {
				return EOF;
			}
			ssize_t size = read(STDIN_FILENO, input_buffer, sizeof input_buffer);
			if (size > 0) {
				input_size = static_cast<size_t>(size);
				input_offset = 1;
				return input_buffer[0];
			}
			if (!size) {
				input_eof = true;
				return EOF;
			}
			if (errno != EINTR && errno != EAGAIN) {
				input_error = errno;
				return EOF;
			}
		}
	}

	static int readline_getc(FILE *) {
		// Readline can request more input within an escape sequence or paste.
		// Keep these reads interruptible by the same self-pipe as the outer loop.
		int byte = input_byte();
		if (byte == EOF && RL_ISSTATE(RL_STATE_MOREINPUT) && !input_error) {
			input_error = EIO;
		}
		if (input_error) {
			// Abort within Readline's callback frame. Returning EOF from a
			// partial paste makes Readline 8.2 use an unterminated buffer.
			rl_abort(0, 0);
		}
		return byte;
	}

	static int input_available() {
		if (stopped() || input_error || input_offset < input_size) {
			return 1;
		}
		// Honor Readline's key-sequence deadline while exposing our read-ahead.
		int timeout = rl_set_keyboard_input_timeout(-1);
		timespec deadline{timeout / 1000000, (timeout % 1000000) * 1000L};
		pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {wake_fd, POLLIN, 0}};
		int ready = ppoll(fds, 2, &deadline, nullptr);
		return stopped() || (ready > 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)));
	}

	static void accept_line(char *line) {
		accepted_line = line;
		line_ready = true;
		// The callback loop continues while a macro or key is pending, even
		// after handler removal. Pause them until the next prompt is installed.
		pending_input = rl_pending_input;
		rl_clear_pending_input();
		RL_UNSETSTATE(RL_STATE_MACROINPUT);
		remove_callback();
	}

	static int submit_line(int count, int key) {
		bool more = multiline_input;
		if (rl_end) {
			char last = rl_line_buffer[rl_end - 1];
			if (last == '/' || last == '\\') {
				rl_delete_text(rl_end - 1, rl_end);
				rl_point = std::min(rl_point, rl_end);
				more = last == '\\' && !more;
			}
		}
		if (!more) {
			return rl_newline(count, key);
		}
		if (rl_end >= static_cast<int>(INPUT_LIMIT - 1)) {
			input_error = EFBIG;
			return rl_newline(count, key);
		}
		rl_point = rl_end;
		rl_insert_text("\n");
		return 0;
	}

	static char *complete_entry(const char *, int state) {
		if (state < 0 || static_cast<size_t>(state) >= completion_matches.size()) {
			return nullptr;
		}
		const auto &[candidate, end] = completion_matches[state];
		return strndup(candidate.c_str(), end);
	}

	static char **complete(const char *text, int start, int end) {
		rl_attempted_completion_over = 1;
		rl_completion_suppress_append = 1;
		rl_completion_suppress_quote = 1;
		rl_filename_completion_desired = 0;
		if (!completion_cb || start || end < 0) {
			return nullptr;
		}
		completion_matches = completion_cb(rl_line_buffer, static_cast<size_t>(end));
		completion_matches.erase(std::remove_if(completion_matches.begin(), completion_matches.end(), [](const auto &candidate) {
			return !candidate.second || candidate.second > candidate.first.size() || candidate.first.size() >= INPUT_LIMIT ||
			       candidate.first.find('\0') != std::string::npos ||
			       (candidate.second < candidate.first.size() && (static_cast<unsigned char>(candidate.first[candidate.second]) & 0xc0) == 0x80);
		}), completion_matches.end());
		if (completion_matches.empty()) {
			return nullptr;
		}
		std::string_view line(rl_line_buffer, static_cast<size_t>(rl_end));
		const auto &[first, cursor] = completion_matches.front();
		std::string_view suffix = std::string_view(first).substr(cursor);
		if (suffix.size() > line.size() || line.substr(line.size() - suffix.size()) != suffix ||
		    std::any_of(completion_matches.begin(), completion_matches.end(), [suffix](const auto &candidate) {
			return std::string_view(candidate.first).substr(candidate.second) != suffix;
		    })) {
			completion_matches.clear();
			return nullptr;
		}
		char **matches = rl_completion_matches(text, complete_entry);
		if (matches && rl_completion_type == '?' && !matches[1]) {
			// Readline otherwise inserts a unique match after a failed attempt,
			// even for possible-completions. Keep this request strictly a list.
			char **listing = static_cast<char **>(calloc(3, sizeof *listing));
			char *prefix = strdup("");
			if (listing && prefix) {
				listing[0] = prefix;
				listing[1] = matches[0];
				free(matches);
				matches = listing;
			} else {
				free(listing);
				listing = nullptr;
				free(prefix);
				prefix = nullptr;
				free(matches[0]);
				matches[0] = nullptr;
				free(matches);
				matches = nullptr;
				input_error = ENOMEM;
			}
		}
		if (matches && rl_completion_type != '?') {
			// Native completion replaces [start, rl_point). Consume the stale
			// token tail as well, leaving Readline in charge of undo and menus.
			// Listing candidates alone must not move the editing cursor.
			rl_point = static_cast<int>(line.size() - suffix.size());
		}
		completion_matches.clear();
		return matches;
	}

	terminal::terminal(bool use_simple_io, bool color, volatile sig_atomic_t *stop, int &error, int wake) {
		error = 0;
		if (initialized) {
			error = EBUSY;
			return;
		}
		simple_io = use_simple_io || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO);
		advanced_display = color && isatty(STDOUT_FILENO);
		if (!simple_io) {
			if (tcgetattr(STDIN_FILENO, &initial_state) < 0) {
				error = errno;
				return;
			}
			setlocale(LC_CTYPE, "");
			rl_readline_name = "askai-cli";
			rl_catch_signals = rl_catch_sigwinch = 0;
			rl_instream = stdin;
			rl_outstream = out;
			rl_initialize();
			rl_add_defun("askai-accept-line", submit_line, -1);
			for (const char *name : {"emacs-standard", "vi-insertion", "vi-move"}) {
				Keymap map = rl_get_keymap_by_name(name);
				if (!map) {
					continue;
				}
				rl_bind_keyseq_in_map("\033[5~", rl_history_search_backward, map);
				rl_bind_keyseq_in_map("\033[6~", rl_history_search_forward, map);
				rl_bind_keyseq_in_map("\033[1;5D", rl_backward_word, map);
				rl_bind_keyseq_in_map("\033[1;5C", rl_forward_word, map);
				rl_bind_key_in_map('\r', submit_line, map);
				rl_bind_key_in_map('\n', submit_line, map);
			}
			rl_variable_bind("enable-bracketed-paste", "on");
			// Application defaults precede user bindings, including $if askai-cli.
			(void)rl_read_init_file(nullptr);
			rl_getc_function = readline_getc;
			rl_input_available_hook = input_available;
			rl_attempted_completion_function = complete;
			rl_completer_word_break_characters = "";
			rl_completer_quote_characters = "";
			using_history();
			stifle_history(256);
		}
		interrupted = stop;
		wake_fd = wake;
		input_eof = false;
		input_offset = input_size = 0;
		initialized = active = true;
	}

	terminal::~terminal() {
		if (!active) {
			return;
		}
		remove_callback();
		free(accepted_line);
		accepted_line = nullptr;
		set_display(DISPLAY_TYPE_RESET);
		if (!simple_io) {
			clear_history();
			rl_getc_function = rl_getc;
			rl_input_available_hook = nullptr;
			rl_attempted_completion_function = nullptr;
			while (tcsetattr(STDIN_FILENO, TCSANOW, &initial_state) < 0 && errno == EINTR) {}
		}
		completion_matches.clear();
		completion_cb = nullptr;
		interrupted = nullptr;
		wake_fd = -1;
		initialized = false;
	}

	void set_display(display_type display) {
		if (advanced_display && current_display != display) {
			switch (display) {
			case DISPLAY_TYPE_RESET:      fputs(ANSI_COLOR_RESET, out); break;
			case DISPLAY_TYPE_INFO:       fputs(ANSI_COLOR_MAGENTA, out); break;
			case DISPLAY_TYPE_PROMPT:     fputs(ANSI_COLOR_YELLOW, out); break;
			case DISPLAY_TYPE_REASONING:  fputs(ANSI_COLOR_GRAY, out); break;
			case DISPLAY_TYPE_USER_INPUT: fputs(ANSI_BOLD ANSI_COLOR_GREEN, out); break;
			case DISPLAY_TYPE_ERROR:      fputs(ANSI_BOLD ANSI_COLOR_RED, out); break;
			}
			current_display = display;
			fflush(out);
		}
	}

	static void remember(std::string_view text) {
		if (!text.empty() && text.back() == '\n') {
			text.remove_suffix(1);
		}
		if (text.empty() || text.size() > HISTORY_LIMIT) {
			return;
		}
		HIST_ENTRY *last = history_get(history_base + history_length - 1);
		if (last && text == last->line) {
			return;
		}
		while (history_length && static_cast<size_t>(history_total_bytes()) + text.size() > HISTORY_LIMIT) {
			free_history_entry(remove_history(0));
		}
		add_history(std::string(text).c_str());
	}

	static void read_simple(std::string &text, bool multiline, const char *prompt) {
		if (prompt) {
			fputs(prompt, out);
		}
		flush();
		bool more;
		do {
			size_t start = text.size();
			int byte;
			while ((byte = input_byte()) != EOF && byte != '\n') {
				if (text.size() >= INPUT_LIMIT - 1) {
					input_error = EFBIG;
					return;
				}
				text.push_back(static_cast<char>(byte));
			}
			if (input_error || (input_eof && text.size() == start)) {
				return;
			}
			if (text.size() > start && text.back() == '\r') {
				text.pop_back();
			}
			more = multiline;
			if (text.size() > start) {
				char last = text.back();
				if (last == '/' || last == '\\') {
					text.pop_back();
					more = last == '\\' && !more;
				}
			}
			if (text.size() == INPUT_LIMIT) {
				input_error = EFBIG;
				return;
			}
			text.push_back('\n');
		} while (more && !input_eof);
	}

	int read_input(std::string &text, bool multiline, bool &eof, const char *prompt) {
		text.clear();
		eof = false;
		input_error = 0;
		if (!initialized) {
			return EINVAL;
		}
		set_display(DISPLAY_TYPE_USER_INPUT);
		if (simple_io) {
			read_simple(text, multiline, prompt);
		} else {
			multiline_input = multiline;
			line_ready = false;
			callback_active = true;
			rl_callback_handler_install(prompt ? prompt : "", accept_line);
			if (rl_executing_macro) {
				RL_SETSTATE(RL_STATE_MACROINPUT);
			}
			if (pending_input) {
				rl_execute_next(pending_input);
				pending_input = 0;
			}
			while (!line_ready && !input_error) {
				input_error = input_offset < input_size || rl_pending_input || RL_ISSTATE(RL_STATE_MACROINPUT) ? 0 : wait_input();
				if (input_error) {
					break;
				}
				if (wake_fd < 0) {
					rl_resize_terminal();
				}
				rl_callback_read_char();
				if (stopped()) {
					input_error = EINTR;
				} else if (rl_end >= static_cast<int>(INPUT_LIMIT)) {
					input_error = EFBIG;
				}
			}
			if (input_error) {
				rl_free_line_state();
				rl_callback_sigcleanup();
			}
			remove_callback();
			if (accepted_line && !input_error) {
				text = accepted_line;
				if (text.size() >= INPUT_LIMIT) {
					input_error = EFBIG;
				} else {
					text.push_back('\n');
					remember(text);
				}
			} else if (!input_error) {
				input_eof = true;
			}
			free(accepted_line);
			accepted_line = nullptr;
		}
		if (input_error) {
			text.clear();
			if (!simple_io) {
				fputc('\n', out);
			}
		}
		eof = input_eof && text.empty();
		set_display(DISPLAY_TYPE_RESET);
		return input_error;
	}

	struct output_state {
		unsigned char pending[4]{};
		unsigned used = 0;
		unsigned expected = 0;
		char32_t value = 0;
		char32_t minimum = 0;
	};

	static void write_to(FILE *destination, std::string_view text, output_state &state) {
		// UTF-8 code points can straddle provider deltas. C1 controls are unsafe too.
		auto &[pending, used, expected, value, minimum] = state;
		for (unsigned char byte : text) {
			if (used) {
				if ((byte & 0xc0) == 0x80) {
					pending[used++] = byte;
					value = (value << 6) | (byte & 0x3f);
					if (used == expected) {
						if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
						    (value >= 0x80 && value <= 0x9f)) {
							fputc('?', destination);
						} else {
							fwrite(pending, 1, used, destination);
						}
						used = 0;
					}
					continue;
				}
				fputc('?', destination);
				used = 0;
			}
			if (byte < 0x80) {
				fputc((byte >= 0x20 && byte != 0x7f) || byte == '\n' || byte == '\t' ? byte : '?', destination);
			} else {
				expected = byte >= 0xc2 && byte <= 0xdf ? 2 :
				           byte >= 0xe0 && byte <= 0xef ? 3 :
				           byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
				if (!expected) {
					fputc('?', destination);
					continue;
				}
				pending[0] = byte;
				used = 1;
				value = byte & (0x7f >> expected);
				minimum = expected == 2 ? 0x80 : expected == 3 ? 0x800 : 0x10000;
			}
		}
	}

	void write(std::string_view text) {
		static output_state state;
		write_to(out, text, state);
	}

	void write_error(std::string_view text) {
		static output_state state;
		write_to(stderr, text, state);
		fflush(stderr);
	}

	void set_completion_callback(completion_callback cb) {
		completion_cb = std::move(cb);
	}

	void log(const char * fmt, ...) {
		va_list args;
		va_start(args, fmt);
		vfprintf(out, fmt, args);
		va_end(args);
	}

	void error(const char * fmt, ...) {
		va_list args;
		va_start(args, fmt);
		vfprintf(stderr, fmt, args);
		va_end(args);
	}

	void flush() {
		fflush(out);
	}
}
