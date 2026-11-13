/** @file member.c
 *
 * The members of the session (src/member.h).
 */
#define _GNU_SOURCE /* memrchr() */
#include <errno.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "blocker.h"
#include "buf.h"
#include "child.h"
#include "dstr.h"
#include "git_run.h"
#include "hexid.h"
#include "jb.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "member.h"
#include "oid.h"
#include "policy.h"
#include "store.h"
#include "tool.h"
#include "util.h"
#include "xlist.h"

/* The refs of the records and of the branches. */
#define RECORDS "refs/askai/"
#define MEMBERS RECORDS "members/"
#define SESSION RECORDS "session"
#define HEADS   "refs/heads/"
#define LEGACY  HEADS "notes/"

enum {
	MEMBER_SHOWN = 32,  /* members that a notice names, at most */
};

static_assert(sizeof MEMBERS + UTIL_WORD_MAX <= MEMBER_REF, "a record ref fits MEMBER_REF");

/* The leaves of the branches, by enum member_branch. */
static struct cstr_view const member_leaves[MEMBER_BRANCHES] = { MEMBER_BRANCH_LIST(XL_VIEW) };

/* git writes a line for each ref: the ref, the commit, the values of the
 * trailer lines Kind, Login, Session, Name and Format, the commit time, the
 * value of the trailer line Asked-by and the subject, each field followed
 * by a NUL. The values of several lines of one key are one field,
 * separated by commas.
 */
char const *const member_list[STORE_WORDS] = {
	"--format=%(refname)%00%(objectname)%00"
	"%(trailers:key=Kind,valueonly,separator=%x2C)%00"
	"%(trailers:key=Login,valueonly,separator=%x2C)%00"
	"%(trailers:key=Session,valueonly,separator=%x2C)%00"
	"%(trailers:key=Name,valueonly,separator=%x2C)%00"
	"%(trailers:key=Format,valueonly,separator=%x2C)%00"
	"%(committerdate:unix)%00"
	"%(trailers:key=Asked-by,valueonly,separator=%x2C)%00"
	"%(contents:subject)%00",
	RECORDS,
	HEADS,
};

/* The fields of a line of the output of member_list. */
enum member_field : uint8_t {
	FIELD_REF,
	FIELD_TIP,
	FIELD_KIND,
	FIELD_LOGIN,
	FIELD_SESSION,
	FIELD_NAME,
	FIELD_FORMAT,
	FIELD_TIME,
	FIELD_ASKER,
	FIELD_SUBJECT,
	FIELDS
};

/* The records of the session, as git lists them with member_list. */
struct member_records {
	struct buf                      out;      /* the output of git */
	UTIL_ARRAY(struct member_info)  all;      /* the members, in the order
	                                             of member_info_order() */
	struct cstr_view                session;  /* the session ID of the
	                                             session record, with data
	                                             nullptr without the
	                                             record */
	struct cstr_view                name;     /* its name */
	struct cstr_view                format;   /* its format */
	size_t                          legacy;   /* the notebooks of the old
	                                             layout under LEGACY */
	size_t                          width;    /* the length of the longest
	                                             name of the members */
};

struct member_registry {
	struct member_records  records;  /* the records that askai read as it
	                                    started, with the session ID and
	                                    name */
	struct member_session  session;  /* views of records, and the names of
	                                    this askai */
	struct store const    *store;
	struct member_login    own;      /* the user who runs askai */
};

struct member_table {
	struct member_records         records;   /* the newest records that git
	                                            listed. Until the first
	                                            read, the entries are a copy
	                                            of the entries of the
	                                            registry, and the views point
	                                            into its output. */
	struct list                   lookups;   /* struct member_lookup, under
	                                            way or refused */
	struct member_registry const *registry;
	struct loop                  *loop;
	struct blocker               *blockers;
	struct list                  *children;  /* the children of the loop,
	                                            which run git */
	struct tool_scratch          *scratch;   /* the scratch text of the
	                                            loop, for the cause of a
	                                            failure that the table
	                                            logs */
};

/* The claim of the member of a login. Each step (member_claim_step())
 * decides the member with the records, and writes the input of git for
 * the commit of a record that the member needs. The registry runs the
 * steps with git on the calling thread as askai starts
 * (member_registry_claim()), and a lookup runs them with git on the loop
 * of its table (member_lookup_step()).
 */
struct member_claim {
	struct member_login login;            /* the login. After
	                                         member_claim_step() returns 0,
	                                         login.member is the member of
	                                         the login. */
	uint32_t            writes;           /* the commits of the record so
	                                         far, at most STORE_TRIES */
	char                ref[MEMBER_REF];  /* the ref of the record that the
	                                         last step wrote the input
	                                         for */
};

/* The flags of a lookup. */
enum {
	LOOKUP_NAME  = 1u << 0,  /* the job name is with a blocker */
	LOOKUP_WRITE = 1u << 1,  /* a wait needs the record of the member, so
	                            git writes a missing record */
	LOOKUP_GUESS = 1u << 2,  /* the name is uid-N, because the lookup of
	                            the name of the user failed in a way that a
	                            retry can fix */
};

/* The lookup of a login whose member the records of a table do not name,
 * on the loop of the table. git reads the records into the table
 * (member_table_parse()), and the steps of the claim of the lookup decide
 * the member (member_lookup_step()). If a wait needs a record that does
 * not exist, git writes the record, and reads the records again.
 * For a Unix login that no record names, the job name first looks up the
 * name of the user on a blocker. A lookup that ends with the member, also
 * a member without a record, or with a refusal stays on the table, and
 * the later lookups of the login get that result without git. Any other
 * end removes the lookup from the table, unless the job name is with a
 * blocker, so that no uid has two jobs with the blockers. The lookup is
 * freed once it is off the table and the job name is not with a blocker.
 */
struct member_lookup {
	struct blocker_job     name;
	struct list            hook;               /* on table->lookups */
	struct list            waits;              /* struct member_wait */
	struct loop_timer      timer;              /* the deadline of the job
	                                              name */
	struct member_table   *table;              /* nullptr once off the
	                                              table */
	struct child          *git;                /* the git that reads the
	                                              records or writes the
	                                              record, or nullptr */
	struct member_claim    claim;              /* the login, the member
	                                              once the records decide,
	                                              and the record that git
	                                              writes */
	uint32_t               flags;
	int16_t                result;             /* EINPROGRESS until the
	                                              lookup ends, then the
	                                              result of the end. After
	                                              EPERM, the later lookups
	                                              of the login return the
	                                              refusal. After any other
	                                              result, a find returns
	                                              the member. */
	char                   user[MEMBER_NAME];  /* the name of the Unix
	                                              user, as the job name
	                                              finds it */
	enum member_kind       refuser_kind;       /* after EPERM, the kind of
	                                              the record that refuses
	                                              the login */
};

/* Frees the output and the records of @p r. */
static void
member_records_fini (struct member_records *r)
{
	buf_fini(&r->out);
	free(r->all.items);
	*r = (struct member_records){};
}

/* Orders the members by name. */
static int
member_info_name (void const *a,
                  void const *b)
{
	return cstr_view_compare(((struct member_info const *)a)->name,
	                         ((struct member_info const *)b)->name);
}

/* Orders the persons before the agents, each by name. */
static int
member_info_order (void const *a,
                   void const *b)
{
	uint32_t const x = ((struct member_info const *)a)->kind;
	uint32_t const y = ((struct member_info const *)b)->kind;
	return x != y ? (x > y) - (x < y) : member_info_name(a, b);
}

/* Returns the member named @p name in @p r, whose parse has sorted the
 * members (member_info_order()), or nullptr.
 */
static struct member_info *
member_records_find (struct member_records const *r,
                     struct cstr_view             name)
{
	if (!r->all.n)
		return nullptr;
	for (uint32_t kind = 0; kind < MEMBER_KINDS; ++kind) {
		struct member_info const key = { .name = name, .kind = kind };
		struct member_info *entry = bsearch(&key, r->all.items, r->all.n, sizeof key,
		                                    member_info_order);
		if (entry)
			return entry;
	}
	return nullptr;
}

/* Appends @p entry to the members of @p r. Returns the member in @p r, or
 * nullptr for ENOMEM.
 */
static struct member_info *
member_records_add (struct member_records *r,
                    struct member_info     entry)
{
	if (util_push(r->all, entry))
		return nullptr;
	if (entry.name.len > r->width)
		r->width = entry.name.len;
	return &r->all.items[r->all.n - 1];
}

void
member_ref (struct member const *member,
            enum member_branch   branch,
            char                 ref[static MEMBER_REF])
{
	(void)util_format(ref, MEMBER_REF, HEADS "%s/%s/%s", member_kinds[member->kind].data,
	                  member->name, member_leaves[branch].data);
}

int
member_ref_parse (struct cstr_view    ref,
                  enum member_kind   *kind,
                  struct cstr_view   *name,
                  enum member_branch *branch)
{
	struct cstr_view rest = cstr_view_slice(ref, sizeof HEADS - 1, SIZE_MAX);
	struct cstr_view kind_name;
	if (!cstr_view_starts(ref, CSTR_VIEW(HEADS)) || !cstr_view_cut(&rest, '/', &kind_name) ||
	    !cstr_view_cut(&rest, '/', name) || !member_name_valid(*name))
		return EINVAL;
	size_t const k = cstr_view_index(member_kinds, MEMBER_KINDS, kind_name);
	size_t const leaf = cstr_view_index(member_leaves, MEMBER_BRANCHES, rest);
	if (k == MEMBER_KINDS || leaf == MEMBER_BRANCHES)
		return EINVAL;
	*kind = (enum member_kind)k;
	*branch = (enum member_branch)leaf;
	return 0;
}

/* Adds the branch of the line @p f of the output of member_list to @p r: a
 * notebook of the old layout to the count, and a branch of a member to the
 * member of a record of the name, one of the first @p records members of
 * @p r, or as a member without a record. A branch of a name whose record
 * is of another kind is left out, and so is any other ref. Returns 0 or
 * ENOMEM.
 */
static int
member_records_branch (struct member_records  *r,
                       size_t                  records,
                       struct cstr_view const  f[FIELDS])
{
	if (cstr_view_starts(f[FIELD_REF], CSTR_VIEW(LEGACY))) {
		++r->legacy;
		return 0;
	}
	enum member_kind kind;
	struct cstr_view name;
	enum member_branch branch;
	struct oid tip;
	uint64_t time = 0;
	if (member_ref_parse(f[FIELD_REF], &kind, &name, &branch) || oid_init(&tip, f[FIELD_TIP]) ||
	    util_number(f[FIELD_TIME].data, f[FIELD_TIME].len, UINT64_MAX, &time))
		return 0;
	struct member_info const key = { .name = name, .kind = kind };
	struct member_info *entry = records ? bsearch(&key, r->all.items, records, sizeof key,
	                                              member_info_name) : nullptr;
	/* git lists the branches of a member one after the other. */
	if (!entry && r->all.n > records && !member_info_order(&key, &r->all.items[r->all.n - 1]))
		entry = &r->all.items[r->all.n - 1];
	if (entry && entry->kind != kind)
		return 0;
	if (!entry) {
		entry = member_records_add(r, key);
		if (!entry)
			return ENOMEM;
	}
	if (branch == MEMBER_BRANCH_FINDINGS) {
		entry->findings = tip;
		return 0;
	}
	entry->change = f[FIELD_SUBJECT];
	entry->asker = f[FIELD_ASKER];
	entry->time = time;
	entry->flags |= MEMBER_NOTES;
	return 0;
}

/* Parses r->out into the records of @p r. A member record whose ref,
 * commit, kind or name is not valid is left out. git lists the records
 * before the branches, in the order of their names, so bsearch() finds
 * the record of the member of a branch. Returns 0, EIO for output that is
 * not in the form of member_list, or ENOMEM.
 */
static int
member_records_parse (struct member_records *r)
{
	r->all.n = 0;
	r->legacy = 0;
	r->width = 0;
	r->session = r->name = r->format = (struct cstr_view){};
	size_t records = 0;
	struct cstr_view rest = cstr_view_span(buf_peek(&r->out));
	while (rest.len) {
		struct cstr_view f[FIELDS];
		for (size_t i = 0; i < FIELDS; ++i)
			if (!cstr_view_cut(&rest, '\0', &f[i]))
				return EIO;
		if (!rest.len || *rest.data != '\n')
			return EIO;
		rest = cstr_view_slice(rest, 1, SIZE_MAX);
		if (cstr_view_equal(f[FIELD_REF], CSTR_VIEW(SESSION))) {
			r->session = f[FIELD_SESSION];
			r->name = f[FIELD_NAME];
			r->format = f[FIELD_FORMAT];
			continue;
		}
		if (cstr_view_starts(f[FIELD_REF], CSTR_VIEW(HEADS))) {
			if (member_records_branch(r, records, f))
				return ENOMEM;
			continue;
		}
		struct cstr_view const name = cstr_view_slice(f[FIELD_REF], sizeof MEMBERS - 1,
		                                              SIZE_MAX);
		size_t const kind = cstr_view_index(member_kinds, MEMBER_KINDS, f[FIELD_KIND]);
		struct oid record;
		if (!cstr_view_starts(f[FIELD_REF], CSTR_VIEW(MEMBERS)) || !member_name_valid(name) ||
		    kind == MEMBER_KINDS || oid_init(&record, f[FIELD_TIP]))
			continue;
		if (r->all.n != records ||
		    (records && cstr_view_compare(r->all.items[records - 1].name, name) >= 0))
			return EIO;
		if (!member_records_add(r, (struct member_info){
			.name   = name,
			.logins = f[FIELD_LOGIN],
			.kind   = (uint32_t)kind,
			.flags  = MEMBER_RECORD,
			.record = record,
		}))
			return ENOMEM;
		++records;
	}
	if (r->all.n)
		qsort(r->all.items, r->all.n, sizeof *r->all.items, member_info_order);
	return 0;
}

/* Returns 0 if git, which ended as @p end says, exited with status 0, and
 * @p e is 0. @p e is the error that kept git from running or its output
 * from being read, such as an error of store_run(), or 0. Otherwise the
 * function returns EIO, and writes to the @p len bytes at @p why that
 * askai could not @p verb @p object, and then the cause. The cause is
 * store_cause() of @p out, the output of a git that writes. If that view
 * is empty, the cause is the text of @p e if git did not run or exited
 * with status 0, and else why git failed, as child_end_why() writes it.
 */
static int
member_git_result (char                   *why,
                   size_t                  len,
                   char const             *verb,
                   char const             *object,
                   struct span             out,
                   int                     e,
                   struct child_end const *end)
{
	int const status = child_end_status(end);
	if (!e && !status)
		return 0;
	size_t const at = util_format(why, len, "Could not %s %s: ", verb, object);
	struct cstr_view const cause = store_cause(out);
	/* end->code is 0 if git did not run. */
	if (cause.len)
		util_format(why + at, len - at, "%.*s", (int)cause.len, cause.data);
	else if (e && (!end->code || !status))
		util_format(why + at, len - at, "%s.", strerror(e));
	else
		(void)child_end_why(end, "git", GIT_RUN_NS, STORE_MAX, why + at, len - at);
	return EIO;
}

/* Returns the result of a commit of a record on the ref @p ref. git ended
 * as @p end says, with the output @p out, and @p e is the error that kept
 * git from running, or 0. Returns 0, EAGAIN if the output shows that
 * another writer changed the ref first, or else EIO with the reason in the
 * @p len bytes at @p why (member_git_result()).
 */
static int
member_commit_result (char                   *why,
                      size_t                  len,
                      char const             *ref,
                      struct span             out,
                      int                     e,
                      struct child_end const *end)
{
	e = member_git_result(why, len, "write", ref, out, e, end);
	return e && store_raced(out) ? EAGAIN : e;
}

/* Replaces the records of @p r with the records in @p out, the whole
 * output of a git that listed them with member_list. git ended as @p end
 * says, after the error @p e as member_git_result() reads it. The storage of
 * @p out moves to the function, and @p out is empty after the call.
 * Returns 0, or EIO or ENOMEM with the reason in the @p len bytes at
 * @p why. On failure the records stay as they are.
 */
static int
member_records_take (struct member_records  *r,
                     struct buf             *out,
                     int                     e,
                     struct child_end const *end,
                     char                   *why,
                     size_t                  len)
{
	static char const records[] = "the member records";
	struct member_records got = { .out = *out };
	*out = buf();
	int const ended = member_git_result(why, len, "read", records, span(nullptr, 0), e, end);
	e = ended ? ended : member_records_parse(&got);
	if (!e) {
		struct member_records const old = *r;
		*r = got;
		got = old;
	} else if (!ended) {
		util_format(why, len, "Could not read %s: %s", records,
		            e == EIO ? "the output of git is not valid." : "Out of memory.");
	}
	member_records_fini(&got);
	return e;
}

/* Reads the records of @p store into @p r on the calling thread. Returns
 * 0, or EIO or ENOMEM with a reason in the @p len bytes at @p why.
 */
static int
member_records_read (struct member_records *r,
                     struct store const    *store,
                     char                  *why,
                     size_t                 len)
{
	struct buf out = buf();
	struct child_end end = child_end();
	int const e = store_run(store, STORE_REFS, member_list, nullptr, span(nullptr, 0), &out, &end);
	return member_records_take(r, &out, e, &end, why, len);
}

/* Returns true if @p logins, Login values separated by commas, contains
 * @p login.
 */
static bool
member_logins_have (struct cstr_view logins,
                    struct cstr_view login)
{
	for (bool more = logins.len != 0; more; ) {
		struct cstr_view item = logins;
		more = cstr_view_cut(&logins, ',', &item);
		if (cstr_view_equal(item, login))
			return true;
	}
	return false;
}

/* Returns the record of @p r with the login @p login, or nullptr. */
static struct member_info const *
member_records_login (struct member_records const *r,
                      struct cstr_view             login)
{
	for (size_t i = 0; login.len && i < r->all.n; ++i)
		if (member_logins_have(r->all.items[i].logins, login))
			return &r->all.items[i];
	return nullptr;
}

/* Returns the member of the record @p info. */
static struct member
member_of (struct member_info const *info)
{
	return member((enum member_kind)info->kind, info->name);
}

/* Writes to @p in the input of git for a commit of a record of @p reg:
 * the message @p message and the trailer @p t of @p n lines, on the ref
 * @p ref, after the commit @p tip, or as a root commit for an empty
 * @p tip. Returns 0, or the error of store_commit_input().
 */
static int
member_commit_input (struct member_registry const *reg,
                     char const                   *ref,
                     struct cstr_view              message,
                     struct store_trailer const   *t,
                     size_t                        n,
                     struct oid const             *tip,
                     struct buf                   *in)
{
	struct store_commit const commit = {
		.author = { CSTR_VIEW("askai"), CSTR_VIEW("askai") },
		.committer = reg->session.committer,
		.message = message,
		.files = {},
		.trailers = t,
		.n_trailers = n,
		.ref = ref,
		.tip = tip,
	};
	return store_commit_input(&commit, in);
}

/* Runs git on the calling thread to commit @p in, the input of a commit
 * of a record of @p reg on the ref @p ref, and frees @p in. @p e is the
 * error of making @p in. Returns 0, EAGAIN if the output of git shows that
 * another writer changed the ref first, or else EIO with the reason in the
 * @p len bytes at @p why.
 */
static int
member_commit (struct member_registry const *reg,
               char const                   *ref,
               struct buf                   *in,
               int                           e,
               char                         *why,
               size_t                        len)
{
	struct buf out = buf();
	struct child_end end = child_end();
	if (!e)
		e = store_run(reg->store, STORE_COMMIT, nullptr, ref, buf_peek(in), &out, &end);
	buf_fini(in);
	e = member_commit_result(why, len, ref, buf_peek(&out), e, &end);
	buf_fini(&out);
	return e;
}

/* Writes to @p in the input of git for the record of @p login, the record
 * of the new member login->member, and to @p ref the ref of the record.
 * @p text is the view of login->text. With @p entry, the record of that
 * name, the commit adds the login to that record instead. Returns as
 * member_commit_input() does.
 */
static int
member_record_input (struct member_registry const *reg,
                     struct member_login const    *login,
                     struct cstr_view              text,
                     struct member_info const     *entry,
                     char                          ref[MEMBER_REF],
                     struct buf                   *in)
{
	struct cstr_view const key = CSTR_VIEW("Login");
	struct member const *member = &login->member;
	struct cstr_view const kind = member_kinds[member->kind];
	struct store_trailer t[2 + MEMBER_LOGINS] = {
		{ CSTR_VIEW("Kind"), kind }
	};
	size_t n = 1;
	struct oid const *tip = &(struct oid){};
	char message[STORE_MESSAGE];
	struct cstr_view subject;
	if (entry) {
		tip = &entry->record;
		struct cstr_view logins = entry->logins;
		for (bool more = logins.len != 0; more && n <= MEMBER_LOGINS; ) {
			struct cstr_view item = logins;
			more = cstr_view_cut(&logins, ',', &item);
			t[n++] = (struct store_trailer){ key, item };
		}
		subject = util_format_view(message, sizeof message, "Add login %s to the %s %s",
		                           login->text, kind.data, member->name);
	} else {
		subject = util_format_view(message, sizeof message, "Add the %s %s", kind.data,
		                           member->name);
	}
	if (text.len)
		t[n++] = (struct store_trailer){ key, text };
	util_format(ref, MEMBER_REF, MEMBERS "%s", member->name);
	return member_commit_input(reg, ref, subject, t, n, tip, in);
}

/* Returns the length of the kind of a login, "uid " or "identity ",
 * which ends at its first space.
 */
static size_t
member_login_kind (struct cstr_view login)
{
	char const *space = login.len ? memchr(login.data, ' ', login.len) : nullptr;
	return space ? (size_t)(space - login.data) + 1 : 0;
}

struct cstr_view
member_login_why (struct member_login const *login,
                  int                        e,
                  struct member const       *refuser,
                  char                      *buf,
                  size_t                     size)
{
	if (!login || !buf || !size)
		return (struct cstr_view){};
	switch (e) {
	case EPERM:
		if (!refuser)
			return (struct cstr_view){};
		break;
	case EIO:
		return CSTR_VIEW("askai could not read or write the member records.");
	case EAGAIN:
		return CSTR_VIEW("Other askai services changed your member record at the same "
		                 "time. Try again.");
	case EBUSY:
		return CSTR_VIEW("askai is busy. Try again in a few seconds.");
	case ENODATA:
		return util_format_view(buf, size, "Could not look up the name of %s. Try again.",
		                        login->text);
	case ECANCELED:
		return CSTR_VIEW("askai stops.");
	default:
		return (struct cstr_view){};
	}
	/* The kind of the login, then the kind of the record. */
	switch (login->member.kind << 1 | refuser->kind) {
	case MEMBER_AGENT << 1 | MEMBER_AGENT:
		return util_format_view(buf, size, "Your login name %s is the name of another "
		                        "agent of this session.", refuser->name);
	case MEMBER_PERSON << 1 | MEMBER_PERSON:
		return util_format_view(buf, size, "Your login name %s is the name of another "
		                        "person of this session.", refuser->name);
	case MEMBER_PERSON << 1 | MEMBER_AGENT:
		return util_format_view(buf, size, "Your login name %s is the name of an agent of "
		                        "this session. Ask the operator to rename the agent.",
		                        refuser->name);
	default: /* an agent and the record of a person */
		return util_format_view(buf, size, "Your login name %s is the name of a person of "
		                        "this session.", refuser->name);
	}
}

/* Decides what @p login, which no record names, does with @p entry, the
 * record of the name of login->member. @p text is the view of login->text.
 * Returns 0 if the login adopts the record: the agent of a service and the
 * record of an agent. Returns EEXIST if askai adds the login to the
 * record: the first login of its kind and the record of a person. A
 * person can then log in as a Unix user and as an identity of one name.
 * Otherwise returns EPERM: the record refuses the login.
 */
static int
member_adopt (struct member_info const  *entry,
              struct member_login const *login,
              struct cstr_view           text)
{
	size_t const kind = member_login_kind(text);
	struct cstr_view logins = entry->logins;
	size_t n = 0;
	bool same = false;
	for (bool more = logins.len != 0; more; ++n) {
		struct cstr_view item = logins;
		more = cstr_view_cut(&logins, ',', &item);
		same = same || (member_login_kind(item) == kind && !memcmp(item.data, text.data, kind));
	}
	/* The kind of the login, then the kind of the record. */
	switch (login->member.kind << 1 | (enum member_kind)entry->kind) {
	case MEMBER_AGENT << 1 | MEMBER_AGENT:
		return text.len ? EPERM : 0;
	case MEMBER_PERSON << 1 | MEMBER_PERSON:
		return !same && n < MEMBER_LOGINS ? EEXIST : EPERM;
	default: /* a person and the record of an agent, or the reverse */
		return EPERM;
	}
}

/*
 * Decides the member of claim->login with the records @p r of the registry
 * @p reg, as one step of the claim. @p text is the login text of
 * claim->login, which the caller measured. @p *p_entry is the record of
 * the login in @p r, which the caller looked up with
 * member_records_login(), or nullptr. The member is the member of that
 * record, or without it, of the record of the name login->member.name that
 * the login adopts (member_adopt()). The step then stores the record of
 * that name in @p *p_entry, or nullptr if no record of the name exists.
 * Returns 0 with the member in login->member, also with the name "" for a
 * Unix login without a record whose name is not known yet. If no record
 * of the member exists yet, the step writes to @p in the input of git for
 * the commit of the record: the record of the new member login->member, or
 * the login added to the record of the person of that name. It writes the
 * ref of the record to claim->ref, counts the commit in claim->writes, and
 * returns EINPROGRESS. For a null @p in, as for a find, the step returns 0
 * instead, with the member that the commit makes. Otherwise the step
 * returns EPERM if the record of the name, which is then @p *p_entry,
 * refuses the login. The step writes no text for EPERM. It returns EAGAIN
 * after STORE_TRIES commits, or EIO if the input cannot be made, each with
 * the reason in the @p len bytes at @p why. After each of the three
 * errors, @p in stays empty.
 */
static int
member_claim_step (struct member_claim          *claim,
                   struct member_registry const *reg,
                   struct member_records const  *r,
                   struct cstr_view              text,
                   struct member_info const    **p_entry,
                   struct buf                   *in,
                   char                         *why,
                   size_t                        len)
{
	struct member_login *login = &claim->login;
	struct member_info const *entry = *p_entry;
	int e = 0;
	if (!entry && *login->member.name) {
		entry = member_records_find(r, cstr_view(login->member.name));
		if (entry && !(entry->flags & MEMBER_RECORD))
			entry = nullptr;
		e = entry ? member_adopt(entry, login, text) : ENOENT;
		*p_entry = entry;
	}
	switch (e) {
	case 0:
		if (entry)
			login->member = member_of(entry);
		return 0;
	case ENOENT:
	case EEXIST:
		/* No record names the member yet, and a find writes none. */
		if (!in)
			return 0;
		break;
	default:
		return e;
	}
	if (claim->writes == STORE_TRIES) {
		util_format(why, len, "Could not write the member record of %s. Other askai services "
		            "changed the member record at the same time.", login->member.name);
		return EAGAIN;
	}
	e = member_record_input(reg, login, text, entry, claim->ref, in);
	if (!e) {
		++claim->writes;
		return EINPROGRESS;
	}
	buf_fini(in);
	struct child_end end = child_end();
	return member_git_result(why, len, "write", claim->ref,
	                         span(nullptr, 0), e, &end);
}

/*
 * Runs the steps of @p claim with the records of @p reg and with git on the
 * calling thread, as askai starts, before any loop: git commits the record
 * that a step writes, and then reads the records into reg->records again.
 * Returns as member_claim_step() does, but never EINPROGRESS, or EIO or
 * ENOMEM, each with the reason in the @p len bytes at @p why. The claim is
 * the claim of the agent of the service, which only the record of a person
 * refuses. The reason for EPERM contains the logins of that person.
 */
static int
member_registry_claim (struct member_registry *reg,
                       struct member_claim    *claim,
                       char                   *why,
                       size_t                  len)
{
	struct cstr_view const text = cstr_view(claim->login.text);
	for (;;) {
		struct buf in = buf();
		struct member_info const *entry = member_records_login(&reg->records, text);
		int e = member_claim_step(claim, reg, &reg->records, text, &entry, &in, why, len);
		if (e == EPERM) {
			char const *const name = claim->login.member.name;
			util_format(why, len, "agent %s: the name %s belongs to the person with "
			            "login %.*s in this session", name, name,
			            (int)entry->logins.len, entry->logins.data);
		}
		if (e != EINPROGRESS)
			return e;
		e = member_commit(reg, claim->ref, &in, 0, why, len);
		if (!e || e == EAGAIN)
			e = member_records_read(&reg->records, reg->store, why, len);
		if (e)
			return e;
	}
}

/* Writes to @p name the name of the Unix user @p uid. Returns 0, ENOENT if
 * no user with the uid exists, EINVAL if member_name_valid() refuses the
 * name, or the error of getpwuid_r(), such as ENOMEM.
 */
static int
member_user (uint32_t uid,
             char     name[MEMBER_NAME])
{
	long const max = sysconf(_SC_GETPW_R_SIZE_MAX);
	size_t size = max > 0 ? (size_t)max : 1024;
	for (;;) {
		char *storage = malloc(size);
		if (!storage)
			return ENOMEM;
		struct passwd pw;
		struct passwd *found = nullptr;
		int e = getpwuid_r((uid_t)uid, &pw, storage, size, &found);
		if (!e && !found)
			e = ENOENT;
		struct cstr_view const user = found ? cstr_view(pw.pw_name) : (struct cstr_view){};
		if (!e && !member_name_valid(user))
			e = EINVAL;
		if (!e)
			member_name_set(name, user);
		free(storage);
		if (e != ERANGE || size > 1 << 20)
			return e;
		size *= 2;
	}
}

int
member_login (struct member_login          *dest,
              struct member_registry const *registry,
              struct policy_grant const    *grant,
              uint32_t const               *uid)
{
	if (!dest || !registry || !grant)
		return EINVAL;
	*dest = (struct member_login){};
	if (uid) {
		dest->uid = *uid;
		util_format(dest->text, sizeof dest->text, "uid %" PRIu32, *uid);
		return 0;
	}
	if (grant->flags & POLICY_OPERATOR) {
		*dest = registry->own;
		return 0;
	}
	struct cstr_view const identity = CSTR_VIEW("identity ");
	struct cstr_view const name = cstr_view_slice(grant->who, identity.len, SIZE_MAX);
	if (!cstr_view_starts(grant->who, identity))
		return ENOENT;
	if (!member_name_valid(name))
		return EINVAL;
	dest->member = member(grant->flags & POLICY_AGENT ? MEMBER_AGENT : MEMBER_PERSON, name);
	util_format(dest->text, sizeof dest->text, "%.*s", (int)grant->who.len, grant->who.data);
	return 0;
}

int
member_call_ids (struct member_call *call)
{
	if (!call)
		return EINVAL;
	int e = hexid_make(&call->ask);
	if (!e && !*call->conversation.hex)
		e = hexid_make(&call->conversation);
	return e;
}

struct cstr_view
member_text (struct cstr_view name,
             enum member_kind kind,
             bool             you,
             char             text[static MEMBER_TEXT])
{
	return util_format_view(text, MEMBER_TEXT, "%.*s (%s%s)", (int)name.len, name.data,
	                        member_kinds[kind].data, you ? ", you" : "");
}

struct json_object *
member_json (struct cstr_view name,
             enum member_kind kind)
{
	if (!name.data || kind >= MEMBER_KINDS)
		return nullptr;
	return jb_object("name", jb_string(name), "kind", jb_string(member_kinds[kind]), nullptr);
}

int
member_init_json (struct member      *dest,
                  struct json_object *json)
{
	if (!dest)
		return EINVAL;
	*dest = (struct member){};
	struct cstr_view const name = jb_view(jb_get(json, "name"));
	size_t const k = cstr_view_index(member_kinds, MEMBER_KINDS, jb_view(jb_get(json, "kind")));
	if (!member_name_valid(name) || k == MEMBER_KINDS)
		return EINVAL;
	*dest = member((enum member_kind)k, name);
	return 0;
}

/* Writes to @p name the name of the session in the repository at @p path:
 * the last part of the path less .git, or of the directory that contains
 * a .git directory, or "session" if member_name_valid() refuses that.
 * Returns the view of the name in @p name.
 */
static struct cstr_view
member_session_name (char const *path,
                     char        name[MEMBER_NAME])
{
	struct cstr_view const git = CSTR_VIEW(".git");
	struct cstr_view rest = cstr_view(path);
	struct cstr_view base = rest;
	for (bool up = true; up; ) {
		char const *slash = rest.len ? memrchr(rest.data, '/', rest.len) : nullptr;
		base = slash ? cstr_view_slice(rest, (size_t)(slash + 1 - rest.data), SIZE_MAX) : rest;
		up = slash && cstr_view_equal(base, git);
		if (up)
			rest.len = (size_t)(slash - rest.data);
	}
	if (cstr_view_ends(base, git))
		base.len -= git.len;
	if (!member_name_valid(base))
		return util_format_view(name, MEMBER_NAME, "session");
	return util_format_view(name, MEMBER_NAME, "%.*s", (int)base.len, base.data);
}

/* Reads the records of @p reg into reg->records, and creates the session
 * record first if no record of the session exists. The session record is
 * create-only: a commit after the commit of another service writes
 * nothing, and the records that git lists after it contain the record of
 * that service. A commit during the commit of another service can fail on
 * the lock file of the record (EAGAIN) before that record exists, so the
 * function commits and reads again, at most STORE_TRIES times, with the
 * session ID and the name made once. Returns 0, or EINVAL, EIO or ENOMEM
 * with a reason in the @p len bytes at @p why.
 */
static int
member_registry_session_read (struct member_registry *reg,
                              char                   *why,
                              size_t                  len)
{
	struct member_records *r = &reg->records;
	int e = member_records_read(r, reg->store, why, len);
	if (!e && !r->session.data) {
		struct hexid id;
		e = hexid_make(&id);
		if (e) {
			util_format(why, len, "Could not make the session ID: %s.", strerror(e));
			return e;
		}
		char text[MEMBER_NAME];
		struct cstr_view const name = member_session_name(store_directory(reg->store), text);
		struct store_trailer const t[] = {
			{ CSTR_VIEW("Session"), hexid_view(&id) },
			{ CSTR_VIEW("Name"),    name },
			{ CSTR_VIEW("Format"),  CSTR_VIEW("1") },
		};
		char message[STORE_MESSAGE];
		struct cstr_view const subject = util_format_view(message, sizeof message,
		                                                  "Create the session %.*s",
		                                                  (int)name.len, name.data);
		for (size_t tries = 0; !e && !r->session.data && tries < STORE_TRIES; ++tries) {
			struct buf in = buf();
			e = member_commit_input(reg, SESSION, subject, t, array_size(t), &(struct oid){},
			                        &in);
			e = member_commit(reg, SESSION, &in, e, why, len);
			if (!e || e == EAGAIN)
				e = member_records_read(r, reg->store, why, len);
		}
	}
	if (e)
		return e;
	static_assert(MEMBER_FORMAT == 1, "update the Format of the session record");
	uint64_t format = 0;
	if (util_number(r->format.data, r->format.len, UINT32_MAX, &format) ||
	    !hexid_valid(r->session) || !member_name_valid(r->name))
		return util_refuse(why, len, "The session record %s is not valid.", SESSION);
	if (format > MEMBER_FORMAT)
		return util_refuse(why, len, "The session has format %" PRIu64 ". This askai writes "
		                   "format %d.", format, MEMBER_FORMAT);
	return 0;
}

/* Starts @p reg, as member_registry_create() says. Returns 0, or an error
 * number with a reason in the @p len bytes at @p why.
 */
static int
member_registry_start (struct member_registry *reg,
                       char                   *why,
                       size_t                  len)
{
	struct buf out = buf();
	struct child_end end = child_end();
	int e = store_init(reg->store, &out, &end);
	e = member_git_result(why, len, "create", "the repository",
	                      buf_peek(&out), e, &end);
	buf_fini(&out);
	if (!e)
		e = member_registry_session_read(reg, why, len);
	if (e)
		return e;
	struct member_claim agent = { .login.member = reg->session.agent };
	e = member_registry_claim(reg, &agent, why, len);
	if (e)
		return e == EPERM ? EINVAL : e;
	/* The records stay as they are from here on. */
	reg->session.id = reg->records.session;
	reg->session.name = reg->records.name;
	reg->session.legacy = reg->records.legacy;
	/* A table looks up the name of the user, as for a Unix login. */
	uint32_t const uid = (uint32_t)geteuid();
	reg->own = (struct member_login){ .uid = uid, .member.kind = MEMBER_PERSON };
	util_format(reg->own.text, sizeof reg->own.text, "uid %" PRIu32, uid);
	return 0;
}

struct member_registry *
member_registry_create (struct store const *store,
                        char const         *agent,
                        char const         *service,
                        char               *why,
                        size_t              len,
                        int                *e)
{
	if (!why || !len) {
		*e = EINVAL;
		return nullptr;
	}
	struct cstr_view const agent_name = agent ? cstr_view(agent) : (struct cstr_view){};
	struct cstr_view const service_name = service ? cstr_view(service) : (struct cstr_view){};
	if (!store || !member_name_valid(agent_name) || !member_name_valid(service_name)) {
		*e = util_refuse(why, len, "The registry needs a repository, and an agent and a "
		                 "service with valid names.");
		return nullptr;
	}
	struct member_registry *reg = malloc(sizeof *reg);
	if (!reg) {
		util_format(why, len, "Out of memory.");
		*e = ENOMEM;
		return nullptr;
	}
	*reg = (struct member_registry){
		.session = {
			.committer = { service_name, CSTR_VIEW("askai") },
			.agent = member(MEMBER_AGENT, agent_name),
		},
		.store = store,
	};
	for (size_t b = 0; b < MEMBER_BRANCHES; ++b)
		member_ref(&reg->session.agent, (enum member_branch)b, reg->session.refs[b]);
	*e = member_registry_start(reg, why, len);
	if (*e)
		member_registry_destroy(&reg);
	return reg;
}

void
member_registry_destroy (struct member_registry **p_registry)
{
	if (!p_registry || !*p_registry)
		return;
	struct member_registry *reg = *p_registry;
	*p_registry = nullptr;
	member_records_fini(&reg->records);
	free(reg);
}

struct member_session const *
member_registry_session (struct member_registry const *registry)
{
	return registry ? &registry->session : nullptr;
}

struct store const *
member_registry_store (struct member_registry const *registry)
{
	return registry ? registry->store : nullptr;
}

void
member_registry_report (struct member_registry const *registry)
{
	if (!registry)
		return;
	LOG_ALL(LOG_INFO, "session:");
	LOG_ALL(LOG_INFO, "  repository  %s", store_directory(registry->store));
	struct member_records const *r = &registry->records;
	struct member_session const *session = &registry->session;
	LOG_ALL(LOG_INFO, "  id          %.*s", (int)session->id.len, session->id.data);
	LOG_ALL(LOG_INFO, "  name        %.*s", (int)session->name.len, session->name.data);
	LOG_ALL(LOG_INFO, "  agent       %s", session->agent.name);
	LOG_ALL(LOG_INFO, "  service     %.*s", (int)session->committer.name.len,
	        session->committer.name.data);
	LOG_ALL(LOG_INFO, "  evidence    every member can read all stored evidence");
	if (session->legacy)
		LOG_ALL(LOG_INFO, "  legacy      %zu notebook%s under " LEGACY, session->legacy,
		        util_s(session->legacy));
	int const width = (int)r->width;
	LOG_ALL(LOG_INFO, "session members:");
	for (size_t i = 0; i < r->all.n; ++i) {
		struct member_info const *info = &r->all.items[i];
		if (!(info->flags & MEMBER_RECORD))
			continue;
		char logins[MEMBER_LOGINS * (MEMBER_LOGIN + 1)] = "";
		size_t at = 0;
		struct cstr_view rest = info->logins;
		for (bool more = rest.len != 0; more && at < sizeof logins; ) {
			struct cstr_view item = rest;
			more = cstr_view_cut(&rest, ',', &item);
			at += util_format(logins + at, sizeof logins - at, "%s%.*s", at ? ", " : "",
			                  (int)item.len, item.data);
		}
		if (at)
			LOG_ALL(LOG_INFO, "  %-*.*s  %-6s  %s", width, (int)info->name.len, info->name.data,
			        member_kinds[info->kind].data, logins);
		else
			LOG_ALL(LOG_INFO, "  %-*.*s  %s", width, (int)info->name.len, info->name.data,
			        member_kinds[info->kind].data);
	}
}

/* Removes @p lookup from its table, and frees it unless the job name is
 * with a blocker. store_drop() kills the git that reads the records, and
 * lets the git that writes a record run to its end.
 */
static void
member_lookup_drop (struct member_lookup *lookup)
{
	if (lookup->table) {
		(void)loop_timer_stop(lookup->table->loop, &lookup->timer);
		list_del(&lookup->hook);
		lookup->table = nullptr;
	}
	while (!list_is_empty(&lookup->waits))
		list_del(lookup->waits.next);
	store_drop(&lookup->git);
	if (!(lookup->flags & LOOKUP_NAME))
		free(lookup);
}

/* Writes the result @p e of @p lookup to @p wait: the member for 0, and
 * else no member. For EPERM, the function also writes the record that
 * refuses the login. That record is the record of the name of the login,
 * so the function copies the name array of the login, and sets the kind
 * to lookup->refuser_kind. Returns @p e.
 */
static int
member_lookup_give (struct member_lookup const *lookup,
                    struct member_wait         *wait,
                    int                         e)
{
	struct member const *claimed = &lookup->claim.login.member;
	wait->e = e;
	wait->member = e ? (struct member){ .kind = claimed->kind } : *claimed;
	if (e == EPERM) {
		wait->refuser = *claimed;
		wait->refuser.kind = lookup->refuser_kind;
	} else {
		wait->refuser = (struct member){};
	}
	return e;
}

/* Ends @p lookup with the result @p e and writes the result to each wait
 * of the lookup. @p now is the wait of a call that ends the lookup before
 * the wait is on the lookup, or nullptr. The function writes the result to
 * @p now too, and does not call the done of @p now. For 0 and EPERM, the
 * lookup stays on the table with the result. For any other result, the
 * function removes the lookup from the table, unless the job name is with
 * a blocker. The waits of the lookup run after that.
 */
static void
member_lookup_end (struct member_lookup *lookup,
                   int                   e,
                   struct member_wait   *now)
{
	lookup->result = (int16_t)e;
	lookup->flags &= ~LOOKUP_WRITE;
	if (now)
		(void)member_lookup_give(lookup, now, e);
	struct list waits;
	list_init(&waits);
	list_transplant(&waits, &lookup->waits);
	struct list *node;
	list_foreach (node, &waits)
		(void)member_lookup_give(lookup, container_of(node, struct member_wait, hook), e);
	if (e && e != EPERM && !(lookup->flags & LOOKUP_NAME))
		member_lookup_drop(lookup);
	while (!list_is_empty(&waits)) {
		struct member_wait *wait = container_of(waits.next, struct member_wait, hook);
		list_del(&wait->hook);
		wait->done(wait);
	}
}

/* Adds @p wait to @p lookup if @p e is EINPROGRESS. Otherwise the function
 * ends the lookup with the result @p e, and member_lookup_end() writes the
 * result to @p wait. Returns @p e.
 */
static int
member_lookup_join (struct member_lookup *lookup,
                    struct member_wait   *wait,
                    int                   e)
{
	if (e == EINPROGRESS) {
		list_append(&lookup->waits, &wait->hook);
		return wait->e = EINPROGRESS;
	}
	member_lookup_end(lookup, e, wait);
	return e;
}

/* Runs git as lookup->git on the loop of the table of @p lookup for the
 * command @p cmd of the store of the registry, as store_command() writes
 * it for @p words, @p ref and @p in. A step of the loop calls @p done with
 * the child once git ends. Returns EINPROGRESS. If git cannot start, the
 * function returns EBUSY for too many children, or else logs why and
 * returns EIO.
 */
static int
member_lookup_git (struct member_lookup *lookup,
                   enum store_cmd        cmd,
                   char const *const    *words,
                   char const           *ref,
                   struct span           in,
                   void                (*done) (struct child *c))
{
	struct member_table const *table = lookup->table;
	struct git_run run;
	int e = store_command(table->registry->store, &run, cmd, words, ref, in, table->loop->now);
	if (!e)
		lookup->git = child_create(table->loop, table->children, &run.c, done, lookup, &e);
	if (!e)
		return EINPROGRESS;
	if (e == EBUSY)
		return EBUSY;
	LOG(LOG_WARN, "member: %s: cannot run git: %s", lookup->claim.login.text, strerror(e));
	return EIO;
}

/* Submits the job name of @p lookup to a blocker, with the deadline
 * MEMBER_NS. Returns EINPROGRESS, EBUSY if the blockers are busy, or the
 * error of loop_timer_set().
 */
static int
member_lookup_submit (struct member_lookup *lookup)
{
	struct loop *loop = lookup->table->loop;
	int const e = loop_timer_set(loop, &lookup->timer, loop->now + MEMBER_NS);
	if (e)
		return e;
	if (blocker_submit(lookup->table->blockers, &lookup->name)) {
		(void)loop_timer_stop(loop, &lookup->timer);
		return EBUSY;
	}
	lookup->flags |= LOOKUP_NAME;
	return EINPROGRESS;
}

static void member_lookup_wrote (struct child *c);

/* Runs a step of the claim of @p lookup with the records of its table, the
 * login text @p text of the lookup and @p entry, the record of the login
 * in the records or nullptr (member_claim_step()). With LOOKUP_WRITE, the
 * step writes the input of a commit, and git commits the record. For a
 * Unix login without a record, the job name first looks up the name of the
 * user. Returns EINPROGRESS while the job name or git runs. Otherwise
 * returns the result of the step. After EPERM, lookup->refuser_kind is the
 * kind of the record that refuses the login. The function logs the reason
 * of an error of the step: for EPERM the sentence of member_login_why(),
 * and else the text that the step wrote into the scratch of the table.
 */
static int
member_lookup_step (struct member_lookup     *lookup,
                    struct cstr_view          text,
                    struct member_info const *entry)
{
	struct member_table *table = lookup->table;
	struct member_login const *login = &lookup->claim.login;
	struct tool_scratch *scratch = table->scratch;
	struct buf in = buf();
	int e = member_claim_step(&lookup->claim, table->registry, &table->records, text, &entry,
	                          lookup->flags & LOOKUP_WRITE ? &in : nullptr, scratch->text,
	                          sizeof scratch->text);
	switch (e) {
	case 0:
		/* A Unix login that no record names needs the name of its user. */
		return *login->member.name ? 0 : member_lookup_submit(lookup);
	case EINPROGRESS:
		e = member_lookup_git(lookup, STORE_COMMIT, nullptr, lookup->claim.ref,
		                      buf_peek(&in), member_lookup_wrote);
		buf_fini(&in);
		return e;
	case EPERM: {
		struct member const refuser = member_of(entry);
		lookup->refuser_kind = refuser.kind;
		LOG(LOG_WARN, "member: %s: %s", login->text,
		    member_login_why(login, EPERM, &refuser, scratch->text,
		                     sizeof scratch->text).data);
		return EPERM;
	}
	default:
		LOG(LOG_WARN, "member: %s: %s", login->text, scratch->text);
		return e;
	}
}

/* Searches the records of the table of @p lookup for the record of the
 * login, and runs a step of the claim with that record or nullptr
 * (member_lookup_step()). The search and the step use one view of the
 * login text. Returns as member_lookup_step() does.
 */
static int
member_lookup_search (struct member_lookup *lookup)
{
	struct cstr_view const text = cstr_view(lookup->claim.login.text);
	return member_lookup_step(lookup, text, member_records_login(&lookup->table->records, text));
}

/* On the loop: git @p c listed the records for the lookup c->owner. The
 * records replace the records of the table (member_table_parse()), and the
 * lookup runs its step with them, or ends with the error of the read,
 * whose cause the table logs.
 */
static void
member_lookup_read_done (struct child *c)
{
	struct member_lookup *lookup = c->owner;
	int e = member_table_parse(lookup->table, c);
	child_destroy(&lookup->git);
	if (!e)
		e = member_lookup_search(lookup);
	if (e != EINPROGRESS)
		member_lookup_end(lookup, e, nullptr);
}

/* Runs git to read the records for @p lookup. Returns as
 * member_lookup_git() does.
 */
static int
member_lookup_read (struct member_lookup *lookup)
{
	return member_lookup_git(lookup, STORE_REFS, member_list, nullptr, span(nullptr, 0),
	                         member_lookup_read_done);
}

/* On the loop: git committed the record for the lookup of @p c, or another
 * writer changed the record first. The records of the table are older
 * than the commit, so git reads the records again. If git failed in
 * another way, the function logs the reason, and the lookup ends with EIO.
 */
static void
member_lookup_wrote (struct child *c)
{
	struct member_lookup *lookup = c->owner;
	struct tool_scratch *scratch = lookup->table->scratch;
	int e = member_commit_result(scratch->text, sizeof scratch->text, lookup->claim.ref,
	                             buf_peek(&c->out), 0, &c->end);
	if (e == EIO)
		LOG(LOG_WARN, "member: %s: %s", lookup->claim.login.text, scratch->text);
	child_destroy(&lookup->git);
	if (!e || e == EAGAIN)
		e = member_lookup_read(lookup);
	if (e != EINPROGRESS)
		member_lookup_end(lookup, e, nullptr);
}

/* On a blocker: looks up the name of the Unix user of the lookup of
 * @p job.
 */
static void
member_lookup_name (struct blocker_job *job)
{
	struct member_lookup *lookup = container_of(job, struct member_lookup, name);
	job->error = member_user(lookup->claim.login.uid, lookup->user);
}

/* Writes to lookup->claim.login the name of the Unix user of @p lookup:
 * the name that the job name found, or uid-N for the error @p e of the
 * job, which the log names. For ENOENT, no user with the uid, and for
 * EINVAL, a name that member_name_valid() refuses, uid-N is the name of
 * the person. For any other error, such as ETIMEDOUT after MEMBER_NS,
 * uid-N is a guess (LOOKUP_GUESS), which a find answers and a claim does
 * not write. Returns ENODATA for a guess that a wait needs to write, else
 * 0.
 */
static int
member_lookup_named (struct member_lookup *lookup,
                     int                   e)
{
	struct member_login *login = &lookup->claim.login;
	lookup->flags &= ~LOOKUP_GUESS;
	if (!e) {
		memcpy(login->member.name, lookup->user, sizeof login->member.name);
		return 0;
	}
	util_format(login->member.name, sizeof login->member.name, "uid-%" PRIu32, login->uid);
	switch (e) {
	case ENOENT:
		LOG(LOG_WARN, "member: %s: no user with the uid exists, so the person is %s",
		    login->text, login->member.name);
		return 0;
	case EINVAL:
		LOG(LOG_WARN, "member: %s: the name of the user is not a valid member name, so the "
		    "person is %s", login->text, login->member.name);
		return 0;
	case ETIMEDOUT:
		LOG(LOG_WARN, "member: %s: the lookup of the name took longer than %" PRIu64 " s",
		    login->text, MEMBER_NS / NS);
		break;
	default:
		LOG(LOG_WARN, "member: %s: the lookup of the name failed: %s", login->text,
		    strerror(e));
		break;
	}
	lookup->flags |= LOOKUP_GUESS;
	return lookup->flags & LOOKUP_WRITE ? ENODATA : 0;
}

/* On the loop: the job name of a lookup came back, also after MEMBER_NS.
 * If askai cancelled the job while the lookup waited for the job, the
 * lookup ends with ECANCELED.
 */
static void
member_lookup_name_done (struct loop *,
                         struct msg  *msg)
{
	struct member_lookup *lookup = container_of(msg, struct member_lookup, name.done);
	lookup->flags &= ~LOOKUP_NAME;
	if (!lookup->table) {
		member_lookup_drop(lookup);
		return;
	}
	(void)loop_timer_stop(lookup->table->loop, &lookup->timer);
	int e = (int)lookup->name.error;
	if (e == ECANCELED && lookup->result == EINPROGRESS) {
		member_lookup_end(lookup, e, nullptr);
		return;
	}
	e = member_lookup_named(lookup, e);
	if (!e && !(lookup->flags & LOOKUP_GUESS))
		e = member_lookup_search(lookup);
	if (e != EINPROGRESS)
		member_lookup_end(lookup, e, nullptr);
}

/* On the loop: the job name of a lookup took longer than MEMBER_NS. The
 * lookup ends, and the job stays with the blocker.
 */
static void
member_lookup_late (struct loop       *,
                    struct loop_timer *timer)
{
	struct member_lookup *lookup = container_of(timer, struct member_lookup, timer);
	member_lookup_end(lookup, member_lookup_named(lookup, ETIMEDOUT), nullptr);
}

/* Creates the lookup of @p login on @p table, with the flags @p flags.
 * Returns the lookup, or returns nullptr with ENOMEM in @p *e.
 */
static struct member_lookup *
member_lookup_create (struct member_table       *table,
                      struct member_login const *login,
                      uint32_t                   flags,
                      int                       *e)
{
	struct member_lookup *lookup = malloc(sizeof *lookup);
	if (!lookup) {
		*e = ENOMEM;
		return nullptr;
	}
	*lookup = (struct member_lookup){
		.name = {
			.done.fn = member_lookup_name_done,
			.work = member_lookup_name,
			.loop = table->loop
		},
		.timer.fn = member_lookup_late,
		.table = table,
		.claim.login = *login,
		.flags = flags,
		.result = EINPROGRESS,
	};
	list_init(&lookup->waits);
	list_append(&table->lookups, &lookup->hook);
	*e = 0;
	return lookup;
}

struct member_table *
member_table_create (struct member_registry const *registry,
                     struct loop                  *loop,
                     struct blocker               *blockers,
                     struct list                  *children,
                     struct tool_scratch          *scratch,
                     int                          *e)
{
	if (!registry || !loop || !blockers || !children || !scratch) {
		*e = EINVAL;
		return nullptr;
	}
	struct member_table *table = malloc(sizeof *table);
	if (!table) {
		*e = ENOMEM;
		return nullptr;
	}
	*table = (struct member_table){
		.registry = registry,
		.loop = loop,
		.blockers = blockers,
		.children = children,
		.scratch = scratch,
	};
	list_init(&table->lookups);
	/* The views of the entries point into the output of the registry,
	 * which outlives the table. */
	struct member_records const *from = &registry->records;
	*e = util_grow(table->records.all.items, table->records.all.cap, from->all.n);
	if (*e) {
		member_table_destroy(&table);
		return nullptr;
	}
	if (from->all.n)
		memcpy(table->records.all.items, from->all.items,
		       from->all.n * sizeof *from->all.items);
	table->records.all.n = from->all.n;
	table->records.width = from->width;
	return table;
}

void
member_table_destroy (struct member_table **p_table)
{
	if (!p_table || !*p_table)
		return;
	struct member_table *table = *p_table;
	*p_table = nullptr;
	while (!list_is_empty(&table->lookups))
		member_lookup_drop(container_of(table->lookups.next, struct member_lookup, hook));
	member_records_fini(&table->records);
	free(table);
}

struct member_registry const *
member_table_registry (struct member_table const *table)
{
	return table ? table->registry : nullptr;
}

/* Finds the member of @p login for @p wait, as member_table_find() and
 * member_table_claim() say. @p write is LOOKUP_WRITE if the wait needs
 * the record of the member, else 0.
 */
static int
member_table_lookup (struct member_table       *table,
                     struct member_login const *login,
                     uint32_t                   write,
                     struct member_wait        *wait)
{
	if (!table || !login || !wait)
		return EINVAL;
	list_init(&wait->hook);
	wait->member = (struct member){};
	wait->refuser = (struct member){};
	struct cstr_view const text = cstr_view(login->text);
	if (!text.len)
		return wait->e = ENOENT;
	struct member_info const *entry = member_records_login(&table->records, text);
	if (entry) {
		wait->member = member_of(entry);
		return wait->e = 0;
	}
	struct member_lookup *lookup = nullptr;
	struct list *node;
	list_foreach (node, &table->lookups) {
		struct member_lookup *l = container_of(node, struct member_lookup, hook);
		if (!strcmp(l->claim.login.text, login->text)) {
			lookup = l;
			break;
		}
	}
	if (!lookup) {
		int e = 0;
		lookup = member_lookup_create(table, login, write, &e);
		if (!lookup)
			return wait->e = e;
		return member_lookup_join(lookup, wait, member_lookup_read(lookup));
	}
	switch (lookup->result) {
	case EINPROGRESS:
		/* A lookup that a wait joins writes the record if any wait needs
		 * it. */
		lookup->flags |= write;
		return member_lookup_join(lookup, wait, EINPROGRESS);
	case EPERM:
		return member_lookup_give(lookup, wait, EPERM);
	}
	/* After any other result of a lookup that ended, a find returns the
	 * member, also a member without a record or a guess. A claim runs the
	 * steps again, and git writes the record. While the job name of a
	 * guess is with a blocker, a claim fails, so that no second job of the
	 * uid goes to the blockers. */
	if (!write)
		return member_lookup_give(lookup, wait, 0);
	switch (lookup->flags & (LOOKUP_NAME | LOOKUP_GUESS)) {
	case 0:
		break;
	case LOOKUP_GUESS:
		lookup->claim.login.member.name[0] = '\0';
		break;
	default:
		return member_lookup_give(lookup, wait, ENODATA);
	}
	lookup->flags = (lookup->flags & ~LOOKUP_GUESS) | LOOKUP_WRITE;
	lookup->result = EINPROGRESS;
	lookup->claim.writes = 0;
	/* The search above found no record of the login in the records. */
	return member_lookup_join(lookup, wait, member_lookup_step(lookup, text, nullptr));
}

int
member_table_find (struct member_table       *table,
                   struct member_login const *login,
                   struct member_wait        *wait)
{
	return member_table_lookup(table, login, 0, wait);
}

int
member_table_claim (struct member_table       *table,
                    struct member_login const *login,
                    struct member_wait        *wait)
{
	return member_table_lookup(table, login, LOOKUP_WRITE, wait);
}

void
member_table_cancel (struct member_wait *wait)
{
	if (wait)
		list_del(&wait->hook);
}

int
member_table_parse (struct member_table *table,
                    struct child        *git)
{
	if (!table || !git)
		return EINVAL;
	struct tool_scratch *scratch = table->scratch;
	int const e = member_records_take(&table->records, &git->out, 0, &git->end, scratch->text,
	                                  sizeof scratch->text);
	if (e)
		LOG(LOG_WARN, "member: %s", scratch->text);
	return e;
}

struct member_info const *
member_table_members (struct member_table const *table,
                      size_t                    *n,
                      size_t                    *width)
{
	if (!table) {
		*n = 0;
		if (width)
			*width = 0;
		return nullptr;
	}
	struct member_records const *r = &table->records;
	*n = r->all.n;
	if (width)
		*width = r->width;
	return r->all.items;
}

int
member_table_name (struct member_table const *table,
                   struct cstr_view           name,
                   struct member             *dest)
{
	if (!table || !dest)
		return EINVAL;
	struct member_info const *entry = member_records_find(&table->records, name);
	if (!entry)
		return ENOENT;
	*dest = member_of(entry);
	return 0;
}

/* Appends to @p d the first MEMBER_SHOWN members of @p table, each with
 * its kind, and with ", you" for @p you if @p you is not nullptr, such as
 * "imaami (person), bro (agent, you) and cuh (agent)", and " and N more"
 * for the members after them. Returns 0 or ENOMEM.
 */
static int
member_table_names (struct member_table const *table,
                    struct member const       *you,
                    struct dstr               *d)
{
	struct cstr_view views[MEMBER_SHOWN];
	char items[array_size(views)][MEMBER_TEXT];
	size_t const n = table->records.all.n;
	size_t const shown = n < array_size(views) ? n : array_size(views);
	struct cstr_view const me = you ? cstr_view(you->name) : (struct cstr_view){};
	for (size_t i = 0; i < shown; ++i) {
		struct member_info const *info = &table->records.all.items[i];
		bool const self = you && info->kind == you->kind && cstr_view_equal(info->name, me);
		views[i] = member_text(info->name, (enum member_kind)info->kind, self, items[i]);
	}
	return cstr_view_join_more(views, shown, n, d);
}

int
member_table_notice (struct member_table const *table,
                     struct member const       *you,
                     struct dstr               *d)
{
	if (!table || !you || !d)
		return EINVAL;
	bool const ok = dstr_cat(d, "[askai: Members of this session: ") &&
	                !member_table_names(table, you, d) &&
	                dstr_cat(d, ". Call notes with action members to see what each member "
	                            "wrote last.]\n");
	return ok ? 0 : ENOMEM;
}

int
member_table_unknown (struct member_table const *table,
                      struct cstr_view           name,
                      struct dstr               *d)
{
	if (!table || !d)
		return EINVAL;
	bool const ok = dstr_printf(d, "No member is named %.*s. %s ",
	                            (int)util_clip(name.data, name.len, 2 * UTIL_WORD_MAX),
	                            name.data, table->records.all.n == 1 ? "The only member is"
	                                                                 : "The members are") &&
	                !member_table_names(table, nullptr, d) && dstr_cat(d, ".");
	return ok ? 0 : ENOMEM;
}

void
member_byline_put (struct member_byline        *byline,
                   struct store_commit         *commit,
                   struct member const         *self,
                   struct member_call const    *call,
                   uint32_t                     k,
                   struct member_session const *session)
{
	struct cstr_view const email = util_format_view(byline->email, sizeof byline->email, "%s/%s",
	                                                member_kinds[self->kind].data, self->name);
	size_t n = 0;
	struct store_trailer *t = byline->trailers;
	if (*call->ask.hex)
		t[n++] = (struct store_trailer){ CSTR_VIEW("Ask"), hexid_view(&call->ask) };
	if (k)
		t[n++] = (struct store_trailer){ CSTR_VIEW("Call"),
		                                 util_format_view(byline->call, sizeof byline->call,
		                                                  "%" PRIu32, k) };
	if (*call->conversation.hex)
		t[n++] = (struct store_trailer){ CSTR_VIEW("Conversation"),
		                                 hexid_view(&call->conversation) };
	if (*call->asker.name)
		t[n++] = (struct store_trailer){ CSTR_VIEW("Asked-by"), cstr_view(call->asker.name) };
	commit->author = (struct store_ident){ cstr_view(self->name), email };
	commit->committer = session->committer;
	commit->trailers = t;
	commit->n_trailers = n;
}

int
member_notice (struct member const      *you,
               struct member_call const *call,
               struct dstr              *d)
{
	if (!you || !call || !d)
		return EINVAL;
	struct member const *asker = &call->asker;
	bool ok = dstr_printf(d, "[askai: You are the %s %s. ", member_kinds[you->kind].data,
	                      you->name);
	if (!*asker->name)
		ok = ok && dstr_cat(d, "A client without a member name made this ask call.]\n");
	else if (asker->kind == MEMBER_AGENT)
		ok = ok && dstr_printf(d, "The agent %s made this ask call.]\n", asker->name);
	else
		ok = ok && dstr_printf(d, "A client program of the person %s made this ask "
		                       "call.]\n", asker->name);
	return ok ? 0 : ENOMEM;
}
