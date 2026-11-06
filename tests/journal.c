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
journal_test_id (char id[JOURNAL_ID + 1], unsigned number)
{
	assert(snprintf(id, JOURNAL_ID + 1, "%032x", number) == JOURNAL_ID);
}

static void
journal_test_thoughts (struct journal *j)
{
	struct loop events = loop();
	assert(!loop_init(&events));
	int const success = 0, mismatch = EILSEQ;
	char id[JOURNAL_ID + 1];
	/* More files than the descriptor cache holds: verification must survive
	 * eviction, even though none of these jobs has a recovery checkpoint. */
	for (unsigned k = 1; k <= JOURNAL_FILES + 1; ++k) {
		journal_test_id(id, k);
		assert(!journal_thought(j, id, 1, 0, "abc", 3, &events,
		                        journal_test_done, (void *)&success));
	}
	for (unsigned k = 1; k <= JOURNAL_FILES + 1; ++k) {
		journal_test_id(id, k);
		assert(!journal_thought_end(j, id, 1, true, &events,
		                            journal_test_done, (void *)&success));
	}
	assert(!journal_thought(j, id, 1, 0, nullptr, 0, &events,
	                        journal_test_done, (void *)&success));
	assert(!journal_thought(j, id, 1, 0, "a", 1, &events,
	                        journal_test_done, (void *)&success));
	assert(!journal_thought_end(j, id, 1, true, &events,
	                            journal_test_done, (void *)&mismatch));
	assert(!journal_thought(j, id, 1, 0, "x", 1, &events,
	                        journal_test_done, (void *)&mismatch));
	assert(!journal_thought_end(j, id, 1, true, &events,
	                            journal_test_done, (void *)&mismatch));
	assert(!journal_thought(j, id, 1, 0, nullptr, 0, &events,
	                        journal_test_done, (void *)&success));
	assert(!journal_thought(j, id, 1, 0, "abc", 3, &events,
	                        journal_test_done, (void *)&success));
	assert(!journal_thought_end(j, id, 1, true, &events,
	                            journal_test_done, (void *)&success));
	assert(!journal_thought(j, id, 1, 3, "def", 3, &events,
	                        journal_test_done, (void *)&success));
	journal_stop(j);
	loop_fini(&events);
	assert(journal_test_completed == 2 * (JOURNAL_FILES + 1) + 9);
	assert(!journal_job_find(j, id)->thought_error);
	assert(!journal_job_find(j, id)->terminal);
	struct journal_item load = {};
	memcpy(load.id, id, sizeof load.id);
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
		journal_test_id(record->id, k);
		memcpy(record->kind, "state", sizeof "state");
		record->len = JOURNAL_RECORD_MAX;
		assert(!journal_checkpoint_write(j, record));
		memcpy(record->kind, "meta", sizeof "meta");
		record->len = 1;
		assert(!journal_checkpoint_write(j, record));
	}
	free(record);
	record = nullptr;
	assert(j->recovery_bytes > JOURNAL_LOAD_MAX);
	struct journal_item load = {};
	journal_test_id(load.id, 2);
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
	struct journal_record other = { .kind = "state" };
	journal_test_id(r.id, 100);
	journal_test_id(other.id, 101);
	memcpy(r.kind, "state", sizeof "state");
	assert(!journal_checkpoint_write(j, &r));
	int jobs = j->jobs_fd;
	j->jobs_fd = -1;
	memcpy(r.kind, "tool-intent", sizeof "tool-intent");
	assert(journal_checkpoint_write(j, &r) == EBADF);
	j->jobs_fd = jobs;
	memcpy(r.kind, "tool-result", sizeof "tool-result");
	assert(journal_checkpoint_write(j, &r) == EBADF);
	assert(!journal_checkpoint_write(j, &other));
	memcpy(other.kind, "meta", sizeof "meta");
	assert(!journal_checkpoint_write(j, &other));
	memcpy(r.kind, "state", sizeof "state");
	assert(!journal_checkpoint_write(j, &r));
	memcpy(r.kind, "tool-intent", sizeof "tool-intent");
	assert(!journal_checkpoint_write(j, &r));
	j->jobs_fd = -1;
	assert(journal_checkpoint_write(j, &r) == EBADF);
	j->jobs_fd = jobs;
	memcpy(r.kind, "result", sizeof "result");
	assert(!journal_checkpoint_write(j, &r));
	assert(!journal_job_find(j, r.id)->error && journal_job_find(j, r.id)->terminal);
}

static void
journal_test_legacy (struct journal *j)
{
	struct journal_record record = { .kind = "state" };
	journal_test_id(record.id, JOURNAL_FILES + 1);
	j->wal_fd = memfd_create("journal-legacy-test", MFD_CLOEXEC);
	assert(j->wal_fd >= 0);
	assert(!journal_record_write(j, j->wal_fd, &record, 1));
	memcpy(record.kind, "stream", sizeof "stream");
	assert(!journal_record_write(j, j->wal_fd, &record, 2));
	struct journal_item load = {};
	memcpy(load.id, record.id, sizeof load.id);
	assert(!journal_load_records(j, &load));
	assert(load.record && !load.record->next && !strcmp(load.record->kind, "state"));
	journal_records_destroy(&load.record);
	memcpy(record.kind, "meta", sizeof "meta");
	assert(!journal_checkpoint_write(j, &record));
	assert(journal_job_find(j, record.id)->legacy);
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
		char id[JOURNAL_ID + 1];
		journal_test_id(id, k);
		struct journal_job *job = journal_job_enter(j, id, &e);
		assert(job && !e);
		job->terminal = true;
	}
	assert(j->n_jobs == JOURNAL_JOBS);
	journal_destroy(&j);
	journal_test_child_run();
	puts("Journal loads, write gaps, legacy migration, reasoning verification "
	     "and child_run(): PASS");
	return 0;
}
