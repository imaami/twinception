/** @file Recovery loads and gaps, and checkpoint-independent reasoning. */
#define _GNU_SOURCE
#include <assert.h>

#include "../src/journal.c"

static unsigned journal_test_completed;

static void
journal_test_done (struct loop *loop, void *arg, int error)
{
	(void)loop;
	assert(error == *(int const *)arg);
	++journal_test_completed;
}

static void
journal_test_id (struct hexid *id, unsigned number)
{
	assert(snprintf(id->hex, sizeof id->hex, "%032x", number) == (int)sizeof id->hex - 1);
}

/* Give @p r the kind @p name. */
static void
journal_test_kind (struct journal_record *r,
                   char const            *name)
{
	struct cstr_view const kind = cstr_view(name);
	memcpy(r->kind, kind.data, kind.len + 1);
	r->type = journal_kind(kind);
}

static void
journal_test_thoughts (struct journal *j)
{
	struct loop events = loop();
	assert(!loop_init(&events));
	int const success = 0, mismatch = EILSEQ;
	struct hexid id;
	/* More files than the descriptor cache holds: verification must survive
	 * eviction, even though none of these jobs has a recovery checkpoint. */
	for (unsigned k = 1; k <= JOURNAL_FILES + 1; ++k) {
		journal_test_id(&id, k);
		assert(!journal_thought(j, &id, 1, 0, "abc", 3, &events,
		                        journal_test_done, (void *)&success));
	}
	for (unsigned k = 1; k <= JOURNAL_FILES + 1; ++k) {
		journal_test_id(&id, k);
		assert(!journal_thought_end(j, &id, 1, true, &events,
		                            journal_test_done, (void *)&success));
	}
	assert(!journal_thought(j, &id, 1, 0, nullptr, 0, &events,
	                        journal_test_done, (void *)&success));
	assert(!journal_thought(j, &id, 1, 0, "a", 1, &events,
	                        journal_test_done, (void *)&success));
	assert(!journal_thought_end(j, &id, 1, true, &events,
	                            journal_test_done, (void *)&mismatch));
	assert(!journal_thought(j, &id, 1, 0, "x", 1, &events,
	                        journal_test_done, (void *)&mismatch));
	assert(!journal_thought_end(j, &id, 1, true, &events,
	                            journal_test_done, (void *)&mismatch));
	assert(!journal_thought(j, &id, 1, 0, nullptr, 0, &events,
	                        journal_test_done, (void *)&success));
	assert(!journal_thought(j, &id, 1, 0, "abc", 3, &events,
	                        journal_test_done, (void *)&success));
	assert(!journal_thought_end(j, &id, 1, true, &events,
	                            journal_test_done, (void *)&success));
	assert(!journal_thought(j, &id, 1, 3, "def", 3, &events,
	                        journal_test_done, (void *)&success));
	journal_stop(j);
	loop_fini(&events);
	assert(journal_test_completed == 2 * (JOURNAL_FILES + 1) + 9);
	assert(!journal_job_find(j, &id)->thought_error);
	assert(!(journal_job_find(j, &id)->flags & JOB_TERMINAL));
	struct journal_item load = { .id = id };
	assert(!journal_load_records(j, &load) && !load.record);
}

/* A load returns its own job's records, in order, however many other jobs
 * the recovery cache holds. */
static void
journal_test_load (struct journal *j)
{
	struct journal_record *record = calloc(1, sizeof *record + JOURNAL_RECORD_MAX + 1);
	assert(record);
	for (unsigned k = 1; k <= 4; ++k) {
		journal_test_id(&record->id, k);
		journal_test_kind(record, "state");
		record->len = JOURNAL_RECORD_MAX;
		assert(!journal_checkpoint_write(j, record));
		journal_test_kind(record, "meta");
		record->len = 1;
		assert(!journal_checkpoint_write(j, record));
	}
	free(record);
	record = nullptr;
	assert(j->recovery_bytes > JOURNAL_LOAD_MAX);
	struct journal_item load = {};
	journal_test_id(&load.id, 2);
	assert(!journal_load_records(j, &load));
	struct journal_record const *r = load.record;
	assert(r && !strcmp(r->kind, "state") && r->len == JOURNAL_RECORD_MAX);
	assert(r->next && !strcmp(r->next->kind, "meta") && !r->next->next);
	journal_records_destroy(&load.record);
}

/* A lost record fails its job's later appends, until a snapshot or a
 * terminal record replaces them; another job goes on meanwhile. */
static void
journal_test_gap (struct journal *j)
{
	struct journal_record r = {};
	struct journal_record other = {};
	journal_test_id(&r.id, 100);
	journal_test_id(&other.id, 101);
	journal_test_kind(&other, "state");
	journal_test_kind(&r, "state");
	assert(!journal_checkpoint_write(j, &r));
	int jobs = j->jobs_fd;
	j->jobs_fd = -1;
	journal_test_kind(&r, "tool-intent");
	assert(journal_checkpoint_write(j, &r) == EBADF);
	j->jobs_fd = jobs;
	journal_test_kind(&r, "tool-result");
	assert(journal_checkpoint_write(j, &r) == EBADF);
	assert(!journal_checkpoint_write(j, &other));
	journal_test_kind(&other, "meta");
	assert(!journal_checkpoint_write(j, &other));
	journal_test_kind(&r, "state");
	assert(!journal_checkpoint_write(j, &r));
	journal_test_kind(&r, "tool-intent");
	assert(!journal_checkpoint_write(j, &r));
	j->jobs_fd = -1;
	assert(journal_checkpoint_write(j, &r) == EBADF);
	j->jobs_fd = jobs;
	journal_test_kind(&r, "result");
	assert(!journal_checkpoint_write(j, &r));
	assert(!journal_job_find(j, &r.id)->error &&
	       journal_job_find(j, &r.id)->flags & JOB_TERMINAL);
}

static void
journal_test_legacy (struct journal *j)
{
	struct journal_record record = {};
	journal_test_kind(&record, "state");
	journal_test_id(&record.id, JOURNAL_FILES + 1);
	j->wal_fd = memfd_create("journal-legacy-test", MFD_CLOEXEC);
	assert(j->wal_fd >= 0);
	assert(!journal_record_write(j, j->wal_fd, &record, 1));
	journal_test_kind(&record, "stream");
	assert(!journal_record_write(j, j->wal_fd, &record, 2));
	struct journal_item load = { .id = record.id };
	assert(!journal_load_records(j, &load));
	assert(load.record && !load.record->next && !strcmp(load.record->kind, "state"));
	journal_records_destroy(&load.record);
	journal_test_kind(&record, "meta");
	assert(!journal_checkpoint_write(j, &record));
	assert(journal_job_find(j, &record.id)->flags & JOB_LEGACY);
	assert(!journal_load_records(j, &load));
	unsigned count = 0;
	for (struct journal_record *r = load.record; r; r = r->next) {
		assert(strcmp(r->kind, "stream"));
		++count;
	}
	assert(count == 3); /* state, migration marker, and new metadata */
	journal_records_destroy(&load.record);
}

/* Run "sh -c @p script" with child_run(), with a deadline @p after_ms from
 * now, or none if 0, and a grace of @p grace_ms. Returns its result, its
 * exit status in @p *status, and how long it ran in @p *ms.
 */
static int
journal_test_run (char const *script,
                  uint64_t    after_ms,
                  uint64_t    grace_ms,
                  int        *status,
                  uint64_t   *ms)
{
	char const *const argv[] = { "sh", "-c", script, nullptr };
	char *const envp[] = { "PATH=/usr/bin:/bin", nullptr };
	uint64_t start = loop_clock();
	struct child_cmd const cmd = {
		.file = "sh",
		.argv = argv,
		.envp = envp,
		.deadline = after_ms ? start + after_ms * MS : 0,
		.grace = grace_ms * MS,
	};
	*status = -1;
	int e = child_run(&cmd, child_fds(), status);
	*ms = (loop_clock() - start) / MS;
	return e;
}

/* The parts of a path, and journal_open_path(), which opens them in the
 * repository @p repo: it skips empty parts and ".", and refuses ".." and a
 * part too long for a file name.
 */
static void
journal_test_path (char const *repo)
{
	struct cstr_view rest = CSTR_VIEW("/a//./bc/.../.");
	struct cstr_view part;
	assert(!util_path_next(&rest, &part) && cstr_view_equal(part, CSTR_VIEW("a")));
	assert(!util_path_next(&rest, &part) && cstr_view_equal(part, CSTR_VIEW("bc")));
	assert(!util_path_next(&rest, &part) && cstr_view_equal(part, CSTR_VIEW("...")));
	assert(!util_path_next(&rest, &part) && !part.len && !rest.len);
	rest = CSTR_VIEW("a/..");
	assert(!util_path_next(&rest, &part) && util_path_next(&rest, &part) == EINVAL);
	struct journal *j = calloc(1, sizeof *j);
	char *path = nullptr;
	assert(j && asprintf(&path, "%s/..", repo) > 0);
	assert(journal_open_path(j, path) == EINVAL);
	free(path);
	assert(asprintf(&path, "%s/%0*d", repo, NAME_MAX + 1, 0) > 0);
	assert(journal_open_path(j, path) == EINVAL);
	free(path);
	assert(asprintf(&path, "/.//%s/./", repo) > 0);
	assert(!journal_open_path(j, path));
	free(path);
	util_close(&j->repo_fd);
	free(j);
}

/* util_private_fd() on the repository @p repo, a private directory, and on
 * a file in it: the type, the links and the mode.
 */
static void
journal_test_private (char const *repo)
{
	int dir = open(repo, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(dir >= 0 && !util_private_fd(dir, S_IFDIR) && util_private_fd(dir, S_IFREG) == EACCES);
	int fd = openat(dir, "private.test", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	assert(fd >= 0 && !util_private_fd(fd, S_IFREG) && util_private_fd(fd, S_IFDIR) == EACCES);
	assert(!linkat(dir, "private.test", dir, "private.link", 0));
	assert(util_private_fd(fd, S_IFREG) == EACCES);
	assert(!unlinkat(dir, "private.link", 0) && !fchmod(fd, 0640));
	assert(util_private_fd(fd, S_IFREG) == EACCES);
	assert(!unlinkat(dir, "private.test", 0));
	util_close(&fd);
	util_close(&dir);
}

/* child_run(): exit statuses, a death by a signal, and a deadline without
 * a grace, with a grace that the command honors, and with a grace that it
 * ignores.
 */
static void
journal_test_child_run (void)
{
	int status;
	uint64_t ms;
	assert(!journal_test_run("exit 3", 0, 0, &status, &ms) && status == 3);
	assert(journal_test_run("kill -KILL $$", 0, 0, &status, &ms) == EIO);
	assert(journal_test_run("exec sleep 5", 100, 0, &status, &ms) == ETIMEDOUT);
	assert(ms >= 100 && ms < 2000);
	assert(journal_test_run("exec sleep 5", 100, 3000, &status, &ms) == ETIMEDOUT);
	assert(ms >= 100 && ms < 2000);
	assert(journal_test_run("trap '' TERM; exec sleep 5", 100, 300, &status, &ms) ==
	       ETIMEDOUT);
	assert(ms >= 400 && ms < 2500);
}

int
main (int argc, char **argv)
{
	assert(argc == 2);
	char why[256];
	int e = 0;
	struct journal *j = journal_create(argv[1], "test", why, sizeof why, &e);
	assert(j && !e);
	journal_test_thoughts(j);
	journal_test_load(j);
	journal_test_gap(j);
	journal_test_legacy(j);
	/* Finished, checkpoint-less jobs must not exhaust the bounded table. */
	for (unsigned k = JOURNAL_FILES + 2; k <= JOURNAL_JOBS + 1; ++k) {
		struct hexid id;
		journal_test_id(&id, k);
		struct journal_job *job = journal_job_find(j, &id);
		if (!job)
			job = journal_job_add(j, &id, &e);
		assert(job && !e);
		job->flags |= JOB_TERMINAL;
	}
	assert(j->n_jobs == JOURNAL_JOBS);
	journal_destroy(&j);
	journal_test_path(argv[1]);
	journal_test_private(argv[1]);
	journal_test_child_run();
	puts("Journal loads, write gaps, legacy migration, reasoning verification, "
	     "path walks, private files and child_run(): PASS");
	return 0;
}
