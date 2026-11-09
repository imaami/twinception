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
#include "hexid.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "member.h"
#include "oid.h"
#include "policy.h"
#include "store.h"
#include "stream.h"
#include "util.h"

/* The refs of the records and of the notebooks. */
#define RECORDS "refs/askai/"
#define MEMBERS RECORDS "members/"
#define SESSION RECORDS "session"
#define HEADS   "refs/heads/"
#define LEGACY  HEADS "notes/"

enum {
	MEMBER_REF   = sizeof MEMBERS + UTIL_WORD_MAX,  /* bytes of the ref of a
	                                                   record, NUL included */
	MEMBER_TRIES = 4,                               /* writes of a record in
	                                                   a claim, at most */
	MEMBER_SHOWN = 32,                              /* members that a notice
	                                                   names, at most */
};

/* git writes a line for each ref: the ref, the commit, the values of the
 * trailer lines Kind, Login, Session, Name and Format, the commit time, the
 * value of the trailer line Asked-by and the subject, each field followed
 * by a NUL. The values of several lines of one key are one field,
 * separated by commas.
 */
static_assert(array_size(member_kinds) == 2, "list the notebooks of the new kind in member_list");
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
	HEADS "person/",
	HEADS "agent/",
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

/* A member in the output of member_list: a member record, a notebook, or
 * both. The views borrow the output.
 */
struct member_entry {
	struct member_info info;
	struct cstr_view   logins;  /* the Login lines, separated by commas */
	struct cstr_view   tip;     /* the commit of the record */
};

/* The records of the session, as git lists them with member_list. */
struct member_records {
	struct buf                      out;      /* the output of git */
	UTIL_ARRAY(struct member_entry) all;      /* the member records, by
	                                             name */
	struct cstr_view                session;  /* the session ID of the
	                                             session record, with data
	                                             nullptr without the
	                                             record */
	struct cstr_view                name;     /* its name */
	struct cstr_view                format;   /* its format */
};

struct member_registry {
	struct member_records  records;  /* the records that askai read as it
	                                    started, with the session ID and
	                                    name */
	struct member_session  session;  /* views of records, and the names of
	                                    this askai */
	struct store const    *store;
	size_t                 legacy;   /* notebooks of the old layout under
	                                    LEGACY */
	struct member_login    own;      /* the user who runs askai */
};

struct member_table {
	struct member_records         records;   /* the newest records that a
	                                            lookup read */
	struct list                   lookups;   /* struct member_lookup, under
	                                            way or refused */
	struct member_registry const *registry;
	struct loop                  *loop;
	struct blocker               *blockers;
	struct list                  *children;  /* the children of the loop,
	                                            which run git */
};

/* The flags of a lookup. */
enum {
	LOOKUP_NAME    = 1u << 0,  /* the job name is with a blocker */
	LOOKUP_WRITE   = 1u << 1,  /* a wait needs the record of the member, so
	                              git writes a missing record */
	LOOKUP_DONE    = 1u << 2,  /* the lookup ended, and the later lookups of
	                              the login get its result */
	LOOKUP_GUESS   = 1u << 3,  /* the name is uid-N, because the lookup of
	                              the name of the user failed in a way that
	                              a retry can fix */
	LOOKUP_REFUSED = 1u << 4,  /* the lookup ended with EPERM, the reason in
	                              why */
};

/* The lookup of a login whose member the records of a table do not name,
 * on the loop of the table. git reads the records, and the records decide
 * the member (member_lookup_decide()). If a wait needs a record that does
 * not exist, git writes the record and reads the records again. For a
 * Unix login that no record names, the job name first looks up the name
 * of the user on a blocker. A lookup that ends with the member, also a
 * member without a record, or with a refusal stays on the table, and the
 * later lookups of the login get that result without git. Any other end
 * removes the lookup from the table, unless the job name is with a
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
	struct child          *git;                /* the git that reads or
	                                              writes the records, or
	                                              nullptr */
	uint32_t               flags;
	struct member_login    login;              /* the login, and once the
	                                              records decide, the kind
	                                              and the name of the
	                                              member */
	char                   user[MEMBER_NAME];  /* the name of the Unix
	                                              user, as the job name
	                                              finds it */
	uint8_t                writes;             /* the records that git
	                                              wrote */
	char                   why[MEMBER_WHY];
};

/* Frees the output and the records of @p r. */
static void
member_records_fini (struct member_records *r)
{
	buf_fini(&r->out);
	free(r->all.items);
	*r = (struct member_records){};
}

/* Returns the member named @p name in @p r, or nullptr. */
static struct member_entry *
member_records_find (struct member_records const *r,
                     struct cstr_view             name)
{
	for (size_t i = 0; i < r->all.n; ++i)
		if (cstr_view_equal(r->all.items[i].info.name, name))
			return &r->all.items[i];
	return nullptr;
}

/* Adds the notebook of the line @p f of the output of member_list to
 * @p r, if the line is the ref of a notebook: to the member of a record of
 * the name, or as a member without a record. A notebook of a name whose
 * record is of another kind is left out. Returns 0 or ENOMEM.
 */
static int
member_records_notebook (struct member_records  *r,
                         struct cstr_view const  f[FIELDS])
{
	struct cstr_view rest = cstr_view_slice(f[FIELD_REF], sizeof HEADS - 1, SIZE_MAX);
	struct cstr_view kind_name, name;
	uint64_t time = 0;
	if (!cstr_view_cut(&rest, '/', &kind_name) || !cstr_view_cut(&rest, '/', &name) ||
	    !cstr_view_equal(rest, CSTR_VIEW("notes")) || !member_name_valid(name) ||
	    util_number(f[FIELD_TIME].data, f[FIELD_TIME].len, UINT64_MAX, &time))
		return 0;
	size_t const kind = cstr_view_index(member_kinds, array_size(member_kinds), kind_name);
	struct member_entry *entry = member_records_find(r, name);
	if (kind == array_size(member_kinds) || (entry && entry->info.kind != kind))
		return 0;
	if (!entry) {
		if (util_grow(r->all.items, r->all.cap, r->all.n + 1))
			return ENOMEM;
		entry = &r->all.items[r->all.n++];
		*entry = (struct member_entry){ .info = { .name = name, .kind = (uint32_t)kind } };
	}
	entry->info.change = f[FIELD_SUBJECT];
	entry->info.asker = f[FIELD_ASKER];
	entry->info.time = time;
	entry->info.flags |= MEMBER_NOTES;
	return 0;
}

/* Returns the length of the longest name of the members of @p r. */
static int
member_records_width (struct member_records const *r)
{
	size_t width = 0;
	for (size_t i = 0; i < r->all.n; ++i)
		if (r->all.items[i].info.name.len > width)
			width = r->all.items[i].info.name.len;
	return (int)width;
}

/* Orders the persons before the agents, each by name. */
static int
member_entry_order (void const *a,
                    void const *b)
{
	struct member_info const *x = &((struct member_entry const *)a)->info;
	struct member_info const *y = &((struct member_entry const *)b)->info;
	return x->kind != y->kind ? (x->kind > y->kind) - (x->kind < y->kind)
	                          : cstr_view_compare(x->name, y->name);
}

/* Parses r->out into the records of @p r. A member record whose ref,
 * commit, kind or name is not valid is left out. git lists the records
 * before the notebooks, so a notebook joins the record of its member.
 * Returns 0, EIO for output that is not in the form of member_list, or
 * ENOMEM.
 */
static int
member_records_parse (struct member_records *r)
{
	r->all.n = 0;
	r->session = r->name = r->format = (struct cstr_view){};
	struct span const out = buf_peek(&r->out);
	struct cstr_view rest = { (char const *)out.ptr, out.len };
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
			if (member_records_notebook(r, f))
				return ENOMEM;
			continue;
		}
		struct cstr_view const name = cstr_view_slice(f[FIELD_REF], sizeof MEMBERS - 1,
		                                              SIZE_MAX);
		size_t const kind = cstr_view_index(member_kinds, array_size(member_kinds),
		                                    f[FIELD_KIND]);
		if (!cstr_view_starts(f[FIELD_REF], CSTR_VIEW(MEMBERS)) || !member_name_valid(name) ||
		    kind == array_size(member_kinds) || !oid_valid(f[FIELD_TIP]))
			continue;
		if (util_grow(r->all.items, r->all.cap, r->all.n + 1))
			return ENOMEM;
		r->all.items[r->all.n++] = (struct member_entry){
			.info = { .name = name, .kind = (uint32_t)kind, .flags = MEMBER_RECORD },
			.logins = f[FIELD_LOGIN],
			.tip = f[FIELD_TIP],
		};
	}
	if (r->all.n)
		qsort(r->all.items, r->all.n, sizeof *r->all.items, member_entry_order);
	return 0;
}

/* Writes to the @p size bytes at @p buf why git failed, after store_run()
 * returned the error @p e and the exit status @p status, as
 * child_end_why() says it for a git on a loop. Returns a view of the
 * text, or an empty view if git exited with status 0.
 */
static struct cstr_view
member_run_why (int     e,
                int     status,
                char   *buf,
                size_t  size)
{
	switch (e) {
	case 0:
		return status ? util_format_view(buf, size, "git exited with status %d.", status)
		              : (struct cstr_view){};
	case ETIMEDOUT:
		return util_format_view(buf, size, "git took longer than %" PRIu64 " s, and was "
		                        "stopped.", GIT_RUN_NS / NS);
	case ENOBUFS:
		return util_format_view(buf, size, "git wrote more than %d MiB.", STORE_MAX >> 20);
	default:
		return util_format_view(buf, size, "%s.", strerror(e));
	}
}

/* Writes to the @p size bytes at @p buf why git @p c, which ran on a loop,
 * failed, as child_end_why() says it. Returns a view of the text, or an
 * empty view if git exited with status 0.
 */
static struct cstr_view
member_child_why (struct child const *c,
                  char               *buf,
                  size_t              size)
{
	if ((c->end.flags & (CHILD_CUT | CHILD_LATE)) == CHILD_CUT)
		return member_run_why(ENOBUFS, 0, buf, size);
	return child_end_why(&c->end, "git", GIT_RUN_NS, c->max, buf, size);
}

/* Writes to the @p len bytes at @p why that askai could not do @p what:
 * with the last line of @p out, the errors of a git that writes, or else
 * with @p because, why git failed. Returns EIO.
 */
static int
member_failed (char             *why,
               size_t            len,
               char const       *what,
               struct span       out,
               struct cstr_view  because)
{
	struct cstr_view text = cstr_view_last_line((struct cstr_view){ (char const *)out.ptr,
	                                                                 out.len });
	if (!text.len)
		text = because;
	util_format(why, len, "Could not %s: %.*s", what, (int)util_clip(text.data, text.len, 200),
	            text.data);
	return EIO;
}

/* Writes to the @p len bytes at @p why why askai could not read the member
 * records, for the error @p e of member_records_parse(). Returns @p e.
 */
static int
member_records_invalid (int     e,
                        char   *why,
                        size_t  len)
{
	if (e)
		util_format(why, len, "Could not read the member records: %s.",
		            e == EIO ? "the output of git is not valid" : strerror(e));
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
	buf_clear(&r->out);
	int status = -1;
	int const e = store_run(store, STORE_REFS, member_list, nullptr, (struct span){}, &r->out,
	                        &status);
	char text[MEMBER_WHY];
	struct cstr_view const because = member_run_why(e, status, text, sizeof text);
	if (because.len)
		return member_failed(why, len, "read the member records", (struct span){}, because);
	return member_records_invalid(member_records_parse(r), why, len);
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
static struct member_entry const *
member_records_login (struct member_records const *r,
                      struct cstr_view             login)
{
	for (size_t i = 0; login.len && i < r->all.n; ++i)
		if (member_logins_have(r->all.items[i].logins, login))
			return &r->all.items[i];
	return nullptr;
}

/* Returns the record of @p r of the member @p name, or nullptr. */
static struct member_entry const *
member_records_name (struct member_records const *r,
                     struct cstr_view             name)
{
	struct member_entry const *entry = member_records_find(r, name);
	return entry && entry->info.flags & MEMBER_RECORD ? entry : nullptr;
}

/* Writes the member of @p info to @p member. */
static void
member_of (struct member            *member,
           struct member_info const *info)
{
	member->kind = (enum member_kind)info->kind;
	util_format(member->name, sizeof member->name, "%.*s", (int)info->name.len,
	            info->name.data);
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
		.committer = { cstr_view(reg->session.service), CSTR_VIEW("askai") },
		.message = message,
		.files = {},
		.trailers = t,
		.n_trailers = n,
		.ref = ref,
		.tip = tip,
	};
	return store_commit_input(&commit, in);
}

/* Returns the result of a git that wrote a record on the ref @p ref, with
 * the output @p out: 0 for an empty @p because, which is why git failed,
 * EAGAIN if @p out shows that another writer changed the ref first, or
 * else EIO with the reason in the @p len bytes at @p why.
 */
static int
member_wrote (char const       *ref,
              struct span       out,
              struct cstr_view  because,
              char             *why,
              size_t            len)
{
	if (!because.len)
		return 0;
	if (store_raced(out))
		return EAGAIN;
	char what[sizeof "write " + MEMBER_REF];
	util_format(what, sizeof what, "write %s", ref);
	return member_failed(why, len, what, out, because);
}

/* Runs git on the calling thread to commit @p in, the input of a commit
 * of a record of @p reg on the ref @p ref, and frees @p in. @p e is the
 * error of making @p in. Returns as member_wrote() does.
 */
static int
member_commit (struct member_registry const *reg,
               char const                   *ref,
               struct buf                   *in,
               int                           e,
               char                         *why,
               size_t                        len)
{
	struct buf out = {};
	int status = -1;
	if (!e)
		e = store_run(reg->store, STORE_COMMIT, nullptr, ref, buf_peek(in), &out, &status);
	buf_fini(in);
	char text[MEMBER_WHY];
	e = member_wrote(ref, buf_peek(&out), member_run_why(e, status, text, sizeof text), why,
	                 len);
	buf_fini(&out);
	return e;
}

/* Writes to @p in the input of git for the record of @p login, the record
 * of a new member named login->name, and to @p ref the ref of the record.
 * With @p entry, the record of that name, the commit adds the login to
 * that record instead. Returns as member_commit_input() does.
 */
static int
member_record_input (struct member_registry const *reg,
                     struct member_login const    *login,
                     struct member_entry const    *entry,
                     char                          ref[MEMBER_REF],
                     struct buf                   *in)
{
	struct cstr_view const key = CSTR_VIEW("Login");
	struct store_trailer t[2 + MEMBER_LOGINS] = {
		{ CSTR_VIEW("Kind"), member_kinds[login->kind] }
	};
	size_t n = 1;
	struct oid tip = {};
	char message[STORE_MESSAGE];
	if (entry) {
		(void)oid_init(&tip, entry->tip);
		struct cstr_view logins = entry->logins;
		for (bool more = logins.len != 0; more && n <= MEMBER_LOGINS; ) {
			struct cstr_view item = logins;
			more = cstr_view_cut(&logins, ',', &item);
			t[n++] = (struct store_trailer){ key, item };
		}
		util_format(message, sizeof message, "Add login %s to the %s %s", login->text,
		            member_kinds[login->kind].data, login->name);
	} else {
		util_format(message, sizeof message, "Add the %s %s", member_kinds[login->kind].data,
		            login->name);
	}
	if (*login->text)
		t[n++] = (struct store_trailer){ key, cstr_view(login->text) };
	util_format(ref, MEMBER_REF, MEMBERS "%s", login->name);
	return member_commit_input(reg, ref, cstr_view(message), t, n, &tip, in);
}

/* Writes to the @p len bytes at @p why that other writers changed the
 * record of @p login first on every try. Returns EIO.
 */
static int
member_raced (struct member_login const *login,
              char                      *why,
              size_t                     len)
{
	util_format(why, len, "Could not write the member record of %s. Other askai services "
	            "changed the member record at the same time.", login->name);
	return EIO;
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

/* Decides what @p login, which no record names, does with @p entry, the
 * record of the name login->name. Returns 0 if the login adopts the
 * record: the agent of a service and the record of an agent. Returns
 * EEXIST if askai adds the login to the record: the first login of its
 * kind and the record of a person. A person can then log in as a Unix user
 * and as an identity of one name. Otherwise returns EPERM with the reason
 * in the @p len bytes at @p why.
 */
static int
member_adopt (struct member_entry const *entry,
              struct member_login const *login,
              char                      *why,
              size_t                     len)
{
	struct cstr_view const text = cstr_view(login->text);
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
	switch (login->kind << 1 | (enum member_kind)entry->info.kind) {
	case MEMBER_AGENT << 1 | MEMBER_AGENT:
		if (!text.len)
			return 0;
		util_format(why, len, "Your login name %s is the name of another agent of this "
		            "session.", login->name);
		return EPERM;
	case MEMBER_PERSON << 1 | MEMBER_PERSON:
		if (!same && n < MEMBER_LOGINS)
			return EEXIST;
		util_format(why, len, "Your login name %s is the name of another person of this "
		            "session.", login->name);
		return EPERM;
	case MEMBER_PERSON << 1 | MEMBER_AGENT:
		util_format(why, len, "Your login name %s is the name of an agent of this session. "
		            "Ask the operator to rename the agent.", login->name);
		return EPERM;
	default: /* an agent and the record of a person */
		if (text.len)
			util_format(why, len, "Your login name %s is the name of a person of this "
			            "session.", login->name);
		else
			util_format(why, len, "agent %s: the name %s belongs to the person with login "
			            "%.*s in this session", login->name, login->name,
			            (int)entry->logins.len, entry->logins.data);
		return EPERM;
	}
}

/*
 * Decides the member of @p login with the records @p r: the member of the
 * record of the login, or of the record of the name login->name that the
 * login adopts (member_adopt()). The function writes the member to
 * @p member, empty if no record names it, and the record of the name to
 * @p *p_entry. Returns 0, also if no record names the login and
 * login->name is empty; ENOENT if no record of the name exists, so that
 * the login makes a new member; EEXIST if askai adds the login to the
 * record of the name; or EPERM with the reason in the @p len bytes at
 * @p why.
 */
static int
member_decide (struct member_records const  *r,
               struct member_login const    *login,
               struct member                *member,
               struct member_entry const   **p_entry,
               char                         *why,
               size_t                        len)
{
	*member = (struct member){};
	struct member_entry const *entry = member_records_login(r, cstr_view(login->text));
	int e = 0;
	if (!entry && *login->name) {
		entry = member_records_name(r, cstr_view(login->name));
		e = entry ? member_adopt(entry, login, why, len) : ENOENT;
	}
	if (!e && entry)
		member_of(member, &entry->info);
	*p_entry = entry;
	return e;
}

/*
 * Finds the member of @p login in the records of the registry @p reg, with
 * git on the calling thread. If no record names the member and
 * login->name is not empty, the function makes the member: it creates the
 * record of the name, adopts it, or adds the login to it. After each
 * write, and after a race with another writer, the function reads the
 * records again. The records that the function reads last are in @p r.
 * Writes the member to @p member, which is empty if no record names the
 * member and login->name is empty. Returns 0, EPERM if the record of the
 * name refuses the login, or EIO or ENOMEM, with the reason in the @p len
 * bytes at @p why.
 */
static int
member_claim (struct member_registry const *reg,
              struct member_login const    *login,
              struct member_records        *r,
              struct member                *member,
              char                         *why,
              size_t                        len)
{
	*member = (struct member){};
	for (size_t writes = 0; ; ++writes) {
		struct member_entry const *entry = nullptr;
		int e = member_records_read(r, reg->store, why, len);
		if (!e)
			e = member_decide(r, login, member, &entry, why, len);
		if (e != ENOENT && e != EEXIST)
			return e;
		if (writes == MEMBER_TRIES)
			return member_raced(login, why, len);
		char ref[MEMBER_REF];
		struct buf in = {};
		e = member_record_input(reg, login, entry, ref, &in);
		e = member_commit(reg, ref, &in, e, why, len);
		if (e && e != EAGAIN)
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
		if (!e && !member_name_valid(cstr_view(pw.pw_name)))
			e = EINVAL;
		if (!e)
			util_format(name, MEMBER_NAME, "%s", pw.pw_name);
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
	dest->kind = grant->flags & POLICY_AGENT ? MEMBER_AGENT : MEMBER_PERSON;
	util_format(dest->text, sizeof dest->text, "%.*s", (int)grant->who.len, grant->who.data);
	util_format(dest->name, sizeof dest->name, "%.*s", (int)name.len, name.data);
	return 0;
}

int
member_call_init (struct member_call           *dest,
                  struct member_registry const *registry,
                  struct policy_grant const    *grant,
                  struct stream_peer const     *peer)
{
	if (!dest)
		return EINVAL;
	*dest = (struct member_call){};
	if (!peer)
		return EINVAL;
	/* The key of a Unix socket's peer is { UINT64_MAX, uid }. */
	uint32_t const uid = (uint32_t)peer->key[1];
	return member_login(&dest->login, registry, grant,
	                    peer->key[0] == UINT64_MAX ? &uid : nullptr);
}

/* Writes to @p name the name of the session in the repository at @p path:
 * the last part of the path less .git, or of the directory that contains
 * a .git directory, or "session" if member_name_valid() refuses that.
 */
static void
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
	util_format(name, MEMBER_NAME, "%.*s", (int)base.len, base.data);
	if (!member_name_valid(base))
		util_format(name, MEMBER_NAME, "session");
}

/* Creates the session record of @p reg if no record of the session
 * exists, and reads the records into reg->records. Returns 0, or EINVAL,
 * EIO or ENOMEM with a reason in the @p len bytes at @p why.
 */
static int
member_registry_session_read (struct member_registry *reg,
                              char                   *why,
                              size_t                  len)
{
	struct member_records *r = &reg->records;
	int e = member_records_read(r, reg->store, why, len);
	for (size_t tries = 1; !e && !r->session.data && tries < MEMBER_TRIES; ++tries) {
		struct hexid id;
		e = hexid_make(&id);
		if (e) {
			util_format(why, len, "Could not make the session ID: %s.", strerror(e));
			return e;
		}
		char name[MEMBER_NAME];
		member_session_name(store_directory(reg->store), name);
		struct store_trailer const t[] = {
			{ CSTR_VIEW("Session"), hexid_view(&id) },
			{ CSTR_VIEW("Name"),    cstr_view(name) },
			{ CSTR_VIEW("Format"),  CSTR_VIEW("1") },
		};
		char message[STORE_MESSAGE];
		util_format(message, sizeof message, "Create the session %s", name);
		struct buf in = {};
		e = member_commit_input(reg, SESSION, cstr_view(message), t, array_size(t),
		                        &(struct oid){}, &in);
		e = member_commit(reg, SESSION, &in, e, why, len);
		if (!e || e == EAGAIN)
			e = member_records_read(r, reg->store, why, len);
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
	struct buf out = {};
	int status = -1;
	int e = store_init(reg->store, &out, &status);
	char text[MEMBER_WHY];
	struct cstr_view because = member_run_why(e, status, text, sizeof text);
	if (because.len)
		e = member_failed(why, len, "create the repository", buf_peek(&out), because);
	buf_fini(&out);
	if (!e)
		e = member_registry_session_read(reg, why, len);
	if (e)
		return e;
	struct member_login agent = { .kind = MEMBER_AGENT };
	util_format(agent.name, sizeof agent.name, "%s", reg->session.agent);
	struct member member;
	e = member_claim(reg, &agent, &reg->records, &member, why, len);
	if (e)
		return e == EPERM ? EINVAL : e;
	/* The records stay as they are from here on. */
	reg->session.id = reg->records.session;
	reg->session.name = reg->records.name;
	/* askai reads no notebook of the old layout, and the report counts
	 * them. */
	char const *const legacy[STORE_WORDS] = { "--format=%(refname)", LEGACY };
	status = -1;
	e = store_run(reg->store, STORE_REFS, legacy, nullptr, (struct span){}, &out, &status);
	because = member_run_why(e, status, text, sizeof text);
	if (because.len)
		e = member_failed(why, len, "list the notebooks under " LEGACY, (struct span){},
		                  because);
	struct span const refs = buf_peek(&out);
	for (size_t i = 0; i < refs.len; ++i)
		reg->legacy += refs.ptr[i] == '\n';
	buf_fini(&out);
	if (e)
		return e;
	/* A table looks up the name of the user, as for a Unix login. */
	uint32_t const uid = (uint32_t)geteuid();
	reg->own = (struct member_login){ .uid = uid, .kind = MEMBER_PERSON };
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
	if (!store || !agent || !service || !member_name_valid(cstr_view(agent)) ||
	    !member_name_valid(cstr_view(service))) {
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
		.session = { .agent = agent, .service = service },
		.store = store,
	};
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
	LOG_ALL(LOG_INFO, "  agent       %s", session->agent);
	LOG_ALL(LOG_INFO, "  service     %s", session->service);
	LOG_ALL(LOG_INFO, "  evidence    every member can read all stored evidence");
	if (registry->legacy)
		LOG_ALL(LOG_INFO, "  legacy      %zu notebook%s under " LEGACY, registry->legacy,
		        util_s(registry->legacy));
	int const width = member_records_width(r);
	LOG_ALL(LOG_INFO, "session members:");
	for (size_t i = 0; i < r->all.n; ++i) {
		struct member_entry const *entry = &r->all.items[i];
		struct member_info const *info = &entry->info;
		if (!(info->flags & MEMBER_RECORD))
			continue;
		char logins[MEMBER_LOGINS * (MEMBER_LOGIN + 1)] = "";
		size_t at = 0;
		struct cstr_view rest = entry->logins;
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
 * with a blocker. store_drop() lets go of the git of the lookup: a git that
 * reads is killed, and a git that writes runs to its end.
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

/* Writes the result @p e of @p lookup to @p wait: the member for 0, or
 * else the reason in lookup->why. Returns @p e.
 */
static int
member_lookup_give (struct member_lookup const *lookup,
                    struct member_wait         *wait,
                    int                         e)
{
	wait->e = e;
	wait->member = (struct member){ .kind = lookup->login.kind };
	wait->why[0] = '\0';
	if (e)
		util_format(wait->why, sizeof wait->why, "%s", lookup->why);
	else
		memcpy(wait->member.name, lookup->login.name, sizeof wait->member.name);
	return e;
}

/* Ends @p lookup with the result @p e and passes the result to each wait.
 * For 0 and EPERM, the lookup stays on the table with the result. For any
 * other result, the function removes the lookup from the table, unless
 * the job name is with a blocker. The waits run after that.
 */
static void
member_lookup_end (struct member_lookup *lookup,
                   int                   e)
{
	if (e && e != ECANCELED)
		LOG(LOG_WARN, "member: %s: %s", lookup->login.text, lookup->why);
	uint32_t const refused = e == EPERM ? LOOKUP_REFUSED : 0;
	bool const kept = !e || refused;
	lookup->flags = (lookup->flags & ~LOOKUP_WRITE) | LOOKUP_DONE | refused;
	struct list waits;
	list_init(&waits);
	list_transplant(&waits, &lookup->waits);
	struct list *node;
	list_foreach (node, &waits)
		(void)member_lookup_give(lookup, container_of(node, struct member_wait, hook), e);
	if (!kept && !(lookup->flags & LOOKUP_NAME))
		member_lookup_drop(lookup);
	while (!list_is_empty(&waits)) {
		struct member_wait *wait = container_of(waits.next, struct member_wait, hook);
		list_del(&wait->hook);
		wait->done(wait);
	}
}

/* Adds @p wait to @p lookup if @p e is EINPROGRESS. Otherwise the function
 * writes the result @p e to @p wait and ends the lookup with it. Returns
 * @p e.
 */
static int
member_lookup_join (struct member_lookup *lookup,
                    struct member_wait   *wait,
                    int                   e)
{
	if (e == EINPROGRESS) {
		list_append(&lookup->waits, &wait->hook);
		return wait->e = e;
	}
	(void)member_lookup_give(lookup, wait, e);
	member_lookup_end(lookup, e);
	return e;
}

/* Writes to lookup->why that askai is busy. Returns EAGAIN. */
static int
member_lookup_busy (struct member_lookup *lookup)
{
	util_format(lookup->why, sizeof lookup->why, "askai is busy. Try again.");
	return EAGAIN;
}

/* Writes to lookup->why that the name of the Unix user of @p lookup is not
 * known. Returns EAGAIN.
 */
static int
member_lookup_unnamed (struct member_lookup *lookup)
{
	util_format(lookup->why, sizeof lookup->why, "Could not look up the name of %s. Try "
	            "again.", lookup->login.text);
	return EAGAIN;
}

/* Runs git for @p lookup on the loop of its table: @p cmd with the words
 * @p words, the ref @p ref and the input @p in, which the function frees.
 * @p done runs once git ends. Returns EINPROGRESS, or EAGAIN or EIO with
 * the reason in lookup->why if git cannot start.
 */
static int
member_lookup_git (struct member_lookup *lookup,
                   enum store_cmd        cmd,
                   char const *const    *words,
                   char const           *ref,
                   struct buf           *in,
                   void                (*done) (struct child *c))
{
	struct member_table const *table = lookup->table;
	int e = 0;
	lookup->git = store_start(table->registry->store, table->loop, table->children, cmd, words,
	                          ref, buf_peek(in), done, lookup, &e);
	buf_fini(in);
	if (e == EBUSY)
		return member_lookup_busy(lookup);
	if (e)
		(void)child_spawn_why(e, "git", lookup->why, sizeof lookup->why);
	return e ? EIO : EINPROGRESS;
}

static void member_lookup_read_done (struct child *c);
static void member_lookup_wrote (struct child *c);

/* Runs git to read the records for @p lookup. Returns as
 * member_lookup_git() does.
 */
static int
member_lookup_read (struct member_lookup *lookup)
{
	return member_lookup_git(lookup, STORE_REFS, member_list, nullptr, &(struct buf){},
	                         member_lookup_read_done);
}

/* Runs git to write the record of the login of @p lookup: a new record,
 * or with @p entry, the login added to that record. Returns as
 * member_lookup_git() does, or EIO with the reason in lookup->why if the
 * input of git cannot be made.
 */
static int
member_lookup_write (struct member_lookup      *lookup,
                     struct member_entry const *entry)
{
	char ref[MEMBER_REF];
	struct buf in = {};
	int e = member_record_input(lookup->table->registry, &lookup->login, entry, ref, &in);
	if (!e)
		return member_lookup_git(lookup, STORE_COMMIT, nullptr, ref, &in, member_lookup_wrote);
	buf_fini(&in);
	char text[MEMBER_WHY];
	return member_wrote(ref, (struct span){}, member_run_why(e, 0, text, sizeof text),
	                    lookup->why, sizeof lookup->why);
}

/* Submits the job name of @p lookup to a blocker, with the deadline
 * MEMBER_NS. Returns EINPROGRESS, EAGAIN with the reason in lookup->why if
 * the blockers are busy, or the error of loop_timer_set().
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
		return member_lookup_busy(lookup);
	}
	lookup->flags |= LOOKUP_NAME;
	return EINPROGRESS;
}

/* Decides the member of the login of @p lookup with the records of its
 * table (member_decide()). For a Unix login that no record names, the job
 * name first looks up the name of the user. If a wait needs a record that
 * does not exist, git writes the record. Returns EINPROGRESS while the job
 * name or git runs. Otherwise returns the result: 0 with the member in
 * lookup->login, also a member whose record does not exist, or an error
 * number with the reason in lookup->why.
 */
static int
member_lookup_decide (struct member_lookup *lookup)
{
	struct member_login *login = &lookup->login;
	struct member member;
	struct member_entry const *entry;
	int const e = member_decide(&lookup->table->records, login, &member, &entry, lookup->why,
	                            sizeof lookup->why);
	if (*member.name) {
		login->kind = member.kind;
		memcpy(login->name, member.name, sizeof login->name);
	}
	/* A Unix login that no record names needs the name of its user. */
	if (!e && !*login->name)
		return member_lookup_submit(lookup);
	if (e != ENOENT && e != EEXIST)
		return e;
	if (!(lookup->flags & LOOKUP_WRITE))
		return 0;
	if (lookup->writes == MEMBER_TRIES)
		return member_raced(login, lookup->why, sizeof lookup->why);
	return member_lookup_write(lookup, entry);
}

/* On the loop: git read the records for the lookup of @p c. The records
 * replace the records of the table, and then decide the member.
 */
static void
member_lookup_read_done (struct child *c)
{
	struct member_lookup *lookup = c->owner;
	char text[MEMBER_WHY];
	struct cstr_view const because = member_child_why(c, text, sizeof text);
	int e;
	if (because.len)
		e = member_failed(lookup->why, sizeof lookup->why, "read the member records",
		                  (struct span){}, because);
	else
		e = member_records_invalid(member_table_refresh(lookup->table, buf_peek(&c->out)),
		                           lookup->why, sizeof lookup->why);
	child_destroy(&lookup->git);
	if (!e)
		e = member_lookup_decide(lookup);
	if (e != EINPROGRESS)
		member_lookup_end(lookup, e);
}

/* On the loop: git wrote the record for the lookup of @p c, or another
 * writer changed the record first. git then reads the records again.
 */
static void
member_lookup_wrote (struct child *c)
{
	struct member_lookup *lookup = c->owner;
	char ref[MEMBER_REF];
	util_format(ref, sizeof ref, MEMBERS "%s", lookup->login.name);
	char text[MEMBER_WHY];
	int e = member_wrote(ref, buf_peek(&c->out), member_child_why(c, text, sizeof text),
	                     lookup->why, sizeof lookup->why);
	child_destroy(&lookup->git);
	++lookup->writes;
	if (!e || e == EAGAIN)
		e = member_lookup_read(lookup);
	if (e != EINPROGRESS)
		member_lookup_end(lookup, e);
}

/* On a blocker: looks up the name of the Unix user of the lookup of
 * @p job.
 */
static void
member_lookup_name (struct blocker_job *job)
{
	struct member_lookup *lookup = container_of(job, struct member_lookup, name);
	job->error = member_user(lookup->login.uid, lookup->user);
}

/* Writes to lookup->login the name of the Unix user of @p lookup: the name
 * that the job name found, or uid-N for the error @p e of the job, which
 * the log names. For ENOENT, no user with the uid, and for EINVAL, a name
 * that member_name_valid() refuses, uid-N is the name of the person. For
 * any other error, such as ETIMEDOUT after MEMBER_NS, uid-N is a guess
 * (LOOKUP_GUESS), which a find answers and a claim does not write.
 * Returns EAGAIN with the reason in lookup->why for a guess that a wait
 * needs to write, else 0.
 */
static int
member_lookup_named (struct member_lookup *lookup,
                     int                   e)
{
	struct member_login *login = &lookup->login;
	lookup->flags &= ~LOOKUP_GUESS;
	if (!e) {
		memcpy(login->name, lookup->user, sizeof login->name);
		return 0;
	}
	util_format(login->name, sizeof login->name, "uid-%" PRIu32, login->uid);
	switch (e) {
	case ENOENT:
		LOG(LOG_WARN, "member: %s: no user with the uid exists, so the person is %s",
		    login->text, login->name);
		return 0;
	case EINVAL:
		LOG(LOG_WARN, "member: %s: the name of the user is not a valid member name, so the "
		    "person is %s", login->text, login->name);
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
	return lookup->flags & LOOKUP_WRITE ? member_lookup_unnamed(lookup) : 0;
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
	if (e == ECANCELED && !(lookup->flags & LOOKUP_DONE)) {
		util_format(lookup->why, sizeof lookup->why, "askai stops.");
		member_lookup_end(lookup, e);
		return;
	}
	e = member_lookup_named(lookup, e);
	if (!e && !(lookup->flags & LOOKUP_GUESS))
		e = member_lookup_decide(lookup);
	if (e != EINPROGRESS)
		member_lookup_end(lookup, e);
}

/* On the loop: the job name of a lookup took longer than MEMBER_NS. The
 * lookup ends, and the job stays with the blocker.
 */
static void
member_lookup_late (struct loop       *,
                    struct loop_timer *timer)
{
	struct member_lookup *lookup = container_of(timer, struct member_lookup, timer);
	member_lookup_end(lookup, member_lookup_named(lookup, ETIMEDOUT));
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
		.flags = flags,
		.login = *login,
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
                     int                          *e)
{
	if (!registry || !loop || !blockers || !children) {
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
	};
	list_init(&table->lookups);
	struct span const s = buf_peek(&registry->records.out);
	*e = buf_put(&table->records.out, s.ptr, s.len, STORE_MAX);
	if (!*e)
		*e = member_records_parse(&table->records);
	if (*e)
		member_table_destroy(&table);
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
	wait->why[0] = '\0';
	struct cstr_view const text = cstr_view(login->text);
	if (!text.len)
		return wait->e = ENOENT;
	struct member_entry const *entry = member_records_login(&table->records, text);
	if (entry) {
		member_of(&wait->member, &entry->info);
		return wait->e = 0;
	}
	struct member_lookup *lookup = nullptr;
	struct list *node;
	list_foreach (node, &table->lookups) {
		struct member_lookup *l = container_of(node, struct member_lookup, hook);
		if (cstr_view_equal(cstr_view(l->login.text), text)) {
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
	if (!(lookup->flags & LOOKUP_DONE)) {
		/* A lookup that a wait joins writes the record if any wait needs
		 * it. */
		lookup->flags |= write;
		return member_lookup_join(lookup, wait, EINPROGRESS);
	}
	/* The result of a lookup that ended answers a find. A claim of a
	 * member without a record decides again, and git writes the record.
	 * While the job name of a guess is with a blocker, a claim fails, so
	 * that no second job of the uid goes to the blockers. */
	if (lookup->flags & LOOKUP_REFUSED)
		return member_lookup_give(lookup, wait, EPERM);
	if (!write)
		return member_lookup_give(lookup, wait, 0);
	if (lookup->flags & LOOKUP_NAME)
		return member_lookup_give(lookup, wait, member_lookup_unnamed(lookup));
	if (lookup->flags & LOOKUP_GUESS)
		lookup->login.name[0] = '\0';
	lookup->flags = (lookup->flags & ~(LOOKUP_DONE | LOOKUP_GUESS)) | LOOKUP_WRITE;
	lookup->writes = 0;
	return member_lookup_join(lookup, wait, member_lookup_decide(lookup));
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

struct member_info const *
member_table_info (struct member_table const *table,
                   size_t                     i)
{
	return table && i < table->records.all.n ? &table->records.all.items[i].info : nullptr;
}

int
member_table_refresh (struct member_table *table,
                      struct span          out)
{
	if (!table)
		return EINVAL;
	struct member_records r = {};
	int e = buf_put(&r.out, out.ptr, out.len, STORE_MAX);
	if (!e)
		e = member_records_parse(&r);
	if (!e) {
		struct member_records const old = table->records;
		table->records = r;
		r = old;
	}
	member_records_fini(&r);
	return e;
}

int
member_table_name (struct member_table const *table,
                   struct cstr_view           name,
                   struct member             *dest)
{
	if (!table || !dest)
		return EINVAL;
	struct member_entry const *entry = member_records_find(&table->records, name);
	if (!entry)
		return ENOENT;
	member_of(dest, &entry->info);
	return 0;
}

int
member_table_width (struct member_table const *table)
{
	return table ? member_records_width(&table->records) : 0;
}

int
member_table_notice (struct member_table const *table,
                     struct member const       *you,
                     struct dstr               *d)
{
	if (!table || !you || !d)
		return EINVAL;
	size_t const n = table->records.all.n;
	bool ok = dstr_cat(d, "[askai: Members of this session: ");
	for (size_t i = 0; ok && i < n && i < MEMBER_SHOWN; ++i) {
		struct member_info const *info = &table->records.all.items[i].info;
		bool const self = info->kind == you->kind &&
		                  cstr_view_equal(info->name, cstr_view(you->name));
		ok = dstr_printf(d, "%s%.*s (%s%s)", i ? ", " : "", (int)info->name.len,
		                 info->name.data, member_kinds[info->kind].data, self ? ", you" : "");
	}
	if (ok && n > MEMBER_SHOWN)
		ok = dstr_printf(d, " and %zu more", n - MEMBER_SHOWN);
	ok = ok && dstr_cat(d, ". Call notes with action members to see what each member wrote "
	                       "last.]\n");
	return ok ? 0 : ENOMEM;
}

int
member_table_unknown (struct member_table const *table,
                      struct cstr_view           name,
                      struct dstr               *d)
{
	if (!table || !d)
		return EINVAL;
	size_t const n = table->records.all.n;
	size_t const shown = n < MEMBER_SHOWN ? n : MEMBER_SHOWN;
	bool ok = dstr_printf(d, "No member is named %.*s. %s ",
	                      (int)util_clip(name.data, name.len, 2 * UTIL_WORD_MAX), name.data,
	                      n == 1 ? "The only member is" : "The members are");
	for (size_t i = 0; ok && i < shown; ++i) {
		struct member_info const *info = &table->records.all.items[i].info;
		char const *sep = !i ? "" : i + 1 < shown || n > shown ? ", " : " and ";
		ok = dstr_printf(d, "%s%.*s", sep, (int)info->name.len, info->name.data);
	}
	if (ok && n > shown)
		ok = dstr_printf(d, " and %zu more", n - shown);
	return ok && dstr_cat(d, ".") ? 0 : ENOMEM;
}

void
member_byline_put (struct member_byline     *byline,
                   struct store_commit      *commit,
                   struct member const      *self,
                   struct member_call const *call,
                   uint32_t                  k,
                   char const               *service)
{
	(void)util_format(byline->email, sizeof byline->email, "%s/%s",
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
	commit->author = (struct store_ident){ cstr_view(self->name), cstr_view(byline->email) };
	commit->committer = (struct store_ident){ cstr_view(service), CSTR_VIEW("askai") };
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
