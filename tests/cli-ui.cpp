/** @file Progress must not split reasoning or text deltas into fragments. */

#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>

#include "cli-ui.hpp"

int main()
{
	FILE *capture = std::tmpfile();
	assert(capture);
	int saved = dup(STDOUT_FILENO);
	assert(saved >= 0);
	assert(!std::fflush(stdout));
	assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
	{
		cli_ui::assistant_turn display;
		display.progress("model", "turn 1, generating, 0 tokens");
		display.push(true, "The");
		display.progress("model", "turn 1, thinking, 1 tokens");
		display.push(true, " user");
		display.progress("model", "turn 1, thinking, 2 tokens");
		display.push(true, " says hi.");
		display.progress("model", "turn 1, thinking, 3 tokens");
		display.action("model", "turn 1, calling notes");
		display.progress("model", "turn 2, generating, 0 tokens");
		display.push(true, "Read");
		display.progress("model", "turn 2, thinking, 1 tokens");
		display.push(true, " the note.");
		display.push(false, "Hello");
		display.progress("model", "turn 2, generating, 3 tokens");
		display.push(false, " there.");
		display.progress("model", "turn 2, generating, 4 tokens");
		display.finish();
		display.finish();
	}
	{
		cli_ui::assistant_turn display;
		display.push(true, "Interrupted");
		display.progress("model", "thinking, 1 tokens");
		display.progress("model", "thinking, 1 tokens");
		display.push(true, " reasoning.");
		display.progress("model", "thinking, 2 tokens");
	}
	assert(!std::fflush(stdout));
	assert(dup2(saved, STDOUT_FILENO) >= 0);
	assert(!close(saved));
	assert(!std::fseek(capture, 0, SEEK_SET));
	std::string output;
	char block[1024];
	while (std::size_t count = std::fread(block, 1, sizeof block, capture)) {
		output.append(block, count);
	}
	assert(!std::ferror(capture));
	assert(!std::fclose(capture));
	assert(output ==
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
	       "* model thinking, 2 tokens\n");
	std::puts("cli UI: contiguous deltas, coalesced status, tool and final boundaries: PASS");
}
