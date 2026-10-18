/** @file Progress must not split reasoning or text deltas into fragments. */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "cli_console.h"
#include "cli_loop.h"
#include "cli_ui.h"

static void
cli_ui_test_colors (void)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
	assert(master >= 0);
	assert(!grantpt(master));
	assert(!unlockpt(master));
	char *path = ptsname(master);
	assert(path);
	int slave = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	assert(slave >= 0);
	struct termios terminal;
	assert(!tcgetattr(slave, &terminal));
	terminal.c_oflag &= ~(tcflag_t)OPOST;
	assert(!tcsetattr(slave, TCSANOW, &terminal));
	assert(!fflush(stdout));
	int saved = dup(STDOUT_FILENO);
	assert(saved >= 0);
	assert(dup2(slave, STDOUT_FILENO) >= 0);
	struct cli_loop loop;
	assert(!cli_loop_init(&loop));
	int e;
	struct cli_console *console = cli_console_create(true, true, &loop, nullptr, &e);
	assert(console && !e);
	struct cli_ui display = cli_ui(console);
	assert(!cli_ui_push(&display, true, CSTR_VIEW("first thought\n")));
	assert(!cli_ui_message(console, CSTR_VIEW("local command output")));
	assert(!cli_ui_push(&display, true, CSTR_VIEW("continued thought\n")));
	assert(!cli_ui_info(console, CSTR_VIEW("local status")));
	cli_ui_fini(&display);
	assert(!cli_console_pending(console));
	cli_console_destroy(&console);
	cli_loop_fini(&loop);
	assert(dup2(saved, STDOUT_FILENO) >= 0);
	assert(!close(saved));
	saved = -1;
	dstr_auto output = dstr(nullptr);
	char block[512];
	for (;;) {
		ssize_t count = read(master, block, sizeof block);
		if (count < 0) {
			assert(errno == EAGAIN);
			break;
		}
		assert(count > 0);
		assert(dstr_cat_n(&output, block, (size_t)count));
	}
	assert(!close(slave));
	slave = -1;
	assert(!close(master));
	master = -1;
	assert(!strcmp(dstr_get(&output),
	               "\033[0;90m[Start thinking]\n\nfirst thought\n"
	               "\033[0mlocal command output\n"
	               "\033[0;90mcontinued thought\n"
	               "\033[0;35mlocal status\n\033[0m"
	               "\033[0;90m[End thinking]\n\033[0m"));
}

int main(void)
{
	FILE *capture = tmpfile();
	assert(capture);
	int saved = dup(STDOUT_FILENO);
	assert(saved >= 0);
	assert(!fflush(stdout));
	assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
	int e = 0;
	struct cli_loop loop;
	assert(!cli_loop_init(&loop));
	struct cli_console *console = cli_console_create(true, false, &loop, nullptr, &e);
	assert(console && !e);
	{
		struct cli_ui display = cli_ui(console);
		assert(!cli_ui_progress(&display, CSTR_VIEW("model"), CSTR_VIEW("turn 1, generating, 0 tokens")));
		assert(!cli_ui_push(&display, true, CSTR_VIEW("The")));
		assert(!cli_ui_progress(&display, CSTR_VIEW("model"), CSTR_VIEW("turn 1, thinking, 1 tokens")));
		assert(!cli_ui_push(&display, true, CSTR_VIEW(" user")));
		assert(!cli_ui_progress(&display, CSTR_VIEW("model"), CSTR_VIEW("turn 1, thinking, 2 tokens")));
		assert(!cli_ui_push(&display, true, CSTR_VIEW(" says hi.")));
		assert(!cli_ui_progress(&display, CSTR_VIEW("model"), CSTR_VIEW("turn 1, thinking, 3 tokens")));
		assert(!cli_ui_action(&display, CSTR_VIEW("model"), CSTR_VIEW("turn 1, calling notes")));
		assert(!cli_ui_progress(&display, CSTR_VIEW("model"), CSTR_VIEW("turn 2, generating, 0 tokens")));
		assert(!cli_ui_push(&display, true, CSTR_VIEW("Read")));
		assert(!cli_ui_progress(&display, CSTR_VIEW("model"), CSTR_VIEW("turn 2, thinking, 1 tokens")));
		assert(!cli_ui_push(&display, true, CSTR_VIEW(" the note.")));
		assert(!cli_ui_push(&display, false, CSTR_VIEW("Hello")));
		assert(!cli_ui_progress(&display, CSTR_VIEW("model"), CSTR_VIEW("turn 2, generating, 3 tokens")));
		assert(!cli_ui_push(&display, false, CSTR_VIEW(" there.")));
		assert(!cli_ui_progress(&display, CSTR_VIEW("model"), CSTR_VIEW("turn 2, generating, 4 tokens")));
		assert(!cli_ui_finish(&display));
		assert(!cli_ui_finish(&display));
		cli_ui_fini(&display);
	}
	{
		struct cli_ui display = cli_ui(console);
		assert(!cli_ui_push(&display, true, CSTR_VIEW("Interrupted")));
		assert(!cli_ui_progress(&display, CSTR_VIEW("model"), CSTR_VIEW("thinking, 1 tokens")));
		assert(!cli_ui_progress(&display, CSTR_VIEW("model"), CSTR_VIEW("thinking, 1 tokens")));
		assert(!cli_ui_push(&display, true, CSTR_VIEW(" reasoning.")));
		assert(!cli_ui_progress(&display, CSTR_VIEW("model"), CSTR_VIEW("thinking, 2 tokens")));
		cli_ui_fini(&display);
	}
	cli_console_destroy(&console);
	cli_loop_fini(&loop);
	assert(!fflush(stdout));
	assert(dup2(saved, STDOUT_FILENO) >= 0);
	assert(!close(saved));
	assert(!fseek(capture, 0, SEEK_SET));
	dstr_auto output = dstr(nullptr);
	char block[1024];
	size_t count;
	while ((count = fread(block, 1, sizeof block, capture))) {
		assert(dstr_cat_n(&output, block, count));
	}
	assert(!ferror(capture));
	assert(!fclose(capture));
	assert(!strcmp(dstr_get(&output),
	       "* model turn 1, generating, 0 tokens\n"
	       "[Start thinking]\n\n"
	       "The user says hi.\n[End thinking]\n"
	       "* model turn 1, thinking, 3 tokens\n"
	       "* model turn 1, calling notes\n"
	       "* model turn 2, generating, 0 tokens\n"
	       "[Start thinking]\n\n"
	       "Read the note.\n[End thinking]\n\n"
	       "Hello there.\n"
	       "* model turn 2, generating, 4 tokens\n"
	       "[Start thinking]\n\n"
	       "Interrupted reasoning.\n[End thinking]\n"
	       "* model thinking, 2 tokens\n"));
	cli_ui_test_colors();
	puts("cli UI: contiguous deltas, coalesced status, tool boundaries and interleaved colors: PASS");
}
