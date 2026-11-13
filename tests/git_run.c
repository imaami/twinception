/** @file Unit tests of the argv and the environment of git, src/git_run.c:
 *  the argv of each shape of a struct git_run_dir word for word, the
 *  environment in order, the limits of both, the copy of the struct
 *  child_cmd in git_run_init(), the paths that git_run_path() reads, the
 *  names of repositories that git_run_name() and git_run_name_valid()
 *  accept, and git run through child_capture(), child_cmd_run() and
 *  git_run_now() in a new bare repository at the path of the argument.
 */
#define _GNU_SOURCE /* pipe2(), memfd_create() */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buf.h"
#include "check.h"
#include "child.h"
#include "clk.h"
#include "cstr_view.h"
#include "git_run.h"
#include "loop.h"
#include "oid.h"
#include "util.h"

/* "git" and the words of GIT_OPTIONS_FIXED, spelled out. */
static char const *const fixed[] = {
	"git", "--no-pager", "--no-replace-objects",
	"-c", "core.fsmonitor=false",
	"-c", "core.hooksPath=/dev/null",
	"-c", "gpg.program=false",
	"-c", "gpg.ssh.program=false",
	"-c", "gpg.x509.program=false",
	"-c", "protocol.allow=never",
	"-c", "gc.auto=0",
	"-c", "maintenance.auto=false",
	"-c", "log.showSignature=false",
};

/* The entries of GIT_ENV_FIXED, spelled out. */
static char const *const fixed_env[] = {
	"HOME=/nonexistent", "LC_ALL=C.UTF-8", "GIT_CONFIG_NOSYSTEM=1",
	"GIT_CONFIG_GLOBAL=/dev/null", "GIT_TERMINAL_PROMPT=0", "GIT_PAGER=cat",
	"GIT_NO_LAZY_FETCH=1", "GIT_ALLOW_PROTOCOL=none",
};

/* Returns the number of words at @p words up to their nullptr. */
static size_t
git_run_test_count (char const *const *words)
{
	size_t n = 0;
	while (words[n])
		++n;
	return n;
}

/* Returns true if @p argv is "git", the words of GIT_OPTIONS_FIXED, the
 * words at @p tail and the words at @p words, each up to their nullptr,
 * and a nullptr.
 */
static bool
git_run_test_argv (char const *const *argv,
                   char const *const *tail,
                   char const *const *words)
{
	for (size_t i = 0; i < array_size(fixed); ++i, ++argv)
		if (!*argv || strcmp(*argv, fixed[i]))
			return false;
	for (char const *const *const *part = (char const *const *const[]){ tail, words, nullptr };
	     *part; ++part)
		for (char const *const *w = *part; *w; ++w, ++argv)
			if (!*argv || strcmp(*argv, *w))
				return false;
	return !*argv;
}

/* Returns true if the readable bytes of @p out are @p text. */
static bool
git_run_test_out (struct buf const *out,
                  struct cstr_view  text)
{
	struct span const got = buf_peek(out);
	return cstr_view_equal((struct cstr_view){ (char const *)got.ptr, got.len }, text);
}

/* Tests git_run_dir_argv() and git_run_init() for each shape of a struct
 * git_run_dir: with -C and a path or in the directory of the child, with
 * and without "--git-dir=.", and with and without options. The argv is
 * checked word for word, alone and with the words of a command, and with
 * a cap that leaves no room for the nullptr.
 */
static void
git_run_test_shapes (void)
{
	static char const *const opts[] = {
		"-c", "core.bare=true", "--literal-pathspecs", nullptr
	};
	static char const *const words[] = { "log", "--oneline", nullptr };
	static struct {
		char const        *path;
		char const *const *opts;
		uint32_t           flags;
		char const        *tail[8];
	} const shapes[] = {
		{ nullptr, nullptr, 0,           { nullptr } },
		{ nullptr, nullptr, GIT_RUN_DOT, { "--git-dir=.", nullptr } },
		{ "/r",    nullptr, 0,           { "-C", "/r", nullptr } },
		{ "/r",    nullptr, GIT_RUN_DOT, { "-C", "/r", "--git-dir=.", nullptr } },
		{ nullptr, opts,    0,           { "-c", "core.bare=true", "--literal-pathspecs",
		                                   nullptr } },
		{ nullptr, opts,    GIT_RUN_DOT, { "-c", "core.bare=true", "--literal-pathspecs",
		                                   "--git-dir=.", nullptr } },
		{ "/r",    opts,    GIT_RUN_DOT, { "-c", "core.bare=true", "--literal-pathspecs",
		                                   "-C", "/r", "--git-dir=.", nullptr } },
	};
	static char const *const none[] = { nullptr };
	for (size_t i = 0; i < array_size(shapes); ++i) {
		struct git_run_dir dir;
		CHECK(!git_run_dir_init(&dir, shapes[i].path, shapes[i].opts, nullptr,
		                        shapes[i].flags));
		CHECK(dir.path == shapes[i].path && dir.opts == shapes[i].opts &&
		      dir.flags == shapes[i].flags);
		char const *argv[GIT_RUN_ARGV];
		CHECK(!git_run_dir_argv(&dir, nullptr, argv, array_size(argv)));
		CHECK(git_run_test_argv(argv, shapes[i].tail, none));
		CHECK(!git_run_dir_argv(&dir, words, argv, array_size(argv)));
		CHECK(git_run_test_argv(argv, shapes[i].tail, words));
		/* The words and the nullptr fit in n + 1 pointers, not in n. */
		size_t const n = array_size(fixed) + git_run_test_count(shapes[i].tail);
		CHECK(!git_run_dir_argv(&dir, nullptr, argv, n + 1));
		CHECK(git_run_test_argv(argv, shapes[i].tail, none));
		CHECK(git_run_dir_argv(&dir, nullptr, argv, n) == E2BIG);
		CHECK(git_run_dir_argv(&dir, words, argv, n + 2) == E2BIG);
		CHECK(!git_run_dir_argv(&dir, words, argv, n + 2 + 1));
		CHECK(git_run_test_argv(argv, shapes[i].tail, words));
		CHECK(git_run_dir_argv(&dir, nullptr, argv, 0) == E2BIG);
		struct git_run run;
		CHECK(!git_run_init(&run, &dir, words, &(struct child_cmd){}));
		CHECK(git_run_test_argv(run.argv, shapes[i].tail, words));
		CHECK(run.c.argv == run.argv && run.c.envp == dir.env);
	}
}

/* Tests the environment of a struct git_run_dir: PATH, the entries of
 * GIT_ENV_FIXED, the entries of the caller in their order, and nullptr.
 * More than 3 entries of the caller are E2BIG, which leaves the struct
 * git_run_dir all zero. A flag other than GIT_RUN_DOT is EINVAL, which
 * leaves the struct git_run_dir as it was.
 */
static void
git_run_test_env (void)
{
	char *const path = util_env_path();
	struct git_run_dir dir;
	CHECK(!git_run_dir_init(&dir, nullptr, nullptr, nullptr, 0));
	CHECK(!strncmp(dir.env[0], "PATH=", 5) && !strcmp(dir.env[0], path));
	for (size_t i = 0; i < array_size(fixed_env); ++i)
		CHECK(!strcmp(dir.env[1 + i], fixed_env[i]));
	for (size_t i = 1 + array_size(fixed_env); i < array_size(dir.env); ++i)
		CHECK(!dir.env[i]);
	char *const three[] = { "A=1", "B=2", "C=3", nullptr };
	CHECK(!git_run_dir_init(&dir, "/r", nullptr, three, GIT_RUN_DOT));
	CHECK(!strcmp(dir.env[0], path));
	for (size_t i = 0; i < array_size(fixed_env); ++i)
		CHECK(!strcmp(dir.env[1 + i], fixed_env[i]));
	for (size_t i = 0; i < 3; ++i)
		CHECK(dir.env[1 + array_size(fixed_env) + i] == three[i]);
	CHECK(array_size(dir.env) == 1 + array_size(fixed_env) + 3 + 1 &&
	      !dir.env[array_size(dir.env) - 1]);
	char *const four[] = { "A=1", "B=2", "C=3", "D=4", nullptr };
	CHECK(git_run_dir_init(&dir, "/r", nullptr, four, GIT_RUN_DOT) == E2BIG);
	CHECK(!dir.path && !dir.opts && !dir.flags);
	for (size_t i = 0; i < array_size(dir.env); ++i)
		CHECK(!dir.env[i]);
	dir.flags = GIT_RUN_DOT;
	CHECK(git_run_dir_init(&dir, "/r", nullptr, nullptr, 1u << 1) == EINVAL);
	CHECK(dir.flags == GIT_RUN_DOT && !dir.path);
	CHECK(git_run_dir_init(nullptr, "/r", nullptr, nullptr, 0) == EINVAL);
}

/* Tests git_run_init(). The struct child_cmd of the result is a copy of
 * the struct child_cmd that the test passes, with the file, the argv and
 * the environment of git in place of the ones of the test. Every pointer
 * of run.argv after the nullptr of the argv is nullptr, although each
 * pointer of run.argv pointed at a word before the call. An argv of
 * GIT_RUN_ARGV pointers fits. After one word too many, the function
 * returns E2BIG and the struct git_run is all zero. After EINVAL for a
 * nullptr, the struct git_run is as it was.
 */
static void
git_run_test_init (void)
{
	struct git_run_dir dir;
	CHECK(!git_run_dir_init(&dir, "/r", nullptr, nullptr, GIT_RUN_DOT));
	static char const *const words[] = { "hash-object", "--stdin", nullptr };
	struct child_cmd const how = {
		.file     = "/nonexistent/askai-test",
		.argv     = (char const *const[]){ "askai-test", nullptr },
		.envp     = (char *const[]){ "A=1", nullptr },
		.in       = { (unsigned char *)"abc", 3 },
		.max      = 7,
		.deadline = 123,
		.grace    = GIT_GRACE,
		.flags    = CHILD_STDERR,
	};
	static char const *const tail[] = { "-C", "/r", "--git-dir=.", nullptr };
	struct git_run run = { .c = how };
	for (size_t i = 0; i < array_size(run.argv); ++i)
		run.argv[i] = "old";
	CHECK(!git_run_init(&run, &dir, words, &how));
	CHECK(!strcmp(run.c.file, "git") && run.c.argv == run.argv && run.c.envp == dir.env);
	CHECK(run.c.in.ptr == how.in.ptr && run.c.in.len == 3 && run.c.max == 7 &&
	      run.c.deadline == 123 && run.c.grace == GIT_GRACE && run.c.flags == CHILD_STDERR);
	CHECK(git_run_test_argv(run.argv, tail, words));
	for (size_t i = git_run_test_count(run.argv); i < array_size(run.argv); ++i)
		CHECK(!run.argv[i]);
	/* The words that fill the argv to its last pointer, then one more. */
	size_t const n = array_size(fixed) + 3;
	char const *many[GIT_RUN_ARGV];
	size_t const fit = array_size(run.argv) - n - 1;
	for (size_t i = 0; i < fit + 1; ++i)
		many[i] = "x";
	many[fit] = nullptr;
	CHECK(!git_run_init(&run, &dir, many, &how));
	CHECK(git_run_test_count(run.argv) == array_size(run.argv) - 1 &&
	      !run.argv[array_size(run.argv) - 1]);
	many[fit] = "x";
	many[fit + 1] = nullptr;
	CHECK(git_run_init(&run, &dir, many, &how) == E2BIG);
	CHECK(!run.c.file && !run.c.argv && !run.c.envp && !run.c.in.ptr && !run.c.in.len &&
	      !run.c.max && !run.c.deadline && !run.c.grace && !run.c.flags);
	for (size_t i = 0; i < array_size(run.argv); ++i)
		CHECK(!run.argv[i]);
	CHECK(!git_run_init(&run, &dir, words, &how));
	CHECK(git_run_init(nullptr, &dir, words, &how) == EINVAL);
	CHECK(git_run_init(&run, nullptr, words, &how) == EINVAL);
	CHECK(git_run_init(&run, &dir, nullptr, &how) == EINVAL);
	CHECK(git_run_init(&run, &dir, words, nullptr) == EINVAL);
	CHECK(!strcmp(run.c.file, "git") && run.c.argv == run.argv && run.c.envp == dir.env &&
	      run.c.max == 7 && git_run_test_argv(run.argv, tail, words));
	CHECK(git_run_dir_argv(nullptr, words, run.argv, array_size(run.argv)) == EINVAL);
	CHECK(git_run_dir_argv(&dir, words, nullptr, array_size(run.argv)) == EINVAL);
}

/* Tests git_run_path(): two absolute paths in a row, then the rest of the
 * output; EINVAL for a relative path, an empty line, a line without its
 * line feed, a NUL in the path and each nullptr.
 */
static void
git_run_test_path (void)
{
	struct cstr_view rest = CSTR_VIEW("/a b\n/c\nd\n");
	struct cstr_view path = {};
	CHECK(!git_run_path(&rest, &path) && cstr_view_equal(path, CSTR_VIEW("/a b")));
	CHECK(!git_run_path(&rest, &path) && cstr_view_equal(path, CSTR_VIEW("/c")));
	CHECK(cstr_view_equal(rest, CSTR_VIEW("d\n")));
	static struct cstr_view const bad[] = {
		CSTR_VIEW_INIT("d\n"),
		CSTR_VIEW_INIT("\n/a\n"),
		CSTR_VIEW_INIT("/a"),
		CSTR_VIEW_INIT("/a\0b\n"),
		{},
	};
	for (size_t i = 0; i < array_size(bad); ++i) {
		rest = bad[i];
		CHECK(git_run_path(&rest, &path) == EINVAL);
	}
	rest = CSTR_VIEW("/a\n");
	CHECK(git_run_path(nullptr, &path) == EINVAL);
	CHECK(git_run_path(&rest, nullptr) == EINVAL);
	CHECK(cstr_view_equal(rest, CSTR_VIEW("/a\n")));
}

/* Tests git_run_name() and git_run_name_valid(). The name of a repository
 * is the last component of its path, without the slashes after it and
 * without one .git suffix. git_run_name() returns the name as a view of
 * the bytes of the path. A valid name is 1 to 64 bytes of [A-Za-z0-9_.-]
 * and does not start with a dot. The view of a path without a valid name
 * is empty. git_run_name_valid() reads only the bytes of the view.
 */
static void
git_run_test_name (void)
{
	static struct {
		char const       *path;
		size_t            at;    /* the offset of the name in the path */
		struct cstr_view  name;  /* empty for a path without a name */
	} const cases[] = {
		{ "/srv/askai",        5, CSTR_VIEW_INIT("askai") },
		{ "/srv/askai.git",    5, CSTR_VIEW_INIT("askai") },
		{ "/srv/askai/",       5, CSTR_VIEW_INIT("askai") },
		{ "/srv/askai.git///", 5, CSTR_VIEW_INIT("askai") },
		{ "/srv//askai",       6, CSTR_VIEW_INIT("askai") },
		{ "askai",             0, CSTR_VIEW_INIT("askai") },
		{ "askai.git",         0, CSTR_VIEW_INIT("askai") },
		{ "./askai",           2, CSTR_VIEW_INIT("askai") },
		{ "/askai",            1, CSTR_VIEW_INIT("askai") },
		{ "/srv/a",            5, CSTR_VIEW_INIT("a") },
		{ "/srv/a.git",        5, CSTR_VIEW_INIT("a") },
		{ "/srv/A-b_c.d9",     5, CSTR_VIEW_INIT("A-b_c.d9") },
		{ "/srv/-x",           5, CSTR_VIEW_INIT("-x") },
		{ "/srv/_x",           5, CSTR_VIEW_INIT("_x") },
		{ "/srv/c7",           5, CSTR_VIEW_INIT("c7") },
		{ "/srv/x.",           5, CSTR_VIEW_INIT("x.") },
		{ "/srv/x..y",         5, CSTR_VIEW_INIT("x..y") },
		{ "/srv/x.lock",       5, CSTR_VIEW_INIT("x.lock") },
		/* One .git suffix in lower case is not part of the name. */
		{ "/srv/x.git.git",    5, CSTR_VIEW_INIT("x.git") },
		{ "/srv/x.GIT",        5, CSTR_VIEW_INIT("x.GIT") },
		{ "/srv/x.gitx",       5, CSTR_VIEW_INIT("x.gitx") },
		{ "/srv/xgit",         5, CSTR_VIEW_INIT("xgit") },
		{ "/srv/git",          5, CSTR_VIEW_INIT("git") },
		/* A last component that starts with a dot is no name, with
		 * and without the suffix. */
		{ "/srv/askai/.git",   0, {} },
		{ "/srv/..git",        0, {} },
		{ "/srv/.askai",       0, {} },
		{ "/srv/.askai.git",   0, {} },
		{ "/srv/.",            0, {} },
		{ "/srv/..",           0, {} },
		/* A path without a last component. */
		{ "",                  0, {} },
		{ "/",                 0, {} },
		{ "//",                0, {} },
		/* A byte that is not one of [A-Za-z0-9_.-]. */
		{ "/srv/a b",          0, {} },
		{ "/srv/a b.git",      0, {} },
		{ "/srv/a\tb",         0, {} },
		{ "/srv/a@b",          0, {} },
		{ "/srv/a+b",          0, {} },
		{ "/srv/a:b",          0, {} },
		{ "/srv/a~",           0, {} },
		{ "/srv/caf\xc3\xa9",  0, {} },
	};
	for (size_t i = 0; i < array_size(cases); ++i) {
		struct cstr_view const name = git_run_name(cases[i].path);
		CHECK(cstr_view_equal(name, cases[i].name));
		if (cases[i].name.len)
			CHECK(name.data == cases[i].path + cases[i].at && git_run_name_valid(name));
		else
			CHECK(!name.data);
	}
	/* A name of 64 bytes is valid and a name of 65 bytes is not, with
	 * and without the suffix and a slash. */
	static char const x65[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
	                          "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
	static_assert(sizeof x65 == 65 + 1, "a name of 65 bytes");
	static char const *const tails[] = { "", ".git", "/", ".git/" };
	for (size_t i = 0; i < array_size(tails); ++i) {
		char path[80];
		int len = snprintf(path, sizeof path, "/srv/%.64s%s", x65, tails[i]);
		CHECK(len > 0 && len < (int)sizeof path);
		struct cstr_view name = git_run_name(path);
		CHECK(name.data == path + 5 && name.len == 64);
		len = snprintf(path, sizeof path, "/srv/%s%s", x65, tails[i]);
		CHECK(len > 0 && len < (int)sizeof path);
		name = git_run_name(path);
		CHECK(!name.data && !name.len);
	}
	static struct {
		struct cstr_view name;
		bool             valid;
	} const names[] = {
		{ CSTR_VIEW_INIT("a"),      true },
		{ CSTR_VIEW_INIT("a.git"),  true },
		{ CSTR_VIEW_INIT("-"),      true },
		{ CSTR_VIEW_INIT("_"),      true },
		{ CSTR_VIEW_INIT("0"),      true },
		{ CSTR_VIEW_INIT("a.b-c_"), true },
		{ {},                       false },
		{ CSTR_VIEW_INIT(""),       false },
		{ CSTR_VIEW_INIT("."),      false },
		{ CSTR_VIEW_INIT(".a"),     false },
		{ CSTR_VIEW_INIT("a/b"),    false },
		{ CSTR_VIEW_INIT("a/"),     false },
		{ CSTR_VIEW_INIT("a\0b"),   false },
		{ CSTR_VIEW_INIT("a\n"),    false },
		{ CSTR_VIEW_INIT("a b"),    false },
	};
	for (size_t i = 0; i < array_size(names); ++i)
		CHECK(git_run_name_valid(names[i].name) == names[i].valid);
	/* The bytes after the view are not part of the name. */
	CHECK(git_run_name_valid((struct cstr_view){ "ab/c", 2 }));
	CHECK(!git_run_name_valid((struct cstr_view){ "ab/c", 3 }));
	CHECK(git_run_name_valid((struct cstr_view){ x65, 64 }));
	CHECK(!git_run_name_valid((struct cstr_view){ x65, 65 }));
}

/* Runs @p words with git in @p dir with the descriptors @p fds, the
 * input, the cap, the grace and the flags of @p how, and a deadline of
 * how->deadline ms from now, or 10 s if that is 0. git runs through
 * child_capture(), which appends the output to @p out, or through
 * child_cmd_run() without a buffer if @p out is nullptr. Returns what
 * child_capture() or child_cmd_run() returns, and writes how git ended to
 * @p *end and how long it ran to @p *ms.
 */
static int
git_run_test_git (struct git_run_dir const *dir,
                  char const *const        *words,
                  struct child_cmd          how,
                  struct child_fds          fds,
                  struct buf               *out,
                  struct child_end         *end,
                  uint64_t                 *ms)
{
	uint64_t const start = loop_clock();
	how.deadline = start + (how.deadline ? how.deadline * MS : 10 * NS);
	struct git_run run;
	int e = git_run_init(&run, dir, words, &how);
	CHECK(!e);
	*end = child_end(.code = -1, .status = -1, .flags = UINT32_MAX);
	if (!e)
		e = out ? child_capture(&run.c, fds, out, end)
		        : child_cmd_run(&run.c, fds, nullptr, end);
	*ms = (loop_clock() - start) / MS;
	return e;
}

/* Tests git_run_now() with git in @p dir, an empty repository. The
 * deadline of the struct child_cmd is a time in the past, and
 * git_run_now() runs each command to its end. The cases with a name to
 * read are the name of an object, an empty name for the exit status 1,
 * EIO for the exit status 128 and for an output that is not one name and
 * a line feed, the EFBIG of a cut output, and EIO for a git that a signal
 * kills. The signal number of SIGHUP is 1, and that end is not the exit
 * status 1. The output replaces the bytes that were in the buffer, and
 * the name is empty after an error. Without a name to read, the result is
 * 0 for the exit status 0 and EIO for the exit status 1, with the output
 * in the buffer, or in fds.out without a buffer. An error of
 * git_run_init() leaves the buffer and the name as they were, and so does
 * the EINVAL of a name to read without a buffer. git_run_now() ends a git
 * that waits for its input GIT_RUN_NS after the call.
 */
static void
git_run_test_now (struct git_run_dir const *dir)
{
	static struct {
		char const       *words[6];
		struct cstr_view  in;
		size_t            max;
		int               e;
		struct cstr_view  out;
	} const cases[] = {
		{ { "hash-object", "--stdin", nullptr }, CSTR_VIEW_INIT("abc"), 4096, 0,
		  CSTR_VIEW_INIT("f2ba8f84ab5c1bce84a7b441cb1959cfc7093b7f\n") },
		{ { "rev-parse", "--verify", "--quiet", "refs/heads/missing", nullptr }, {}, 4096, 0,
		  {} },
		{ { "rev-parse", "--verify", "missing", nullptr }, {}, 4096, EIO, {} },
		{ { "rev-parse", "--is-bare-repository", nullptr }, {}, 4096, EIO,
		  CSTR_VIEW_INIT("true\n") },
		{ { "rev-parse", "f2ba8f84ab5c1bce84a7b441cb1959cfc7093b7f",
		    "f2ba8f84ab5c1bce84a7b441cb1959cfc7093b7f", nullptr }, {}, 4096, EIO,
		  CSTR_VIEW_INIT("f2ba8f84ab5c1bce84a7b441cb1959cfc7093b7f\n"
		                 "f2ba8f84ab5c1bce84a7b441cb1959cfc7093b7f\n") },
		{ { "hash-object", "--stdin", nullptr }, CSTR_VIEW_INIT("abc"), 40, EFBIG,
		  CSTR_VIEW_INIT("f2ba8f84ab5c1bce84a7b441cb1959cfc7093b7f") },
		{ { "-c", "alias.die=!kill -HUP $PPID", "die", nullptr }, {}, 4096, EIO, {} },
		{ { "-c", "alias.die=!kill -KILL $PPID", "die", nullptr }, {}, 4096, EIO, {} },
	};
	static_assert(SIGHUP == 1, "the signal number that equals the exit status 1");
	char const *const *const hash = cases[0].words;
	char const *const *const missing = cases[1].words;
	struct cstr_view const name = cases[0].out;
	struct buf out = buf();
	for (size_t i = 0; i < array_size(cases); ++i) {
		struct child_cmd const how = {
			.in       = { (unsigned char *)(uintptr_t)cases[i].in.data,
			              cases[i].in.len },
			.max      = cases[i].max,
			.deadline = 1,
		};
		CHECK(!buf_put(&out, "x", 1, SIZE_MAX));
		struct oid oid = { "0123" };
		CHECK(git_run_now(dir, cases[i].words, how, child_fds(), &out, &oid) == cases[i].e);
		CHECK(git_run_test_out(&out, cases[i].out));
		if (i)
			CHECK(!oid_len(&oid));
		else
			CHECK(!strcmp(oid.hex, "f2ba8f84ab5c1bce84a7b441cb1959cfc7093b7f"));
	}
	struct child_cmd const how = {
		.in       = { (unsigned char *)"abc", 3 },
		.max      = 4096,
		.deadline = 1,
		.grace    = GIT_GRACE,
	};
	struct oid oid = { "0123" };
	CHECK(!buf_put(&out, "x", 1, SIZE_MAX));
	CHECK(git_run_now(dir, missing, how, child_fds(), &out, nullptr) == EIO);
	CHECK(!buf_peek(&out).len);
	CHECK(git_run_now(dir, missing, how, child_fds(), nullptr, nullptr) == EIO);
	CHECK(!git_run_now(dir, hash, how, child_fds(), &out, nullptr));
	CHECK(git_run_test_out(&out, name));
	/* Without a buffer, git writes to fds.out. */
	struct child_fds fds = child_fds();
	fds.out = memfd_create("askai-git-run-test", MFD_CLOEXEC);
	CHECK(fds.out >= 0);
	CHECK(!git_run_now(dir, hash, how, fds, nullptr, nullptr));
	char text[64];
	CHECK(pread(fds.out, text, sizeof text, 0) == (ssize_t)name.len &&
	      !memcmp(text, name.data, name.len));
	util_close(&fds.out);

	/* The errors of git_run_init(), then a name to read without a buffer. */
	char const *many[GIT_RUN_ARGV];
	for (size_t i = 0; i < array_size(many) - 1; ++i)
		many[i] = "x";
	many[array_size(many) - 1] = nullptr;
	CHECK(git_run_now(dir, many, how, child_fds(), &out, &oid) == E2BIG);
	CHECK(git_run_now(nullptr, hash, how, child_fds(), &out, &oid) == EINVAL);
	CHECK(git_run_now(dir, nullptr, how, child_fds(), &out, &oid) == EINVAL);
	CHECK(git_run_now(dir, hash, how, child_fds(), nullptr, &oid) == EINVAL);
	CHECK(!strcmp(oid.hex, "0123") && git_run_test_out(&out, name));

	/* git reads its input from a pipe that stays open. */
	int pipe_fds[2];
	CHECK(!pipe2(pipe_fds, O_CLOEXEC));
	fds.in = pipe_fds[0];
	uint64_t const start = loop_clock();
	CHECK(git_run_now(dir, hash, how, fds, &out, &oid) == ETIMEDOUT);
	uint64_t const took = loop_clock() - start;
	CHECK(took >= GIT_RUN_NS && took < GIT_RUN_NS + GIT_GRACE);
	CHECK(!oid_len(&oid) && !buf_peek(&out).len);
	util_close(&pipe_fds[0]);
	util_close(&pipe_fds[1]);
	buf_fini(&out);
}

/* Runs git through git_run_init() and child_capture() in a new bare
 * repository at @p repo: with -C and the path, and with the directory of
 * the child; the options and the entries of the caller as git config
 * reads them; GIT_PAGER of GIT_ENV_FIXED; the input; the errors with and
 * without CHILD_STDERR; the cap; the deadline with and without a grace;
 * and a git that a signal kills. Runs git through child_cmd_run() with
 * the output in fds.out, at the deadline with and without a grace, and
 * with a signal.
 */
static void
git_run_test_run (char const *repo)
{
	CHECK(!mkdir(repo, 0700));
	char *real = realpath(repo, nullptr);
	CHECK(real != nullptr);
	if (!real)
		return;
	struct child_cmd const how = { .max = 4096, .grace = GIT_GRACE };
	struct buf out = buf();
	struct child_end end;
	uint64_t ms;
	struct git_run_dir at;
	CHECK(!git_run_dir_init(&at, repo, nullptr, nullptr, GIT_RUN_DOT));
	CHECK(!git_run_test_git(&at, (char const *const[]){ "init", "--bare", "--quiet",
	                                                    "--template=", nullptr },
	                        how, child_fds(), &out, &end, &ms));
	CHECK(end.code == CLD_EXITED && !end.status && !end.flags && !buf_peek(&out).len);
	git_run_test_now(&at);

	/* -C and the path, then the directory of the child, with "--git-dir=.". */
	char *want = nullptr;
	CHECK(asprintf(&want, "true\n%s\n", real) > 0);
	struct cstr_view const bare = cstr_view(want);
	char const *const where[] = {
		"rev-parse", "--is-bare-repository", "--absolute-git-dir", nullptr
	};
	CHECK(!git_run_test_git(&at, where, how, child_fds(), &out, &end, &ms));
	CHECK(end.code == CLD_EXITED && !end.status && !end.flags);
	CHECK(git_run_test_out(&out, bare));
	buf_clear(&out);
	struct git_run_dir here;
	CHECK(!git_run_dir_init(&here, nullptr, nullptr, nullptr, GIT_RUN_DOT));
	struct child_fds fds = child_fds();
	fds.dir = open(repo, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fds.dir >= 0);
	CHECK(!git_run_test_git(&here, where, how, fds, &out, &end, &ms));
	CHECK(end.code == CLD_EXITED && !end.status && !end.flags);
	CHECK(git_run_test_out(&out, bare));
	util_close(&fds.dir);
	buf_clear(&out);

	/* The entries of the caller come before the options in the output of
	 * git config. GIT_PAGER is cat. */
	struct git_run_dir set;
	CHECK(!git_run_dir_init(&set, repo,
	                        (char const *const[]){ "-c", "askai.option=1", nullptr },
	                        (char *const[]){ "GIT_CONFIG_COUNT=1", "GIT_CONFIG_KEY_0=askai.env",
	                                         "GIT_CONFIG_VALUE_0=2", nullptr },
	                        GIT_RUN_DOT));
	CHECK(!git_run_test_git(&set, (char const *const[]){ "config", "--get-regexp",
	                                                     "^askai\\.", nullptr },
	                        how, child_fds(), &out, &end, &ms));
	CHECK(end.code == CLD_EXITED && !end.status && !end.flags);
	CHECK(git_run_test_out(&out, CSTR_VIEW("askai.env 2\naskai.option 1\n")));
	buf_clear(&out);
	CHECK(!git_run_test_git(&at, (char const *const[]){ "var", "GIT_PAGER", nullptr }, how,
	                        child_fds(), &out, &end, &ms));
	CHECK(git_run_test_out(&out, CSTR_VIEW("cat\n")));
	buf_clear(&out);

	/* The input of the struct child_cmd. */
	struct child_cmd input = how;
	input.in = span((unsigned char *)"abc", 3);
	char const *const hash[] = { "hash-object", "--stdin", nullptr };
	CHECK(!git_run_test_git(&at, hash, input, child_fds(), &out, &end, &ms));
	CHECK(end.code == CLD_EXITED && !end.status && !end.flags);
	CHECK(git_run_test_out(&out, CSTR_VIEW("f2ba8f84ab5c1bce84a7b441cb1959cfc7093b7f\n")));
	buf_clear(&out);
	/* Without a buffer, git writes to fds.out. cli_state runs git in that
	 * way. */
	struct child_fds file = child_fds();
	file.out = memfd_create("askai-git-run-test", MFD_CLOEXEC);
	CHECK(file.out >= 0);
	CHECK(!git_run_test_git(&at, hash, input, file, nullptr, &end, &ms));
	CHECK(end.code == CLD_EXITED && !end.status && !end.flags);
	char name[64];
	CHECK(pread(file.out, name, sizeof name, 0) == 41 &&
	      !memcmp(name, "f2ba8f84ab5c1bce84a7b441cb1959cfc7093b7f\n", 41));
	util_close(&file.out);

	/* The standard error goes to the output only with CHILD_STDERR. */
	char const *const missing[] = {
		"rev-parse", "--verify", "--end-of-options", "missing", nullptr
	};
	CHECK(!git_run_test_git(&at, missing, how, child_fds(), &out, &end, &ms));
	CHECK(end.code == CLD_EXITED && end.status == 128 && !end.flags && !buf_peek(&out).len);
	struct child_cmd errors = how;
	errors.flags = CHILD_STDERR;
	CHECK(!git_run_test_git(&at, missing, errors, child_fds(), &out, &end, &ms));
	CHECK(end.code == CLD_EXITED && end.status == 128 && !end.flags);
	struct span got = buf_peek(&out);
	CHECK(got.len > 7 && !memcmp(got.ptr, "fatal: ", 7));
	buf_clear(&out);

	/* The cap. */
	struct child_cmd small = how;
	small.max = 4;
	CHECK(git_run_test_git(&at, where, small, child_fds(), &out, &end, &ms) == EFBIG);
	CHECK(end.code == CLD_EXITED && !end.status && end.flags == CHILD_CUT);
	CHECK(git_run_test_out(&out, CSTR_VIEW("true")));
	buf_clear(&out);

	/* git reads its input from a pipe that stays open until the deadline,
	 * 100 ms after the start. With the grace, SIGTERM ends git. Without
	 * the grace, SIGKILL ends git. */
	int pipe_fds[2];
	CHECK(!pipe2(pipe_fds, O_CLOEXEC));
	fds = child_fds();
	fds.in = pipe_fds[0];
	struct child_cmd late = how;
	late.deadline = 100;
	CHECK(git_run_test_git(&at, hash, late, fds, &out, &end, &ms) == ETIMEDOUT);
	CHECK(end.code == CLD_KILLED && end.status == SIGTERM && end.flags == CHILD_LATE);
	CHECK(ms >= 100 && ms < 2000 && !buf_peek(&out).len);
	late.grace = 0;
	CHECK(git_run_test_git(&at, hash, late, fds, &out, &end, &ms) == ETIMEDOUT);
	CHECK(end.code == CLD_KILLED && end.status == SIGKILL && end.flags == CHILD_LATE);
	CHECK(ms >= 100 && ms < 2000 && !buf_peek(&out).len);
	/* Without a buffer, the deadline ends git in the same way. */
	CHECK(git_run_test_git(&at, hash, late, fds, nullptr, &end, &ms) == ETIMEDOUT);
	CHECK(end.code == CLD_KILLED && end.status == SIGKILL && end.flags == CHILD_LATE);
	CHECK(ms >= 100 && ms < 2000);
	late.grace = GIT_GRACE;
	CHECK(git_run_test_git(&at, hash, late, fds, nullptr, &end, &ms) == ETIMEDOUT);
	CHECK(end.code == CLD_KILLED && end.status == SIGTERM && end.flags == CHILD_LATE);
	CHECK(ms >= 100 && ms < 2000);
	util_close(&pipe_fds[0]);
	util_close(&pipe_fds[1]);

	/* An alias of a shell command kills git with SIGKILL, through
	 * child_capture() and through child_cmd_run(). */
	char const *const die[] = { "-c", "alias.die=!kill -KILL $PPID", "die", nullptr };
	CHECK(git_run_test_git(&at, die, how, child_fds(), &out, &end, &ms) == EIO);
	CHECK(end.code == CLD_KILLED && end.status == SIGKILL && !end.flags);
	CHECK(!buf_peek(&out).len);
	CHECK(git_run_test_git(&at, die, how, child_fds(), nullptr, &end, &ms) == EIO);
	CHECK(end.code == CLD_KILLED && end.status == SIGKILL && !end.flags);
	buf_fini(&out);
	free(want);
	free(real);
}

int
main (int    argc,
      char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s REPOSITORY\n", argv[0]);
		return 2;
	}
	git_run_test_shapes();
	git_run_test_env();
	git_run_test_init();
	git_run_test_path();
	git_run_test_name();
	git_run_test_run(argv[1]);
	return check_report("git_run");
}
