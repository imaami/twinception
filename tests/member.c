/** @file Unit tests of the members of the session, src/member.c: the name
 *  rule, the refs of the branches, the logins of clients, the session
 *  record and the record of the agent that the registry writes as askai
 *  starts, also for services that start at the same time, the lookups of a
 *  table, with git on a loop and names from blockers, which find, create,
 *  adopt, extend and refuse member records in new repositories, the reads
 *  of the records of a table, the list of the members and their branches
 *  with its notices, two tables that claim one record at the same time,
 *  the sentences for the error numbers of a lookup, a lookup while no
 *  child can start, and the reasons of a git that fails at the start and
 *  on a loop. The test replaces mcp_slot_spawn() and mcp_slot_loop(),
 *  which src/store.c calls.
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
#include "dstr.h"
#include "git_run.h"
#include "hexid.h"
#include "jb.h"
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

/* On the loop: the git of test_take() ended. */
static void
test_git_done (struct child *)
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
	struct child_end end;
	CHECK(!store_run(store, STORE_READ, nullptr, nullptr,
	                 (struct span){ (unsigned char *)line, (size_t)n }, out, &end) &&
	      !end.status);
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

/* Returns true if the name of @p member ends with a NUL and every byte
 * after that NUL is 0. A struct assignment and memcpy() copy the whole
 * name, so a byte that no code wrote would be copied too.
 */
static bool
test_name_zeroed (struct member const *member)
{
	size_t const len = strnlen(member->name, sizeof member->name);
	for (size_t i = len; i < sizeof member->name; ++i)
		if (member->name[i])
			return false;
	return len < sizeof member->name;
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
	/* member_name_set() writes the name and 0 in every byte after the
	 * name, also into an array that the test fills with 'x' first. It
	 * writes the first UTIL_WORD_MAX bytes of a name that is too long, and
	 * "" for an empty view. member() returns a member of the kind with
	 * the same bytes as the name. */
	struct cstr_view const sets[] = {
		CSTR_VIEW("bro"), CSTR_VIEW("p"), { long_name, UTIL_WORD_MAX },
		{ long_name, sizeof long_name }, {}, CSTR_VIEW(""),
	};
	for (size_t i = 0; i < array_size(sets); ++i) {
		size_t const len = sets[i].len < UTIL_WORD_MAX ? sets[i].len : UTIL_WORD_MAX;
		struct member set;
		memset(&set, 'x', sizeof set);
		member_name_set(set.name, sets[i]);
		CHECK(strnlen(set.name, sizeof set.name) == len && test_name_zeroed(&set) &&
		      (!len || !memcmp(set.name, sets[i].data, len)));
		struct member const made = member(i % 2 ? MEMBER_AGENT : MEMBER_PERSON, sets[i]);
		CHECK(made.kind == (i % 2 ? MEMBER_AGENT : MEMBER_PERSON) &&
		      !memcmp(made.name, set.name, sizeof made.name));
	}
	/* member_json() writes a member, and member_init_json() makes a member
	 * of a valid name and a known kind. */
	struct member m;
	memset(&m, 'x', sizeof m);
	struct json_object *json = member_json(CSTR_VIEW("imaami"), MEMBER_PERSON);
	CHECK(cstr_view_equal(jb_wire(json), CSTR_VIEW("{\"name\":\"imaami\",\"kind\":\"person\"}")));
	CHECK(!member_init_json(&m, json) && m.kind == MEMBER_PERSON && !strcmp(m.name, "imaami") &&
	      test_name_zeroed(&m));
	json_object_put(json);
	CHECK(!member_json((struct cstr_view){}, MEMBER_PERSON));
	CHECK(!member_json(CSTR_VIEW("imaami"), MEMBER_KINDS));
	json = jb_object("name", jb_string(valid[array_size(valid) - 1]),
	                 "kind", json_object_new_string("agent"), nullptr);
	CHECK(!member_init_json(&m, json) && m.kind == MEMBER_AGENT &&
	      strlen(m.name) == UTIL_WORD_MAX && test_name_zeroed(&m));
	json_object_put(json);
	for (size_t i = 0; i < array_size(invalid); ++i) {
		m = (struct member){ .kind = MEMBER_AGENT, .name = "old" };
		json = jb_object("name", invalid[i].data ? jb_string(invalid[i])
		                                         : json_object_new_int(3),
		                 "kind", json_object_new_string("person"), nullptr);
		CHECK(json && member_init_json(&m, json) == EINVAL && m.kind == MEMBER_PERSON &&
		      !*m.name);
		json_object_put(json);
	}
	struct cstr_view const kinds[] = {
		CSTR_VIEW(""), CSTR_VIEW("Person"), CSTR_VIEW("team"), CSTR_VIEW("agent "),
	};
	for (size_t i = 0; i < array_size(kinds); ++i) {
		json = jb_object("name", json_object_new_string("bro"), "kind", jb_string(kinds[i]),
		                 nullptr);
		CHECK(json && member_init_json(&m, json) == EINVAL && !*m.name);
		json_object_put(json);
	}
	json = jb_object("name", json_object_new_string("bro"), nullptr);
	CHECK(json && member_init_json(&m, json) == EINVAL && !*m.name);
	CHECK(member_init_json(nullptr, json) == EINVAL);
	json_object_put(json);
	CHECK(member_init_json(&m, nullptr) == EINVAL && !*m.name);

	/* The refs of the branches of a member, and back. */
	char ref[MEMBER_REF];
	member_ref(&(struct member){ MEMBER_PERSON, "first.last@ad" }, MEMBER_BRANCH_NOTES, ref);
	CHECK(!strcmp(ref, "refs/heads/person/first.last@ad/notes"));
	enum member_kind kind = MEMBER_AGENT;
	struct cstr_view name = {};
	enum member_branch branch = MEMBER_BRANCH_FINDINGS;
	CHECK(!member_ref_parse(cstr_view(ref), &kind, &name, &branch));
	CHECK(kind == MEMBER_PERSON && cstr_view_equal(name, CSTR_VIEW("first.last@ad")) &&
	      branch == MEMBER_BRANCH_NOTES);
	/* The longest ref fills MEMBER_REF. */
	m = (struct member){ .kind = MEMBER_PERSON };
	memset(m.name, 'n', UTIL_WORD_MAX);
	member_ref(&m, MEMBER_BRANCH_FINDINGS, ref);
	CHECK(strlen(ref) == MEMBER_REF - 1 && !strncmp(ref, "refs/heads/person/nnn", 21) &&
	      !strcmp(ref + MEMBER_REF - sizeof "n/findings", "n/findings"));
	CHECK(!member_ref_parse(cstr_view(ref), &kind, &name, &branch));
	CHECK(kind == MEMBER_PERSON && name.len == UTIL_WORD_MAX &&
	      branch == MEMBER_BRANCH_FINDINGS);
	member_ref(&(struct member){ MEMBER_AGENT, "bro" }, MEMBER_BRANCH_FINDINGS, ref);
	CHECK(!strcmp(ref, "refs/heads/agent/bro/findings"));
	struct cstr_view const refs[] = {
		{}, CSTR_VIEW("refs/heads/agent/bro"), CSTR_VIEW("refs/heads/agent/bro/"),
		CSTR_VIEW("refs/heads/agent/bro/review"), CSTR_VIEW("refs/heads/team/bro/notes"),
		CSTR_VIEW("refs/heads/agent/c7/notes"), CSTR_VIEW("refs/heads/agent/a/b/notes"),
		CSTR_VIEW("refs/heads/agent/bro/notes/x"), CSTR_VIEW("refs/tags/agent/bro/notes"),
		CSTR_VIEW("refs/heads/notes/bro/anonymous/upstream"),
		CSTR_VIEW("refs/heads/agent//notes"), CSTR_VIEW("refs/askai/members/bro"),
		CSTR_VIEW("refs/heads/Agent/bro/notes"),
	};
	for (size_t i = 0; i < array_size(refs); ++i)
		CHECK(member_ref_parse(refs[i], &kind, &name, &branch) == EINVAL);
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
	      login.member.kind == MEMBER_PERSON && !*login.member.name);
	/* The token of askai's own policy over TCP is the Unix user who runs
	 * askai, whose name a lookup finds. */
	CHECK(!member_login(&login, reg, &grant, nullptr));
	char own[MEMBER_LOGIN];
	CHECK(snprintf(own, sizeof own, "uid %" PRIu32, (uint32_t)geteuid()) > 0);
	CHECK(!strcmp(login.text, own) && login.uid == geteuid() &&
	      login.member.kind == MEMBER_PERSON && !*login.member.name);
	/* An identity is a person, or an agent with POLICY_AGENT. */
	grant = (struct policy_grant){ .who = CSTR_VIEW("identity alice") };
	memset(&login, 'x', sizeof login);
	CHECK(!member_login(&login, reg, &grant, nullptr));
	CHECK(!strcmp(login.text, "identity alice") && !strcmp(login.member.name, "alice") &&
	      login.member.kind == MEMBER_PERSON && test_name_zeroed(&login.member));
	grant.flags = POLICY_AGENT | POLICY_METRICS;
	CHECK(!member_login(&login, reg, &grant, nullptr));
	CHECK(!strcmp(login.text, "identity alice") && login.member.kind == MEMBER_AGENT &&
	      !strcmp(login.member.name, "alice") && test_name_zeroed(&login.member));
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
	/* member_login() uses the uid of a Unix login, not the identity of the
	 * grant. */
	grant = (struct policy_grant){ .who = CSTR_VIEW("identity alice") };
	memset(&login, 'x', sizeof login);
	CHECK(!member_login(&login, reg, &grant, &uid));
	CHECK(!strcmp(login.text, "uid 1000") && login.uid == 1000 &&
	      login.member.kind == MEMBER_PERSON && !*login.member.name);
}

/* member_call_ids() makes the ID of the ask call, and the ID of a new
 * conversation unless the call continues one.
 */
static void
test_call_ids (void)
{
	struct member_call call = {};
	CHECK(!member_call_ids(&call) && hexid_valid(hexid_view(&call.ask)) &&
	      hexid_valid(hexid_view(&call.conversation)) &&
	      strcmp(call.ask.hex, call.conversation.hex));
	struct hexid const first = call.ask;
	CHECK(!hexid_init(&call.conversation, CSTR_VIEW("fedcba9876543210fedcba9876543210")));
	CHECK(!member_call_ids(&call) && hexid_valid(hexid_view(&call.ask)) &&
	      strcmp(call.ask.hex, first.hex) &&
	      !strcmp(call.conversation.hex, "fedcba9876543210fedcba9876543210"));
	CHECK(member_call_ids(nullptr) == EINVAL);
}

/* member_login_why() writes the sentence for each error number of a
 * lookup. The sentence of a refusal names the kind and the name of the
 * record that refuses the login, also for a Unix login, which is without a
 * name. An error number without a sentence gives an empty view.
 */
static void
test_sentences (void)
{
	struct member_login const user = { .uid = 1000, .text = "uid 1000" };
	struct member_login const agent = {
		.member = { MEMBER_AGENT, "bro" },
		.text = "identity bro",
	};
	struct member const person = { MEMBER_PERSON, "bro" };
	struct member const bot = { MEMBER_AGENT, "bro" };
	static struct {
		int         e;
		bool        agent;  /* the login is the login of an agent */
		bool        bot;    /* the record that refuses is of an agent */
		char const *why;
	} const cases[] = {
		{ EPERM, false, false,
		  "Your login name bro is the name of another person of this session." },
		{ EPERM, false, true,
		  "Your login name bro is the name of an agent of this session. Ask the operator "
		  "to rename the agent." },
		{ EPERM, true, false,
		  "Your login name bro is the name of a person of this session." },
		{ EPERM, true, true,
		  "Your login name bro is the name of another agent of this session." },
		{ EIO, false, false, "askai could not read or write the member records." },
		{ EAGAIN, false, false,
		  "Other askai services changed your member record at the same time. Try again." },
		{ EBUSY, false, false, "askai is busy. Try again in a few seconds." },
		{ ENODATA, false, false, "Could not look up the name of uid 1000. Try again." },
		{ ENODATA, true, false, "Could not look up the name of identity bro. Try again." },
		{ ECANCELED, false, false, "askai stops." },
		{ 0, false, false, "" },
		{ ENOENT, false, false, "" },
		{ ENOMEM, false, false, "" },
		{ EINVAL, false, false, "" },
		{ EINPROGRESS, false, false, "" },
	};
	for (size_t i = 0; i < array_size(cases); ++i) {
		struct member_login const *login = cases[i].agent ? &agent : &user;
		struct member const *by = cases[i].bot ? &bot : &person;
		char buf[TOOL_TELL_MAX];
		memset(buf, 'x', sizeof buf);
		struct cstr_view const why = member_login_why(login, cases[i].e, by, buf,
		                                              sizeof buf);
		CHECK(cstr_view_equal(why, cstr_view(cases[i].why)));
		/* A sentence with a name is in the buffer, with its NUL. */
		if (why.data == buf)
			CHECK(!buf[why.len]);
		else
			CHECK(buf[0] == 'x');
		/* Only EPERM reads the record. */
		struct cstr_view const bare = member_login_why(login, cases[i].e, nullptr, buf,
		                                               sizeof buf);
		CHECK(cases[i].e == EPERM ? !bare.len : cstr_view_equal(bare, why));
		CHECK(!member_login_why(nullptr, cases[i].e, by, buf, sizeof buf).len);
		CHECK(!member_login_why(login, cases[i].e, by, nullptr, sizeof buf).len);
		CHECK(!member_login_why(login, cases[i].e, by, buf, 0).len);
	}
	/* A sentence that does not fit is cut and ends with "...". */
	char cut[32];
	struct cstr_view const why = member_login_why(&user, EPERM, &bot, cut, sizeof cut);
	CHECK(cstr_view_equal(why, CSTR_VIEW("Your login name bro is the n...")) &&
	      why.data == cut);
}

/* Writes to @p user the name of the person of the Unix user who runs the
 * test: the name of the user, or uid-N if member_name_valid() refuses that
 * name. Writes to @p text the Unix login of the user, "uid N".
 */
static void
test_user (char user[static MEMBER_NAME],
           char text[static MEMBER_LOGIN])
{
	uint32_t const uid = (uint32_t)geteuid();
	struct passwd const *pw = getpwuid((uid_t)uid);
	if (pw && member_name_valid(cstr_view(pw->pw_name)))
		CHECK(snprintf(user, MEMBER_NAME, "%s", pw->pw_name) > 0);
	else
		CHECK(snprintf(user, MEMBER_NAME, "uid-%" PRIu32, uid) > 0);
	CHECK(snprintf(text, MEMBER_LOGIN, "uid %" PRIu32, uid) > 0);
}

/* A login of the kind @p kind with the text @p text and the name @p name. */
static struct member_login
test_login (enum member_kind  kind,
            char const       *text,
            char const       *name)
{
	struct member_login login = { .member.kind = kind };
	snprintf(login.text, sizeof login.text, "%s", text);
	snprintf(login.member.name, sizeof login.member.name, "%s", name);
	if (!strncmp(text, "uid ", 4))
		login.uid = (uint32_t)strtoul(text + 4, nullptr, 10);
	return login;
}

/* The scratch text of the tables of the test. A table writes there the
 * cause of a failure that it logs.
 */
static struct tool_scratch test_scratch;

/* Returns true if the scratch of the test contains the text @p cause. */
static bool
test_cause (char const *cause)
{
	return !strcmp(test_scratch.text, cause);
}

/* Creates a table of @p reg on the loop of the test, with the scratch of
 * the test (member_table_create()).
 */
static struct member_table *
test_table_make (struct member_registry const *reg,
                 struct blocker               *blockers,
                 struct list                  *children,
                 int                          *e)
{
	return member_table_create(reg, &test_loop, blockers, children, &test_scratch, e);
}

/* Returns true if the sentence of member_login_why() for the result of
 * @p wait, a lookup of @p login, is the text @p why. The text of a result
 * without a sentence is "".
 */
static bool
test_why (struct member_wait const  *wait,
          struct member_login const *login,
          char const                *why)
{
	char buf[TOOL_TELL_MAX] = "";
	struct cstr_view const got = member_login_why(login, wait->e, &wait->by, buf, sizeof buf);
	bool const same = got.len == strlen(why) && (!got.len || !memcmp(got.data, why, got.len));
	if (!same)
		fprintf(stderr, "got:  %.*s\nwant: %s\n", (int)got.len, got.data, why);
	return same;
}

/* Returns true if @p wait names no record that refuses a login. */
static bool
test_by_none (struct member_wait const *wait)
{
	return !wait->by.kind && !*wait->by.name && test_name_zeroed(&wait->by);
}

/* Returns true if @p wait names the record @p name of the kind @p kind as
 * the record that refuses a login.
 */
static bool
test_by (struct member_wait const *wait,
         enum member_kind          kind,
         char const               *name)
{
	return wait->by.kind == kind && !strcmp(wait->by.name, name) && test_name_zeroed(&wait->by);
}

/* Finds the member of @p login in @p table with a lookup of @p lookup,
 * member_table_find() or member_table_claim(), which reads the records
 * with git, and checks the result: the error @p e, and the member @p name
 * of the kind @p kind. For an error, the result is without a member name,
 * and the sentence of member_login_why() is @p why. For EPERM, @p kind and
 * @p name are the kind and the name of the record that refuses the login.
 * Every byte of the names in the wait is written (test_name_zeroed()).
 * Returns the number of waits that ended.
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
	CHECK(wait.e == e && test_name_zeroed(&wait.member) && test_why(&wait, login, why));
	if (!e)
		CHECK(wait.member.kind == kind && !strcmp(wait.member.name, name));
	else
		CHECK(wait.member.kind == login->member.kind && !*wait.member.name);
	if (e == EPERM)
		CHECK(test_by(&wait, kind, name));
	else
		CHECK(test_by_none(&wait));
	/* After a lookup, the next find needs no lookup. It gets the member,
	 * also a member without a record, or the refusal. */
	struct member_wait again = { .done = test_ended };
	CHECK(member_table_find(table, login, &again) == e && again.e == e &&
	      test_name_zeroed(&again.member) && test_why(&again, login, why));
	CHECK(!memcmp(&again.member, &wait.member, sizeof again.member) &&
	      !memcmp(&again.by, &wait.by, sizeof again.by));
	return ended;
}

/* Returns true if the ref @p ref exists in @p store. */
static bool
test_exists (struct store const *store,
             char const         *ref)
{
	char const *const words[] = { ref };
	struct buf out = {};
	struct child_end end;
	CHECK(!store_run(store, STORE_HEAD, words, nullptr, (struct span){}, &out, &end));
	buf_fini(&out);
	return !end.status;
}

static void
test_table (struct store const           *store,
            struct member_registry const *reg,
            struct blocker               *blockers,
            struct list                  *children)
{
	int e = -1;
	struct tool_scratch *const scratch = &test_scratch;
	CHECK(!member_table_create(nullptr, &test_loop, blockers, children, scratch, &e) &&
	      e == EINVAL);
	CHECK(!member_table_create(reg, nullptr, blockers, children, scratch, &e) && e == EINVAL);
	CHECK(!member_table_create(reg, &test_loop, nullptr, children, scratch, &e) && e == EINVAL);
	CHECK(!member_table_create(reg, &test_loop, blockers, nullptr, scratch, &e) && e == EINVAL);
	e = -1;
	CHECK(!member_table_create(reg, &test_loop, blockers, children, nullptr, &e) &&
	      e == EINVAL);
	struct member_table *table = test_table_make(reg, blockers, children, &e);
	CHECK(table && !e);
	if (!table)
		return;
	CHECK(member_table_registry(table) == reg && !member_table_registry(nullptr));
	struct member_wait wait = { .done = test_ended };
	struct member_login login = {};
	CHECK(member_table_find(table, &login, &wait) == ENOENT && wait.e == ENOENT &&
	      test_name_zeroed(&wait.member));
	CHECK(member_table_claim(table, &login, &wait) == ENOENT && wait.e == ENOENT &&
	      test_name_zeroed(&wait.member));
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
	/* The lookup of a find of a login without a record ends with the
	 * member and stays on the table. */
	struct member_login const early = test_login(MEMBER_PERSON, "uid 4747", "claude");
	test_find(table, member_table_find, &early, 0, MEMBER_PERSON, "claude", "");
	login = test_login(MEMBER_AGENT, "identity claude", "claude");
	test_find(table, member_table_claim, &login, 0, MEMBER_AGENT, "claude", "");
	CHECK(test_has(store, "refs/askai/members/claude", "\n\nAdd the agent claude\n\n"
	               "Kind: agent\nLogin: identity claude\n"));
	/* member_table_claim() for the login of that find runs the step of the
	 * lookup again at once. The name is the name of an agent now, so the
	 * function returns the refusal without a wait, with the record of the
	 * agent as the record that refuses the login. A later find returns the
	 * same refusal. */
	static char const taken[] = "Your login name claude is the name of an agent of this "
	                            "session. Ask the operator to rename the agent.";
	unsigned const refused = ended;
	CHECK(member_table_claim(table, &early, &wait) == EPERM && wait.e == EPERM &&
	      test_by(&wait, MEMBER_AGENT, "claude") && test_why(&wait, &early, taken) &&
	      wait.member.kind == MEMBER_PERSON && !*wait.member.name &&
	      test_name_zeroed(&wait.member));
	wait.by = (struct member){};
	CHECK(member_table_find(table, &early, &wait) == EPERM &&
	      test_by(&wait, MEMBER_AGENT, "claude") && test_why(&wait, &early, taken) &&
	      !*wait.member.name && test_name_zeroed(&wait.member));
	CHECK(ended == refused);
	/* An agent identity of the name of another agent is refused, and the
	 * record of the agent stays. */
	login = test_login(MEMBER_AGENT, "identity bro", "bro");
	test_find(table, member_table_claim, &login, EPERM, MEMBER_AGENT, "bro", "Your login name "
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
	CHECK(!member_table_find(table, &login, &known) && !strcmp(known.member.name, "carol") &&
	      test_name_zeroed(&known.member));

	/* A person of the name of an agent or of another person is refused,
	 * by a find too, and the refusal stays without a lookup. */
	login = test_login(MEMBER_PERSON, "uid 4444", "bro");
	test_find(table, member_table_find, &login, EPERM, MEMBER_AGENT, "bro", "Your login name "
	          "bro is the name of an agent of this session. Ask the operator to rename the "
	          "agent.");
	CHECK(member_table_claim(table, &login, &wait) == EPERM && test_name_zeroed(&wait.member) &&
	      test_by(&wait, MEMBER_AGENT, "bro"));
	login = test_login(MEMBER_PERSON, "uid 4545", "carol");
	test_find(table, member_table_claim, &login, EPERM, MEMBER_PERSON, "carol", "Your login "
	          "name carol is the name of another person of this session.");
	login = test_login(MEMBER_AGENT, "identity alice", "alice");
	login.text[9] = 'A';
	test_find(table, member_table_claim, &login, EPERM, MEMBER_PERSON, "alice", "Your login "
	          "name alice is the name of a person of this session.");

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
	char user[MEMBER_NAME], text[MEMBER_LOGIN];
	test_user(user, text);
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
	for (size_t i = 0; i < array_size(waits); ++i)
		CHECK(test_name_zeroed(&waits[i].member));
	CHECK(test_has(store, "refs/askai/members/dave", "Login: identity dave\n"));

	/* A claim ends with EAGAIN after STORE_TRIES writes that lose a race,
	 * such as with the lock file of another writer. The table logs the
	 * cause, and the client reads that a retry helps. */
	char lock[4096];
	CHECK(snprintf(lock, sizeof lock, "%s/refs/askai/members/frank.lock",
	               store_directory(store)) > 0);
	FILE *f = fopen(lock, "w");
	CHECK(f && !fclose(f));
	login = test_login(MEMBER_PERSON, "identity frank", "frank");
	unsigned const raced = ended;
	CHECK(member_table_claim(table, &login, &wait) == EINPROGRESS);
	test_until(raced + 1);
	CHECK(wait.e == EAGAIN && test_by_none(&wait) && !*wait.member.name &&
	      test_name_zeroed(&wait.member) &&
	      test_cause("Could not write the member record of frank. Other askai services "
	                 "changed the member record at the same time.") &&
	      test_why(&wait, &login, "Other askai services changed your member record at the "
	               "same time. Try again."));
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

/* Commits the note @p name to the ref @p ref of @p store, with the
 * subject @p subject and the trailer line Asked-by @p asker, or none for
 * nullptr.
 */
static void
test_note (struct store const *store,
           char const         *ref,
           char const         *subject,
           char const         *asker)
{
	struct buf files = {};
	struct extent part;
	CHECK(!store_part_file(&part, &files, CSTR_VIEW("plan.md"), CSTR_VIEW("x")));
	struct store_trailer const t[] = { { CSTR_VIEW("Asked-by"), asker ? cstr_view(asker)
	                                                                  : CSTR_VIEW("") } };
	struct store_commit const commit = {
		.author = { CSTR_VIEW("bro"), CSTR_VIEW("agent/bro") },
		.committer = { CSTR_VIEW("gpu1"), CSTR_VIEW("askai") },
		.message = cstr_view(subject),
		.files = buf_peek(&files),
		.trailers = asker ? t : nullptr,
		.n_trailers = asker ? 1 : 0,
		.ref = ref,
		.tip = &(struct oid){},
	};
	struct buf in = {};
	struct buf out = {};
	struct child_end end;
	CHECK(!store_commit_input(&commit, &in));
	CHECK(!store_run(store, STORE_COMMIT, nullptr, ref, buf_peek(&in), &out, &end) &&
	      !end.status);
	buf_fini(&files);
	buf_fini(&in);
	buf_fini(&out);
}

/* Checks that @p d contains the text @p want, and clears @p d. */
static void
test_text (struct dstr *d,
           char const  *want)
{
	bool const same = !strcmp(dstr_get(d), want);
	if (!same)
		fprintf(stderr, "got:  %s\nwant: %s\n", dstr_get(d), want);
	CHECK(same);
	dstr_clear(d);
}

/* Returns the tip of the ref @p ref of @p store. */
static struct oid
test_tip (struct store const *store,
          char const         *ref)
{
	char const *const words[] = { ref };
	struct buf out = {};
	struct child_end end;
	CHECK(!store_run(store, STORE_HEAD, words, nullptr, (struct span){}, &out, &end) &&
	      !end.status);
	struct span const s = buf_peek(&out);
	struct oid tip;
	CHECK(!oid_init_line(&tip, (struct cstr_view){ (char const *)s.ptr, s.len }));
	buf_fini(&out);
	return tip;
}

/* Runs git on the loop with the words @p words of STORE_REFS for @p store,
 * and replaces the records of @p table with its output
 * (member_table_parse()). Returns the result. The scratch of the test
 * contains "x" before the parse. A parse that fails writes the cause
 * there, and a parse that succeeds writes nothing.
 */
static int
test_take (struct store const *store,
           struct member_table *table,
           struct list        *children,
           char const *const  *words)
{
	struct git_run run;
	CHECK(!store_command(store, &run, STORE_REFS, words, nullptr, (struct span){},
	                     loop_clock()));
	int e = -1;
	unsigned const before = ended;
	struct child *c = child_create(&test_loop, children, &run.c, test_git_done, nullptr, &e);
	CHECK(c && !e);
	test_until(before + 1);
	test_scratch = (struct tool_scratch){ "x" };
	e = member_table_parse(table, c);
	child_destroy(&c);
	return e;
}

/* The reads of the records of a table of @p reg, the list of the members
 * of @p store and their branches in the table, the notices of the list,
 * and the lookup of a notebook by name.
 */
static void
test_list (struct store const           *store,
           struct member_registry const *reg,
           struct blocker               *blockers,
           struct list                  *children)
{
	int e = -1;
	struct member_table *table = test_table_make(reg, blockers, children, &e);
	CHECK(table && !e);
	/* The notebook and the findings of bro, a notebook moved from the old
	 * layout without a record, a notebook of a kind that the record of the
	 * name does not have, the findings of an agent without a record or a
	 * notebook, the findings of a person, a branch that is no branch of a
	 * member, and two notebooks of the old layout. */
	test_note(store, "refs/heads/agent/bro/notes", "Add \"plan.md\" (1 byte)", "alice");
	test_note(store, "refs/heads/person/moved/notes", "Add \"a.md\" (1 byte)", nullptr);
	test_note(store, "refs/heads/person/bro/notes", "Add \"b.md\" (1 byte)", nullptr);
	test_note(store, "refs/heads/agent/bro/findings", "Add bro#1", nullptr);
	test_note(store, "refs/heads/agent/ghost/findings", "Add ghost#1", nullptr);
	test_note(store, "refs/heads/person/moved/findings", "Add moved#1", nullptr);
	test_note(store, "refs/heads/agent/bro/review", "Review", nullptr);
	test_note(store, "refs/heads/notes/bro/anonymous/upstream", "Add \"c.md\" (1 byte)",
	          nullptr);
	test_note(store, "refs/heads/notes/cuh/anonymous/client", "Add \"d.md\" (1 byte)",
	          nullptr);
	struct oid const findings[] = {
		test_tip(store, "refs/heads/agent/bro/findings"),
		test_tip(store, "refs/heads/agent/ghost/findings"),
		test_tip(store, "refs/heads/person/moved/findings"),
	};
	/* The output of git that lists the records replaces the records of the
	 * table. Output in another form leaves the records as they are. */
	char const *const other[STORE_WORDS] = { "--format=%(refname)", "refs/askai/" };
	CHECK(test_take(store, table, children, other) == EIO &&
	      test_cause("Could not read the member records: the output of git is not valid."));
	CHECK(!member_table_name(table, CSTR_VIEW("bro"), &(struct member){}) &&
	      member_table_name(table, CSTR_VIEW("moved"), &(struct member){}) == ENOENT);
	/* A git that fails leaves the records as they are too. git exits with
	 * status 128 for a format with an unknown field. */
	char const *const wrong[STORE_WORDS] = { "--format=%(nosuchfield)", "refs/askai/" };
	CHECK(test_take(store, table, children, wrong) == EIO &&
	      test_cause("Could not read the member records: git exited with status 128."));
	CHECK(!member_table_name(table, CSTR_VIEW("bro"), &(struct member){}) &&
	      member_table_name(table, CSTR_VIEW("moved"), &(struct member){}) == ENOENT);
	CHECK(!test_take(store, table, children, member_list) && test_cause("x"));
	CHECK(list_is_empty(children));
	CHECK(member_table_parse(nullptr, nullptr) == EINVAL);
	CHECK(member_table_parse(table, nullptr) == EINVAL && test_cause("x"));
	CHECK(member_table_parse(nullptr, &(struct child){}) == EINVAL && test_cause("x"));
	/* The persons come first, then the agents, each by name. The user who
	 * runs the test is a person of the table too. */
	char user[MEMBER_NAME], uid[MEMBER_LOGIN];
	test_user(user, uid);
	char const *names[] = {
		"alice", "carol", "dave", "frank", "moved", "uid-4242420042", user, "bro", "claude",
		"cuh", "ghost",
	};
	for (size_t i = 1; i < 7; ++i)
		for (size_t j = i; j && strcmp(names[j - 1], names[j]) > 0; --j) {
			char const *t = names[j];
			names[j] = names[j - 1];
			names[j - 1] = t;
		}
	size_t count = 0;
	size_t width = 0;
	struct member_info const *members = member_table_members(table, &count, &width);
	CHECK(members && count == array_size(names));
	size_t n = 0;
	struct member_info const *info;
	dstr_auto notice = dstr("[askai: Members of this session: ");
	dstr_auto unknown = dstr("No member is named own. The members are ");
	for (; n < count && n < array_size(names); ++n) {
		info = &members[n];
		CHECK(cstr_view_equal(info->name, cstr_view(names[n])));
		bool const bro = !strcmp(names[n], "bro");
		bool const moved = !strcmp(names[n], "moved");
		bool const ghost = !strcmp(names[n], "ghost");
		CHECK(info->kind == (n < 7 ? MEMBER_PERSON : MEMBER_AGENT));
		CHECK(info->flags == (moved ? MEMBER_NOTES : bro ? MEMBER_RECORD | MEMBER_NOTES
		                                           : ghost ? 0 : MEMBER_RECORD));
		/* The commit of the record is set only for a member with a record. */
		CHECK(!(info->flags & MEMBER_RECORD) == !oid_len(&info->record));
		struct oid const *tip = bro ? &findings[0] : ghost ? &findings[1]
		                      : moved ? &findings[2] : &(struct oid){};
		CHECK(oid_equal(&info->findings, tip));
		char const *sep = !n ? "" : n + 1 < array_size(names) ? ", " : " and ";
		char const *kind = n < 7 ? "person" : "agent";
		CHECK(dstr_printf(&notice, "%s%s (%s%s)", sep, names[n], kind, bro ? ", you" : ""));
		CHECK(dstr_printf(&unknown, "%s%s (%s)", sep, names[n], kind));
		if (!bro)
			continue;
		CHECK(cstr_view_equal(info->change, CSTR_VIEW("Add \"plan.md\" (1 byte)")));
		CHECK(cstr_view_equal(info->asker, CSTR_VIEW("alice")) && info->time > 1700000000);
	}
	CHECK(n == array_size(names));
	size_t longest = 0;
	for (size_t i = 0; i < array_size(names); ++i) {
		size_t const len = strlen(names[i]);
		longest = len > longest ? len : longest;
	}
	CHECK(width == longest);
	size_t none = 9;
	size_t none_width = 9;
	CHECK(!member_table_members(nullptr, &none, &none_width) && !none && !none_width);
	CHECK(member_table_members(table, &none, nullptr) == members && none == count);
	CHECK(dstr_cat(&notice, ". Call notes with action members to see what each member wrote "
	               "last.]\n") && dstr_cat(&unknown, "."));
	struct member member;
	memset(&member, 'x', sizeof member);
	CHECK(!member_table_name(table, CSTR_VIEW("bro"), &member));
	CHECK(member.kind == MEMBER_AGENT && !strcmp(member.name, "bro") &&
	      test_name_zeroed(&member));
	memset(&member, 'x', sizeof member);
	CHECK(!member_table_name(table, CSTR_VIEW("moved"), &member));
	CHECK(member.kind == MEMBER_PERSON && !strcmp(member.name, "moved") &&
	      test_name_zeroed(&member));
	CHECK(member_table_name(table, CSTR_VIEW("nobody"), &member) == ENOENT);
	CHECK(member_table_name(table, CSTR_VIEW("anonymous"), &member) == ENOENT);
	CHECK(member_table_name(nullptr, CSTR_VIEW("bro"), &member) == EINVAL);
	CHECK(member_table_name(table, CSTR_VIEW("bro"), nullptr) == EINVAL);

	dstr_auto d = DSTR_INIT;
	struct member const bro = { MEMBER_AGENT, "bro" };
	CHECK(!member_table_notice(table, &bro, &d));
	test_text(&d, dstr_get(&notice));
	CHECK(!member_table_unknown(table, CSTR_VIEW("own"), &d));
	test_text(&d, dstr_get(&unknown));
	struct member_call call = { .asker = { MEMBER_PERSON, "alice" } };
	CHECK(!member_notice(&bro, &call, &d));
	test_text(&d, "[askai: You are the agent bro. A client program of the person alice made "
	          "this ask call.]\n");
	call.asker = (struct member){ MEMBER_AGENT, "claude" };
	CHECK(!member_notice(&bro, &call, &d));
	test_text(&d, "[askai: You are the agent bro. The agent claude made this ask call.]\n");
	call.asker = (struct member){ MEMBER_AGENT, "" };
	CHECK(!member_notice(&bro, &call, &d));
	test_text(&d, "[askai: You are the agent bro. A client without a member name made this "
	          "ask call.]\n");
	CHECK(member_table_notice(nullptr, &bro, &d) == EINVAL);
	CHECK(member_table_notice(table, nullptr, &d) == EINVAL);
	CHECK(member_table_notice(table, &bro, nullptr) == EINVAL);
	CHECK(member_table_unknown(nullptr, CSTR_VIEW("own"), &d) == EINVAL);
	CHECK(member_table_unknown(table, CSTR_VIEW("own"), nullptr) == EINVAL);
	CHECK(member_notice(nullptr, &call, &d) == EINVAL);
	CHECK(member_notice(&bro, nullptr, &d) == EINVAL);
	CHECK(member_notice(&bro, &call, nullptr) == EINVAL);
	CHECK(!*dstr_get(&d));

	/* A branch without a record makes a member of the kind of the
	 * branch, also for a name of a member of the other kind without a
	 * record. A record of the name decides the kind. */
	test_note(store, "refs/heads/person/ghost/notes", "Add \"g.md\" (1 byte)", nullptr);
	CHECK(!test_take(store, table, children, member_list));
	size_t ghosts = 0;
	members = member_table_members(table, &count, &width);
	for (size_t i = 0; i < count; ++i) {
		info = &members[i];
		if (!cstr_view_equal(info->name, CSTR_VIEW("ghost")))
			continue;
		++ghosts;
		CHECK(info->kind == MEMBER_PERSON ? info->flags == MEMBER_NOTES &&
		                                    !oid_len(&info->findings)
		                                  : !info->flags && oid_equal(&info->findings,
		                                                              &findings[1]));
	}
	CHECK(ghosts == 2 && count == n + 1 && width == longest);
	CHECK(!member_table_name(table, CSTR_VIEW("ghost"), &member));
	CHECK(member.kind == MEMBER_PERSON && !strcmp(member.name, "ghost"));

	/* The notices name the first 32 members and count the others. */
	for (size_t i = 0; i < 21; ++i) {
		char ref[64];
		CHECK(snprintf(ref, sizeof ref, "refs/heads/agent/zz%02zu/notes", i) > 0);
		test_note(store, ref, "Add \"z.md\" (1 byte)", nullptr);
	}
	CHECK(!test_take(store, table, children, member_list));
	members = member_table_members(table, &count, nullptr);
	CHECK(count == 33);
	dstr_auto many = dstr("[askai: Members of this session: ");
	for (size_t i = 0; i < count && i < 32; ++i) {
		info = &members[i];
		bool const agent = info->kind == MEMBER_AGENT;
		bool const you = agent && cstr_view_equal(info->name, CSTR_VIEW("bro"));
		CHECK(dstr_printf(&many, "%s%.*s (%s%s)", i ? ", " : "", (int)info->name.len,
		                  info->name.data, agent ? "agent" : "person", you ? ", you" : ""));
	}
	CHECK(dstr_cat(&many, " and 1 more. Call notes with action members to see what each "
	               "member wrote last.]\n"));
	CHECK(!member_table_notice(table, &bro, &d));
	test_text(&d, dstr_get(&many));
	CHECK(!member_table_unknown(table, CSTR_VIEW("own"), &d));
	struct cstr_view const all = cstr_view_dstr(&d);
	CHECK(cstr_view_starts(all, CSTR_VIEW("No member is named own. The members are alice "
	                                      "(person), carol (person), ")) &&
	      cstr_view_ends(all, CSTR_VIEW(", zz18 (agent), zz19 (agent) and 1 more.")));
	dstr_clear(&d);
	member_table_destroy(&table);
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
	struct member_table *table = test_table_make(reg, blockers, children, &e);
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
	table = test_table_make(reg, blockers, children, &e);
	CHECK(table && !e);
	login = test_login(MEMBER_PERSON, "uid 4242420051", "");
	char const *const unnamed = "Could not look up the name of uid 4242420051. Try again.";
	CHECK(member_table_claim(table, &login, &wait) == EINPROGRESS);
	uint64_t const deadline = loop_clock() + MEMBER_NS + 10 * NS;
	while (ended == before && loop_clock() < deadline)
		CHECK(!loop_turn(&test_loop, deadline));
	CHECK(ended == before + 1 && wait.e == ENODATA && test_why(&wait, &login, unnamed) &&
	      test_by_none(&wait) && !*wait.member.name && test_name_zeroed(&wait.member));
	CHECK(!test_exists(store, "refs/askai/members/uid-4242420051"));
	CHECK(!member_table_find(table, &login, &wait) &&
	      !strcmp(wait.member.name, "uid-4242420051") && test_name_zeroed(&wait.member));
	CHECK(member_table_claim(table, &login, &wait) == ENODATA && wait.e == ENODATA &&
	      test_why(&wait, &login, unnamed) && test_by_none(&wait) && !*wait.member.name &&
	      test_name_zeroed(&wait.member));
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
	table = test_table_make(reg, blockers, children, &e);
	CHECK(table && !e);
	login = test_login(MEMBER_PERSON, "uid 4242420052", "");
	unsigned const stopped = ended;
	CHECK(member_table_claim(table, &login, &wait) == EINPROGRESS);
	test_git_ended(children);
	blocker_stop(blockers);
	CHECK(!eventfd_write(test_gate, 1));
	test_until(stopped + 1);
	CHECK(wait.e == ECANCELED && test_why(&wait, &login, "askai stops.") &&
	      test_by_none(&wait) && !*wait.member.name && test_name_zeroed(&wait.member));
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
	struct child_end end;
	CHECK(!store_run(store, STORE_CREATE, nullptr, nullptr, (struct span){}, &out, &end) &&
	      !end.status);
	struct store_commit const commit = {
		.committer = { CSTR_VIEW("old"), CSTR_VIEW("askai") },
		.message = CSTR_VIEW("Create the session"),
		.trailers = t,
		.n_trailers = n,
		.ref = "refs/askai/session",
		.tip = &(struct oid){},
	};
	CHECK(!store_commit_input(&commit, &in));
	CHECK(!store_run(store, STORE_COMMIT, nullptr, commit.ref, buf_peek(&in), &out, &end) &&
	      !end.status);
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

/* Two tables of two services claim one new login at the same time. Each
 * table finds the login first, so that the claims start their commits in
 * one turn of the loop. The commit that loses the race reads the records
 * again and finds the record of the other commit.
 */
static void
test_race (struct store const           *store,
           struct member_registry const *reg,
           struct blocker               *blockers,
           struct list                  *children)
{
	struct member_registry *other = test_registry_make(store, "bro", "gpu5", 0, "");
	int e = -1;
	struct member_table *tables[2] = {
		test_table_make(reg, blockers, children, &e),
		other ? test_table_make(other, blockers, children, &e) : nullptr,
	};
	CHECK(tables[0] && tables[1]);
	if (!tables[0] || !tables[1]) {
		member_table_destroy(&tables[0]);
		member_registry_destroy(&other);
		return;
	}
	struct member_login const login = test_login(MEMBER_PERSON, "identity zoe", "zoe");
	struct member_wait waits[2] = { { .done = test_ended }, { .done = test_ended } };
	for (size_t i = 0; i < array_size(tables); ++i)
		test_find(tables[i], member_table_find, &login, 0, MEMBER_PERSON, "zoe", "");
	CHECK(!test_exists(store, "refs/askai/members/zoe"));
	unsigned const before = ended;
	for (size_t i = 0; i < array_size(tables); ++i)
		CHECK(member_table_claim(tables[i], &login, &waits[i]) == EINPROGRESS);
	CHECK(!list_is_empty(children));
	test_until(before + 2);
	for (size_t i = 0; i < array_size(waits); ++i)
		CHECK(!waits[i].e && waits[i].member.kind == MEMBER_PERSON &&
		      !strcmp(waits[i].member.name, "zoe") && test_why(&waits[i], &login, "") &&
		      test_by_none(&waits[i]) && test_name_zeroed(&waits[i].member));
	CHECK(test_has(store, "refs/askai/members/zoe", "\n\nAdd the person zoe\n\nKind: person\n"
	               "Login: identity zoe\n"));
	CHECK(!test_in(store, "refs/askai/members/zoe", "\nparent ", false));
	CHECK(test_in(store, "refs/askai/members/zoe", "\ncommitter gpu1 <askai> ", false) !=
	      test_in(store, "refs/askai/members/zoe", "\ncommitter gpu5 <askai> ", false));
	/* Both tables answer later finds and claims without git. */
	for (size_t i = 0; i < array_size(tables); ++i) {
		CHECK(!member_table_claim(tables[i], &login, &waits[i]) && !waits[i].e &&
		      !strcmp(waits[i].member.name, "zoe") && test_name_zeroed(&waits[i].member));
		member_table_destroy(&tables[i]);
	}
	CHECK(list_is_empty(children));
	member_registry_destroy(&other);
}

/* Ends a child of test_busy() that its owner did not let go of. */
static void
test_slept (struct child *)
{
	CHECK(!"a child of test_busy() ended");
}

/* With CHILD_MAX children that run, git cannot start for a lookup. The
 * lookup ends at once with EBUSY, and the table writes no text. The claim
 * works once the children have ended.
 */
static void
test_busy (struct store const           *store,
           struct member_registry const *reg,
           struct blocker               *blockers,
           struct list                  *children)
{
	extern char **environ;
	static char const *const argv[] = { "sleep", "60", nullptr };
	struct child_cmd const cmd = child_cmd("sleep", argv, environ, (struct span){}, 16, 0, 0,
	                                       0);
	test_git_ended(children);
	struct child *sleepers[CHILD_MAX] = {};
	int e = -1;
	for (size_t i = 0; i < array_size(sleepers); ++i) {
		sleepers[i] = child_create(&test_loop, children, &cmd, test_slept, nullptr, &e);
		CHECK(sleepers[i] && !e);
	}
	CHECK(!child_create(&test_loop, children, &cmd, test_slept, nullptr, &e) && e == EBUSY);
	struct member_table *table = test_table_make(reg, blockers, children, &e);
	CHECK(table && !e);
	struct member_login const login = test_login(MEMBER_PERSON, "identity kim", "kim");
	struct member_wait wait = { .done = test_ended };
	unsigned const before = ended;
	test_scratch = (struct tool_scratch){ "x" };
	CHECK(member_table_claim(table, &login, &wait) == EBUSY && wait.e == EBUSY &&
	      test_by_none(&wait) && !*wait.member.name && test_name_zeroed(&wait.member) &&
	      ended == before && test_cause("x"));
	CHECK(test_why(&wait, &login, "askai is busy. Try again in a few seconds."));
	for (size_t i = 0; i < array_size(sleepers); ++i)
		child_destroy(&sleepers[i]);
	test_git_ended(children);
	test_find(table, member_table_claim, &login, 0, MEMBER_PERSON, "kim", "");
	CHECK(test_has(store, "refs/askai/members/kim", "Login: identity kim\n"));
	member_table_destroy(&table);
}

/* The Unix login of a user who is named like an agent, in the new
 * repository @p dir/user.git whose agent is named like the user who runs
 * the test. The login of the client is without a name, because the table
 * looks up the name of the user. The record of the agent refuses the
 * login, and the record in the wait names the user, so the sentence for
 * the client names the user too.
 */
static void
test_user_refused (char const     *dir,
                   struct blocker *blockers,
                   struct list    *children)
{
	char user[MEMBER_NAME], text[MEMBER_LOGIN], path[4096], why[256], taken[256];
	test_user(user, text);
	CHECK(snprintf(path, sizeof path, "%s/user.git", dir) > 0);
	CHECK(snprintf(taken, sizeof taken, "Your login name %s is the name of an agent of this "
	               "session. Ask the operator to rename the agent.", user) > 0);
	int e = -1;
	struct store *store = store_create(path, why, sizeof why, &e);
	CHECK(store && !e);
	struct member_registry *reg = store ? test_registry_make(store, user, "gpu1", 0, "")
	                                    : nullptr;
	struct member_table *table = reg ? test_table_make(reg, blockers, children, &e) : nullptr;
	CHECK(table);
	if (table) {
		struct member_login const login = test_login(MEMBER_PERSON, text, "");
		test_find(table, member_table_find, &login, EPERM, MEMBER_AGENT, user, taken);
		struct member_wait wait = { .done = test_ended };
		unsigned const before = ended;
		CHECK(member_table_claim(table, &login, &wait) == EPERM && wait.e == EPERM &&
		      test_by(&wait, MEMBER_AGENT, user) && test_why(&wait, &login, taken) &&
		      !*wait.member.name && test_name_zeroed(&wait.member) && ended == before);
		char ref[MEMBER_REF];
		CHECK(snprintf(ref, sizeof ref, "refs/askai/members/%s", user) > 0);
		CHECK(test_has(store, ref, "Kind: agent\n") &&
		      !test_in(store, ref, "Login:", false));
	}
	member_table_destroy(&table);
	member_registry_destroy(&reg);
	store_destroy(&store);
}

/* Ends a wait whose result the test reads from the wait. */
static void
test_ignored (struct member_wait *)
{
}

/* The reasons of a git that fails, for the registry with git on the calling
 * thread and for a table with git on the loop: an exit with a status other
 * than 0, the errors of a git that writes, a signal, an output longer than
 * STORE_MAX, no git to run, and a git that runs longer than GIT_RUN_NS. A
 * script named git in a new directory @p dir/fake-NAME at the start of
 * PATH runs in place of git, and a PATH of only an empty directory names
 * no git. The lookups that fail leave no lookup on the table, and the next
 * claim of the login writes the record.
 */
static void
test_ends (char const                   *dir,
           struct store const           *store,
           struct member_registry const *reg,
           struct blocker               *blockers,
           struct list                  *children)
{
	enum { FAIL, KILL, CUT, LATE, NONE, LONG, FAKES };
	struct {
		char const *name;
		char const *script;
	} const fakes[FAKES] = {
		[FAIL] = { "fail", "echo 'fatal: boom' >&2\nexit 3\n" },
		[KILL] = { "kill", "kill -KILL $$\n" },
		[CUT]  = { "cut",  "exec head -c 16777217 /dev/zero\n" },
		[LATE] = { "late", "exec sleep 60\n" },
		[NONE] = { "none", nullptr },
		[LONG] = { "long", "printf 'fatal: %0250d\\n' 0 >&2\nexit 3\n" },
	};
	char *old = strdup(getenv("PATH"));
	CHECK(old);
	if (!old)
		return;
	static char paths[FAKES][8192];
	for (size_t i = 0; i < FAKES; ++i) {
		char sub[4096], file[4096];
		CHECK(snprintf(sub, sizeof sub, "%s/fake-%s", dir, fakes[i].name) > 0 &&
		      !mkdir(sub, 0700));
		CHECK(snprintf(paths[i], sizeof paths[i], fakes[i].script ? "%s:%s" : "%s", sub,
		               old) > 0);
		if (!fakes[i].script)
			continue;
		CHECK(snprintf(file, sizeof file, "%s/git", sub) > 0);
		FILE *f = fopen(file, "we");
		CHECK(f && fprintf(f, "#!/bin/sh\n%s", fakes[i].script) > 0 && !fclose(f));
		CHECK(!chmod(file, 0700));
	}

	/* The read of a table that runs longer than GIT_RUN_NS ends while the
	 * other cases run. */
	int e = -1;
	struct member_table *late = test_table_make(reg, blockers, children, &e);
	struct member_table *table = test_table_make(reg, blockers, children, &e);
	CHECK(late && table);
	if (!late || !table) {
		member_table_destroy(&late);
		free(old);
		return;
	}
	struct member_wait slow = { .done = test_ignored };
	struct member_login const gil = test_login(MEMBER_PERSON, "identity gil", "gil");
	struct member_login login = gil;
	unsigned const before = ended;
	CHECK(!setenv("PATH", paths[LATE], 1));
	CHECK(member_table_claim(late, &gil, &slow) == EINPROGRESS);
	CHECK(!setenv("PATH", old, 1));

	/* The registry reads the records of a session that exists, and creates
	 * a new repository. */
	char const *const reads[] = {
		[FAIL] = "Could not read the member records: git exited with status 3.",
		[KILL] = "Could not read the member records: git was killed by signal 9.",
		[CUT]  = "Could not read the member records: git wrote more than 16 MiB.",
	};
	for (size_t i = 0; i < array_size(reads); ++i) {
		CHECK(!setenv("PATH", paths[i], 1));
		test_registry_make(store, "bro", "gpu1", EIO, reads[i]);
		CHECK(!setenv("PATH", old, 1));
	}
	CHECK(!setenv("PATH", paths[NONE], 1));
	test_registry_make(store, "bro", "gpu1", EIO, "Could not read the member records: No such "
	                   "file or directory.");
	CHECK(!setenv("PATH", old, 1));
	/* A reason that does not fit its storage ends with "...", for a cut in
	 * the cause and for a cut before the cause. */
	char cut[48] = "";
	CHECK(!setenv("PATH", paths[FAIL], 1));
	CHECK(!member_registry_create(store, "bro", "gpu1", cut, sizeof cut, &e) && e == EIO &&
	      !strcmp(cut, "Could not read the member records: git exite..."));
	CHECK(!member_registry_create(store, "bro", "gpu1", cut, 24, &e) && e == EIO &&
	      !strcmp(cut, "Could not read the m..."));
	CHECK(!setenv("PATH", old, 1));
	char path[4096], why[256];
	CHECK(snprintf(path, sizeof path, "%s/new.git", dir) > 0);
	struct store *made = store_create(path, why, sizeof why, &e);
	CHECK(made && !e);
	CHECK(!setenv("PATH", paths[FAIL], 1));
	test_registry_make(made, "bro", "gpu1", EIO, "Could not create the repository: fatal: boom");
	CHECK(!setenv("PATH", old, 1));
	CHECK(!store_exists(made));
	store_destroy(&made);

	/* The reads and the commits of a table. A git that cannot start ends
	 * the lookup at once. */
	struct member_wait wait = { .done = test_ended };
	login = test_login(MEMBER_PERSON, "identity hal", "hal");
	CHECK(!setenv("PATH", paths[FAIL], 1));
	CHECK(member_table_claim(table, &login, &wait) == EINPROGRESS);
	CHECK(!setenv("PATH", old, 1));
	test_until(before + 1);
	static char const failed[] = "askai could not read or write the member records.";
	CHECK(wait.e == EIO && test_cause(reads[FAIL]) && test_why(&wait, &login, failed) &&
	      test_by_none(&wait) && !*wait.member.name && test_name_zeroed(&wait.member));
	test_find(table, member_table_claim, &login, 0, MEMBER_PERSON, "hal", "");
	CHECK(test_has(store, "refs/askai/members/hal", "Login: identity hal\n"));
	login = test_login(MEMBER_PERSON, "identity ian", "ian");
	CHECK(!setenv("PATH", paths[NONE], 1));
	test_scratch = (struct tool_scratch){ "x" };
	CHECK(member_table_claim(table, &login, &wait) == EIO && wait.e == EIO &&
	      test_by_none(&wait) && !*wait.member.name && test_name_zeroed(&wait.member));
	CHECK(!setenv("PATH", old, 1));
	/* The table logs why git cannot start, and writes no text. */
	CHECK(test_why(&wait, &login, failed) && test_cause("x"));
	test_find(table, member_table_find, &login, 0, MEMBER_PERSON, "ian", "");
	CHECK(!setenv("PATH", paths[FAIL], 1));
	unsigned const found = ended;
	CHECK(member_table_claim(table, &login, &wait) == EINPROGRESS);
	CHECK(!setenv("PATH", old, 1));
	test_until(found + 1);
	CHECK(wait.e == EIO && test_cause("Could not write refs/askai/members/ian: fatal: boom") &&
	      test_why(&wait, &login, failed) && test_by_none(&wait) && !*wait.member.name &&
	      test_name_zeroed(&wait.member));
	CHECK(!test_exists(store, "refs/askai/members/ian"));
	test_find(table, member_table_claim, &login, 0, MEMBER_PERSON, "ian", "");
	CHECK(test_has(store, "refs/askai/members/ian", "Login: identity ian\n"));

	/* A long cause is whole in the scratch of the table. The cause is 301
	 * bytes: the ref of a name of UTIL_WORD_MAX bytes, and the first 200
	 * bytes of the error line of git. */
	char name[MEMBER_NAME], text[MEMBER_LOGIN], ref[MEMBER_REF], reason[512];
	memset(name, 'n', sizeof name - 1);
	name[sizeof name - 1] = '\0';
	CHECK(snprintf(text, sizeof text, "identity %s", name) == 9 + UTIL_WORD_MAX);
	CHECK(snprintf(ref, sizeof ref, "refs/askai/members/%s", name) == 19 + UTIL_WORD_MAX);
	CHECK(snprintf(reason, sizeof reason, "Could not write %s: fatal: %0193d", ref, 0) == 301);
	login = test_login(MEMBER_PERSON, text, name);
	test_find(table, member_table_find, &login, 0, MEMBER_PERSON, name, "");
	CHECK(!setenv("PATH", paths[LONG], 1));
	unsigned const whole = ended;
	CHECK(member_table_claim(table, &login, &wait) == EINPROGRESS);
	CHECK(!setenv("PATH", old, 1));
	test_until(whole + 1);
	CHECK(wait.e == EIO && test_cause(reason) && test_why(&wait, &login, failed) &&
	      !test_exists(store, ref) && !*wait.member.name && test_name_zeroed(&wait.member));

	/* The read that runs too long ends the lookup, and writes no record. */
	uint64_t const deadline = loop_clock() + GIT_RUN_NS + 10 * NS;
	while (slow.e == EINPROGRESS && loop_clock() < deadline)
		CHECK(!loop_turn(&test_loop, deadline));
	CHECK(slow.e == EIO && test_cause("Could not read the member records: git took longer "
	                                  "than 10 s, and was stopped.") &&
	      test_why(&slow, &gil, failed) && !*slow.member.name &&
	      test_name_zeroed(&slow.member));
	CHECK(!test_exists(store, "refs/askai/members/gil"));
	member_table_destroy(&late);
	member_table_destroy(&table);
	CHECK(list_is_empty(children));
	free(old);
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
	test_call_ids();
	test_sentences();
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
	struct member_session const *session = member_registry_session(reg);
	CHECK(session && hexid_valid(session->id) && !member_registry_session(nullptr));
	CHECK(cstr_view_equal(session->name, CSTR_VIEW("test1")) &&
	      cstr_view_equal(session->committer.name, CSTR_VIEW("gpu1")) &&
	      cstr_view_equal(session->committer.email, CSTR_VIEW("askai")) &&
	      session->agent.kind == MEMBER_AGENT && !strcmp(session->agent.name, "bro") &&
	      !strcmp(session->refs[MEMBER_BRANCH_NOTES], "refs/heads/agent/bro/notes") &&
	      !strcmp(session->refs[MEMBER_BRANCH_FINDINGS], "refs/heads/agent/bro/findings") &&
	      !session->legacy);
	member_registry_report(reg);

	/* The byline of a commit of a turn names the author, the committer of
	 * the session, and the ask call, the call, the conversation and the
	 * asker. A commit of a client names none of them. */
	struct member_call call = { .asker = { MEMBER_PERSON, "alice" } };
	CHECK(!hexid_init(&call.ask, CSTR_VIEW("0123456789abcdef0123456789abcdef")) &&
	      !hexid_init(&call.conversation, CSTR_VIEW("fedcba9876543210fedcba9876543210")));
	struct member_byline byline;
	struct store_commit commit = {};
	member_byline_put(&byline, &commit, &(struct member){ MEMBER_PERSON, "carol" }, &call, 7,
	                  session);
	CHECK(cstr_view_equal(commit.author.name, CSTR_VIEW("carol")) &&
	      cstr_view_equal(commit.author.email, CSTR_VIEW("person/carol")) &&
	      commit.committer.name.data == session->committer.name.data &&
	      cstr_view_equal(commit.committer.email, CSTR_VIEW("askai")) &&
	      commit.trailers == byline.trailers && commit.n_trailers == 4);
	struct cstr_view const lines[][2] = {
		{ CSTR_VIEW("Ask"),          CSTR_VIEW("0123456789abcdef0123456789abcdef") },
		{ CSTR_VIEW("Call"),         CSTR_VIEW("7") },
		{ CSTR_VIEW("Conversation"), CSTR_VIEW("fedcba9876543210fedcba9876543210") },
		{ CSTR_VIEW("Asked-by"),     CSTR_VIEW("alice") },
	};
	for (size_t i = 0; i < array_size(lines) && i < commit.n_trailers; ++i)
		CHECK(cstr_view_equal(commit.trailers[i].key, lines[i][0]) &&
		      cstr_view_equal(commit.trailers[i].value, lines[i][1]));
	member_byline_put(&byline, &commit, &session->agent, &(struct member_call){}, 0, session);
	CHECK(cstr_view_equal(commit.author.name, CSTR_VIEW("bro")) &&
	      cstr_view_equal(commit.author.email, CSTR_VIEW("agent/bro")) && !commit.n_trailers);

	/* Another service of the agent adopts its record, and another agent
	 * gets a record. The session stays. */
	struct buf head = {};
	struct child_end end;
	char const *const bro[] = { "refs/askai/members/bro" };
	CHECK(!store_run(store, STORE_HEAD, bro, nullptr, (struct span){}, &head, &end));
	struct member_registry *again = test_registry_make(store, "bro", "gpu2", 0, "");
	CHECK(!store_run(store, STORE_HEAD, bro, nullptr, (struct span){}, &head, &end));
	struct span const heads = buf_peek(&head);
	CHECK(heads.len == 2 * (OID_SHA1 + 1) && !memcmp(heads.ptr, heads.ptr + OID_SHA1 + 1,
	                                                  OID_SHA1));
	buf_fini(&head);
	CHECK(cstr_view_equal(member_registry_session(again)->id, session->id));
	member_registry_destroy(&again);
	CHECK(!again);
	member_registry_destroy(&again);
	member_registry_destroy(nullptr);
	/* The registry copies the name of the agent. */
	char cuh[] = "cuh";
	again = test_registry_make(store, cuh, "gpu3", 0, "");
	cuh[0] = 'x';
	CHECK(again && !strcmp(member_registry_session(again)->agent.name, "cuh") &&
	      !strcmp(member_registry_session(again)->refs[MEMBER_BRANCH_FINDINGS],
	              "refs/heads/agent/cuh/findings"));
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
	test_list(store, reg, blockers, &children);
	test_race(store, reg, blockers, &children);
	test_busy(store, reg, blockers, &children);
	test_user_refused(argv[1], blockers, &children);
	test_ends(argv[1], store, reg, blockers, &children);
	test_blocked(store, reg, &children);

	/* The agent of a service adopts the record of an agent identity. The
	 * registry counts the notebooks of the old layout of test_list(). */
	again = test_registry_make(store, "claude", "gpu4", 0, "");
	CHECK(again && member_registry_session(again)->legacy == 2);
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
