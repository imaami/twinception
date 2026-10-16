/*
 * Adapted from llama.cpp common/console.h and tools/cli/cli-ui.h.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#pragma once

#include <csignal>
#include <functional>
#include <string>
#include <string_view>

#include "autocomplete.h"

namespace cli_console {

enum display_type {
	DISPLAY_TYPE_RESET,
	DISPLAY_TYPE_INFO,
	DISPLAY_TYPE_PROMPT,
	DISPLAY_TYPE_REASONING,
	DISPLAY_TYPE_USER_INPUT,
	DISPLAY_TYPE_ERROR,
};

/** Owns Readline and terminal state; wake_fd is an optional nonblocking signal pipe. */
struct terminal {
	terminal(bool simple_io, bool color, volatile sig_atomic_t *interrupted, int &error, int wake_fd = -1);
	~terminal();
	terminal(const terminal &) = delete;
	terminal &operator=(const terminal &) = delete;
private:
	bool active = false;
};

/** Candidates contain the complete replacement line and its byte cursor.
 * Their suffixes after the cursor must be identical and an unchanged suffix
 * of the input line. Readline replaces everything before that suffix. The
 * callback owns the memory and keeps it valid until its next invocation. */
struct completion_matches {
	const autocomplete_match *data = nullptr;
	size_t size = 0;
};
using completion_callback = std::function<completion_matches(std::string_view, size_t)>;
void set_completion_callback(completion_callback callback);

/** Returns 0, EINTR, or an input error. EOF is reported separately from an empty line. */
int read_input(std::string &text, bool multiline, bool &eof, const char *prompt = "\n> ");

void set_display(display_type display);
/** Writes text without executing terminal control sequences supplied by the peer. */
void write(std::string_view text);
/** Writes a diagnostic to stderr with the same control-character filtering. */
void write_error(std::string_view text);
void flush();

__attribute__((format(printf, 1, 2))) void log(const char *format, ...);
__attribute__((format(printf, 1, 2))) void error(const char *format, ...);

}
