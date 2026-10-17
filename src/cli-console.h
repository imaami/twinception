/*
 * Adapted from llama.cpp common/console.h.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#ifndef TWINCEPTION_SRC_CLI_CONSOLE_H_
#define TWINCEPTION_SRC_CLI_CONSOLE_H_

#include <signal.h>

#include "autocomplete.h"
#include "cli-text.h"

struct cli_console;

enum cli_console_display {
	CLI_CONSOLE_RESET,
	CLI_CONSOLE_INFO,
	CLI_CONSOLE_PROMPT,
	CLI_CONSOLE_REASONING,
	CLI_CONSOLE_USER_INPUT,
	CLI_CONSOLE_ERROR,
};

/** Readline supports one active console. wake_fd is a borrowed nonblocking
 * signal pipe, or -1. The interrupt flag must outlive the console. */
struct cli_console *cli_console_create(bool simple, bool color,
                                      volatile sig_atomic_t *interrupted,
                                      int wake_fd, int *e);
void cli_console_destroy(struct cli_console **console);

/** Candidates are borrowed until the next callback. Their suffixes after
 * the cursor must be identical, unchanged suffixes of the input line. */
typedef int cli_console_complete_fn(void *context, struct cli_text line,
                                    size_t cursor,
                                    struct autocomplete_match const **matches,
                                    size_t *count);
void cli_console_set_completion(struct cli_console *console,
                                cli_console_complete_fn *callback, void *context);

int cli_console_remember(struct cli_console *console, struct cli_text text);
/** EOF is distinct from an empty submitted line. On error text is empty. */
int cli_console_read(struct cli_console *console, struct dstr *text,
                     bool multiline, bool *eof, char const *prompt);

int cli_console_set_display(struct cli_console *console, enum cli_console_display display);
/** Peer-supplied terminal controls are filtered, including UTF-8 C1 controls. */
int cli_console_write(struct cli_console *console, struct cli_text text);
int cli_console_write_error(struct cli_text text);
int cli_console_flush(struct cli_console *console);

#endif /* TWINCEPTION_SRC_CLI_CONSOLE_H_ */
