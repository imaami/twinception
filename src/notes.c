/** @file The notes (notes.h). */
#define _GNU_SOURCE /* pipe2() */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
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

/* Run git with @p argv, its output, cut at @p len - 1 bytes, in @p out,
 * NUL-terminated, and wait for it; true if it exited 0.  If it could not be
 * run, @p out says why. */
static bool
run (struct notes const *notes,
     char const         *argv[],
     char               *out,
     size_t              len)
{
	int fds[2];
	if (pipe2(fds, O_CLOEXEC))
		return false;
	posix_spawn_file_actions_t fa;
	pid_t pid = 0;
	int e = posix_spawn_file_actions_init(&fa);
	if (!e) {
		e = posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
		if (!e)
			e = posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
		if (!e)
			e = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null",
			                                     O_RDONLY, 0);
		if (!e)
			e = posix_spawnp(&pid, "git", &fa, nullptr, (char *const *)argv,
			                 notes->env);
		posix_spawn_file_actions_destroy(&fa);
	}
	close(fds[1]);
	size_t n = 0;
	for (ssize_t r; !e && (r = read(fds[0], out + n, len - 1 - n)) != 0; ) {
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			break;
		n += (size_t)r;
		if (n == len - 1) {
			/* Drain the rest, so that git is not stopped writing. */
			char sink[256];
			while ((r = read(fds[0], sink, sizeof sink)) > 0 ||
			       (r < 0 && errno == EINTR))
				;
			break;
		}
	}
	out[n] = '\0';
	close(fds[0]);
	int status = -1;
	while (!e && waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (e)
		snprintf(out, len, "cannot run git: %s", strerror(e));
	return !e && status != -1 && WIFEXITED(status) && !WEXITSTATUS(status);
}

/* Tell in @p why, of @p len bytes, @p what failed, and git's last line of
 * @p out if any; errno EINVAL. */
static void
failed (char       *why,
        size_t      len,
        char const *what,
        char       *out)
{
	size_t n = strlen(out);
	while (n && (out[n - 1] == '\n' || out[n - 1] == ' '))
		out[--n] = '\0';
	char const *line = strrchr(out, '\n');
	line = line ? line + 1 : out;
	snprintf(why, len, "%s%s%s", what, *line ? ": " : "", line);
	errno = EINVAL;
}

struct notes *
notes_new (char const *path,
           char       *why,
           size_t      len)
{
	static char const env_path[] = "PATH=";
	static char const dir[] = "--git-dir=";
	char const *search = getenv("PATH");
	search = search && *search ? search : "/usr/bin:/bin";
	struct notes *notes = calloc(1, sizeof *notes);
	size_t size = sizeof env_path + strlen(search);
	char *env = notes ? malloc(size) : nullptr;
	if (!env) {
		free(notes);
		snprintf(why, len, "out of memory");
		errno = ENOMEM;
		return nullptr;
	}
	snprintf(env, size, "%s%s", env_path, search);
	char *const vars[ENV] = {
		env, "HOME=/nonexistent", "LC_ALL=C.UTF-8", "GIT_CONFIG_NOSYSTEM=1",
		"GIT_CONFIG_GLOBAL=/dev/null", "GIT_TERMINAL_PROMPT=0",
		"GIT_NO_LAZY_FETCH=1", "GIT_ALLOW_PROTOCOL=none", "GIT_PAGER=cat",
		nullptr
	};
	static_assert(sizeof vars == sizeof notes->env);
	memcpy(notes->env, vars, sizeof vars);

	/* Made bare, with no hooks' samples, if nothing is there. */
	char out[512];
	struct stat st;
	if (stat(path, &st) && errno == ENOENT) {
		char const *argv[] = {
			"git", "init", "--bare", "--quiet", "--template=", "--", path, nullptr
		};
		if (!run(notes, argv, out, sizeof out)) {
			failed(why, len, "cannot make the repository", out);
			notes_free(&notes);
			return nullptr;
		}
	}
	char real[PATH_MAX];
	if (!realpath(path, real)) {
		int e = errno;
		snprintf(why, len, "%s", strerror(e));
		notes_free(&notes);
		errno = e;
		return nullptr;
	}
	size = sizeof dir + strlen(real);
	if (!(notes->dir = malloc(size))) {
		snprintf(why, len, "out of memory");
		notes_free(&notes);
		errno = ENOMEM;
		return nullptr;
	}
	snprintf(notes->dir, size, "%s%s", dir, real);
	char const *argv[] = {
		"git", notes->dir, "rev-parse", "--is-bare-repository", nullptr
	};
	bool ok = run(notes, argv, out, sizeof out);
	if (!ok || strcmp(out, "true\n")) {
		if (ok)
			*out = '\0';
		failed(why, len, ok ? "not a bare git repository" : "not a git repository",
		       out);
		notes_free(&notes);
		return nullptr;
	}
	return notes;
}

void
notes_free (struct notes **p_notes)
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

bool
notes_id (char id[NOTES_ID + 1])
{
	static char const hex[] = "0123456789abcdef";
	unsigned char rnd[NOTES_ID / 2];
	/* Never short for so few bytes, but for a signal. */
	ssize_t got;
	while ((got = getrandom(rnd, sizeof rnd, 0)) < 0 && errno == EINTR)
		;
	if (got != (ssize_t)sizeof rnd)
		return false;
	for (size_t i = 0; i < sizeof rnd; ++i) {
		id[2 * i] = hex[rnd[i] >> 4];
		id[2 * i + 1] = hex[rnd[i] & 15];
	}
	id[NOTES_ID] = '\0';
	return true;
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
	static_assert(NOTES_REF > sizeof "refs/heads//" + 16 + NOTES_ID);
	if (principal)
		snprintf(ref, NOTES_REF, "refs/heads/%016" PRIx64 "/%.*s", principal,
		         NOTES_ID, id);
	else
		snprintf(ref, NOTES_REF, "refs/heads/anonymous/%.*s", NOTES_ID, id);
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

bool
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
	int t = snprintf(tail, sizeof tail, "\n%s%s%s\n", after ? "from " : "",
	                 after ? ref : "", after ? "^0\n" : "");
	size_t n = (size_t)h + len + (size_t)t;
	struct span s;
	if (buf_reserve(in, n, SIZE_MAX, &s))
		return false;
	memcpy(s.ptr, head, (size_t)h);
	memcpy(s.ptr + h, text, len);
	memcpy(s.ptr + (size_t)h + len, tail, (size_t)t);
	buf_commit(in, n);
	return true;
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
