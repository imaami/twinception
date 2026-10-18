/** @file Console-only driver for PTY tests, independent of MCP transport. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "autocomplete_file.h"
#include "autocomplete_fixture.h"
#include "cli_console.h"
#include "cli_loop.h"
#include "compat.h"
#include "dstr.h"

struct cli_input_driver {
	struct cli_loop loop;
	struct cli_loop_ref signals;
	struct cli_console *console;
	int stop;
};

static void cli_input_signal(struct cli_loop *loop, struct cli_loop_ref *ref, uint32_t events)
{
	(void)events;
	struct cli_input_driver *driver = container_of(loop, struct cli_input_driver, loop);
	struct signalfd_siginfo signal;
	while (read(ref->fd, &signal, sizeof signal) == sizeof signal) {
		if (signal.ssi_signo == SIGWINCH)
			(void)cli_console_resize(driver->console);
		else {
			driver->stop = (int)signal.ssi_signo;
			(void)cli_console_interrupt(driver->console, true);
		}
	}
}

static void cli_input_service(struct cli_loop *loop)
{
	struct cli_input_driver *driver = container_of(loop, struct cli_input_driver, loop);
	(void)cli_console_service(driver->console);
}

static int cli_input_record(int fd, char const *kind, struct cstr_view text)
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
	struct cstr_view remaining = cstr_view_dstr(&record);
	while (remaining.len) {
		ssize_t count = write(fd, remaining.data, remaining.len);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (!count)
			return EIO;
		remaining = cstr_view_slice(remaining, (size_t)count, SIZE_MAX);
	}
	return 0;
}

struct cli_input {
	struct autocomplete *catalog;
	struct autocomplete_file *files;
};

static int cli_input_complete(void *context, struct cstr_view line, size_t cursor,
                              struct autocomplete_match const **matches, size_t *count)
{
	struct cli_input *input = context;
	int e = autocomplete_complete(input->catalog, line, cursor, matches, count);
	static struct cstr_view const commands[] = {{"/read ", sizeof "/read " - 1}};
	if (!e && !*count)
		e = autocomplete_file_complete(input->files, line, cursor, commands,
		                                sizeof commands / sizeof *commands, matches, count);
	if (e) {
		*matches = nullptr;
		*count = 0;
	}
	return e;
}

static int cli_input_run(struct cli_input_driver *driver, int records, bool multiline)
{
	for (;;) {
		dstr_auto text = dstr(nullptr);
		bool eof;
		int e = cli_console_read(driver->console, &text, multiline, &eof, "test> ");
		if (e == EINTR) {
			dstr_auto signum = dstr(nullptr);
			if (!dstr_printf(&signum, "%d", driver->stop))
				return ENOMEM;
			return cli_input_record(records, "STOP ", cstr_view_dstr(&signum));
		}
		if (e || eof)
			return e ? e : cli_input_record(records, "EOF", (struct cstr_view){});
		e = cli_input_record(records, "LINE ", cstr_view_dstr(&text));
		if (e)
			return e;
	}
}

int main(int argc, char **argv)
{
	(void)setlocale(LC_CTYPE, "");
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
	struct cli_input_driver driver = {.signals.fd = -1};
	sigset_t mask;
	int e = sigemptyset(&mask) || sigaddset(&mask, SIGINT) ||
	        sigaddset(&mask, SIGTERM) || sigaddset(&mask, SIGWINCH) ||
	        sigprocmask(SIG_BLOCK, &mask, nullptr) ? errno : 0;
	int signal_fd = e ? -1 : signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	if (signal_fd < 0 && !e)
		e = errno;
	if (!e)
		e = cli_loop_init(&driver.loop);
	struct cli_input input = {};
	if (!e)
		driver.console = cli_console_create(simple, false, &driver.loop, nullptr, &e);
	if (!e)
		e = cli_loop_watch(&driver.loop, &driver.signals, signal_fd, EPOLLIN, cli_input_signal);
	driver.loop.after_turn = cli_input_service;
	if (!e)
		input.catalog = autocomplete_create(&e);
	if (!e)
		input.files = autocomplete_file_create(&e);
	if (!e && completion)
		e = autocomplete_fixture(input.catalog);
	if (!e) {
		cli_console_set_completion(driver.console, cli_input_complete, &input);
		e = cli_input_run(&driver, records, multiline);
	}
	cli_console_destroy(&driver.console);
	cli_loop_fini(&driver.loop);
	autocomplete_file_destroy(&input.files);
	autocomplete_destroy(&input.catalog);
	if (signal_fd >= 0)
		(void)close(signal_fd);
	signal_fd = -1;
	(void)close(records);
	records = -1;
	return e ? 1 : 0;
}
