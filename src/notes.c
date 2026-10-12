/** @file The notes (notes.h). */
#define _GNU_SOURCE /* memrchr(), pipe2() */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "notes.h"
#include "util.h"

/* The environment's variables, its nullptr included. */
enum { ENV = 10 };

struct notes {
	char *env[ENV];
	char *dir;    /* "--git-dir=" and the repository's real path */
};

/* Settings that stop what the repository's own configuration, or its hooks,
 * which fast-import's ref update would run, could have git run. */
static char const *const fixed[] = {
	"--no-pager", "--no-replace-objects", "-c", "core.hooksPath=/dev/null", "-c",
	"log.showSignature=false",
};

/* Spawn git with @p argv, its output and error the pipe end @p fd, its
 * input /dev/null, into @p pid. */
static int
notes_spawn (struct notes const *notes,
             char const         *argv[],
             int                 fd,
             pid_t              *pid)
{
	posix_spawn_file_actions_t fa;
	int e = posix_spawn_file_actions_init(&fa);
	if (e)
		return e;
	if (!(e = posix_spawn_file_actions_adddup2(&fa, fd, STDOUT_FILENO)) &&
	    !(e = posix_spawn_file_actions_adddup2(&fa, fd, STDERR_FILENO)) &&
	    !(e = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null",
	                                           O_RDONLY, 0)))
		/* POSIX declares the vectors without const. */
		e = posix_spawnp(pid, "git", &fa, nullptr, (char *const *)argv, notes->env);
	posix_spawn_file_actions_destroy(&fa);
	return e;
}

/* Read @p fd to its end into @p out, keeping @p len - 1 bytes at most, and
 * the rest drained, so that git is not stopped writing.  Returns the bytes
 * kept. */
static size_t
notes_drain (int     fd,
             char   *out,
             size_t  len)
{
	size_t n = 0;
	for (;;) {
		char sink[256];
		bool full = n == len - 1;
		ssize_t r = full ? read(fd, sink, sizeof sink) : read(fd, out + n, len - 1 - n);
		if (r > 0)
			n += full ? 0 : (size_t)r;
		else if (!r || errno != EINTR)
			return n;
	}
}

/* Run git with @p argv, its output, cut at @p len - 1 bytes, in @p out,
 * NUL-terminated, and wait for it.  Returns 0 if it exited 0; EIO if it
 * failed otherwise; or the error that kept it from running, which @p out
 * then says. */
static int
notes_run (struct notes const *notes,
           char const         *argv[],
           char               *out,
           size_t              len)
{
	int fds[2];
	if (pipe2(fds, O_CLOEXEC))
		return errno;
	pid_t pid = 0;
	int e = notes_spawn(notes, argv, fds[1], &pid);
	close(fds[1]);
	size_t n = e ? 0 : notes_drain(fds[0], out, len);
	out[n] = '\0';
	close(fds[0]);
	if (e) {
		util_format(out, len, "cannot run git: %s", strerror(e));
		return e;
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return errno;
	return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : EIO;
}

/* Say in @p why, of @p len bytes, that @p what failed, and git's last line
 * of @p out if any; returns EINVAL. */
static int
notes_fail (char       *why,
            size_t      len,
            char const *what,
            char       *out)
{
	size_t n = strlen(out);
	while (n && (out[n - 1] == '\n' || out[n - 1] == ' '))
		out[--n] = '\0';
	char const *line = memrchr(out, '\n', n);
	line = line ? line + 1 : out;
	if (*line)
		util_format(why, len, "%s: %s", what, line);
	else
		util_format(why, len, "%s", what);
	return EINVAL;
}

/* Make the bare repository at @p path, with no hooks' samples, if nothing is
 * there, and find its real path. */
static int
notes_open (struct notes *notes,
            char const   *path,
            char         *why,
            size_t        len)
{
	static char const dir[] = "--git-dir=";
	char out[512];
	struct stat st;
	if (stat(path, &st) && errno == ENOENT) {
		char const *argv[] = {
			"git", "init", "--bare", "--quiet", "--template=", "--", path, nullptr
		};
		if (notes_run(notes, argv, out, sizeof out))
			return notes_fail(why, len, "cannot make the repository", out);
	}
	char *real = realpath(path, nullptr);
	if (!real) {
		int e = errno;
		util_format(why, len, "%s", strerror(e));
		return e;
	}
	notes->dir = util_concat(dir, sizeof dir - 1, real);
	free(real);
	if (!notes->dir) {
		util_format(why, len, "out of memory");
		return ENOMEM;
	}
	char const *argv[] = {
		"git", notes->dir, "rev-parse", "--is-bare-repository", nullptr
	};
	if (notes_run(notes, argv, out, sizeof out))
		return notes_fail(why, len, "not a git repository", out);
	if (!strcmp(out, "true\n"))
		return 0;
	util_format(why, len, "not a bare git repository");
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
	struct notes *notes = *p_notes;
	if (!notes)
		return;
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
	/* Never short for so few bytes, but for a signal. */
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
notes_ref (uint64_t    principal,
           char const *id,
           char        ref[NOTES_REF])
{
	static char const heads[] = "refs/heads/";
	static char const anonymous[] = "anonymous/";
	static char const digits[] = "0123456789abcdef";
	static_assert(NOTES_REF > sizeof heads - 1 + 16 + 1 + NOTES_ID);
	memcpy(ref, heads, sizeof heads - 1);
	char *p = ref + sizeof heads - 1;
	if (principal) {
		for (unsigned i = 16; i--; principal >>= 4)
			p[i] = digits[principal & 15];
		p[16] = '/';
		p += 17;
	} else {
		memcpy(p, anonymous, sizeof anonymous - 1);
		p += sizeof anonymous - 1;
	}
	memcpy(p, id, NOTES_ID);
	p[NOTES_ID] = '\0';
}

void
notes_write (struct notes const *notes,
             char const         *argv[NOTES_ARGV])
{
	size_t k = 0;
	argv[k++] = "git";
	argv[k++] = notes->dir;
	argv[k++] = "-c";
	argv[k++] = "core.hooksPath=/dev/null";
	argv[k++] = "fast-import";
	argv[k++] = "--quiet";
	argv[k++] = "--date-format=now";
	argv[k] = nullptr;
}

int
notes_input (struct buf *in,
             char const *ref,
             char const *text,
             size_t      len,
             bool        after)
{
	char head[192];
	char tail[NOTES_REF + 16];
	int h = snprintf(head, sizeof head, "commit %s\ncommitter askai <askai> now\n"
	                 "data %zu\n", ref, len);
	int t = after ? snprintf(tail, sizeof tail, "\nfrom %s^0\n\n", ref)
	              : snprintf(tail, sizeof tail, "\n\n");
	if (h < 0 || h >= (int)sizeof head || t < 0 || t >= (int)sizeof tail)
		return EINVAL;
	size_t n = (size_t)h + len + (size_t)t;
	struct span s;
	int e = buf_reserve(in, n, SIZE_MAX, &s);
	if (e)
		return e;
	memcpy(s.ptr, head, (size_t)h);
	memcpy(s.ptr + h, text, len);
	memcpy(s.ptr + (size_t)h + len, tail, (size_t)t);
	buf_commit(in, n);
	return 0;
}

void
notes_read (struct notes const *notes,
            char const         *ref,
            char const         *search,
            char const         *argv[NOTES_ARGV])
{
	static_assert(NOTES_READ == 256, "the argument says");
	size_t k = 0;
	argv[k++] = "git";
	argv[k++] = notes->dir;
	for (size_t i = 0; i < ARRAY_SIZE(fixed); ++i)
		argv[k++] = fixed[i];
	argv[k++] = "log";
	argv[k++] = "-z";
	argv[k++] = "--format=%B";
	argv[k++] = "--max-count=256";
	if (search) {
		argv[k++] = "--fixed-strings";
		argv[k++] = "--regexp-ignore-case";
		argv[k++] = "--grep";
		argv[k++] = search;
	}
	argv[k++] = ref;
	argv[k++] = "--";
	argv[k] = nullptr;
}
