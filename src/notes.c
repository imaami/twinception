/** @file The notes (notes.h). */
#define _GNU_SOURCE /* memrchr(), mempcpy() */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include "notes.h"
#include "util.h"

/* The variables of the environment, its nullptr included. */
enum { ENV = 10 };

/* The branch that has the notes. */
#define BRANCH "refs/heads/notes"

struct notes {
	char *env[ENV];
	char *dir;    /* the real path of the directory of the threads */
};

/* The option of git that names a repository. */
static char const git_dir[] = "--git-dir=";

/* The options of git before each command.  They stop the configuration of
 * the repository from making git run other programs, and they stop git
 * from running the hooks of the repository when it updates a branch. */
static char const *const fixed[] = {
	"--no-pager", "--no-replace-objects", "-c", "core.hooksPath=/dev/null", "-c",
	"log.showSignature=false",
};

/* The words of each command after the options in fixed. */
static char const *const cmds[][8] = {
	[NOTES_CREATE] = { "init", "--bare", "--quiet", "--template=",
	                   "--initial-branch=notes", "--" },
	[NOTES_CHECK]  = { "cat-file", "--batch-check" },
	[NOTES_ADD]    = { "fast-import", "--quiet", "--date-format=now" },
	[NOTES_LIST]   = { "log", "-z", "--no-renames", "--format=%ct", "--numstat",
	                   BRANCH, "--" },
	[NOTES_READ]   = { "cat-file", "--batch" },
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
 * path in notes->dir.  Returns 0, or the error number with a message in
 * @p why, of @p len bytes. */
static int
notes_open (struct notes *notes,
            char const   *path,
            char         *why,
            size_t        len)
{
	int e = mkdir(path, 0700) && errno != EEXIST ? errno : 0;
	if (!e && !(notes->dir = realpath(path, nullptr)))
		e = errno;
	int fd = e ? -1 : open(notes->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (!e && fd < 0)
		e = errno;
	if (e) {
		util_format(why, len, "%s", strerror(e));
		return e;
	}
	struct stat st;
	bool repo = !fstatat(fd, "HEAD", &st, AT_SYMLINK_NOFOLLOW);
	close(fd);
	if (!repo)
		return 0;
	util_format(why, len, "the directory is a git repository.  Older versions "
	            "of askai kept all the notes in one repository, but askai now "
	            "keeps a repository for each thread in the directory.  Name a "
	            "new directory");
	return EINVAL;
}

struct notes *
notes_create (char const *path,
              char       *why,
              size_t      len,
              int        *e)
{
	static char const env_path[] = "PATH=";
	char const *search = getenv("PATH");
	struct notes *notes = calloc(1, sizeof *notes);
	char *env = notes ? util_concat(env_path, sizeof env_path - 1,
	                                search && *search ? search : "/usr/bin:/bin")
	                  : nullptr;
	if (!env) {
		free(notes);
		util_format(why, len, "out of memory");
		*e = ENOMEM;
		return nullptr;
	}
	char *const vars[ENV] = {
		env, "HOME=/nonexistent", "LC_ALL=C.UTF-8", "GIT_CONFIG_NOSYSTEM=1",
		"GIT_CONFIG_GLOBAL=/dev/null", "GIT_TERMINAL_PROMPT=0",
		"GIT_NO_LAZY_FETCH=1", "GIT_ALLOW_PROTOCOL=none", "GIT_PAGER=cat",
		nullptr
	};
	static_assert(sizeof vars == sizeof notes->env);
	memcpy(notes->env, vars, sizeof vars);
	if ((*e = notes_open(notes, path, why, len)))
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
	free(notes->env[0]);
	free(notes->dir);
	free(notes);
}

char *const *
notes_env (struct notes const *notes)
{
	return notes->env;
}

int
notes_id (char id[NOTES_ID + 1])
{
	static char const hex[] = "0123456789abcdef";
	unsigned char rnd[NOTES_ID / 2];
	/* For so few bytes, getrandom() returns fewer bytes only if a signal
	 * interrupts it. */
	ssize_t got;
	while ((got = getrandom(rnd, sizeof rnd, 0)) < 0)
		if (errno != EINTR)
			return errno;
	if (got != (ssize_t)sizeof rnd)
		return EIO;
	for (size_t i = 0; i < sizeof rnd; ++i) {
		id[2 * i] = hex[rnd[i] >> 4];
		id[2 * i + 1] = hex[rnd[i] & 15];
	}
	id[NOTES_ID] = '\0';
	return 0;
}

bool
notes_is_id (char const *s,
             size_t      len)
{
	return len == NOTES_ID && strspn(s, "0123456789abcdef") >= NOTES_ID;
}

void
notes_repo (uint64_t        principal,
            enum notes_side side,
            char const     *id,
            char            repo[NOTES_REPO])
{
	static char const digits[] = "0123456789abcdef";
	static char const sides[][sizeof "/upstream/"] = {
		[NOTES_UPSTREAM] = "/upstream/",
		[NOTES_CLIENT]   = "/client/",
	};
	static_assert(NOTES_REPO > sizeof git_dir - 1 + 16 + sizeof sides[0] - 1 + NOTES_ID);
	char *p = mempcpy(repo, git_dir, sizeof git_dir - 1);
	if (principal) {
		for (unsigned i = 16; i--; principal >>= 4)
			p[i] = digits[principal & 15];
		p += 16;
	} else {
		p = stpcpy(p, "anonymous");
	}
	p = stpcpy(p, sides[side]);
	memcpy(p, id, NOTES_ID);
	p[NOTES_ID] = '\0';
}

void
notes_argv (struct notes const *notes,
            char const         *repo,
            enum notes_cmd      cmd,
            char const         *argv[NOTES_ARGV])
{
	static_assert(4 + ARRAY_SIZE(fixed) + ARRAY_SIZE(cmds[0]) + 1 <= NOTES_ARGV);
	size_t k = 0;
	argv[k++] = "git";
	argv[k++] = "-C";
	argv[k++] = notes->dir;
	/* git init makes the directories above the repository only if the
	 * repository is its argument. */
	if (cmd != NOTES_CREATE)
		argv[k++] = repo;
	for (size_t i = 0; i < ARRAY_SIZE(fixed); ++i)
		argv[k++] = fixed[i];
	for (size_t i = 0; i < ARRAY_SIZE(cmds[cmd]) && cmds[cmd][i]; ++i)
		argv[k++] = cmds[cmd][i];
	if (cmd == NOTES_CREATE)
		argv[k++] = repo + sizeof git_dir - 1;
	argv[k] = nullptr;
}

/* Returns true if the part of a name of @p n bytes at @p part is a name
 * that NTFS would read as .git: ".git" or "git~1" in any case, followed
 * only by spaces and dots up to the end, a backslash or a colon.  The check
 * looks at the part and at each piece of it after a backslash.  git refuses
 * such names on every system (core.protectNTFS), so askai refuses them
 * before git runs. */
static bool
notes_is_dotgit (char const *part,
                 size_t      n)
{
	for (size_t at = 0; at <= n; ) {
		char const *s = part + at;
		size_t m = n - at;
		size_t k = m >= 4 && !strncasecmp(s, ".git", 4) ? 4
		         : m >= 5 && !strncasecmp(s, "git~1", 5) ? 5 : 0;
		while (k && k < m && (s[k] == ' ' || s[k] == '.'))
			++k;
		if (k && (k == m || s[k] == ':' || s[k] == '\\'))
			return true;
		char const *bs = memchr(s, '\\', m);
		if (!bs)
			return false;
		at += (size_t)(bs - s) + 1;
	}
	return false;
}

int
notes_name (char const *name,
            size_t      len,
            size_t     *skip)
{
	size_t k = 0;
	while (k < len && name[k] == '/')
		++k;
	*skip = k;
	if (k == len || len - k > NOTES_NAME)
		return EINVAL;
	for (size_t i = k; i < len; ++i)
		if ((unsigned char)name[i] < 0x20 || name[i] == 0x7f)
			return EINVAL;
	/* A name that ends with a slash has an empty last part. */
	for (size_t at = k; at <= len; ) {
		char const *part = name + at;
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
notes_check_input (struct buf *in,
                   char const *name,
                   size_t      len)
{
	static char const at[] = BRANCH ":";
	/* The input has a line for the branch, a line for each directory in the
	 * name, and a line for the name. */
	size_t lines = 1;
	for (size_t i = 0; i < len; ++i)
		lines += name[i] == '/';
	struct span s;
	int e = buf_reserve(in, sizeof BRANCH + lines * (sizeof at + len), SIZE_MAX, &s);
	if (e)
		return e;
	char *p = mempcpy(s.ptr, BRANCH "\n", sizeof BRANCH);
	for (size_t i = 0; i <= len; ++i) {
		if (i < len && name[i] != '/')
			continue;
		p = mempcpy(p, at, sizeof at - 1);
		p = mempcpy(p, name, i);
		*p++ = '\n';
	}
	buf_commit(in, (size_t)(p - (char *)s.ptr));
	return 0;
}

/* Returns the kind of the object of the line of @p len bytes at @p line,
 * without its newline, in the output of git cat-file: "OID TYPE SIZE", or
 * "NAME missing".  Stores the length of OID in @p *oid. */
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
notes_checked (struct span out,
               char        tip[NOTES_TIP + 1])
{
	char const *p = (char const *)out.ptr;
	char const *end = p + out.len;
	*tip = '\0';
	/* The first line is about the branch, the last line is about the name,
	 * and the lines between are about the directories in the name. */
	size_t lines = 0;
	bool dir_note = false;
	enum notes_kind kind = KIND_OTHER;
	for (char const *nl; p < end; p = nl + 1, ++lines) {
		if (!(nl = memchr(p, '\n', (size_t)(end - p))))
			return EIO;
		dir_note |= lines > 1 && kind == KIND_BLOB;
		size_t oid = 0;
		kind = notes_kind(p, (size_t)(nl - p), &oid);
		if (lines || kind == KIND_MISSING)
			continue;
		if (kind != KIND_COMMIT || oid > NOTES_TIP)
			return EIO;
		memcpy(tip, p, oid);
		tip[oid] = '\0';
	}
	return lines < 2 || kind == KIND_OTHER || kind == KIND_COMMIT ? EIO
	     : dir_note ? ENOTDIR : kind == KIND_TREE ? EISDIR
	     : kind == KIND_BLOB ? EEXIST : 0;
}

int
notes_add_file (struct buf *in,
                char const *name,
                size_t      name_len,
                char const *text,
                size_t      len)
{
	static char const m[] = "M 100644 inline \"";
	char head[sizeof m + 2 * NOTES_NAME + sizeof "\"\ndata 18446744073709551615\n"];
	if (name_len > NOTES_NAME)
		return EINVAL;
	/* The name is quoted as a C string. */
	char *p = mempcpy(head, m, sizeof m - 1);
	for (size_t i = 0; i < name_len; ++i) {
		if (name[i] == '"' || name[i] == '\\')
			*p++ = '\\';
		*p++ = name[i];
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
	memcpy(s.ptr + h, text, len);
	s.ptr[h + len] = '\n';
	buf_commit(in, h + len + 1);
	return 0;
}

int
notes_add_input (struct buf      *in,
                 enum notes_side  side,
                 char const      *tip,
                 struct span      file)
{
	static char const *const messages[] = {
		[NOTES_UPSTREAM] = "The upstream added a note.",
		[NOTES_CLIENT]   = "A client added a note.",
	};
	char head[256];
	int h = snprintf(head, sizeof head, "commit " BRANCH "\ncommitter askai <askai> "
	                 "now\ndata <<END\n%s\nEND\n%s%s%s", messages[side],
	                 *tip ? "from " : "", tip, *tip ? "\n" : "");
	if (h < 0 || h >= (int)sizeof head)
		return EINVAL;
	struct span s;
	int e = buf_reserve(in, (size_t)h + file.len, SIZE_MAX, &s);
	if (e)
		return e;
	memcpy(s.ptr, head, (size_t)h);
	memcpy(s.ptr + h, file.ptr, file.len);
	buf_commit(in, (size_t)h + file.len);
	return 0;
}

int
notes_read_input (struct buf *in,
                  char const *name,
                  size_t      len)
{
	static char const at[] = BRANCH ":";
	struct span s;
	int e = buf_reserve(in, sizeof at + len, SIZE_MAX, &s);
	if (e)
		return e;
	char *p = mempcpy(s.ptr, at, sizeof at - 1);
	p = mempcpy(p, name, len);
	*p = '\n';
	buf_commit(in, sizeof at + len);
	return 0;
}

int
notes_read_out (struct span  out,
                struct span *text)
{
	*text = (struct span){};
	char const *line = (char const *)out.ptr;
	char const *nl = memchr(line, '\n', out.len);
	if (!nl)
		return EIO;
	size_t n = (size_t)(nl - line);
	size_t oid = 0;
	enum notes_kind kind = notes_kind(line, n, &oid);
	if (kind == KIND_MISSING)
		return ENOENT;
	if (kind == KIND_TREE)
		return EISDIR;
	if (kind != KIND_BLOB)
		return EIO;
	/* The size of the text is the last word of the line.  The loop stops
	 * growing the size once it is past NOTES_MAX, and the end of the output
	 * limits the text. */
	uint64_t size = 0;
	for (char const *d = (char const *)memrchr(line, ' ', n) + 1; d < nl; ++d) {
		if (*d < '0' || *d > '9')
			return EIO;
		if (size <= NOTES_MAX)
			size = size * 10 + (uint64_t)(*d - '0');
	}
	size_t room = out.len - n - 1;
	*text = (struct span){ out.ptr + n + 1, size < room ? (size_t)size : room };
	return 0;
}

/* Returns the offset of the start of the token that ends with the NUL at
 * the offset @p nul in @p s. */
static size_t
notes_token (char const *s,
             size_t      nul)
{
	char const *prev = memrchr(s, '\0', nul);
	return prev ? (size_t)(prev - s) + 1 : 0;
}

bool
notes_entry_before (struct span         out,
                    size_t             *end,
                    struct notes_entry *entry)
{
	char const *s = (char const *)out.ptr;
	size_t at = *end;
	/* Each note is two tokens that end with a NUL: the time, then a newline
	 * and "LINES\tREMOVED\tNAME".  A cut output ends inside a token. */
	if (at && s[at - 1]) {
		char const *nul = memrchr(s, '\0', at);
		at = nul ? (size_t)(nul - s) + 1 : 0;
	}
	while (at) {
		size_t stat = notes_token(s, at - 1);
		size_t time = stat ? notes_token(s, stat - 1) : 0;
		char const *tab = s[stat] == '\n' ? memchr(s + stat, '\t', at - 1 - stat)
		                                  : nullptr;
		char const *name = tab ? memchr(tab + 1, '\t', (size_t)(s + at - 1 - tab - 1))
		                       : nullptr;
		/* The loop skips a token that does not describe a file, and a file
		 * without a time before it. */
		if (!name || !stat) {
			at = stat;
			continue;
		}
		*entry = (struct notes_entry){
			.name = { (unsigned char *)(uintptr_t)(name + 1),
			          (size_t)(s + at - 1 - name - 1) },
		};
		for (char const *d = s + stat + 1; d < tab && *d >= '0' && *d <= '9'; ++d)
			entry->lines = entry->lines * 10 + (uint64_t)(*d - '0');
		for (char const *d = s + time; d < s + stat - 1 && *d >= '0' && *d <= '9'; ++d)
			entry->time = entry->time * 10 + (uint64_t)(*d - '0');
		*end = time;
		return true;
	}
	*end = 0;
	return false;
}
