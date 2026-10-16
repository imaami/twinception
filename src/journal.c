/** @file The durable transcript writer (journal.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/pidfd.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "journal.h"
#include "loop.h"
#include "util.h"

enum {
	JOURNAL_QUEUE = 256,
	JOURNAL_BYTES = 32 << 20,
	JOURNAL_CONTROL = 1024,
	JOURNAL_CONTROL_BYTES = 2 << 20,
	JOURNAL_CONTROL_RECORD = 1024,
	JOURNAL_BATCH = 4 << 20,
	JOURNAL_HEADER = 120,
	JOURNAL_SIGNED = 88,
	JOURNAL_GIT_MS = 10000,
};

struct journal_item {
	struct msg             msg;
	struct journal_item   *next;
	struct journal_record *record;
	struct loop           *loop;
	journal_done_cb       *done;
	journal_load_cb       *loaded;
	void                  *arg;
	size_t                 bytes;
	int                    error;
	bool                   current;
	char                   id[JOURNAL_ID + 1];
	char                   kind[JOURNAL_KIND + 1];
};

static_assert((sizeof(struct journal_item) + sizeof(struct journal_record) +
               JOURNAL_CONTROL_RECORD + 1) * JOURNAL_CONTROL <= JOURNAL_CONTROL_BYTES);

struct journal {
	pthread_mutex_t        lock;
	pthread_cond_t         ready;
	pthread_t              thread;
	struct journal_item   *first;
	struct journal_item   *last;
	char                  *path;
	char                  *ref;
	uint64_t               seed;
	uint64_t               sequence;
	uint64_t               committed;
	size_t                 queued;
	size_t                 bytes;
	_Atomic int            error;
	int                    repo_fd;
	int                    state_fd;
	int                    lock_fd;
	int                    wal_fd;
	bool                   mutex_ready;
	bool                   cond_ready;
	bool                   started;
	bool                   stop;
	char                   tip[65];
};

static bool
journal_word (char const *word, size_t max)
{
	if (!word || !*word)
		return false;
	size_t n = 0;
	for (; word[n] && n < max; ++n) {
		unsigned char c = (unsigned char)word[n];
		if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
		    !(c >= '0' && c <= '9') && c != '-' && c != '_')
			return false;
	}
	return n && n <= max && !word[n];
}

static bool
journal_id (char const *id)
{
	if (!id)
		return false;
	for (size_t i = 0; i < JOURNAL_ID; ++i)
		if (!(id[i] >= 'a' && id[i] <= 'f') &&
		    !(id[i] >= '0' && id[i] <= '9'))
			return false;
	return !id[JOURNAL_ID];
}

static int
journal_write (int fd, void const *data, size_t len)
{
	unsigned char const *p = data;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (!n)
			return EIO;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* A short read is EOF, not success disguised as a complete record. */
static int
journal_read (int fd, void *data, size_t len, off_t at, size_t *got)
{
	unsigned char *p = data;
	*got = 0;
	while (*got < len) {
		ssize_t n = pread(fd, p + *got, len - *got, at + (off_t)*got);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (!n)
			break;
		*got += (size_t)n;
	}
	return 0;
}

static void
journal_number (unsigned char *to, uint64_t number)
{
	for (size_t i = 0; i < 8; ++i) {
		to[7 - i] = (unsigned char)number;
		number >>= 8;
	}
}

static uint64_t
journal_value (unsigned char const *from)
{
	uint64_t value = 0;
	for (size_t i = 0; i < 8; ++i)
		value = (value << 8) | from[i];
	return value;
}

static int
journal_digest (unsigned char const *header, void const *data, size_t len,
                unsigned char digest[32])
{
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (!ctx)
		return ENOMEM;
	unsigned n = 0;
	bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
	          EVP_DigestUpdate(ctx, header, JOURNAL_SIGNED) == 1 &&
	          EVP_DigestUpdate(ctx, data, len) == 1 &&
	          EVP_DigestFinal_ex(ctx, digest, &n) == 1 && n == 32;
	EVP_MD_CTX_free(ctx);
	ctx = nullptr;
	return ok ? 0 : EIO;
}

static int
journal_directory (int parent, char const *name, int *fd)
{
	bool made = !mkdirat(parent, name, 0700);
	if (!made && errno != EEXIST)
		return errno;
	*fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (*fd < 0)
		return errno;
	if (made && fsync(parent))
		return errno;
	return 0;
}

/* Walk, opening each component without following links. */
static int
journal_open_path (struct journal *j, char const *path)
{
	if (!path || !*path)
		return EINVAL;
	char *copy = strdup(path);
	if (!copy)
		return ENOMEM;
	int fd = open(*path == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int e = fd < 0 ? errno : 0;
	char *rest = copy;
	for (char *part; !e && (part = strsep(&rest, "/")); ) {
		if (!*part || !strcmp(part, "."))
			continue;
		if (!strcmp(part, "..")) {
			e = EINVAL;
			break;
		}
		int next = -1;
		e = journal_directory(fd, part, &next);
		close(fd);
		fd = next;
	}
	free(copy);
	copy = nullptr;
	struct stat st;
	if (!e && fstat(fd, &st))
		e = errno;
	if (!e && (st.st_uid != geteuid() || (st.st_mode & 0022)))
		e = EACCES;
	if (!e && (st.st_mode & 0077) && (fchmod(fd, 0700) || fsync(fd)))
		e = errno;
	if (!e) {
		j->path = realpath(path, nullptr);
		if (!j->path)
			e = errno;
	}
	if (e) {
		if (fd >= 0)
			close(fd);
		return e;
	}
	j->repo_fd = fd;
	return 0;
}

static int
journal_file (int dir, char const *name, int *fd)
{
	*fd = openat(dir, name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (*fd < 0)
		return errno;
	struct stat st;
	if (fstat(*fd, &st))
		return errno;
	if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & 0077) || st.st_nlink != 1)
		return EACCES;
	return 0;
}

static int
journal_private (int fd)
{
	struct stat st;
	if (fstat(fd, &st))
		return errno;
	if (st.st_uid != geteuid() || (st.st_mode & 0022))
		return EACCES;
	if ((st.st_mode & 0077) && (fchmod(fd, 0700) || fsync(fd)))
		return errno;
	return 0;
}

/* Fixed argv and environment.  Input/output are anonymous files, so a
 * child's output can never deadlock its input pipe.  Only the writer and
 * startup call this function. */
static int
journal_git (struct journal *j, char const *const *command, int input,
             char **output, size_t maximum, int *status)
{
	static char *const env[] = {
		"PATH=/usr/bin:/bin", "HOME=/nonexistent", "LC_ALL=C",
		"GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null",
		"GIT_TERMINAL_PROMPT=0", "GIT_NO_LAZY_FETCH=1",
		"GIT_ALLOW_PROTOCOL=none", "GIT_PAGER=cat", nullptr
	};
	char const *argv[48] = {
		"git", "--git-dir=.", "--no-pager", "--no-replace-objects",
		"-c", "core.hooksPath=/dev/null", "-c", "core.fsmonitor=false",
		"-c", "core.fsync=all", "-c", "core.fsyncMethod=fsync",
		"-c", "gc.auto=0", "-c", "maintenance.auto=false"
	};
	size_t argc = 16;
	for (size_t i = 0; command[i]; ++i) {
		if (argc + 1 >= ARRAY_SIZE(argv))
			return E2BIG;
		argv[argc++] = command[i];
	}
	argv[argc] = nullptr;
	int out = memfd_create("askai-journal-git", MFD_CLOEXEC);
	int null = open("/dev/null", O_RDWR | O_CLOEXEC);
	int e = out < 0 || null < 0 ? errno : 0;
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attr;
	bool have_actions = false, have_attr = false;
	if (!e) {
		e = posix_spawn_file_actions_init(&actions);
		have_actions = !e;
	}
	if (!e) {
		e = posix_spawnattr_init(&attr);
		have_attr = !e;
	}
	if (!e)
		e = posix_spawn_file_actions_addfchdir_np(&actions, j->repo_fd);
	if (!e)
		e = posix_spawn_file_actions_adddup2(&actions, input >= 0 ? input : null,
		                                    STDIN_FILENO);
	if (!e)
		e = posix_spawn_file_actions_adddup2(&actions, out, STDOUT_FILENO);
	if (!e)
		e = posix_spawn_file_actions_adddup2(&actions, null, STDERR_FILENO);
	/* A Git child can outlive an abruptly killed AskAI.  Keep the same
	 * flock open through exec, so restart cannot race that child. */
	if (!e && j->lock_fd >= 0)
		e = posix_spawn_file_actions_adddup2(&actions, j->lock_fd, j->lock_fd);
	sigset_t mask, defaults;
	sigemptyset(&mask);
	sigemptyset(&defaults);
	sigaddset(&defaults, SIGPIPE);
	sigaddset(&defaults, SIGINT);
	sigaddset(&defaults, SIGTERM);
	if (!e)
		e = posix_spawnattr_setsigmask(&attr, &mask);
	if (!e)
		e = posix_spawnattr_setsigdefault(&attr, &defaults);
	if (!e)
		e = posix_spawnattr_setpgroup(&attr, 0);
	if (!e)
		e = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK |
		                                    POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
	pid_t pid = -1;
	if (!e)
		e = posix_spawnp(&pid, "git", &actions, &attr, (char *const *)argv, env);
	if (have_actions)
		posix_spawn_file_actions_destroy(&actions);
	if (have_attr)
		posix_spawnattr_destroy(&attr);
	int result = 0;
	if (!e) {
		int pidfd = pidfd_open(pid, 0);
		if (pidfd < 0 && errno != ESRCH)
			e = errno;
		if (pidfd >= 0) {
			struct pollfd watch = { .fd = pidfd, .events = POLLIN };
			uint64_t start = loop_clock();
			uint64_t deadline = start + (uint64_t)JOURNAL_GIT_MS * 1'000'000;
			for (;;) {
				uint64_t now = loop_clock();
				int timeout = now >= deadline ? 0 :
				              (int)((deadline - now + 999'999) / 1'000'000);
				int ready = poll(&watch, 1, timeout);
				if (ready > 0)
					break;
				if (!ready) {
					e = ETIMEDOUT;
					break;
				}
				if (errno != EINTR) {
					e = errno;
					break;
				}
			}
			if (e)
				(void)pidfd_send_signal(pidfd, SIGKILL, nullptr, 0);
			close(pidfd);
			pidfd = -1;
		} else if (e) {
			(void)kill(pid, SIGKILL);
		}
		pid_t got;
		do {
			got = waitpid(pid, &result, 0);
		} while (got < 0 && errno == EINTR);
		if (got < 0 && !e)
			e = errno;
		else if (!e && !WIFEXITED(result))
			e = EIO;
		else if (!e && status)
			*status = WEXITSTATUS(result);
		else if (!e && WEXITSTATUS(result))
			e = EIO;
	}
	if (!e && output) {
		struct stat st;
		if (fstat(out, &st))
			e = errno;
		else if (st.st_size < 0 || (uint64_t)st.st_size > maximum)
			e = EFBIG;
		else if (!(*output = malloc((size_t)st.st_size + 1)))
			e = ENOMEM;
		else {
			size_t got = 0;
			e = journal_read(out, *output, (size_t)st.st_size, 0, &got);
			if (!e && got != (size_t)st.st_size)
				e = EIO;
			(*output)[got] = '\0';
		}
	}
	if (out >= 0)
		close(out);
	if (null >= 0)
		close(null);
	return e;
}

static int
journal_tip (struct journal *j)
{
	char const *command[] = { "rev-parse", "--verify", "--quiet", j->ref, nullptr };
	char *out = nullptr;
	int status = 0;
	int e = journal_git(j, command, -1, &out, 80, &status);
	if (!e && status == 1)
		j->tip[0] = '\0';
	else if (!e && status)
		e = EIO;
	else if (!e) {
		size_t n = strcspn(out, "\n");
		if ((n != 40 && n != 64) || out[n] != '\n' || out[n + 1])
			e = EIO;
		else {
			for (size_t i = 0; i < n; ++i)
				if (!(out[i] >= '0' && out[i] <= '9') &&
				    !(out[i] >= 'a' && out[i] <= 'f'))
					e = EIO;
			if (!e) {
				memcpy(j->tip, out, n);
				j->tip[n] = '\0';
			}
		}
	}
	free(out);
	out = nullptr;
	return e;
}

static int
journal_checkpoint (struct journal *j)
{
	int e = journal_tip(j);
	if (e || !*j->tip)
		return e;
	char name[sizeof j->tip + sizeof ":checkpoint"];
	int n = snprintf(name, sizeof name, "%s:checkpoint", j->tip);
	if (n < 0 || n >= (int)sizeof name)
		return EOVERFLOW;
	char const *command[] = { "cat-file", "blob", name, nullptr };
	char *out = nullptr;
	e = journal_git(j, command, -1, &out, 32, nullptr);
	uint64_t value = 0;
	if (!e) {
		size_t i = 0;
		for (; out[i] >= '0' && out[i] <= '9'; ++i) {
			unsigned digit = (unsigned)(out[i] - '0');
			if (value > (UINT64_MAX - digit) / 10) {
				e = EOVERFLOW;
				break;
			}
			value = value * 10 + digit;
		}
		if (!i || out[i] != '\n' || out[i + 1])
			e = EILSEQ;
	}
	if (!e)
		j->committed = value;
	free(out);
	out = nullptr;
	return e;
}

static int
journal_commit (struct journal *j, struct journal_record *records)
{
	if (!records)
		return 0;
	int fd = memfd_create("askai-journal-import", MFD_CLOEXEC);
	if (fd < 0)
		return errno;
	FILE *file = fdopen(fd, "w+");
	if (!file) {
		int e = errno;
		close(fd);
		return e;
	}
	uint64_t last = 0;
	for (struct journal_record *r = records; r; r = r->next) {
		fprintf(file, "blob\nmark :%" PRIu64 "\ndata %zu\n", r->sequence, r->len);
		if (r->len)
			(void)fwrite(r->data, 1, r->len, file);
		fputc('\n', file);
		last = r->sequence;
	}
	time_t now = time(nullptr);
	int e = now < 0 ? EIO : 0;
	fprintf(file, "commit %s\ncommitter AskAI <askai@localhost> %" PRIdMAX
	        " +0000\ndata 18\nRecord transcript\n\n", j->ref, (intmax_t)now);
	if (*j->tip)
		fprintf(file, "from %s\n", j->tip);
	for (struct journal_record *r = records; r; r = r->next) {
		fprintf(file, "M 100644 :%" PRIu64 " records/%020" PRIu64 "-%s-%s\n"
		        "M 100644 :%" PRIu64 " jobs/%s/%s\n", r->sequence, r->sequence,
		        r->id, r->kind, r->sequence, r->id, r->kind);
	}
	char checkpoint[32];
	int n = snprintf(checkpoint, sizeof checkpoint, "%" PRIu64 "\n", last);
	if (n < 0 || n >= (int)sizeof checkpoint)
		e = EOVERFLOW;
	if (!e)
		fprintf(file, "M 100644 inline checkpoint\ndata %d\n%s\n\ndone\n", n, checkpoint);
	if (fflush(file) || ferror(file))
		e = e ? e : EIO;
	if (!e && lseek(fd, 0, SEEK_SET) < 0)
		e = errno;
	char const *command[] = { "fast-import", "--quiet", "--done", nullptr };
	if (!e)
		e = journal_git(j, command, fd, nullptr, 0, nullptr);
	if (fclose(file) && !e)
		e = errno;
	file = nullptr;
	fd = -1;
	if (!e)
		e = journal_tip(j);
	if (!e)
		j->committed = last;
	return e;
}

void
journal_records_destroy (struct journal_record **p_records)
{
	if (!p_records)
		return;
	while (*p_records) {
		struct journal_record *record = *p_records;
		*p_records = record->next;
		free(record);
		record = nullptr;
	}
}

/* Reads exactly one framed record.  Incomplete final records may be
 * truncated at startup; a complete record with a bad digest never is. */
static int
journal_record_read (struct journal *j, off_t *offset,
                     struct journal_record **record, bool repair)
{
	unsigned char header[JOURNAL_HEADER];
	size_t got = 0;
	int e = journal_read(j->wal_fd, header, sizeof header, *offset, &got);
	*record = nullptr;
	if (e || !got)
		return e;
	if (got != sizeof header)
		goto incomplete;
	if (memcmp(header, "ASKAIJ1\n", 8))
		return EILSEQ;
	uint64_t length = journal_value(header + 16);
	if (length > JOURNAL_RECORD_MAX)
		return EILSEQ;
	struct journal_record *r = calloc(1, sizeof *r + (size_t)length + 1);
	if (!r)
		return ENOMEM;
	r->sequence = journal_value(header + 8);
	r->len = (size_t)length;
	memcpy(r->id, header + 24, JOURNAL_ID);
	memcpy(r->kind, header + 56, JOURNAL_KIND);
	if (!journal_id(r->id) || !journal_word(r->kind, JOURNAL_KIND)) {
		journal_records_destroy(&r);
		return EILSEQ;
	}
	e = journal_read(j->wal_fd, r->data, r->len, *offset + JOURNAL_HEADER, &got);
	if (e || got != r->len) {
		journal_records_destroy(&r);
		if (e)
			return e;
		goto incomplete;
	}
	unsigned char digest[32];
	e = journal_digest(header, r->data, r->len, digest);
	if (!e && memcmp(digest, header + JOURNAL_SIGNED, sizeof digest))
		e = EILSEQ;
	if (e) {
		journal_records_destroy(&r);
		return e;
	}
	*offset += JOURNAL_HEADER + (off_t)r->len;
	*record = r;
	return 0;

incomplete:
	if (!repair)
		return EILSEQ;
	if (ftruncate(j->wal_fd, *offset) || fdatasync(j->wal_fd))
		return errno;
	return 0;
}

static int
journal_recover (struct journal *j)
{
	int e = journal_checkpoint(j);
	off_t offset = 0;
	uint64_t sequence = 0;
	/* Validate all existing data before making any recovery commits. */
	while (!e) {
		struct journal_record *r = nullptr;
		e = journal_record_read(j, &offset, &r, true);
		if (e || !r)
			break;
		if (sequence == UINT64_MAX || r->sequence != sequence + 1)
			e = EILSEQ;
		sequence = r->sequence;
		journal_records_destroy(&r);
	}
	if (!e && j->committed > sequence)
		e = EILSEQ;
	j->sequence = sequence;
	offset = 0;
	struct journal_record *batch = nullptr, **tail = &batch;
	size_t bytes = 0;
	while (!e) {
		struct journal_record *r = nullptr;
		e = journal_record_read(j, &offset, &r, false);
		if (e || !r)
			break;
		if (r->sequence <= j->committed) {
			journal_records_destroy(&r);
			continue;
		}
		*tail = r;
		tail = &r->next;
		bytes += sizeof *r + r->len;
		if (bytes >= JOURNAL_BATCH) {
			e = journal_commit(j, batch);
			journal_records_destroy(&batch);
			tail = &batch;
			bytes = 0;
		}
	}
	if (!e)
		e = journal_commit(j, batch);
	journal_records_destroy(&batch);
	if (!e && lseek(j->wal_fd, 0, SEEK_END) < 0)
		e = errno;
	return e;
}

static int
journal_append (struct journal *j, struct journal_record *r)
{
	if (j->sequence == UINT64_MAX)
		return EOVERFLOW;
	r->sequence = ++j->sequence;
	unsigned char header[JOURNAL_HEADER] = {0};
	memcpy(header, "ASKAIJ1\n", 8);
	journal_number(header + 8, r->sequence);
	journal_number(header + 16, r->len);
	memcpy(header + 24, r->id, JOURNAL_ID);
	memcpy(header + 56, r->kind, strlen(r->kind));
	int e = journal_digest(header, r->data, r->len, header + JOURNAL_SIGNED);
	if (!e)
		e = journal_write(j->wal_fd, header, sizeof header);
	if (!e)
		e = journal_write(j->wal_fd, r->data, r->len);
	return e;
}

static int
journal_load_records (struct journal *j, struct journal_item *item)
{
	off_t offset = 0;
	size_t bytes = 0;
	bool found = !item->current;
	bool overflow = false;
	struct journal_record **tail = &item->record;
	for (;;) {
		struct journal_record *r = nullptr;
		int e = journal_record_read(j, &offset, &r, false);
		if (e || !r)
			return e ? e : overflow ? EFBIG : 0;
		if ((*item->id && strcmp(item->id, r->id)) ||
		    (*item->kind && strcmp(item->kind, r->kind))) {
			journal_records_destroy(&r);
			continue;
		}
		if (item->current && !strcmp(r->kind, "state")) {
			journal_records_destroy(&item->record);
			tail = &item->record;
			bytes = 0;
			found = true;
			overflow = false;
		}
		if (!found || overflow) {
			journal_records_destroy(&r);
			continue;
		}
		size_t size = sizeof *r + r->len + 1;
		if (size > JOURNAL_LOAD_MAX - bytes) {
			journal_records_destroy(&r);
			if (!item->current)
				return EFBIG;
			/* A later state can supersede this oversized old turn.  Keep
			 * validating the log without retaining its data meanwhile. */
			journal_records_destroy(&item->record);
			tail = &item->record;
			overflow = true;
			continue;
		}
		bytes += size;
		*tail = r;
		tail = &r->next;
	}
}

static void
journal_item_destroy (struct journal_item **p_item)
{
	struct journal_item *item = *p_item;
	*p_item = nullptr;
	journal_records_destroy(&item->record);
	free(item);
	item = nullptr;
}

static void
journal_complete (struct loop *loop, struct msg *msg)
{
	struct journal_item *item = container_of(msg, struct journal_item, msg);
	if (item->loaded) {
		struct journal_record *records = item->record;
		item->record = nullptr;
		item->loaded(loop, item->arg, item->error, records);
	} else if (item->done) {
		item->done(loop, item->arg, item->error);
	}
	journal_item_destroy(&item);
}

static void
journal_finish (struct journal_item *item, int error)
{
	item->error = error;
	if (item->loop)
		loop_post(item->loop, &item->msg);
	else
		journal_item_destroy(&item);
}

static struct journal_item *
journal_take (struct journal *j)
{
	struct journal_item *item = j->first;
	if (!item)
		return nullptr;
	j->first = item->next;
	if (!j->first)
		j->last = nullptr;
	item->next = nullptr;
	--j->queued;
	j->bytes -= item->bytes;
	return item;
}

static void *
journal_thread (void *arg)
{
	struct journal *j = arg;
	(void)pthread_setname_np(pthread_self(), "journal");
	for (;;) {
		pthread_mutex_lock(&j->lock);
		while (!j->first && !j->stop)
			pthread_cond_wait(&j->ready, &j->lock);
		struct journal_item *batch = journal_take(j);
		if (!batch) {
			pthread_mutex_unlock(&j->lock);
			break;
		}
		struct journal_item *last = batch;
		size_t bytes = batch->bytes;
		while (!batch->loaded && j->first && !j->first->loaded && bytes < JOURNAL_BATCH) {
			last->next = journal_take(j);
			last = last->next;
			bytes += last->bytes;
		}
		pthread_mutex_unlock(&j->lock);
		int e = atomic_load(&j->error);
		if (batch->loaded) {
			if (!e)
				e = journal_load_records(j, batch);
			if (e && e != EFBIG && e != ENOMEM)
				atomic_store(&j->error, e);
			journal_finish(batch, e);
			continue;
		}
		struct journal_record *records = nullptr, **tail = &records;
		for (struct journal_item *item = batch; !e && item; item = item->next) {
			e = journal_append(j, item->record);
			*tail = item->record;
			tail = &item->record->next;
		}
		if (!e && fdatasync(j->wal_fd))
			e = errno;
		if (!e)
			e = journal_commit(j, records);
		if (e)
			atomic_store(&j->error, e);
		while (batch) {
			struct journal_item *item = batch;
			batch = item->next;
			item->next = nullptr;
			item->record->next = nullptr;
			journal_finish(item, e);
		}
	}
	return nullptr;
}

static int
journal_enqueue (struct journal *j, struct journal_item *item)
{
	int e = atomic_load(&j->error);
	if (e)
		return e;
	/* One small cancellation per ledger job must fit even when ordinary
	 * transcript traffic saturates its queue.  These extra bounds remain
	 * finite and cannot be used by streams, loads, or oversized records. */
	bool control = item->record && item->record->len <= JOURNAL_CONTROL_RECORD &&
	               !strcmp(item->record->kind, "cancel");
	size_t maximum = JOURNAL_QUEUE + (control ? JOURNAL_CONTROL : 0);
	size_t bytes = JOURNAL_BYTES + (control ? JOURNAL_CONTROL_BYTES : 0);
	pthread_mutex_lock(&j->lock);
	if (j->stop)
		e = ECANCELED;
	else if (j->queued >= maximum || j->bytes > bytes || item->bytes > bytes - j->bytes)
		e = EAGAIN;
	else {
		if (j->last)
			j->last->next = item;
		else
			j->first = item;
		j->last = item;
		++j->queued;
		j->bytes += item->bytes;
	}
	pthread_mutex_unlock(&j->lock);
	if (!e)
		pthread_cond_signal(&j->ready);
	return e;
}

int
journal_put (struct journal *j, char const *id, char const *kind,
             void const *data, size_t len, struct loop *loop,
             journal_done_cb *done, void *arg)
{
	if (!j)
		return 0;
	if (!journal_id(id) || !journal_word(kind, JOURNAL_KIND) ||
	    (len && !data) || (!!loop != !!done))
		return EINVAL;
	if (len > JOURNAL_RECORD_MAX)
		return EFBIG;
	struct journal_item *item = calloc(1, sizeof *item);
	if (!item)
		return ENOMEM;
	item->record = calloc(1, sizeof *item->record + len + 1);
	if (!item->record) {
		journal_item_destroy(&item);
		return ENOMEM;
	}
	item->msg.fn = journal_complete;
	item->loop = loop;
	item->done = done;
	item->arg = arg;
	item->bytes = sizeof *item + sizeof *item->record + len + 1;
	item->record->len = len;
	memcpy(item->record->id, id, JOURNAL_ID + 1);
	memcpy(item->record->kind, kind, strlen(kind) + 1);
	if (len)
		memcpy(item->record->data, data, len);
	int e = journal_enqueue(j, item);
	if (e)
		journal_item_destroy(&item);
	return e;
}

static int
journal_load_submit (struct journal *j, char const *id, char const *kind,
                     struct loop *loop, journal_load_cb *done, void *arg,
                     bool current)
{
	if (!j || !loop || !done || (id && !journal_id(id)) ||
	    (kind && !journal_word(kind, JOURNAL_KIND)))
		return EINVAL;
	struct journal_item *item = calloc(1, sizeof *item);
	if (!item)
		return ENOMEM;
	item->msg.fn = journal_complete;
	item->loop = loop;
	item->loaded = done;
	item->arg = arg;
	item->current = current;
	item->bytes = sizeof *item;
	if (id)
		memcpy(item->id, id, JOURNAL_ID + 1);
	if (kind)
		memcpy(item->kind, kind, strlen(kind) + 1);
	int e = journal_enqueue(j, item);
	if (e)
		journal_item_destroy(&item);
	return e;
}

int
journal_load (struct journal *j, char const *id, char const *kind,
              struct loop *loop, journal_load_cb *done, void *arg)
{
	return journal_load_submit(j, id, kind, loop, done, arg, false);
}

int
journal_load_current (struct journal *j, char const *id,
                      struct loop *loop, journal_load_cb *done, void *arg)
{
	if (!id)
		return EINVAL;
	return journal_load_submit(j, id, nullptr, loop, done, arg, true);
}

static int
journal_unlock (struct journal *j, char const *participant)
{
	/* Our participant lock excludes both other writers and their surviving
	 * Git children.  Only this participant's loose-ref lock is ours. */
	static char const *const dirs[] = { "refs", "heads", "transcripts" };
	int fd = fcntl(j->repo_fd, F_DUPFD_CLOEXEC, 3);
	if (fd < 0)
		return errno;
	int e = 0;
	for (size_t i = 0; i < ARRAY_SIZE(dirs); ++i) {
		int next = openat(fd, dirs[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (next < 0) {
			e = errno == ENOENT ? 0 : errno;
			close(fd);
			return e;
		}
		close(fd);
		fd = next;
	}
	char name[70];
	int n = snprintf(name, sizeof name, "%s.lock", participant);
	if (n < 0 || n >= (int)sizeof name)
		e = EOVERFLOW;
	struct stat st;
	if (!e && fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW))
		e = errno == ENOENT ? 0 : errno;
	else if (!e) {
		if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1)
			e = EACCES;
		else if (unlinkat(fd, name, 0) || fsync(fd))
			e = errno;
	}
	close(fd);
	return e;
}

static int
journal_setup (struct journal *j, char const *path, char const *participant)
{
	int e = journal_open_path(j, path);
	int init_fd = -1;
	int state = -1;
	if (!e)
		e = journal_file(j->repo_fd, "askai-journal.lock", &init_fd);
	if (!e && flock(init_fd, LOCK_EX))
		e = errno;
	struct stat st;
	if (!e && fstatat(j->repo_fd, "HEAD", &st, AT_SYMLINK_NOFOLLOW)) {
		if (errno != ENOENT)
			e = errno;
		else {
			char const *command[] = { "init", "--bare", "--quiet", "--template=",
			                          "--initial-branch=transcripts", ".", nullptr };
			e = journal_git(j, command, -1, nullptr, 0, nullptr);
		}
	} else if (!e && !S_ISREG(st.st_mode)) {
		e = EINVAL;
	}
	if (!e) {
		char const *command[] = { "rev-parse", "--is-bare-repository", nullptr };
		char *out = nullptr;
		e = journal_git(j, command, -1, &out, 16, nullptr);
		if (!e && strcmp(out, "true\n"))
			e = EINVAL;
		free(out);
		out = nullptr;
	}
	if (!e)
		e = journal_directory(j->repo_fd, "askai-journal", &state);
	if (!e)
		e = journal_private(state);
	if (!e)
		e = journal_directory(state, participant, &j->state_fd);
	if (!e)
		e = journal_private(j->state_fd);
	if (!e)
		e = journal_file(j->state_fd, "writer.lock", &j->lock_fd);
	if (!e && flock(j->lock_fd, LOCK_EX | LOCK_NB))
		e = errno;
	if (!e)
		e = journal_file(j->state_fd, "records.wal", &j->wal_fd);
	int seed_fd = -1;
	if (!e)
		e = journal_file(j->state_fd, "owner.seed", &seed_fd);
	if (!e && fstat(seed_fd, &st))
		e = errno;
	if (!e && !st.st_size) {
		ssize_t n;
		do {
			do {
				n = getrandom(&j->seed, sizeof j->seed, 0);
			} while (n < 0 && errno == EINTR);
		} while (n == (ssize_t)sizeof j->seed && !j->seed);
		if (n != (ssize_t)sizeof j->seed)
			e = n < 0 ? errno : EIO;
		if (!e)
			e = journal_write(seed_fd, &j->seed, sizeof j->seed);
		if (!e && fdatasync(seed_fd))
			e = errno;
	} else if (!e) {
		if (st.st_size != (off_t)sizeof j->seed)
			e = EILSEQ;
		size_t n = 0;
		if (!e)
			e = journal_read(seed_fd, &j->seed, sizeof j->seed, 0, &n);
		if (!e && n != sizeof j->seed)
			e = EIO;
		if (!e && !j->seed)
			e = EILSEQ;
	}
	if (!e && (fsync(j->state_fd) || fsync(j->repo_fd)))
		e = errno;
	if (seed_fd >= 0)
		close(seed_fd);
	if (state >= 0)
		close(state);
	if (init_fd >= 0) {
		(void)flock(init_fd, LOCK_UN);
		close(init_fd);
	}
	if (!e)
		e = journal_unlock(j, participant);
	return e ? e : journal_recover(j);
}

struct journal *
journal_create (char const *path, char const *participant,
                char *why, size_t why_len, int *e)
{
	*e = 0;
	if (!journal_word(participant, 64)) {
		*e = EINVAL;
		util_format(why, why_len, "invalid transcript participant");
		return nullptr;
	}
	struct journal *j = calloc(1, sizeof *j);
	if (!j) {
		*e = ENOMEM;
		util_format(why, why_len, "%s", strerror(*e));
		return nullptr;
	}
	atomic_init(&j->error, 0);
	j->repo_fd = j->state_fd = j->lock_fd = j->wal_fd = -1;
	j->ref = util_concat("refs/heads/transcripts/", sizeof "refs/heads/transcripts/" - 1,
	                     participant);
	if (!j->ref)
		*e = ENOMEM;
	if (!*e) {
		*e = pthread_mutex_init(&j->lock, nullptr);
		j->mutex_ready = !*e;
	}
	if (!*e) {
		*e = pthread_cond_init(&j->ready, nullptr);
		j->cond_ready = !*e;
	}
	if (!*e)
		*e = journal_setup(j, path, participant);
	if (!*e) {
		/* Creation precedes brainstem_run(), which routes asynchronous
		 * signals through signalfd.  Inherit its worker mask at creation,
		 * never leaving a window for SIGTERM to choose this thread. */
		static int const faults[] = {
			SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGSYS, SIGTRAP
		};
		sigset_t all, old;
		sigfillset(&all);
		for (size_t i = 0; i < ARRAY_SIZE(faults); ++i)
			sigdelset(&all, faults[i]);
		*e = pthread_sigmask(SIG_BLOCK, &all, &old);
		if (!*e) {
			*e = pthread_create(&j->thread, nullptr, journal_thread, j);
			j->started = !*e;
			int restore = pthread_sigmask(SIG_SETMASK, &old, nullptr);
			if (!*e)
				*e = restore;
		}
	}
	if (*e) {
		util_format(why, why_len, "%s", strerror(*e));
		journal_destroy(&j);
	}
	return j;
}

void
journal_stop (struct journal *j)
{
	if (!j || !j->started)
		return;
	pthread_mutex_lock(&j->lock);
	j->stop = true;
	pthread_mutex_unlock(&j->lock);
	pthread_cond_signal(&j->ready);
	(void)pthread_join(j->thread, nullptr);
	j->started = false;
}

void
journal_destroy (struct journal **p_journal)
{
	if (!p_journal || !*p_journal)
		return;
	struct journal *j = *p_journal;
	*p_journal = nullptr;
	journal_stop(j);
	if (j->wal_fd >= 0)
		close(j->wal_fd);
	j->wal_fd = -1;
	if (j->lock_fd >= 0)
		close(j->lock_fd);
	j->lock_fd = -1;
	if (j->state_fd >= 0)
		close(j->state_fd);
	j->state_fd = -1;
	if (j->repo_fd >= 0)
		close(j->repo_fd);
	j->repo_fd = -1;
	if (j->cond_ready)
		pthread_cond_destroy(&j->ready);
	if (j->mutex_ready)
		pthread_mutex_destroy(&j->lock);
	free(j->ref);
	j->ref = nullptr;
	free(j->path);
	j->path = nullptr;
	free(j);
	j = nullptr;
}

int
journal_error (struct journal const *j)
{
	return j ? atomic_load(&j->error) : 0;
}

uint64_t
journal_seed (struct journal const *j)
{
	return j ? j->seed : 0;
}

char const *
journal_path (struct journal const *j)
{
	return j ? j->path : nullptr;
}
