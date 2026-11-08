/** @file store.c
 *
 * The session repository as a git store (src/store.h).
 */
#define _GNU_SOURCE /* asprintf(), memmem(), memrchr(), mempcpy() */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "child.h"
#include "clk.h"
#include "git_run.h"
#include "log.h"
#include "loop.h"
#include "mcp.h"
#include "oid.h"
#include "store.h"
#include "util.h"

struct store {
	struct git_run_dir  git;        /* the path, the options and the
	                                   environment of git: -C directory,
	                                   "--git-dir=." and store_options */
	char               *directory;  /* the canonical git common directory */
	int                 fd;         /* the descriptor of the directory at
	                                   the path of --notes, or -1 */
	uint32_t            bare;       /* 1 if fd is a bare repository or
	                                   empty */
};

/* The options of git after the fixed options of git_run: literal paths,
 * and no worktree.
 */
static char const *const store_options[] = {
	"--literal-pathspecs", "-c", "core.bare=true", nullptr
};

/* The place in a row of cmds[] where store_command() puts the next of the
 * words that the caller passes.
 */
static char const word[] = "";

/* The words of each command after the options. */
static char const *const cmds[][10] = {
	[STORE_CREATE]   = { GIT_INIT_BARE_WORDS("session") },
	[STORE_CHECK]    = { "cat-file", "--batch-check" },
	[STORE_COMMIT]   = { "fast-import", "--quiet", "--date-format=now" },
	[STORE_LOG]      = { "log", "-z", "--no-renames",
	                     "--format=%ct %H %(trailers:key=Asked-by,valueonly,separator=%x2C) %s",
	                     "--numstat", word, "--", word },
	[STORE_READ]     = { "cat-file", "--batch" },
	[STORE_REFS]     = { "for-each-ref", word, word, word, word },
	[STORE_FILES]    = { "ls-tree", "-r", "-z", "-l", word, "--" },
	[STORE_HEAD]     = { "rev-parse", "--verify", "--end-of-options", word },
	[STORE_ANCESTOR] = { "merge-base", "--is-ancestor", word, word },
	[STORE_DIFF]     = { "diff", "--no-ext-diff", "--no-textconv", "--no-renames",
	                     "--no-color", "--submodule=short", word, word, "--", word },
	[STORE_STAT]     = { "diff", "--numstat", "--no-textconv", word, word },
	[STORE_GREP]     = { "grep", "-IniEz", "--no-color", "-f", "-", word, "--" },
};

/* A git that writes ends, at its deadline and grace at the latest, before
 * its lock file is STORE_STALE old. */
static_assert(GIT_RUN_NS + GIT_GRACE < (uint64_t)STORE_STALE * NS);

/* Replaces store->directory, the real path of a worktree or a clone, with
 * the canonical git common directory that git rev-parse prints. git reads
 * a gitfile given as --git-dir, such as the .git of a worktree. The output
 * of git, with the errors of git, is stored in @p out. Returns 0, EIO if
 * git fails or prints anything but one line with an absolute path, or
 * another error number.
 */
static int
store_open_common (struct store *store,
                   struct buf   *out)
{
	static char const *const words[] = {
		"--git-dir=.git", "rev-parse", "--path-format=absolute", "--git-common-dir", nullptr
	};
	struct git_run_dir dir;
	struct git_run run;
	int e = git_run_dir_init(&dir, store->directory, nullptr, nullptr, 0);
	if (!e)
		e = git_run_init(&run, &dir, words, &(struct child_cmd){
			.max      = STORE_MAX,
			.deadline = loop_clock() + GIT_RUN_NS,
			.flags    = CHILD_STDERR,
		});
	if (!e)
		e = git_run_capture(&run, child_fds(), out, nullptr);
	if (e)
		return e;
	struct span const got = buf_peek(out);
	struct cstr_view rest = { (char const *)got.ptr, got.len };
	struct cstr_view line;
	if (git_run_path(&rest, &line) || rest.len)
		return EIO;
	char *common = strndup(line.data, line.len);
	if (!common)
		return ENOMEM;
	free(store->directory);
	store->directory = common;
	return 0;
}

/* Makes the directory @p path if it does not exist, opens it as store->fd,
 * and stores the canonical git common directory in store->directory: the
 * real path of @p path for a bare repository or an empty directory, else
 * the common directory of the worktree or the clone. Returns 0, or the
 * error number with a message in @p why, of @p len bytes.
 */
static int
store_open (struct store *store,
            char const   *path,
            char         *why,
            size_t        len)
{
	int e = mkdir(path, 0700) && errno != EEXIST ? errno : 0;
	if (!e && !(store->directory = realpath(path, nullptr)))
		e = errno;
	if (!e && (store->fd = open(store->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0)
		e = errno;
	if (e) {
		util_format(why, len, "%s", strerror(e));
		return e;
	}
	struct stat st;
	if (!fstatat(store->fd, ".git", &st, AT_SYMLINK_NOFOLLOW)) {
		if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
			return util_refuse(why, len,
			                   "the repository .git is not a file or directory");
		struct buf out = {};
		e = store_open_common(store, &out);
		if (e) {
			/* After EIO, the message contains the last line of the
			 * output of git. The line is the error of git, or a line
			 * of output that is not one absolute path. */
			struct span const got = buf_peek(&out);
			struct cstr_view because = cstr_view_last_line((struct cstr_view){
				(char const *)got.ptr, got.len
			});
			if (e != EIO || !because.len)
				because = cstr_view(strerror(e));
			util_format(why, len, "cannot resolve the Git common directory: %.*s",
			            (int)because.len, because.data);
		}
		buf_fini(&out);
		return e;
	}
	if (!fstatat(store->fd, "HEAD", &st, AT_SYMLINK_NOFOLLOW)) {
		if (!S_ISREG(st.st_mode))
			return util_refuse(why, len, "the repository HEAD is not a regular file");
		store->bare = true;
		return 0;
	}
	/* Never silently start a new session over the old per-thread layout.
	 * util_dir_empty() returns EEXIST for an entry, and store_create()
	 * refuses the directory with EINVAL. */
	e = util_dir_empty(store->fd, nullptr, 0);
	if (e == EEXIST)
		e = EINVAL;
	/* Another service can be creating the repository (store_init()). Its
	 * lock ends once git init has ended. */
	if (e == EINVAL && !flock(store->fd, LOCK_SH)) {
		if (!fstatat(store->fd, "HEAD", &st, AT_SYMLINK_NOFOLLOW))
			e = 0;
		(void)flock(store->fd, LOCK_UN);
	}
	if (e)
		util_format(why, len, "notes now names one shared Git repository; this "
		            "nonempty directory is not a repository. Keep the old notes "
		            "directory and select a new repository path for the session");
	store->bare = true;
	return e;
}

struct store *
store_create (char const *path,
              char       *why,
              size_t      len,
              int        *e)
{
	if (!path) {
		*e = util_refuse(why, len, "no repository path");
		return nullptr;
	}
	struct store *store = malloc(sizeof *store);
	if (!store) {
		util_format(why, len, "out of memory");
		*e = ENOMEM;
		return nullptr;
	}
	*store = (struct store){ .fd = -1 };
	*e = store_open(store, path, why, len);
	if (*e) {
		store_destroy(&store);
		return nullptr;
	}
	*e = git_run_dir_init(&store->git, store->directory, store_options, nullptr, GIT_RUN_DOT);
	if (*e) {
		util_format(why, len, "%s", strerror(*e));
		store_destroy(&store);
	}
	return store;
}

void
store_destroy (struct store **p_store)
{
	if (!p_store || !*p_store)
		return;
	struct store *store = *p_store;
	*p_store = nullptr;
	store->git = (struct git_run_dir){};
	free(store->directory);
	store->directory = nullptr;
	util_close(&store->fd);
	free(store);
}

char const *
store_directory (struct store const *store)
{
	return store ? store->directory : nullptr;
}

bool
store_exists (struct store const *store)
{
	if (!store)
		return false;
	struct stat st;
	return !fstatat(store->fd, store->bare ? "HEAD" : ".git", &st, AT_SYMLINK_NOFOLLOW);
}

int
store_command (struct store const *store,
               struct git_run     *dest,
               enum store_cmd      cmd,
               char const *const  *words,
               char const         *ref,
               struct span         in,
               uint64_t            now)
{
	if (!store || !dest || cmd >= array_size(cmds) || (cmd == STORE_COMMIT && !ref))
		return EINVAL;
	char const *argv[array_size(cmds[0]) + 1];
	size_t k = 0;
	for (size_t i = 0; i < array_size(cmds[cmd]) && cmds[cmd][i]; ++i) {
		if (cmds[cmd][i] != word)
			argv[k++] = cmds[cmd][i];
		else if (words)
			argv[k++] = *words++;
		else
			return EINVAL;
	}
	argv[k] = nullptr;
	bool const writes = cmd == STORE_CREATE || cmd == STORE_COMMIT;
	int e = git_run_init(dest, &store->git, argv, &(struct child_cmd){
		.in       = in,
		.max      = STORE_MAX,
		.deadline = now + GIT_RUN_NS,
		/* git removes its own lock files on SIGTERM. */
		.grace    = writes ? GIT_GRACE : 0,
		/* For the commands that write, the output includes the errors. */
		.flags    = writes ? CHILD_STDERR : 0,
	});
	if (e || cmd != STORE_COMMIT)
		return e;
	int64_t age;
	int const lock = store_lock(store, ref, &age);
	if (lock)
		LOG(LOG_WARN, "notes: cannot check the lock of %s: %s", ref, strerror(lock));
	return 0;
}

struct child *
store_spawn (struct store const *store,
             struct mcp_slot    *slot,
             enum store_cmd      cmd,
             char const *const  *words,
             char const         *ref,
             struct span         in,
             int                *e)
{
	if (!slot) {
		*e = EINVAL;
		return nullptr;
	}
	struct git_run run;
	*e = store_command(store, &run, cmd, words, ref, in, mcp_slot_loop(slot)->now);
	return *e ? nullptr : mcp_slot_spawn(slot, &run.c, e);
}

struct child *
store_start (struct store const *store,
             struct loop        *loop,
             struct list        *list,
             enum store_cmd      cmd,
             char const *const  *words,
             char const         *ref,
             struct span         in,
             void              (*done) (struct child *c),
             void               *owner,
             int                *e)
{
	if (!loop) {
		*e = EINVAL;
		return nullptr;
	}
	struct git_run run;
	*e = store_command(store, &run, cmd, words, ref, in, loop->now);
	return *e ? nullptr : child_create(loop, list, &run.c, done, owner, e);
}

int
store_run (struct store const *store,
           enum store_cmd      cmd,
           char const *const  *words,
           char const         *ref,
           struct span         in,
           struct buf         *out,
           int                *status)
{
	if (!out || !status)
		return EINVAL;
	struct git_run run;
	int e = store_command(store, &run, cmd, words, ref, in, loop_clock());
	if (e)
		return e;
	size_t const had = buf_peek(out).len;
	struct child_end end;
	e = child_capture(&run.c, child_fds(), out, &end);
	*status = end.status;
	if (e != EFBIG)
		return e;
	/* For an output longer than STORE_MAX, child_capture() appends the
	 * first STORE_MAX bytes and returns EFBIG. store_run() removes those
	 * bytes and returns ENOBUFS. */
	buf_cut(out, had, buf_peek(out).len - had);
	return ENOBUFS;
}

int
store_init (struct store const *store,
            struct buf         *out,
            int                *status)
{
	if (!store || !out || !status)
		return EINVAL;
	if (flock(store->fd, LOCK_EX))
		return errno;
	*status = 0;
	int e = 0;
	if (!store_exists(store))
		e = store_run(store, STORE_CREATE, nullptr, nullptr, (struct span){}, out, status);
	(void)flock(store->fd, LOCK_UN);
	return e;
}

void
store_drop (struct child **p_c)
{
	/* store_spawn() sets a grace only for a git that writes. */
	if (p_c && *p_c && (*p_c)->grace)
		child_release(p_c);
	else
		child_destroy(p_c);
}

bool
store_raced (struct span out)
{
	struct cstr_view const raced[] = {
		CSTR_VIEW("Not updating "), CSTR_VIEW("not updating "),
		CSTR_VIEW(" but expected "), CSTR_VIEW("File exists"),
		CSTR_VIEW("reference already exists"),
	};
	for (size_t i = 0; i < array_size(raced) && out.len; ++i)
		if (memmem(out.ptr, out.len, raced[i].data, raced[i].len))
			return true;
	return false;
}

int
store_lock (struct store const *store,
            char const         *ref,
            int64_t            *age)
{
	if (!store || !ref || !age)
		return EINVAL;
	*age = 0;

	struct timespec now = clk_now_or_1969(CLOCK_REALTIME);
	if (now.tv_sec < 0)
		return clk_errno(now);

	struct timespec up = clk_now_or_1969(CLOCK_BOOTTIME);
	if (up.tv_sec < 0)
		return clk_errno(up);

	char *path = nullptr;
	if (asprintf(&path, "%s.lock", ref) < 0)
		return ENOMEM;
	/* util_parent_open() opens the directories of the ref one at a time,
	 * so that no part of the path can be a symbolic link. */
	char const *name = nullptr;
	int fd = -1;
	int top = open(store->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int e = top < 0 ? errno : util_parent_open(top, path, &name, &fd);
	util_close(&top);
	struct stat st;
	/* Without the directory, there is no lock file. */
	if (e)
		e = e == ENOENT ? 0 : e;
	else if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW))
		e = errno == ENOENT ? 0 : errno;
	/* With a margin, a clock step after the boot cannot make a live lock
	 * look older than the boot. */
	else if (!S_ISREG(st.st_mode) || st.st_mtime >= now.tv_sec - up.tv_sec - STORE_STALE)
		*age = now.tv_sec - st.st_mtime;
	else if (unlinkat(fd, name, 0) && errno != ENOENT)
		e = errno;
	util_close(&fd);
	free(path);
	return e;
}

/* Writes @p name at @p p in double quotes, with a backslash before each
 * double quote and backslash in it, as fast-import reads a path, and
 * returns the end. The quoted name is at most 2 * name.len + 2 bytes long.
 */
static char *
store_quote (char             *p,
             struct cstr_view  name)
{
	*p++ = '"';
	for (size_t i = 0; i < name.len; ++i) {
		if (name.data[i] == '"' || name.data[i] == '\\')
			*p++ = '\\';
		*p++ = name.data[i];
	}
	*p++ = '"';
	return p;
}

int
store_part_head (struct store_part *part,
                 struct buf        *in,
                 struct cstr_view   name,
                 size_t             len)
{
	static char const m[] = "M 100644 inline ";
	if (!part || !in || !name.data || name.len > STORE_PATH)
		return EINVAL;
	/* The head is m, the name quoted as a C string, which at most doubles
	 * its bytes, and the data command. Then come the data and a line
	 * feed. */
	struct span s;
	int e = buf_reserve(in, sizeof m + 2 * name.len + sizeof "\"\"\ndata \n" + UTIL_DECIMAL +
	                    len, SIZE_MAX, &s);
	if (e)
		return e;
	char *const head = (char *)s.ptr;
	char *p = store_quote(mempcpy(head, m, sizeof m - 1), name);
	size_t const room = s.len - (size_t)(p - head);
	int n = snprintf(p, room, "\ndata %zu\n", len);
	if (n < 0 || (size_t)n >= room)
		return EINVAL;
	size_t const h = (size_t)(p - head) + (size_t)n;
	head[h + len] = '\n';
	/* The quoted name runs from the end of m to p. */
	size_t at = buf_peek(in).len;
	*part = (struct store_part){
		.name_at = at + sizeof m - 1,
		.name_len = (size_t)(p - head) - (sizeof m - 1),
		.data_at = at + h,
		.data_len = len,
	};
	buf_commit(in, h + len + 1);
	return 0;
}

int
store_part_file (struct store_part *part,
                 struct buf        *in,
                 struct cstr_view   name,
                 struct cstr_view   data)
{
	int e = store_part_head(part, in, name, data.len);
	if (!e)
		memcpy(buf_peek(in).ptr + part->data_at, data.data, data.len);
	return e;
}

int
store_part_delete (struct store_part *part,
                   struct buf        *in,
                   struct cstr_view   name)
{
	if (!part || !in || !name.data || name.len > STORE_PATH)
		return EINVAL;
	/* The part is "D ", the name quoted as a C string, which at most
	 * doubles its bytes, and a line feed. */
	struct span s;
	int e = buf_reserve(in, sizeof "D \"\"\n" + 2 * name.len, SIZE_MAX, &s);
	if (e)
		return e;
	char *const head = (char *)s.ptr;
	char *p = store_quote(mempcpy(head, "D ", 2), name);
	size_t const at = buf_peek(in).len;
	size_t const end = (size_t)(p - head);
	*part = (struct store_part){ .name_at = at + 2, .name_len = end - 2, .data_at = at + end };
	*p = '\n';
	buf_commit(in, end + 1);
	return 0;
}

/* Returns true if v.data is not nullptr, @p v is shorter than
 * STORE_MESSAGE bytes, and @p v contains no NUL and none of the bytes of
 * the string @p bad.
 */
static bool
store_clean (struct cstr_view  v,
             char const       *bad)
{
	if (!v.data || v.len >= STORE_MESSAGE)
		return false;
	for (size_t i = 0; i < v.len; ++i)
		if (!v.data[i] || strchr(bad, v.data[i]))
			return false;
	return true;
}

/* Returns true if @p id can be the author or the committer of a commit. */
static bool
store_ident_valid (struct store_ident const *id)
{
	return id->name.len && store_clean(id->name, "<>\n") && store_clean(id->email, "<>\n");
}

int
store_commit_input (struct store_commit const *commit,
                    struct buf                *in)
{
	if (!commit || !in || !commit->ref || strchr(commit->ref, '\n') || !commit->tip ||
	    !store_clean(commit->message, "") || !store_ident_valid(&commit->committer) ||
	    (commit->author.name.len && !store_ident_valid(&commit->author)) ||
	    (commit->n_trailers && !commit->trailers))
		return EINVAL;
	/* The count of the data includes the line feed after the message, and
	 * with a trailer the blank line before the trailer. */
	size_t data = commit->message.len + 1 + (commit->n_trailers != 0);
	for (size_t i = 0; i < commit->n_trailers; ++i) {
		struct store_trailer const *t = &commit->trailers[i];
		if (!util_word(t->key.data, t->key.len, UTIL_WORD_MAX) || !store_clean(t->value, "\n"))
			return EINVAL;
		data += t->key.len + sizeof ": \n" - 1 + t->value.len;
	}
	struct store_ident const *a = &commit->author;
	struct store_ident const *c = &commit->committer;
	/* get-mark ends the commit, and git writes the commit's id. */
	int e = buf_printf(in, "commit %s\nmark :1\n", commit->ref);
	if (!e && a->name.len)
		e = buf_printf(in, "author %.*s <%.*s> now\n", (int)a->name.len, a->name.data,
		               (int)a->email.len, a->email.data);
	if (!e)
		e = buf_printf(in, "committer %.*s <%.*s> now\ndata %zu\n%.*s\n", (int)c->name.len,
		               c->name.data, (int)c->email.len, c->email.data, data,
		               (int)commit->message.len, commit->message.data);
	for (size_t i = 0; !e && i < commit->n_trailers; ++i) {
		struct store_trailer const *t = &commit->trailers[i];
		e = buf_printf(in, "%s%.*s: %.*s\n", i ? "" : "\n", (int)t->key.len, t->key.data,
		               (int)t->value.len, t->value.data);
	}
	if (!e && oid_len(commit->tip))
		e = buf_printf(in, "from %s\n", commit->tip->hex);
	if (!e)
		e = buf_put(in, commit->files.ptr, commit->files.len, SIZE_MAX);
	if (!e)
		e = buf_printf(in, "get-mark :1\n");
	return e;
}

int
store_committed (struct span  out,
                 struct oid  *commit)
{
	if (!commit)
		return EINVAL;
	unsigned char *nl = out.len > 1 ? memrchr(out.ptr, '\n', out.len - 1) : nullptr;
	if (nl)
		out = (struct span){ nl + 1, out.len - (size_t)(nl + 1 - out.ptr) };
	struct cstr_view const line = { (char const *)out.ptr, out.len };
	return oid_init_line(commit, line) ? EIO : 0;
}
