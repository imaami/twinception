/** @file Unit tests of the git store, src/store.c: the argv of each
 *  command, the checks of store_command(), store_spawn(), store_run() and
 *  store_lock(), the input of git fast-import, the commits that git makes
 *  from that input in a new bare repository, the common directory of a
 *  clone and of a worktree, and the result of store_run() for each way
 *  that git ends. The test replaces mcp_slot_spawn() and mcp_slot_loop().
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
test_trailer (void)
{
	struct oid tip;
	CHECK(!oid_init(&tip, CSTR_VIEW(A)));
	struct oid const root = {};

	/* The author line comes before the committer line. The trailer comes
	 * after a blank line, and the data count includes the line feed after
	 * the message, the blank line and each line of the trailer. */
	struct store_trailer const trailers[] = {
		{ CSTR_VIEW("Asked-by"), CSTR_VIEW("imaami") },
		{ CSTR_VIEW("Ask"),      CSTR_VIEW("") },
	};
	struct store_commit const commit = {
		.author = { CSTR_VIEW("bro"), CSTR_VIEW("agent/bro") },
		.committer = { CSTR_VIEW("gpu1"), CSTR_VIEW("askai") },
		.message = CSTR_VIEW(""),
		.trailers = trailers,
		.n_trailers = array_size(trailers),
		.ref = "refs/heads/notes/t",
		.tip = &root,
	};
	struct buf in = {};
	CHECK(!store_commit_input(&commit, &in));
	struct span s = buf_peek(&in);
	SAME(s.ptr, s.len, "commit refs/heads/notes/t\nmark :1\nauthor bro <agent/bro> now\n"
	     "committer gpu1 <askai> now\ndata 25\n\n\nAsked-by: imaami\nAsk: \n"
	     "get-mark :1\n");
	buf_fini(&in);

	/* A member record: no author, a message, a trailer and a parent. */
	struct store_trailer const record[] = {
		{ CSTR_VIEW("Kind"),  CSTR_VIEW("agent") },
		{ CSTR_VIEW("Login"), CSTR_VIEW("imaami") },
	};
	struct store_commit c = commit;
	c.author = (struct store_ident){};
	c.message = CSTR_VIEW("Add the member bro");
	c.trailers = record;
	c.n_trailers = array_size(record);
	c.ref = "refs/askai/members/bro";
	c.tip = &tip;
	CHECK(!store_commit_input(&c, &in));
	s = buf_peek(&in);
	SAME(s.ptr, s.len, "commit refs/askai/members/bro\nmark :1\ncommitter gpu1 <askai> now\n"
	     "data 46\nAdd the member bro\n\nKind: agent\nLogin: imaami\nfrom " A "\n"
	     "get-mark :1\n");
	buf_fini(&in);

	/* An author without a name and trailers without a count are left
	 * out. */
	c = commit;
	c.author = (struct store_ident){ {}, CSTR_VIEW("x<y") };
	c.n_trailers = 0;
	CHECK(!store_commit_input(&c, &in));
	s = buf_peek(&in);
	SAME(s.ptr, s.len, "commit refs/heads/notes/t\nmark :1\ncommitter gpu1 <askai> now\n"
	     "data 1\n\nget-mark :1\n");
	buf_fini(&in);

	struct cstr_view const idents[] = {
		{}, { nullptr, 5 }, CSTR_VIEW("a\0b"), CSTR_VIEW("a<b"), CSTR_VIEW("a>b"),
		CSTR_VIEW("a\nb"),
	};
	for (size_t i = 0; i < array_size(idents); ++i) {
		c = commit;
		c.author.email = idents[i];
		CHECK(refused(c));
		c = commit;
		c.author.name = idents[i];
		/* An author without a name is left out. */
		CHECK(refused(c) == (idents[i].len != 0));
	}
	char word[UTIL_WORD_MAX + 1];
	memset(word, 'k', sizeof word);
	struct cstr_view const keys[] = {
		{}, { nullptr, 5 }, CSTR_VIEW(""), CSTR_VIEW("Asked by"), CSTR_VIEW("Ask:"),
		CSTR_VIEW("a\nb"), CSTR_VIEW("a\0b"), CSTR_VIEW("Ask\xc3\xa4"),
		{ word, UTIL_WORD_MAX + 1 },
	};
	for (size_t i = 0; i < array_size(keys); ++i) {
		struct store_trailer const pair[] = { trailers[0], { keys[i], CSTR_VIEW("v") } };
		c = commit;
		c.trailers = pair;
		CHECK(refused(c));
	}
	struct store_trailer t[] = { trailers[0], { { word, UTIL_WORD_MAX }, CSTR_VIEW("v") } };
	c = commit;
	c.trailers = t;
	CHECK(!store_commit_input(&c, &in));
	buf_fini(&in);
	char big[STORE_MESSAGE];
	memset(big, 'v', sizeof big);
	struct cstr_view const values[] = {
		{}, { nullptr, 5 }, CSTR_VIEW("x\ny"), CSTR_VIEW("x\0y"), { big, STORE_MESSAGE },
	};
	for (size_t i = 0; i < array_size(values); ++i) {
		t[1] = (struct store_trailer){ CSTR_VIEW("Key"), values[i] };
		CHECK(refused(c));
	}
	t[1] = (struct store_trailer){ CSTR_VIEW("Key"), { big, STORE_MESSAGE - 1 } };
	CHECK(!store_commit_input(&c, &in));
	buf_fini(&in);
	c = commit;
	c.trailers = nullptr;
	CHECK(refused(c));
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

/* store_run() runs git on the calling thread, after the lock check of a
 * commit, and appends the output to its buffer. The ref
 * refs/heads/notes/t of @p store is at the commit @p tip.
 */
static void
test_run (struct store const *store,
          struct oid const   *tip)
{
	struct buf out = {};
	int status = -1;
	CHECK(!buf_put(&out, "x", 1, SIZE_MAX));
	CHECK(!store_run(store, STORE_HEAD, (char const *const[]){ "refs/heads/notes/t" }, nullptr,
	                 (struct span){}, &out, &status) && !status);
	struct span s = buf_peek(&out);
	CHECK(s.len == OID_SHA1 + 2 && s.ptr[0] == 'x' && !memcmp(s.ptr + 1, tip->hex, OID_SHA1));
	buf_clear(&out);

	/* A commit removes a lock file older than the boot, and a root commit
	 * on a ref that exists fails with the errors in the output. */
	char lock[4096];
	CHECK(snprintf(lock, sizeof lock, "%s/refs/heads/notes/t.lock", store_directory(store)) > 0);
	lock_make(lock, 1);
	struct oid const root = {};
	struct store_commit const commit = {
		.committer = { CSTR_VIEW("gpu1"), CSTR_VIEW("askai") },
		.message = CSTR_VIEW("Race"),
		.ref = "refs/heads/notes/t",
		.tip = &root,
	};
	struct buf in = {};
	CHECK(!store_commit_input(&commit, &in));
	CHECK(!store_run(store, STORE_COMMIT, nullptr, commit.ref, buf_peek(&in), &out, &status) &&
	      status == 1);
	buf_fini(&in);
	CHECK(access(lock, F_OK) && errno == ENOENT);
	CHECK(store_raced(buf_peek(&out)));
	buf_fini(&out);

	/* Refusals run no git. */
	CHECK(store_run(store, STORE_COMMIT, nullptr, nullptr, (struct span){}, &out, &status) ==
	      EINVAL);
	CHECK(store_run(store, STORE_HEAD, nullptr, nullptr, (struct span){}, &out, &status) ==
	      EINVAL);
	CHECK(store_run(nullptr, STORE_READ, nullptr, nullptr, (struct span){}, &out, &status) ==
	      EINVAL);
	CHECK(store_run(store, STORE_READ, nullptr, nullptr, (struct span){}, nullptr, &status) ==
	      EINVAL);
	CHECK(store_run(store, STORE_READ, nullptr, nullptr, (struct span){}, &out, nullptr) ==
	      EINVAL);
	CHECK(!buf_peek(&out).len);
	buf_fini(&out);
}

/* store_init() creates a new repository in @p parent once. store_run()
 * returns 0 with the exit status of a git that fails, ENOBUFS for an
 * output longer than STORE_MAX with the first STORE_MAX bytes in its
 * buffer, EIO for a git that a signal killed, and ETIMEDOUT for a git that
 * runs past its deadline. In the last three cases, a script named git in a
 * new directory at the start of PATH runs in place of git.
 */
static void
test_ends (char const *parent)
{
	char path[4096];
	CHECK(snprintf(path, sizeof path, "%s/ends.git", parent) > 0);
	char why[512] = "";
	int e = -1;
	struct store *store = store_create(path, why, sizeof why, &e);
	CHECK(store && !e && !store_exists(store));
	if (!store)
		return;
	struct buf out = {};
	int status = -1;
	CHECK(!store_init(store, &out, &status) && !status && store_exists(store));
	buf_clear(&out);
	status = -1;
	CHECK(!store_init(store, &out, &status) && !status && !buf_peek(&out).len);
	CHECK(store_init(nullptr, &out, &status) == EINVAL);
	CHECK(store_init(store, nullptr, &status) == EINVAL);
	CHECK(store_init(store, &out, nullptr) == EINVAL);
	CHECK(!store_run(store, STORE_HEAD, (char const *const[]){ "refs/heads/none" }, nullptr,
	                 (struct span){}, &out, &status) && status == 128);

	/* After each error, out contains only the byte that the test puts
	 * there before the run. */
	struct {
		char const *name;
		char const *script;
		int         e;
	} const fakes[] = {
		{ "cut",  "exec head -c 16777217 /dev/zero\n", ENOBUFS },
		{ "kill", "kill -KILL $$\n",                   EIO },
		{ "late", "exec sleep 60\n",                   ETIMEDOUT },
	};
	char *old = strdup(getenv("PATH"));
	CHECK(old);
	for (size_t i = 0; old && i < array_size(fakes); ++i) {
		char dir[4096], file[4096], env[8192];
		CHECK(snprintf(dir, sizeof dir, "%s/%s", parent, fakes[i].name) > 0);
		CHECK(!mkdir(dir, 0700));
		CHECK(snprintf(file, sizeof file, "%s/git", dir) > 0);
		FILE *f = fopen(file, "we");
		CHECK(f && fprintf(f, "#!/bin/sh\n%s", fakes[i].script) > 0 && !fclose(f));
		CHECK(!chmod(file, 0700));
		CHECK(snprintf(env, sizeof env, "%s:%s", dir, old) > 0 && !setenv("PATH", env, 1));
		buf_clear(&out);
		CHECK(!buf_put(&out, "x", 1, SIZE_MAX));
		e = store_run(store, STORE_READ, nullptr, nullptr, (struct span){}, &out, &status);
		CHECK(!setenv("PATH", old, 1));
		struct span const got = buf_peek(&out);
		CHECK(e == fakes[i].e && got.len == 1 && *got.ptr == 'x');
	}
	free(old);
	buf_fini(&out);
	store_destroy(&store);
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
	CHECK(store_raced((struct span){ (unsigned char *)out, len }));
	commit.tip = &root;
	CHECK(commit_run(store, &commit, &c4, out, sizeof out, &len) > 0);
	CHECK(store_raced((struct span){ (unsigned char *)out, len }));
	CHECK(!store_raced((struct span){}));
	CHECK(!store_raced((struct span){ (unsigned char *)"Saved", 5 }));
	/* Two root commits of one ref at the same time make git say this. */
	static char const exists[] = "error: cannot lock ref 'refs/askai/members/bro': reference "
	                             "already exists\n";
	CHECK(store_raced((struct span){ (unsigned char *)exists, sizeof exists - 1 }));
	CHECK(!run(store, STORE_HEAD, (char const *const[]){ commit.ref }, (struct span){}, out,
	           sizeof out, &len));
	CHECK(len == OID_SHA1 + 1 && !memcmp(out, c3.hex, OID_SHA1));

	/* git reads the author, the committer and the trailer of a commit
	 * with a parent and a file part. */
	struct store_trailer const asked[] = {
		{ CSTR_VIEW("Ask"),      CSTR_VIEW("6f0c2b9a41d8e3f75a2c1b0d9e8f7a6b") },
		{ CSTR_VIEW("Asked-by"), CSTR_VIEW("imaami") },
	};
	CHECK(!store_part_file(&part, &files, CSTR_VIEW("b"), CSTR_VIEW("x")));
	commit.author = (struct store_ident){ CSTR_VIEW("bro"), CSTR_VIEW("agent/bro") };
	commit.message = CSTR_VIEW("Add b (1 byte)");
	commit.files = buf_peek(&files);
	commit.trailers = asked;
	commit.n_trailers = array_size(asked);
	commit.tip = &c3;
	struct oid c5;
	CHECK(!commit_run(store, &commit, &c5, out, sizeof out, &len));
	buf_fini(&files);
	char const *const log[] = {
		"log", "-1",
		"--format=%(trailers:key=Asked-by,valueonly)|%an <%ae>|%cn <%ce>|%P|%B", c5.hex,
		nullptr
	};
	char want[512];
	CHECK(snprintf(want, sizeof want, "imaami\n|bro <agent/bro>|gpu1 <askai>|%s|Add b (1 byte)\n\n"
	               "Ask: 6f0c2b9a41d8e3f75a2c1b0d9e8f7a6b\nAsked-by: imaami\n\n", c3.hex) > 0);
	CHECK(!run_words(store_directory(store), log, out, sizeof out, &len));
	SAME(out, len, want);
	CHECK(!run(store, STORE_FILES, (char const *const[]){ c5.hex }, (struct span){}, out,
	           sizeof out, &len));
	CHECK(len > tree_len && !memcmp(out, tree, tree_len) && memmem(out, len, "\tb", 3));

	/* A member record is a root commit with the empty tree, and git reads
	 * its trailer. */
	struct store_trailer const record[] = {
		{ CSTR_VIEW("Kind"),  CSTR_VIEW("agent") },
		{ CSTR_VIEW("Login"), CSTR_VIEW("imaami") },
	};
	struct store_commit const member = {
		.author = { CSTR_VIEW("bro"), CSTR_VIEW("agent/bro") },
		.committer = { CSTR_VIEW("gpu1"), CSTR_VIEW("askai") },
		.message = CSTR_VIEW("Add the member bro"),
		.trailers = record,
		.n_trailers = array_size(record),
		.ref = "refs/askai/members/bro",
		.tip = &root,
	};
	struct oid m;
	CHECK(!commit_run(store, &member, &m, out, sizeof out, &len));
	CHECK(!run(store, STORE_FILES, (char const *const[]){ m.hex }, (struct span){}, out,
	           sizeof out, &len) && !len);
	char const *const show[] = {
		"log", "-1", "--format=%(trailers:only)|%an <%ae>|%cn <%ce>|%P|%B", m.hex, nullptr
	};
	CHECK(!run_words(store_directory(store), show, out, sizeof out, &len));
	SAME(out, len, "Kind: agent\nLogin: imaami\n|bro <agent/bro>|gpu1 <askai>||"
	     "Add the member bro\n\nKind: agent\nLogin: imaami\n\n");

	test_argv(store);
	test_spawn(store);
	test_run(store, &c5);
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
	test_ends(parent);
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
	test_trailer();
	test_git(argv[1]);
	return check_report("store");
}
