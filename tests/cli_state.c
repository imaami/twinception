/** @file Git history, restart, multiline and concurrent-writer checks,
 *  the argv of git, the grace of git at the deadline, the gc of a save and
 *  the refusal of a directory that contains other files.
 */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cli_state.h"
#include "cstr_view.h"
#include "git_run.h"
#include "loop.h"
#include "util.h"

/* The identity of the writers of the tests. */
static struct cstr_view const identity = CSTR_VIEW_INIT("session\0alice\0ask");

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

/* Runs git with the options @p options, shell words, for the repository
 * @p repo through the shell, and asserts that git exits with 0. If @p out
 * is not nullptr, stores up to @p size - 1 bytes of the output and a NUL
 * at @p out.
 */
static void
check_git (char const *repo,
           char       *out,
           size_t      size,
           char const *options)
{
	struct dstr command = dstr("");
	assert(dstr_printf(&command, "env -u GIT_DIR -u GIT_WORK_TREE git --git-dir='%s' %s", repo,
	                   options));
	FILE *f = popen(dstr_get(&command), "r");
	assert(f);
	size_t got = out ? fread(out, 1, size - 1, f) : 0;
	if (out)
		out[got] = '\0';
	assert(!pclose(f));
	dstr_cleanup(&command);
}

/* Returns the number of entries of the directory @p path but "." and "..",
 * or 0 if @p path does not exist.
 */
static size_t
check_entries (char const *path)
{
	DIR *dir = opendir(path);
	if (!dir) {
		assert(errno == ENOENT);
		return 0;
	}
	size_t n = 0;
	int e = 0;
	for (struct dirent *entry; (entry = util_dir_next(dir, &e)); )
		n += strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..");
	assert(!e && !closedir(dir));
	return n;
}

/* Tests util_dir_empty(), and that cli_state_create() refuses the new
 * directory @p path if it contains another file than cli-init.lock.
 */
static void
check_nonempty (char const *path)
{
	assert(!mkdir(path, 0700));
	int dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(dir >= 0);
	static struct cstr_view const lock[] = { CSTR_VIEW_INIT("cli-init.lock") };
	assert(!util_dir_empty(dir, nullptr, 0) && !util_dir_empty(dir, lock, 1));
	int fd = openat(dir, "cli-init.lock", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	assert(fd >= 0 && !close(fd));
	assert(util_dir_empty(dir, nullptr, 0) == EEXIST && !util_dir_empty(dir, lock, 1));
	fd = openat(dir, "other", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	assert(fd >= 0 && !close(fd));
	assert(util_dir_empty(dir, lock, 1) == EEXIST && util_dir_empty(dir, nullptr, 0) == EEXIST);
	/* util_dir_empty() reads the directory through a new descriptor. */
	assert(lseek(dir, 0, SEEK_CUR) == 0);
	assert(util_dir_empty(-1, nullptr, 0) == EBADF);
	fd = openat(dir, "other", O_RDONLY | O_CLOEXEC);
	assert(fd >= 0 && util_dir_empty(fd, nullptr, 0) == ENOTDIR);
	util_close(&fd);
	int e = 0;
	struct cli_state *state = cli_state_create(cstr_view(path), identity, nullptr, &e);
	assert(!state && e == EEXIST);
	struct stat st;
	assert(fstatat(dir, "config", &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT);
	assert(!fstatat(dir, "other", &st, AT_SYMLINK_NOFOLLOW));
	assert(!unlinkat(dir, "other", 0));
	state = cli_state_create(cstr_view(path), identity, nullptr, &e);
	assert(state && !e);
	cli_state_destroy(&state);
	assert(!fstatat(dir, "config", &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode));
	util_close(&dir);
}

/* Appends the words at @p words, up to their nullptr, to @p text, each
 * word on its own line.
 */
static void
check_words (struct dstr       *text,
             char const *const *words)
{
	for (; *words; ++words)
		assert(dstr_printf(text, "%s\n", *words));
}

/* Pins the argv of each git command of cli_state, and tests the deadline
 * and the grace of git. A git wrapper first on PATH writes the words of
 * each command after "git" to a log, then runs git. Two writers open a new
 * repository in the new directory @p root one after the other. The first
 * one makes the repository and saves one input. The second one loads the
 * snapshot of the first one.
 *
 * While the file HANG exists, the wrapper runs no git for update-ref. It
 * creates the lock file of each ref of the transaction, writes the paths
 * of the lock files to the file LOCKS, and waits. At SIGTERM, it removes
 * the lock files, writes TERM to LOCKS and exits. The save of the second
 * writer then fails with ETIMEDOUT after GIT_RUN_NS, no lock file is left,
 * and the next save succeeds.
 */
static void
check_argv (char const *root)
{
	struct dstr bin = dstr("");
	struct dstr wrapper = dstr("");
	struct dstr log = dstr("");
	struct dstr hang = dstr("");
	struct dstr locks = dstr("");
	struct dstr repo = dstr("");
	struct dstr path = dstr("");
	assert(!mkdir(root, 0700));
	assert(dstr_printf(&bin, "%s/bin", root) && dstr_printf(&wrapper, "%s/bin/git", root) &&
	       dstr_printf(&log, "%s/git.log", root) && dstr_printf(&hang, "%s/HANG", root) &&
	       dstr_printf(&locks, "%s/LOCKS", root) && dstr_printf(&repo, "%s/pin.git", root));
	assert(!mkdir(dstr_get(&bin), 0700));
	FILE *f = fopen(dstr_get(&wrapper), "w");
	assert(f);
	fprintf(f, "#!/bin/sh\n"
	           "locks='%s'\n"
	           "printf '%%s\\n' \"$@\" '' >> '%s'\n"
	           "for word; do\n"
	           "\t[ \"$word\" = update-ref ] && [ -e '%s' ] || continue\n"
	           "\tset --\n"
	           "\twhile read -r verb ref rest; do\n"
	           "\t\t[ \"$verb\" = update ] || continue\n"
	           "\t\tmkdir -p \"%s/${ref%%/*}\"\n"
	           "\t\t: > \"%s/$ref.lock\"\n"
	           "\t\tset -- \"$@\" \"%s/$ref.lock\"\n"
	           "\tdone\n"
	           "\tprintf '%%s\\n' \"$@\" >> \"$locks\"\n"
	           "\ttrap 'kill $! 2>/dev/null; rm -f \"$@\"; echo TERM >> \"$locks\"; exit 143' TERM\n"
	           "\tsleep 60 & wait\n"
	           "\texit 1\n"
	           "done\n"
	           "PATH=${PATH#*:}\n"
	           "export PATH\n"
	           "exec git \"$@\"\n",
	        dstr_get(&locks), dstr_get(&log), dstr_get(&hang), dstr_get(&repo), dstr_get(&repo),
	        dstr_get(&repo));
	assert(!fclose(f) && !chmod(dstr_get(&wrapper), 0700));
	char const *const old = getenv("PATH");
	char *saved = strdup(old ? old : "/usr/bin:/bin");
	assert(saved && dstr_printf(&path, "%s:%s", dstr_get(&bin), saved));
	assert(!setenv("PATH", dstr_get(&path), 1));

	int e = 0;
	struct cli_state *state = cli_state_create(cstr_view_dstr(&repo), identity, nullptr, &e);
	assert(state && !e);
	assert(!cli_state_add_input(state, CSTR_VIEW("pin")));
	cli_state_destroy(&state);
	state = cli_state_create(cstr_view_dstr(&repo), identity, nullptr, &e);
	assert(state && !e && cli_state_input_count(state) == 1);
	check_text(cli_state_input_at(state, 0), CSTR_VIEW("pin"));
	static char text[1 << 14];
	assert(!util_read_text(dstr_get(&log), text, sizeof text) && !unlink(dstr_get(&log)));

	/* The writer branch of the first writer and the latest ref of the
	 * group name the commit of the first save. */
	char refs[1024];
	check_git(dstr_get(&repo), refs, sizeof refs,
	          "for-each-ref --format='%(refname) %(objectname) %(tree)' refs/heads/cli/");
	char branch[128], commit[65], tree[65], latest[128], tip[65], tip_tree[65];
	assert(sscanf(refs, "%127s %64s %64s %127s %64s %64s", branch, commit, tree,
	              latest, tip, tip_tree) == 6);
	assert(!strcmp(commit, tip) && !strcmp(tree, tip_tree));
	char const *const slash = strrchr(branch, '/');
	assert(slash && !strncmp(latest, branch, (size_t)(slash + 1 - branch)) &&
	       !strcmp(latest + (slash + 1 - branch), "latest"));
	char group[128], name[128], blob[128];
	(void)util_format(group, sizeof group, "%.*s", (int)(slash + 1 - branch), branch);
	(void)util_format(name, sizeof name, "cli-%s.snapshot", slash + 1);
	(void)util_format(blob, sizeof blob, "%s:snapshot", commit);

	char const *const prefix[] = {
		GIT_OPTIONS_FIXED,
		"-c", "user.name=AskAI CLI", "-c", "user.email=askai-cli@localhost", "-c", "core.fsync=committed",
		"-C", dstr_get(&repo), "--git-dir=.", nullptr
	};
	static char const *const init[] = {
		"init", "--bare", "--quiet", "--template=", "--initial-branch=history", "--", ".", nullptr
	};
	static char const *const version[] = { "config", "--local", "askai.cliHistoryVersion", "1", nullptr };
	static char const *const get[] = { "config", "--local", "--get", "askai.cliHistoryVersion", nullptr };
	static char const *const hash[] = { "hash-object", "-w", "--stdin", nullptr };
	static char const *const mktree[] = { "mktree", nullptr };
	static char const *const update[] = { "update-ref", "--stdin", nullptr };
	static char const *const gc[] = { "-c", "gc.auto=6700", "gc", "--auto", "--quiet", nullptr };
	char const *const head[] = { "rev-parse", "--verify", "--quiet", latest, nullptr };
	char const *const snapshot[] = { "hash-object", "-w", "--", name, nullptr };
	char const *const commit_tree[] = { "commit-tree", tree, "-F", "-", nullptr };
	char const *const tips[] = {
		"for-each-ref", "--sort=-committerdate", "--count=32", "--format=%(objectname)", group, nullptr
	};
	char const *const cat[] = { "cat-file", "blob", blob, nullptr };
	char const *const *const runs[] = {
		init, version, head,
		snapshot, hash, hash, mktree, commit_tree, update, gc,
		get, head, tips, cat,
	};
	struct dstr expected = dstr("");
	for (size_t i = 0; i < array_size(runs); ++i) {
		check_words(&expected, prefix);
		check_words(&expected, runs[i]);
		assert(dstr_printf(&expected, "\n"));
	}
	if (!cstr_view_equal(cstr_view(text), cstr_view_dstr(&expected)))
		fprintf(stderr, "git wrote:\n%s\nexpected:\n%s\n", text, dstr_get(&expected));
	assert(cstr_view_equal(cstr_view(text), cstr_view_dstr(&expected)));
	dstr_cleanup(&expected);

	/* At the deadline, cli_state sends SIGTERM to the wrapper of
	 * update-ref, which removes the lock files in the grace. */
	int fd = open(dstr_get(&hang), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	assert(fd >= 0 && !close(fd));
	uint64_t const start = loop_clock();
	assert(cli_state_add_input(state, CSTR_VIEW("hang")) == ETIMEDOUT);
	uint64_t const took = loop_clock() - start;
	assert(took >= GIT_RUN_NS && took < GIT_RUN_NS + GIT_GRACE);
	assert(cli_state_recovery_file(state).len);
	assert(!util_read_text(dstr_get(&locks), text, sizeof text));
	char *line = text;
	for (size_t i = 0; i < 2; ++i) {
		char *end = strchr(line, '\n');
		assert(end && !strncmp(line, dstr_get(&repo), dstr_len(&repo)) &&
		       !strncmp(line + dstr_len(&repo) + 1, group, strlen(group)) &&
		       end - line > 5 && !memcmp(end - 5, ".lock", 5));
		*end = '\0';
		struct stat st;
		assert(stat(line, &st) && errno == ENOENT);
		line = end + 1;
	}
	assert(!strcmp(line, "TERM\n"));
	assert(!unlink(dstr_get(&hang)));
	assert(!cli_state_add_input(state, CSTR_VIEW("hang")));
	assert(!cli_state_recovery_file(state).len);
	cli_state_destroy(&state);
	assert(!setenv("PATH", saved, 1));
	char options[sizeof latest + sizeof "rev-parse '^'"];
	(void)util_format(options, sizeof options, "rev-parse '%s^'", latest);
	char parent[128];
	check_git(dstr_get(&repo), parent, sizeof parent, options);
	assert(!strncmp(parent, commit, strlen(commit)) && !strcmp(parent + strlen(commit), "\n"));
	free(saved);
	saved = nullptr;
	dstr_cleanup(&path);
	dstr_cleanup(&repo);
	dstr_cleanup(&locks);
	dstr_cleanup(&hang);
	dstr_cleanup(&log);
	dstr_cleanup(&wrapper);
	dstr_cleanup(&bin);
}

/* Tests that the gc of a save packs the loose objects of the history
 * repository, although GIT_OPTIONS_FIXED sets gc.auto=0. For gc --auto,
 * git counts the loose objects in objects/17 only. With gc.auto=6700, gc
 * packs the objects if objects/17 contains more than 27 entries, 6700 / 256
 * rounded up. The test makes the repository @p repo and sets gc.autoDetach
 * to false, so that gc runs in the foreground. fast-import writes 12800
 * blobs and a commit whose tree contains them as loose objects, with the
 * commit on a test ref, because gc leaves recent unreachable objects
 * loose. Then one save packs the loose objects.
 */
static void
check_gc (char const *repo)
{
	int e = 0;
	struct cli_state *state = cli_state_create(cstr_view(repo), identity, nullptr, &e);
	assert(state && !e);
	check_git(repo, nullptr, 0, "config --local gc.autoDetach false");
	struct dstr command = dstr("");
	assert(dstr_printf(&command, "env -u GIT_DIR -u GIT_WORK_TREE git --git-dir='%s' "
	                   "-c fastimport.unpackLimit=100000 fast-import --quiet --done", repo));
	FILE *f = popen(dstr_get(&command), "w");
	assert(f);
	enum { BLOBS = 12800 };
	for (unsigned i = 1; i <= BLOBS; ++i) {
		char data[UTIL_DECIMAL + 1];
		int n = snprintf(data, sizeof data, "%u\n", i);
		assert(n > 0 && n < (int)sizeof data);
		assert(fprintf(f, "blob\nmark :%u\ndata %d\n%s\n", i, n, data) > 0);
	}
	assert(fprintf(f, "commit refs/heads/test\ncommitter test <test@localhost> 0 +0000\ndata 0\n") > 0);
	for (unsigned i = 1; i <= BLOBS; ++i)
		assert(fprintf(f, "M 100644 :%u %u\n", i, i) > 0);
	assert(fprintf(f, "\ndone\n") > 0 && !pclose(f));
	dstr_cleanup(&command);
	struct dstr fanout = dstr("");
	assert(dstr_printf(&fanout, "%s/objects/17", repo));
	assert(check_entries(dstr_get(&fanout)) >= 28);
	char out[1024];
	unsigned long loose = 0;
	check_git(repo, out, sizeof out, "count-objects -v");
	assert(sscanf(out, "count: %lu", &loose) == 1 && loose >= BLOBS);
	assert(!cli_state_add_input(state, CSTR_VIEW("gc")));
	check_git(repo, out, sizeof out, "count-objects -v");
	assert(sscanf(out, "count: %lu", &loose) == 1 && loose < 100);
	assert(!check_entries(dstr_get(&fanout)));
	cli_state_destroy(&state);
	dstr_cleanup(&fanout);
}

/* Open the default repository with XDG_STATE_HOME @p xdg and HOME @p home,
 * and check that the directory @p expected, whose parents may be missing,
 * is now a private one.
 */
static void
check_default (char const *xdg,
               char const *home,
               char const *expected)
{
	assert(!setenv("XDG_STATE_HOME", xdg, 1) && !setenv("HOME", home, 1));
	int e = 0;
	struct cli_state *state = cli_state_create((struct cstr_view){}, identity, nullptr, &e);
	assert(state && !e);
	cli_state_destroy(&state);
	struct stat status;
	assert(!stat(expected, &status) && S_ISDIR(status.st_mode) && (status.st_mode & 0777) == 0700);
}

int
main (int argc, char **argv)
{
	assert(argc == 2);
	struct cstr_view repo = cstr_view(argv[1]);
	int e = 0;
	assert(!setenv("GIT_DIR", "/nonexistent/askai-history-wrong-repository", 1));
	assert(!setenv("GIT_WORK_TREE", "/nonexistent/askai-history-wrong-worktree", 1));
	struct cli_state *first = cli_state_create(repo, identity, nullptr, &e);
	assert(first && !e);
	assert(!cli_state_add_input(first, CSTR_VIEW("one\ntwo")));
	assert(!cli_state_add_turn(first, CSTR_VIEW("one\ntwo"), CSTR_VIEW("answer"), false));
	struct cli_state *second = cli_state_create(repo, identity, nullptr, &e);
	assert(second && !e);
	check_text(cli_state_input_at(second, 0), CSTR_VIEW("one\ntwo"));
	assert(cli_state_turn_count(second) == 1);
	check_text(cstr_view_dstr(&cli_state_turn_at(second, 0)->answer), CSTR_VIEW("answer"));
	assert(!cli_state_add_input(first, CSTR_VIEW("first writer")));
	assert(!cli_state_add_input(second, CSTR_VIEW("second writer")));
	assert(!cli_state_add_input(second, CSTR_VIEW("A")));
	assert(!cli_state_add_input(second, CSTR_VIEW("B")));
	assert(!cli_state_add_input(second, CSTR_VIEW("A")));
	assert(!cli_state_add_turn(second, CSTR_VIEW("one\ntwo"), CSTR_VIEW("replacement"), true));
	cli_state_destroy(&first);
	cli_state_destroy(&second);
	first = cli_state_create(repo, identity, nullptr, &e);
	assert(first && !e);
	check_text(cstr_view_dstr(&cli_state_turn_at(first, 0)->answer), CSTR_VIEW("replacement"));
	check_text(cli_state_input_at(first, cli_state_input_count(first) - 1), CSTR_VIEW("A"));
	assert(has_input(first, CSTR_VIEW("one\ntwo")));
	assert(has_input(first, CSTR_VIEW("first writer")));
	assert(has_input(first, CSTR_VIEW("second writer")));
	assert(!cli_state_clear_turns(first));
	cli_state_destroy(&first);
	first = cli_state_create(repo, identity, nullptr, &e);
	assert(first && !e && !cli_state_turn_count(first));
	assert(has_input(first, CSTR_VIEW("first writer")));
	assert(has_input(first, CSTR_VIEW("second writer")));
	assert(cli_state_add_turn(first, CSTR_VIEW("absent"), CSTR_VIEW("answer"), true) == EINVAL);
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
	struct dstr recovery_path = dstr("");
	assert(dstr_printf(&recovery_path, "%s/%.*s", argv[1], (int)recovery.len, recovery.data));
	cli_state_destroy(&first);
	struct stat status;
	assert(!stat(dstr_get(&recovery_path), &status));
	assert((status.st_mode & 0777) == 0600 && status.st_size > 0);
	dstr_cleanup(&recovery_path);
	first = cli_state_create(repo, CSTR_VIEW("session\0bob\0ask"), nullptr, &e);
	assert(first && !e && !cli_state_input_count(first));
	cli_state_destroy(&first);
	/* The default repository is in XDG_STATE_HOME if it is absolute,
	 * else in HOME's .local/state. */
	struct dstr xdg = dstr("");
	struct dstr home = dstr("");
	struct dstr expected = dstr("");
	assert(dstr_printf(&xdg, "%s.xdg//a", argv[1]) && dstr_printf(&home, "%s.home", argv[1]) &&
	       dstr_printf(&expected, "%s.xdg/a/askai/cli.git", argv[1]));
	check_default(dstr_get(&xdg), "/nonexistent/askai-home", dstr_get(&expected));
	assert(dstr_clear(&expected) && dstr_printf(&expected, "%s/.local/state/askai/cli.git", dstr_get(&home)));
	check_default("relative", dstr_get(&home), dstr_get(&expected));
	dstr_cleanup(&xdg);
	dstr_cleanup(&home);
	dstr_cleanup(&expected);
	/* The git processes of the tests themselves read no configuration
	 * file of the user. */
	assert(!setenv("GIT_CONFIG_NOSYSTEM", "1", 1) && !setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1));
	struct dstr path = dstr("");
	assert(dstr_printf(&path, "%s.full", argv[1]));
	check_nonempty(dstr_get(&path));
	assert(dstr_clear(&path) && dstr_printf(&path, "%s.gc", argv[1]));
	check_gc(dstr_get(&path));
	assert(dstr_clear(&path) && dstr_printf(&path, "%s.argv", argv[1]));
	check_argv(dstr_get(&path));
	dstr_cleanup(&path);
	puts("CLI Git history, its default path, the argv of git, the grace of git, "
	     "gc and nonempty directories: PASS");
	return 0;
}
