/** @file cli_state.c
 *
 * Atomic, counted snapshots; never stores provider reasoning.
 */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "child.h"
#include "cli_state.h"
#include "cstr_view.h"
#include "dstr.h"
#include "git.h"
#include "hexid.h"
#include "jb.h"
#include "oid.h"
#include "util.h"

enum {
	CLI_STATE_TURN_BYTES = 16 << 20,
	CLI_STATE_ID_BYTES = 64 << 10,
};

struct cli_state {
	_Atomic bool const     *cancel;
	struct dstr             inputs[CLI_STATE_INPUTS];
	struct cli_state_turn   turns[CLI_STATE_TURNS];
	struct cli_state_turn   pending;
	struct dstr             identity;
	struct dstr             path;
	struct dstr             name;
	struct dstr             temporary;
	struct dstr             branch;
	struct dstr             latest;
	struct dstr             output;
	struct dstr             tree;
	struct dstr             metadata;
	struct dstr             scratch;
	struct cli_state_ring   ring;  /**< of the inputs */
	struct tally            turns_used;  /**< the turns and their bytes */
	int                     directory;
	int                     lock;
	uint32_t                flags;
	struct oid              identity_blob;     /**< empty until hashed */
	struct oid              parent;
};

/** The flags of struct cli_state. */
enum {
	CLI_STATE_SNAPSHOT_PENDING = 1u << 0,  /**< the snapshot is not committed */
	CLI_STATE_UNSAVED          = 1u << 1,  /**< the last save failed */
};

static char const cli_state_magic[] = "ASKAI-CLI-1\n";

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
	unsigned char bytes[4];
	util_be_put(bytes, value, sizeof bytes);
	return util_write_all(fd, bytes, sizeof bytes);
}

static int
cli_state_read_number (int     fd,
                       size_t *value)
{
	unsigned char bytes[4];
	int e = cli_state_read(fd, bytes, sizeof bytes);
	if (!e)
		*value = (size_t)util_be_get(bytes, sizeof bytes);
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
 * on a blocker, with a bounded cancellation delay; no UI tick is involved.
 */
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
		struct timespec delay = clk_timespec(20 * MS);
		int e = clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, nullptr);
		if (e && e != EINTR)
			return e;
	}
}

/** Run Git directly, with bounded captured output and no shell. */
static int
cli_state_git (struct cli_state  *state,
               char const *const  args[],
               struct cstr_view   input,
               int                output_fd,
               int               *status)
{
	char const *const prefix[] = {
		"git", "-C", dstr_get(&state->path), "-c", "user.name=AskAI CLI",
		"-c", "user.email=askai-cli@localhost", "-c", "core.fsync=committed"
	};
	char const *argv[32];
	int e = child_argv(argv, array_size(argv), prefix, array_size(prefix), args);
	if (e)
		return e;
	char *const env[] = { util_env_path(), GIT_ENV_FIXED, nullptr };
	struct child_cmd const cmd = {
		.file = "git",
		.argv = argv,
		.envp = env,
		.in   = { (unsigned char *)(uintptr_t)input.data, input.len },
	};
	struct child_fds fds = child_fds();
	(void)dstr_clear(&state->output);
	if (output_fd >= 0) {
		fds.out = output_fd;
		return child_run(&cmd, fds, status);
	}
	char buffer[4097];
	size_t got;
	e = child_capture(&cmd, fds, buffer, sizeof buffer, &got, status);
	return e ? e : cstr_view_cat((struct cstr_view){ buffer, got }, &state->output);
}

static int
cli_state_git_ok (struct cli_state  *state,
                  char const *const  args[],
                  struct cstr_view   input)
{
	int status = 0;
	int e = cli_state_git(state, args, input, -1, &status);
	return e ? e : status ? EIO : 0;
}

/* Runs git as cli_state_git_ok() does and stores the one name that it
 * prints in @p oid. Returns 0, EINVAL if git prints anything else, or the
 * error of cli_state_git_ok().
 */
static int
cli_state_git_oid (struct cli_state  *state,
                   char const *const  args[],
                   struct cstr_view   input,
                   struct oid        *oid)
{
	int e = cli_state_git_ok(state, args, input);
	return e ? e : oid_init_line(oid, cstr_view_dstr(&state->output));
}

static int
cli_state_tree_entry (struct cli_state *state,
                      struct oid const *oid,
                      char const       *name)
{
	return dstr_printf(&state->tree, "100644 blob %s\t%s\n", oid->hex, name) ? 0 : ENOMEM;
}

/* Write the blob of text to the object store and name it in the tree. */
static int
cli_state_tree_blob (struct cli_state *state,
                     char const       *name,
                     struct cstr_view  text)
{
	struct oid oid;
	int e = cli_state_git_oid(state, (char const *[]){ "hash-object", "-w", "--stdin", nullptr }, text,
	                          &oid);
	return e ? e : cli_state_tree_entry(state, &oid, name);
}

static int
cli_state_commit (struct cli_state *state)
{
	(void)dstr_clear(&state->tree);
	struct oid blob;
	int e = cli_state_git_oid(state, (char const *[]){ "hash-object", "-w", "--", dstr_get(&state->name), nullptr },
	                          (struct cstr_view){}, &blob);
	if (!e)
		e = cli_state_tree_entry(state, &blob, "snapshot");
	/* The identity of a writer never changes: write its blob once. */
	if (!e && !oid_len(&state->identity_blob))
		e = cli_state_git_oid(state, (char const *[]){ "hash-object", "-w", "--stdin", nullptr },
		                      cstr_view_dstr(&state->metadata), &state->identity_blob);
	if (!e)
		e = cli_state_tree_entry(state, &state->identity_blob, "identity.txt");
	if (!e && state->ring.used.n)
		e = cli_state_tree_blob(state, "input.txt", cli_state_input_at(state, state->ring.used.n - 1));
	if (!e && state->turns_used.n) {
		struct cli_state_turn const *last = &state->turns[state->turns_used.n - 1];
		e = cli_state_tree_blob(state, "prompt.txt", cstr_view_dstr(&last->prompt));
		if (!e)
			e = cli_state_tree_blob(state, "answer.txt", cstr_view_dstr(&last->answer));
	}
	struct oid tree;
	if (!e)
		e = cli_state_git_oid(state, (char const *[]){ "mktree", nullptr }, cstr_view_dstr(&state->tree),
		                      &tree);
	static char const message[] = "Save CLI input history and completed conversation\n";
	char const *args[] = { "commit-tree", tree.hex, "-F", "-", "-p", state->parent.hex, nullptr };
	if (!oid_len(&state->parent))
		args[4] = nullptr;
	struct oid commit;
	if (!e)
		e = cli_state_git_oid(state, args, (struct cstr_view){ message, sizeof message - 1 }, &commit);
	(void)dstr_clear(&state->scratch);
	if (!e && !dstr_printf(&state->scratch, "start\nupdate %s %s\nupdate %s %s\nprepare\ncommit\n",
	                       dstr_get(&state->branch), commit.hex, dstr_get(&state->latest), commit.hex))
		e = ENOMEM;
	if (!e)
		e = cli_state_lock(state, state->lock);
	if (!e) {
		e = cli_state_git_ok(state, (char const *[]){ "update-ref", "--stdin", nullptr }, cstr_view_dstr(&state->scratch));
		if (flock(state->lock, LOCK_UN) && !e)
			e = errno;
	}
	if (e)
		return e;
	state->parent = commit;
	(void)cli_state_git_ok(state, (char const *[]){ "gc", "--auto", "--quiet", nullptr },
	                       (struct cstr_view){});
	return 0;
}

static int
cli_state_save (struct cli_state *state)
{
	state->flags |= CLI_STATE_UNSAVED;
	int fd = openat(state->directory, dstr_get(&state->temporary),
	                O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return errno;
	int e = util_private_fd(fd, S_IFREG);
	if (!e && ftruncate(fd, 0))
		e = errno;
	if (!e)
		e = util_write_all(fd, cli_state_magic, sizeof cli_state_magic - 1);
	if (!e)
		e = cli_state_write_text(fd, &state->identity);
	if (!e)
		e = cli_state_write_number(fd, state->ring.used.n);
	if (!e)
		e = cli_state_write_number(fd, state->turns_used.n);
	for (size_t i = 0; !e && i < state->ring.used.n; ++i)
		e = cli_state_write_text(fd, &state->inputs[cli_state_ring_slot(&state->ring, i)]);
	for (size_t i = 0; !e && i < state->turns_used.n; ++i) {
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
			state->flags |= CLI_STATE_SNAPSHOT_PENDING;
	}
	if (!e && fsync(state->directory))
		e = errno;
	if (!e)
		e = cli_state_commit(state);
	if (!e) {
		state->flags &= ~(CLI_STATE_SNAPSHOT_PENDING | CLI_STATE_UNSAVED);
		if (unlinkat(state->directory, dstr_get(&state->name), 0))
			e = errno;
	}
	return e;
}

static int
cli_state_default_path (struct cli_state *state)
{
	char const *base;
	char const *rest;
	int e = util_state_path(&base, &rest);
	if (e)
		return e;
	if (!dstr_printf(&state->path, "%s%s/cli.git", base, rest))
		return ENOMEM;
	/* Only cli.git, which cli_state_create() creates with the mode 0700,
	 * contains data. util_mkdir_parents() does not change the mode of the
	 * parent directories that exist. */
	return util_mkdir_parents((char *)dstr_raw(&state->path));
}

static int
cli_state_push_input (struct cli_state *state,
                      struct cstr_view  text)
{
	int e = cstr_view_store(text, &state->scratch);
	if (e)
		return e;
	while (cli_state_ring_full(&state->ring, text.len))
		cli_state_ring_drop(&state->ring, dstr_len(&state->inputs[state->ring.first]));
	size_t index = cli_state_ring_add(&state->ring, text.len);
	struct dstr previous = state->inputs[index];
	state->inputs[index] = state->scratch;
	state->scratch = previous;
	return 0;
}

/* Add text to the input recall, or move an equal input to the newest place,
 * where Up finds it first.
 */
static int
cli_state_recall (struct cli_state *state,
                  struct cstr_view  text)
{
	if (!jb_is_text(text))
		return EILSEQ;
	struct cli_state_ring const *ring = &state->ring;
	for (size_t j = 0; j < ring->used.n; ++j) {
		if (!cstr_view_equal(cli_state_input_at(state, j), text))
			continue;
		struct dstr *inputs = state->inputs;
		struct dstr entry = inputs[cli_state_ring_slot(ring, j)];
		for (size_t k = j; k + 1 < ring->used.n; ++k)
			inputs[cli_state_ring_slot(ring, k)] = inputs[cli_state_ring_slot(ring, k + 1)];
		inputs[cli_state_ring_slot(ring, ring->used.n - 1)] = entry;
		return 0;
	}
	return cli_state_push_input(state, text);
}

/* Copy the snapshot of the commit rev to fd, from its start, and read its
 * header: check its magic and identity and read its counts.
 */
static int
cli_state_snapshot (struct cli_state *state,
                    int               fd,
                    struct oid const *rev,
                    size_t           *inputs,
                    size_t           *turns)
{
	(void)dstr_clear(&state->scratch);
	if (!dstr_printf(&state->scratch, "%s:snapshot", rev->hex))
		return ENOMEM;
	if (ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET) < 0)
		return errno;
	int status = 0;
	int e = cli_state_git(state, (char const *[]){ "cat-file", "blob", dstr_get(&state->scratch), nullptr },
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
	if (!e && !cstr_view_equal(cstr_view_dstr(&state->scratch), cstr_view_dstr(&state->identity)))
		e = EXDEV;
	if (!e)
		e = cli_state_read_number(fd, inputs);
	if (!e)
		e = cli_state_read_number(fd, turns);
	if (!e && (*inputs > array_size(state->inputs) ||
	           *turns > array_size(state->turns)))
		e = EFBIG;
	return e;
}

/* Read the count turns of the latest snapshot in fd, which ends after them. */
static int
cli_state_read_turns (struct cli_state *state,
                      int               fd,
                      size_t            count)
{
	size_t budget = CLI_STATE_TURN_BYTES;

	for (size_t i = 0; i < count; ++i) {
		struct cli_state_turn *t = &state->turns[i];
		int e = cli_state_read_text(fd, &t->prompt, &budget);
		if (e || (e = cli_state_read_text(fd, &t->answer, &budget)))
			return e;
	}

	state->turns_used = (struct tally){ count, CLI_STATE_TURN_BYTES - budget };

	char trailing = '\0';
	size_t got = 0;
	int e = util_read_all(fd, &trailing, 1, -1, &got);

	return !e && got ? EINVAL : e;
}

/* Add the count inputs that follow the header of the snapshot in fd to the
 * recall. Each input is read into entry.
 */
static int
cli_state_read_inputs (struct cli_state *state,
                       int               fd,
                       size_t            count,
                       struct dstr      *entry)
{
	size_t budget = CLI_STATE_INPUT_BYTES;
	int e = 0;
	for (size_t i = 0; !e && i < count; ++i) {
		e = cli_state_read_text(fd, entry, &budget);
		if (!e)
			e = cli_state_recall(state, cstr_view_dstr(entry));
	}
	return e;
}

/** Load the turns of the latest snapshot, the parent, and merge the input
 * recall of up to 32 recent writers of the group. Completed conversations
 * from different writers never mix.
 */
static int
cli_state_load (struct cli_state *state,
                uint64_t          group)
{
	/* The parent, then the tips that git lists, as many as fit. */
	struct oid tips[1 + 32] = { state->parent };
	char option[sizeof "--count=" + UTIL_DECIMAL];
	(void)util_format(option, sizeof option, "--count=%zu", array_size(tips) - 1);
	(void)dstr_clear(&state->scratch);
	int e = dstr_printf(&state->scratch, "refs/heads/cli/%016" PRIx64 "/", group) ? 0 : ENOMEM;
	if (!e)
		e = cli_state_git_ok(state, (char const *[]){ "for-each-ref", "--sort=-committerdate", option,
		                         "--format=%(objectname)", dstr_get(&state->scratch), nullptr },
		                         (struct cstr_view){});
	size_t n = 1;
	for (struct cstr_view out = cstr_view_dstr(&state->output); !e && out.len; )
		if (n == array_size(tips) || oid_init_field(&tips[n], &out, '\n'))
			e = EIO;
		else
			n += !oid_equal(&tips[n], &state->parent);
	if (e)
		return e;
	int fd = openat(state->directory, dstr_get(&state->temporary),
	                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return errno;
	e = util_private_fd(fd, S_IFREG);
	dstr_auto entry = dstr("");
	/* Git lists newest tips first. Process the selected latest snapshot last
	 * even when Git's second-resolution commit times tie. Repeated exact
	 * inputs move to the newest position, preserving the expected Up entry. */
	for (size_t t = n; !e && t--; ) {
		size_t count = 0, turns = 0;
		e = cli_state_snapshot(state, fd, &tips[t], &count, &turns);
		if (!e)
			e = cli_state_read_inputs(state, fd, count, &entry);
		/* The parent is the first tip. */
		if (!e && !t)
			e = cli_state_read_turns(state, fd, turns);
	}
	if (close(fd) && !e)
		e = errno;
	fd = -1;
	if (unlinkat(state->directory, dstr_get(&state->temporary), 0) && !e)
		e = errno;
	return e;
}

/* Returns 0 if the repository directory contains no entries but ".", ".."
 * and cli-init.lock, EEXIST if it contains another entry, or another error
 * number.
 */
static int
cli_state_directory_check_empty (struct cli_state *state)
{
	static struct cstr_view const allowed[] = {
		CSTR_VIEW_INIT("."), CSTR_VIEW_INIT(".."), CSTR_VIEW_INIT("cli-init.lock")
	};
	int directory_fd = dup(state->directory);
	if (directory_fd < 0)
		return errno;
	DIR *directory = fdopendir(directory_fd);
	if (!directory) {
		int e = errno;
		util_close(&directory_fd);
		return e;
	}
	directory_fd = -1;
	int e = 0;
	for (;;) {
		/* readdir() returns nullptr at the end and after an error, and
		 * only an error sets errno. */
		errno = 0;
		struct dirent *entry = readdir(directory);
		if (!entry) {
			e = errno;
			break;
		}
		if (cstr_view_index(allowed, array_size(allowed), cstr_view(entry->d_name)) == array_size(allowed)) {
			e = EEXIST;
			break;
		}
	}
	if (closedir(directory) && !e)
		e = errno;
	return e;
}

/* Check the version of the history repository if its config file exists.
 * Otherwise create the repository, if the directory is empty. Returns 0 or
 * an error number.
 */
static int
cli_state_repository_open (struct cli_state *state)
{
	struct stat status;
	if (!fstatat(state->directory, "config", &status, AT_SYMLINK_NOFOLLOW)) {
		if (!S_ISREG(status.st_mode))
			return EACCES;
		int e = cli_state_git_ok(state, (char const *[]){ "config", "--local", "--get", "askai.cliHistoryVersion", nullptr },
		                         (struct cstr_view){});
		if (!e && !cstr_view_equal(cstr_view_dstr(&state->output), CSTR_VIEW("1\n")))
			e = EINVAL;
		return e;
	}
	if (errno != ENOENT)
		return errno;
	int e = cli_state_directory_check_empty(state);
	if (!e)
		e = cli_state_git_ok(state, (char const *[]){ "init", "--bare", "--quiet", ".", nullptr },
		                     (struct cstr_view){});
	if (!e)
		e = cli_state_git_ok(state, (char const *[]){ "config", "--local", "askai.cliHistoryVersion", "1", nullptr },
		                     (struct cstr_view){});
	return e;
}

/* Open or create the history repository while cli-init.lock is locked.
 * Returns 0 or an error number.
 */
static int
cli_state_repository_init (struct cli_state *state)
{
	int fd = openat(state->directory, "cli-init.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return errno;
	int e = util_private_fd(fd, S_IFREG);
	if (!e)
		e = cli_state_lock(state, fd);
	if (!e)
		e = cli_state_repository_open(state);
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
	/* The writers of an identity form the group of its FNV-1a hash. */
	uint64_t group = UINT64_C(14695981039346656037);
	struct cstr_view identity = cstr_view_dstr(&state->identity);
	for (size_t i = 0; i < identity.len; ++i) {
		group ^= (unsigned char)identity.data[i];
		group *= UINT64_C(1099511628211);
	}
	struct hexid writer;
	e = hexid_make(&writer);
	if (!e)
		e = cstr_view_store(identity, &state->metadata);
	if (e)
		return e;
	/* The readable copy of the identity has a line for each of its parts. */
	unsigned char *metadata = dstr_raw(&state->metadata);
	for (size_t i = 0; i < identity.len; ++i)
		if (!metadata[i])
			metadata[i] = '\n';
	(void)dstr_clear(&state->scratch);
	if (!dstr_printf(&state->metadata, "\nwriter=%s\n", writer.hex) ||
	    !dstr_printf(&state->branch, "refs/heads/cli/%016" PRIx64 "/%s", group, writer.hex) ||
	    !dstr_printf(&state->latest, "refs/heads/cli/%016" PRIx64 "/latest", group) ||
	    !dstr_printf(&state->name, "cli-%s.snapshot", writer.hex) ||
	    !dstr_printf(&state->temporary, "cli-%s.snapshot.tmp", writer.hex) ||
	    !dstr_printf(&state->scratch, "cli-%016" PRIx64 ".lock", group))
		return ENOMEM;
	state->lock = openat(state->directory, dstr_get(&state->scratch),
	                     O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (state->lock < 0)
		return errno;
	e = util_private_fd(state->lock, S_IFREG);
	if (e)
		return e;
	int status = 0;
	e = cli_state_git(state, (char const *[]){ "rev-parse", "--verify", "--quiet", dstr_get(&state->latest), nullptr },
	                  (struct cstr_view){}, -1, &status);
	if (e || status == 1)
		return e;
	if (status)
		return EIO;
	e = oid_init_line(&state->parent, cstr_view_dstr(&state->output));
	return e ? e : cli_state_load(state, group);
}

struct cli_state *
cli_state_create (struct cstr_view    path,
                  struct cstr_view    identity,
                  _Atomic bool const *cancel,
                  int                *e)
{
	*e = EINVAL;
	if (!identity.data || !identity.len || identity.len > CLI_STATE_ID_BYTES ||
	    !jb_is_text(path) || jb_utf8(identity.data, identity.len) != identity.len)
		return nullptr;
	struct cli_state *state = calloc(1, sizeof *state);
	if (!state) {
		*e = ENOMEM;
		return nullptr;
	}
	state->cancel = cancel;
	state->directory = state->lock = -1;
	for (size_t i = 0; i < array_size(state->inputs); ++i)
		state->inputs[i] = dstr("");
	for (size_t i = 0; i < array_size(state->turns); ++i)
		state->turns[i] = (struct cli_state_turn){ dstr(""), dstr("") };
	state->pending = (struct cli_state_turn){ dstr(""), dstr("") };
	state->identity = state->path = state->name = state->temporary = state->scratch = dstr("");
	state->branch = state->latest = state->output = state->tree = state->metadata = dstr("");
	*e = cstr_view_store(identity, &state->identity);
	if (!*e)
		*e = path.len ? cstr_view_store(path, &state->path) : cli_state_default_path(state);
	if (!*e && mkdir(dstr_get(&state->path), 0700) && errno != EEXIST)
		*e = errno;
	if (!*e) {
		state->directory = open(dstr_get(&state->path), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (state->directory < 0)
			*e = errno;
	}
	if (!*e)
		*e = util_private_fd(state->directory, S_IFDIR);
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
		if (!(state->flags & CLI_STATE_SNAPSHOT_PENDING) && dstr_len(&state->name))
			(void)unlinkat(state->directory, dstr_get(&state->name), 0);
		if (dstr_len(&state->temporary))
			(void)unlinkat(state->directory, dstr_get(&state->temporary), 0);
		util_close(&state->directory);
	}

	util_close(&state->lock);

	for (size_t i = 0; i < array_size(state->inputs); ++i)
		dstr_cleanup(&state->inputs[i]);

	for (size_t i = 0; i < array_size(state->turns); ++i) {
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
	dstr_cleanup(&state->output);
	dstr_cleanup(&state->tree);
	dstr_cleanup(&state->metadata);
	free(state);
	state = nullptr;
	*pstate = nullptr;
}

struct cstr_view
cli_state_recovery_file (struct cli_state const *state)
{
	return state && (state->flags & CLI_STATE_SNAPSHOT_PENDING) ? cstr_view_dstr(&state->name)
	                                                            : (struct cstr_view){};
}

size_t
cli_state_input_count (struct cli_state const *state)
{
	return state ? state->ring.used.n : 0;
}

struct cstr_view
cli_state_input_at (struct cli_state const *state,
                    size_t                  index)
{
	return state && index < state->ring.used.n
	       ? cstr_view_dstr(&state->inputs[cli_state_ring_slot(&state->ring, index)])
	       : (struct cstr_view){};
}

size_t
cli_state_turn_count (struct cli_state const *state)
{
	return state ? state->turns_used.n : 0;
}

struct cli_state_turn const *
cli_state_turn_at (struct cli_state const *state,
                   size_t                  index)
{
	return state && index < state->turns_used.n ? &state->turns[index] : nullptr;
}

int
cli_state_add_input (struct cli_state *state,
                     struct cstr_view  text)
{
	if (!state || !jb_is_text(text))
		return EINVAL;
	if (text.len > CLI_STATE_INPUT_BYTES)
		return EFBIG;
	if (!text.len)
		return 0;
	/* An input that repeats the last one saves only to retry a failure. */
	if (!cstr_view_equal(cli_state_input_at(state, state->ring.used.n - 1), text)) {
		int e = cli_state_push_input(state, text);
		if (e)
			return e;
	} else if (!(state->flags & CLI_STATE_UNSAVED)) {
		return 0;
	}
	return cli_state_save(state);
}

int
cli_state_add_turn (struct cli_state *state,
                    struct cstr_view  prompt,
                    struct cstr_view  answer,
                    bool              replace)
{
	if (!state || (replace && !state->turns_used.n) ||
	    !jb_is_text(prompt) || !jb_is_text(answer))
		return EINVAL;
	struct tally used = state->turns_used;
	size_t const index = used.n - replace;
	if (replace)
		tally_sub(&used, dstr_len(&state->turns[index].prompt) + dstr_len(&state->turns[index].answer));
	if (!tally_fits(&used, (struct tally){ array_size(state->turns), CLI_STATE_TURN_BYTES },
	                prompt.len + answer.len))
		return EFBIG;
	int e = cstr_view_store(prompt, &state->pending.prompt);
	if (!e)
		e = cstr_view_store(answer, &state->pending.answer);
	if (e)
		return e;
	struct cli_state_turn previous = state->turns[index];
	state->turns[index] = state->pending;
	state->pending = previous;
	tally_add(&used, prompt.len + answer.len);
	state->turns_used = used;
	return cli_state_save(state);
}

int
cli_state_clear_turns (struct cli_state *state)
{
	if (!state)
		return EINVAL;
	state->turns_used = (struct tally){};
	return cli_state_save(state);
}
