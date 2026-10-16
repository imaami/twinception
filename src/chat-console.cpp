/*
 * Terminal editor adapted from llama.cpp common/console.cpp, commit
 * b76b13379d3b7d67ab0bc424516bb6fd2522bd8e.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */

#include <cassert>
#include <cctype>
#include <cerrno>
#include <clocale>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwctype>
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

	static bool         advanced_display = false;
	static bool         simple_io        = true;
	static display_type current_display  = DISPLAY_TYPE_RESET;
	static FILE *       out              = stdout;
	static termios      initial_state{};
	static bool         initialized      = false;
	static bool         input_eof        = false;
	static int          input_error      = 0;
	static unsigned char input_buffer[4096];
	static size_t input_offset = 0;
	static size_t input_size = 0;
	static volatile sig_atomic_t *interrupted = nullptr;
	static completion_callback completion_cb = nullptr;
	static constexpr char32_t INPUT_END = UINT32_MAX;
	static constexpr size_t INPUT_LIMIT = 16 * 1024 * 1024;

	terminal::terminal(bool use_simple_io, bool color, volatile sig_atomic_t *stop, int &error) {
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
			struct termios new_termios = initial_state;
			new_termios.c_lflag &= ~(ICANON | ECHO);
			new_termios.c_cc[VMIN] = 1;
			new_termios.c_cc[VTIME] = 0;
			if (tcsetattr(STDIN_FILENO, TCSANOW, &new_termios) < 0) {
				error = errno;
				return;
			}
		}
		setlocale(LC_CTYPE, "");
		interrupted = stop;
		input_eof = false;
		input_offset = input_size = 0;
		initialized = active = true;
	}

	terminal::~terminal() {
		if (!active) {
			return;
		}
		set_display(DISPLAY_TYPE_RESET);
		if (!simple_io) {
			while (tcsetattr(STDIN_FILENO, TCSANOW, &initial_state) < 0 && errno == EINTR) {}
		}
		completion_cb = nullptr;
		interrupted = nullptr;
		initialized = false;
	}

	//
	// Display and IO
	//

	// Keep track of current display and only emit ANSI code if it changes
	void set_display(display_type display) {
		if (advanced_display && current_display != display) {
			switch(display) {
				case DISPLAY_TYPE_RESET:
					fprintf(out, ANSI_COLOR_RESET);
					break;
				case DISPLAY_TYPE_INFO:
					fprintf(out, ANSI_COLOR_MAGENTA);
					break;
				case DISPLAY_TYPE_PROMPT:
					fprintf(out, ANSI_COLOR_YELLOW);
					break;
				case DISPLAY_TYPE_REASONING:
					fprintf(out, ANSI_COLOR_GRAY);
					break;
				case DISPLAY_TYPE_USER_INPUT:
					fprintf(out, ANSI_BOLD ANSI_COLOR_GREEN);
					break;
				case DISPLAY_TYPE_ERROR:
					fprintf(out, ANSI_BOLD ANSI_COLOR_RED);
			}
			current_display = display;
			fflush(out);
		}
	}

	static int read_byte(unsigned char &byte) {
		if (interrupted && *interrupted) {
			input_error = EINTR;
			return -1;
		}
		if (input_eof) {
			return 0;
		}
		if (input_offset < input_size) {
			byte = input_buffer[input_offset++];
			return 1;
		}
		ssize_t n = read(STDIN_FILENO, input_buffer, sizeof(input_buffer));
		if (n < 0) {
			input_error = errno;
			return -1;
		}
		if (!n) {
			input_eof = true;
			return 0;
		}
		input_size = static_cast<size_t>(n);
		input_offset = 1;
		byte = input_buffer[0];
		return 1;
	}

	static char32_t getchar32() {
		unsigned char byte;
		if (read_byte(byte) <= 0) {
			return INPUT_END;
		}
		if (byte < 0x80) {
			return byte;
		}
		unsigned count = byte >= 0xc2 && byte <= 0xdf ? 1 :
		                 byte >= 0xe0 && byte <= 0xef ? 2 :
		                 byte >= 0xf0 && byte <= 0xf4 ? 3 : 0;
		if (!count) {
			return 0xfffd;
		}
		char32_t value = byte & (0x7f >> (count + 1));
		char32_t minimum = count == 1 ? 0x80 : count == 2 ? 0x800 : 0x10000;
		for (unsigned i = 0; i < count; ++i) {
			if (read_byte(byte) <= 0) {
				return INPUT_END;
			}
			if ((byte & 0xc0) != 0x80) {
				return 0xfffd;
			}
			value = (value << 6) | (byte & 0x3f);
		}
		return value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ? 0xfffd : value;
	}

	static void pop_cursor() {
		putc('\b', out);
	}

	static int estimate_width(char32_t codepoint) {
		return wcwidth(codepoint);
	}

	static int put_codepoint(const char *utf8_codepoint, size_t length, int expected_width) {
		if (expected_width < 0) {
			fputc('?', out);
			return 1;
		}
		fwrite(utf8_codepoint, length, 1, out);
		return expected_width;
	}

	static void replace_last(char ch) {
		fprintf(out, "\b%c", ch);
	}

	static char32_t decode_utf8(const std::string & input, size_t pos, size_t & advance) {
		unsigned char c = static_cast<unsigned char>(input[pos]);
		if ((c & 0x80u) == 0u) {
			advance = 1;
			return c;
		}
		if ((c & 0xE0u) == 0xC0u && pos + 1 < input.size()) {
			unsigned char c1 = static_cast<unsigned char>(input[pos + 1]);
			if ((c1 & 0xC0u) != 0x80u) {
				advance = 1;
				return 0xFFFD;
			}
			advance = 2;
			return ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(input[pos + 1]) & 0x3Fu);
		}
		if ((c & 0xF0u) == 0xE0u && pos + 2 < input.size()) {
			unsigned char c1 = static_cast<unsigned char>(input[pos + 1]);
			unsigned char c2 = static_cast<unsigned char>(input[pos + 2]);
			if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u) {
				advance = 1;
				return 0xFFFD;
			}
			advance = 3;
			return ((c & 0x0Fu) << 12) |
				   ((static_cast<unsigned char>(input[pos + 1]) & 0x3Fu) << 6) |
				   (static_cast<unsigned char>(input[pos + 2]) & 0x3Fu);
		}
		if ((c & 0xF8u) == 0xF0u && pos + 3 < input.size()) {
			unsigned char c1 = static_cast<unsigned char>(input[pos + 1]);
			unsigned char c2 = static_cast<unsigned char>(input[pos + 2]);
			unsigned char c3 = static_cast<unsigned char>(input[pos + 3]);
			if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u || (c3 & 0xC0u) != 0x80u) {
				advance = 1;
				return 0xFFFD;
			}
			advance = 4;
			return ((c & 0x07u) << 18) |
				   ((static_cast<unsigned char>(input[pos + 1]) & 0x3Fu) << 12) |
				   ((static_cast<unsigned char>(input[pos + 2]) & 0x3Fu) << 6) |
				   (static_cast<unsigned char>(input[pos + 3]) & 0x3Fu);
		}

		advance = 1;
		return 0xFFFD; // replacement character for invalid input
	}

	static void append_utf8(char32_t ch, std::string & out) {
		if (ch <= 0x7F) {
			out.push_back(static_cast<unsigned char>(ch));
		} else if (ch <= 0x7FF) {
			out.push_back(static_cast<unsigned char>(0xC0 | ((ch >> 6) & 0x1F)));
			out.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
		} else if (ch <= 0xFFFF) {
			out.push_back(static_cast<unsigned char>(0xE0 | ((ch >> 12) & 0x0F)));
			out.push_back(static_cast<unsigned char>(0x80 | ((ch >> 6) & 0x3F)));
			out.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
		} else if (ch <= 0x10FFFF) {
			out.push_back(static_cast<unsigned char>(0xF0 | ((ch >> 18) & 0x07)));
			out.push_back(static_cast<unsigned char>(0x80 | ((ch >> 12) & 0x3F)));
			out.push_back(static_cast<unsigned char>(0x80 | ((ch >> 6) & 0x3F)));
			out.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
		} else {
			// Invalid Unicode code point
		}
	}

	// Helper function to remove the last UTF-8 character from a string
	static size_t prev_utf8_char_pos(const std::string & line, size_t pos) {
		if (pos == 0) return 0;
		pos--;
		while (pos > 0 && (line[pos] & 0xC0) == 0x80) {
			pos--;
		}
		return pos;
	}

	static size_t next_utf8_char_pos(const std::string & line, size_t pos) {
		if (pos >= line.length()) return line.length();
		pos++;
		while (pos < line.length() && (line[pos] & 0xC0) == 0x80) {
			pos++;
		}
		return pos;
	}

	static void move_cursor(int delta);
	static void move_word_left(size_t & char_pos, size_t & byte_pos, const std::vector<int> & widths, const std::string & line);
	static void move_word_right(size_t & char_pos, size_t & byte_pos, const std::vector<int> & widths, const std::string & line);
	static void move_to_line_start(size_t & char_pos, size_t & byte_pos, const std::vector<int> & widths);
	static void move_to_line_end(size_t & char_pos, size_t & byte_pos, const std::vector<int> & widths, const std::string & line);

	static void delete_at_cursor(std::string & line, std::vector<int> & widths, size_t & char_pos, size_t & byte_pos) {
		if (char_pos >= widths.size()) {
			return;
		}

		size_t next_pos = next_utf8_char_pos(line, byte_pos);
		int w = widths[char_pos];
		size_t char_len = next_pos - byte_pos;

		line.erase(byte_pos, char_len);
		widths.erase(widths.begin() + char_pos);

		size_t p = byte_pos;
		int tail_width = 0;
		for (size_t i = char_pos; i < widths.size(); ++i) {
			size_t following = next_utf8_char_pos(line, p);
			put_codepoint(line.c_str() + p, following - p, widths[i]);
			tail_width += widths[i];
			p = following;
		}

		for (int i = 0; i < w; ++i) {
			fputc(' ', out);
		}

		move_cursor(-(tail_width + w));
	}

	static void clear_current_line(const std::vector<int> & widths) {
		int total_width = 0;
		for (int w : widths) {
			total_width += (w > 0 ? w : 1);
		}

		if (total_width > 0) {
			std::string spaces(total_width, ' ');
			fwrite(spaces.c_str(), 1, total_width, out);
			move_cursor(-total_width);
		}
	}

	static void set_line_contents(std::string new_line, std::string & line, std::vector<int> & widths, size_t & char_pos,
	                              size_t & byte_pos, size_t cursor_byte_pos = SIZE_MAX) {
		move_to_line_start(char_pos, byte_pos, widths);
		clear_current_line(widths);

		line = std::move(new_line);
		widths.clear();
		byte_pos = 0;
		char_pos = 0;

		size_t idx = 0;
		int back_width = 0;
		while (idx < line.size()) {
			size_t advance = 0;
			char32_t cp = decode_utf8(line, idx, advance);
			int expected_width = estimate_width(cp);
			int real_width = put_codepoint(line.c_str() + idx, advance, expected_width);
			if (real_width < 0) real_width = 0;
			widths.push_back(real_width);
			idx += advance;
			if (cursor_byte_pos < idx) {
				back_width += real_width;
			} else {
				++char_pos;
				byte_pos = idx;
			}
		}
		if (cursor_byte_pos != SIZE_MAX) {
			move_cursor(-back_width);
		}
	}

	static void move_to_line_start(size_t & char_pos, size_t & byte_pos, const std::vector<int> & widths) {
		int back_width = 0;
		for (size_t i = 0; i < char_pos; ++i) {
			back_width += widths[i];
		}
		move_cursor(-back_width);
		char_pos = 0;
		byte_pos = 0;
	}

	static void move_to_line_end(size_t & char_pos, size_t & byte_pos, const std::vector<int> & widths, const std::string & line) {
		int forward_width = 0;
		for (size_t i = char_pos; i < widths.size(); ++i) {
			forward_width += widths[i];
		}
		move_cursor(forward_width);
		char_pos = widths.size();
		byte_pos = line.length();
	}

	static bool has_ctrl_modifier(const std::string & params) {
		size_t start = 0;
		while (start < params.size()) {
			size_t end = params.find(';', start);
			size_t len = (end == std::string::npos) ? params.size() - start : end - start;
			if (len == 1 && params[start] == '5') {
				return true;
			}

			if (end == std::string::npos) {
				break;
			}
			start = end + 1;
		}
		return false;
	}

	static bool is_space_codepoint(char32_t cp) {
		return std::iswspace(static_cast<wint_t>(cp)) != 0;
	}

	static void move_word_left(size_t & char_pos, size_t & byte_pos, const std::vector<int> & widths, const std::string & line) {
		if (char_pos == 0) {
			return;
		}

		size_t new_char_pos = char_pos;
		size_t new_byte_pos = byte_pos;
		int move_width = 0;

		while (new_char_pos > 0) {
			size_t prev_byte = prev_utf8_char_pos(line, new_byte_pos);
			size_t advance = 0;
			char32_t cp = decode_utf8(line, prev_byte, advance);
			if (!is_space_codepoint(cp)) {
				break;
			}
			move_width += widths[new_char_pos - 1];
			new_char_pos--;
			new_byte_pos = prev_byte;
		}

		while (new_char_pos > 0) {
			size_t prev_byte = prev_utf8_char_pos(line, new_byte_pos);
			size_t advance = 0;
			char32_t cp = decode_utf8(line, prev_byte, advance);
			if (is_space_codepoint(cp)) {
				break;
			}
			move_width += widths[new_char_pos - 1];
			new_char_pos--;
			new_byte_pos = prev_byte;
		}

		move_cursor(-move_width);
		char_pos = new_char_pos;
		byte_pos = new_byte_pos;
	}

	static void move_word_right(size_t & char_pos, size_t & byte_pos, const std::vector<int> & widths, const std::string & line) {
		if (char_pos >= widths.size()) {
			return;
		}

		size_t new_char_pos = char_pos;
		size_t new_byte_pos = byte_pos;
		int move_width = 0;

		while (new_char_pos < widths.size()) {
			size_t advance = 0;
			char32_t cp = decode_utf8(line, new_byte_pos, advance);
			if (!is_space_codepoint(cp)) {
				break;
			}
			move_width += widths[new_char_pos];
			new_char_pos++;
			new_byte_pos += advance;
		}

		while (new_char_pos < widths.size()) {
			size_t advance = 0;
			char32_t cp = decode_utf8(line, new_byte_pos, advance);
			if (is_space_codepoint(cp)) {
				break;
			}
			move_width += widths[new_char_pos];
			new_char_pos++;
			new_byte_pos += advance;
		}

		while (new_char_pos < widths.size()) {
			size_t advance = 0;
			char32_t cp = decode_utf8(line, new_byte_pos, advance);
			if (!is_space_codepoint(cp)) {
				break;
			}
			move_width += widths[new_char_pos];
			new_char_pos++;
			new_byte_pos += advance;
		}

		move_cursor(move_width);
		char_pos = new_char_pos;
		byte_pos = new_byte_pos;
	}

	static void move_cursor(int delta) {
		if (delta == 0) return;
		if (delta < 0) {
			for (int i = 0; i < -delta; i++) fprintf(out, "\b");
		} else {
			for (int i = 0; i < delta; i++) fprintf(out, "\033[C");
		}
	}

	struct history_state {
		std::vector<std::string> entries;
		size_t viewing_idx = SIZE_MAX;
		size_t bytes = 0;
		std::string backup_line; // current line before viewing history
		void add(std::string_view line) {
			end_viewing();
			if (line.empty() || line.size() > 1024 * 1024) {
				return;
			}
			// avoid duplicates with the last entry
			if (entries.empty() || entries.back() != line) {
				while (!entries.empty() && (entries.size() >= 256 || bytes + line.size() > 1024 * 1024)) {
					bytes -= entries.front().size();
					entries.erase(entries.begin());
				}
				entries.emplace_back(line);
				bytes += line.size();
			}
		}
		bool prev(std::string & cur_line) {
			if (entries.empty()) {
				return false;
			}
			if (viewing_idx == SIZE_MAX) {
				return false;
			}
			if (viewing_idx > 0) {
				viewing_idx--;
			}
			cur_line = entries[viewing_idx];
			return true;
		}
		bool next(std::string & cur_line) {
			if (entries.empty() || viewing_idx == SIZE_MAX) {
				return false;
			}
			viewing_idx++;
			if (viewing_idx >= entries.size()) {
				cur_line = backup_line;
				end_viewing();
			} else {
				cur_line = entries[viewing_idx];
			}
			return true;
		}
		void begin_viewing(const std::string & line) {
			backup_line = line;
			viewing_idx = entries.size();
		}
		void end_viewing() {
			viewing_idx = SIZE_MAX;
			backup_line.clear();
		}
		bool is_viewing() const {
			return viewing_idx != SIZE_MAX;
		}
	} history;

	static bool readline_advanced(std::string & line, bool multiline_input) {
		line.clear();
		std::vector<int> widths;
		bool is_special_char = false;
		bool end_of_stream = false;

		size_t byte_pos = 0; // current byte index
		size_t char_pos = 0; // current character index (one char can be multiple bytes)

		char32_t input_char;
		while (true) {
			assert(char_pos <= byte_pos);
			assert(char_pos <= widths.size());
			auto history_prev = [&]() {
				if (!history.is_viewing()) {
					history.begin_viewing(line);
				}
				std::string new_line;
				if (!history.prev(new_line)) {
					return;
				}
				set_line_contents(new_line, line, widths, char_pos, byte_pos);
			};
			auto history_next = [&]() {
				if (history.is_viewing()) {
					std::string new_line;
					if (!history.next(new_line)) {
						return;
					}
					set_line_contents(new_line, line, widths, char_pos, byte_pos);
				}
			};

			fflush(out); // Ensure all output is displayed before waiting for input
			input_char = getchar32();

			if (input_char == '\r' || input_char == '\n') {
				break;
			}

			if (completion_cb && input_char == '\t') {
				auto candidates = completion_cb(line, byte_pos);

				if (!candidates.empty()) {
					if (candidates[0].first.size() < INPUT_LIMIT) {
						set_line_contents(candidates[0].first, line, widths, char_pos, byte_pos, candidates[0].second);
					}
					continue;
				}
			}

			if (input_char == INPUT_END || input_char == 0x04 /* Ctrl+D */) {
				input_eof = !input_error;
				end_of_stream = true;
				break;
			}

			if (is_special_char) {
				replace_last(line.back());
				is_special_char = false;
			}

			if (input_char == '\033') { // Escape sequence
				char32_t code = getchar32();
				if (code == '[') {
					std::string params;
					while (true) {
						code = getchar32();
						if ((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z') || code == '~' || code == INPUT_END || params.size() == 64) {
							break;
						}
						params.push_back(static_cast<char>(code));
					}

					const bool ctrl_modifier = has_ctrl_modifier(params);

					if (code == 'D') { // left
						if (ctrl_modifier) {
							move_word_left(char_pos, byte_pos, widths, line);
						} else if (char_pos > 0) {
							int w = widths[char_pos - 1];
							move_cursor(-w);
							char_pos--;
							byte_pos = prev_utf8_char_pos(line, byte_pos);
						}
					} else if (code == 'C') { // right
						if (ctrl_modifier) {
							move_word_right(char_pos, byte_pos, widths, line);
						} else if (char_pos < widths.size()) {
							int w = widths[char_pos];
							move_cursor(w);
							char_pos++;
							byte_pos = next_utf8_char_pos(line, byte_pos);
						}
					} else if (code == 'H') { // home
						move_to_line_start(char_pos, byte_pos, widths);
					} else if (code == 'F') { // end
						move_to_line_end(char_pos, byte_pos, widths, line);
					} else if (code == 'A' || code == 'B') {
						// up/down
						if (code == 'A') {
							history_prev();
							is_special_char = false;
						} else if (code == 'B') {
							history_next();
							is_special_char = false;
						}
					} else if ((code == '~' || (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z')) && !params.empty()) {
						std::string digits;
						for (char ch : params) {
							if (ch == ';') {
								break;
							}
							if (std::isdigit(static_cast<unsigned char>(ch))) {
								digits.push_back(ch);
							}
						}

						if (code == '~') {
							if (digits == "1" || digits == "7") { // home
								move_to_line_start(char_pos, byte_pos, widths);
							} else if (digits == "4" || digits == "8") { // end
								move_to_line_end(char_pos, byte_pos, widths, line);
							} else if (digits == "3") { // delete
								delete_at_cursor(line, widths, char_pos, byte_pos);
							}
						}
					}
				} else if (code == 0x1B) {
					// Discard the rest of the escape sequence
					for (unsigned n = 0; n < 64 && (code = getchar32()) != INPUT_END; ++n) {
						if ((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z') || code == '~') {
							break;
						}
					}
				}
			} else if (input_char == 0x08 || input_char == 0x7F) { // Backspace
				if (char_pos > 0) {
					int w = widths[char_pos - 1];
					move_cursor(-w);
					char_pos--;
					size_t prev_pos = prev_utf8_char_pos(line, byte_pos);
					size_t char_len = byte_pos - prev_pos;
					byte_pos = prev_pos;

					// remove the character
					line.erase(byte_pos, char_len);
					widths.erase(widths.begin() + char_pos);

					// redraw tail
					size_t p = byte_pos;
					int tail_width = 0;
					for (size_t i = char_pos; i < widths.size(); ++i) {
						size_t next_p = next_utf8_char_pos(line, p);
						put_codepoint(line.c_str() + p, next_p - p, widths[i]);
						tail_width += widths[i];
						p = next_p;
					}

					// clear display
					for (int i = 0; i < w; ++i) {
						fputc(' ', out);
					}
					move_cursor(-(tail_width + w));
				}
			} else if (input_char >= 0x20) {
				// insert character
				std::string new_char_str;
				append_utf8(input_char, new_char_str);
				if (line.size() >= INPUT_LIMIT - new_char_str.size()) {
					input_error = EFBIG;
					break;
				}
				int w = estimate_width(input_char);

				if (char_pos == widths.size()) {
					// insert at the end
					line += new_char_str;
					int real_w = put_codepoint(new_char_str.c_str(), new_char_str.length(), w);
					if (real_w < 0) real_w = 0;
					widths.push_back(real_w);
					byte_pos += new_char_str.length();
					char_pos++;
				} else {
					// insert in middle
					line.insert(byte_pos, new_char_str);

					int real_w = put_codepoint(new_char_str.c_str(), new_char_str.length(), w);
					if (real_w < 0) real_w = 0;

					widths.insert(widths.begin() + char_pos, real_w);

					// print the tail
					size_t p = byte_pos + new_char_str.length();
					int tail_width = 0;
					for (size_t i = char_pos + 1; i < widths.size(); ++i) {
						size_t next_p = next_utf8_char_pos(line, p);
						put_codepoint(line.c_str() + p, next_p - p, widths[i]);
						tail_width += widths[i];
						p = next_p;
					}

					move_cursor(-tail_width);

					byte_pos += new_char_str.length();
					char_pos++;
				}
			}

			if (!line.empty() && (line.back() == '\\' || line.back() == '/')) {
				replace_last(line.back());
				is_special_char = true;
			}
		}

		if (input_error) {
			history.end_viewing();
			fputc('\n', out);
			return false;
		}
		bool has_more = multiline_input && !end_of_stream;
		if (is_special_char) {
			replace_last(' ');
			pop_cursor();

			char last = line.back();
			line.pop_back();
			if (last == '\\') {
				line += '\n';
				fputc('\n', out);
				has_more = !has_more && !end_of_stream;
			} else {
				// A separate slash terminates the accumulated input.
				if (line.length() == 1 && line.back() == ' ') {
					line.clear();
					pop_cursor();
				}
				line += '\n';
				has_more = false;
			}
		} else {
			if (end_of_stream) {
				has_more = false;
			} else {
				line += '\n';
				fputc('\n', out);
			}
		}

		if (!end_of_stream && !line.empty()) {
			// remove the trailing newline for history storage
			std::string_view hline = line;
			if (!line.empty() && line.back() == '\n') {
				hline.remove_suffix(1);
			}
			history.add(hline);
		}

		fflush(out);
		return has_more;
	}

	static bool readline_simple(std::string & line, bool multiline_input) {
		line.clear();
		unsigned char byte;
		while (read_byte(byte) > 0) {
			if (byte == '\n') {
				break;
			}
			if (line.size() == INPUT_LIMIT - 1) {
				input_error = EFBIG;
				return false;
			}
			line.push_back(static_cast<char>(byte));
		}
		if (input_error || (input_eof && line.empty())) {
			return false;
		}
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (!line.empty()) {
			char last = line.back();
			if (last == '/') { // Always return control on '/' symbol
				line.back() = '\n';
				return false;
			}
			if (last == '\\') { // '\\' changes the default action
				line.pop_back();
				multiline_input = !multiline_input;
			}
		}
		line += '\n';

		// By default, continue input if multiline_input is set
		return multiline_input && !input_eof;
	}

	int read_input(std::string &text, bool multiline, bool &eof, const char *prompt) {
		text.clear();
		eof = false;
		input_error = 0;
		if (!initialized) {
			return EINVAL;
		}
		set_display(DISPLAY_TYPE_USER_INPUT);
		if (prompt) {
			log("%s", prompt);
		}
		flush();
		bool more;
		do {
			std::string line;
			more = simple_io ? readline_simple(line, multiline) : readline_advanced(line, multiline);
			if (input_error) {
				text.clear();
				input_offset = input_size = 0;
				break;
			}
			if (line.size() > INPUT_LIMIT || text.size() > INPUT_LIMIT - line.size()) {
				input_error = EFBIG;
				text.clear();
				break;
			}
			text += line;
		} while (more);
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
		completion_cb = cb;
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
