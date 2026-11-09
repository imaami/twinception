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
	assert(!journal_job_find(j, &id)->thought.error);
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
		record->type = JOURNAL_STATE;
		record->len = JOURNAL_RECORD_MAX;
		assert(!journal_checkpoint_write(j, record));
		record->type = JOURNAL_META;
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
	assert(r && r->type == JOURNAL_STATE && r->len == JOURNAL_RECORD_MAX);
	assert(r->next && r->next->type == JOURNAL_META && !r->next->next);
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
	other.type = JOURNAL_STATE;
	r.type = JOURNAL_STATE;
	assert(!journal_checkpoint_write(j, &r));
	int jobs = j->jobs_fd;
	j->jobs_fd = -1;
	r.type = JOURNAL_TOOL_INTENT;
	assert(journal_checkpoint_write(j, &r) == EBADF);
	j->jobs_fd = jobs;
	r.type = JOURNAL_TOOL_RESULT;
	assert(journal_checkpoint_write(j, &r) == EBADF);
	assert(!journal_checkpoint_write(j, &other));
	other.type = JOURNAL_META;
	assert(!journal_checkpoint_write(j, &other));
	r.type = JOURNAL_STATE;
	assert(!journal_checkpoint_write(j, &r));
	r.type = JOURNAL_TOOL_INTENT;
	assert(!journal_checkpoint_write(j, &r));
	j->jobs_fd = -1;
	assert(journal_checkpoint_write(j, &r) == EBADF);
	j->jobs_fd = jobs;
	r.type = JOURNAL_RESULT;
	assert(!journal_checkpoint_write(j, &r));
	assert(!journal_job_find(j, &r.id)->error &&
	       journal_job_find(j, &r.id)->flags & JOB_TERMINAL);
}

static void
journal_test_legacy (struct journal *j)
{
	struct journal_record record = {};
	record.type = JOURNAL_STATE;
	journal_test_id(&record.id, JOURNAL_FILES + 1);
	j->wal_fd = memfd_create("journal-legacy-test", MFD_CLOEXEC);
	assert(j->wal_fd >= 0);
	assert(!journal_record_write(j, j->wal_fd, &record, 1));
	/* "stream" is not in JOURNAL_KIND_LIST, so journal_record_write() cannot
	 * write a record of that kind, and the test writes it with
	 * util_write_all(). journal_load_records() reads the record as
	 * JOURNAL_OTHER and skips it. */
	unsigned char header[JOURNAL_HEADER] = "ASKAIJ1\n";
	util_be_put(header + JOURNAL_AT_SEQUENCE, 2, sizeof (uint64_t));
	memcpy(header + JOURNAL_AT_ID, record.id.hex, HEXID_DIGITS);
	memcpy(header + JOURNAL_AT_KIND, "stream", sizeof "stream" - 1);
	assert(!journal_digest(j, header, record.data, 0, header + JOURNAL_SIGNED));
	assert(!util_write_all(j->wal_fd, header, sizeof header));
	struct journal_item load = { .id = record.id };
	assert(!journal_load_records(j, &load));
	assert(load.record && !load.record->next && load.record->type == JOURNAL_STATE);
	journal_records_destroy(&load.record);
	record.type = JOURNAL_META;
	assert(!journal_checkpoint_write(j, &record));
	assert(journal_job_find(j, &record.id)->flags & JOB_LEGACY);
	assert(!journal_load_records(j, &load));
	unsigned count = 0;
	for (struct journal_record *r = load.record; r; r = r->next) {
		assert(r->type != JOURNAL_OTHER);
		++count;
	}
	assert(count == 3); /* state, migration marker, and new metadata */
	journal_records_destroy(&load.record);
}

/* Run "sh -c @p script" with a deadline @p after_ms from now, or none if
 * 0, and a grace of @p grace_ms. If @p out is nullptr, the function runs
 * the script with child_run(). Otherwise it runs the script with
 * child_capture(), which appends up to @p max bytes of the output to
 * @p out. Returns the result of child_run() or child_capture(), and writes
 * how the child ended to @p *end and how long it ran to @p *ms.
 */
static int
journal_test_run (char const       *script,
                  uint64_t          after_ms,
                  uint64_t          grace_ms,
                  size_t            max,
                  struct buf       *out,
                  struct child_end *end,
                  uint64_t         *ms)
{
	char const *const argv[] = { "sh", "-c", script, nullptr };
	char *const envp[] = { "PATH=/usr/bin:/bin", nullptr };
	uint64_t start = loop_clock();
	struct child_cmd const cmd = {
		.file = "sh",
		.argv = argv,
		.envp = envp,
		.max = max,
		.deadline = after_ms ? start + after_ms * MS : 0,
		.grace = grace_ms * MS,
	};
	*end = (struct child_end){ .code = -1, .status = -1, .flags = UINT32_MAX };
	int e = out ? child_capture(&cmd, child_fds(), out, end) : child_run(&cmd, child_fds(), end);
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

/* Returns true if the readable bytes of @p out are @p text. */
static bool
journal_test_out (struct buf const *out,
                  struct cstr_view  text)
{
	struct span const got = buf_peek(out);
	return cstr_view_equal((struct cstr_view){ (char const *)got.ptr, got.len }, text);
}

/* Tests child_run() with exit statuses, a death by a signal, and a
 * deadline without a grace, with a grace that the command honors, and with
 * a grace that the command ignores. Each case asserts the error and the
 * struct child_end. A failed spawn is tested with child_run() and with
 * child_capture().
 */
static void
journal_test_child_run (void)
{
	struct child_end end;
	uint64_t ms;
	assert(!journal_test_run("exit 0", 0, 0, 0, nullptr, &end, &ms));
	assert(end.code == CLD_EXITED && end.status == 0 && !end.flags);
	assert(!journal_test_run("exit 3", 0, 0, 0, nullptr, &end, &ms));
	assert(end.code == CLD_EXITED && end.status == 3 && !end.flags);
	assert(journal_test_run("kill -KILL $$", 0, 0, 0, nullptr, &end, &ms) == EIO);
	assert(end.code == CLD_KILLED && end.status == SIGKILL && !end.flags);
	assert(journal_test_run("exec sleep 5", 100, 0, 0, nullptr, &end, &ms) == ETIMEDOUT);
	assert(end.code == CLD_KILLED && end.status == SIGKILL && end.flags == CHILD_LATE);
	assert(ms >= 100 && ms < 2000);
	assert(journal_test_run("exec sleep 5", 100, 3000, 0, nullptr, &end, &ms) == ETIMEDOUT);
	assert(end.code == CLD_KILLED && end.status == SIGTERM && end.flags == CHILD_LATE);
	assert(ms >= 100 && ms < 2000);
	assert(journal_test_run("trap '' TERM; exec sleep 5", 100, 300, 0, nullptr, &end, &ms) ==
	       ETIMEDOUT);
	assert(end.code == CLD_KILLED && end.status == SIGKILL && end.flags == CHILD_LATE);
	assert(ms >= 400 && ms < 2500);
	/* After a failed spawn, every member of the struct child_end is 0, and
	 * child_capture() appends nothing to the buffer. */
	struct child_cmd const none = {
		.file = "/nonexistent/askai-test",
		.argv = (char const *const[]){ "askai-test", nullptr },
		.envp = (char *const[]){ nullptr },
		.max = 3,
	};
	end = (struct child_end){ .code = -1, .status = -1, .flags = UINT32_MAX };
	assert(child_run(&none, child_fds(), &end) == ENOENT);
	assert(!end.code && !end.status && !end.flags);
	struct buf out = {};
	assert(!buf_put(&out, "x", 1, SIZE_MAX));
	end = (struct child_end){ .code = -1, .status = -1, .flags = UINT32_MAX };
	assert(child_capture(&none, child_fds(), &out, &end) == ENOENT);
	assert(!end.code && !end.status && !end.flags);
	assert(journal_test_out(&out, CSTR_VIEW("x")));
	buf_fini(&out);
}

/* Tests child_capture(): the output is appended to a buffer up to the cap,
 * a longer output is cut at the cap with EFBIG and CHILD_CUT, and after a
 * signal or the deadline the buffer does not change. Each case asserts the
 * error and the struct child_end. journal_test_child_run() tests a failed
 * spawn.
 */
static void
journal_test_child_capture (void)
{
	struct buf out = {};
	struct child_end end;
	uint64_t ms;
	assert(!journal_test_run("true", 0, 0, 0, &out, &end, &ms));
	assert(end.code == CLD_EXITED && end.status == 0 && !end.flags);
	assert(!buf_peek(&out).len);
	assert(!journal_test_run("printf abc", 0, 0, 3, &out, &end, &ms));
	assert(end.code == CLD_EXITED && end.status == 0 && !end.flags);
	assert(journal_test_out(&out, CSTR_VIEW("abc")));
	/* The cap applies to the output, not to the bytes already in out. */
	assert(!journal_test_run("printf de; exit 3", 0, 0, 2, &out, &end, &ms));
	assert(end.code == CLD_EXITED && end.status == 3 && !end.flags);
	assert(journal_test_out(&out, CSTR_VIEW("abcde")));
	assert(journal_test_run("printf fghi", 0, 0, 3, &out, &end, &ms) == EFBIG);
	assert(end.code == CLD_EXITED && end.status == 0 && end.flags == CHILD_CUT);
	assert(journal_test_out(&out, CSTR_VIEW("abcdefgh")));
	buf_clear(&out);
	assert(journal_test_run("printf a", 0, 0, 0, &out, &end, &ms) == EFBIG);
	assert(end.code == CLD_EXITED && end.flags == CHILD_CUT && !buf_peek(&out).len);
	/* Outputs of 64 KiB and of one byte more, with a cap of 64 KiB. Both
	 * buffers contain the first 64 KiB of the output. */
	static char const line[] = "abcdefg\n";
	assert(!journal_test_run("yes abcdefg | head -c 65536", 0, 0, 64 << 10, &out, &end, &ms));
	assert(end.code == CLD_EXITED && end.status == 0 && !end.flags);
	struct span got = buf_peek(&out);
	assert(got.len == 64 << 10);
	for (size_t i = 0; i < got.len; ++i)
		assert(got.ptr[i] == line[i % (sizeof line - 1)]);
	buf_clear(&out);
	assert(journal_test_run("yes abcdefg | head -c 65537", 0, 0, 64 << 10, &out, &end, &ms) ==
	       EFBIG);
	assert(end.code == CLD_EXITED && end.status == 0 && end.flags == CHILD_CUT);
	got = buf_peek(&out);
	assert(got.len == 64 << 10);
	for (size_t i = 0; i < got.len; ++i)
		assert(got.ptr[i] == line[i % (sizeof line - 1)]);
	/* After a signal or the deadline, the output is not read. */
	buf_clear(&out);
	assert(!buf_put(&out, "x", 1, SIZE_MAX));
	assert(journal_test_run("printf abc; kill -KILL $$", 0, 0, 3, &out, &end, &ms) == EIO);
	assert(end.code == CLD_KILLED && end.status == SIGKILL && !end.flags);
	assert(journal_test_out(&out, CSTR_VIEW("x")));
	assert(journal_test_run("printf abc; exec sleep 5", 100, 0, 3, &out, &end, &ms) ==
	       ETIMEDOUT);
	assert(end.code == CLD_KILLED && end.status == SIGKILL && end.flags == CHILD_LATE);
	assert(ms >= 100 && ms < 2000);
	assert(journal_test_out(&out, CSTR_VIEW("x")));
	buf_fini(&out);
}

/* Tests the reason that child_end_why() writes for each kind of end. The
 * late test comes first and the cut test second. For an exit with status
 * 0, the view is empty.
 */
static void
journal_test_child_end_why (void)
{
	static struct {
		struct child_end end;
		struct cstr_view why;
	} const cases[] = {
		{ { CLD_EXITED, 0, 0 },
		  {} },
		{ { CLD_EXITED, 2, 0 },
		  CSTR_VIEW_INIT("git exited with status 2.") },
		{ { CLD_KILLED, SIGKILL, 0 },
		  CSTR_VIEW_INIT("git was killed by signal 9.") },
		{ { CLD_DUMPED, SIGSEGV, 0 },
		  CSTR_VIEW_INIT("git was killed by signal 11.") },
		{ { 0, 0, 0 },
		  CSTR_VIEW_INIT("git ended, and why is unknown.") },
		{ { CLD_KILLED, SIGKILL, CHILD_CUT },
		  CSTR_VIEW_INIT("git wrote more than 4 MiB.") },
		{ { CLD_EXITED, 0, CHILD_CUT },
		  CSTR_VIEW_INIT("git wrote more than 4 MiB.") },
		{ { CLD_KILLED, SIGKILL, CHILD_LATE },
		  CSTR_VIEW_INIT("git took longer than 10 s, and was stopped.") },
		{ { CLD_KILLED, SIGKILL, CHILD_CUT | CHILD_LATE },
		  CSTR_VIEW_INIT("git took longer than 10 s, and was stopped.") },
	};
	for (size_t i = 0; i < array_size(cases); ++i) {
		char buf[64];
		struct cstr_view const why = child_end_why(&cases[i].end, "git", 10 * NS,
		                                           (4 << 20) + 4096, buf, sizeof buf);
		assert(cstr_view_equal(why, cases[i].why) && !why.data == !cases[i].why.data);
	}
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
	journal_test_child_capture();
	journal_test_child_end_why();
	puts("Journal loads, write gaps, legacy migration, reasoning verification, "
	     "path walks, private files, child_run(), child_capture() and "
	     "child_end_why(): PASS");
	return 0;
}
