/** @file journal.c
 *
 * The durable transcript writer.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buf.h"
#include "child.h"
#include "clk.h"
#include "git_run.h"
#include "journal.h"
#include "loop.h"
#include "member_name.h"
#include "oid.h"
#include "tally.h"
#include "util.h"
#include "xlist.h"

/* The header of a record on disk: the magic, the sequence number and the
 * length of the data, both big-endian, the job's id and the kind, then the
 * SHA-256 digest of all of that and the data.
 */
#define JOURNAL_MAGIC "ASKAIJ1\n"
enum {
	JOURNAL_AT_SEQUENCE = sizeof JOURNAL_MAGIC - 1,
	JOURNAL_AT_LENGTH   = JOURNAL_AT_SEQUENCE + sizeof (uint64_t),
	JOURNAL_AT_ID       = JOURNAL_AT_LENGTH + sizeof (uint64_t),
	JOURNAL_AT_KIND     = JOURNAL_AT_ID + HEXID_DIGITS,
	JOURNAL_SIGNED      = JOURNAL_AT_KIND + JOURNAL_KIND,
	JOURNAL_DIGEST      = 32,
	JOURNAL_HEADER      = JOURNAL_SIGNED + JOURNAL_DIGEST,
};
static_assert(JOURNAL_HEADER == 120, "the records on disk have 120-byte headers");

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
	JOURNAL_TERMINAL_BYTES = JOURNAL_RECORD_MAX + JOURNAL_HEADER,
};

struct journal_item {
	struct msg             msg;
	struct list            hook;
	struct journal_record *record;
	struct loop           *loop;
	union {
		journal_done_cb   *done;    /* of a write */
		journal_load_cb   *loaded;  /* of a JOURNAL_LOAD */
	};
	void                  *arg;
	size_t                 bytes;
	uint64_t               offset;
	uint32_t               turn;
	int                    error;
	uint8_t                operation;
	struct hexid           id;
};

/* The prefix of the reasoning branch of the service. */
static char const REF[] = "refs/heads/reasoning/";

/* The options of the git processes of the journal, which
 * git_run_dir_argv() writes after the fixed options. With these options,
 * git calls fsync() on every file that it writes to the repository.
 */
static char const *const git_options[] = {
	"-c", "core.fsync=all", "-c", "core.fsyncMethod=fsync", nullptr
};

static_assert((sizeof(struct journal_item) + sizeof(struct journal_record) +
               JOURNAL_CONTROL_RECORD + 1) * JOURNAL_CONTROL <= JOURNAL_CONTROL_BYTES);

enum {
	JOURNAL_CONTROL_PUT,
	JOURNAL_THOUGHT_PUT,
	JOURNAL_THOUGHT_END,
	JOURNAL_THOUGHT_FINISH,
	JOURNAL_LOAD,
};

/* The names of the kinds of records, by enum journal_kind. */
static struct cstr_view const kind_names[] = { JOURNAL_KIND_LIST(XL_VIEW) };

/* The bit of the kind @p k in a mask of kinds, and the mask of the kinds
 * after which a job runs no more.
 */
#define KIND(k) (1u << (k))
enum { TERMINAL = KIND(JOURNAL_RESULT) | KIND(JOURNAL_CANCEL) };

/* The bits of journal_job.flags: the job runs no more, and its records
 * came from the legacy WAL.
 */
enum { JOB_TERMINAL = 1, JOB_LEGACY = 2 };

/* The bits of journal.flags: the writer was started, and was told to stop. */
enum { WRITER_STARTED = 1, WRITER_STOP = 2 };

struct journal_job {
	uint64_t     bytes; /* zero until a recovery checkpoint exists */
	uint64_t     touched;
	struct {
		uint64_t seen;
		uint32_t turn;
		int      error;
	}            thought;
	int          error;   /* the error of a lost record; the job's later
	                         appends fail with it */
	uint16_t     flags;   /* JOB_* bits */
	struct hexid id;
};

struct journal_file {
	uint64_t     touched;
	uint64_t     size;
	uint64_t     committed_size;  /* the size committed; 0: none */
	uint32_t     turn;
	int          fd;
	struct hexid id;
};

struct journal {
	pthread_mutex_t        lock;
	pthread_cond_t         ready;
	pthread_t              thread;
	struct list            items;
	uint64_t               seed;
	uint64_t               clock;
	uint64_t               recovery_bytes;
	size_t                 n_jobs;
	struct journal_job     jobs[JOURNAL_JOBS];
	struct journal_file    files[JOURNAL_FILES];
	EVP_MD_CTX            *digest;
	struct buf             out;       /* the output of the last git, at
	                                     most sizeof scratch bytes */
	struct git_run_dir     git;       /* git runs in repo_fd with
	                                     "--git-dir=." and git_options */
	char                   scratch[JOURNAL_SCRATCH];
	unsigned char          key[POLICY_KEY];  /* the key of the principals,
	                                            read from
	                                            askai-journal/principal.key */
	struct tally           queue;     /* the queued items and their bytes */
	int                    repo_fd;
	int                    state_fd;
	int                    jobs_fd;
	int                    thoughts_fd;
	int                    lock_fd;
	int                    wal_fd;
	uint8_t                flags;     /* WRITER_* bits, set before the writer
	                                     starts or under the lock */
	struct oid             tip;
	char                   ref[sizeof REF + UTIL_WORD_MAX];
};

/* The kind named @p name, or JOURNAL_OTHER for one that is in no
 * checkpoint.
 */
static enum journal_kind
journal_kind (struct cstr_view name)
{
	return (enum journal_kind)cstr_view_index(kind_names, array_size(kind_names), name);
}

static int
journal_digest (struct journal      *j,
                unsigned char const *header,
                void const          *data,
                size_t               len,
                unsigned char        digest[JOURNAL_DIGEST])
{
	unsigned n = 0;
	bool ok = EVP_DigestInit_ex(j->digest, EVP_sha256(), nullptr) == 1 &&
	          EVP_DigestUpdate(j->digest, header, JOURNAL_SIGNED) == 1 &&
	          EVP_DigestUpdate(j->digest, data, len) == 1 &&
	          EVP_DigestFinal_ex(j->digest, digest, &n) == 1 && n == JOURNAL_DIGEST;
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

/* Make sure only askai's user may change the directory open as @p fd,
 * which no one else may read either.
 */
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

/** Walk, opening each component without following links. */
static int
journal_open_path (struct journal *j,
                   char const     *path)
{
	if (!path || !*path)
		return EINVAL;
	int fd = open(*path == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int e = fd < 0 ? errno : 0;
	struct cstr_view rest = cstr_view(path);
	struct cstr_view part = {};
	while (!e && !(e = util_path_next(&rest, &part)) && part.len) {
		UTIL_TEXT(NAME_MAX + 1) name;
		int next = -1;
		e = util_text_store(&name, part);
		if (!e)
			e = journal_directory(fd, name.data, &next);
		close(fd);
		fd = next;
	}
	if (!e)
		e = journal_private(fd);
	if (e) {
		util_close(&fd);
		return e;
	}
	j->repo_fd = fd;
	return 0;
}

/** Runs the git command @p command in the repository. The standard input
 * of git is the file open as @p input, or none if @p input is -1. git
 * writes the output to a file, which child_capture() reads into j->out
 * after git ends, so that git never waits on a full pipe. If @p oid is not
 * nullptr, the function reads the name of an object that git prints into
 * @p oid. Returns what git_run_now() returns. Only the writer and startup
 * call this function.
 */
static int
journal_git (struct journal    *j,
             char const *const *command,
             int                input,
             struct oid        *oid)
{
	struct child_cmd const how = child_cmd(.max = sizeof j->scratch, .grace = GIT_GRACE);
	/* A git child can outlive an askai that was killed. fds.keep passes
	 * the descriptor of the writer lock to git through exec, so the
	 * flock() of a new askai on the writer lock fails until git ends. */
	struct child_fds const fds = { .in = input, .out = -1, .dir = j->repo_fd,
	                               .keep = j->lock_fd };
	return git_run_now(&j->git, command, how, fds, &j->out, oid);
}

static int
journal_tip (struct journal *j)
{
	char const *command[] = { "rev-parse", "--verify", "--quiet", j->ref, nullptr };
	struct oid tip;
	int e = journal_git(j, command, -1, &tip);
	/* The tip is empty if the ref does not exist. */
	if (!e)
		j->tip = tip;
	return e;
}

static int
journal_unlock (struct journal *j)
{
	/* The writer lock of the service excludes both other writers and their
	 * surviving Git children. Only the loose-ref lock of this service is
	 * ours. */
	char path[sizeof j->ref + sizeof ".lock"];
	int n = snprintf(path, sizeof path, "%s.lock", j->ref);
	if (n < 0 || n >= (int)sizeof path)
		return EOVERFLOW;
	char const *name;
	int fd = -1;
	int e = util_parent_open(j->repo_fd, path, &name, &fd);
	if (e)
		return e == ENOENT ? 0 : e;
	struct stat st;
	if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW))
		e = errno == ENOENT ? 0 : errno;
	else if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1)
		e = EACCES;
	else if (unlinkat(fd, name, 0) || fsync(fd))
		e = errno;
	close(fd);
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
	int e = util_read_all(fd, header, sizeof header, *offset, &got);
	*record = nullptr;
	if (e || !got)
		return e;
	if (got != sizeof header)
		goto incomplete;
	if (memcmp(header, JOURNAL_MAGIC, sizeof JOURNAL_MAGIC - 1))
		return EILSEQ;
	uint64_t length = util_be_get(header + JOURNAL_AT_LENGTH, sizeof length);
	if (length > JOURNAL_RECORD_MAX)
		return EILSEQ;
	struct journal_record *r = calloc(1, sizeof *r + (size_t)length + 1);
	if (!r)
		return ENOMEM;
	r->sequence = util_be_get(header + JOURNAL_AT_SEQUENCE, sizeof r->sequence);
	r->len = (size_t)length;
	struct cstr_view const id = cstr_view_n((char const *)header + JOURNAL_AT_ID, HEXID_DIGITS);
	char const *kind = (char const *)header + JOURNAL_AT_KIND;
	size_t const kind_len = strnlen(kind, JOURNAL_KIND);
	if (hexid_init(&r->id, id) || !util_word(kind, kind_len, JOURNAL_KIND)) {
		journal_records_destroy(&r);
		return EILSEQ;
	}
	r->type = journal_kind(cstr_view_n(kind, kind_len));
	e = util_read_all(fd, r->data, r->len, *offset + (off_t)sizeof header, &got);
	if (e || got != r->len) {
		journal_records_destroy(&r);
		if (e)
			return e;
		goto incomplete;
	}
	unsigned char digest[JOURNAL_DIGEST];
	e = journal_digest(j, header, r->data, r->len, digest);
	if (!e && memcmp(digest, header + JOURNAL_SIGNED, sizeof digest))
		e = EILSEQ;
	if (e) {
		journal_records_destroy(&r);
		return e;
	}
	*offset += (off_t)(sizeof header + r->len);
	*record = r;
	return 0;

incomplete:
	return partial ? 0 : EILSEQ;
}

/* Free the records of @p *list whose kinds' bits are set in @p kinds;
 * returns the bytes they took.
 */
static size_t
journal_records_drop (struct journal_record **list,
                      unsigned                kinds)
{
	size_t bytes = 0;
	while (*list) {
		struct journal_record *r = *list;
		if (!(KIND(r->type) & kinds)) {
			list = &r->next;
			continue;
		}
		*list = r->next;
		r->next = nullptr;
		bytes += sizeof *r + r->len + 1;
		journal_records_destroy(&r);
	}
	return bytes;
}

/* The flags of a job whose checkpoint is the list @p records. */
static uint16_t
journal_job_flags (struct journal_record const *records)
{
	unsigned kinds = 0;
	for (; records; records = records->next)
		kinds |= KIND(records->type);
	return (kinds & TERMINAL ? JOB_TERMINAL : 0) |
	       (kinds & KIND(JOURNAL_LEGACY) ? JOB_LEGACY : 0);
}

/** New snapshots are atomic replacements; legacy WAL tails may end with an
 * interrupted write. Neither loading nor migration modifies that WAL.
 */
static int
journal_records_read (struct journal         *j,
                      int                     fd,
                      struct hexid const     *id,
                      bool                    legacy,
                      struct journal_record **out)
{
	/* What a record of the legacy WAL replaces: a state all records before
	 * it, and a meta the metas before it. */
	static unsigned const replaces[JOURNAL_OTHER] = {
		[JOURNAL_STATE] = ~0u,
		[JOURNAL_META]  = KIND(JOURNAL_META),
	};
	off_t offset = 0;
	uint64_t sequence = 0;
	size_t bytes = 0;
	struct journal_record **tail = out;
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
		if ((id && !hexid_equal(&r->id, id)) || r->type == JOURNAL_OTHER) {
			journal_records_destroy(&r);
			continue;
		}
		if (legacy && replaces[r->type]) {
			bytes -= journal_records_drop(out, replaces[r->type]);
			for (tail = out; *tail; tail = &(*tail)->next)
				;
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
journal_job_find (struct journal     *j,
                  struct hexid const *id)
{
	for (size_t k = 0; k < array_size(j->jobs); ++k)
		if (hexid_equal(&j->jobs[k].id, id))
			return &j->jobs[k];
	return nullptr;
}

static int
journal_jobs_scan (struct journal *j)
{
	int e;
	DIR *directory = util_dir_open(j->jobs_fd, &e);
	if (!directory)
		return e;
	for (struct dirent *entry; (entry = util_dir_next(directory, &e)); ) {
		struct hexid id = hexid(cstr_view(entry->d_name), &e);
		if (e)
			continue;
		if (j->n_jobs == JOURNAL_JOBS) {
			e = EFBIG;
			break;
		}
		int input = -1;
		e = util_private_open(j->jobs_fd, entry->d_name, O_RDONLY, &input);
		struct stat st;
		if (!e && fstat(input, &st))
			e = errno;
		/* A negative size, impossible for a regular file, converts to more
		 * than any limit. */
		uint64_t const size = e ? 0 : (uint64_t)st.st_size;
		if (size > JOURNAL_LOAD_MAX ||
		    size > JOURNAL_RECOVERY_BYTES + JOURNAL_TERMINAL_BYTES - j->recovery_bytes)
			e = EFBIG;
		struct journal_record *records = nullptr;
		if (!e)
			e = journal_records_read(j, input, &id, false, &records);
		util_close(&input);
		if (!e) {
			struct journal_job *job = &j->jobs[j->n_jobs++];
			job->id = id;
			job->bytes = size;
			job->touched = st.st_mtim.tv_sec < 0 ? 0 : clk_ns(st.st_mtim);
			job->flags = journal_job_flags(records);
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
		for (size_t k = 0; k < array_size(j->jobs); ++k) {
			struct journal_job *job = &j->jobs[k];
			if (job != keep && *job->id.hex &&
			    (job->flags & (JOB_TERMINAL | JOB_LEGACY)) == JOB_TERMINAL &&
			    (!oldest || job->touched < oldest->touched))
				oldest = job;
		}
		if (!oldest)
			return ENOSPC;
		if (oldest->bytes &&
		    (unlinkat(j->jobs_fd, oldest->id.hex, 0) || fsync(j->jobs_fd)))
			return errno;
		j->recovery_bytes -= oldest->bytes;
		--j->n_jobs;
		*oldest = (struct journal_job){};
	}
	return 0;
}

/* Add the job @p id, which is not in the table, making room for it;
 * nullptr with the error in @p *e if there is none.
 */
static struct journal_job *
journal_job_add (struct journal     *j,
                 struct hexid const *id,
                 int                *e)
{
	*e = journal_checkpoint_space(j, nullptr, 0, true);
	if (*e)
		return nullptr;
	for (size_t k = 0; k < array_size(j->jobs); ++k) {
		struct journal_job *job = &j->jobs[k];
		if (*job->id.hex)
			continue;
		job->id = *id;
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
	unsigned char header[JOURNAL_HEADER] = JOURNAL_MAGIC;
	util_be_put(header + JOURNAL_AT_SEQUENCE, sequence, sizeof sequence);
	util_be_put(header + JOURNAL_AT_LENGTH, r->len, sizeof (uint64_t));
	memcpy(header + JOURNAL_AT_ID, r->id.hex, HEXID_DIGITS);
	memcpy(header + JOURNAL_AT_KIND, kind_names[r->type].data, kind_names[r->type].len);
	int e = journal_digest(j, header, r->data, r->len, header + JOURNAL_SIGNED);
	if (!e)
		e = util_write_all(fd, header, sizeof header);
	return e ? e : util_write_all(fd, r->data, r->len);
}

static int
journal_checkpoint_write (struct journal        *j,
                          struct journal_record *record)
{
	if (record->type == JOURNAL_OTHER)
		return EINVAL;
	unsigned const kind = KIND(record->type);
	bool const state = kind == KIND(JOURNAL_STATE);
	bool const terminal = kind & TERMINAL;
	struct journal_job *job = journal_job_find(j, &record->id);
	/* A lost record leaves a gap in its job's checkpoint, so the job's
	 * later records fail with its error, until a state snapshot or a
	 * terminal record, which keeps only the state, replaces them all. */
	if (job && job->error && !(kind & (KIND(JOURNAL_STATE) | TERMINAL)))
		return job->error;
	bool legacy = job && job->flags & JOB_LEGACY;
	struct journal_record *records = nullptr;
	int e = 0;
	if (!state) {
		if (job && job->bytes) {
			int input = -1;
			e = util_private_open(j->jobs_fd, record->id.hex, O_RDONLY, &input);
			if (!e)
				e = journal_records_read(j, input, &record->id, false, &records);
			util_close(&input);
		} else if (j->wal_fd >= 0) {
			e = journal_records_read(j, j->wal_fd, &record->id, true, &records);
			legacy = records != nullptr;
		}
	}
	uint16_t const had = journal_job_flags(records);
	if (!e && legacy && !(had & JOB_LEGACY)) {
		struct journal_record *mark = calloc(1, sizeof *mark + 1);
		if (!mark)
			e = ENOMEM;
		else {
			mark->id = record->id;
			mark->type = JOURNAL_LEGACY;
			mark->next = records;
			records = mark;
		}
	}
	/* A completed job needs only its owner/state and result. Drop tool
	 * evidence once the terminal record makes further execution impossible.
	 * A terminal reserve lets even a full active cache retire a job. A meta
	 * replaces the metas before it. */
	unsigned const stale = terminal ? ~(KIND(JOURNAL_STATE) | KIND(JOURNAL_LEGACY))
	                     : kind & KIND(JOURNAL_META);
	if (stale)
		(void)journal_records_drop(&records, stale);
	uint64_t bytes = JOURNAL_HEADER + record->len;
	for (struct journal_record *r = records; r; r = r->next)
		bytes += JOURNAL_HEADER + r->len;
	if (!e && bytes > JOURNAL_LOAD_MAX)
		e = EFBIG;
	if (!e)
		e = journal_checkpoint_space(j, job, bytes, terminal);
	int fd = -1;
	if (!e)
		e = util_private_open(j->jobs_fd, ".checkpoint.tmp", O_RDWR | O_CREAT, &fd);
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
	if (!e && (renameat(j->jobs_fd, ".checkpoint.tmp", j->jobs_fd, record->id.hex) ||
	           fsync(j->jobs_fd)))
		e = errno;
	journal_records_destroy(&records);
	if (!e && !job)
		job = journal_job_add(j, &record->id, &e);
	if (e) {
		if (job)
			job->error = e;
		return e;
	}
	j->recovery_bytes = j->recovery_bytes - job->bytes + bytes;
	job->bytes = bytes;
	uint64_t now = clk_realtime_ns();
	if (now)
		job->touched = now;
	job->flags = had | (legacy ? JOB_LEGACY : 0) | (terminal ? JOB_TERMINAL : 0);
	job->error = 0;
	if (state)
		job->thought = (typeof(job->thought)){};
	return 0;
}

static int
journal_load_records (struct journal      *j,
                      struct journal_item *item)
{
	int fd = -1;
	int e = util_private_open(j->jobs_fd, item->id.hex, O_RDONLY, &fd);
	if (!e)
		e = journal_records_read(j, fd, &item->id, false, &item->record);
	util_close(&fd);
	if (e == ENOENT)
		e = j->wal_fd < 0 ? 0 : journal_records_read(j, j->wal_fd, &item->id, true,
		                                             &item->record);
	return e;
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
	for (size_t k = 0; k < array_size(j->files); ++k) {
		struct journal_file *file = &j->files[k];
		if (file->fd >= 0 && file->turn == item->turn && hexid_equal(&file->id, &item->id)) {
			file->touched = ++j->clock;
			*out = file;
			return 0;
		}
		if (file->fd < 0 || (oldest->fd >= 0 && file->touched < oldest->touched))
			oldest = file;
	}
	util_close(&oldest->fd);
	int directory = -1;
	bool append = item->operation == JOURNAL_THOUGHT_PUT;
	int e = 0;
	if (append)
		e = journal_directory(j->thoughts_fd, item->id.hex, &directory);
	else {
		directory = openat(j->thoughts_fd, item->id.hex,
		                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (directory < 0)
			e = errno;
	}
	char name[UTIL_DECIMAL + sizeof ".txt"];
	int n = snprintf(name, sizeof name, "%" PRIu32 ".txt", item->turn);
	if (!e && (n < 0 || n >= (int)sizeof name))
		e = EOVERFLOW;
	/* Every write appends, wherever a commit left the offset. Only a new
	 * file's name needs its directory synchronized. */
	if (!e)
		e = util_private_open(directory, name, O_RDWR | O_APPEND | (append ? O_CREAT : 0),
		                      &oldest->fd);
	struct stat st;
	if (!e && fstat(oldest->fd, &st))
		e = errno;
	if (!e && st.st_size < 0)
		e = EILSEQ;
	if (!e && append && !st.st_size && fsync(directory))
		e = errno;
	util_close(&directory);
	if (e) {
		util_close(&oldest->fd);
		return e;
	}
	oldest->size = (uint64_t)st.st_size;
	oldest->committed_size = 0;
	oldest->turn = item->turn;
	oldest->touched = ++j->clock;
	oldest->id = item->id;
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
		e = util_read_all(file->fd, j->scratch, n, (off_t)(item->offset + at), &got);
		if (e || got != n || memcmp(j->scratch, record->data + at, n))
			return e ? e : EILSEQ;
		at += n;
	}
	if (same == record->len)
		return 0;
	e = util_write_all(file->fd, record->data + same, record->len - same);
	if (!e)
		file->size += record->len - same;
	return e;
}

static int
journal_thought_put (struct journal            *j,
                     struct journal_item const *item)
{
	int e = 0;
	struct journal_job *job = journal_job_find(j, &item->id);
	if (!job && !(job = journal_job_add(j, &item->id, &e)))
		return e;
	bool begin = !item->offset && !item->record->len;
	if (begin || job->thought.turn != item->turn)
		job->thought = (typeof(job->thought)){ .turn = item->turn };
	e = begin ? 0 : job->thought.error ? job->thought.error : journal_thought_write(j, item);
	job->thought.error = e;
	if (!job->bytes)
		job->flags &= ~JOB_TERMINAL;
	if (!e)
		job->thought.seen = item->offset + item->record->len;
	return e;
}

/* Commit the reasoning of the turn of @p item, whose job is @p job, or
 * nullptr for an item without a job.
 */
static int
journal_thought_commit (struct journal            *j,
                        struct journal_item const *item,
                        struct journal_job        *job)
{
	bool const turn = job && job->thought.turn == item->turn;
	if (turn && job->thought.error)
		return job->thought.error;
	struct journal_file *file = nullptr;
	int e = journal_thought_file(j, item, &file);
	if (e || !file->size)
		return e == ENOENT ? 0 : e;
	uint64_t seen = turn ? job->thought.seen : 0;
	if (seen < file->size)
		return item->operation == JOURNAL_THOUGHT_FINISH ? EILSEQ : 0;
	if (file->committed_size == file->size)
		return 0;
	if (fdatasync(file->fd) || lseek(file->fd, 0, SEEK_SET) < 0)
		return errno;
	char const *hash[] = { "hash-object", "-w", "--stdin", nullptr };
	struct oid blob;
	e = journal_git(j, hash, file->fd, &blob);
	char path[sizeof "reasoning/xx//.txt" + HEXID_DIGITS + UTIL_DECIMAL];
	int n = snprintf(path, sizeof path, "reasoning/%.2s/%s/%" PRIu32 ".txt",
	                 item->id.hex, item->id.hex, item->turn);
	if (!e && (n < 0 || n >= (int)sizeof path))
		e = EOVERFLOW;
	if (!e && oid_len(&j->tip)) {
		char name[sizeof j->tip + sizeof path];
		n = snprintf(name, sizeof name, "%s:%s", j->tip.hex, path);
		if (n < 0 || n >= (int)sizeof name)
			e = EOVERFLOW;
		char const *find[] = { "rev-parse", "--verify", "--quiet", name, nullptr };
		struct oid old;
		if (!e)
			e = journal_git(j, find, -1, &old);
		/* The check only decides whether the commit can be skipped.
		 * The journal commits after EIO: an exit status other than 0
		 * and 1, an output that is not one name, or a signal. */
		if (e == EIO)
			e = 0;
		else if (!e && oid_equal(&old, &blob)) {
			file->committed_size = file->size;
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
		if (oid_len(&j->tip))
			fprintf(output, "from %s\n", j->tip.hex);
		fprintf(output, "M 100644 %s %s\n\ndone\n", blob.hex, path);
		if (fflush(output) || ferror(output))
			e = e ? e : EIO;
		if (!e && lseek(fd, 0, SEEK_SET) < 0)
			e = errno;
		char const *import[] = { "fast-import", "--quiet", "--done", nullptr };
		if (!e)
			e = journal_git(j, import, fd, nullptr);
	}
	if (output) {
		if (fclose(output) && !e)
			e = errno;
		output = nullptr;
		fd = -1;
	}
	util_close(&fd);
	/* Only this writer's children write its ref, and they have all been
	 * reaped: a lock that one left is stale, and the ref may have moved. */
	if (e)
		(void)journal_unlock(j);
	int tip = journal_tip(j);
	if (!e && !(e = tip))
		file->committed_size = file->size;
	return e;
}

/* Commit the reasoning of the turn of @p item, after which a final item
 * ends its job, unless a checkpoint of the job exists.
 */
static int
journal_thought_close (struct journal            *j,
                       struct journal_item const *item)
{
	struct journal_job *job = journal_job_find(j, &item->id);
	int e = journal_thought_commit(j, item, job);
	if (!e && job && !job->bytes && item->operation == JOURNAL_THOUGHT_FINISH)
		job->flags |= JOB_TERMINAL;
	return e;
}

static void
journal_item_destroy (struct journal_item **p_item)
{
	struct journal_item *item = *p_item;
	*p_item = nullptr;
	journal_records_destroy(&item->record);
	free(item);
}

static void
journal_complete (struct loop *loop,
                  struct msg  *msg)
{
	struct journal_item *item = container_of(msg, struct journal_item, msg);
	if (item->operation == JOURNAL_LOAD) {
		struct journal_record *records = item->record;
		item->record = nullptr;
		item->loaded(loop, item->arg, item->error, records);
	} else {
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
	if (list_is_empty(&j->items))
		return nullptr;
	struct journal_item *item = container_of(j->items.next, struct journal_item, hook);
	list_del(&item->hook);
	tally_sub(&j->queue, item->bytes);
	return item;
}

static void *
journal_thread (void *arg)
{
	struct journal *j = arg;
	(void)pthread_setname_np(pthread_self(), "journal");
	for (;;) {
		pthread_mutex_lock(&j->lock);
		while (list_is_empty(&j->items) && !(j->flags & WRITER_STOP))
			pthread_cond_wait(&j->ready, &j->lock);
		struct journal_item *item = journal_take(j);
		pthread_mutex_unlock(&j->lock);
		if (!item)
			break;
		/* An error concerns only its own job, and a reasoning error
		 * only its own turn's reasoning. */
		int e;
		switch (item->operation) {
		case JOURNAL_CONTROL_PUT:
			e = journal_checkpoint_write(j, item->record);
			break;
		case JOURNAL_THOUGHT_PUT:
			e = journal_thought_put(j, item);
			break;
		case JOURNAL_LOAD:
			e = journal_load_records(j, item);
			break;
		default:
			e = journal_thought_close(j, item);
		}
		journal_finish(item, e);
	}
	return nullptr;
}

/* Queue @p item, or destroy it; returns 0 or why it was not queued. */
static int
journal_enqueue (struct journal      *j,
                 struct journal_item *item)
{
	int e = 0;
	/* One small cancellation per ledger job must fit even when ordinary
	 * transcript traffic saturates its queue. These extra bounds remain
	 * finite and cannot be used by streams, loads, or oversized records. */
	bool control = item->record && item->record->len <= JOURNAL_CONTROL_RECORD &&
	               item->record->type == JOURNAL_CANCEL;
	struct tally const max = tally(JOURNAL_QUEUE + (control ? JOURNAL_CONTROL : 0),
	                               JOURNAL_BYTES + (control ? JOURNAL_CONTROL_BYTES : 0));
	pthread_mutex_lock(&j->lock);
	if (j->flags & WRITER_STOP)
		e = ECANCELED;
	else if (!tally_fits(&j->queue, max, item->bytes))
		e = EAGAIN;
	else {
		list_append(&j->items, &item->hook);
		tally_add(&j->queue, item->bytes);
	}
	pthread_mutex_unlock(&j->lock);
	if (e)
		journal_item_destroy(&item);
	else
		pthread_cond_signal(&j->ready);
	return e;
}

static int
journal_submit (struct journal     *j,
                struct hexid const *id,
                enum journal_kind   type,
                uint32_t            turn,
                uint64_t            offset,
                uint8_t             operation,
                void const         *data,
                size_t              len,
                struct loop        *loop,
                journal_done_cb    *done,
                void               *arg)
{
	if (!j)
		return 0;
	if (!id || !*id->hex || (len && !data) || (!!loop != !!done))
		return EINVAL;
	if (len > JOURNAL_RECORD_MAX)
		return EFBIG;
	struct journal_item *item = malloc(sizeof *item);
	struct journal_record *record = item ? calloc(1, sizeof *record + len + 1) : nullptr;
	if (!record) {
		free(item);
		return ENOMEM;
	}
	*item = (struct journal_item){
		.msg.fn    = journal_complete,
		.hook      = list(&item->hook),
		.record    = record,
		.loop      = loop,
		.done      = done,
		.arg       = arg,
		.bytes     = sizeof *item + sizeof *record + len + 1,
		.offset    = offset,
		.turn      = turn,
		.operation = operation,
		.id        = *id,
	};
	record->len = len;
	record->id = *id;
	record->type = type;
	if (len)
		memcpy(record->data, data, len);
	return journal_enqueue(j, item);
}

int
journal_put (struct journal     *j,
             struct hexid const *id,
             char const         *kind,
             void const         *data,
             size_t              len,
             struct loop        *loop,
             journal_done_cb    *done,
             void               *arg)
{
	size_t const n = util_word_len(kind, JOURNAL_KIND);
	return j && !n ? EINVAL
	               : journal_submit(j, id, journal_kind(cstr_view_n(kind, n)), 0, 0,
	                                JOURNAL_CONTROL_PUT, data, len, loop, done, arg);
}

int
journal_thought (struct journal     *j,
                 struct hexid const *id,
                 uint32_t            turn,
                 uint64_t            offset,
                 void const         *data,
                 size_t              len,
                 struct loop        *loop,
                 journal_done_cb    *done,
                 void               *arg)
{
	if (!turn)
		return EINVAL;
	return journal_submit(j, id, JOURNAL_OTHER, turn, offset, JOURNAL_THOUGHT_PUT,
	                      data, len, loop, done, arg);
}

int
journal_thought_end (struct journal     *j,
                     struct hexid const *id,
                     uint32_t            turn,
                     bool                final,
                     struct loop        *loop,
                     journal_done_cb    *done,
                     void               *arg)
{
	if (!turn)
		return EINVAL;
	return journal_submit(j, id, JOURNAL_OTHER, turn, 0,
	                      final ? JOURNAL_THOUGHT_FINISH : JOURNAL_THOUGHT_END,
	                      nullptr, 0, loop, done, arg);
}

int
journal_load_current (struct journal     *j,
                      struct hexid const *id,
                      struct loop        *loop,
                      journal_load_cb    *done,
                      void               *arg)
{
	if (!j || !loop || !done || !id || !*id->hex)
		return EINVAL;
	struct journal_item *item = malloc(sizeof *item);
	if (!item)
		return ENOMEM;
	*item = (struct journal_item){
		.msg.fn    = journal_complete,
		.hook      = list(&item->hook),
		.loop      = loop,
		.loaded    = done,
		.arg       = arg,
		.bytes     = sizeof *item,
		.operation = JOURNAL_LOAD,
		.id        = *id,
	};
	return journal_enqueue(j, item);
}

/* Whether the @p len bytes at @p buf are all zero. */
static bool
journal_zero (unsigned char const *buf,
              size_t               len)
{
	for (size_t i = 0; i < len; ++i)
		if (buf[i])
			return false;
	return true;
}

/* Read the secret of @p len bytes, at most 256, in the file @p name of the
 * directory @p dir into @p secret. If the file is empty or new, fill
 * @p secret with random bytes that are not all zero, and write them to the
 * file first. The file has mode 0600. The caller holds a lock that keeps
 * other processes from making the same file at the same time. Returns 0,
 * EILSEQ if the file does not have exactly @p len bytes or has only zero
 * bytes, EACCES if it is not a private file of askai's user
 * (util_private()), or another error number. On failure, the
 * function writes to the @p why_len bytes at @p why the path of the file,
 * @p shown and @p name, and what is wrong with it.
 */
static int
journal_secret (int         dir,
                char const *shown,
                char const *name,
                void       *secret,
                size_t      len,
                char       *why,
                size_t      why_len)
{
	int fd = -1;
	int e = util_private_open(dir, name, O_RDWR | O_CREAT, &fd);
	struct stat st;
	if (!e && fstat(fd, &st))
		e = errno;
	/* util_private_open() returns EACCES if the file is not private. */
	char const *problem = nullptr;
	if (e == EACCES && !fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW))
		problem = util_private(&st, S_IFREG);
	if (!e && !st.st_size) {
		do
			e = util_random(secret, len);
		while (!e && journal_zero(secret, len));
		if (!e)
			e = util_write_all(fd, secret, len);
		if (!e && fdatasync(fd))
			e = errno;
	} else if (!e && st.st_size != (off_t)len) {
		/* The reason of this problem contains two numbers, so the
		 * branch formats the whole reason and returns. */
		util_close(&fd);
		util_format(why, why_len, "%s/%s: the file has %jd bytes, not %zu", shown, name,
		            (intmax_t)st.st_size, len);
		return EILSEQ;
	} else if (!e) {
		size_t n = 0;
		e = util_read_all(fd, secret, len, 0, &n);
		if (!e && n != len)
			e = EIO;
		if (!e && journal_zero(secret, len)) {
			e = EILSEQ;
			problem = "the file has only zero bytes";
		}
	}
	util_close(&fd);
	if (e)
		util_format(why, why_len, "%s/%s: %s", shown, name,
		            problem ? problem : strerror(e));
	return e;
}

static int
journal_setup (struct journal *j,
               char const     *path,
               char const     *service,
               char           *why,
               size_t          why_len)
{
	int e = git_run_dir_init(&j->git, nullptr, git_options, nullptr, GIT_RUN_DOT);
	int init_fd = -1;
	int state = -1;
	if (!e)
		e = journal_open_path(j, path);
	if (!e)
		e = util_private_open(j->repo_fd, "askai-journal.lock", O_RDWR | O_CREAT, &init_fd);
	if (!e && flock(init_fd, LOCK_EX))
		e = errno;
	struct stat st;
	if (!e && fstatat(j->repo_fd, "HEAD", &st, AT_SYMLINK_NOFOLLOW)) {
		if (errno != ENOENT)
			e = errno;
		else {
			char const *command[] = { GIT_INIT_BARE_WORDS("reasoning"), nullptr };
			e = journal_git(j, command, -1, nullptr);
		}
	} else if (!e && !S_ISREG(st.st_mode)) {
		e = EINVAL;
	}
	if (!e) {
		char const *command[] = { "rev-parse", "--is-bare-repository", nullptr };
		e = journal_git(j, command, -1, nullptr);
		if (!e && !cstr_view_equal(cstr_view_span(buf_peek(&j->out)), CSTR_VIEW("true\n")))
			e = EINVAL;
	}
	if (!e)
		e = journal_directory(j->repo_fd, "askai-journal", &state);
	if (!e)
		e = journal_private(state);
	if (!e)
		e = journal_directory(state, service, &j->state_fd);
	if (!e)
		e = journal_private(j->state_fd);
	if (!e)
		e = util_private_open(j->state_fd, "writer.lock", O_RDWR | O_CREAT, &j->lock_fd);
	if (!e && flock(j->lock_fd, LOCK_EX | LOCK_NB)) {
		e = errno;
		if (e == EWOULDBLOCK)
			util_format(why, why_len, "the service name %s is in use in this repository; "
			            "give this askai another --service-name", service);
	}
	if (!e)
		e = journal_directory(j->state_fd, "jobs", &j->jobs_fd);
	if (!e)
		e = journal_directory(j->state_fd, "reasoning", &j->thoughts_fd);
	if (!e) {
		e = util_private_open(j->state_fd, "records.wal", O_RDONLY, &j->wal_fd);
		if (e == ENOENT)
			e = 0;
	}
	/* Every service of the repository reads the same principal.key. Each
	 * service reads its own owner.seed. */
	char shown[sizeof "askai-journal/" + UTIL_WORD_MAX];
	(void)util_format(shown, sizeof shown, "askai-journal/%s", service);
	if (!e)
		e = journal_secret(state, "askai-journal", "principal.key", j->key,
		                   sizeof j->key, why, why_len);
	if (!e)
		e = journal_secret(j->state_fd, shown, "owner.seed", &j->seed,
		                   sizeof j->seed, why, why_len);
	if (!e && (fsync(state) || fsync(j->state_fd) || fsync(j->repo_fd)))
		e = errno;
	util_close(&state);
	/* The close releases the lock, as no other descriptor shares it. */
	util_close(&init_fd);
	if (!e)
		e = journal_unlock(j);
	if (!e)
		e = journal_tip(j);
	return e ? e : journal_jobs_scan(j);
}

struct journal *
journal_create (char const *path,
                char const *service,
                char       *why,
                size_t      why_len,
                int        *e)
{
	*e = 0;
	if (!why || !why_len) {
		*e = EINVAL;
		return nullptr;
	}
	/* journal_setup() writes a message of its own for some errors. For
	 * the others, the message is the text of the error number. */
	*why = '\0';
	if (!service || !member_name_valid(cstr_view(service))) {
		*e = util_refuse(why, why_len, "invalid service name");
		return nullptr;
	}
	struct journal *j = calloc(1, sizeof *j);
	*e = j ? pthread_mutex_init(&j->lock, nullptr) : ENOMEM;
	if (!*e && (*e = pthread_cond_init(&j->ready, nullptr)))
		pthread_mutex_destroy(&j->lock);
	if (*e) {
		free(j);
		util_format(why, why_len, "%s", strerror(*e));
		return nullptr;
	}
	j->repo_fd = j->state_fd = j->jobs_fd = j->thoughts_fd = j->lock_fd = j->wal_fd = -1;
	for (size_t k = 0; k < array_size(j->files); ++k)
		j->files[k].fd = -1;
	list_init(&j->items);
	j->digest = EVP_MD_CTX_new();
	(void)util_format(j->ref, sizeof j->ref, "%s%s", REF, service);
	*e = j->digest ? journal_setup(j, path, service, why, why_len) : ENOMEM;
	if (!*e) {
		/* Creation precedes brainstem_run(), which routes asynchronous
		 * signals through signalfd. Inherit its worker mask at creation,
		 * never leaving a window for SIGTERM to choose this thread. */
		sigset_t old;
		*e = util_block_async(&old);
		if (!*e) {
			/* The writer reads the flags, so they change before it
			 * starts, and back if it does not. */
			j->flags |= WRITER_STARTED;
			*e = pthread_create(&j->thread, nullptr, journal_thread, j);
			if (*e)
				j->flags &= ~WRITER_STARTED;
			int restore = pthread_sigmask(SIG_SETMASK, &old, nullptr);
			if (!*e)
				*e = restore;
		}
	}
	if (*e) {
		if (!*why)
			util_format(why, why_len, "%s", strerror(*e));
		journal_destroy(&j);
	}
	return j;
}

void
journal_stop (struct journal *j)
{
	if (!j || (j->flags & (WRITER_STARTED | WRITER_STOP)) != WRITER_STARTED)
		return;
	pthread_mutex_lock(&j->lock);
	j->flags |= WRITER_STOP;
	pthread_mutex_unlock(&j->lock);
	pthread_cond_signal(&j->ready);
	(void)pthread_join(j->thread, nullptr);
}

void
journal_destroy (struct journal **p_journal)
{
	if (!p_journal || !*p_journal)
		return;
	struct journal *j = *p_journal;
	*p_journal = nullptr;
	journal_stop(j);
	for (size_t k = 0; k < array_size(j->files); ++k)
		util_close(&j->files[k].fd);
	EVP_MD_CTX_free(j->digest);
	j->digest = nullptr;
	buf_fini(&j->out);
	j->git = (struct git_run_dir){};
	util_close(&j->jobs_fd);
	util_close(&j->thoughts_fd);
	util_close(&j->wal_fd);
	util_close(&j->lock_fd);
	util_close(&j->state_fd);
	util_close(&j->repo_fd);
	pthread_cond_destroy(&j->ready);
	pthread_mutex_destroy(&j->lock);
	free(j);
}

uint64_t
journal_seed (struct journal const *j)
{
	return j ? j->seed : 0;
}

int
journal_key (struct journal const *j,
             unsigned char         key[POLICY_KEY])
{
	if (!j || !key)
		return EINVAL;
	memcpy(key, j->key, sizeof j->key);
	return 0;
}

#undef JOURNAL_MAGIC
#undef KIND
