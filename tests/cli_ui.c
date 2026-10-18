/** @file Progress must not split reasoning or text deltas into fragments. */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cli_console.h"
#include "cli_ui.h"

int main(void)
{
	FILE *capture = tmpfile();
	assert(capture);
	int saved = dup(STDOUT_FILENO);
	assert(saved >= 0);
	assert(!fflush(stdout));
	assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
	int e = 0;
	struct cli_console *console = cli_console_create(true, false, nullptr, -1, &e);
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
	puts("cli UI: contiguous deltas, coalesced status, tool and final boundaries: PASS");
}
