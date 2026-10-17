/** @file Git history, restart, multiline and concurrent-writer checks. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cli-state.h"

#define TEXT(s) (struct cli_state_text){ s, sizeof(s) - 1 }

static void
check_text (struct cli_state_text actual, struct cli_state_text expected)
{
	assert(actual.len == expected.len);
	assert(!memcmp(actual.data, expected.data, actual.len));
}

static bool
has_input (struct cli_state const *state, struct cli_state_text text)
{
	for (size_t i = 0; i < cli_state_input_count(state); ++i) {
		struct cli_state_text input = cli_state_input_at(state, i);
		if (input.len == text.len && !memcmp(input.data, text.data, text.len))
			return true;
	}
	return false;
}

int
main (int argc, char **argv)
{
	assert(argc == 2);
	struct cli_state_text repo = { argv[1], strlen(argv[1]) };
	int e = 0;
	assert(!setenv("GIT_DIR", "/nonexistent/askai-history-wrong-repository", 1));
	assert(!setenv("GIT_WORK_TREE", "/nonexistent/askai-history-wrong-worktree", 1));
	struct cli_state *first = cli_state_create(repo, TEXT("session\0alice\0ask"), &e);
	assert(first && !e);
	assert(!cli_state_add_input(first, TEXT("one\ntwo")));
	assert(!cli_state_add_turn(first, TEXT("one\ntwo"), TEXT("answer"), 0));
	struct cli_state *second = cli_state_create(repo, TEXT("session\0alice\0ask"), &e);
	assert(second && !e);
	check_text(cli_state_input_at(second, 0), TEXT("one\ntwo"));
	assert(cli_state_turn_count(second) == 1);
	check_text(cli_state_turn_at(second, 0).answer, TEXT("answer"));
	assert(!cli_state_add_input(first, TEXT("first writer")));
	assert(!cli_state_add_input(second, TEXT("second writer")));
	assert(!cli_state_add_input(second, TEXT("A")));
	assert(!cli_state_add_input(second, TEXT("B")));
	assert(!cli_state_add_input(second, TEXT("A")));
	assert(!cli_state_add_turn(second, TEXT("one\ntwo"), TEXT("replacement"), 1));
	cli_state_destroy(&first);
	cli_state_destroy(&second);
	first = cli_state_create(repo, TEXT("session\0alice\0ask"), &e);
	assert(first && !e);
	check_text(cli_state_turn_at(first, 0).answer, TEXT("replacement"));
	check_text(cli_state_input_at(first, cli_state_input_count(first) - 1), TEXT("A"));
	assert(has_input(first, TEXT("one\ntwo")));
	assert(has_input(first, TEXT("first writer")));
	assert(has_input(first, TEXT("second writer")));
	assert(!cli_state_clear_turns(first));
	cli_state_destroy(&first);
	first = cli_state_create(repo, TEXT("session\0alice\0ask"), &e);
	assert(first && !e && !cli_state_turn_count(first));
	assert(has_input(first, TEXT("first writer")));
	assert(has_input(first, TEXT("second writer")));
	assert(cli_state_add_turn(first, TEXT("absent"), TEXT("answer"), 1) == EINVAL);
	char *large = malloc((1 << 20) + 1);
	assert(large);
	memset(large, 'x', (1 << 20) + 1);
	assert(cli_state_add_input(first, (struct cli_state_text){ large, (1 << 20) + 1 }) == EFBIG);
	free(large);
	large = nullptr;
	char *saved_path = strdup(getenv("PATH"));
	assert(saved_path && !setenv("PATH", "/nonexistent/askai-git", 1));
	assert(cli_state_add_input(first, TEXT("recoverable after Git failure")) == ENOENT);
	assert(!setenv("PATH", saved_path, 1));
	free(saved_path);
	saved_path = nullptr;
	struct cli_state_text recovery = cli_state_recovery_file(first);
	assert(recovery.len);
	char *recovery_path = malloc(repo.len + recovery.len + 2);
	assert(recovery_path);
	memcpy(recovery_path, repo.data, repo.len);
	recovery_path[repo.len] = '/';
	memcpy(recovery_path + repo.len + 1, recovery.data, recovery.len);
	recovery_path[repo.len + recovery.len + 1] = '\0';
	cli_state_destroy(&first);
	struct stat status;
	assert(!stat(recovery_path, &status));
	assert((status.st_mode & 0777) == 0600 && status.st_size > 0);
	free(recovery_path);
	recovery_path = nullptr;
	first = cli_state_create(repo, TEXT("session\0bob\0ask"), &e);
	assert(first && !e && !cli_state_input_count(first));
	cli_state_destroy(&first);
	puts("CLI Git history: PASS");
	return 0;
}

#undef TEXT
