/** @file notes.c
 *
 * The notes tool.
 */
#define _GNU_SOURCE /* asprintf(), memrchr(), mempcpy() */
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
#include "hexid.h"
#include "notes.h"
#include "oid.h"
#include "util.h"

/* Only this namespace is exposed by the notes tool. */
#define BRANCH "refs/heads/notes/"

struct notes {
	char *dir;    /* the real path of the shared session repository */
	char *participant;
	char *directory; /* canonical Git common directory */
	size_t named; /* the bytes of participant */
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
 * literal paths, and no worktree.
 */
static char const *const fixed[] = {
	GIT_OPTIONS_FIXED, "--literal-pathspecs", "-c", "core.bare=true",
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
	[NOTES_FILES]    = { "ls-tree", "-r", "-z", "-l", word, "--" },
	[NOTES_HEAD]     = { "rev-parse", "--verify", "--end-of-options", word },
	[NOTES_ANCESTOR] = { "merge-base", "--is-ancestor", word, word },
	[NOTES_DIFF]     = { "diff", "--no-ext-diff", "--no-textconv", "--no-renames",
	                     "--no-color", "--submodule=short", word, word, "--", word },
	[NOTES_STAT]     = { "diff", "--numstat", "--no-textconv", word, word },
	[NOTES_GREP]     = { "grep", "-IniEz", "--no-color", "-f", "-", word, "--" },
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
	if (*name != '/' && asprintf(&path, "%s/%s", base, name) < 0)
		return ENOMEM;
	*resolved = realpath(path ? path : name, nullptr);
	int e = *resolved ? 0 : errno;
	free(path);
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
		static char const gitdir[] = "gitdir: ";
		e = notes_line(notes->dir, ".git", &line);
		if (!e)
			e = strncmp(line, gitdir, sizeof gitdir - 1) ? EINVAL
			  : notes_resolve(notes->dir, line + sizeof gitdir - 1, &git);
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
	free(git);
	return e;
}

struct notes *
notes_create (char const *path,
              char const *participant,
              char       *why,
              size_t      len,
              int        *e)
{
	size_t const named = util_word_len(participant, UTIL_WORD_MAX);
	if (!named) {
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
	notes->fd = -1;
	notes->named = named;
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
notes_book (struct cstr_view participant,
            uint64_t         principal,
            enum notes_side  side,
            char             repo[NOTES_REPO])
{
	static_assert(NOTES_REPO >= sizeof BRANCH + UTIL_WORD_MAX + 1 + NOTES_WHO + 1 +
	                            sizeof "upstream" - 1);
	char hex[NOTES_WHO + 1];
	struct cstr_view const who = principal
	                           ? util_format_view(hex, sizeof hex, "%0*" PRIx64, NOTES_WHO,
	                                              principal)
	                           : notes_anonymous;
	util_format(repo, NOTES_REPO, BRANCH "%.*s/%.*s/%s", (int)participant.len,
	            participant.data, (int)who.len, who.data, notes_sides[side].data);
}

void
notes_repo (struct notes const *notes,
            uint64_t            principal,
            enum notes_side     side,
            char                repo[NOTES_REPO])
{
	notes_book((struct cstr_view){ notes->participant, notes->named }, principal, side, repo);
}

int
notes_select (struct cstr_view name,
              char             repo[NOTES_REPO])
{
	if (!name.data || sizeof BRANCH + name.len > NOTES_REPO || !notes_selector_valid(name))
		return EINVAL;
	if (repo)
		util_format(repo, NOTES_REPO, BRANCH "%.*s", (int)name.len, name.data);
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
notes_revision_pin (struct notes_revision *revision,
                    struct oid const      *from,
                    struct oid const      *to)
{
	revision->from = *from;
	revision->to = *to;
	revision->phase = REVISION_DATA;
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

/* A git that writes ends, at its deadline and grace at the latest, before
 * its lock file is NOTES_STALE old. */
static_assert(NOTES_NS + GIT_GRACE < (uint64_t)NOTES_STALE * NS);

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
	/* Write a line for the branch, a line for each directory in the name,
	 * and a line for the name. */
	int e = buf_printf(in, "%s\n", repo);
	for (size_t i = 0; !e && i <= name.len; ++i)
		if (i == name.len || name.data[i] == '/')
			e = buf_printf(in, "%s:%.*s\n", repo, (int)i, name.data);
	return e;
}

/* Returns the kind of the object of @p line, a line without its newline in
 * the output of git cat-file: "OID TYPE SIZE", or "NAME missing". Stores
 * OID in @p *oid, or no name if the line is KIND_MISSING or OID is not a
 * name, which makes the line KIND_OTHER. Stores SIZE in @p *size if the
 * line has a TYPE.
 */
static enum notes_kind
notes_kind (struct cstr_view  line,
            struct oid       *oid,
            struct cstr_view *size)
{
	static struct cstr_view const types[] = {
		[KIND_BLOB] = CSTR_VIEW_INIT("blob"), [KIND_TREE] = CSTR_VIEW_INIT("tree"),
		[KIND_COMMIT] = CSTR_VIEW_INIT("commit"),
	};
	*oid = (struct oid){};
	if (cstr_view_ends(line, CSTR_VIEW(" missing")))
		return KIND_MISSING;
	struct cstr_view type;
	if (oid_init_field(oid, &line, ' ') || !cstr_view_cut(&line, ' ', &type))
		return KIND_OTHER;
	*size = line;
	return KIND_BLOB + cstr_view_index(types + KIND_BLOB, KIND_OTHER - KIND_BLOB, type);
}

int
notes_checked (struct span        out,
               struct oid        *tip,
               struct notes_file *note)
{
	if (!tip || !note)
		return EINVAL;
	*tip = (struct oid){};
	*note = (struct notes_file){};
	/* The first line is about the branch, the last line is about the name,
	 * and the lines between are about the directories in the name. */
	size_t lines = 0;
	bool dir_note = false;
	enum notes_kind kind = KIND_OTHER;
	struct cstr_view rest = { (char const *)out.ptr, out.len };
	struct cstr_view line, size = {};
	struct oid oid;
	for (; rest.len; ++lines) {
		if (!cstr_view_cut(&rest, '\n', &line))
			return EIO;
		dir_note |= lines > 1 && kind == KIND_BLOB;
		kind = notes_kind(line, &oid, &size);
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
	uint64_t bytes;
	if (util_number(size.data, size.len, UINT64_MAX, &bytes))
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
	size_t const h = util_format(head, sizeof head, "blob %zu", data.len);
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned n = 0;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	bool ok = ctx &&
	          EVP_DigestInit_ex(ctx, oid_len(&file->id) == OID_SHA1 ? EVP_sha1() : EVP_sha256(),
	                            nullptr) == 1 &&
	          EVP_DigestUpdate(ctx, head, h + 1) == 1 &&
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

/* Writes @p name at @p p in double quotes, with a backslash before each
 * double quote and backslash in it, as fast-import reads a path, and
 * returns the end. The quoted name takes at most 2 * name.len + 2 bytes.
 */
static char *
notes_quote (char             *p,
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
notes_add_head (struct buf        *in,
                struct cstr_view   name,
                size_t             len,
                struct notes_part *part)
{
	static char const m[] = "M 100644 inline ";
	if (!in || !name.data || name.len > NOTES_NAME || !part)
		return EINVAL;
	/* The head is m, the name quoted as a C string, which at most doubles
	 * its bytes, and the data command. Then come the text and a line
	 * feed. */
	struct span s;
	int e = buf_reserve(in, sizeof m + 2 * name.len + sizeof "\"\"\ndata \n" + UTIL_DECIMAL +
	                    len, SIZE_MAX, &s);
	if (e)
		return e;
	char *const head = (char *)s.ptr;
	char *p = notes_quote(mempcpy(head, m, sizeof m - 1), name);
	size_t const room = s.len - (size_t)(p - head);
	size_t const h = (size_t)(p - head) + util_format(p, room, "\ndata %zu\n", len);
	head[h + len] = '\n';
	/* The quoted name runs from the end of m to p. */
	size_t at = buf_peek(in).len;
	*part = (struct notes_part){
		.name_at = at + sizeof m - 1,
		.name_len = (size_t)(p - head) - (sizeof m - 1),
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
notes_delete_file (struct buf        *in,
                   struct cstr_view   name,
                   struct notes_part *part)
{
	if (!in || !name.data || name.len > NOTES_NAME || !part)
		return EINVAL;
	/* The part is "D ", the name quoted as a C string, which at most
	 * doubles its bytes, and a line feed. */
	struct span s;
	int e = buf_reserve(in, sizeof "D \"\"\n" + 2 * name.len, SIZE_MAX, &s);
	if (e)
		return e;
	char *const head = (char *)s.ptr;
	char *p = notes_quote(mempcpy(head, "D ", 2), name);
	size_t const at = buf_peek(in).len;
	size_t const end = (size_t)(p - head);
	*part = (struct notes_part){ .name_at = at + 2, .name_len = end - 2, .data_at = at + end };
	*p = '\n';
	buf_commit(in, end + 1);
	return 0;
}

int
notes_add_input (struct buf       *in,
                 char const       *repo,
                 char const       *book,
                 struct oid const *tip,
                 struct cstr_view  message,
                 struct span       file)
{
	if (!in || !repo || !book || !tip || !message.data ||
	    message.len >= NOTES_MESSAGE || memchr(message.data, '\0', message.len))
		return EINVAL;
	/* The count of the data includes the line feed after the message.
	 * get-mark ends the commit, and git writes the commit's id. */
	int e = buf_printf(in, "commit %s\nmark :1\ncommitter %s <askai> now\ndata %zu\n%.*s\n",
	                   repo, notes_selector(book), message.len + 1, (int)message.len, message.data);
	if (!e && oid_len(tip))
		e = buf_printf(in, "from %s\n", tip->hex);
	e = e ? e : buf_put(in, file.ptr, file.len, SIZE_MAX);
	return e ? e : buf_printf(in, "get-mark :1\n");
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
	return buf_printf(in, "%s:%.*s\n", repo, (int)name.len, name.data);
}

int
notes_read_out (struct span  out,
                struct span *text,
                struct oid  *id)
{
	*text = (struct span){};
	if (id)
		*id = (struct oid){};
	struct cstr_view rest = { (char const *)out.ptr, out.len };
	struct cstr_view line;
	if (!cstr_view_cut(&rest, '\n', &line))
		return EIO;
	struct oid oid;
	struct cstr_view digits;
	enum notes_kind kind = notes_kind(line, &oid, &digits);
	if (kind == KIND_MISSING)
		return ENOENT;
	if (kind == KIND_TREE)
		return EISDIR;
	if (kind != KIND_BLOB)
		return EIO;
	/* A line feed comes after the text. */
	uint64_t size;
	if (util_number(digits.data, digits.len, UINT64_MAX, &size) ||
	    !rest.len || size != rest.len - 1 || rest.data[size] != '\n')
		return EIO;
	*text = (struct span){ out.ptr + line.len + 1, (size_t)size };
	if (id)
		*id = oid;
	return 0;
}

void
notes_review (char const         *repo,
              struct hexid const *thread,
              char                ref[NOTES_REPO])
{
	static_assert(NOTES_REPO >= sizeof "refs/review/" + UTIL_WORD_MAX + 1 + NOTES_WHO + 1 +
	                            HEXID_DIGITS);
	char const *who = notes_selector(repo);
	util_format(ref, NOTES_REPO, "refs/review/%.*s/%s", (int)(strrchr(who, '/') - who), who,
	            thread->hex);
}

int
notes_review_out (struct span  out,
                  struct oid  *tip,
                  struct span *text)
{
	struct cstr_view rest = { (char const *)out.ptr, out.len };
	struct cstr_view line, size;
	if (!cstr_view_cut(&rest, '\n', &line))
		return EIO;
	enum notes_kind kind = notes_kind(line, tip, &size);
	if (kind == KIND_MISSING)
		return ENOENT;
	/* The commit's text and a line feed come before the line of the blob. */
	uint64_t n;
	if (kind != KIND_COMMIT || util_number(size.data, size.len, UINT64_MAX, &n) ||
	    n >= rest.len || rest.data[n] != '\n')
		return EIO;
	size_t const at = line.len + 1 + (size_t)n + 1;
	return notes_read_out((struct span){ out.ptr + at, out.len - at }, text, nullptr) ? EIO : 0;
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
	if (util_number(size.data, size.len, UINT64_MAX, &bytes))
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
		    !util_number(s + token, digits, UINT64_MAX, &cursor->time)) {
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
		uint64_t added = 0, removed = 0;
		size_t digits = tab ? (size_t)(tab - s - start) : 0;
		bool binary = digits == 1 && s[start] == '-';
		if (!name || name + 1 == s + at - 1 ||
		    (!binary && (util_number(s + start, digits, UINT64_MAX, &added) ||
		                 util_number(tab + 1, (size_t)(name - tab - 1), UINT64_MAX,
		                             &removed)))) {
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
			.added = binary ? NOTES_BINARY : added,
			.removed = removed,
		};
		cursor->end = stat;
		return true;
	}
	cursor->end = 0;
	return false;
}
