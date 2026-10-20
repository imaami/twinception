/** @file The durable transcript writer (journal.h). */
#define _GNU_SOURCE

#include <dirent.h>
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
	JOURNAL_QUEUE          = 256,
	JOURNAL_BYTES          = 32 << 20,
	JOURNAL_CONTROL        = 1024,
	JOURNAL_CONTROL_BYTES  = 2 << 20,
	JOURNAL_CONTROL_RECORD = 1024,
	JOURNAL_JOBS           = 1024,
	JOURNAL_RECOVERY_BYTES = 256 << 20,
	JOURNAL_FILES          = 16,
	JOURNAL_SCRATCH        = 64 << 10,
	JOURNAL_HEADER         = 120,
	JOURNAL_SIGNED         = 88,
	JOURNAL_TERMINAL_BYTES = JOURNAL_RECORD_MAX + JOURNAL_HEADER,
	JOURNAL_GIT_MS         = 10000,
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
	uint64_t               offset;
	uint32_t               turn;
	uint8_t                operation;
	char                   id[JOURNAL_ID + 1];
	char                   kind[JOURNAL_KIND + 1];
};

static_assert((sizeof(struct journal_item) + sizeof(struct journal_record) +
               JOURNAL_CONTROL_RECORD + 1) * JOURNAL_CONTROL <= JOURNAL_CONTROL_BYTES);

enum {
	JOURNAL_CONTROL_PUT,
	JOURNAL_THOUGHT_PUT,
	JOURNAL_THOUGHT_END,
	JOURNAL_THOUGHT_FINISH,
};

struct journal_job {
	uint64_t bytes; /* zero until a recovery checkpoint exists */
	uint64_t touched;
	uint64_t thought_seen;
	uint32_t thought_turn;
	int      thought_error;
	char     id[JOURNAL_ID + 1];
	bool     terminal;
	bool     legacy;
};

struct journal_file {
	uint64_t touched;
	uint64_t size;
	uint64_t committed_size;
	uint32_t turn;
	int      fd;
	char     id[JOURNAL_ID + 1];
	bool     committed;
};

struct journal {
	pthread_mutex_t        lock;
	pthread_cond_t         ready;
	pthread_t              thread;
	struct journal_item   *first;
	struct journal_item   *last;
	char                  *path;
	char                  *ref;
	uint64_t               seed;
	uint64_t               clock;
	uint64_t               recovery_bytes;
	size_t                 n_jobs;
	struct journal_job     jobs[JOURNAL_JOBS];
	struct journal_file    files[JOURNAL_FILES];
	EVP_MD_CTX            *digest;
	unsigned char          scratch[JOURNAL_SCRATCH];
	size_t                 queued;
	size_t                 bytes;
	_Atomic int            error;
	int                    repo_fd;
	int                    state_fd;
	int                    jobs_fd;
	int                    thoughts_fd;
	int                    lock_fd;
	int                    wal_fd;
	bool                   mutex_ready;
	bool                   cond_ready;
	bool                   started;
	bool                   stop;
	char                   tip[65];
};

static bool
journal_word (char const *word,
              size_t      max)
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
journal_write (int         fd,
               void const *data,
               size_t      len)
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

/** A short read is EOF, not success disguised as a complete record. */
static int
journal_read (int     fd,
              void   *data,
              size_t  len,
              off_t   at,
              size_t *got)
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
journal_number (unsigned char *to,
                uint64_t       number)
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
journal_digest (struct journal      *j,
                unsigned char const *header,
                void const          *data,
                size_t               len,
                unsigned char        digest[32])
{
	unsigned n = 0;
	bool ok = EVP_DigestInit_ex(j->digest, EVP_sha256(), nullptr) == 1 &&
	          EVP_DigestUpdate(j->digest, header, JOURNAL_SIGNED) == 1 &&
	          EVP_DigestUpdate(j->digest, data, len) == 1 &&
	          EVP_DigestFinal_ex(j->digest, digest, &n) == 1 && n == 32;
	return ok ? 0 : EIO;
}

static int
journal_directory (int         parent,
                   char const *name,
                   int        *fd)
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

/** Walk, opening each component without following links. */
static int
journal_open_path (struct journal *j,
                   char const     *path)
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
journal_file_open (int         dir,
                   char const *name,
                   int         flags,
                   int        *fd)
{
	*fd = openat(dir, name, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
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
journal_file (int         dir,
              char const *name,
              int        *fd)
{
	return journal_file_open(dir, name, O_RDWR | O_CREAT, fd);
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

/** Fixed argv and environment.  Input/output are anonymous files, so a
 * child's output can never deadlock its input pipe.  Only the writer and
 * startup call this function.
 */
static int
journal_git (struct journal    *j,
             char const *const *command,
             int                input,
             char             **output,
             size_t             maximum,
             int               *status)
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
		                                   POSIX_SPAWN_SETSIGDEF |
		                                   POSIX_SPAWN_SETPGROUP);
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

/** Reads one framed record. Legacy WALs may end with an incomplete write;
 * their intact prefix is usable, but they are never changed by this reader.
 */
static int
journal_record_read (struct journal         *j,
                     int                     fd,
                     off_t                  *offset,
                     struct journal_record **record,
                     bool                    partial)
{
	unsigned char header[JOURNAL_HEADER];
	size_t got = 0;
	int e = journal_read(fd, header, sizeof header, *offset, &got);
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
	e = journal_read(fd, r->data, r->len, *offset + JOURNAL_HEADER, &got);
	if (e || got != r->len) {
		journal_records_destroy(&r);
		if (e)
			return e;
		goto incomplete;
	}
	unsigned char digest[32];
	e = journal_digest(j, header, r->data, r->len, digest);
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
	return partial ? 0 : EILSEQ;
}

static bool
journal_control_kind (char const *kind)
{
	return !strcmp(kind, "state") || !strcmp(kind, "meta") ||
	       !strcmp(kind, "tool-intent") || !strcmp(kind, "tool-result") ||
	       !strcmp(kind, "result") || !strcmp(kind, "cancel") ||
	       !strcmp(kind, "legacy");
}

static int
journal_existing (int         directory,
                  char const *name,
                  int        *fd)
{
	return journal_file_open(directory, name, O_RDONLY, fd);
}

/** New snapshots are atomic replacements; legacy WAL tails may end with an
 * interrupted write. Neither loading nor migration modifies that WAL.
 */
static int
journal_records_read (struct journal         *j,
                      int                     fd,
                      char const             *id,
                      char const             *kind,
                      bool                    legacy,
                      struct journal_record **out)
{
	off_t offset = 0;
	uint64_t sequence = 0;
	size_t bytes = 0;
	struct journal_record **tail = out;
	while (*tail) {
		bytes += sizeof **tail + (*tail)->len + 1;
		tail = &(*tail)->next;
	}
	for (;;) {
		struct journal_record *r = nullptr;
		int e = journal_record_read(j, fd, &offset, &r, legacy);
		if (e || !r)
			return e;
		if (sequence == UINT64_MAX || r->sequence != sequence + 1) {
			journal_records_destroy(&r);
			return EILSEQ;
		}
		sequence = r->sequence;
		if ((id && strcmp(r->id, id)) || !journal_control_kind(r->kind)) {
			journal_records_destroy(&r);
			continue;
		}
		if (legacy && !strcmp(r->kind, "state")) {
			journal_records_destroy(out);
			tail = out;
			bytes = 0;
		}
		if (legacy && !strcmp(r->kind, "meta")) {
			struct journal_record **at = out;
			while (*at) {
				struct journal_record *old = *at;
				if (!strcmp(old->kind, "meta")) {
					*at = old->next;
					if (!*at)
						tail = at;
					bytes -= sizeof *old + old->len + 1;
					old->next = nullptr;
					journal_records_destroy(&old);
				} else {
					at = &old->next;
				}
			}
		}
		if (kind && strcmp(r->kind, kind)) {
			journal_records_destroy(&r);
			continue;
		}
		size_t size = sizeof *r + r->len + 1;
		if (size > JOURNAL_LOAD_MAX - bytes) {
			journal_records_destroy(&r);
			return EFBIG;
		}
		bytes += size;
		*tail = r;
		tail = &r->next;
	}
}

static struct journal_job *
journal_job_find (struct journal *j,
                  char const     *id)
{
	for (size_t k = 0; k < ARRAY_SIZE(j->jobs); ++k)
		if (!strcmp(j->jobs[k].id, id))
			return &j->jobs[k];
	return nullptr;
}

static int
journal_jobs_scan (struct journal *j)
{
	int fd = fcntl(j->jobs_fd, F_DUPFD_CLOEXEC, 3);
	if (fd < 0)
		return errno;
	DIR *directory = fdopendir(fd);
	if (!directory) {
		int e = errno;
		close(fd);
		return e;
	}
	int e = 0;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(directory);
		if (!entry) {
			e = errno;
			break;
		}
		if (!journal_id(entry->d_name))
			continue;
		if (j->n_jobs == JOURNAL_JOBS) {
			e = EFBIG;
			break;
		}
		int input = -1;
		e = journal_existing(j->jobs_fd, entry->d_name, &input);
		struct stat st;
		if (!e && fstat(input, &st))
			e = errno;
		if (!e && (st.st_size < 0 || (uint64_t)st.st_size > JOURNAL_LOAD_MAX ||
		           (uint64_t)st.st_size >
		           JOURNAL_RECOVERY_BYTES + JOURNAL_TERMINAL_BYTES - j->recovery_bytes))
			e = EFBIG;
		struct journal_record *records = nullptr;
		if (!e)
			e = journal_records_read(j, input, entry->d_name, nullptr, false, &records);
		if (input >= 0)
			close(input);
		if (!e) {
			struct journal_job *job = &j->jobs[j->n_jobs++];
			memcpy(job->id, entry->d_name, sizeof job->id);
			job->bytes = (uint64_t)st.st_size;
			job->touched = st.st_mtim.tv_sec < 0 ? 0 :
			               (uint64_t)st.st_mtim.tv_sec * 1'000'000'000 +
			               (uint64_t)st.st_mtim.tv_nsec;
			for (struct journal_record *r = records; r; r = r->next) {
				job->terminal |= !strcmp(r->kind, "result") || !strcmp(r->kind, "cancel");
				job->legacy |= !strcmp(r->kind, "legacy");
			}
			j->recovery_bytes += job->bytes;
		}
		journal_records_destroy(&records);
		if (e)
			break;
	}
	if (closedir(directory) && !e)
		e = errno;
	return e;
}

/** Only completed jobs may expire. Active checkpoints never disappear to
 * make room for another job, even if that means refusing the new write.
 */
static int
journal_checkpoint_space (struct journal     *j,
                          struct journal_job *keep,
                          uint64_t            bytes,
                          bool                terminal)
{
	uint64_t previous = keep ? keep->bytes : 0;
	uint64_t limit = JOURNAL_RECOVERY_BYTES + (terminal ? JOURNAL_TERMINAL_BYTES : 0);
	while (j->recovery_bytes - previous > limit ||
	       bytes > limit - (j->recovery_bytes - previous) ||
	       (!keep && j->n_jobs == JOURNAL_JOBS)) {
		struct journal_job *oldest = nullptr;
		for (size_t k = 0; k < ARRAY_SIZE(j->jobs); ++k) {
			struct journal_job *job = &j->jobs[k];
			if (job != keep && *job->id && job->terminal && !job->legacy &&
			    (!oldest || job->touched < oldest->touched))
				oldest = job;
		}
		if (!oldest)
			return ENOSPC;
		if (oldest->bytes &&
		    (unlinkat(j->jobs_fd, oldest->id, 0) || fsync(j->jobs_fd)))
			return errno;
		j->recovery_bytes -= oldest->bytes;
		--j->n_jobs;
		*oldest = (struct journal_job){};
	}
	return 0;
}

static struct journal_job *
journal_job_enter (struct journal *j,
                   char const     *id,
                   int            *e)
{
	struct journal_job *job = journal_job_find(j, id);
	*e = job ? 0 : journal_checkpoint_space(j, nullptr, 0, true);
	if (job || *e)
		return job;
	for (size_t k = 0; k < ARRAY_SIZE(j->jobs); ++k) {
		job = &j->jobs[k];
		if (*job->id)
			continue;
		memcpy(job->id, id, sizeof job->id);
		++j->n_jobs;
		return job;
	}
	*e = ENOSPC;
	return nullptr;
}

static int
journal_record_write (struct journal              *j,
                      int                          fd,
                      struct journal_record const *r,
                      uint64_t                     sequence)
{
	unsigned char header[JOURNAL_HEADER] = {};
	memcpy(header, "ASKAIJ1\n", 8);
	journal_number(header + 8, sequence);
	journal_number(header + 16, r->len);
	memcpy(header + 24, r->id, JOURNAL_ID);
	memcpy(header + 56, r->kind, JOURNAL_KIND);
	int e = journal_digest(j, header, r->data, r->len, header + JOURNAL_SIGNED);
	if (!e)
		e = journal_write(fd, header, sizeof header);
	return e ? e : journal_write(fd, r->data, r->len);
}

static int
journal_checkpoint_write (struct journal        *j,
                          struct journal_record *record)
{
	if (!journal_control_kind(record->kind))
		return !strcmp(record->kind, "stream") || !strcmp(record->kind, "body") ||
		       !strcmp(record->kind, "message") ? 0 : EINVAL;
	struct journal_job *job = journal_job_find(j, record->id);
	bool legacy = job && job->legacy;
	struct journal_record *records = nullptr;
	int e = 0;
	if (strcmp(record->kind, "state")) {
		if (job && job->bytes) {
			int input = -1;
			e = journal_existing(j->jobs_fd, record->id, &input);
			if (!e)
				e = journal_records_read(j, input, record->id, nullptr, false, &records);
			if (input >= 0)
				close(input);
		} else if (j->wal_fd >= 0) {
			e = journal_records_read(j, j->wal_fd, record->id, nullptr, true, &records);
			legacy = records != nullptr;
		}
	}
	if (!e && legacy) {
		bool marked = false;
		for (struct journal_record *r = records; r; r = r->next)
			marked |= !strcmp(r->kind, "legacy");
		if (!marked) {
			struct journal_record *mark = calloc(1, sizeof *mark + 1);
			if (!mark)
				e = ENOMEM;
			else {
				memcpy(mark->id, record->id, sizeof mark->id);
				memcpy(mark->kind, "legacy", sizeof "legacy");
				mark->next = records;
				records = mark;
			}
		}
	}
	/* A completed job needs only its owner/state and result. Drop tool
	 * evidence once the terminal record makes further execution impossible.
	 * A terminal reserve lets even a full active cache retire a job. */
	bool terminal = !strcmp(record->kind, "result") || !strcmp(record->kind, "cancel");
	if (!strcmp(record->kind, "meta") || terminal) {
		struct journal_record **at = &records;
		while (*at) {
			struct journal_record *r = *at;
			if (!strcmp(r->kind, record->kind) ||
			    (terminal && strcmp(r->kind, "state") && strcmp(r->kind, "legacy"))) {
				*at = r->next;
				r->next = nullptr;
				journal_records_destroy(&r);
			} else {
				at = &r->next;
			}
		}
	}
	uint64_t bytes = JOURNAL_HEADER + record->len;
	for (struct journal_record *r = records; r; r = r->next)
		bytes += JOURNAL_HEADER + r->len;
	if (!e && bytes > JOURNAL_LOAD_MAX)
		e = EFBIG;
	if (!e)
		e = journal_checkpoint_space(j, job, bytes, terminal);
	int fd = -1;
	if (!e)
		e = journal_file(j->jobs_fd, ".checkpoint.tmp", &fd);
	if (!e && ftruncate(fd, 0))
		e = errno;
	uint64_t sequence = 0;
	for (struct journal_record *r = records; !e && r; r = r->next)
		e = journal_record_write(j, fd, r, ++sequence);
	if (!e)
		e = journal_record_write(j, fd, record, ++sequence);
	if (!e && fdatasync(fd))
		e = errno;
	if (fd >= 0 && close(fd) && !e)
		e = errno;
	if (!e && (renameat(j->jobs_fd, ".checkpoint.tmp", j->jobs_fd, record->id) ||
	           fsync(j->jobs_fd)))
		e = errno;
	if (!e) {
		if (!job)
			job = journal_job_enter(j, record->id, &e);
		if (job) {
			j->recovery_bytes = j->recovery_bytes - job->bytes + bytes;
			job->bytes = bytes;
			struct timespec now;
			if (!clock_gettime(CLOCK_REALTIME, &now))
				job->touched = (uint64_t)now.tv_sec * 1'000'000'000 + (uint64_t)now.tv_nsec;
			job->legacy = legacy;
			job->terminal = terminal;
			for (struct journal_record *r = records; r; r = r->next)
				job->terminal |= !strcmp(r->kind, "result") || !strcmp(r->kind, "cancel");
			if (!strcmp(record->kind, "state")) {
				job->thought_error = 0;
				job->thought_turn = 0;
				job->thought_seen = 0;
			}
		}
	}
	journal_records_destroy(&records);
	return e;
}

static int
journal_load_records (struct journal      *j,
                      struct journal_item *item)
{
	if (*item->id) {
		int fd = -1;
		int e = journal_existing(j->jobs_fd, item->id, &fd);
		if (!e)
			e = journal_records_read(j, fd, item->id, *item->kind ? item->kind : nullptr,
			                         false, &item->record);
		if (fd >= 0)
			close(fd);
		if (e == ENOENT)
			e = j->wal_fd < 0 ? 0 :
			    journal_records_read(j, j->wal_fd, item->id,
			                         *item->kind ? item->kind : nullptr, true, &item->record);
		return e;
	}
	for (size_t k = 0; k < ARRAY_SIZE(j->jobs); ++k) {
		struct journal_job const *job = &j->jobs[k];
		if (!job->bytes)
			continue;
		int fd = -1;
		int e = journal_existing(j->jobs_fd, job->id, &fd);
		if (!e)
			e = journal_records_read(j, fd, job->id, *item->kind ? item->kind : nullptr,
			                         false, &item->record);
		if (fd >= 0)
			close(fd);
		if (e)
			return e;
	}
	return 0;
}

/** A small descriptor cache keeps the common two-model workload free of
 * pathname traversal per delta, while bounding descriptors for many jobs.
 */
static int
journal_thought_file (struct journal            *j,
                      struct journal_item const *item,
                      struct journal_file      **out)
{
	struct journal_file *oldest = &j->files[0];
	for (size_t k = 0; k < ARRAY_SIZE(j->files); ++k) {
		struct journal_file *file = &j->files[k];
		if (file->fd >= 0 && file->turn == item->turn && !strcmp(file->id, item->id)) {
			file->touched = ++j->clock;
			*out = file;
			return 0;
		}
		if (file->fd < 0 || (oldest->fd >= 0 && file->touched < oldest->touched))
			oldest = file;
	}
	if (oldest->fd >= 0)
		close(oldest->fd);
	oldest->fd = -1;
	int directory = -1;
	bool append = item->operation == JOURNAL_THOUGHT_PUT;
	int e = 0;
	if (append)
		e = journal_directory(j->thoughts_fd, item->id, &directory);
	else {
		directory = openat(j->thoughts_fd, item->id,
		                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (directory < 0)
			e = errno;
	}
	char name[32];
	int n = snprintf(name, sizeof name, "%" PRIu32 ".txt", item->turn);
	if (!e && (n < 0 || n >= (int)sizeof name))
		e = EOVERFLOW;
	if (!e) {
		if (append)
			e = journal_file(directory, name, &oldest->fd);
		else
			e = journal_file_open(directory, name, O_RDWR, &oldest->fd);
	}
	struct stat st;
	if (!e && fstat(oldest->fd, &st))
		e = errno;
	if (!e && st.st_size < 0)
		e = EILSEQ;
	if (!e && fsync(directory))
		e = errno;
	if (directory >= 0)
		close(directory);
	if (e) {
		if (oldest->fd >= 0)
			close(oldest->fd);
		oldest->fd = -1;
		return e;
	}
	oldest->size = (uint64_t)st.st_size;
	oldest->committed = false;
	oldest->committed_size = 0;
	oldest->turn = item->turn;
	oldest->touched = ++j->clock;
	memcpy(oldest->id, item->id, sizeof oldest->id);
	*out = oldest;
	return 0;
}

static int
journal_thought_write (struct journal            *j,
                       struct journal_item const *item)
{
	struct journal_file *file = nullptr;
	int e = journal_thought_file(j, item, &file);
	if (e)
		return e;
	struct journal_record const *record = item->record;
	if (item->offset > file->size || record->len > INT64_MAX - item->offset)
		return EILSEQ;
	uint64_t overlap = file->size - item->offset;
	size_t same = overlap < record->len ? (size_t)overlap : record->len;
	for (size_t at = 0; at < same; ) {
		size_t n = same - at < sizeof j->scratch ? same - at : sizeof j->scratch;
		size_t got;
		e = journal_read(file->fd, j->scratch, n, (off_t)(item->offset + at), &got);
		if (e || got != n || memcmp(j->scratch, record->data + at, n))
			return e ? e : EILSEQ;
		at += n;
	}
	if (same == record->len)
		return 0;
	if (lseek(file->fd, 0, SEEK_END) < 0)
		return errno;
	e = journal_write(file->fd, record->data + same, record->len - same);
	if (!e)
		file->size += record->len - same;
	return e;
}

static int
journal_thought_commit (struct journal            *j,
                        struct journal_item const *item)
{
	struct journal_job *job = journal_job_find(j, item->id);
	if (job && job->thought_turn == item->turn && job->thought_error)
		return job->thought_error;
	struct journal_file *file = nullptr;
	int e = journal_thought_file(j, item, &file);
	if (e || !file->size)
		return e == ENOENT ? 0 : e;
	uint64_t seen = job && job->thought_turn == item->turn ? job->thought_seen : 0;
	if (seen < file->size)
		return item->operation == JOURNAL_THOUGHT_FINISH ? EILSEQ : 0;
	if (file->committed && file->committed_size == file->size)
		return 0;
	if (fdatasync(file->fd) || lseek(file->fd, 0, SEEK_SET) < 0)
		return errno;
	char const *hash[] = { "hash-object", "-w", "--stdin", nullptr };
	char *oid = nullptr;
	e = journal_git(j, hash, file->fd, &oid, 80, nullptr);
	size_t oid_len = oid ? strcspn(oid, "\n") : 0;
	if (!e && ((oid_len != 40 && oid_len != 64) || oid[oid_len] != '\n' || oid[oid_len + 1]))
		e = EILSEQ;
	if (!e)
		oid[oid_len] = '\0';
	char path[96];
	int n = snprintf(path, sizeof path, "reasoning/%.2s/%s/%" PRIu32 ".txt",
	                 item->id, item->id, item->turn);
	if (!e && (n < 0 || n >= (int)sizeof path))
		e = EOVERFLOW;
	if (!e && *j->tip) {
		char name[sizeof j->tip + sizeof path];
		n = snprintf(name, sizeof name, "%s:%s", j->tip, path);
		if (n < 0 || n >= (int)sizeof name)
			e = EOVERFLOW;
		char const *find[] = { "rev-parse", "--verify", "--quiet", name, nullptr };
		char *previous = nullptr;
		int status = 0;
		if (!e)
			e = journal_git(j, find, -1, &previous, 80, &status);
		bool same = !e && !status && !strncmp(previous, oid, oid_len) && previous[oid_len] == '\n';
		free(previous);
		previous = nullptr;
		if (same) {
			file->committed = true;
			file->committed_size = file->size;
			free(oid);
			oid = nullptr;
			return 0;
		}
	}
	int fd = -1;
	if (!e) {
		fd = memfd_create("askai-reasoning-import", MFD_CLOEXEC);
		if (fd < 0)
			e = errno;
	}
	FILE *output = nullptr;
	if (!e && !(output = fdopen(fd, "w+")))
		e = errno;
	if (!e) {
		time_t now = time(nullptr);
		if (now < 0)
			e = EIO;
		fprintf(output, "commit %s\ncommitter AskAI <askai@localhost> %" PRIdMAX
		                " +0000\ndata 17\nRecord reasoning\n\n", j->ref, (intmax_t)now);
		if (*j->tip)
			fprintf(output, "from %s\n", j->tip);
		fprintf(output, "M 100644 %s %s\n\ndone\n", oid, path);
		if (fflush(output) || ferror(output))
			e = e ? e : EIO;
		if (!e && lseek(fd, 0, SEEK_SET) < 0)
			e = errno;
		char const *import[] = { "fast-import", "--quiet", "--done", nullptr };
		if (!e)
			e = journal_git(j, import, fd, nullptr, 0, nullptr);
	}
	if (output) {
		if (fclose(output) && !e)
			e = errno;
		output = nullptr;
		fd = -1;
	}
	if (fd >= 0)
		close(fd);
	free(oid);
	oid = nullptr;
	if (!e)
		e = journal_tip(j);
	if (!e) {
		file->committed = true;
		file->committed_size = file->size;
	}
	return e;
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
journal_complete (struct loop *loop,
                  struct msg  *msg)
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
journal_finish (struct journal_item *item,
                int                  error)
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
		struct journal_item *item = journal_take(j);
		pthread_mutex_unlock(&j->lock);
		if (!item)
			break;
		int e = atomic_load(&j->error);
		if (!e && item->loaded)
			e = journal_load_records(j, item);
		else if (!e && item->operation == JOURNAL_THOUGHT_PUT) {
			struct journal_job *job = journal_job_enter(j, item->id, &e);
			bool begin = !item->offset && !item->record->len;
			if (job && (begin || job->thought_turn != item->turn)) {
				job->thought_turn = item->turn;
				job->thought_seen = 0;
				job->thought_error = 0;
			}
			if (!e)
				e = begin ? 0 : job->thought_error ? job->thought_error :
				    journal_thought_write(j, item);
			if (job) {
				job->thought_error = e;
				if (!job->bytes)
					job->terminal = false;
				if (!e)
					job->thought_seen = item->offset + item->record->len;
			}
		} else if (!e && (item->operation == JOURNAL_THOUGHT_END ||
		                  item->operation == JOURNAL_THOUGHT_FINISH)) {
			e = journal_thought_commit(j, item);
			struct journal_job *job = journal_job_find(j, item->id);
			if (!e && job && !job->bytes && item->operation == JOURNAL_THOUGHT_FINISH)
				job->terminal = true;
		} else if (!e)
			e = journal_checkpoint_write(j, item->record);
		/* Capacity and a mismatching replay concern one job. Disk errors
		 * stop the writer rather than claiming subsequent writes worked. */
		if (e && e != EFBIG && e != ENOMEM && e != ENOSPC && e != EILSEQ)
			atomic_store(&j->error, e);
		journal_finish(item, e);
	}
	return nullptr;
}

static int
journal_enqueue (struct journal      *j,
                 struct journal_item *item)
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

static int
journal_submit (struct journal  *j,
                char const      *id,
                char const      *kind,
                uint32_t         turn,
                uint64_t         offset,
                uint8_t          operation,
                void const      *data,
                size_t           len,
                struct loop     *loop,
                journal_done_cb *done,
                void            *arg)
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
	item->operation = operation;
	item->turn = turn;
	item->offset = offset;
	memcpy(item->id, id, sizeof item->id);
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

int
journal_put (struct journal  *j,
             char const      *id,
             char const      *kind,
             void const      *data,
             size_t           len,
             struct loop     *loop,
             journal_done_cb *done,
             void            *arg)
{
	return journal_submit(j, id, kind, 0, 0, JOURNAL_CONTROL_PUT,
	                      data, len, loop, done, arg);
}

int
journal_thought (struct journal  *j,
                 char const      *id,
                 uint32_t         turn,
                 uint64_t         offset,
                 void const      *data,
                 size_t           len,
                 struct loop     *loop,
                 journal_done_cb *done,
                 void            *arg)
{
	if (!turn)
		return EINVAL;
	return journal_submit(j, id, "thought", turn, offset, JOURNAL_THOUGHT_PUT,
	                      data, len, loop, done, arg);
}

int
journal_thought_end (struct journal  *j,
                     char const      *id,
                     uint32_t         turn,
                     bool             final,
                     struct loop     *loop,
                     journal_done_cb *done,
                     void            *arg)
{
	if (!turn)
		return EINVAL;
	return journal_submit(j, id, "thought-end", turn, 0,
	                      final ? JOURNAL_THOUGHT_FINISH : JOURNAL_THOUGHT_END,
	                      nullptr, 0, loop, done, arg);
}

static int
journal_load_submit (struct journal  *j,
                     char const      *id,
                     char const      *kind,
                     struct loop     *loop,
                     journal_load_cb *done,
                     void            *arg)
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
journal_load (struct journal  *j,
              char const      *id,
              char const      *kind,
              struct loop     *loop,
              journal_load_cb *done,
              void            *arg)
{
	return journal_load_submit(j, id, kind, loop, done, arg);
}

int
journal_load_current (struct journal  *j,
                      char const      *id,
                      struct loop     *loop,
                      journal_load_cb *done,
                      void            *arg)
{
	if (!id)
		return EINVAL;
	return journal_load_submit(j, id, nullptr, loop, done, arg);
}

static int
journal_unlock (struct journal *j,
                char const     *participant)
{
	/* Our participant lock excludes both other writers and their surviving
	 * Git children.  Only this participant's loose-ref lock is ours. */
	static char const *const dirs[] = { "refs", "heads", "reasoning" };
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
journal_setup (struct journal *j,
               char const     *path,
               char const     *participant)
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
			                          "--initial-branch=reasoning", ".", nullptr };
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
		e = journal_directory(j->state_fd, "jobs", &j->jobs_fd);
	if (!e)
		e = journal_directory(j->state_fd, "reasoning", &j->thoughts_fd);
	if (!e) {
		e = journal_existing(j->state_fd, "records.wal", &j->wal_fd);
		if (e == ENOENT)
			e = 0;
	}
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
	if (!e)
		e = journal_tip(j);
	return e ? e : journal_jobs_scan(j);
}

struct journal *
journal_create (char const *path,
                char const *participant,
                char       *why,
                size_t      why_len,
                int        *e)
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
	j->repo_fd = j->state_fd = j->jobs_fd = j->thoughts_fd = j->lock_fd = j->wal_fd = -1;
	for (size_t k = 0; k < ARRAY_SIZE(j->files); ++k)
		j->files[k].fd = -1;
	j->digest = EVP_MD_CTX_new();
	j->ref = util_concat("refs/heads/reasoning/", sizeof "refs/heads/reasoning/" - 1,
	                     participant);
	if (!j->ref || !j->digest)
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
	for (size_t k = 0; k < ARRAY_SIZE(j->files); ++k) {
		if (j->files[k].fd >= 0)
			close(j->files[k].fd);
		j->files[k].fd = -1;
	}
	EVP_MD_CTX_free(j->digest);
	j->digest = nullptr;
	if (j->jobs_fd >= 0)
		close(j->jobs_fd);
	j->jobs_fd = -1;
	if (j->thoughts_fd >= 0)
		close(j->thoughts_fd);
	j->thoughts_fd = -1;
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
