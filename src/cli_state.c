/** @file Atomic, counted snapshots; never stores provider reasoning. */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "child.h"
#include "cli_state.h"
#include "cstr_view.h"
#include "dstr.h"
#include "jb.h"
#include "util.h"

enum {
	CLI_STATE_INPUTS = 256,
	CLI_STATE_INPUT_BYTES = 1 << 20,
	CLI_STATE_TURNS = 512,
	CLI_STATE_TURN_BYTES = 16 << 20,
	CLI_STATE_ID_BYTES = 64 << 10,
};

struct cli_state_pair {
	struct dstr prompt;
	struct dstr answer;
};

struct cli_state {
	_Atomic bool const   *cancel;
	struct dstr           inputs[CLI_STATE_INPUTS];
	struct cli_state_pair turns[CLI_STATE_TURNS];
	struct cli_state_pair pending;
	struct dstr           identity;
	struct dstr           path;
	struct dstr           name;
	struct dstr           temporary;
	struct dstr           branch;
	struct dstr           latest;
	struct dstr           parent;
	struct dstr           output;
	struct dstr           tree;
	struct dstr           metadata;
	char                **environment;
	struct dstr           scratch;
	size_t                input_first;
	size_t                input_count;
	size_t                input_bytes;
	size_t                turn_count;
	size_t                turn_bytes;
	int                   directory;
	int                   lock;
	bool                  snapshot_pending;
};

static char const cli_state_magic[] = "ASKAI-CLI-1\n";

extern char **environ;

static int
cli_state_copy (struct dstr      *dest,
                struct cstr_view  text)
{
	(void)dstr_clear(dest);
	return dstr_cat_n(dest, text.data, text.len) ? 0 : ENOMEM;
}

static bool
cli_state_valid (struct cstr_view text)
{
	return !text.len ||
	       (text.data && !memchr(text.data, '\0', text.len)
	        && jb_utf8(text.data, text.len) == text.len);
}

static int
cli_state_private (int fd)
{
	struct stat status;
	if (fstat(fd, &status))
		return errno;
	if (!S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
	    (status.st_mode & 0077) || status.st_nlink != 1)
		return EACCES;
	return 0;
}

/* A short read is a snapshot cut short. */
static int
cli_state_read (int     fd,
                void   *bytes,
                size_t  len)
{
	size_t got;
	int e = util_read_all(fd, bytes, len, -1, &got);
	return e ? e : got < len ? EINVAL : 0;
}

static int
cli_state_write_number (int    fd,
                        size_t value)
{
	unsigned char bytes[] = {
		(unsigned char)(value >> 24),
		(unsigned char)(value >> 16),
		(unsigned char)(value >>  8),
		(unsigned char) value
	};
	return util_write_all(fd, bytes, sizeof bytes);
}

static int
cli_state_read_number (int     fd,
                       size_t *value)
{
	unsigned char bytes[4];
	int e = cli_state_read(fd, bytes, sizeof bytes);
	if (!e)
		*value = (size_t)bytes[0] << 24U
		       | (size_t)bytes[1] << 16U
		       | (size_t)bytes[2] <<  8U
		       |         bytes[3];
	return e;
}

static int
cli_state_write_text (int                fd,
                      struct dstr const *text)
{
	size_t n = dstr_len(text);
	int e = cli_state_write_number(fd, n);
	return e ? e : util_write_all(fd, dstr_get(text), n);
}

static int
cli_state_read_text (int          fd,
                     struct dstr *text,
                     size_t      *budget)
{
	size_t len = 0;
	int e = cli_state_read_number(fd, &len);
	if (e)
		return e;
	if (len > *budget)
		return EFBIG;
	*budget -= len;
	(void)dstr_clear(text);
	if (!dstr_reserve(text, len))
		return ENOMEM;
	char buffer[8192];
	while (len) {
		size_t count = len < sizeof buffer
		             ? len : sizeof buffer;
		e = cli_state_read(fd, buffer, count);
		if (e)
			return e;
		if (!dstr_cat_n(text, buffer, count))
			return ENOMEM;
		len -= count;
	}
	return jb_utf8(dstr_get(text), dstr_len(text)) == dstr_len(text) ? 0 : EILSEQ;
}

/** flock has no readiness notification. Retry only this contended lock,
 * on a blocker, with a bounded cancellation delay; no UI tick is involved. */
static int
cli_state_lock (struct cli_state *state,
                int               fd)
{
	for (;;) {
		if (state->cancel && atomic_load(state->cancel))
			return EINTR;
		if (!flock(fd, LOCK_EX | LOCK_NB))
			return 0;
		if (errno == EINTR)
			continue;
		if (errno != EWOULDBLOCK)
			return errno;
		struct timespec delay = {.tv_nsec = 20'000'000};
		int e = clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, nullptr);
		if (e && e != EINTR)
			return e;
	}
}

/** Run Git directly, with bounded captured output and no shell. */
static int
cli_state_git (struct cli_state  *state,
               char const *const  arguments[],
               struct cstr_view   input,
               int                output_fd,
               int               *status)
{
	char const *argv[32] = { "git", "-C", dstr_get(&state->path),
	                         "-c", "user.name=AskAI CLI", "-c",
	                         "user.email=askai-cli@localhost", "-c",
	                         "core.fsync=committed" };
	size_t count = 9;
	for (size_t i = 0; arguments[i]; ++i) {
		if (count == ARRAY_SIZE(argv) - 1)
			return E2BIG;
		argv[count++] = arguments[i];
	}
	int out = output_fd >= 0 ? output_fd : memfd_create("askai-cli-git", MFD_CLOEXEC);
	if (out < 0)
		return errno;
	struct child_cmd const cmd = {
		.file = "git",
		.argv = argv,
		.envp = state->environment,
		.in   = { (unsigned char *)(uintptr_t)input.data, input.len },
	};
	int e = child_run(&cmd, (struct child_fds){ -1, out, -1, -1 }, status);
	(void)dstr_clear(&state->output);
	if (output_fd >= 0)
		return e;
	char buffer[4097];
	size_t got = 0;
	if (!e)
		e = util_read_all(out, buffer, sizeof buffer, 0, &got);
	if (!e && got == sizeof buffer)
		e = EFBIG;
	if (!e && !dstr_cat_n(&state->output, buffer, got))
		e = ENOMEM;
	close(out);
	return e;
}

static int
cli_state_git_ok (struct cli_state *state, char const *const arguments[],
                  struct cstr_view input)
{
	int status = 0;
	int e = cli_state_git(state, arguments, input, -1, &status);
	return e ? e : status ? EIO : 0;
}

static int
cli_state_oid (struct cli_state *state, char oid[65], size_t *len)
{
	struct cstr_view output = cstr_view_dstr(&state->output);
	if (output.len && output.data[output.len - 1] == '\n')
		--output.len;
	if (!util_is_oid(output.data, output.len))
		return EINVAL;
	memcpy(oid, output.data, output.len);
	oid[output.len] = '\0';
	*len = output.len;
	return 0;
}

static int
cli_state_tree_blob (struct cli_state *state, struct cstr_view name,
                     struct cstr_view text)
{
	int e = cli_state_git_ok(state, (char const *[]){ "hash-object", "-w", "--stdin", nullptr }, text);
	char oid[65];
	size_t len = 0;
	if (!e)
		e = cli_state_oid(state, oid, &len);
	if (!e && (!dstr_cat_n(&state->tree, "100644 blob ", 12) ||
	           !dstr_cat_n(&state->tree, oid, len) ||
	           !dstr_cat_n(&state->tree, "\t", 1) ||
	           !dstr_cat_n(&state->tree, name.data, name.len) ||
	           !dstr_cat_n(&state->tree, "\n", 1)))
		e = ENOMEM;
	return e;
}

static int
cli_state_commit (struct cli_state *state)
{
	char blob[65], tree[65], commit[65];
	size_t blob_len = 0, tree_len = 0, commit_len = 0;
	int e = cli_state_git_ok(state, (char const *[]){ "hash-object", "-w", "--", dstr_get(&state->name), nullptr },
	                         (struct cstr_view){});
	if (!e)
		e = cli_state_oid(state, blob, &blob_len);
	(void)dstr_clear(&state->tree);
	if (!e && (!dstr_cat_n(&state->tree, "100644 blob ", 12) ||
	           !dstr_cat_n(&state->tree, blob, blob_len) ||
	           !dstr_cat_n(&state->tree, "\tsnapshot\n", 10)))
		e = ENOMEM;
	if (!e)
		e = cli_state_tree_blob(state, CSTR_VIEW("identity.txt"), cstr_view_dstr(&state->metadata));
	if (!e && state->input_count)
		e = cli_state_tree_blob(state, CSTR_VIEW("input.txt"),
		                        cli_state_input_at(state, state->input_count - 1));
	if (!e && state->turn_count)
		e = cli_state_tree_blob(state, CSTR_VIEW("prompt.txt"),
		                        cstr_view_dstr(&state->turns[state->turn_count - 1].prompt));
	if (!e && state->turn_count)
		e = cli_state_tree_blob(state, CSTR_VIEW("answer.txt"),
		                        cstr_view_dstr(&state->turns[state->turn_count - 1].answer));
	if (!e)
		e = cli_state_git_ok(state, (char const *[]){ "mktree", nullptr }, cstr_view_dstr(&state->tree));

	if (!e)
		e = cli_state_oid(state, tree, &tree_len);
	static char const message[] = "Save CLI input history and completed conversation\n";
	char const *arguments[] = { "commit-tree", tree, "-F", "-", "-p", dstr_get(&state->parent), nullptr };
	if (!dstr_len(&state->parent))
		arguments[4] = nullptr;
	if (!e)
		e = cli_state_git_ok(state, arguments, (struct cstr_view){ message, sizeof message - 1 });
	if (!e)
		e = cli_state_oid(state, commit, &commit_len);
	(void)dstr_clear(&state->scratch);
	if (!e && (!dstr_cat_n(&state->scratch, "start\nupdate ", 13) ||
	           !dstr_cat_n(&state->scratch, dstr_get(&state->branch), dstr_len(&state->branch)) ||
	           !dstr_cat_n(&state->scratch, " ", 1) ||
	           !dstr_cat_n(&state->scratch, commit, commit_len) ||
	           !dstr_cat_n(&state->scratch, "\nupdate ", 8) ||
	           !dstr_cat_n(&state->scratch, dstr_get(&state->latest), dstr_len(&state->latest)) ||
	           !dstr_cat_n(&state->scratch, " ", 1) ||
	           !dstr_cat_n(&state->scratch, commit, commit_len) ||
	           !dstr_cat_n(&state->scratch, "\nprepare\ncommit\n", 16)))
		e = ENOMEM;
	if (!e) {
		e = cli_state_lock(state, state->lock);
		if (!e) {
			e = cli_state_git_ok(state, (char const *[]){ "update-ref", "--stdin", nullptr }, cstr_view_dstr(&state->scratch));
			if (flock(state->lock, LOCK_UN) && !e)
				e = errno;
		}
	}
	if (!e)
		e = cli_state_copy(&state->parent, (struct cstr_view){ commit, commit_len });
	if (!e)
		(void)cli_state_git_ok(state, (char const *[]){ "gc", "--auto", "--quiet", nullptr },
		                       (struct cstr_view){});
	return e;
}

static int
cli_state_save (struct cli_state *state)
{
	int fd = openat(state->directory, dstr_get(&state->temporary),
	                O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return errno;
	int e = cli_state_private(fd);
	if (!e && ftruncate(fd, 0))
		e = errno;
	if (!e)
		e = util_write_all(fd, cli_state_magic, sizeof cli_state_magic - 1);
	if (!e)
		e = cli_state_write_text(fd, &state->identity);
	if (!e)
		e = cli_state_write_number(fd, state->input_count);
	if (!e)
		e = cli_state_write_number(fd, state->turn_count);
	for (size_t i = 0; !e && i < state->input_count; ++i)
		e = cli_state_write_text(fd, &state->inputs[(state->input_first + i) % CLI_STATE_INPUTS]);
	for (size_t i = 0; !e && i < state->turn_count; ++i) {
		e = cli_state_write_text(fd, &state->turns[i].prompt);
		if (!e)
			e = cli_state_write_text(fd, &state->turns[i].answer);
	}
	if (!e && fsync(fd))
		e = errno;
	if (close(fd) && !e)
		e = errno;
	fd = -1;
	if (!e) {
		if (renameat(state->directory, dstr_get(&state->temporary),
		             state->directory, dstr_get(&state->name)))
			e = errno;
		else
			state->snapshot_pending = true;
	}
	if (!e && fsync(state->directory))
		e = errno;
	if (!e) {
		state->snapshot_pending = true;
		e = cli_state_commit(state);
	}
	if (!e) {
		state->snapshot_pending = false;
		if (unlinkat(state->directory, dstr_get(&state->name), 0))
			e = errno;
	}
	return e;
}

static int
cli_state_load (struct cli_state *state, int fd)
{
	char magic[sizeof cli_state_magic - 1];
	int e = cli_state_private(fd);
	if (!e)
		e = cli_state_read(fd, magic, sizeof magic);
	if (e)
		return e;
	if (memcmp(magic, cli_state_magic, sizeof magic))
		return EINVAL;
	size_t budget = CLI_STATE_ID_BYTES;
	e = cli_state_read_text(fd, &state->scratch, &budget);
	if (e)
		return e;
	if (dstr_len(&state->identity) != dstr_len(&state->scratch) ||
	    memcmp(dstr_get(&state->identity), dstr_get(&state->scratch), dstr_len(&state->identity)))
		return EXDEV;
	e = cli_state_read_number(fd, &state->input_count);
	if (!e)
		e = cli_state_read_number(fd, &state->turn_count);
	if (e)
		return e;
	if (state->input_count > CLI_STATE_INPUTS || state->turn_count > CLI_STATE_TURNS)
		return EFBIG;
	budget = CLI_STATE_INPUT_BYTES;
	for (size_t i = 0; !e && i < state->input_count; ++i) {
		e = cli_state_read_text(fd, &state->inputs[i], &budget);
		if (!e && !cli_state_valid(cstr_view_dstr(&state->inputs[i])))
			e = EILSEQ;
	}
	state->input_bytes = CLI_STATE_INPUT_BYTES - budget;
	budget = CLI_STATE_TURN_BYTES;
	for (size_t i = 0; !e && i < state->turn_count; ++i) {
		e = cli_state_read_text(fd, &state->turns[i].prompt, &budget);
		if (!e)
			e = cli_state_read_text(fd, &state->turns[i].answer, &budget);
	}
	state->turn_bytes = CLI_STATE_TURN_BYTES - budget;
	if (e)
		return e;
	char trailing;
	ssize_t count;
	do { count = read(fd, &trailing, 1); } while (count < 0 && errno == EINTR);
	return count < 0 ? errno : count ? EINVAL : 0;
}

static int
cli_state_default_path (struct cli_state *state)
{
	char const *base = getenv("XDG_STATE_HOME");
	if (base && *base == '/') {
		if (!dstr_cat(&state->path, base))
			return ENOMEM;
	} else {
		base = getenv("HOME");
		if (!base || *base != '/')
			return ENOENT;
		if (!dstr_cat(&state->path, base) || !dstr_cat_n(&state->path, "/.local/state", sizeof "/.local/state" - 1))
			return ENOMEM;
	}
	if (!dstr_cat_n(&state->path, "/askai/cli.git", sizeof "/askai/cli.git" - 1))
		return ENOMEM;
	/* Only the private leaf holds data. Existing parent modes are preserved. */
	size_t len = dstr_len(&state->path);
	for (size_t i = 1; i <= len; ++i) {
		if (i < len && dstr_get(&state->path)[i] != '/')
			continue;
		int e = cli_state_copy(&state->scratch, (struct cstr_view){ dstr_get(&state->path), i });
		if (e)
			return e;
		if (mkdir(dstr_get(&state->scratch), 0700) && errno != EEXIST)
			return errno;
	}
	return 0;
}

static int
cli_state_push_input (struct cli_state *state, struct cstr_view text)
{
	int e = cli_state_copy(&state->scratch, text);
	if (e)
		return e;
	while (state->input_count && (state->input_count == CLI_STATE_INPUTS ||
	       text.len > CLI_STATE_INPUT_BYTES - state->input_bytes)) {
		state->input_bytes -= dstr_len(&state->inputs[state->input_first]);
		state->input_first = (state->input_first + 1) % CLI_STATE_INPUTS;
		--state->input_count;
	}
	size_t index = (state->input_first + state->input_count) % CLI_STATE_INPUTS;
	struct dstr previous = state->inputs[index];
	state->inputs[index] = state->scratch;
	state->scratch = previous;
	++state->input_count;
	state->input_bytes += text.len;
	return 0;
}

/** Merge recall only. Completed conversations from different writers never mix. */
static int
cli_state_merge_inputs (struct cli_state *state)
{
	int e = cli_state_copy(&state->scratch, (struct cstr_view){ dstr_get(&state->latest), dstr_len(&state->latest) - 6 });
	if (!e)
		e = cli_state_git_ok(state, (char const *[]){ "for-each-ref", "--sort=-committerdate", "--count=32",
		                         "--format=%(objectname)", dstr_get(&state->scratch), nullptr },
		                         (struct cstr_view){});
	(void)dstr_clear(&state->tree);
	if (!e && (!dstr_cat_n(&state->tree, dstr_get(&state->parent), dstr_len(&state->parent)) ||
	           !dstr_cat_n(&state->tree, "\n", 1)))
		e = ENOMEM;
	struct cstr_view revisions = cstr_view_dstr(&state->output);
	for (size_t start = 0; !e && start < revisions.len;) {
		size_t end = start;
		while (end < revisions.len && revisions.data[end] != '\n')
			++end;
		bool latest = end - start == dstr_len(&state->parent) &&
		              !memcmp(revisions.data + start, dstr_get(&state->parent), end - start);
		if (!latest && (!dstr_cat_n(&state->tree, revisions.data + start, end - start) ||
		                !dstr_cat_n(&state->tree, "\n", 1)))
			e = ENOMEM;
		start = end + 1;
	}
	if (e)
		return e;
	struct dstr entries[CLI_STATE_INPUTS];
	for (size_t i = 0; i < CLI_STATE_INPUTS; ++i)
		entries[i] = dstr("");
	int fd = openat(state->directory, dstr_get(&state->temporary),
	                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return errno;
	state->input_first = state->input_count = state->input_bytes = 0;
	/* Git lists newest tips first. Process the selected latest snapshot last
	 * even when Git's second-resolution commit times tie. Repeated exact
	 * inputs move to the newest position, preserving the expected Up entry. */
	size_t end = dstr_len(&state->tree);
	while (!e && end) {
		if (dstr_get(&state->tree)[end - 1] == '\n')
			--end;
		size_t start = end;
		while (start && dstr_get(&state->tree)[start - 1] != '\n')
			--start;
		e = cli_state_copy(&state->scratch, (struct cstr_view){ dstr_get(&state->tree) + start, end - start });
		if (!e && !dstr_cat_n(&state->scratch, ":snapshot", 9))
			e = ENOMEM;
		if (!e && (ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET) < 0))
			e = errno;
		int status = 0;
		if (!e)
			e = cli_state_git(state, (char const *[]){ "cat-file", "blob", dstr_get(&state->scratch), nullptr },
			                  (struct cstr_view){}, fd, &status);
		if (!e && status)
			e = EIO;
		if (!e && lseek(fd, 0, SEEK_SET) < 0)
			e = errno;
		char magic[sizeof cli_state_magic - 1];
		if (!e)
			e = cli_state_read(fd, magic, sizeof magic);
		if (!e && memcmp(magic, cli_state_magic, sizeof magic))
			e = EINVAL;
		size_t budget = CLI_STATE_ID_BYTES;
		if (!e)
			e = cli_state_read_text(fd, &state->scratch, &budget);
		if (!e && (dstr_len(&state->scratch) != dstr_len(&state->identity) ||
		    memcmp(dstr_get(&state->scratch), dstr_get(&state->identity), dstr_len(&state->identity))))
			e = EXDEV;
		size_t count = 0, turns = 0;
		if (!e)
			e = cli_state_read_number(fd, &count);
		if (!e)
			e = cli_state_read_number(fd, &turns);
		if (!e && (count > CLI_STATE_INPUTS || turns > CLI_STATE_TURNS))
			e = EFBIG;
		budget = CLI_STATE_INPUT_BYTES;
		for (size_t i = 0; !e && i < count; ++i) {
			e = cli_state_read_text(fd, &entries[i], &budget);
			struct cstr_view text = cstr_view_dstr(&entries[i]);
			if (!e && !cli_state_valid(text))
				e = EILSEQ;
			bool present = false;
			for (size_t j = 0; !e && !present && j < state->input_count; ++j) {
				struct cstr_view old = cli_state_input_at(state, j);
				present = cstr_view_equal(old, text);
				if (present) {
					struct dstr entry = state->inputs[(state->input_first + j) % CLI_STATE_INPUTS];
					for (size_t k = j; k + 1 < state->input_count; ++k)
						state->inputs[(state->input_first + k) % CLI_STATE_INPUTS] =
							state->inputs[(state->input_first + k + 1) % CLI_STATE_INPUTS];
					state->inputs[(state->input_first + state->input_count - 1) % CLI_STATE_INPUTS] = entry;
				}
			}
			if (!e && !present)
				e = cli_state_push_input(state, text);
		}
		end = start;
	}
	if (close(fd) && !e)
		e = errno;
	fd = -1;
	if (unlinkat(state->directory, dstr_get(&state->temporary), 0) && !e)
		e = errno;
	for (size_t i = 0; i < CLI_STATE_INPUTS; ++i)
		dstr_cleanup(&entries[i]);
	return e;
}

static int
cli_state_repository_init (struct cli_state *state)
{
	int fd = openat(state->directory, "cli-init.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return errno;
	int e = cli_state_private(fd);
	if (!e)
		e = cli_state_lock(state, fd);
	struct stat status;
	bool exists = false;
	if (!e) {
		exists = !fstatat(state->directory, "config", &status, AT_SYMLINK_NOFOLLOW);
		if (!exists && errno != ENOENT)
			e = errno;
		else if (exists && !S_ISREG(status.st_mode))
			e = EACCES;
	}
	if (!e && exists) {
		e = cli_state_git_ok(state, (char const *[]){ "config", "--local", "--get", "askai.cliHistoryVersion", nullptr },
		                     (struct cstr_view){});
		if (!e && (dstr_len(&state->output) != 2 || memcmp(dstr_get(&state->output), "1\n", 2)))
			e = EINVAL;
	} else if (!e) {
		int directory_fd = dup(state->directory);
		DIR *directory = directory_fd < 0 ? nullptr : fdopendir(directory_fd);
		if (!directory) {
			e = errno;
			if (directory_fd >= 0)
				(void)close(directory_fd);
		} else {
			for (;;) {
				errno = 0;
				struct dirent *entry = readdir(directory);
				if (!entry) {
					e = errno;
					break;
				}
				if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") &&
				    strcmp(entry->d_name, "cli-init.lock")) {
					e = EEXIST;
					break;
				}
			}
			if (closedir(directory) && !e)
				e = errno;
		}
		if (!e)
			e = cli_state_git_ok(state, (char const *[]){ "init", "--bare", "--quiet", ".", nullptr },
			                     (struct cstr_view){});
		if (!e)
			e = cli_state_git_ok(state, (char const *[]){ "config", "--local", "askai.cliHistoryVersion", "1", nullptr },
			                     (struct cstr_view){});
	}
	if (close(fd) && !e)
		e = errno;
	fd = -1;
	return e;
}

static int
cli_state_repository (struct cli_state *state)
{
	int e = cli_state_repository_init(state);
	if (e)
		return e;
	uint64_t hash = UINT64_C(14695981039346656037);
	struct cstr_view identity = cstr_view_dstr(&state->identity);
	for (size_t i = 0; i < identity.len; ++i) {
		hash ^= (unsigned char)identity.data[i];
		hash *= UINT64_C(1099511628211);
	}
	char group[17];
	for (size_t i = 0; i < 16; ++i)
		group[i] = "0123456789abcdef"[(hash >> (60 - 4 * i)) & 15];
	group[16] = '\0';
	char writer[33];
	writer[32] = '\0';
	e = util_random_hex(writer, 16);
	if (e)
		return e;
	for (size_t i = 0; i < identity.len; ++i) {
		char byte = identity.data[i] ? identity.data[i] : '\n';
		if (!dstr_cat_n(&state->metadata, &byte, 1))
			return ENOMEM;
	}
	if (!dstr_cat_n(&state->metadata, "\nwriter=", 8) ||
	    !dstr_cat_n(&state->metadata, writer, 32) ||
	    !dstr_cat_n(&state->metadata, "\n", 1))
		return ENOMEM;
	if (!dstr_cat_n(&state->branch, "refs/heads/cli/", sizeof "refs/heads/cli/" - 1) ||
	    !dstr_cat_n(&state->branch, group, 16) ||
	    !dstr_cat_n(&state->branch, "/", 1))
		return ENOMEM;
	e = cli_state_copy(&state->latest, cstr_view_dstr(&state->branch));
	if (e || !dstr_cat_n(&state->latest, "latest", 6) ||
	    !dstr_cat_n(&state->branch, writer, 32) ||
	    !dstr_cat_n(&state->name, "cli-", 4) ||
	    !dstr_cat_n(&state->name, writer, 32) ||
	    !dstr_cat_n(&state->name, ".snapshot", 9))
		return ENOMEM;
	e = cli_state_copy(&state->temporary, cstr_view_dstr(&state->name));
	if (e || !dstr_cat_n(&state->temporary, ".tmp", 4))
		return ENOMEM;
	(void)dstr_clear(&state->scratch);
	if (!dstr_cat_n(&state->scratch, "cli-", 4) ||
	    !dstr_cat_n(&state->scratch, group, 16) ||
	    !dstr_cat_n(&state->scratch, ".lock", 5))
		return ENOMEM;
	state->lock = openat(state->directory, dstr_get(&state->scratch),
	                     O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (state->lock < 0)
		return errno;
	e = cli_state_private(state->lock);
	if (e)
		return e;
	int status = 0;
	e = cli_state_git(state, (char const *[]){ "rev-parse", "--verify", "--quiet", dstr_get(&state->latest), nullptr },
	                  (struct cstr_view){}, -1, &status);
	if (e || status == 1)
		return e;
	if (status)
		return EIO;
	char oid[65];
	size_t oid_len = 0;
	e = cli_state_oid(state, oid, &oid_len);
	if (!e)
		e = cli_state_copy(&state->parent, (struct cstr_view){ oid, oid_len });
	if (!e)
		e = cli_state_copy(&state->scratch, cstr_view_dstr(&state->parent));
	if (e || !dstr_cat_n(&state->scratch, ":snapshot", 9))
		return e ? e : ENOMEM;
	int fd = openat(state->directory, dstr_get(&state->temporary),
	                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return errno;
	e = cli_state_git(state, (char const *[]){ "cat-file", "blob", dstr_get(&state->scratch), nullptr },
	                  (struct cstr_view){}, fd, &status);
	if (!e && status)
		e = EIO;
	if (!e && lseek(fd, 0, SEEK_SET) < 0)
		e = errno;
	if (!e)
		e = cli_state_load(state, fd);
	if (close(fd) && !e)
		e = errno;
	fd = -1;
	if (unlinkat(state->directory, dstr_get(&state->temporary), 0) && !e)
		e = errno;
	return e ? e : cli_state_merge_inputs(state);
}

struct cli_state *
cli_state_create (struct cstr_view    path,
                  struct cstr_view    identity,
                  _Atomic bool const *cancel,
                  int                *e)
{
	*e = EINVAL;
	if (!identity.data || !identity.len || identity.len > CLI_STATE_ID_BYTES ||
	    !cli_state_valid(path) || jb_utf8(identity.data, identity.len) != identity.len)
		return nullptr;
	struct cli_state *state = calloc(1, sizeof *state);
	if (!state) {
		*e = ENOMEM;
		return nullptr;
	}
	state->cancel = cancel;
	state->directory = state->lock = -1;
	for (size_t i = 0; i < CLI_STATE_INPUTS; ++i)
		state->inputs[i] = dstr("");
	for (size_t i = 0; i < CLI_STATE_TURNS; ++i)
		state->turns[i] = (struct cli_state_pair){ dstr(""), dstr("") };
	state->pending = (struct cli_state_pair){ dstr(""), dstr("") };
	state->identity = state->path = state->name = state->temporary = state->scratch = dstr("");
	state->branch = state->latest = state->parent = state->output = state->tree = state->metadata = dstr("");
	*e = cli_state_copy(&state->identity, identity);
	if (!*e)
		*e = path.len ? cli_state_copy(&state->path, path) : cli_state_default_path(state);
	if (!*e && mkdir(dstr_get(&state->path), 0700) && errno != EEXIST)
		*e = errno;
	if (!*e) {
		state->directory = open(dstr_get(&state->path), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (state->directory < 0)
			*e = errno;
	}
	if (!*e) {
		struct stat status;
		if (fstat(state->directory, &status))
			*e = errno;
		else if (status.st_uid != geteuid() || (status.st_mode & 0077))
			*e = EACCES;
	}
	if (!*e) {
		size_t count = 0;
		while (environ[count])
			++count;
		state->environment = calloc(count + 3, sizeof *state->environment);
		if (!state->environment)
			*e = ENOMEM;
		else {
			size_t used = 0;
			for (size_t i = 0; i < count; ++i)
				if (strncmp(environ[i], "GIT_", 4))
					state->environment[used++] = environ[i];
			state->environment[used++] = "GIT_CONFIG_NOSYSTEM=1";
			state->environment[used] = "GIT_CONFIG_GLOBAL=/dev/null";
		}
	}
	if (!*e)
		*e = cli_state_repository(state);
	if (*e)
		cli_state_destroy(&state);
	return state;
}

void
cli_state_destroy (struct cli_state **pstate)
{
	if (!pstate || !*pstate)
		return;
	struct cli_state *state = *pstate;
	if (state->directory >= 0) {
		if (!state->snapshot_pending && dstr_len(&state->name))
			(void)unlinkat(state->directory, dstr_get(&state->name), 0);
		if (dstr_len(&state->temporary))
			(void)unlinkat(state->directory, dstr_get(&state->temporary), 0);
	}
	if (state->lock >= 0)
		(void)close(state->lock);
	state->lock = -1;
	if (state->directory >= 0)
		(void)close(state->directory);
	state->directory = -1;
	for (size_t i = 0; i < CLI_STATE_INPUTS; ++i)
		dstr_cleanup(&state->inputs[i]);
	for (size_t i = 0; i < CLI_STATE_TURNS; ++i) {
		dstr_cleanup(&state->turns[i].prompt);
		dstr_cleanup(&state->turns[i].answer);
	}
	dstr_cleanup(&state->pending.prompt);
	dstr_cleanup(&state->pending.answer);
	dstr_cleanup(&state->identity);
	dstr_cleanup(&state->path);
	dstr_cleanup(&state->name);
	dstr_cleanup(&state->temporary);
	dstr_cleanup(&state->scratch);
	dstr_cleanup(&state->branch);
	dstr_cleanup(&state->latest);
	dstr_cleanup(&state->parent);
	dstr_cleanup(&state->output);
	dstr_cleanup(&state->tree);
	dstr_cleanup(&state->metadata);
	free(state->environment);
	state->environment = nullptr;
	free(state);
	state = nullptr;
	*pstate = nullptr;
}

struct cstr_view
cli_state_recovery_file (struct cli_state const *state)
{
	return state && state->snapshot_pending ? cstr_view_dstr(&state->name)
	                                       : (struct cstr_view){};
}

size_t
cli_state_input_count (struct cli_state const *state)
{
	return state ? state->input_count : 0;
}

struct cstr_view
cli_state_input_at (struct cli_state const *state, size_t index)
{
	return state && index < state->input_count
	       ? cstr_view_dstr(&state->inputs[(state->input_first + index) % CLI_STATE_INPUTS])
	       : (struct cstr_view){};
}

size_t
cli_state_turn_count (struct cli_state const *state)
{
	return state ? state->turn_count : 0;
}

struct cli_state_turn
cli_state_turn_at (struct cli_state const *state, size_t index)
{
	return state && index < state->turn_count
	       ? (struct cli_state_turn){ cstr_view_dstr(&state->turns[index].prompt),
	                                  cstr_view_dstr(&state->turns[index].answer) }
	       : (struct cli_state_turn){};
}

int
cli_state_add_input (struct cli_state *state, struct cstr_view text)
{
	if (!state || !cli_state_valid(text))
		return EINVAL;
	if (text.len > CLI_STATE_INPUT_BYTES)
		return EFBIG;
	if (!text.len)
		return 0;
	struct cstr_view last = cli_state_input_at(state, state->input_count - 1);
	if (cstr_view_equal(last, text))
		return cli_state_save(state);
	int e = cli_state_push_input(state, text);
	return e ? e : cli_state_save(state);
}

int
cli_state_add_turn (struct cli_state *state,
                    struct cstr_view  prompt,
                    struct cstr_view  answer,
                    int               replace)
{
	if (!state || (replace && !state->turn_count) ||
	    !cli_state_valid(prompt) || !cli_state_valid(answer))
		return EINVAL;
	size_t index = state->turn_count - (replace != 0);
	if (index >= CLI_STATE_TURNS)
		return EFBIG;
	size_t bytes = state->turn_bytes;
	if (replace)
		bytes -= dstr_len(&state->turns[index].prompt) + dstr_len(&state->turns[index].answer);
	if (prompt.len > CLI_STATE_TURN_BYTES - bytes ||
	    answer.len > CLI_STATE_TURN_BYTES - bytes - prompt.len)
		return EFBIG;
	int e = cli_state_copy(&state->pending.prompt, prompt);
	if (!e)
		e = cli_state_copy(&state->pending.answer, answer);
	if (e)
		return e;
	struct cli_state_pair previous = state->turns[index];
	state->turns[index] = state->pending;
	state->pending = previous;
	state->turn_count = index + 1;
	state->turn_bytes = bytes + prompt.len + answer.len;
	return cli_state_save(state);
}

int
cli_state_clear_turns (struct cli_state *state)
{
	if (!state)
		return EINVAL;
	state->turn_count = state->turn_bytes = 0;
	return cli_state_save(state);
}
