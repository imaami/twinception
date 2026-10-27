/** @file Git history, restart, multiline and concurrent-writer checks. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cli_state.h"
#include "cstr_view.h"

static void
check_text (struct cstr_view actual, struct cstr_view expected)
{
	assert(cstr_view_equal(actual, expected));
}

static bool
has_input (struct cli_state const *state, struct cstr_view text)
{
	for (size_t i = 0; i < cli_state_input_count(state); ++i) {
		struct cstr_view input = cli_state_input_at(state, i);
		if (cstr_view_equal(input, text))
			return true;
	}
	return false;
}

int
main (int argc, char **argv)
{
	assert(argc == 2);
	struct cstr_view repo = cstr_view(argv[1]);
	int e = 0;
	assert(!setenv("GIT_DIR", "/nonexistent/askai-history-wrong-repository", 1));
	assert(!setenv("GIT_WORK_TREE", "/nonexistent/askai-history-wrong-worktree", 1));
	struct cli_state *first = cli_state_create(repo, CSTR_VIEW("session\0alice\0ask"), nullptr, &e);
	assert(first && !e);
	assert(!cli_state_add_input(first, CSTR_VIEW("one\ntwo")));
	assert(!cli_state_add_turn(first, CSTR_VIEW("one\ntwo"), CSTR_VIEW("answer"), 0));
	struct cli_state *second = cli_state_create(repo, CSTR_VIEW("session\0alice\0ask"), nullptr, &e);
	assert(second && !e);
	check_text(cli_state_input_at(second, 0), CSTR_VIEW("one\ntwo"));
	assert(cli_state_turn_count(second) == 1);
	check_text(cli_state_turn_at(second, 0).answer, CSTR_VIEW("answer"));
	assert(!cli_state_add_input(first, CSTR_VIEW("first writer")));
	assert(!cli_state_add_input(second, CSTR_VIEW("second writer")));
	assert(!cli_state_add_input(second, CSTR_VIEW("A")));
	assert(!cli_state_add_input(second, CSTR_VIEW("B")));
	assert(!cli_state_add_input(second, CSTR_VIEW("A")));
	assert(!cli_state_add_turn(second, CSTR_VIEW("one\ntwo"), CSTR_VIEW("replacement"), 1));
	cli_state_destroy(&first);
	cli_state_destroy(&second);
	first = cli_state_create(repo, CSTR_VIEW("session\0alice\0ask"), nullptr, &e);
	assert(first && !e);
	check_text(cli_state_turn_at(first, 0).answer, CSTR_VIEW("replacement"));
	check_text(cli_state_input_at(first, cli_state_input_count(first) - 1), CSTR_VIEW("A"));
	assert(has_input(first, CSTR_VIEW("one\ntwo")));
	assert(has_input(first, CSTR_VIEW("first writer")));
	assert(has_input(first, CSTR_VIEW("second writer")));
	assert(!cli_state_clear_turns(first));
	cli_state_destroy(&first);
	first = cli_state_create(repo, CSTR_VIEW("session\0alice\0ask"), nullptr, &e);
	assert(first && !e && !cli_state_turn_count(first));
	assert(has_input(first, CSTR_VIEW("first writer")));
	assert(has_input(first, CSTR_VIEW("second writer")));
	assert(cli_state_add_turn(first, CSTR_VIEW("absent"), CSTR_VIEW("answer"), 1) == EINVAL);
	char *large = malloc((1 << 20) + 1);
	assert(large);
	memset(large, 'x', (1 << 20) + 1);
	assert(cli_state_add_input(first, (struct cstr_view){ large, (1 << 20) + 1 }) == EFBIG);
	free(large);
	large = nullptr;
	char *saved_path = strdup(getenv("PATH"));
	assert(saved_path && !setenv("PATH", "/nonexistent/askai-git", 1));
	/* A saved input that repeats needs no Git; an unsaved one retries. */
	assert(!cli_state_add_input(first, cli_state_input_at(first, cli_state_input_count(first) - 1)));
	assert(cli_state_add_input(first, CSTR_VIEW("recoverable after Git failure")) == ENOENT);
	assert(cli_state_add_input(first, CSTR_VIEW("recoverable after Git failure")) == ENOENT);
	assert(!setenv("PATH", saved_path, 1));
	free(saved_path);
	saved_path = nullptr;
	struct cstr_view recovery = cli_state_recovery_file(first);
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
	first = cli_state_create(repo, CSTR_VIEW("session\0bob\0ask"), nullptr, &e);
	assert(first && !e && !cli_state_input_count(first));
	cli_state_destroy(&first);
	puts("CLI Git history: PASS");
	return 0;
}
