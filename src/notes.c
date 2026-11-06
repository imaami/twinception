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
#include "oid.h"
#include "util.h"

/* The variables of the environment, its nullptr included. */
enum { ENV = 10 };

/* Only this namespace is exposed by the notes tool. */
#define BRANCH "refs/heads/notes/"

/* The hexadecimal digits of a principal in a notebook's ref. */
enum { NOTES_WHO = 16 };

/* The principal in the ref of a notebook of a caller without one. */
static struct cstr_view const notes_anonymous = CSTR_VIEW_INIT("anonymous");

/* The sides in a notebook's ref. */
static struct cstr_view const notes_sides[] = {
	[NOTES_UPSTREAM] = CSTR_VIEW_INIT("upstream"),
	[NOTES_CLIENT]   = CSTR_VIEW_INIT("client"),
};

struct notes {
	char *env[ENV];
	char *dir;    /* the real path of the shared session repository */
	char *participant;
	char *directory; /* canonical Git common directory */
	int   fd;     /* the repository directory, open; -1 if not open */
	uint32_t bare;
};

/* The stages of a historical read or diff, in their order. */
enum notes_revision_phase : uint8_t {
	REVISION_HEAD,  /* git pins the tip of the notebook */
	REVISION_FROM,  /* git checks that from is an ancestor of the tip */
	REVISION_TO,    /* git checks that to is one too */
	REVISION_DATA,  /* git reads the note at from, or compares it with to */
};

/* The options of git before each command: GIT_OPTIONS_FIXED, which stop
 * the configuration of the repository from making git run other programs,
 * literal paths, and no signature checks.
 */
static char const *const fixed[] = {
	GIT_OPTIONS_FIXED, "--literal-pathspecs",
	"-c", "core.bare=true", "-c", "log.showSignature=false",
};

/* The place in a row of cmds[] where notes_argv() puts the next of the
 * words that the caller passes.
 */
static char const word[] = "";

/* The words of each command after the options in fixed. */
static char const *const cmds[][10] = {
	[NOTES_CREATE]   = { "init", "--bare", "--quiet", "--template=",
	                     "--initial-branch=session", "--", "." },
	[NOTES_CHECK]    = { "cat-file", "--batch-check" },
	[NOTES_ADD]      = { "fast-import", "--quiet", "--date-format=now" },
	[NOTES_LIST]     = { "log", "-z", "--no-renames", "--format=%ct %H %s", "--numstat",
	                     word, "--", word },
	[NOTES_READ]     = { "cat-file", "--batch" },
	[NOTES_BOOKS]    = { "for-each-ref", "--format=%(refname:strip=3)", BRANCH },
	[NOTES_FORMAT]   = { "rev-parse", "--show-object-format" },
	[NOTES_FILES]    = { "ls-tree", "-r", "-z", "-l", word, "--" },
	[NOTES_HEAD]     = { "rev-parse", "--verify", "--end-of-options", word },
	[NOTES_ANCESTOR] = { "merge-base", "--is-ancestor", word, word },
	[NOTES_DIFF]     = { "diff", "--no-ext-diff", "--no-textconv", "--no-renames",
	                     "--no-color", "--submodule=short", word, word, "--", word },
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
	if (e)
		goto fail;
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
		goto fail;
	}
	DIR *dir = fdopendir(fd);
	if (!dir) {
		e = errno;
		util_close(&fd);
		goto fail;
	}
	/* closedir() closes the descriptor. */
	fd = -1;
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
fail:
	util_format(why, len, "%s", strerror(e));
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
		*e = util_refuse(why, len, "the participant must have 1 to %d ASCII letters, "
		                 "digits, underscores or hyphens", UTIL_WORD_MAX);
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

void
notes_repo (struct notes const *notes,
            uint64_t            principal,
            enum notes_side     side,
            char                repo[NOTES_REPO])
{
	static_assert(NOTES_REPO >= sizeof BRANCH + UTIL_WORD_MAX + 1 + NOTES_WHO + 1 +
	                            sizeof "upstream" - 1);
	char hex[NOTES_WHO + 1];
	struct cstr_view const who = principal
	                           ? util_format_view(hex, sizeof hex, "%0*" PRIx64, NOTES_WHO,
	                                              principal)
	                           : notes_anonymous;
	util_format(repo, NOTES_REPO, BRANCH "%s/%.*s/%s", notes->participant, (int)who.len,
	            who.data, notes_sides[side].data);
}

int
notes_select (struct cstr_view name,
              char             repo[NOTES_REPO])
{
	if (!name.data || !name.len || sizeof BRANCH + name.len > NOTES_REPO)
		return EINVAL;
	/* who is notes_anonymous or the NOTES_WHO hexadecimal digits of a
	 * principal, as notes_repo() writes it. */
	struct cstr_view side = name, participant, who;
	if (!cstr_view_cut(&side, '/', &participant) ||
	    !util_word(participant.data, participant.len, UTIL_WORD_MAX) ||
	    !cstr_view_cut(&side, '/', &who))
		return EINVAL;
	if ((!cstr_view_equal(who, notes_anonymous) &&
	     (who.len != NOTES_WHO || !util_is_hex(who.data, who.len))) ||
	    cstr_view_index(notes_sides, array_size(notes_sides), side) == array_size(notes_sides))
		return EINVAL;
	if (!repo)
		return 0;
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

#undef BRANCH

void
notes_argv (struct notes const *notes,
            enum notes_cmd      cmd,
            char const *const  *words,
            char const         *argv[NOTES_ARGV])
{
	static_assert(4 + array_size(fixed) + array_size(cmds[0]) + 1 <= NOTES_ARGV);
	size_t k = 0;
	argv[k++] = "git";
	argv[k++] = "-C";
	if (cmd == NOTES_CREATE) {
		argv[k++] = notes->dir;
	} else {
		argv[k++] = notes->directory;
		argv[k++] = "--git-dir=.";
	}
	for (size_t i = 0; i < array_size(fixed); ++i)
		argv[k++] = fixed[i];
	for (size_t i = 0; i < array_size(cmds[cmd]) && cmds[cmd][i]; ++i)
		argv[k++] = cmds[cmd][i] == word ? *words++ : cmds[cmd][i];
	argv[k] = nullptr;
}

int
notes_revision_init (struct notes_revision *dest,
                     struct cstr_view       file,
                     struct oid const      *from,
                     struct oid const      *to)
{
	if (!dest || !from || !to || notes_name(&file))
		return EINVAL;
	/* No name stands for the tip until the first stage pins it. */
	*dest = (struct notes_revision){ .from = *from, .to = *to, .phase = REVISION_HEAD };
	(void)util_text_store(&dest->file, file);
	return 0;
}

void
notes_revision_argv (struct notes_revision const *revision,
                     struct notes const          *notes,
                     char const                  *repo,
                     enum notes_cmd               cmd,
                     char const                  *argv[NOTES_ARGV])
{
	enum notes_cmd const phases[] = {
		[REVISION_HEAD] = NOTES_HEAD,
		[REVISION_FROM] = NOTES_ANCESTOR,
		[REVISION_TO]   = NOTES_ANCESTOR,
		[REVISION_DATA] = cmd,
	};
	char const *const words[][3] = {
		[REVISION_HEAD] = { repo },
		[REVISION_FROM] = { revision->from.hex, revision->tip.hex },
		[REVISION_TO]   = { revision->to.hex, revision->tip.hex },
		[REVISION_DATA] = { revision->from.hex, revision->to.hex, revision->file.data },
	};
	notes_argv(notes, phases[revision->phase], words[revision->phase], argv);
}

int
notes_revision_next (struct notes_revision *revision,
                     struct span            out)
{
	struct oid const *const tip = &revision->tip;
	switch (revision->phase) {
	case REVISION_HEAD:
		if (oid_init_line(&revision->tip,
		                  (struct cstr_view){ (char const *)out.ptr, out.len }))
			return EIO;
		if (!oid_len(&revision->from))
			revision->from = *tip;
		if (!oid_len(&revision->to))
			revision->to = *tip;
		if (oid_len(&revision->from) != oid_len(tip) || oid_len(&revision->to) != oid_len(tip))
			return EINVAL;
		if (!oid_equal(&revision->from, tip)) {
			revision->phase = REVISION_FROM;
			return 0;
		}
		break;
	case REVISION_FROM:
		/* git merge-base --is-ancestor writes nothing. */
		if (out.len)
			return EIO;
		break;
	case REVISION_TO:
		if (out.len)
			return EIO;
		revision->phase = REVISION_DATA;
		return 0;
	case REVISION_DATA:
		return EINVAL;
	}
	revision->phase = oid_equal(&revision->to, tip) || oid_equal(&revision->to, &revision->from)
	                ? REVISION_DATA : REVISION_TO;
	return 0;
}

bool
notes_revision_ready (struct notes_revision const *revision)
{
	return revision->phase == REVISION_DATA;
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
 * "NAME missing". Stores OID in @p *oid, or no name if the line is
 * KIND_MISSING or OID is not a name, which makes the line KIND_OTHER.
 */
static enum notes_kind
notes_kind (char const *line,
            size_t      len,
            struct oid *oid)
{
	static char const types[][8] = {
		[KIND_BLOB] = "blob", [KIND_TREE] = "tree", [KIND_COMMIT] = "commit",
	};
	*oid = (struct oid){};
	if (cstr_view_ends((struct cstr_view){ line, len }, CSTR_VIEW(" missing")))
		return KIND_MISSING;
	struct cstr_view type = { line, len };
	char const *end = oid_init_field(oid, &type, ' ') ? nullptr
	                : memchr(type.data, ' ', type.len);
	if (!end)
		return KIND_OTHER;
	size_t n = (size_t)(end - type.data);
	for (enum notes_kind k = KIND_BLOB; k < KIND_OTHER; ++k)
		if (n < sizeof types[k] && !memcmp(type.data, types[k], n) && !types[k][n])
			return k;
	return KIND_OTHER;
}

int
notes_checked (struct span        out,
               struct oid        *tip,
               struct notes_file *note)
{
	if (!tip || !note)
		return EINVAL;
	char const *p = (char const *)out.ptr;
	char const *end = p + out.len;
	*tip = (struct oid){};
	*note = (struct notes_file){};
	/* The first line is about the branch, the last line is about the name,
	 * and the lines between are about the directories in the name. */
	size_t lines = 0;
	bool dir_note = false;
	enum notes_kind kind = KIND_OTHER;
	char const *line = p;
	struct oid oid;
	for (char const *nl; p < end; p = nl + 1, ++lines) {
		if (!(nl = memchr(p, '\n', (size_t)(end - p))))
			return EIO;
		dir_note |= lines > 1 && kind == KIND_BLOB;
		line = p;
		kind = notes_kind(p, (size_t)(nl - p), &oid);
		if (lines || kind == KIND_MISSING)
			continue;
		if (kind != KIND_COMMIT)
			return EIO;
		*tip = oid;
	}
	int r = lines < 2 || kind == KIND_OTHER || kind == KIND_COMMIT ? EIO
	      : dir_note ? ENOTDIR : kind == KIND_TREE ? EISDIR
	      : kind == KIND_BLOB ? EEXIST : 0;
	if (r != EEXIST)
		return r;
	/* The last line is "OID blob SIZE". */
	char const *size = line + oid_len(&oid) + sizeof " blob " - 1;
	uint64_t bytes;
	if (!notes_number(size, (size_t)(end - 1 - size), &bytes))
		return EIO;
	*note = (struct notes_file){ .bytes = bytes, .id = oid };
	return EEXIST;
}

int
notes_file_same (struct notes_file const *file,
                 struct span              data,
                 bool                    *same)
{
	if (!file || !same || !oid_len(&file->id))
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
	          EVP_DigestInit_ex(ctx, oid_len(&file->id) == OID_SHA1 ? EVP_sha1() : EVP_sha256(),
	                            nullptr) == 1 &&
	          EVP_DigestUpdate(ctx, head, (size_t)h + 1) == 1 &&
	          EVP_DigestUpdate(ctx, data.ptr, data.len) == 1 &&
	          EVP_DigestFinal_ex(ctx, digest, &n) == 1 && 2 * n == oid_len(&file->id);
	EVP_MD_CTX_free(ctx);
	if (!ok)
		return EIO;
	struct oid got = {};
	util_hex(got.hex, digest, n);
	*same = oid_equal(&got, &file->id);
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
notes_add_input (struct buf       *in,
                 char const       *repo,
                 struct oid const *tip,
                 struct span       message,
                 struct span       file)
{
	static char const mark[] = "get-mark :1\n";
	char head[NOTES_REPO + NOTES_MESSAGE + OID_SHA256 + UTIL_DECIMAL +
	          sizeof "commit \nmark :1\ncommitter askai <askai> now\ndata \n\nfrom \n"];
	if (!in || !repo || !tip || !oid_len(tip) || !message.ptr ||
	    message.len >= NOTES_MESSAGE || memchr(message.ptr, '\0', message.len))
		return EINVAL;
	/* The count of the data includes the line feed after the message. */
	int h = snprintf(head, sizeof head, "commit %s\nmark :1\ncommitter askai <askai> now\n"
	                 "data %zu\n%.*s\nfrom %s\n", repo, message.len + 1,
	                 (int)message.len, (char const *)message.ptr, tip->hex);
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
notes_add_out (struct span  out,
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

int
notes_read_out (struct span  out,
                struct span *text,
                struct oid  *id)
{
	*text = (struct span){};
	if (id)
		*id = (struct oid){};
	char const *line = (char const *)out.ptr;
	char const *nl = out.len ? memchr(line, '\n', out.len) : nullptr;
	if (!nl)
		return EIO;
	size_t n = (size_t)(nl - line);
	struct oid oid;
	enum notes_kind kind = notes_kind(line, n, &oid);
	if (kind == KIND_MISSING)
		return ENOENT;
	if (kind == KIND_TREE)
		return EISDIR;
	if (kind != KIND_BLOB)
		return EIO;
	/* The size of the text is the last word of the line, and a line feed
	 * comes after the text. */
	char const *digits = (char const *)memrchr(line, ' ', n) + 1;
	uint64_t size;
	size_t room = out.len - n - 1;
	if (!notes_number(digits, (size_t)(nl - digits), &size) || !room || size != room - 1 ||
	    out.ptr[out.len - 1] != '\n')
		return EIO;
	*text = (struct span){ out.ptr + n + 1, (size_t)size };
	if (id)
		*id = oid;
	return 0;
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
	/* The record is "MODE blob OID SIZE\tNAME\0", with spaces before SIZE. */
	struct cstr_view rest = { s, tab ? (size_t)(tab - s) : 0 };
	struct cstr_view mode, type;
	struct oid id;
	if (!tab || tab + 1 == nul || !cstr_view_cut(&rest, ' ', &mode) ||
	    !cstr_view_cut(&rest, ' ', &type) || !cstr_view_equal(type, CSTR_VIEW("blob")) ||
	    oid_init_field(&id, &rest, ' '))
		return EIO;
	struct cstr_view const size = cstr_view_trim(rest);
	uint64_t bytes;
	if (!notes_number(size.data, size.len, &bytes))
		return EIO;
	*file = (struct notes_file){
		.name = { out->ptr + (tab + 1 - s), (size_t)(nul - tab - 1) },
		.bytes = bytes,
		.id = id,
	};
	size_t consumed = (size_t)(nul + 1 - s);
	out->ptr += consumed;
	out->len -= consumed;
	return 0;
}

/* Finds the header of the commit before the numstat token at @p stat in
 * @p out, the output of NOTES_LIST, and stores its commit, time, subject
 * and end in @p cursor. Returns false if no header precedes @p stat, as in
 * an output cut short.
 */
static bool
notes_cursor_header (struct span          out,
                     struct notes_cursor *cursor,
                     size_t               stat)
{
	char const *s = (char const *)out.ptr;
	for (size_t before = stat; before; ) {
		size_t token = notes_token(s, before - 1);
		size_t len = before - 1 - token;
		char const *space = memchr(s + token, ' ', len);
		size_t digits = space ? (size_t)(space - s - token) : len;
		struct cstr_view rest = cstr_view_slice((struct cstr_view){ s + token, len },
		                                        digits + 1, SIZE_MAX);
		if (!oid_init_field(&cursor->commit, &rest, ' ') &&
		    notes_number(s + token, digits, &cursor->time)) {
			cursor->subject = (struct span){ out.ptr + (rest.data - s), rest.len };
			cursor->first = before;
			return true;
		}
		before = token;
	}
	return false;
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
		/* Find this commit's header once, and reuse it for the other
		 * files of the commit, which the reverse iterator reads later. */
		if ((!cursor->first || stat < cursor->first) &&
		    !notes_cursor_header(out, cursor, stat))
			break;
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
