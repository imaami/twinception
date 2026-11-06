/** @file notes.c
 *
 * The notes tool.
 */
#define _GNU_SOURCE /* memrchr(), mempcpy() */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clk.h"
#include "git.h"
#include "notes.h"
#include "util.h"

/* The variables of the environment, its nullptr included. */
enum { ENV = 10 };

/* Only this namespace is exposed by the notes tool. */
#define BRANCH "refs/heads/notes/"

struct notes {
	char *env[ENV];
	char *dir;    /* the real path of the shared session repository */
	char *participant;
	char *directory; /* canonical Git common directory */
	int   fd;     /* the repository directory, open; -1 if not open */
	uint32_t bare;
};

enum notes_revision_phase : uint8_t {
	REVISION_HEAD,
	REVISION_FROM,
	REVISION_TO,
	REVISION_DATA,
};

struct notes_revision {
	char                      file[NOTES_NAME + 1];
	char                      from[NOTES_TIP + 1];
	char                      to[NOTES_TIP + 1];
	char                      tip[NOTES_TIP + 1];
	uint8_t                   file_len;
	enum notes_revision_phase phase;
};

/* The options of git before each command: GIT_OPTIONS_FIXED, which stop
 * the configuration of the repository from making git run other programs,
 * literal paths, and no signature checks.
 */
static char const *const fixed[] = {
	GIT_OPTIONS_FIXED, "--literal-pathspecs",
	"-c", "core.bare=true", "-c", "log.showSignature=false",
};

/* The words of each command after the options in fixed. */
static char const *const cmds[][8] = {
	[NOTES_CREATE] = { "init", "--bare", "--quiet", "--template=",
	                   "--initial-branch=session", "--" },
	[NOTES_CHECK]  = { "cat-file", "--batch-check" },
	[NOTES_ADD]    = { "fast-import", "--quiet", "--date-format=now" },
	[NOTES_LIST]   = { "log", "-z", "--no-renames", "--format=%ct %H %s", "--numstat",
	                   nullptr },
	[NOTES_READ]   = { "cat-file", "--batch" },
	[NOTES_BOOKS]  = { "for-each-ref", "--format=%(refname:strip=3)", BRANCH },
	[NOTES_FORMAT] = { "rev-parse", "--show-object-format" },
	[NOTES_FILES]  = { "ls-tree", "-r", "-z", "-l" },
	[NOTES_HEAD]   = { "rev-parse", "--verify", "--end-of-options" },
};

/* The kinds of the objects in the output of git cat-file. */
enum notes_kind {
	KIND_MISSING,
	KIND_BLOB,
	KIND_TREE,
	KIND_COMMIT,
	KIND_OTHER,
};

/* Makes the directory @p path if it does not exist, and stores its real
 * path in notes->dir. Returns 0, or the error number with a message in
 * @p why, of @p len bytes.
 */
static int
notes_open (struct notes *notes,
            char const   *path,
            char         *why,
            size_t        len)
{
	int e = mkdir(path, 0700) && errno != EEXIST ? errno : 0;
	if (!e && !(notes->dir = realpath(path, nullptr)))
		e = errno;
	if (!e && (notes->fd = open(notes->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0)
		e = errno;
	if (e) {
		util_format(why, len, "%s", strerror(e));
		return e;
	}
	struct stat st;
	if (!fstatat(notes->fd, ".git", &st, AT_SYMLINK_NOFOLLOW)) {
		if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))
			return 0;
		return util_refuse(why, len, "the repository .git is not a file or directory");
	}
	if (!fstatat(notes->fd, "HEAD", &st, AT_SYMLINK_NOFOLLOW)) {
		if (!S_ISREG(st.st_mode))
			return util_refuse(why, len, "the repository HEAD is not a regular file");
		notes->bare = true;
		return 0;
	}
	/* Never silently start a new session over the old per-thread layout. */
	int fd = dup(notes->fd);
	if (fd < 0) {
		e = errno;
		util_format(why, len, "%s", strerror(e));
		return e;
	}
	DIR *dir = fdopendir(fd);
	if (!dir) {
		e = errno;
		close(fd);
		util_format(why, len, "%s", strerror(e));
		return e;
	}
	struct dirent *entry;
	errno = 0; /* readdir distinguishes exhaustion from errors through errno. */
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		e = EINVAL;
		break;
	}
	if (!entry && errno)
		e = errno;
	if (closedir(dir) && !e)
		e = errno;
	if (e)
		util_format(why, len, "notes now names one shared Git repository; this "
		            "nonempty directory is not a repository. Keep the old notes "
		            "directory and select a new repository path for the session");
	notes->bare = true;
	return e;
}

/* Resolves a Git path, relative to its containing directory when needed. */
static int
notes_resolve (char const *base,
               char const *name,
               char      **resolved)
{
	char *path = nullptr;
	if (asprintf(&path, "%s%s%s", *name == '/' ? "" : base,
	             *name == '/' ? "" : "/", name) < 0)
		return ENOMEM;
	*resolved = realpath(path, nullptr);
	int e = *resolved ? 0 : errno;
	free(path);
	path = nullptr;
	return e;
}

/* Reads Git's one-line indirection files at startup, outside the service loops. */
static int
notes_line (char const *base,
            char const *name,
            char      **line)
{
	char *path = nullptr;
	if (asprintf(&path, "%s/%s", base, name) < 0)
		return ENOMEM;
	FILE *file = fopen(path, "re");
	int e = file ? 0 : errno;
	free(path);
	path = nullptr;
	if (e)
		return e;
	size_t size = 0;
	ssize_t n = getline(line, &size, file);
	e = n < 0 ? ferror(file) ? EIO : EINVAL : 0;
	if (!e) {
		while (n && ((*line)[n - 1] == '\n' || (*line)[n - 1] == '\r'))
			(*line)[--n] = '\0';
		if (!n || memchr(*line, '\0', (size_t)n))
			e = EINVAL;
	}
	if (fclose(file) && !e)
		e = errno;
	return e;
}

/* Finds the common directory even when --notes names an attached worktree. */
static int
notes_common (struct notes *notes)
{
	if (notes->bare) {
		notes->directory = strdup(notes->dir);
		return notes->directory ? 0 : ENOMEM;
	}
	struct stat st;
	if (fstatat(notes->fd, ".git", &st, AT_SYMLINK_NOFOLLOW))
		return errno;
	char *line = nullptr;
	char *git = nullptr;
	int e = 0;
	if (S_ISDIR(st.st_mode))
		e = notes_resolve(notes->dir, ".git", &git);
	else {
		e = notes_line(notes->dir, ".git", &line);
		if (!e && strncmp(line, "gitdir: ", 8))
			e = EINVAL;
		if (!e)
			e = notes_resolve(notes->dir, line + 8, &git);
		free(line);
		line = nullptr;
	}
	if (!e) {
		e = notes_line(git, "commondir", &line);
		if (e == ENOENT) {
			notes->directory = git;
			git = nullptr;
			e = 0;
		} else if (!e) {
			e = notes_resolve(git, line, &notes->directory);
		}
	}
	free(line);
	line = nullptr;
	free(git);
	git = nullptr;
	return e;
}

struct notes *
notes_create (char const *path,
              char const *participant,
              char       *why,
              size_t      len,
              int        *e)
{
	if (!util_word_len(participant, UTIL_WORD_MAX)) {
		*e = util_refuse(why, len, "the participant must have 1 to 64 ASCII letters, "
		                 "digits, underscores or hyphens");
		return nullptr;
	}
	struct notes *notes = calloc(1, sizeof *notes);
	if (!notes) {
		util_format(why, len, "out of memory");
		*e = ENOMEM;
		return nullptr;
	}
	char *const vars[] = { util_env_path(), GIT_ENV_FIXED, nullptr };
	static_assert(sizeof vars == sizeof notes->env);
	memcpy(notes->env, vars, sizeof vars);
	notes->fd = -1;
	if (!(notes->participant = strdup(participant))) {
		*e = ENOMEM;
		util_format(why, len, "out of memory");
		notes_destroy(&notes);
		return nullptr;
	}
	if (!(*e = notes_open(notes, path, why, len)) && (*e = notes_common(notes)))
		util_format(why, len, "cannot resolve the Git common directory: %s", strerror(*e));
	if (*e)
		notes_destroy(&notes);
	return notes;
}

void
notes_destroy (struct notes **p_notes)
{
	if (!p_notes || !*p_notes)
		return;
	struct notes *notes = *p_notes;
	*p_notes = nullptr;
	free(notes->dir);
	notes->dir = nullptr;
	free(notes->participant);
	notes->participant = nullptr;
	free(notes->directory);
	notes->directory = nullptr;
	util_close(&notes->fd);
	free(notes);
}

char *const *
notes_env (struct notes const *notes)
{
	return notes->env;
}

char const *
notes_directory (struct notes const *notes)
{
	return notes->directory;
}

char const *
notes_writer (struct notes const *notes)
{
	return notes->participant;
}

int
notes_id (char id[NOTES_ID + 1])
{
	id[NOTES_ID] = '\0';
	return util_random_hex(id, NOTES_ID / 2);
}

bool
notes_is_id (struct cstr_view id)
{
	return id.len == NOTES_ID && util_is_hex(id.data, id.len);
}

void
notes_repo (struct notes const *notes,
            uint64_t            principal,
            enum notes_side     side,
            char                repo[NOTES_REPO])
{
	static_assert(NOTES_REPO >= sizeof BRANCH + 64 + 1 + 16 + 1 + 8);
	char who[17] = "anonymous";
	if (principal)
		util_format(who, sizeof who, "%016" PRIx64, principal);
	util_format(repo, NOTES_REPO, BRANCH "%s/%s/%s", notes->participant, who,
	            side == NOTES_CLIENT ? "client" : "upstream");
}

int
notes_select (struct cstr_view name,
              char             repo[NOTES_REPO])
{
	if (!name.data || !name.len || sizeof BRANCH + name.len > NOTES_REPO)
		return EINVAL;
	char const *slash = memchr(name.data, '/', name.len);
	size_t const participant = slash ? (size_t)(slash - name.data) : 0;
	if (!util_word(name.data, participant, UTIL_WORD_MAX))
		return EINVAL;
	struct cstr_view const rest = cstr_view_slice(name, participant + 1, SIZE_MAX);
	slash = memchr(rest.data, '/', rest.len);
	if (!slash)
		return EINVAL;
	/* who is "anonymous" or the 16 hexadecimal digits of a principal, as
	 * notes_repo() writes it. */
	struct cstr_view const who = { rest.data, (size_t)(slash - rest.data) };
	struct cstr_view const side = cstr_view_slice(rest, who.len + 1, SIZE_MAX);
	if ((!cstr_view_equal(who, CSTR_VIEW("anonymous")) &&
	     (who.len != 16 || !util_is_hex(who.data, who.len))) ||
	    (!cstr_view_equal(side, CSTR_VIEW("client")) &&
	     !cstr_view_equal(side, CSTR_VIEW("upstream"))))
		return EINVAL;
	memcpy(repo, BRANCH, sizeof BRANCH - 1);
	memcpy(repo + sizeof BRANCH - 1, name.data, name.len);
	repo[sizeof BRANCH - 1 + name.len] = '\0';
	return 0;
}

char const *
notes_selector (char const *repo)
{
	return repo + sizeof BRANCH - 1;
}

static size_t
notes_argv_prefix (struct notes const *notes,
                   bool                create,
                   char const         *argv[NOTES_ARGV])
{
	size_t k = 0;
	argv[k++] = "git";
	argv[k++] = "-C";
	argv[k++] = create ? notes->dir : notes->directory;
	if (!create)
		argv[k++] = "--git-dir=.";
	for (size_t i = 0; i < array_size(fixed); ++i)
		argv[k++] = fixed[i];
	return k;
}

static size_t
notes_argv_command (struct notes const *notes,
                    char const         *repo,
                    enum notes_cmd      cmd,
                    char const         *argv[NOTES_ARGV])
{
	static_assert(4 + array_size(fixed) + array_size(cmds[0]) + 3 <= NOTES_ARGV);
	size_t k = notes_argv_prefix(notes, cmd == NOTES_CREATE, argv);
	for (size_t i = 0; i < array_size(cmds[cmd]) && cmds[cmd][i]; ++i)
		argv[k++] = cmds[cmd][i];
	if (cmd == NOTES_CREATE)
		argv[k++] = ".";
	if (cmd == NOTES_LIST || cmd == NOTES_FILES) {
		argv[k++] = repo;
		argv[k++] = "--";
	}
	if (cmd == NOTES_HEAD)
		argv[k++] = repo;
	argv[k] = nullptr;
	return k;
}

void
notes_argv (struct notes const *notes,
            char const         *repo,
            enum notes_cmd      cmd,
            char const         *argv[NOTES_ARGV])
{
	notes_argv_command(notes, repo, cmd, argv);
}

static int
notes_head_out (struct span out,
                char        tip[NOTES_TIP + 1])
{
	*tip = '\0';
	if (!out.len || out.ptr[out.len - 1] != '\n' ||
	    !util_is_oid((char const *)out.ptr, out.len - 1))
		return EIO;
	memcpy(tip, out.ptr, out.len - 1);
	tip[out.len - 1] = '\0';
	return 0;
}

static void
notes_ancestor_argv (struct notes const *notes,
                     char const         *revision,
                     char const         *tip,
                     char const         *argv[NOTES_ARGV])
{
	static_assert(4 + array_size(fixed) + 5 <= NOTES_ARGV);
	size_t k = notes_argv_prefix(notes, false, argv);
	argv[k++] = "merge-base";
	argv[k++] = "--is-ancestor";
	argv[k++] = revision;
	argv[k++] = tip;
	argv[k] = nullptr;
}

static void
notes_diff_argv (struct notes const *notes,
                 char const         *from,
                 char const         *to,
                 char const         *name,
                 char const         *argv[NOTES_ARGV])
{
	static char const *const diff[] = {
		"diff", "--no-ext-diff", "--no-textconv", "--no-renames", "--no-color",
		"--submodule=short",
	};
	static_assert(4 + array_size(fixed) + array_size(diff) + 5 <= NOTES_ARGV);
	size_t k = notes_argv_prefix(notes, false, argv);
	for (size_t i = 0; i < array_size(diff); ++i)
		argv[k++] = diff[i];
	argv[k++] = from;
	argv[k++] = to;
	argv[k++] = "--";
	if (name)
		argv[k++] = name;
	argv[k] = nullptr;
}

/* Empty strings stand for the tip until the first stage pins it. */
static int
notes_revision_word (struct cstr_view word,
                     char             dest[NOTES_TIP + 1])
{
	if (!word.len || cstr_view_equal(word, CSTR_VIEW("current"))) {
		*dest = '\0';
		return 0;
	}
	if (!util_is_oid(word.data, word.len))
		return EINVAL;
	memcpy(dest, word.data, word.len);
	dest[word.len] = '\0';
	return 0;
}

struct notes_revision *
notes_revision_create (struct cstr_view file,
                       struct cstr_view from,
                       struct cstr_view to,
                       int             *e)
{
	static_assert(NOTES_NAME <= UINT8_MAX);
	*e = notes_name(&file);
	if (*e)
		return nullptr;
	struct notes_revision *revision = calloc(1, sizeof *revision);
	if (!revision) {
		*e = ENOMEM;
		return nullptr;
	}
	*e = notes_revision_word(from, revision->from);
	if (!*e)
		*e = notes_revision_word(to, revision->to);
	if (*e) {
		notes_revision_destroy(&revision);
		return nullptr;
	}
	revision->file_len = (uint8_t)file.len;
	memcpy(revision->file, file.data, file.len);
	return revision;
}

void
notes_revision_destroy (struct notes_revision **p_revision)
{
	if (!p_revision)
		return;
	free(*p_revision);
	*p_revision = nullptr;
}

void
notes_revision_argv (struct notes_revision const *revision,
                     struct notes const          *notes,
                     char const                  *repo,
                     bool                         diff,
                     char const                  *argv[NOTES_ARGV])
{
	switch (revision->phase) {
	case REVISION_HEAD:
		notes_argv(notes, repo, NOTES_HEAD, argv);
		break;
	case REVISION_FROM:
		notes_ancestor_argv(notes, revision->from, revision->tip, argv);
		break;
	case REVISION_TO:
		notes_ancestor_argv(notes, revision->to, revision->tip, argv);
		break;
	case REVISION_DATA:
		if (diff)
			notes_diff_argv(notes, revision->from, revision->to, revision->file, argv);
		else
			notes_argv(notes, repo, NOTES_READ, argv);
		break;
	}
}

void
notes_revision_history_argv (struct notes_revision const *revision,
                             struct notes const          *notes,
                             char const                  *repo,
                             char const                  *argv[NOTES_ARGV])
{
	static_assert(4 + array_size(fixed) + array_size(cmds[NOTES_LIST]) + 4 <= NOTES_ARGV);
	size_t k = notes_argv_command(notes, repo, NOTES_LIST, argv);
	argv[k++] = revision->file;
	argv[k] = nullptr;
}

int
notes_revision_next (struct notes_revision *revision,
                     struct span            out)
{
	if (revision->phase == REVISION_DATA)
		return EINVAL;
	if (revision->phase == REVISION_HEAD) {
		int e = notes_head_out(out, revision->tip);
		if (e)
			return e;
		if ((*revision->from && strlen(revision->from) != out.len - 1) ||
		    (*revision->to && strlen(revision->to) != out.len - 1))
			return EINVAL;
		if (!*revision->from)
			memcpy(revision->from, revision->tip, sizeof revision->from);
		if (!*revision->to)
			memcpy(revision->to, revision->tip, sizeof revision->to);
		if (strcmp(revision->from, revision->tip)) {
			revision->phase = REVISION_FROM;
			return 0;
		}
	} else if (out.len)
		return EIO;
	revision->phase = revision->phase != REVISION_TO &&
	                  strcmp(revision->to, revision->tip) &&
	                  strcmp(revision->to, revision->from) ? REVISION_TO : REVISION_DATA;
	return 0;
}

bool
notes_revision_ready (struct notes_revision const *revision)
{
	return revision->phase == REVISION_DATA;
}

struct cstr_view
notes_revision_file (struct notes_revision const *revision)
{
	return (struct cstr_view){ revision->file, revision->file_len };
}

char const *
notes_revision_from (struct notes_revision const *revision)
{
	return revision->from;
}

char const *
notes_revision_to (struct notes_revision const *revision)
{
	return revision->to;
}

char const *
notes_revision_tip (struct notes_revision const *revision)
{
	return revision->tip;
}

/* Returns true if the part of a name of @p n bytes at @p part is a name
 * that NTFS would read as .git: ".git" or "git~1" in any case, followed
 * only by spaces and dots up to the end, a backslash or a colon. The check
 * looks at the part and at each piece of it after a backslash. git refuses
 * such names on every system (core.protectNTFS), so askai refuses them
 * before git runs.
 */
static bool
notes_is_dotgit (char const *part,
                 size_t      n)
{
	for (size_t at = 0; at <= n; ) {
		char const *s = part + at;
		struct cstr_view const rest = { s, n - at };
		size_t k = cstr_view_starts_case(rest, CSTR_VIEW(".git")) ? 4
		         : cstr_view_starts_case(rest, CSTR_VIEW("git~1")) ? 5 : 0;
		while (k && k < rest.len && (s[k] == ' ' || s[k] == '.'))
			++k;
		if (k && (k == rest.len || s[k] == ':' || s[k] == '\\'))
			return true;
		char const *bs = memchr(s, '\\', rest.len);
		if (!bs)
			return false;
		at += (size_t)(bs - s) + 1;
	}
	return false;
}

int
notes_lock (struct notes const *notes,
            char const         *repo,
            int64_t            *age)
{
	*age = 0;

	struct timespec now = clk_now_or_1969(CLOCK_REALTIME);
	if (now.tv_sec < 0)
		return clk_errno(now);

	struct timespec up = clk_now_or_1969(CLOCK_BOOTTIME);
	if (up.tv_sec < 0)
		return clk_errno(up);

	char *path = nullptr;
	if (asprintf(&path, "%s/%s.lock", notes->directory, repo) < 0)
		return ENOMEM;
	struct stat st;
	int e = 0;
	if (lstat(path, &st))
		e = errno == ENOENT ? 0 : errno;
	/* A margin keeps a clock step after the boot from making a live lock
	 * look older than the boot. */
	else if (!S_ISREG(st.st_mode) || st.st_mtime >= now.tv_sec - up.tv_sec - NOTES_STALE)
		*age = now.tv_sec - st.st_mtime;
	else if (unlink(path) && errno != ENOENT)
		e = errno;
	free(path);
	path = nullptr;
	return e;
}

bool
notes_exists (struct notes const *notes)
{
	if (!notes)
		return false;
	struct stat st;
	return !fstatat(notes->fd, notes->bare ? "HEAD" : ".git", &st,
	                AT_SYMLINK_NOFOLLOW);
}

int
notes_name (struct cstr_view *name)
{
	size_t k = 0;
	while (k < name->len && name->data[k] == '/')
		++k;
	*name = cstr_view_slice(*name, k, SIZE_MAX);
	char const *s = name->data;
	size_t const len = name->len;
	if (!len || len > NOTES_NAME)
		return EINVAL;
	for (size_t i = 0; i < len; ++i)
		if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f)
			return EINVAL;
	/* The last part of a name that ends with a slash is empty. */
	for (size_t at = 0; at <= len; ) {
		char const *part = s + at;
		char const *slash = memchr(part, '/', len - at);
		size_t n = slash ? (size_t)(slash - part) : len - at;
		if (!n || (n == 1 && part[0] == '.') ||
		    (n == 2 && part[0] == '.' && part[1] == '.') || notes_is_dotgit(part, n))
			return EINVAL;
		at += n + 1;
	}
	return 0;
}

int
notes_check_input (struct buf       *in,
                   char const       *repo,
                   struct cstr_view  name)
{
	if (!in || !repo || !name.data)
		return EINVAL;
	size_t branch = strlen(repo);
	/* The input consists of a line for the branch, a line for each
	 * directory in the name, and a line for the name. */
	size_t lines = 1;
	for (size_t i = 0; i < name.len; ++i)
		lines += name.data[i] == '/';
	struct span s;
	int e = buf_reserve(in, branch + 1 + lines * (branch + 2 + name.len), SIZE_MAX, &s);
	if (e)
		return e;
	char *p = mempcpy(s.ptr, repo, branch);
	*p++ = '\n';
	for (size_t i = 0; i <= name.len; ++i) {
		if (i < name.len && name.data[i] != '/')
			continue;
		p = mempcpy(p, repo, branch);
		*p++ = ':';
		p = mempcpy(p, name.data, i);
		*p++ = '\n';
	}
	buf_commit(in, (size_t)(p - (char *)s.ptr));
	return 0;
}

/* Parses one unsigned decimal field without accepting a prefix of a token. */
static bool
notes_number (char const *s,
              size_t      len,
              uint64_t   *value)
{
	return len && util_digits(s, len, UINT64_MAX, value) == len;
}

/* Returns the kind of the object of the line of @p len bytes at @p line,
 * without its newline, in the output of git cat-file: "OID TYPE SIZE", or
 * "NAME missing". Stores the length of OID in @p *oid.
 */
static enum notes_kind
notes_kind (char const *line,
            size_t      len,
            size_t     *oid)
{
	static char const missing[] = " missing";
	static char const types[][8] = {
		[KIND_BLOB] = "blob", [KIND_TREE] = "tree", [KIND_COMMIT] = "commit",
	};
	*oid = 0;
	if (len >= sizeof missing - 1 &&
	    !memcmp(line + len - (sizeof missing - 1), missing, sizeof missing - 1))
		return KIND_MISSING;
	char const *sp = memchr(line, ' ', len);
	char const *type = sp ? sp + 1 : line;
	char const *end = sp ? memchr(type, ' ', len - (size_t)(type - line)) : nullptr;
	if (!end)
		return KIND_OTHER;
	size_t n = (size_t)(end - type);
	*oid = (size_t)(sp - line);
	for (enum notes_kind k = KIND_BLOB; k < KIND_OTHER; ++k)
		if (n < sizeof types[k] && !memcmp(type, types[k], n) && !types[k][n])
			return k;
	return KIND_OTHER;
}

int
notes_checked (struct span        out,
               char               tip[NOTES_TIP + 1],
               struct notes_file *note)
{
	if (!tip || !note)
		return EINVAL;
	char const *p = (char const *)out.ptr;
	char const *end = p + out.len;
	*tip = '\0';
	*note = (struct notes_file){};
	/* The first line is about the branch, the last line is about the name,
	 * and the lines between are about the directories in the name. */
	size_t lines = 0;
	bool dir_note = false;
	enum notes_kind kind = KIND_OTHER;
	char const *line = p;
	size_t oid = 0;
	for (char const *nl; p < end; p = nl + 1, ++lines) {
		if (!(nl = memchr(p, '\n', (size_t)(end - p))))
			return EIO;
		dir_note |= lines > 1 && kind == KIND_BLOB;
		line = p;
		kind = notes_kind(p, (size_t)(nl - p), &oid);
		if (lines || kind == KIND_MISSING)
			continue;
		/* The name of a commit is 40 hexadecimal digits, or 64 with
		 * SHA-256. */
		if (kind != KIND_COMMIT || !util_is_oid(p, oid))
			return EIO;
		memcpy(tip, p, oid);
		tip[oid] = '\0';
	}
	int r = lines < 2 || kind == KIND_OTHER || kind == KIND_COMMIT ? EIO
	      : dir_note ? ENOTDIR : kind == KIND_TREE ? EISDIR
	      : kind == KIND_BLOB ? EEXIST : 0;
	if (r != EEXIST)
		return r;
	/* The last line is "OID blob SIZE". */
	char const *size = line + oid + sizeof " blob " - 1;
	uint64_t bytes;
	if (!util_is_oid(line, oid) || !notes_number(size, (size_t)(end - 1 - size), &bytes))
		return EIO;
	*note = (struct notes_file){
		.id = { out.ptr + (line - (char const *)out.ptr), oid },
		.bytes = bytes,
	};
	return EEXIST;
}

int
notes_file_same (struct notes_file const *file,
                 struct span              data,
                 bool                    *same)
{
	if (!file || !same || !util_is_oid((char const *)file->id.ptr, file->id.len))
		return EINVAL;
	*same = false;
	if (data.len != file->bytes)
		return 0;
	/* The id of a blob is the digest of "blob SIZE", a NUL and the bytes:
	 * SHA-1 for 40 digits, SHA-256 for 64. */
	char head[sizeof "blob " + UTIL_DECIMAL];
	int h = snprintf(head, sizeof head, "blob %zu", data.len);
	if (h < 0 || h >= (int)sizeof head)
		return EINVAL;
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned n = 0;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	bool ok = ctx &&
	          EVP_DigestInit_ex(ctx, file->id.len == 40 ? EVP_sha1() : EVP_sha256(),
	                            nullptr) == 1 &&
	          EVP_DigestUpdate(ctx, head, (size_t)h + 1) == 1 &&
	          EVP_DigestUpdate(ctx, data.ptr, data.len) == 1 &&
	          EVP_DigestFinal_ex(ctx, digest, &n) == 1 && 2 * n == file->id.len;
	EVP_MD_CTX_free(ctx);
	if (!ok)
		return EIO;
	char hex[2 * EVP_MAX_MD_SIZE];
	util_hex(hex, digest, n);
	*same = !memcmp(hex, file->id.ptr, file->id.len);
	return 0;
}

int
notes_add_head (struct buf        *in,
                struct cstr_view   name,
                size_t             len,
                struct notes_part *part)
{
	static char const m[] = "M 100644 inline \"";
	char head[sizeof m + 2 * NOTES_NAME + sizeof "\"\ndata \n" + UTIL_DECIMAL];
	if (!in || !name.data || name.len > NOTES_NAME || !part)
		return EINVAL;
	/* The name is quoted as a C string. */
	char *p = mempcpy(head, m, sizeof m - 1);
	for (size_t i = 0; i < name.len; ++i) {
		if (name.data[i] == '"' || name.data[i] == '\\')
			*p++ = '\\';
		*p++ = name.data[i];
	}
	int n = snprintf(p, (size_t)(head + sizeof head - p), "\"\ndata %zu\n", len);
	if (n < 0 || n >= head + sizeof head - p)
		return EINVAL;
	size_t h = (size_t)(p + n - head);
	struct span s;
	int e = buf_reserve(in, h + len + 1, SIZE_MAX, &s);
	if (e)
		return e;
	memcpy(s.ptr, head, h);
	s.ptr[h + len] = '\n';
	/* The name runs from the last byte of m to the quote at p. */
	size_t at = buf_peek(in).len;
	*part = (struct notes_part){
		.name_at = at + sizeof m - 2,
		.name_len = (size_t)(p - head) - (sizeof m - 2) + 1,
		.data_at = at + h,
		.data_len = len,
	};
	buf_commit(in, h + len + 1);
	return 0;
}

int
notes_add_file (struct buf        *in,
                struct cstr_view   name,
                struct cstr_view   text,
                struct notes_part *part)
{
	int e = notes_add_head(in, name, text.len, part);
	if (!e)
		memcpy(buf_peek(in).ptr + part->data_at, text.data, text.len);
	return e;
}

int
notes_add_input (struct buf  *in,
                 char const  *repo,
                 char const  *tip,
                 struct span  message,
                 struct span  file)
{
	static char const mark[] = "get-mark :1\n";
	char head[NOTES_REPO + NOTES_MESSAGE + NOTES_TIP + UTIL_DECIMAL +
	          sizeof "commit \nmark :1\ncommitter askai <askai> now\ndata \n\nfrom \n"];
	if (!in || !repo || !tip || !message.ptr || message.len >= NOTES_MESSAGE ||
	    memchr(message.ptr, '\0', message.len))
		return EINVAL;
	/* The count of the data includes the line feed after the message. */
	int h = snprintf(head, sizeof head, "commit %s\nmark :1\ncommitter askai <askai> now\n"
	                 "data %zu\n%.*s\nfrom %s\n", repo, message.len + 1,
	                 (int)message.len, (char const *)message.ptr,
	                 *tip ? tip : "0000000000000000000000000000000000000000");
	if (h < 0 || h >= (int)sizeof head)
		return EINVAL;
	/* get-mark ends the commit, and git writes the commit's id. */
	size_t len = (size_t)h + file.len + sizeof mark - 1;
	struct span s;
	int e = buf_reserve(in, len, SIZE_MAX, &s);
	if (e)
		return e;
	memcpy(s.ptr, head, (size_t)h);
	memcpy(s.ptr + h, file.ptr, file.len);
	memcpy(s.ptr + h + file.len, mark, sizeof mark - 1);
	buf_commit(in, len);
	return 0;
}

int
notes_add_out (struct span out,
               char        commit[NOTES_TIP + 1])
{
	if (!commit)
		return EINVAL;
	unsigned char *nl = out.len > 1 ? memrchr(out.ptr, '\n', out.len - 1) : nullptr;
	if (nl)
		out = (struct span){ nl + 1, out.len - (size_t)(nl + 1 - out.ptr) };
	return notes_head_out(out, commit);
}

int
notes_read_input (struct buf       *in,
                  char const       *repo,
                  struct cstr_view  name)
{
	if (!in || !repo || !name.data)
		return EINVAL;
	size_t branch = strlen(repo);
	size_t len = branch + name.len + 2;
	struct span s;
	int e = buf_reserve(in, len, SIZE_MAX, &s);
	if (e)
		return e;
	char *p = mempcpy(s.ptr, repo, branch);
	*p++ = ':';
	p = mempcpy(p, name.data, name.len);
	*p = '\n';
	buf_commit(in, len);
	return 0;
}

static int
notes_read_decode (struct span  out,
                   struct span *text,
                   struct span *id,
                   bool         full)
{
	*text = (struct span){};
	if (id)
		*id = (struct span){};
	char const *line = (char const *)out.ptr;
	char const *nl = out.len ? memchr(line, '\n', out.len) : nullptr;
	if (!nl)
		return EIO;
	size_t n = (size_t)(nl - line);
	size_t oid = 0;
	enum notes_kind kind = notes_kind(line, n, &oid);
	if (kind == KIND_MISSING)
		return ENOENT;
	if (kind == KIND_TREE)
		return EISDIR;
	if (kind != KIND_BLOB || !util_is_oid(line, oid))
		return EIO;
	/* The size of the text is the last word of the line. The end of the
	 * output limits the text. */
	char const *digits = (char const *)memrchr(line, ' ', n) + 1;
	uint64_t size;
	if (!notes_number(digits, (size_t)(nl - digits), &size))
		return EIO;
	size_t room = out.len - n - 1;
	if (full && (!room || size != room - 1 || out.ptr[out.len - 1] != '\n'))
		return EIO;
	*text = (struct span){ out.ptr + n + 1, size < room ? (size_t)size : room };
	if (id)
		*id = (struct span){ out.ptr, oid };
	return 0;
}

int
notes_read_out (struct span  out,
                struct span *text,
                struct span *id)
{
	return notes_read_decode(out, text, id, false);
}

int
notes_read_full_out (struct span  out,
                     struct span *text,
                     struct span *id)
{
	return notes_read_decode(out, text, id, true);
}

/* Returns the offset of the start of the token that ends with the NUL at
 * the offset @p nul in @p s.
 */
static size_t
notes_token (char const *s,
             size_t      nul)
{
	char const *prev = memrchr(s, '\0', nul);
	return prev ? (size_t)(prev - s) + 1 : 0;
}

int
notes_file_next (struct span       *out,
                 struct notes_file *file)
{
	*file = (struct notes_file){};
	if (!out->len)
		return ENOENT;
	char const *s = (char const *)out->ptr;
	char const *nul = memchr(s, '\0', out->len);
	char const *tab = nul ? memchr(s, '\t', (size_t)(nul - s)) : nullptr;
	if (!tab || tab - s < (ptrdiff_t)sizeof "100644 blob " ||
	    memcmp(s + 6, " blob ", 6) || tab + 1 == nul)
		return EIO;
	char const *id = s + sizeof "100644 blob " - 1;
	char const *end = memchr(id, ' ', (size_t)(tab - id));
	size_t len = end ? (size_t)(end - id) : 0;
	if (!util_is_oid(id, len))
		return EIO;
	char const *size = end + 1;
	while (size < tab && *size == ' ')
		++size;
	uint64_t bytes;
	if (!notes_number(size, (size_t)(tab - size), &bytes))
		return EIO;
	*file = (struct notes_file){
		.name = { out->ptr + (tab + 1 - s), (size_t)(nul - tab - 1) },
		.id = { out->ptr + (id - s), len },
		.bytes = bytes,
	};
	size_t consumed = (size_t)(nul + 1 - s);
	out->ptr += consumed;
	out->len -= consumed;
	return 0;
}

bool
notes_entry_before (struct span          out,
                    struct notes_cursor *cursor,
                    struct notes_entry  *entry)
{
	char const *s = (char const *)out.ptr;
	size_t at = cursor->end;
	/* Git emits TIMESTAMP SPACE COMMIT SPACE SUBJECT\0, then one numstat
	 * token per file. Only the first file token starts with a newline. A
	 * numstat token has no space before its first tab, and a header has
	 * one, so neither can pass as the other. A cut output may end within
	 * a token. */
	if (at && s[at - 1]) {
		char const *nul = memrchr(s, '\0', at);
		at = nul ? (size_t)(nul - s) + 1 : 0;
	}
	while (at) {
		size_t stat = notes_token(s, at - 1);
		size_t start = stat + (s[stat] == '\n');
		char const *tab = memchr(s + start, '\t', at - 1 - start);
		char const *name = tab ? memchr(tab + 1, '\t', (size_t)(s + at - 1 - tab - 1))
		                       : nullptr;
		uint64_t lines = 0;
		size_t digits = tab ? (size_t)(tab - s - start) : 0;
		bool binary = digits == 1 && s[start] == '-';
		if (!name || name + 1 == s + at - 1 ||
		    (!binary && !notes_number(s + start, digits, &lines))) {
			at = stat;
			cursor->first = 0;
			continue;
		}
		if (!cursor->first || stat < cursor->first) {
			/* Find this commit's header once, then reuse it for the other
			 * files as the reverse iterator reaches them. */
			size_t before = stat;
			while (before) {
				size_t token = notes_token(s, before - 1);
				size_t len = before - 1 - token;
				char const *space = memchr(s + token, ' ', len);
				size_t digits = space ? (size_t)(space - s - token) : len;
				char const *id = space ? space + 1 : nullptr;
				char const *sep = id ? memchr(id, ' ', len - digits - 1) : nullptr;
				size_t oid = sep ? (size_t)(sep - id) : 0;
				if (util_is_oid(id, oid) && notes_number(s + token, digits, &cursor->time)) {
					cursor->commit = (struct span){ out.ptr + token + digits + 1, oid };
					cursor->subject = (struct span){ out.ptr + token + digits + oid + 2,
					                                 len - digits - oid - 2 };
					cursor->first = before;
					break;
				}
				before = token;
			}
			if (!before)
				break;
		}
		*entry = (struct notes_entry){
			.name = { (unsigned char *)(uintptr_t)(name + 1),
			          (size_t)(s + at - 1 - name - 1) },
			.commit = cursor->commit,
			.subject = cursor->subject,
			.time = cursor->time,
			.lines = binary ? NOTES_BINARY : lines,
		};
		cursor->end = stat;
		return true;
	}
	cursor->end = 0;
	return false;
}
