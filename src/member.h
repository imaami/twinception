/** @file member.h
 *
 * The members of the session of --notes: persons and agents. A member is
 * of one kind, with one name. A person is a human or a service account,
 * named by a Unix user or by an identity of the policy (src/policy.h). An
 * agent is an AI member: the agent of an askai service (--agent), or an
 * identity whose member "agent" is true. A login is how a client proves
 * who it is: "uid N" for a Unix user, or "identity NAME" for an identity.
 *
 * The registry of the members is in the repository of the session
 * (src/store.h). The record of a member is the create-only ref
 * refs/askai/members/NAME: a commit with the empty tree, whose message is
 * the record, with the trailer lines Kind and Login. The trailer of the
 * session record refs/askai/session consists of the lines Session, Name
 * and Format. The first writer of a record creates it. A later writer
 * adopts the record or is refused, so a name belongs to one member of one
 * kind.
 *
 * askai creates the repository, the session record and the record of its
 * agent as it starts (member_registry_create()). A person becomes a member
 * on the first claim of a login of the person (member_table_claim()). A
 * lookup finds the login in the records first, so a person keeps the name
 * after the Unix user is renamed. Otherwise the name of a Unix login is
 * the name of the user, which a blocker looks up with getpwuid_r().
 *
 * Each member has one notebook (src/notes.h), the branch
 * refs/heads/KIND/NAME/notes. A table lists the records and the notebooks
 * together, so a notebook without a record, such as a notebook that the
 * operator moved from the old layout, still names a member.
 */
#ifndef ASKAI_SRC_MEMBER_H_
#define ASKAI_SRC_MEMBER_H_

#include <stddef.h>
#include <stdint.h>

#include "clk.h"
#include "cstr_view.h"
#include "hexid.h"
#include "list.h"
#include "policy.h"
#include "store.h"
#include "util.h"

struct blocker;
struct dstr;
struct list;
struct loop;
struct stream_peer;
struct member_registry;
struct member_table;
struct store;

enum {
	MEMBER_NAME   = UTIL_WORD_MAX + 1,               /**< bytes of a member
	                                                      name, NUL
	                                                      included */
	MEMBER_LOGIN  = sizeof "identity " + POLICY_NAME, /**< bytes of a login,
	                                                      NUL included */
	MEMBER_LOGINS = 8,                               /**< logins of a member,
	                                                      at most */
	MEMBER_WHY    = 258,                             /**< bytes of the reason
	                                                      for a login without
	                                                      a member, NUL
	                                                      included */
	MEMBER_FORMAT = 1,                               /**< the format of the
	                                                      records that askai
	                                                      writes */
	MEMBER_EMAIL  = sizeof "person/" + UTIL_WORD_MAX, /**< bytes of the email
	                                                      address of an author,
	                                                      KIND/NAME, NUL
	                                                      included */
};

/** The time for the lookup of the name of a Unix user, in nanoseconds.
 *  After a longer lookup, a find names the person uid-N, and a claim fails
 *  with EAGAIN.
 */
static constexpr uint64_t MEMBER_NS = 10 * NS;

/** The kinds of members. */
enum member_kind : uint8_t {
	MEMBER_PERSON,  /**< a human or a service account */
	MEMBER_AGENT,   /**< an AI member */
};

/** The names of the kinds, indexed by enum member_kind, as they are in
 *  the trailer line Kind of a record.
 */
static struct cstr_view const member_kinds[] = {
	[MEMBER_PERSON] = CSTR_VIEW_INIT("person"),
	[MEMBER_AGENT]  = CSTR_VIEW_INIT("agent"),
};

/** A member of the session. */
struct member {
	enum member_kind kind;
	char             name[MEMBER_NAME];  /**< "" for no member */
};

/** The flags of a member in a table (struct member_info). */
enum {
	MEMBER_RECORD = 1u << 0,  /**< a record names the member */
	MEMBER_NOTES  = 1u << 1,  /**< the notebook of the member contains
	                               notes */
};

/** A member in a table, and the newest change of the notebook of the
 *  member. The views borrow the records of the table, which a lookup or
 *  member_table_refresh() can replace.
 */
struct member_info {
	struct cstr_view name;
	struct cstr_view change;  /**< the subject of the newest commit of the
	                               notebook */
	struct cstr_view asker;   /**< the trailer line Asked-by of that commit,
	                               or empty */
	uint64_t         time;    /**< the time of that commit, in seconds
	                               since the epoch */
	uint32_t         kind;    /**< enum member_kind */
	uint32_t         flags;   /**< MEMBER_RECORD and MEMBER_NOTES */
};

/** The session of a registry, and this askai in it. The views borrow the
 *  registry.
 */
struct member_session {
	struct cstr_view id;       /**< the session ID, 32 hexadecimal digits */
	struct cstr_view name;     /**< the name of the session */
	char const      *agent;    /**< the name of the agent of this askai */
	char const      *service;  /**< the name of this askai, the committer of
	                                its commits */
};

/** A login: how a client proves who it is, and the member that the login
 *  makes if the records do not name the member of the login yet.
 */
struct member_login {
	uint32_t         uid;                 /**< the uid of a Unix login */
	enum member_kind kind;                /**< the kind of a new member */
	char             text[MEMBER_LOGIN];  /**< "uid N" or "identity NAME"
	                                           as in the trailer line Login,
	                                           or "" for no login */
	char             name[MEMBER_NAME];   /**< the name of a new member, or
	                                           "" for a Unix login whose
	                                           name a lookup finds */
};

/** The member of the client of a request, and the IDs that the commits of
 *  the turns of an ask call name. Each request of mcp.c contains one
 *  (mcp_slot_member()), and the notes tool fills in the rest.
 */
struct member_call {
	struct member_login login;         /**< the login of the client, from
	                                        the head of the request */
	struct hexid        ask;           /**< the ID of the ask call, or
	                                        empty */
	struct hexid        conversation;  /**< the conversation of the ask
	                                        call, or empty */
	struct member       asker;         /**< the member of the login that
	                                        made the ask call, or the name
	                                        "" for no such member */
};

/** The storage of the parts of a commit that member_byline_put() fills:
 *  the author, the committer and the trailer lines.
 */
struct member_byline {
	struct store_trailer trailers[4];      /**< Ask, Call, Conversation and
	                                            Asked-by, each if known */
	char                 email[MEMBER_EMAIL];  /**< the email address of
	                                            the author, KIND/NAME */
	char                 call[16];             /**< the number of the call,
	                                                NUL included */
};

/** Initializes @p dest for a request of a client with the grant @p grant
 *  and the address @p peer (src/stream.h): the login of the client, as
 *  member_login() writes it, no IDs, and no asker. grant->who must be
 *  valid. Returns 0, or the error of member_login(), such as EINVAL for a
 *  null @p registry, as without --notes, with no login in @p dest.
 */
extern int
member_call_init (struct member_call           *dest,
                  struct member_registry const *registry,
                  struct policy_grant const    *grant,
                  struct stream_peer const     *peer);

/** The words of STORE_REFS that list the records of the session and the
 *  notebooks of the members, for member_table_refresh().
 */
extern char const *const member_list[STORE_WORDS];

/**
 * Returns true if @p name can be the name of a member: 1 to UTIL_WORD_MAX
 * characters from [A-Za-z0-9_.@-], not starting with . or -, without ..,
 * not ending with . or .lock, not @, and not a letter followed only by
 * digits, such as c7. The name of a service of askai is of the same
 * form.
 */
static inline bool
member_name_valid (struct cstr_view name)
{
	if (!name.data || !name.len || name.len > UTIL_WORD_MAX || *name.data == '.' ||
	    *name.data == '-' || name.data[name.len - 1] == '.' ||
	    cstr_view_ends(name, CSTR_VIEW(".lock")) || cstr_view_equal(name, CSTR_VIEW("@")))
		return false;
	bool digits = name.len > 1;
	for (size_t i = 0; i < name.len; ++i) {
		char const c = name.data[i];
		bool const digit = c >= '0' && c <= '9';
		bool const letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		if (!digit && !letter && c != '_' && c != '.' && c != '@' && c != '-')
			return false;
		if (c == '.' && i && name.data[i - 1] == '.')
			return false;
		/* The first character is a letter, and the others are digits. */
		if (i ? !digit : !letter)
			digits = false;
	}
	return !digits;
}

/**
 * Writes to @p dest the login of a client with the grant @p grant
 * (src/policy.h). @p uid is the uid of a client on a Unix socket, or
 * nullptr for a client over TCP. A Unix user is a person whose name a
 * lookup finds. An identity over TCP is a person or, with POLICY_AGENT, an
 * agent, named like the identity. ASKAI_TOKEN under askai's own policy
 * (POLICY_OPERATOR) over TCP is the user who runs askai. Returns 0, ENOENT
 * with no login in @p dest for a client over TCP without a token, which is
 * no member, or EINVAL for a null @p dest, @p registry or @p grant, or an
 * identity whose name member_name_valid() refuses. grant->who must be
 * valid: httpd clears it after the head of a request (src/stream.h).
 */
extern int
member_login (struct member_login          *dest,
              struct member_registry const *registry,
              struct policy_grant const    *grant,
              uint32_t const               *uid);

/**
 * Creates the registry of the session in the repository @p store, as askai
 * starts, off the loops: it creates the repository if it does not exist,
 * creates the session record if it does not exist, and creates the record
 * of the agent @p agent or adopts it, with git on the calling thread.
 * Another askai can create the repository or a record at the same time.
 * @p service is the name of this askai, the committer of the records.
 * Both are names that member_name_valid() accepts. @p store, @p agent and
 * @p service must outlive the registry. The function then reads all
 * records and counts the notebooks of the old layout. Returns the
 * registry, or returns nullptr with an error number in @p *e and a message
 * in the @p len bytes at @p why: EINVAL for a null or invalid argument, a
 * session of a later format than MEMBER_FORMAT, or a record of the name of
 * the agent that belongs to a person; EIO if git fails; or ENOMEM.
 */
extern struct member_registry *
member_registry_create (struct store const *store,
                        char const         *agent,
                        char const         *service,
                        char               *why,
                        size_t              len,
                        int                *e);

/** Frees the registry at @p *p_registry and sets @p *p_registry to
 *  nullptr.
 */
extern void
member_registry_destroy (struct member_registry **p_registry);

/** Returns the session of @p registry and this askai in it, or nullptr
 *  for a null @p registry.
 */
extern struct member_session const *
member_registry_session (struct member_registry const *registry);

/** Logs @p registry at LOG_INFO as two lists: the session, with its
 *  repository, ID, name, agent, service, that every member can read the
 *  evidence of the findings, and the number of notebooks of the old layout
 *  under refs/heads/notes/ if there are any, and the members as askai read
 *  them at the start, each with its kind and logins.
 */
extern void
member_registry_report (struct member_registry const *registry);

/**
 * A call that waits for the member of a login (member_table_find() and
 * member_table_claim()). The owner of the wait sets done, and the table
 * sets the other members before it calls done. The owner calls
 * member_table_cancel() before it frees a wait that waits.
 */
struct member_wait {
	struct list    hook;             /**< on the lookup of the login while
	                                      the wait waits */
	/** Called on the loop of the table once the lookup ends. */
	void         (*done) (struct member_wait *wait);
	int            e;                /**< 0, or the error number for a
	                                      login without a member, as
	                                      member_table_find() returns it */
	struct member  member;           /**< the member of the login */
	char           why[MEMBER_WHY];  /**< the reason for an error other
	                                      than ENOENT and ENOMEM, a text
	                                      for the client */
};

/** Creates the table of the members on @p loop, with the records that
 *  @p registry read at the start. git runs for the lookups on @p loop,
 *  with its children on @p children (src/child.h), and the blockers
 *  @p blockers look up the names of Unix users. Returns the table, or
 *  returns nullptr with an error number in @p *e: EINVAL for a null
 *  argument, or ENOMEM.
 */
extern struct member_table *
member_table_create (struct member_registry const *registry,
                     struct loop                  *loop,
                     struct blocker               *blockers,
                     struct list                  *children,
                     int                          *e);

/** Frees the table at @p *p_table and sets @p *p_table to nullptr. A
 *  lookup under way calls no wait. Its git is killed if it reads, and runs
 *  to its end if it writes.
 */
extern void
member_table_destroy (struct member_table **p_table);

/**
 * Finds the member of @p login for @p wait, and writes no record. If the
 * records of @p table name the member, the function writes the member to
 * wait->member and returns 0. Otherwise git reads the records again. If
 * they do not name the member either, the member is the member that
 * member_table_claim() makes, without a record: a person or an agent named
 * login->name, or for an empty name a person named like the Unix user, or
 * uid-N if no user with the uid exists or member_name_valid() refuses the
 * name of the user. If the lookup of the name fails in another way, such
 * as after MEMBER_NS, the person is named uid-N until a later lookup of
 * the name works. If the record of the name exists, the login adopts it
 * if it is a record of the same kind and of the same login. An identity
 * login and a Unix login of one name are one person. Any other record
 * refuses the login. The later finds of the login get the result without
 * git: the member, also a member without a record, or the refusal.
 *
 * Returns 0, or EINPROGRESS if the wait waits: the function adds @p wait
 * to the lookup of the login, and wait->done runs later, with the result
 * in @p wait. The function and wait->done set wait->e to 0; ENOENT for no
 * login, which is no member; EPERM if the record of the name refuses the
 * login; EIO if git fails; EAGAIN if askai is busy; ECANCELED if askai
 * stops; ENOMEM; or EINVAL for a null argument. wait->why contains the
 * reason for EPERM, EIO, EAGAIN and ECANCELED, a text for the client.
 */
extern int
member_table_find (struct member_table       *table,
                   struct member_login const *login,
                   struct member_wait        *wait);

/**
 * Finds the member of @p login for @p wait as member_table_find() does,
 * and writes the record of the member if no record names the member: git
 * creates the record of a new member, or adds the login to the record of
 * the person of the name. git reads the records again after each write.
 * Returns as member_table_find() does, also EIO after MEMBER_TRIES writes
 * that lost a race to other writers. A name of a Unix user that is uid-N
 * until a later lookup works gets no record: the claim looks up the name
 * again, or fails with EAGAIN while that lookup runs or after it fails.
 */
extern int
member_table_claim (struct member_table       *table,
                    struct member_login const *login,
                    struct member_wait        *wait);

/** Stops @p wait from waiting, if it waits. Its done does not run. */
extern void
member_table_cancel (struct member_wait *wait);

/** Returns the member at the index @p i of @p table, or nullptr past the
 *  last member. The persons come first, then the agents, each by name.
 */
extern struct member_info const *
member_table_info (struct member_table const *table,
                   size_t                     i);

/** Replaces the records of @p table with the records in @p out, the whole
 *  output of STORE_REFS for member_list. Returns 0, EINVAL for a null
 *  @p table, EIO for output that is not in that form, or ENOMEM. On
 *  failure the records stay as they are.
 */
extern int
member_table_refresh (struct member_table *table,
                      struct span          out);

/** Writes to @p dest the member named @p name: the member of a record of
 *  the name, or else of a notebook of the name. Returns 0, ENOENT if
 *  @p table names no such member, or EINVAL for a null argument.
 */
extern int
member_table_name (struct member_table const *table,
                   struct cstr_view           name,
                   struct member             *dest);

/** Returns the length of the longest name of the members of @p table, or
 *  0 for a null @p table.
 */
extern int
member_table_width (struct member_table const *table);

/** Appends to @p d the notice of the members for a turn of the agent
 *  @p you: "[askai: Members of this session: ...]" and a line feed.
 *  Returns 0, EINVAL for a null argument, or ENOMEM.
 */
extern int
member_table_notice (struct member_table const *table,
                     struct member const       *you,
                     struct dstr               *d);

/** Appends to @p d that no member is named @p name, and the names of the
 *  members. Returns 0, EINVAL for a null @p table or @p d, or ENOMEM.
 */
extern int
member_table_unknown (struct member_table const *table,
                      struct cstr_view           name,
                      struct dstr               *d);

/**
 * Fills the author, the committer and the trailer lines of @p commit, a
 * commit that the member @p self writes for the call @p k of the request
 * of @p call: the author is @p self as NAME <KIND/NAME>, the committer is
 * the service @p service as SERVICE <askai>, and the trailer lines name
 * the ask call, the call, the conversation and the asker of @p call, each
 * if it is known. @p k is 0 for a call that is not a turn's call. The
 * views of @p commit point to @p byline, @p self, @p call and @p service,
 * which must outlive @p commit.
 */
extern void
member_byline_put (struct member_byline     *byline,
                   struct store_commit      *commit,
                   struct member const      *self,
                   struct member_call const *call,
                   uint32_t                  k,
                   char const               *service);

/** Appends to @p d the notice of who the agent @p you is and who made the
 *  ask call @p call: call->asker, or a client without a member name if
 *  the name of call->asker is "". Returns 0, EINVAL for a null argument,
 *  or ENOMEM.
 */
extern int
member_notice (struct member const      *you,
               struct member_call const *call,
               struct dstr              *d);

#endif /* ASKAI_SRC_MEMBER_H_ */
