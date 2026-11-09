/** @file Unit tests of the git store, src/store.c: the argv of each
 *  command, the checks of store_command(), store_spawn() and store_lock(),
 *  the input of git fast-import, the commits that git makes from that
 *  input in a new bare repository, and the common directory of a clone and
 *  of a worktree. The test replaces mcp_slot_spawn() and mcp_slot_loop().
 */
#define _GNU_SOURCE /* memmem() */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buf.h"
#include "check.h"
#include "child.h"
#include "git_run.h"
#include "loop.h"
#include "mcp.h"
#include "oid.h"
#include "store.h"
#include "util.h"

/* Check that the @p len bytes at @p got are the string @p want. */
#define SAME(got, len, want) same(__LINE__, got, len, want)

static void
same (int         line,
      void const *got,
      size_t      len,
      char const *want)
{
	size_t const n = strlen(want);
	if (len == n && !memcmp(got, want, n))
		return;
	++failures;
	fprintf(stderr, "%s:%d: got\n%.*s\nwant\n%s\n", __FILE__, line, (int)len,
	        (char const *)got, want);
}

/* A commit id that no repository of the test contains. */
#define A "55aca1e8682351e4c39a92b040721def3f005402"

/* Writes the words of @p argv up to its nullptr to @p text, each after a
 * space, and returns the number of words.
 */
static size_t
join (char const *const *argv,
      char              *text,
      size_t             size)
{
	size_t n = 0;
	size_t at = 0;
	for (text[0] = '\0'; argv[n]; ++n) {
		int k = snprintf(text + at, size - at, " %s", argv[n]);
		CHECK(k >= 0 && (size_t)k < size - at);
		if (k < 0 || (size_t)k >= size - at)
			break;
		at += (size_t)k;
	}
	return n;
}

/* Writes to @p text the start of the argv of every command in @p store:
 * "git" and the fixed options, as git_run_dir_argv() writes them, then the
 * options of the store, -C, the common directory and "--git-dir=.".
 */
static void
prefix (struct store const *store,
        char               *text,
        size_t              size)
{
	struct git_run_dir dir;
	char const *argv[GIT_RUN_ARGV];
	CHECK(!git_run_dir_init(&dir, nullptr, nullptr, nullptr, 0));
	CHECK(!git_run_dir_argv(&dir, nullptr, argv, array_size(argv)));
	char fixed[1024];
	join(argv, fixed, sizeof fixed);
	int k = snprintf(text, size, "%s --literal-pathspecs -c core.bare=true -C %s --git-dir=.",
	                 fixed, store_directory(store));
	CHECK(k > 0 && (size_t)k < size);
}

/* What store_spawn() passed to mcp_slot_spawn(). */
static struct {
	char     argv[2048];
	uint64_t deadline;
	uint64_t grace;
	uint64_t flags;
	size_t   max;
	size_t   in;
	unsigned calls;
	bool     path;
} spawned;

static struct loop test_loop;
static struct child test_child;
static max_align_t test_slot;

struct loop *
mcp_slot_loop (struct mcp_slot const *slot)
{
	CHECK(slot == (void const *)&test_slot);
	return &test_loop;
}

struct child *
mcp_slot_spawn (struct mcp_slot        *slot,
                struct child_cmd const *cmd,
                int                    *e)
{
	CHECK(slot == (void *)&test_slot && !strcmp(cmd->file, "git"));
	join(cmd->argv, spawned.argv, sizeof spawned.argv);
	spawned.deadline = cmd->deadline;
	spawned.grace = cmd->grace;
	spawned.flags = cmd->flags;
	spawned.max = cmd->max;
	spawned.in = cmd->in.len;
	spawned.path = !strncmp(cmd->envp[0], "PATH=", 5);
	++spawned.calls;
	*e = 0;
	return &test_child;
}

/* Runs the git command @p run on the calling thread with child_capture(),
 * with a cap of @p size - 1 bytes, and writes the output to the @p size
 * bytes at @p out and its length to @p *len. Returns the exit status of
 * git, or -1 if git did not run or wrote more.
 */
static int
run_git (struct git_run *run,
         char           *out,
         size_t          size,
         size_t         *len)
{
	struct buf got = {};
	struct child_end end;
	run->c.max = size - 1;
	int e = child_capture(&run->c, child_fds(), &got, &end);
	CHECK(!e);
	struct span const s = buf_peek(&got);
	*len = s.len;
	if (s.len)
		memcpy(out, s.ptr, s.len);
	buf_fini(&got);
	return e ? -1 : end.status;
}

/* Runs @p cmd in @p store as store_spawn() would, with the words @p words
 * and the input @p in, as run_git() does. Without CHILD_STDERR, the errors
 * of git go to /dev/null.
 */
static int
run (struct store const *store,
     enum store_cmd      cmd,
     char const *const  *words,
     struct span         in,
     char               *out,
     size_t              size,
     size_t             *len)
{
	struct git_run r;
	int e = store_command(store, &r, cmd, words, nullptr, in, loop_clock());
	CHECK(!e);
	return e ? -1 : run_git(&r, out, size, len);
}

/* Runs git with the words @p words in the directory @p path, with the
 * fixed options and environment of git_run, as run_git() does. The output
 * includes the errors of git.
 */
static int
run_words (char const        *path,
           char const *const *words,
           char              *out,
           size_t             size,
           size_t            *len)
{
	struct git_run_dir dir;
	struct git_run r;
	int e = git_run_dir_init(&dir, path, nullptr, nullptr, 0);
	if (!e)
		e = git_run_init(&r, &dir, words, &(struct child_cmd){
			.deadline = loop_clock() + GIT_RUN_NS,
			.flags    = CHILD_STDERR,
		});
	CHECK(!e);
	return e ? -1 : run_git(&r, out, size, len);
}

/* Runs git fast-import in @p store with the input of @p commit, after the
 * lock check of commit->ref. Writes the new commit to @p id and the output
 * of git to the @p size bytes at @p out and its length to @p *len. Returns
 * the exit status of git.
 */
static int
commit_run (struct store const        *store,
            struct store_commit const *commit,
            struct oid                *id,
            char                      *out,
            size_t                     size,
            size_t                    *len)
{
	struct buf in = {};
	CHECK(!store_commit_input(commit, &in));
	struct git_run r;
	int e = store_command(store, &r, STORE_COMMIT, nullptr, commit->ref, buf_peek(&in),
	                      loop_clock());
	CHECK(!e);
	int status = e ? -1 : run_git(&r, out, size, len);
	buf_fini(&in);
	*id = (struct oid){};
	if (!status)
		CHECK(!store_committed((struct span){ (unsigned char *)out, *len }, id));
	return status;
}

/* Returns true if store_commit_input() refuses @p commit and writes
 * nothing.
 */
static bool
refused (struct store_commit commit)
{
	struct buf in = {};
	int e = store_commit_input(&commit, &in);
	bool empty = !buf_peek(&in).len;
	buf_fini(&in);
	return e == EINVAL && empty;
}

static void
test_input (void)
{
	struct oid tip;
	CHECK(!oid_init(&tip, CSTR_VIEW(A)));
	struct oid const root = {};

	/* A file part quotes the path as fast-import reads it. */
	struct buf files = {};
	struct store_part part;
	CHECK(!store_part_file(&part, &files, CSTR_VIEW("a\"b\\c"), CSTR_VIEW("abc")));
	CHECK(part.name_at == 16 && part.name_len == 9 && part.data_at == 33 && part.data_len == 3);
	CHECK(!store_part_delete(&part, &files, CSTR_VIEW("d")));
	CHECK(part.name_at == 39 && part.name_len == 3 && part.data_at == 42 && !part.data_len);
	char name[STORE_PATH + 2];
	memset(name, 'n', sizeof name);
	CHECK(store_part_file(&part, &files, (struct cstr_view){ name, STORE_PATH + 1 },
	                      CSTR_VIEW("")) == EINVAL);
	CHECK(store_part_delete(&part, &files, (struct cstr_view){}) == EINVAL);
	struct span s = buf_peek(&files);
	SAME(s.ptr, s.len, "M 100644 inline \"a\\\"b\\\\c\"\ndata 3\nabc\nD \"d\"\n");

	/* The input of a commit with a committer is the same byte for byte as
	 * the input that notes_add_input() wrote before the store. */
	struct store_commit const commit = {
		.committer = { CSTR_VIEW("gpu1/upstream"), CSTR_VIEW("askai") },
		.message = CSTR_VIEW("Add a (3 bytes)"),
		.files = s,
		.ref = "refs/heads/notes/t",
		.tip = &tip,
	};
	struct buf in = {};
	CHECK(!store_commit_input(&commit, &in));
	s = buf_peek(&in);
	SAME(s.ptr, s.len, "commit refs/heads/notes/t\nmark :1\n"
	     "committer gpu1/upstream <askai> now\ndata 16\nAdd a (3 bytes)\n"
	     "from " A "\nM 100644 inline \"a\\\"b\\\\c\"\ndata 3\nabc\nD \"d\"\n"
	     "get-mark :1\n");
	buf_fini(&in);

	/* A root commit has no from line, and an empty email is valid. */
	struct store_commit c = commit;
	c.committer.email = CSTR_VIEW("");
	c.message = CSTR_VIEW("");
	c.files = (struct span){};
	c.tip = &root;
	CHECK(!store_commit_input(&c, &in));
	s = buf_peek(&in);
	SAME(s.ptr, s.len, "commit refs/heads/notes/t\nmark :1\ncommitter gpu1/upstream <> now\n"
	     "data 1\n\nget-mark :1\n");
	buf_fini(&in);

	char big[STORE_MESSAGE];
	memset(big, 'x', sizeof big);
	c = commit;
	c.message = (struct cstr_view){ big, STORE_MESSAGE - 1 };
	CHECK(!store_commit_input(&c, &in));
	buf_fini(&in);
	c.message.len = STORE_MESSAGE;
	CHECK(refused(c));
	CHECK(store_commit_input(nullptr, &in) == EINVAL);
	CHECK(store_commit_input(&commit, nullptr) == EINVAL);

	struct cstr_view const bad[] = {
		{}, { nullptr, 5 }, CSTR_VIEW("a\0b"),
	};
	for (size_t i = 0; i < array_size(bad); ++i) {
		c = commit;
		c.message = bad[i];
		CHECK(refused(c));
		c = commit;
		c.committer.name = bad[i];
		CHECK(refused(c));
		c = commit;
		c.committer.email = bad[i];
		CHECK(refused(c));
	}
	struct cstr_view const idents[] = {
		CSTR_VIEW("a<b"), CSTR_VIEW("a>b"), CSTR_VIEW("a\nb"),
	};
	for (size_t i = 0; i < array_size(idents); ++i) {
		c = commit;
		c.committer.name = idents[i];
		CHECK(refused(c));
		c = commit;
		c.committer.email = idents[i];
		CHECK(refused(c));
	}
	c = commit;
	c.committer.name = CSTR_VIEW("");
	CHECK(refused(c));
	c = commit;
	c.ref = nullptr;
	CHECK(refused(c));
	c.ref = "refs/heads/a\nb";
	CHECK(refused(c));
	c = commit;
	c.tip = nullptr;
	CHECK(refused(c));
	buf_fini(&files);

	struct oid id = {};
	CHECK(store_committed((struct span){}, nullptr) == EINVAL);
	CHECK(store_committed((struct span){ (unsigned char *)"x\n", 2 }, &id) == EIO && !oid_len(&id));
}

static void
test_argv (struct store const *store)
{
	struct {
		enum store_cmd  cmd;
		char const     *words[STORE_WORDS];
		char const     *tail;
	} const rows[] = {
		{ STORE_CREATE,   {},                 " init --bare --quiet --template="
		                                      " --initial-branch=session -- ." },
		{ STORE_CHECK,    {},                 " cat-file --batch-check" },
		{ STORE_COMMIT,   {},                 " fast-import --quiet --date-format=now" },
		{ STORE_LOG,      { "R", "p" },       " log -z --no-renames --format=%ct %H %s --numstat R"
		                                      " -- p" },
		{ STORE_LOG,      { "R" },            " log -z --no-renames --format=%ct %H %s --numstat R"
		                                      " --" },
		{ STORE_READ,     {},                 " cat-file --batch" },
		{ STORE_REFS,     { "--format=%(refname)", "refs/heads/" },
		                                      " for-each-ref --format=%(refname) refs/heads/" },
		{ STORE_FILES,    { "R" },            " ls-tree -r -z -l R --" },
		{ STORE_HEAD,     { "R" },            " rev-parse --verify --end-of-options R" },
		{ STORE_ANCESTOR, { "A", "B" },       " merge-base --is-ancestor A B" },
		{ STORE_DIFF,     { "A", "B", "p" },  " diff --no-ext-diff --no-textconv --no-renames"
		                                      " --no-color --submodule=short A B -- p" },
		{ STORE_STAT,     { "A", "B" },       " diff --numstat --no-textconv A B" },
		{ STORE_GREP,     { "A" },            " grep -IniEz --no-color -f - A --" },
	};
	/* The ref of the lock check of STORE_COMMIT. The test repository
	 * contains no lock file. */
	char const *const ref = "refs/heads/notes/t";
	char start[1024], want[2048], got[2048];
	struct git_run run;
	prefix(store, start, sizeof start);
	for (size_t i = 0; i < array_size(rows); ++i) {
		CHECK(!store_command(store, &run, rows[i].cmd, rows[i].words, ref, (struct span){},
		                     7));
		CHECK(join(run.argv, got, sizeof got) < GIT_RUN_ARGV);
		CHECK(snprintf(want, sizeof want, "%s%s", start, rows[i].tail) > 0);
		SAME(got, strlen(got), want);
		/* For the commands that write, the grace is GIT_GRACE, and the
		 * output includes the errors. */
		bool const writes = rows[i].cmd == STORE_CREATE || rows[i].cmd == STORE_COMMIT;
		CHECK(!strcmp(run.c.file, "git") && run.c.argv == run.argv &&
		      !strncmp(run.c.envp[0], "PATH=", 5) && !run.c.in.len &&
		      run.c.max == STORE_MAX && run.c.deadline == 7 + GIT_RUN_NS &&
		      run.c.grace == (writes ? GIT_GRACE : 0) &&
		      run.c.flags == (writes ? CHILD_STDERR : 0));
		/* A command without words does not read the words. */
		int e = store_command(store, &run, rows[i].cmd, nullptr, ref, (struct span){}, 7);
		CHECK(e == (rows[i].words[0] ? EINVAL : 0));
	}
	struct span const in = { (unsigned char *)"abc", 3 };
	CHECK(!store_command(store, &run, STORE_READ, nullptr, nullptr, in, 7));
	CHECK(run.c.in.ptr == in.ptr && run.c.in.len == in.len);
	CHECK(store_command(store, &run, STORE_COMMIT, nullptr, nullptr, in, 7) == EINVAL);
	CHECK(store_command(nullptr, &run, STORE_READ, nullptr, nullptr, in, 7) == EINVAL);
	CHECK(store_command(store, nullptr, STORE_READ, nullptr, nullptr, in, 7) == EINVAL);
	CHECK(store_command(store, &run, (enum store_cmd)(STORE_GREP + 1), nullptr, nullptr, in,
	                    7) == EINVAL);
}

/* Creates the file @p path, with the time of its last change @p mtime
 * seconds after the epoch, or now if @p mtime is 0.
 */
static void
lock_make (char const *path,
           time_t      mtime)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(fd >= 0);
	struct timespec const times[2] = { { .tv_nsec = UTIME_OMIT }, { .tv_sec = mtime } };
	if (mtime)
		CHECK(!futimens(fd, times));
	util_close(&fd);
}

static void
test_spawn (struct store const *store)
{
	struct mcp_slot *slot = (struct mcp_slot *)(void *)&test_slot;
	char start[1024], want[2048];
	prefix(store, start, sizeof start);
	test_loop.now = 5 * NS;
	int e = -1;
	CHECK(store_spawn(store, slot, STORE_READ, nullptr, nullptr, (struct span){}, &e) ==
	      &test_child && !e);
	CHECK(spawned.calls == 1 && spawned.deadline == 5 * NS + GIT_RUN_NS && !spawned.grace &&
	      !spawned.flags && spawned.max == STORE_MAX && !spawned.in && spawned.path);
	CHECK(snprintf(want, sizeof want, "%s cat-file --batch", start) > 0);
	SAME(spawned.argv, strlen(spawned.argv), want);
	CHECK(store_spawn(store, slot, STORE_HEAD, (char const *const[]){ "R" }, nullptr,
	                  (struct span){}, &e) == &test_child && !e);
	CHECK(snprintf(want, sizeof want, "%s rev-parse --verify --end-of-options R", start) > 0);
	SAME(spawned.argv, strlen(spawned.argv), want);

	/* A commit removes a lock file older than the boot before git runs,
	 * and git that writes gets a grace and writes its errors to the
	 * output. */
	char lock[4096];
	CHECK(snprintf(lock, sizeof lock, "%s/refs/heads/notes/t.lock", store_directory(store)) > 0);
	lock_make(lock, 1);
	CHECK(store_spawn(store, slot, STORE_COMMIT, nullptr, "refs/heads/notes/t",
	                  (struct span){ (unsigned char *)"abc", 3 }, &e) == &test_child && !e);
	CHECK(spawned.calls == 3 && spawned.grace == GIT_GRACE && spawned.flags == CHILD_STDERR &&
	      spawned.in == 3);
	CHECK(access(lock, F_OK) && errno == ENOENT);
	CHECK(store_spawn(store, slot, STORE_CREATE, nullptr, nullptr, (struct span){}, &e) ==
	      &test_child && !e);
	CHECK(spawned.calls == 4 && spawned.grace == GIT_GRACE && spawned.flags == CHILD_STDERR);

	/* A recent lock file stays, and store_lock() writes its age. */
	int64_t age = -1;
	CHECK(!store_lock(store, "refs/heads/notes/t", &age) && !age);
	lock_make(lock, 0);
	CHECK(!store_lock(store, "refs/heads/notes/t", &age) && age >= 0 && age <= STORE_STALE);
	CHECK(!access(lock, F_OK));
	CHECK(!unlink(lock));
	CHECK(store_lock(nullptr, "refs/heads/notes/t", &age) == EINVAL);
	CHECK(store_lock(store, nullptr, &age) == EINVAL);
	CHECK(store_lock(store, "refs/heads/notes/t", nullptr) == EINVAL);

	/* Refusals start no git. */
	struct {
		struct store const *store;
		struct mcp_slot    *slot;
		enum store_cmd      cmd;
		char const         *ref;
	} const bad[] = {
		{ nullptr, slot,    STORE_READ,                       nullptr },
		{ store,   nullptr, STORE_READ,                       nullptr },
		{ store,   slot,    STORE_COMMIT,                     nullptr },
		{ store,   slot,    STORE_HEAD,                       nullptr },
		{ store,   slot,    (enum store_cmd)(STORE_GREP + 1), nullptr },
	};
	for (size_t i = 0; i < array_size(bad); ++i) {
		e = -1;
		CHECK(!store_spawn(bad[i].store, bad[i].slot, bad[i].cmd, nullptr, bad[i].ref,
		                   (struct span){}, &e) && e == EINVAL);
	}
	CHECK(spawned.calls == 4);
}

/* store_create() reads the common directory of a clone and of a linked
 * worktree of the clone from git. Both are new directories in @p parent.
 * store_create() refuses a .git file that git cannot resolve with EIO and
 * the error of git.
 */
static void
test_common (char const *parent)
{
	char clone[4096], linked[4096], out[4096];
	size_t len = 0;
	CHECK(snprintf(clone, sizeof clone, "%s/clone", parent) > 0);
	CHECK(snprintf(linked, sizeof linked, "%s/linked", parent) > 0);
	CHECK(!run_words(parent, (char const *const[]){ "init", "--quiet", clone, nullptr }, out,
	                 sizeof out, &len));
	CHECK(!run_words(clone, (char const *const[]){
		"-c", "user.name=t", "-c", "user.email=t", "commit", "--quiet", "--allow-empty",
		"-m", "c", nullptr
	}, out, sizeof out, &len));
	CHECK(!run_words(clone, (char const *const[]){
		"worktree", "add", "--quiet", "--detach", linked, nullptr
	}, out, sizeof out, &len));
	char *git = nullptr;
	CHECK(snprintf(out, sizeof out, "%s/.git", clone) > 0 && (git = realpath(out, nullptr)));
	char const *const paths[] = { clone, linked };
	for (size_t i = 0; git && i < array_size(paths); ++i) {
		char why[512] = "";
		int e = -1;
		struct store *store = store_create(paths[i], why, sizeof why, &e);
		CHECK(store && !e && store_exists(store) && !strcmp(store_directory(store), git));
		store_destroy(&store);
	}
	free(git);
	static struct {
		char const *name;
		char const *data;
		char const *why;
	} const broken[] = {
		{ "gitdir-missing",  "gitdir: /nonexistent/askai\n", "/nonexistent/askai" },
		{ "gitfile-invalid", "askai\n",                      "gitfile" },
	};
	static char const head[] = "cannot resolve the Git common directory: fatal: ";
	for (size_t i = 0; i < array_size(broken); ++i) {
		char dir[4096], file[4096], why[512] = "";
		CHECK(snprintf(dir, sizeof dir, "%s/%s", parent, broken[i].name) > 0);
		CHECK(!mkdir(dir, 0700) && snprintf(file, sizeof file, "%s/.git", dir) > 0);
		FILE *f = fopen(file, "we");
		CHECK(f && fputs(broken[i].data, f) >= 0 && !fclose(f));
		int e = -1;
		CHECK(!store_create(dir, why, sizeof why, &e) && e == EIO);
		CHECK(!strncmp(why, head, sizeof head - 1) && strstr(why, broken[i].why));
		CHECK(!strchr(why, '\n'));
	}
}

static void
test_git (char const *path)
{
	char why[512] = "";
	int e = -1;
	CHECK(!store_create(nullptr, why, sizeof why, &e) && e == EINVAL);
	struct store *store = store_create(path, why, sizeof why, &e);
	CHECK(store && !e);
	if (!store)
		return;
	char *real = realpath(path, nullptr);
	CHECK(real && !strcmp(real, store_directory(store)));
	free(real);
	CHECK(!store_exists(store) && !store_exists(nullptr) && !store_directory(nullptr));

	static char out[1 << 16];
	size_t len = 0;
	CHECK(!run(store, STORE_CREATE, nullptr, (struct span){}, out, sizeof out, &len));
	CHECK(store_exists(store));

	/* A root commit without file parts has the empty tree. */
	struct oid const root = {};
	struct store_commit commit = {
		.committer = { CSTR_VIEW("gpu1"), CSTR_VIEW("askai") },
		.message = CSTR_VIEW("Root"),
		.ref = "refs/heads/notes/t",
		.tip = &root,
	};
	struct oid c1, c2, c3, c4;
	CHECK(!commit_run(store, &commit, &c1, out, sizeof out, &len));
	CHECK(oid_len(&c1) == OID_SHA1);
	CHECK(!run(store, STORE_FILES, (char const *const[]){ c1.hex }, (struct span){}, out,
	           sizeof out, &len) && !len);

	/* A file part adds the file. */
	struct buf files = {};
	struct store_part part;
	CHECK(!store_part_file(&part, &files, CSTR_VIEW("a"), CSTR_VIEW("abc")));
	commit.message = CSTR_VIEW("Add a");
	commit.files = buf_peek(&files);
	commit.tip = &c1;
	CHECK(!commit_run(store, &commit, &c2, out, sizeof out, &len));
	buf_fini(&files);
	static char tree[4096];
	size_t tree_len = 0;
	CHECK(!run(store, STORE_FILES, (char const *const[]){ c2.hex }, (struct span){}, tree,
	           sizeof tree, &tree_len));
	CHECK(tree_len && !tree[tree_len - 1]);
	SAME(tree, tree_len - 1, "100644 blob f2ba8f84ab5c1bce84a7b441cb1959cfc7093b7f       3\ta");

	/* A commit without file parts has the tree of its parent. */
	commit.message = CSTR_VIEW("Keep");
	commit.files = (struct span){};
	commit.tip = &c2;
	CHECK(!commit_run(store, &commit, &c3, out, sizeof out, &len));
	CHECK(!run(store, STORE_FILES, (char const *const[]){ c3.hex }, (struct span){}, out,
	           sizeof out, &len));
	CHECK(len == tree_len && !memcmp(out, tree, len));
	CHECK(!run(store, STORE_HEAD, (char const *const[]){ commit.ref }, (struct span){}, out,
	           sizeof out, &len));
	CHECK(len == OID_SHA1 + 1 && !memcmp(out, c3.hex, OID_SHA1) && out[OID_SHA1] == '\n');

	/* git writes the committer as the author too. */
	char line[OID_SHA1 + 2];
	CHECK(snprintf(line, sizeof line, "%s\n", c3.hex) == OID_SHA1 + 1);
	CHECK(!run(store, STORE_READ, nullptr, (struct span){ (unsigned char *)line, OID_SHA1 + 1 },
	           out, sizeof out, &len));
	CHECK(len > OID_SHA1 && !memcmp(out, c3.hex, OID_SHA1));
	CHECK(memmem(out, len, "\nauthor gpu1 <askai> ", 21));
	CHECK(memmem(out, len, "\ncommitter gpu1 <askai> ", 24));
	CHECK(len > 8 && !memcmp(out + len - 8, "\n\nKeep\n\n", 8));

	/* A commit from an old tip and a root commit on a ref that exists are
	 * races, and the ref stays. */
	commit.message = CSTR_VIEW("Race");
	commit.tip = &c2;
	CHECK(commit_run(store, &commit, &c4, out, sizeof out, &len) > 0);
	struct child c = {};
	CHECK(!buf_put(&c.out, out, len, SIZE_MAX) && store_raced(&c));
	buf_fini(&c.out);
	commit.tip = &root;
	CHECK(commit_run(store, &commit, &c4, out, sizeof out, &len) > 0);
	CHECK(!buf_put(&c.out, out, len, SIZE_MAX) && store_raced(&c));
	buf_fini(&c.out);
	CHECK(!store_raced(nullptr) && !store_raced(&c));
	CHECK(!run(store, STORE_HEAD, (char const *const[]){ commit.ref }, (struct span){}, out,
	           sizeof out, &len));
	CHECK(len == OID_SHA1 + 1 && !memcmp(out, c3.hex, OID_SHA1));

	test_argv(store);
	test_spawn(store);
	store_destroy(&store);
	CHECK(!store);
	store_destroy(&store);
	store_destroy(nullptr);

	/* A nonempty directory that is not a repository is refused. */
	char *parent = strdup(path);
	CHECK(parent && strrchr(parent, '/'));
	if (!parent || !strrchr(parent, '/'))
		return;
	*strrchr(parent, '/') = '\0';
	store = store_create(parent, why, sizeof why, &e);
	CHECK(!store && e == EINVAL && strstr(why, "nonempty directory"));
	test_common(parent);
	free(parent);
}

int
main (int    argc,
      char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s PATH\n", argv[0]);
		return 2;
	}
	test_input();
	test_git(argv[1]);
	return check_report("store");
}
