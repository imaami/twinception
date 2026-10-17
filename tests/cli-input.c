/** @file Console-only driver for PTY tests, independent of MCP transport. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "autocomplete-file.h"
#include "autocomplete-fixture.h"
#include "cli-console.h"

static volatile sig_atomic_t cli_input_stop;
static volatile sig_atomic_t cli_input_wake = -1;

static void cli_input_signal(int signal)
{
	int saved = errno;
	if (signal != SIGWINCH)
		cli_input_stop = signal;
	unsigned char byte = (unsigned char)signal;
	(void)write(cli_input_wake, &byte, 1);
	errno = saved;
}

static int cli_input_record(int fd, char const *kind, struct cli_text text)
{
	static char const digits[] = "0123456789abcdef";
	dstr_auto record = dstr(nullptr);
	if (!dstr_cat(&record, kind) || !dstr_reserve(&record, 2 * text.len + 1))
		return ENOMEM;
	for (size_t i = 0; i < text.len; ++i) {
		unsigned char byte = (unsigned char)text.data[i];
		char pair[] = {digits[byte >> 4], digits[byte & 15]};
		if (!dstr_cat_n(&record, pair, sizeof pair))
			return ENOMEM;
	}
	if (!dstr_cat(&record, "\n"))
		return ENOMEM;
	struct cli_text remaining = cli_text_dstr(&record);
	while (remaining.len) {
		ssize_t count = write(fd, remaining.data, remaining.len);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (!count)
			return EIO;
		remaining = cli_text_slice(remaining, (size_t)count, SIZE_MAX);
	}
	return 0;
}

struct cli_input {
	struct autocomplete *catalog;
	struct autocomplete_file *files;
};

static int cli_input_complete(void *context, struct cli_text line, size_t cursor,
                              struct autocomplete_match const **matches, size_t *count)
{
	struct cli_input *input = context;
	struct autocomplete_text text = {line.data, line.len};
	int e = autocomplete_complete(input->catalog, text, cursor, matches, count);
	static struct autocomplete_text const commands[] = {{"/read ", sizeof "/read " - 1}};
	if (!e && !*count)
		e = autocomplete_file_complete(input->files, text, cursor, commands,
		                                sizeof commands / sizeof *commands, matches, count);
	if (e) {
		*matches = nullptr;
		*count = 0;
	}
	return e;
}

static int cli_input_run(struct cli_console *console, int records, bool multiline)
{
	for (;;) {
		dstr_auto text = dstr(nullptr);
		bool eof;
		int e = cli_console_read(console, &text, multiline, &eof, "test> ");
		if (e == EINTR) {
			dstr_auto signum = dstr(nullptr);
			if (!dstr_printf(&signum, "%d", (int)cli_input_stop))
				return ENOMEM;
			return cli_input_record(records, "STOP ", cli_text_dstr(&signum));
		}
		if (e || eof)
			return e ? e : cli_input_record(records, "EOF", (struct cli_text){});
		e = cli_input_record(records, "LINE ", cli_text_dstr(&text));
		if (e)
			return e;
	}
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	errno = 0;
	char *end = nullptr;
	long descriptor = strtol(argv[1], &end, 10);
	if (errno || end == argv[1] || *end || descriptor < 0 || descriptor > INT_MAX)
		return 2;
	int records = (int)descriptor;
	bool simple = false, multiline = false, completion = false;
	for (int i = 2; i < argc; ++i) {
		if (!strcmp(argv[i], "--simple"))
			simple = true;
		else if (!strcmp(argv[i], "--multiline"))
			multiline = true;
		else if (!strcmp(argv[i], "--catalog"))
			completion = true;
		else
			return 2;
	}
	int wake[2] = {-1, -1};
	if (pipe2(wake, O_CLOEXEC | O_NONBLOCK))
		return 1;
	cli_input_wake = wake[1];
	struct sigaction action = {.sa_handler = cli_input_signal};
	int e = sigemptyset(&action.sa_mask) || sigaction(SIGINT, &action, nullptr) ||
	        sigaction(SIGTERM, &action, nullptr) || sigaction(SIGWINCH, &action, nullptr) ? errno : 0;
	struct cli_console *console = nullptr;
	struct cli_input input = {};
	if (!e)
		console = cli_console_create(simple, false, &cli_input_stop, wake[0], &e);
	if (!e)
		input.catalog = autocomplete_create(&e);
	if (!e)
		input.files = autocomplete_file_create(&e);
	if (!e && completion)
		e = autocomplete_fixture(input.catalog);
	if (!e) {
		cli_console_set_completion(console, cli_input_complete, &input);
		e = cli_input_run(console, records, multiline);
	}
	cli_console_destroy(&console);
	autocomplete_file_destroy(&input.files);
	autocomplete_destroy(&input.catalog);
	cli_input_wake = -1;
	(void)close(wake[0]);
	wake[0] = -1;
	(void)close(wake[1]);
	wake[1] = -1;
	(void)close(records);
	records = -1;
	return e ? 1 : 0;
}
