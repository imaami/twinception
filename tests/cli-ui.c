/** @file Progress must not split reasoning or text deltas into fragments. */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cli-ui.h"

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
		assert(!cli_ui_progress(&display, CLI_TEXT("model"), CLI_TEXT("turn 1, generating, 0 tokens")));
		assert(!cli_ui_push(&display, true, CLI_TEXT("The")));
		assert(!cli_ui_progress(&display, CLI_TEXT("model"), CLI_TEXT("turn 1, thinking, 1 tokens")));
		assert(!cli_ui_push(&display, true, CLI_TEXT(" user")));
		assert(!cli_ui_progress(&display, CLI_TEXT("model"), CLI_TEXT("turn 1, thinking, 2 tokens")));
		assert(!cli_ui_push(&display, true, CLI_TEXT(" says hi.")));
		assert(!cli_ui_progress(&display, CLI_TEXT("model"), CLI_TEXT("turn 1, thinking, 3 tokens")));
		assert(!cli_ui_action(&display, CLI_TEXT("model"), CLI_TEXT("turn 1, calling notes")));
		assert(!cli_ui_progress(&display, CLI_TEXT("model"), CLI_TEXT("turn 2, generating, 0 tokens")));
		assert(!cli_ui_push(&display, true, CLI_TEXT("Read")));
		assert(!cli_ui_progress(&display, CLI_TEXT("model"), CLI_TEXT("turn 2, thinking, 1 tokens")));
		assert(!cli_ui_push(&display, true, CLI_TEXT(" the note.")));
		assert(!cli_ui_push(&display, false, CLI_TEXT("Hello")));
		assert(!cli_ui_progress(&display, CLI_TEXT("model"), CLI_TEXT("turn 2, generating, 3 tokens")));
		assert(!cli_ui_push(&display, false, CLI_TEXT(" there.")));
		assert(!cli_ui_progress(&display, CLI_TEXT("model"), CLI_TEXT("turn 2, generating, 4 tokens")));
		assert(!cli_ui_finish(&display));
		assert(!cli_ui_finish(&display));
		cli_ui_fini(&display);
	}
	{
		struct cli_ui display = cli_ui(console);
		assert(!cli_ui_push(&display, true, CLI_TEXT("Interrupted")));
		assert(!cli_ui_progress(&display, CLI_TEXT("model"), CLI_TEXT("thinking, 1 tokens")));
		assert(!cli_ui_progress(&display, CLI_TEXT("model"), CLI_TEXT("thinking, 1 tokens")));
		assert(!cli_ui_push(&display, true, CLI_TEXT(" reasoning.")));
		assert(!cli_ui_progress(&display, CLI_TEXT("model"), CLI_TEXT("thinking, 2 tokens")));
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
