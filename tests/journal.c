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
 * the script with child_cmd_run() and no buffer. Otherwise it runs the
 * script with child_capture(), which appends up to @p max bytes of the
 * output to @p out. Returns the result of child_cmd_run() or
 * child_capture(), and writes how the child ended to @p *end and how long
 * it ran to @p *ms.
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
	*end = child_end(.code = -1, .status = -1, .flags = UINT32_MAX);
	int e = out ? child_capture(&cmd, child_fds(), out, end)
	            : child_cmd_run(&cmd, child_fds(), nullptr, end);
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
	struct cstr_view part = {};
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

/* Tests journal_secret() on a file of the repository @p repo. A new file
 * and an empty file get random bytes that are not all zero, and the next
 * call reads the same bytes. journal_secret() refuses a file of another
 * size, a file of zero bytes, a file that other users can access, a file
 * with two links and a directory. The reason is the path of the file and
 * the problem, cut at the size of its buffer. A call that succeeds writes
 * no reason.
 */
static void
journal_test_secret (char const *repo)
{
	static char const name[] = "secret.test";
	int dir = open(repo, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(dir >= 0);
	unsigned char made[32] = {};
	unsigned char got[32] = {};
	char why[96] = "none";
	assert(!journal_secret(dir, "shown", name, made, sizeof made, why, sizeof why));
	struct stat st;
	assert(!fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) &&
	       st.st_size == (off_t)sizeof made && (st.st_mode & 0777) == 0600);
	assert(!journal_zero(made, sizeof made) && !strcmp(why, "none"));
	assert(!journal_secret(dir, "shown", name, got, sizeof got, why, sizeof why));
	assert(!memcmp(made, got, sizeof made) && !strcmp(why, "none"));
	int fd = openat(dir, name, O_RDWR | O_CLOEXEC);
	assert(fd >= 0 && !ftruncate(fd, 0));
	memset(got, 0, sizeof got);
	assert(!journal_secret(dir, "shown", name, got, 8, why, sizeof why));
	assert(!journal_zero(got, 8) && journal_zero(got + 8, sizeof got - 8) &&
	       !fstat(fd, &st) && st.st_size == 8 && !strcmp(why, "none"));
	/* Another size, also with a reason that does not fit. */
	assert(journal_secret(dir, "shown", name, got, sizeof got, why, sizeof why) == EILSEQ);
	assert(!strcmp(why, "shown/secret.test: the file has 8 bytes, not 32"));
	char cut[32] = "";
	assert(journal_secret(dir, "a/b", name, got, 7, cut, sizeof cut) == EILSEQ);
	assert(!strcmp(cut, "a/b/secret.test: the file ha..."));
	static unsigned char const zero[8];
	assert(pwrite(fd, zero, sizeof zero, 0) == (ssize_t)sizeof zero);
	assert(journal_secret(dir, "shown", name, got, sizeof zero, why, sizeof why) == EILSEQ);
	assert(!strcmp(why, "shown/secret.test: the file has only zero bytes"));
	assert(!fchmod(fd, 0640));
	assert(journal_secret(dir, "shown", name, got, sizeof zero, why, sizeof why) == EACCES);
	assert(!strcmp(why, "shown/secret.test: the mode of the file lets other users access it"));
	assert(!fchmod(fd, 0600) && !linkat(dir, name, dir, "secret.link", 0));
	assert(journal_secret(dir, "shown", name, got, sizeof zero, why, sizeof why) == EACCES);
	assert(!strcmp(why, "shown/secret.test: the file has more than one hard link"));
	assert(!unlinkat(dir, "secret.link", 0) && !unlinkat(dir, name, 0));
	util_close(&fd);
	/* Any other error is named by the text of its number. */
	char want[96];
	assert(snprintf(want, sizeof want, "shown/secret.test: %s", strerror(EISDIR)) > 0);
	assert(!mkdirat(dir, name, 0700));
	assert(journal_secret(dir, "shown", name, got, sizeof zero, why, sizeof why) == EISDIR);
	assert(!strcmp(why, want) && !unlinkat(dir, name, AT_REMOVEDIR));
	util_close(&dir);
}

/* Tests util_dir_open() and util_dir_next() on the repository @p repo. The
 * stream reads every entry through a new descriptor, so the offset of the
 * descriptor of the caller stays 0, and closedir() leaves that descriptor
 * open. util_dir_open() returns EBADF for a bad descriptor and ENOTDIR for
 * a file.
 */
static void
journal_test_dir (char const *repo)
{
	int fd = open(repo, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(fd >= 0);
	int e = -1;
	DIR *dir = util_dir_open(fd, &e);
	assert(dir && !e);
	size_t dots = 0;
	size_t n = 0;
	for (struct dirent *entry; (entry = util_dir_next(dir, &e)); ++n)
		dots += !strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..");
	assert(!e && dots == 2 && n > 2 && !closedir(dir));
	assert(lseek(fd, 0, SEEK_CUR) == 0 && fcntl(fd, F_GETFD) >= 0);
	e = 0;
	assert(!util_dir_open(-1, &e) && e == EBADF);
	int file = openat(fd, "HEAD", O_RDONLY | O_CLOEXEC);
	assert(file >= 0);
	e = 0;
	assert(!util_dir_open(file, &e) && e == ENOTDIR);
	util_close(&file);
	util_close(&fd);
}

/* Returns true if the readable bytes of @p out are @p text. */
static bool
journal_test_out (struct buf const *out,
                  struct cstr_view  text)
{
	return cstr_view_equal(cstr_view_span(buf_peek(out)), text);
}

/* Tests util_parent_open() and journal_unlock() in a new directory of the
 * repository @p repo. util_parent_open() refuses an empty or absolute
 * path, a path with a ".." part, a part longer than NAME_MAX bytes and a
 * path whose last part is empty or ".", and leaves the path as it was.
 * journal_unlock() removes the regular lock file of the participant, and
 * returns 0 if there is no lock file. journal_unlock() refuses a lock file
 * that is a symbolic link, a directory or a file with two links, and a
 * directory of the ref that is a symbolic link, and leaves every file as
 * it was.
 */
static void
journal_test_unlock (char const *repo)
{
	int top = open(repo, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(top >= 0 && !mkdirat(top, "unlock", 0700));
	int dir = openat(top, "unlock", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	util_close(&top);
	assert(dir >= 0);

	static char const *const invalid[] = {
		"", ".", "..", "/a", "a/", "a/.", "a/..", "../a", "a/../b", "./", "a//"
	};
	char path[NAME_MAX + 1 + sizeof "/a"];
	char const *leaf = nullptr;
	int fd = -1;
	for (size_t i = 0; i < array_size(invalid); ++i) {
		strcpy(path, invalid[i]);
		fd = 0;
		assert(util_parent_open(dir, path, &leaf, &fd) == EINVAL && fd == -1);
		assert(!strcmp(path, invalid[i]));
	}
	memset(path, 'a', NAME_MAX + 1);
	memcpy(path + NAME_MAX + 1, "/a", sizeof "/a");
	assert(util_parent_open(dir, path, &leaf, &fd) == EINVAL && fd == -1);
	assert(strlen(path) == NAME_MAX + 3 && path[NAME_MAX + 1] == '/');
	strcpy(path, "refs/heads/a.lock");
	assert(util_parent_open(dir, path, &leaf, &fd) == ENOENT && fd == -1);
	assert(!strcmp(path, "refs/heads/a.lock"));
	/* For a path of one part, util_parent_open() opens a copy of the
	 * descriptor of the directory. */
	strcpy(path, "a.lock");
	struct stat st, at;
	assert(!util_parent_open(dir, path, &leaf, &fd) && leaf == path && fd >= 0 && fd != dir);
	assert(!fstat(fd, &st) && !fstat(dir, &at) && st.st_ino == at.st_ino && st.st_dev == at.st_dev);
	util_close(&fd);

	struct journal *j = calloc(1, sizeof *j);
	assert(j);
	j->repo_fd = dir;
	(void)util_format(j->ref, sizeof j->ref, "%stest", REF);
	assert(!journal_unlock(j));
	assert(!mkdirat(dir, "refs", 0700) && !mkdirat(dir, "refs/heads", 0700) &&
	       !mkdirat(dir, "refs/heads/reasoning", 0700));
	assert(!journal_unlock(j));
	strcpy(path, ".//refs//heads/./reasoning/test.lock");
	assert(!util_parent_open(dir, path, &leaf, &fd) && leaf == path + strlen(path) - 9);
	assert(!strcmp(path, ".//refs//heads/./reasoning/test.lock"));
	assert(!fstat(fd, &st) && !fstatat(dir, "refs/heads/reasoning", &at, AT_SYMLINK_NOFOLLOW) &&
	       st.st_ino == at.st_ino);
	util_close(&fd);

	/* A regular lock file is removed. */
	fd = openat(dir, "refs/heads/reasoning/test.lock", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
	            0600);
	assert(fd >= 0);
	util_close(&fd);
	assert(!journal_unlock(j));
	assert(fstatat(dir, "refs/heads/reasoning/test.lock", &st, AT_SYMLINK_NOFOLLOW) &&
	       errno == ENOENT);
	/* A lock file with two links, a directory and a symbolic link stay. */
	fd = openat(dir, "refs/heads/reasoning/test.lock", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
	            0600);
	assert(fd >= 0);
	util_close(&fd);
	assert(!linkat(dir, "refs/heads/reasoning/test.lock", dir, "other.lock", 0));
	assert(journal_unlock(j) == EACCES);
	assert(!fstatat(dir, "refs/heads/reasoning/test.lock", &st, AT_SYMLINK_NOFOLLOW) &&
	       st.st_nlink == 2);
	assert(!unlinkat(dir, "refs/heads/reasoning/test.lock", 0));
	assert(!mkdirat(dir, "refs/heads/reasoning/test.lock", 0700));
	assert(journal_unlock(j) == EACCES);
	assert(!unlinkat(dir, "refs/heads/reasoning/test.lock", AT_REMOVEDIR));
	assert(!symlinkat("../../../other.lock", dir, "refs/heads/reasoning/test.lock"));
	assert(journal_unlock(j) == EACCES);
	assert(!fstatat(dir, "refs/heads/reasoning/test.lock", &st, AT_SYMLINK_NOFOLLOW) &&
	       S_ISLNK(st.st_mode));
	assert(!fstatat(dir, "other.lock", &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode));
	assert(!unlinkat(dir, "refs/heads/reasoning/test.lock", 0));
	/* A directory of the ref that is a symbolic link is not opened. */
	assert(!renameat(dir, "refs/heads/reasoning", dir, "refs/heads/real"));
	assert(!symlinkat("real", dir, "refs/heads/reasoning"));
	assert(!linkat(dir, "other.lock", dir, "refs/heads/real/test.lock", 0));
	assert(!unlinkat(dir, "other.lock", 0));
	assert(journal_unlock(j) == ENOTDIR);
	assert(!fstatat(dir, "refs/heads/real/test.lock", &st, AT_SYMLINK_NOFOLLOW) &&
	       S_ISREG(st.st_mode));
	assert(!unlinkat(dir, "refs/heads/reasoning", 0));
	assert(!symlinkat("..", dir, "refs/heads/reasoning"));
	assert(journal_unlock(j) == ENOTDIR);
	strcpy(path, "refs/heads/reasoning/test.lock");
	assert(util_parent_open(dir, path, &leaf, &fd) == ENOTDIR && fd == -1);
	assert(!strcmp(path, "refs/heads/reasoning/test.lock"));
	free(j);
	util_close(&dir);
}

/* The words of each git command of the journal after "git" and before
 * the words of the command. tests/git_run.c checks the words of
 * GIT_OPTIONS_FIXED.
 */
static char const *const journal_test_prefix[] = {
	GIT_OPTIONS_FIXED, "-c", "core.fsync=all", "-c", "core.fsyncMethod=fsync", "--git-dir=.",
	nullptr
};

/* Returns true if the text at @p *at starts with the words of
 * journal_test_prefix and the words at @p words, one word a line, and an
 * empty line, as the git wrapper of journal_test_argv() writes them.
 * Moves @p *at past those lines.
 */
static bool
journal_test_logged (char const        **at,
                     char const *const  *words)
{
	char const *const *const parts[] = { journal_test_prefix, words };
	for (size_t i = 0; i < array_size(parts); ++i) {
		for (char const *const *w = parts[i]; *w; ++w) {
			size_t const n = strlen(*w);
			if (strncmp(*at, *w, n) || (*at)[n] != '\n')
				return false;
			*at += n + 1;
		}
	}
	if (**at != '\n')
		return false;
	++*at;
	return true;
}

/* Pins the argv of each git command of the journal. A git wrapper first
 * on PATH writes the words of each command after "git" to a log, then
 * runs git. Three journals of the participant "pin" run one after the
 * other in a new repository in @p repo, and each one commits the reasoning
 * of one job. The first journal makes the repository. The second journal
 * checks whether the tip already contains the file of its job. For the
 * third journal, the wrapper exits with 128 for that check, and the
 * journal commits, because the check only decides whether the commit can
 * be skipped.
 */
static void
journal_test_argv (char const *repo)
{
	char *real = realpath(repo, nullptr);
	char *bin = nullptr;
	char *wrapper = nullptr;
	char *log = nullptr;
	char *pin = nullptr;
	char *path = nullptr;
	char *fail = nullptr;
	assert(real && asprintf(&bin, "%s/bin", real) > 0 && !mkdir(bin, 0700));
	assert(asprintf(&wrapper, "%s/git", bin) > 0 && asprintf(&log, "%s/git.log", real) > 0);
	assert(asprintf(&pin, "%s/pin.git", real) > 0 && asprintf(&fail, "%s/fail", real) > 0);
	FILE *f = fopen(wrapper, "w");
	assert(f);
	fprintf(f, "#!/bin/sh\nprintf '%%s\\n' \"$@\" '' >> '%s'\n"
	           "case \"$*\" in *:reasoning/*) [ -e '%s' ] && exit 128;; esac\n"
	           "PATH=${PATH#*:}\nexport PATH\nexec git \"$@\"\n", log, fail);
	assert(!fclose(f) && !chmod(wrapper, 0700));
	char const *const old = getenv("PATH");
	char *saved = strdup(old ? old : "/usr/bin:/bin");
	assert(saved && asprintf(&path, "PATH=%s:%s", bin, saved) > 0);
	assert(!setenv("PATH", path + sizeof "PATH=" - 1, 1));
	struct oid tips[3] = {};
	for (unsigned k = 1; k <= array_size(tips); ++k) {
		if (k == 3) {
			int fd = open(fail, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
			assert(fd >= 0);
			util_close(&fd);
		}
		char why[256];
		int e = 0;
		struct journal *j = journal_create(pin, "pin", why, sizeof why, &e);
		assert(j && !e);
		assert(!j->git.path && j->git.opts == git_options && j->git.flags == GIT_RUN_DOT);
		assert(!strcmp(j->git.env[0], path) && !strcmp(j->git.env[1], "HOME=/nonexistent"));
		struct loop events = loop();
		assert(!loop_init(&events));
		int const success = 0;
		struct hexid id;
		journal_test_id(&id, k);
		assert(!journal_thought(j, &id, 1, 0, "abc", 3, &events,
		                        journal_test_done, (void *)&success));
		assert(!journal_thought_end(j, &id, 1, true, &events,
		                            journal_test_done, (void *)&success));
		journal_stop(j);
		loop_fini(&events);
		assert(oid_len(&j->tip) && (k == 1 || !oid_equal(&j->tip, &tips[k - 2])));
		tips[k - 1] = j->tip;
		journal_destroy(&j);
	}
	assert(!setenv("PATH", saved, 1));
	static char const *const init[] = {
		"init", "--bare", "--quiet", "--template=", "--initial-branch=reasoning", "--", ".",
		nullptr
	};
	static char const *const bare[] = { "rev-parse", "--is-bare-repository", nullptr };
	static char const *const head[] = {
		"rev-parse", "--verify", "--quiet", "refs/heads/reasoning/pin", nullptr
	};
	static char const *const hash[] = { "hash-object", "-w", "--stdin", nullptr };
	static char const *const import[] = { "fast-import", "--quiet", "--done", nullptr };
	char *names[2] = {};
	for (size_t i = 0; i < array_size(names); ++i) {
		struct hexid id;
		journal_test_id(&id, (unsigned)i + 2);
		assert(asprintf(&names[i], "%s:reasoning/%.2s/%s/1.txt", tips[i].hex, id.hex,
		                id.hex) > 0);
	}
	char const *const find[] = { "rev-parse", "--verify", "--quiet", names[0], nullptr };
	char const *const again[] = { "rev-parse", "--verify", "--quiet", names[1], nullptr };
	char const *const *const runs[] = {
		init, bare, head, hash, import, head,
		bare, head, hash, find, import, head,
		bare, head, hash, again, import, head,
	};
	static char text[1 << 14];
	assert(!util_read_text(log, text, sizeof text));
	char const *at = text;
	for (size_t i = 0; i < array_size(runs); ++i)
		assert(journal_test_logged(&at, runs[i]));
	assert(!*at);
	for (size_t i = 0; i < array_size(names); ++i)
		free(names[i]);
	free(fail);
	free(saved);
	free(path);
	free(pin);
	free(log);
	free(wrapper);
	free(bin);
	free(real);
}

/* Tests journal_git() and journal_tip() with the journal @p j after
 * journal_stop(): the name of an object, an empty name and an empty tip
 * for a missing ref, and EIO for another exit status, for the exit status
 * 1 without a name to read and for an output that is not a name. A git
 * that cannot be spawned is ENOENT, and journal_tip() then leaves the tip
 * as it was.
 */
static void
journal_test_git (struct journal *j)
{
	struct oid tip = j->tip;
	struct oid oid;
	assert(oid_len(&tip));
	assert(!journal_git(j, (char const *[]){ "rev-parse", "--verify", "--quiet", j->ref, nullptr },
	                    -1, &oid) && oid_equal(&oid, &tip));
	oid = tip;
	assert(!journal_git(j, (char const *[]){ "rev-parse", "--verify", "--quiet",
	                                         "refs/heads/reasoning/missing", nullptr },
	                    -1, &oid) && !oid_len(&oid));
	assert(journal_git(j, (char const *[]){ "rev-parse", "--verify", "--quiet",
	                                        "refs/heads/reasoning/missing", nullptr },
	                   -1, nullptr) == EIO);
	oid = tip;
	assert(journal_git(j, (char const *[]){ "rev-parse", "--verify", "missing", nullptr },
	                   -1, &oid) == EIO && !oid_len(&oid));
	oid = tip;
	assert(journal_git(j, (char const *[]){ "rev-parse", "--is-bare-repository", nullptr },
	                   -1, &oid) == EIO && !oid_len(&oid));
	assert(!journal_git(j, (char const *[]){ "rev-parse", "--is-bare-repository", nullptr },
	                    -1, nullptr));
	assert(journal_test_out(&j->out, CSTR_VIEW("true\n")));
	char const ref[] = "refs/heads/reasoning/missing";
	static_assert(sizeof ref <= sizeof j->ref);
	char saved[sizeof j->ref];
	memcpy(saved, j->ref, sizeof saved);
	memcpy(j->ref, ref, sizeof ref);
	assert(!journal_tip(j) && !oid_len(&j->tip));
	memcpy(j->ref, saved, sizeof saved);
	assert(!journal_tip(j) && oid_equal(&j->tip, &tip));
	char *const path = strdup(getenv("PATH"));
	assert(path && !setenv("PATH", "/nonexistent", 1));
	assert(journal_tip(j) == ENOENT && oid_equal(&j->tip, &tip));
	assert(!setenv("PATH", path, 1));
	free(path);
}

/* Tests the run of a child without a buffer, through child_cmd_run(),
 * with exit statuses, a death by a signal, and a deadline without a grace,
 * with a grace that the command honors, and with a grace that the command
 * ignores. Each case asserts the error and the struct child_end. A failed
 * spawn is tested with child_cmd_run() and with child_capture().
 */
static void
journal_test_child_run (void)
{
	struct child_end end;
	uint64_t ms;
	assert(!journal_test_run("exit 0", 0, 0, 0, nullptr, &end, &ms));
	assert(end.code == CLD_EXITED && end.status == 0 && !end.flags);
	assert(journal_test_run("exit 3", 0, 0, 0, nullptr, &end, &ms) == EIO);
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
	end = child_end(.code = -1, .status = -1, .flags = UINT32_MAX);
	assert(child_cmd_run(&none, child_fds(), nullptr, &end) == ENOENT);
	assert(!end.code && !end.status && !end.flags);
	struct buf out = buf();
	assert(!buf_put(&out, "x", 1, SIZE_MAX));
	end = child_end(.code = -1, .status = -1, .flags = UINT32_MAX);
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
	struct buf out = buf();
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

/* Tests child_cmd_run() with a buffer and with a file as fds.out. The
 * error is EIO for an exit status other than 0, and the error of the run
 * or of the read comes before that EIO. After EIO, the exit status 1 and
 * the signal number 1 differ in the code of the struct child_end. With a
 * buffer, the output replaces the bytes that were in the buffer, and
 * those bytes are also removed if the child is not spawned. The cap is 3
 * bytes, and the whole output goes to the file. A deadline ends the
 * child, also a deadline in the past. Each case asserts the error and the
 * struct child_end.
 */
static void
journal_test_child_cmd_run (void)
{
	static struct {
		char const       *script;
		int               e;      /* with a buffer */
		int               plain;  /* with a file */
		struct child_end  end;    /* with a buffer */
		struct cstr_view  out;    /* the buffer */
		struct cstr_view  all;    /* the file */
	} const cases[] = {
		{ "printf abc", 0, 0, { CLD_EXITED, 0, 0 },
		  CSTR_VIEW_INIT("abc"), CSTR_VIEW_INIT("abc") },
		{ "printf abc; exit 1", EIO, EIO, { CLD_EXITED, 1, 0 },
		  CSTR_VIEW_INIT("abc"), CSTR_VIEW_INIT("abc") },
		{ "printf abc; exit 3", EIO, EIO, { CLD_EXITED, 3, 0 },
		  CSTR_VIEW_INIT("abc"), CSTR_VIEW_INIT("abc") },
		{ "printf abcd", EFBIG, 0, { CLD_EXITED, 0, CHILD_CUT },
		  CSTR_VIEW_INIT("abc"), CSTR_VIEW_INIT("abcd") },
		{ "printf abcd; exit 3", EFBIG, EIO, { CLD_EXITED, 3, CHILD_CUT },
		  CSTR_VIEW_INIT("abc"), CSTR_VIEW_INIT("abcd") },
		{ "printf abc; kill -HUP $$", EIO, EIO, { CLD_KILLED, SIGHUP, 0 },
		  {}, CSTR_VIEW_INIT("abc") },
		{ "printf abc; kill -KILL $$", EIO, EIO, { CLD_KILLED, SIGKILL, 0 },
		  {}, CSTR_VIEW_INIT("abc") },
	};
	static_assert(SIGHUP == 1, "the signal number that equals the exit status 1");
	struct child_end const unset = child_end(.code = -1, .status = -1, .flags = UINT32_MAX);
	char *const envp[] = { "PATH=/usr/bin:/bin", nullptr };
	struct buf out = buf();
	for (size_t i = 0; i < array_size(cases); ++i) {
		char const *const argv[] = { "sh", "-c", cases[i].script, nullptr };
		struct child_cmd const cmd = { .file = "sh", .argv = argv, .envp = envp, .max = 3 };
		struct child_end const *const want = &cases[i].end;
		assert(!buf_put(&out, "x", 1, SIZE_MAX));
		struct child_end end = unset;
		assert(child_cmd_run(&cmd, child_fds(), &out, &end) == cases[i].e);
		assert(end.code == want->code && end.status == want->status &&
		       end.flags == want->flags);
		assert(journal_test_out(&out, cases[i].out));
		/* A file has no cap, so the output is not cut. */
		struct child_fds fds = child_fds();
		fds.out = memfd_create("askai-journal-test", MFD_CLOEXEC);
		assert(fds.out >= 0);
		end = unset;
		assert(child_cmd_run(&cmd, fds, nullptr, &end) == cases[i].plain);
		assert(end.code == want->code && end.status == want->status && !end.flags);
		char text[8];
		struct cstr_view const all = cases[i].all;
		assert(pread(fds.out, text, sizeof text, 0) == (ssize_t)all.len &&
		       !memcmp(text, all.data, all.len));
		util_close(&fds.out);
	}
	/* The deadline comes 100 ms after the start, before the exit. */
	char const *const slow[] = { "sh", "-c", "printf abc; exec sleep 5", nullptr };
	struct child_cmd late = { .file = "sh", .argv = slow, .envp = envp, .max = 3 };
	assert(!buf_put(&out, "x", 1, SIZE_MAX));
	late.deadline = loop_clock() + 100 * MS;
	struct child_end end = unset;
	assert(child_cmd_run(&late, child_fds(), &out, &end) == ETIMEDOUT);
	assert(end.code == CLD_KILLED && end.status == SIGKILL && end.flags == CHILD_LATE);
	assert(!buf_peek(&out).len);
	late.deadline = loop_clock() + 100 * MS;
	end = unset;
	assert(child_cmd_run(&late, child_fds(), nullptr, &end) == ETIMEDOUT);
	assert(end.code == CLD_KILLED && end.status == SIGKILL && end.flags == CHILD_LATE);
	/* A deadline in the past ends the child at once. */
	assert(!buf_put(&out, "x", 1, SIZE_MAX));
	late.deadline = 1;
	end = unset;
	uint64_t const start = loop_clock();
	assert(child_cmd_run(&late, child_fds(), &out, &end) == ETIMEDOUT);
	assert(end.code == CLD_KILLED && end.status == SIGKILL && end.flags == CHILD_LATE);
	assert(loop_clock() - start < 2000 * MS && !buf_peek(&out).len);
	/* A child that is not spawned. */
	struct child_cmd const none = {
		.file = "/nonexistent/askai-test",
		.argv = (char const *const[]){ "askai-test", nullptr },
		.envp = (char *const[]){ nullptr },
		.max = 3,
	};
	assert(!buf_put(&out, "x", 1, SIZE_MAX));
	end = unset;
	assert(child_cmd_run(&none, child_fds(), &out, &end) == ENOENT);
	assert(!end.code && !end.status && !end.flags && !buf_peek(&out).len);
	end = unset;
	assert(child_cmd_run(&none, child_fds(), nullptr, &end) == ENOENT);
	assert(!end.code && !end.status && !end.flags);
	buf_fini(&out);
}

/* Tests the reason that child_end_why() writes and the status that
 * child_end_status() returns for each kind of end. The late test comes
 * first and the cut test second. For an exit with status 0, the view is
 * empty. The status is the exit status of a child that exited before its
 * deadline and whose output was not cut, and else -1. child_status() and
 * child_ok() read the end of a child in the same way.
 */
static void
journal_test_child_end_why (void)
{
	static struct {
		struct child_end end;
		int              status;
		struct cstr_view why;
	} const cases[] = {
		{ { CLD_EXITED, 0, 0 }, 0,
		  {} },
		{ { CLD_EXITED, 2, 0 }, 2,
		  CSTR_VIEW_INIT("git exited with status 2.") },
		{ { CLD_KILLED, SIGKILL, 0 }, -1,
		  CSTR_VIEW_INIT("git was killed by signal 9.") },
		{ { CLD_DUMPED, SIGSEGV, 0 }, -1,
		  CSTR_VIEW_INIT("git was killed by signal 11.") },
		{ { 0, 0, 0 }, -1,
		  CSTR_VIEW_INIT("git ended, and why is unknown.") },
		{ { CLD_KILLED, SIGKILL, CHILD_CUT }, -1,
		  CSTR_VIEW_INIT("git wrote more than 4 MiB.") },
		{ { CLD_EXITED, 0, CHILD_CUT }, -1,
		  CSTR_VIEW_INIT("git wrote more than 4 MiB.") },
		{ { CLD_KILLED, SIGKILL, CHILD_LATE }, -1,
		  CSTR_VIEW_INIT("git took longer than 10 s, and was stopped.") },
		{ { CLD_KILLED, SIGKILL, CHILD_CUT | CHILD_LATE }, -1,
		  CSTR_VIEW_INIT("git took longer than 10 s, and was stopped.") },
		{ { CLD_EXITED, 0, CHILD_LATE }, -1,
		  CSTR_VIEW_INIT("git took longer than 10 s, and was stopped.") },
	};
	for (size_t i = 0; i < array_size(cases); ++i) {
		char buf[64];
		struct cstr_view const why = child_end_why(&cases[i].end, "git", 10 * NS,
		                                           (4 << 20) + 4096, buf, sizeof buf);
		assert(cstr_view_equal(why, cases[i].why) && !why.data == !cases[i].why.data);
		assert(child_end_status(&cases[i].end) == cases[i].status &&
		       !why.len == !cases[i].status);
		struct child const c = { .end = cases[i].end };
		assert(child_status(&c) == cases[i].status && child_ok(&c) == !cases[i].status);
	}
	assert(!child_ok(nullptr));
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
	journal_test_git(j);
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
	journal_test_secret(argv[1]);
	journal_test_dir(argv[1]);
	journal_test_child_run();
	journal_test_child_capture();
	journal_test_child_cmd_run();
	journal_test_child_end_why();
	journal_test_unlock(argv[1]);
	journal_test_argv(argv[1]);
	puts("Journal loads, write gaps, legacy migration, reasoning verification, "
	     "path walks, private files, secret files, directory streams, child_capture(), "
	     "child_cmd_run(), child_end_why(), child_end_status(), journal_git(), "
	     "lock removal and the argv of git: PASS");
	return 0;
}
