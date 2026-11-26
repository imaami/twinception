/** @file Calls of one tool in a row share a line, which does not split
 *  reasoning or text deltas into fragments. Simple output shows no status.
 */
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
	struct cli_loop loop = {};
	assert(!cli_loop_init(&loop));
	int e;
	struct cli_console *console = cli_console_create(CLI_CONSOLE_SIMPLE | CLI_CONSOLE_COLOR, &loop, nullptr, &e);
	assert(console && !e);
	struct cli_ui display = cli_ui(console);
	assert(!cli_ui_push(&display, true, CSTR_VIEW("first thought\n")));
	assert(!cli_ui_message(console, CSTR_VIEW("local command output")));
	assert(!cli_ui_push(&display, true, CSTR_VIEW("continued thought\n")));
	assert(!cli_ui_info(console, CSTR_VIEW("local status")));
	assert(!cli_ui_action(&display, CSTR_VIEW("git")));
	assert(!cli_ui_action(&display, CSTR_VIEW("git")));
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
	struct cli_console_span const expected[] = {
		cli_console_span(CSTR_VIEW("[Start thinking]\n\nfirst thought\n"), CLI_STYLE_REASONING),
		cli_console_span(CSTR_VIEW("local command output\n"), CLI_STYLE_RESET),
		cli_console_span(CSTR_VIEW("continued thought\n"), CLI_STYLE_REASONING),
		cli_console_span(CSTR_VIEW("local status\n"), CLI_STYLE_INFO),
		cli_console_span(CSTR_VIEW(""), CLI_STYLE_RESET),
		cli_console_span(CSTR_VIEW("[End thinking]\n"), CLI_STYLE_REASONING),
		cli_console_span(CSTR_VIEW(""), CLI_STYLE_RESET),
		cli_console_span(CSTR_VIEW("* git x2\n"), CLI_STYLE_TOOL),
		cli_console_span(CSTR_VIEW(""), CLI_STYLE_RESET),
	};
	struct cli_style_entry const *palette = cli_style_palette(true);
	struct cstr_view remaining = cstr_view_dstr(&output);
	for (size_t i = 0; i < array_size(expected); ++i) {
		struct cli_style_entry const *entry = &palette[expected[i].style];
		struct cstr_view style = cstr_view_n(entry->bytes, entry->length);
		assert(cstr_view_starts(remaining, style));
		remaining = cstr_view_slice(remaining, style.len, SIZE_MAX);
		assert(cstr_view_starts(remaining, expected[i].text));
		remaining = cstr_view_slice(remaining, expected[i].text.len, SIZE_MAX);
	}
	assert(!remaining.len);
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
	struct cli_loop loop = {};
	assert(!cli_loop_init(&loop));
	struct cli_console *console = cli_console_create(CLI_CONSOLE_SIMPLE, &loop, nullptr, &e);
	assert(console && !e);
	{
		struct cli_ui display = cli_ui(console);
		assert(!cli_console_set_progress(console, (struct cli_console_progress){
			.turn = 1, .reasoning = 3, .active = CLI_CONSOLE_COUNT_REASONING}));
		assert(cli_console_set_progress(console, (struct cli_console_progress){
			.active = CLI_CONSOLE_COUNT_NONE + 1}) == EINVAL);
		assert(cli_console_set_progress(nullptr, (struct cli_console_progress){}) == EINVAL);
		assert(!cli_ui_push(&display, true, CSTR_VIEW("The")));
		assert(!cli_ui_push(&display, true, CSTR_VIEW(" user")));
		assert(!cli_ui_push(&display, true, CSTR_VIEW(" says hi.")));
		assert(!cli_ui_action(&display, CSTR_VIEW("notes")));
		assert(!cli_ui_action(&display, CSTR_VIEW("notes")));
		assert(!cli_ui_push(&display, true, CSTR_VIEW("Read")));
		assert(!cli_ui_push(&display, true, CSTR_VIEW(" the note.")));
		assert(!cli_ui_action(&display, CSTR_VIEW("git")));
		assert(!cli_ui_action(&display, CSTR_VIEW("notes")));
		assert(!cli_ui_push(&display, false, CSTR_VIEW("Hello")));
		assert(!cli_ui_push(&display, false, CSTR_VIEW(" there.")));
		assert(!cli_ui_finish(&display));
		assert(!cli_ui_finish(&display));
		cli_ui_fini(&display);
	}
	{
		struct cli_ui display = cli_ui(console);
		assert(!cli_ui_push(&display, false, CSTR_VIEW("Let me look.")));
		assert(!cli_ui_action(&display, CSTR_VIEW("git")));
		assert(!cli_ui_push(&display, false, CSTR_VIEW("Done.")));
		assert(!cli_ui_push(&display, true, CSTR_VIEW("Interrupted")));
		assert(!cli_ui_action(&display, CSTR_VIEW("git")));
		assert(!cli_ui_action(&display, CSTR_VIEW("git")));
		assert(!cli_ui_action(&display, CSTR_VIEW("git")));
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
	       "[Start thinking]\n\n"
	       "The user says hi.\n[End thinking]\n"
	       "* notes x2\n"
	       "[Start thinking]\n\n"
	       "Read the note.\n[End thinking]\n"
	       "* git\n"
	       "* notes\n"
	       "Hello there.\n"
	       "Let me look.\n"
	       "* git\n"
	       "Done.\n"
	       "[Start thinking]\n\n"
	       "Interrupted\n[End thinking]\n"
	       "* git x3\n"));
	cli_ui_test_colors();
	puts("cli UI: contiguous deltas, calls in a row on one line, no status and interleaved colors: PASS");
}
