/** @file Unit tests of the members of the session, src/member.c: the name
 *  rule, the logins of clients, the session record and the record of the
 *  agent that the registry writes as askai starts, also for services that
 *  start at the same time, and the lookups of a table, with git on a loop
 *  and names from blockers, which find, create, adopt, extend and refuse
 *  member records in new repositories. The test replaces mcp_slot_spawn()
 *  and mcp_slot_loop(), which src/store.c calls.
 */
#define _GNU_SOURCE /* memmem() */
#include <errno.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "blocker.h"
#include "buf.h"
#include "check.h"
#include "child.h"
#include "hexid.h"
#include "list.h"
#include "loop.h"
#include "mcp.h"
#include "member.h"
#include "store.h"
#include "util.h"

struct loop *
mcp_slot_loop (struct mcp_slot const *)
{
	CHECK(!"mcp_slot_loop() runs");
	return nullptr;
}

struct child *
mcp_slot_spawn (struct mcp_slot        *,
                struct child_cmd const *,
                int                    *e)
{
	CHECK(!"mcp_slot_spawn() runs");
	*e = ENOSYS;
	return nullptr;
}

/* The loop of the tables, and the number of waits that ended. */
static struct loop test_loop;
static unsigned ended;

static void
test_ended (struct member_wait *)
{
	++ended;
}

/* Runs the loop until @p n waits have ended in all, for 10 s at most. */
static void
test_until (unsigned n)
{
	uint64_t const deadline = loop_clock() + 10 * NS;
	while (ended < n && loop_clock() < deadline)
		CHECK(!loop_turn(&test_loop, deadline));
	CHECK(ended == n);
}

/* Writes to @p out the commit of @p ref in @p store, as git cat-file
 * prints it, or nothing if the ref does not exist.
 */
static void
test_record (struct store const *store,
             char const         *ref,
             struct buf         *out)
{
	char line[256];
	int n = snprintf(line, sizeof line, "%s\n", ref);
	CHECK(n > 0 && (size_t)n < sizeof line);
	buf_clear(out);
	int status = -1;
	CHECK(!store_run(store, STORE_READ, nullptr, nullptr,
	                 (struct span){ (unsigned char *)line, (size_t)n }, out, &status) &&
	      !status);
}

/* Returns true if the commit of @p ref in @p store contains @p text, and
 * else prints the commit if @p loud is true.
 */
static bool
test_in (struct store const *store,
         char const         *ref,
         char const         *text,
         bool                loud)
{
	struct buf out = {};
	test_record(store, ref, &out);
	struct span const s = buf_peek(&out);
	bool const found = s.len && memmem(s.ptr, s.len, text, strlen(text));
	if (!found && loud)
		fprintf(stderr, "no \"%s\" in %s:\n%.*s\n", text, ref, (int)s.len, s.ptr);
	buf_fini(&out);
	return found;
}

/* Returns true if the commit of @p ref in @p store contains @p text, and
 * else prints the commit.
 */
static bool
test_has (struct store const *store,
          char const         *ref,
          char const         *text)
{
	return test_in(store, ref, text, true);
}

static void
test_names (void)
{
	char long_name[UTIL_WORD_MAX + 2];
	memset(long_name, 'n', sizeof long_name);
	struct cstr_view const valid[] = {
		CSTR_VIEW("imaami"), CSTR_VIEW("bro"), CSTR_VIEW("first.last"),
		CSTR_VIEW("user@ad"), CSTR_VIEW("p"), CSTR_VIEW("ask_ai"), CSTR_VIEW("uid-1000"),
		CSTR_VIEW("A-b_c.d@e"), CSTR_VIEW("c7x"), CSTR_VIEW("7c"), CSTR_VIEW("a.lck"),
		{ long_name, UTIL_WORD_MAX },
	};
	for (size_t i = 0; i < array_size(valid); ++i)
		CHECK(member_name_valid(valid[i]));
	struct cstr_view const invalid[] = {
		{}, CSTR_VIEW(""), { nullptr, 3 }, CSTR_VIEW(".x"), CSTR_VIEW("-x"),
		CSTR_VIEW("a..b"), CSTR_VIEW("x."), CSTR_VIEW("x.lock"), CSTR_VIEW("@"),
		CSTR_VIEW("c7"), CSTR_VIEW("C12"), CSTR_VIEW("a/b"), CSTR_VIEW("bro#5"),
		CSTR_VIEW("a b"), CSTR_VIEW("a\0b"), CSTR_VIEW("\xc3\xa4"), CSTR_VIEW("a:b"),
		{ long_name, UTIL_WORD_MAX + 1 },
	};
	for (size_t i = 0; i < array_size(invalid); ++i)
		CHECK(!member_name_valid(invalid[i]));
}

static void
test_logins (struct member_registry const *reg)
{
	struct member_login login;
	struct policy_grant grant = {};
	uint32_t const uid = 1000;
	/* A Unix user is a person whose name a lookup finds, also with the
	 * token of askai's own policy. */
	grant.flags = POLICY_OPERATOR;
	CHECK(!member_login(&login, reg, &grant, &uid));
	CHECK(!strcmp(login.text, "uid 1000") && login.uid == 1000 &&
	      login.kind == MEMBER_PERSON && !*login.name);
	/* The token of askai's own policy over TCP is the Unix user who runs
	 * askai, whose name a lookup finds. */
	CHECK(!member_login(&login, reg, &grant, nullptr));
	char own[MEMBER_LOGIN];
	CHECK(snprintf(own, sizeof own, "uid %" PRIu32, (uint32_t)geteuid()) > 0);
	CHECK(!strcmp(login.text, own) && login.uid == geteuid() && login.kind == MEMBER_PERSON &&
	      !*login.name);
	/* An identity is a person, or an agent with POLICY_AGENT. */
	grant = (struct policy_grant){ .who = CSTR_VIEW("identity alice") };
	CHECK(!member_login(&login, reg, &grant, nullptr));
	CHECK(!strcmp(login.text, "identity alice") && !strcmp(login.name, "alice") &&
	      login.kind == MEMBER_PERSON);
	grant.flags = POLICY_AGENT | POLICY_METRICS;
	CHECK(!member_login(&login, reg, &grant, nullptr));
	CHECK(!strcmp(login.text, "identity alice") && login.kind == MEMBER_AGENT);
	/* A client over TCP without a token is without a login. */
	grant = (struct policy_grant){ .who = CSTR_VIEW("anonymous loopback (role user)") };
	memset(&login, 'x', sizeof login);
	CHECK(member_login(&login, reg, &grant, nullptr) == ENOENT && !*login.text);
	grant.who = (struct cstr_view){};
	CHECK(member_login(&login, reg, &grant, nullptr) == ENOENT);
	grant.who = CSTR_VIEW("identity c7");
	CHECK(member_login(&login, reg, &grant, nullptr) == EINVAL);
	CHECK(member_login(nullptr, reg, &grant, nullptr) == EINVAL);
	CHECK(member_login(&login, nullptr, &grant, nullptr) == EINVAL);
	CHECK(member_login(&login, reg, nullptr, nullptr) == EINVAL);
}

/* A login of the kind @p kind with the text @p text and the name @p name. */
static struct member_login
test_login (enum member_kind  kind,
            char const       *text,
            char const       *name)
{
	struct member_login login = { .kind = kind };
	snprintf(login.text, sizeof login.text, "%s", text);
	snprintf(login.name, sizeof login.name, "%s", name);
	if (!strncmp(text, "uid ", 4))
		login.uid = (uint32_t)strtoul(text + 4, nullptr, 10);
	return login;
}

/* Finds the member of @p login in @p table with a lookup of @p lookup,
 * member_table_find() or member_table_claim(), and checks the result: the
 * error @p e, and the member @p name of the kind @p kind, or the start of
 * the reason @p why. Returns the number of waits that ended.
 */
static unsigned
test_find (struct member_table       *table,
           int                      (*lookup) (struct member_table *,
                                               struct member_login const *,
                                               struct member_wait *),
           struct member_login const *login,
           int                        e,
           enum member_kind           kind,
           char const                *name,
           char const                *why)
{
	struct member_wait wait = { .done = test_ended };
	unsigned const before = ended;
	int const first = lookup(table, login, &wait);
	CHECK(first == EINPROGRESS && wait.e == EINPROGRESS);
	if (first == EINPROGRESS)
		test_until(before + 1);
	CHECK(wait.e == e);
	if (!e)
		CHECK(wait.member.kind == kind && !strcmp(wait.member.name, name) && !*wait.why);
	else
		CHECK(!strncmp(wait.why, why, strlen(why)));
	/* After a lookup, the next find needs no lookup. It gets the member,
	 * also a member without a record, or the refusal. */
	struct member_wait again = { .done = test_ended };
	CHECK(member_table_find(table, login, &again) == e && again.e == e);
	if (!e)
		CHECK(again.member.kind == kind && !strcmp(again.member.name, name));
	else
		CHECK(!strcmp(again.why, wait.why));
	return ended;
}

/* Returns true if the ref @p ref exists in @p store. */
static bool
test_exists (struct store const *store,
             char const         *ref)
{
	char const *const words[] = { ref };
	struct buf out = {};
	int status = -1;
	CHECK(!store_run(store, STORE_HEAD, words, nullptr, (struct span){}, &out, &status));
	buf_fini(&out);
	return !status;
}

static void
test_table (struct store const           *store,
            struct member_registry const *reg,
            struct blocker               *blockers,
            struct list                  *children)
{
	int e = -1;
	CHECK(!member_table_create(nullptr, &test_loop, blockers, children, &e) && e == EINVAL);
	CHECK(!member_table_create(reg, nullptr, blockers, children, &e) && e == EINVAL);
	CHECK(!member_table_create(reg, &test_loop, nullptr, children, &e) && e == EINVAL);
	CHECK(!member_table_create(reg, &test_loop, blockers, nullptr, &e) && e == EINVAL);
	struct member_table *table = member_table_create(reg, &test_loop, blockers, children, &e);
	CHECK(table && !e);
	if (!table)
		return;
	struct member_wait wait = { .done = test_ended };
	struct member_login login = {};
	CHECK(member_table_find(table, &login, &wait) == ENOENT && wait.e == ENOENT);
	CHECK(member_table_claim(table, &login, &wait) == ENOENT && wait.e == ENOENT);
	CHECK(member_table_find(nullptr, &login, &wait) == EINVAL);
	CHECK(member_table_find(table, nullptr, &wait) == EINVAL);
	CHECK(member_table_claim(table, &login, nullptr) == EINVAL);

	/* A find names the member that a claim makes, and writes no record. */
	login = test_login(MEMBER_PERSON, "identity alice", "alice");
	test_find(table, member_table_find, &login, 0, MEMBER_PERSON, "alice", "");
	CHECK(!test_exists(store, "refs/askai/members/alice"));
	/* An identity makes a person or an agent of its name. */
	test_find(table, member_table_claim, &login, 0, MEMBER_PERSON, "alice", "");
	CHECK(test_has(store, "refs/askai/members/alice", "\n\nAdd the person alice\n\n"
	               "Kind: person\nLogin: identity alice\n"));
	CHECK(test_has(store, "refs/askai/members/alice", "\nauthor askai <askai> "));
	CHECK(test_has(store, "refs/askai/members/alice", "\ncommitter gpu1 <askai> "));
	CHECK(test_has(store, "refs/askai/members/alice",
	               "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"));
	login = test_login(MEMBER_AGENT, "identity claude", "claude");
	test_find(table, member_table_claim, &login, 0, MEMBER_AGENT, "claude", "");
	CHECK(test_has(store, "refs/askai/members/claude", "\n\nAdd the agent claude\n\n"
	               "Kind: agent\nLogin: identity claude\n"));
	/* An agent identity of the name of another agent is refused, and the
	 * record of the agent stays. */
	login = test_login(MEMBER_AGENT, "identity bro", "bro");
	test_find(table, member_table_claim, &login, EPERM, MEMBER_AGENT, "", "Your login name "
	          "bro is the name of another agent of this session.");
	CHECK(!test_in(store, "refs/askai/members/bro", "Login:", false));
	CHECK(!test_in(store, "refs/askai/members/bro", "\nparent ", false));

	/* A Unix login and an identity login of one name are one person. A find
	 * of the second login adds nothing, and a claim adds the login. */
	login = test_login(MEMBER_PERSON, "uid 4343", "carol");
	test_find(table, member_table_claim, &login, 0, MEMBER_PERSON, "carol", "");
	login = test_login(MEMBER_PERSON, "identity carol", "carol");
	test_find(table, member_table_find, &login, 0, MEMBER_PERSON, "carol", "");
	CHECK(!test_in(store, "refs/askai/members/carol", "identity carol", false));
	test_find(table, member_table_claim, &login, 0, MEMBER_PERSON, "carol", "");
	CHECK(test_has(store, "refs/askai/members/carol", "\n\nAdd login identity carol to the "
	               "person carol\n\nKind: person\nLogin: uid 4343\nLogin: identity carol\n"));
	/* A login stays with its member, whatever name comes with it. */
	login = test_login(MEMBER_PERSON, "uid 4343", "renamed");
	struct member_wait known = { .done = test_ended };
	CHECK(!member_table_find(table, &login, &known) && !strcmp(known.member.name, "carol"));

	/* A person of the name of an agent or of another person is refused,
	 * by a find too, and the refusal stays without a lookup. */
	login = test_login(MEMBER_PERSON, "uid 4444", "bro");
	test_find(table, member_table_find, &login, EPERM, MEMBER_PERSON, "", "Your login name "
	          "bro is the name of an agent of this session. Ask the operator to rename the "
	          "agent.");
	CHECK(member_table_claim(table, &login, &wait) == EPERM);
	login = test_login(MEMBER_PERSON, "uid 4545", "carol");
	test_find(table, member_table_claim, &login, EPERM, MEMBER_PERSON, "", "Your login name "
	          "carol is the name of another person of this session.");
	login = test_login(MEMBER_AGENT, "identity alice", "alice");
	login.text[9] = 'A';
	test_find(table, member_table_claim, &login, EPERM, MEMBER_PERSON, "", "Your login name "
	          "alice is the name of a person of this session.");

	/* A Unix login without a name gets the name of the user, or uid-N
	 * without a user. */
	uint32_t const nobody = 4242420042u;
	CHECK(!getpwuid((uid_t)nobody) && !getpwuid((uid_t)nobody + 1));
	login = test_login(MEMBER_PERSON, "uid 4242420043", "");
	test_find(table, member_table_find, &login, 0, MEMBER_PERSON, "uid-4242420043", "");
	CHECK(!test_exists(store, "refs/askai/members/uid-4242420043"));
	login = test_login(MEMBER_PERSON, "uid 4242420042", "");
	test_find(table, member_table_claim, &login, 0, MEMBER_PERSON, "uid-4242420042", "");
	CHECK(test_has(store, "refs/askai/members/uid-4242420042", "Login: uid 4242420042\n"));
	struct passwd const *pw = getpwuid(geteuid());
	char text[MEMBER_LOGIN];
	CHECK(snprintf(text, sizeof text, "uid %" PRIu32, (uint32_t)geteuid()) > 0);
	char user[MEMBER_NAME];
	if (pw && member_name_valid(cstr_view(pw->pw_name)))
		CHECK(snprintf(user, sizeof user, "%s", pw->pw_name) > 0);
	else
		CHECK(snprintf(user, sizeof user, "uid-%" PRIu32, (uint32_t)geteuid()) > 0);
	login = test_login(MEMBER_PERSON, text, "");
	test_find(table, member_table_claim, &login, 0, MEMBER_PERSON, user, "");

	/* The waits of one login wait for one lookup, which writes the record
	 * that a claim needs. A wait that stops waiting does not end. */
	struct member_wait waits[3] = {
		{ .done = test_ended }, { .done = test_ended }, { .done = test_ended },
	};
	login = test_login(MEMBER_PERSON, "identity dave", "dave");
	unsigned const before = ended;
	CHECK(member_table_find(table, &login, &waits[0]) == EINPROGRESS);
	CHECK(member_table_find(table, &login, &waits[1]) == EINPROGRESS);
	CHECK(member_table_claim(table, &login, &waits[2]) == EINPROGRESS);
	member_table_cancel(&waits[1]);
	member_table_cancel(&waits[1]);
	test_until(before + 2);
	CHECK(!waits[0].e && !waits[2].e && waits[1].e == EINPROGRESS);
	CHECK(!strcmp(waits[0].member.name, "dave") && !strcmp(waits[2].member.name, "dave"));
	CHECK(test_has(store, "refs/askai/members/dave", "Login: identity dave\n"));

	/* A claim ends with EIO after MEMBER_TRIES writes that lose a race,
	 * such as with the lock file of another writer. */
	char lock[4096];
	CHECK(snprintf(lock, sizeof lock, "%s/refs/askai/members/frank.lock",
	               store_directory(store)) > 0);
	FILE *f = fopen(lock, "w");
	CHECK(f && !fclose(f));
	login = test_login(MEMBER_PERSON, "identity frank", "frank");
	unsigned const raced = ended;
	CHECK(member_table_claim(table, &login, &wait) == EINPROGRESS);
	test_until(raced + 1);
	CHECK(wait.e == EIO && !strcmp(wait.why, "Could not write the member record of frank. "
	                                          "Other askai services changed the member "
	                                          "record at the same time."));
	CHECK(!unlink(lock));
	test_find(table, member_table_claim, &login, 0, MEMBER_PERSON, "frank", "");

	/* A table that goes away with a lookup under way kills the git that
	 * reads, and the lookup ends no wait. */
	unsigned const after = ended;
	login = test_login(MEMBER_PERSON, "identity erin", "erin");
	CHECK(member_table_claim(table, &login, &wait) == EINPROGRESS);
	member_table_destroy(&table);
	CHECK(!table);
	member_table_destroy(&table);
	member_table_destroy(nullptr);
	for (size_t i = 0; i < 20; ++i)
		CHECK(!loop_turn(&test_loop, loop_clock() + NS / 100));
	CHECK(ended == after && wait.e == EINPROGRESS);
	CHECK(!test_exists(store, "refs/askai/members/erin"));
}

/* The eventfd that the jobs of test_hold() read, and the number of those
 * jobs that came back.
 */
static int test_gate = -1;
static unsigned held;

/* On a blocker: waits until test_gate lets the job go. */
static void
test_hold (struct blocker_job *job)
{
	eventfd_t one;
	job->error = eventfd_read(test_gate, &one) ? errno : 0;
}

static void
test_held (struct loop *,
           struct msg  *)
{
	++held;
}

/* Holds every thread of @p blockers with the jobs @p jobs. */
static void
test_hold_all (struct blocker     *blockers,
               struct blocker_job  jobs[BLOCKER_THREADS])
{
	for (size_t i = 0; i < BLOCKER_THREADS; ++i) {
		jobs[i] = (struct blocker_job){
			.done.fn = test_held, .work = test_hold, .loop = &test_loop
		};
		CHECK(!blocker_submit(blockers, &jobs[i]));
	}
}

/* Turns the loop until the git of the lookups has ended, for 10 s at most,
 * so that the job name of a lookup waits for a blocker.
 */
static void
test_git_ended (struct list const *children)
{
	uint64_t const deadline = loop_clock() + 10 * NS;
	while (!list_is_empty(children) && loop_clock() < deadline)
		CHECK(!loop_turn(&test_loop, loop_clock() + NS / 100));
	CHECK(list_is_empty(children));
}

/* Turns the loop until @p blockers have done @p n jobs in all and the
 * loop has run the messages of the jobs, for 10 s at most.
 */
static void
test_jobs (struct blocker const *blockers,
           uint64_t              n)
{
	_Atomic uint64_t const *done = &blocker_counts(blockers)->done;
	uint64_t const deadline = loop_clock() + 10 * NS;
	while (atomic_load(done) < n && loop_clock() < deadline)
		CHECK(!loop_turn(&test_loop, loop_clock() + NS / 100));
	CHECK(atomic_load(done) == n);
	for (size_t i = 0; i < 10; ++i)
		CHECK(!loop_turn(&test_loop, loop_clock() + NS / 100));
}

/* The lookups of the names of Unix users while the blockers are busy: a
 * table that goes away, a lookup longer than MEMBER_NS, and blockers that
 * stop. The jobs of test_hold() keep the blockers busy, so that the job
 * name of a lookup waits in the queue.
 */
static void
test_blocked (struct store const           *store,
              struct member_registry const *reg,
              struct list                  *children)
{
	int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	test_gate = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
	CHECK(fd >= 0 && test_gate >= 0);
	int e = -1;
	struct blocker *blockers = blocker_create(fd, &e);
	CHECK(blockers && !e && !blocker_start(blockers));
	if (!blockers)
		return;
	struct blocker_job hold[BLOCKER_THREADS];
	test_git_ended(children);

	/* A table that goes away while the job name waits frees the lookup
	 * once the job comes back, and ends no wait. */
	test_hold_all(blockers, hold);
	struct member_table *table = member_table_create(reg, &test_loop, blockers, children, &e);
	CHECK(table && !e);
	struct member_wait wait = { .done = test_ended };
	struct member_login login = test_login(MEMBER_PERSON, "uid 4242420050", "");
	unsigned const before = ended;
	CHECK(member_table_claim(table, &login, &wait) == EINPROGRESS);
	test_git_ended(children);
	member_table_destroy(&table);
	CHECK(!eventfd_write(test_gate, BLOCKER_THREADS));
	test_jobs(blockers, BLOCKER_THREADS + 1);
	CHECK(held == BLOCKER_THREADS && ended == before && wait.e == EINPROGRESS);
	CHECK(!test_exists(store, "refs/askai/members/uid-4242420050"));

	/* After MEMBER_NS, a claim fails and writes no record. Until the job
	 * comes back, a find names the person uid-N, and a claim fails at
	 * once. A job that comes back late gives the name to the next claim. */
	test_hold_all(blockers, hold);
	table = member_table_create(reg, &test_loop, blockers, children, &e);
	CHECK(table && !e);
	login = test_login(MEMBER_PERSON, "uid 4242420051", "");
	char const *const unnamed = "Could not look up the name of uid 4242420051. Try again.";
	CHECK(member_table_claim(table, &login, &wait) == EINPROGRESS);
	uint64_t const deadline = loop_clock() + MEMBER_NS + 10 * NS;
	while (ended == before && loop_clock() < deadline)
		CHECK(!loop_turn(&test_loop, deadline));
	CHECK(ended == before + 1 && wait.e == EAGAIN && !strcmp(wait.why, unnamed));
	CHECK(!test_exists(store, "refs/askai/members/uid-4242420051"));
	CHECK(!member_table_find(table, &login, &wait) && !strcmp(wait.member.name, "uid-4242420051"));
	CHECK(member_table_claim(table, &login, &wait) == EAGAIN && !strcmp(wait.why, unnamed));
	CHECK(!eventfd_write(test_gate, BLOCKER_THREADS));
	test_jobs(blockers, 2 * BLOCKER_THREADS + 2);
	CHECK(held == 2 * BLOCKER_THREADS && ended == before + 1);
	CHECK(!test_exists(store, "refs/askai/members/uid-4242420051"));
	test_find(table, member_table_claim, &login, 0, MEMBER_PERSON, "uid-4242420051", "");
	CHECK(test_has(store, "refs/askai/members/uid-4242420051", "Login: uid 4242420051\n"));
	member_table_destroy(&table);

	/* Blockers that stop cancel the job name, and the wait ends without a
	 * record. */
	test_hold_all(blockers, hold);
	table = member_table_create(reg, &test_loop, blockers, children, &e);
	CHECK(table && !e);
	login = test_login(MEMBER_PERSON, "uid 4242420052", "");
	unsigned const stopped = ended;
	CHECK(member_table_claim(table, &login, &wait) == EINPROGRESS);
	test_git_ended(children);
	blocker_stop(blockers);
	CHECK(!eventfd_write(test_gate, 1));
	test_until(stopped + 1);
	CHECK(wait.e == ECANCELED && !strcmp(wait.why, "askai stops."));
	CHECK(!test_exists(store, "refs/askai/members/uid-4242420052"));
	CHECK(!eventfd_write(test_gate, 1));
	uint64_t const end = loop_clock() + 10 * NS;
	while ((!blocker_done(blockers) || held < 3 * BLOCKER_THREADS) && loop_clock() < end)
		CHECK(!loop_turn(&test_loop, loop_clock() + NS / 100));
	CHECK(blocker_done(blockers) && held == 3 * BLOCKER_THREADS);
	member_table_destroy(&table);
	blocker_destroy(&blockers);
	util_close(&test_gate);
	util_close(&fd);
}

/* Writes the session record with the trailer @p t of @p n lines to the new
 * repository @p path.
 */
static void
test_session (char const                 *path,
              struct store_trailer const *t,
              size_t                      n)
{
	char why[256];
	int e = -1;
	struct store *store = store_create(path, why, sizeof why, &e);
	CHECK(store && !e);
	if (!store)
		return;
	struct buf in = {};
	struct buf out = {};
	int status = -1;
	CHECK(!store_run(store, STORE_CREATE, nullptr, nullptr, (struct span){}, &out, &status) &&
	      !status);
	struct store_commit const commit = {
		.committer = { CSTR_VIEW("old"), CSTR_VIEW("askai") },
		.message = CSTR_VIEW("Create the session"),
		.trailers = t,
		.n_trailers = n,
		.ref = "refs/askai/session",
		.tip = &(struct oid){},
	};
	CHECK(!store_commit_input(&commit, &in));
	CHECK(!store_run(store, STORE_COMMIT, nullptr, commit.ref, buf_peek(&in), &out, &status) &&
	      !status);
	buf_fini(&in);
	buf_fini(&out);
	store_destroy(&store);
}

/* Returns the registry of @p store for the agent @p agent, after a check
 * that its creation fails with the error @p e and a reason that begins
 * with @p why, if @p e is not 0.
 */
static struct member_registry *
test_registry_make (struct store const *store,
                    char const         *agent,
                    char const         *service,
                    int                 e,
                    char const         *why)
{
	char text[512] = "";
	int got = -1;
	struct member_registry *reg = member_registry_create(store, agent, service, text,
	                                                     sizeof text, &got);
	CHECK(got == e && !reg == !!e);
	if (e)
		CHECK(!strncmp(text, why, strlen(why)));
	if (got != e)
		fprintf(stderr, "%s: %s\n", agent, text);
	return reg;
}

/* The registry of the session in the new repository @p path. */
static void
test_registry (char const *dir)
{
	char path[4096];
	char why[256];
	int e = -1;
	CHECK(snprintf(path, sizeof path, "%s/sess/.git", dir) > 0);
	CHECK(snprintf(why, sizeof why, "%s/sess", dir) > 0 && !mkdir(why, 0700));
	struct store *store = store_create(path, why, sizeof why, &e);
	CHECK(store && !e);
	struct member_registry *reg = test_registry_make(store, "bro", "gpu1", 0, "");
	CHECK(test_has(store, "refs/askai/session", "\n\nCreate the session sess\n\nSession: "));
	CHECK(test_has(store, "refs/askai/session", "\nName: sess\nFormat: 1\n"));
	member_registry_destroy(&reg);
	store_destroy(&store);

	/* Bad arguments make no repository. */
	CHECK(snprintf(path, sizeof path, "%s/none", dir) > 0);
	store = store_create(path, why, sizeof why, &e);
	CHECK(store && !e);
	test_registry_make(nullptr, "bro", "gpu1", EINVAL, "The registry needs");
	test_registry_make(store, "c7", "gpu1", EINVAL, "The registry needs");
	test_registry_make(store, "bro", "x.lock", EINVAL, "The registry needs");
	test_registry_make(store, nullptr, "gpu1", EINVAL, "The registry needs");
	CHECK(!member_registry_create(store, "bro", "gpu1", nullptr, 0, &e) && e == EINVAL);
	CHECK(!store_exists(store));
	store_destroy(&store);

	/* A session of a later format or without an ID is refused. */
	CHECK(snprintf(path, sizeof path, "%s/future.git", dir) > 0);
	struct store_trailer const future[] = {
		{ CSTR_VIEW("Session"), CSTR_VIEW("6f0c2b9a41d8e3f75a2c1b0d9e8f7a6b") },
		{ CSTR_VIEW("Name"),    CSTR_VIEW("future") },
		{ CSTR_VIEW("Format"),  CSTR_VIEW("2") },
	};
	test_session(path, future, array_size(future));
	store = store_create(path, why, sizeof why, &e);
	test_registry_make(store, "bro", "gpu1", EINVAL, "The session has format 2. This askai "
	                   "writes format 1.");
	store_destroy(&store);
	CHECK(snprintf(path, sizeof path, "%s/broken.git", dir) > 0);
	test_session(path, future + 1, 2);
	store = store_create(path, why, sizeof why, &e);
	test_registry_make(store, "bro", "gpu1", EINVAL, "The session record refs/askai/session "
	                   "is not valid.");
	store_destroy(&store);

	/* Services that start at the same time on a new repository all start,
	 * also if their git init, their session record or the record of their
	 * agent races another service. Each service opens the repository. */
	CHECK(snprintf(path, sizeof path, "%s/race.git", dir) > 0);
	int gate[2];
	CHECK(!pipe(gate));
	pid_t pids[8];
	for (size_t i = 0; i < array_size(pids); ++i) {
		pids[i] = fork();
		CHECK(pids[i] >= 0);
		if (pids[i])
			continue;
		char c;
		close(gate[1]);
		if (read(gate[0], &c, 1) < 0)
			_exit(2);
		char service[16];
		snprintf(service, sizeof service, "gpu%zu", i);
		store = store_create(path, why, sizeof why, &e);
		if (!store)
			fprintf(stderr, "%s: %s\n", service, why);
		reg = store ? test_registry_make(store, "bro", service, 0, "") : nullptr;
		_exit(reg ? 0 : 1);
	}
	close(gate[0]);
	close(gate[1]);
	for (size_t i = 0; i < array_size(pids); ++i) {
		int status = -1;
		CHECK(pids[i] > 0 && waitpid(pids[i], &status, 0) == pids[i]);
		CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
	}
	store = store_create(path, why, sizeof why, &e);
	CHECK(store && !e);
	CHECK(test_has(store, "refs/askai/members/bro", "\n\nAdd the agent bro\n\nKind: agent\n"));
	store_destroy(&store);
}

int
main (int    argc,
      char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s DIRECTORY\n", argv[0]);
		return 2;
	}
	test_names();
	test_registry(argv[1]);

	char path[4096];
	char why[256];
	int e = -1;
	CHECK(snprintf(path, sizeof path, "%s/test1.git", argv[1]) > 0);
	struct store *store = store_create(path, why, sizeof why, &e);
	CHECK(store && !e);
	if (!store)
		return check_report("member");
	struct member_registry *reg = test_registry_make(store, "bro", "gpu1", 0, "");
	if (!reg)
		return check_report("member");
	CHECK(test_has(store, "refs/askai/session", "\n\nCreate the session test1\n\n"));
	CHECK(test_has(store, "refs/askai/session", "\ncommitter gpu1 <askai> "));
	CHECK(test_has(store, "refs/askai/members/bro", "\n\nAdd the agent bro\n\nKind: agent\n"));
	struct cstr_view const session = member_registry_session(reg);
	CHECK(hexid_valid(session) && !member_registry_session(nullptr).len);
	member_registry_report(reg);

	/* Another service of the agent adopts its record, and another agent
	 * gets a record. The session stays. */
	struct buf head = {};
	int status = -1;
	char const *const bro[] = { "refs/askai/members/bro" };
	CHECK(!store_run(store, STORE_HEAD, bro, nullptr, (struct span){}, &head, &status));
	struct member_registry *again = test_registry_make(store, "bro", "gpu2", 0, "");
	CHECK(!store_run(store, STORE_HEAD, bro, nullptr, (struct span){}, &head, &status));
	struct span const heads = buf_peek(&head);
	CHECK(heads.len == 2 * (OID_SHA1 + 1) && !memcmp(heads.ptr, heads.ptr + OID_SHA1 + 1,
	                                                  OID_SHA1));
	buf_fini(&head);
	CHECK(cstr_view_equal(member_registry_session(again), session));
	member_registry_destroy(&again);
	CHECK(!again);
	member_registry_destroy(&again);
	member_registry_destroy(nullptr);
	again = test_registry_make(store, "cuh", "gpu3", 0, "");
	CHECK(test_has(store, "refs/askai/members/cuh", "\n\nAdd the agent cuh\n\nKind: agent\n"));
	CHECK(test_has(store, "refs/askai/members/cuh", "\ncommitter gpu3 <askai> "));
	member_registry_destroy(&again);

	test_logins(reg);

	int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	CHECK(fd >= 0);
	struct blocker *blockers = blocker_create(fd, &e);
	CHECK(blockers && !e && !blocker_start(blockers));
	CHECK(!loop_init(&test_loop));
	struct list children;
	list_init(&children);
	test_table(store, reg, blockers, &children);
	test_blocked(store, reg, &children);

	/* The agent of a service adopts the record of an agent identity. */
	again = test_registry_make(store, "claude", "gpu4", 0, "");
	member_registry_destroy(&again);
	CHECK(test_has(store, "refs/askai/members/claude", "Login: identity claude\n"));

	/* An agent cannot take the name of a person. */
	test_registry_make(store, "carol", "gpu1", EINVAL, "agent carol: the name carol belongs "
	                   "to the person with login uid 4343,identity carol in this session");
	again = test_registry_make(store, "bro", "gpu1", 0, "");
	member_registry_report(again);
	member_registry_destroy(&again);

	blocker_stop(blockers);
	uint64_t const deadline = loop_clock() + 10 * NS;
	while (!blocker_done(blockers) && loop_clock() < deadline)
		CHECK(!loop_turn(&test_loop, loop_clock() + NS / 100));
	CHECK(blocker_done(blockers));
	blocker_destroy(&blockers);
	util_close(&fd);
	child_destroy_all(&children);
	loop_fini(&test_loop);
	member_registry_destroy(&reg);
	store_destroy(&store);
	return check_report("member");
}
