/** @file member.h
 *
 * The members of the session of --notes: persons and agents. A member is
 * of one kind, with one name (src/member_name.h). A person is a human or a
 * service account, named by a Unix user or by an identity of the policy
 * (src/policy.h). An agent is an AI member: the agent of an askai service
 * (--agent), or an identity whose member "agent" is true. A login is how a
 * client proves who it is: "uid N" for a Unix user, or "identity NAME" for
 * an identity.
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
 * The branches of a member are refs/heads/KIND/NAME/LEAF (member_ref()):
 * the notebook of each member (src/notes.h), and the findings of an agent
 * (src/finding.h). A table lists the records and the branches together, so
 * a branch without a record, such as a notebook that the operator moved
 * from the old layout, still names a member.
 */
#ifndef ASKAI_SRC_MEMBER_H_
#define ASKAI_SRC_MEMBER_H_

#include <stddef.h>
#include <stdint.h>

#include "clk.h"
#include "cstr_view.h"
#include "hexid.h"
#include "list.h"
#include "member_name.h"
#include "oid.h"
#include "policy.h"
#include "store.h"
#include "tool.h"
#include "util.h"

struct blocker;
struct child;
struct dstr;
struct json_object;
struct loop;
struct member_registry;
struct member_table;

enum {
	MEMBER_LOGIN  = sizeof "identity " + POLICY_NAME, /**< bytes of a login,
	                                                      NUL included */
	MEMBER_LOGINS = 8,                               /**< logins of a member,
	                                                      at most */
	MEMBER_FORMAT = 1,                               /**< the format of the
	                                                      records that askai
	                                                      writes */
	MEMBER_EMAIL  = sizeof "person/" + UTIL_WORD_MAX, /**< bytes of the email
	                                                      address of an author,
	                                                      KIND/NAME, NUL
	                                                      included */
	MEMBER_REF    = sizeof "refs/heads/person//findings" + UTIL_WORD_MAX,
	                                                 /**< bytes of the ref of a
	                                                      branch or of the
	                                                      record of a member,
	                                                      NUL included */
	MEMBER_TEXT   = sizeof " (person, you)" + UTIL_WORD_MAX,
	                                                 /**< bytes of a member as
	                                                      member_text() writes
	                                                      it, NUL included */
};

/** The time for the lookup of the name of a Unix user, in nanoseconds.
 *  After a longer lookup, a find names the person uid-N, and a claim fails
 *  with EAGAIN.
 */
#define MEMBER_NS (10 * NS)

/** The branches of a member: X(ID, leaf), the constant MEMBER_BRANCH_ID of
 *  enum member_branch and the last part of the ref of the branch,
 *  refs/heads/KIND/NAME/LEAF. NOTES is the notebook, and FINDINGS the
 *  findings of an agent.
 */
#define MEMBER_BRANCH_LIST(X)   \
        X(NOTES,    "notes")    \
        X(FINDINGS, "findings")

enum member_branch : uint8_t {
#define MEMBER_BRANCH_ENUM(id, ...) MEMBER_BRANCH_##id,
	MEMBER_BRANCH_LIST(MEMBER_BRANCH_ENUM)
#undef MEMBER_BRANCH_ENUM
	MEMBER_BRANCHES
};

/** The flags of a member in a table (struct member_info). */
enum {
	MEMBER_RECORD = 1u << 0,  /**< a record names the member */
	MEMBER_NOTES  = 1u << 1,  /**< the notebook of the member contains
	                               notes */
};

/** A member in a table: the record of the member, the newest change of
 *  the notebook of the member, and the tip of the branch of findings of
 *  the member. The views point into the records of the table, which
 *  member_table_parse() can replace.
 */
struct member_info {
	struct cstr_view name;
	struct cstr_view logins;    /**< the values of the trailer lines Login
	                                 of the record, separated by commas, or
	                                 empty */
	struct cstr_view change;    /**< the subject of the newest commit of
	                                 the notebook */
	struct cstr_view asker;     /**< the trailer line Asked-by of that
	                                 commit, or empty */
	uint64_t         time;      /**< the time of that commit, in seconds
	                                 since the epoch */
	uint32_t         kind;      /**< enum member_kind */
	uint32_t         flags;     /**< MEMBER_RECORD and MEMBER_NOTES */
	struct oid       record;    /**< the commit of the record, or no name */
	struct oid       findings;  /**< the tip of the branch of findings, or
	                                 no name */
};

/** The session of a registry, and this askai in it, made once as askai
 *  starts. The views borrow the registry and the name of the service.
 */
struct member_session {
	struct cstr_view   id;         /**< the session ID, 32 hexadecimal
	                                    digits */
	struct cstr_view   name;       /**< the name of the session */
	struct store_ident committer;  /**< this askai, SERVICE <askai>, the
	                                    committer of its commits */
	size_t             legacy;     /**< the notebooks of the old layout
	                                    under refs/heads/notes/, which askai
	                                    does not read */
	struct member      agent;      /**< the agent of this askai */
	char               refs[MEMBER_BRANCHES][MEMBER_REF];
	                               /**< the branches of the agent, by enum
	                                    member_branch (member_ref()) */
};

/** A login: how a client proves who it is, and the member that the login
 *  makes if the records do not name the member of the login yet.
 */
struct member_login {
	uint32_t      uid;                 /**< the uid of a Unix login */
	struct member member;              /**< the new member, with the name
	                                        "" for a Unix login whose name
	                                        a lookup finds */
	char          text[MEMBER_LOGIN];  /**< "uid N" or "identity NAME" as
	                                        in the trailer line Login, or
	                                        "" for no login */
};

/** The member of the client of a request, and the IDs that the commits of
 *  the turns of an ask call name. Each request of mcp.c contains one
 *  (mcp_slot_member()). mcp.c makes the IDs (member_call_ids()), and the
 *  notes tool sets the asker (mcp_slot_asker()).
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

/** Writes the member @p name of the kind @p kind to @p text as NAME (KIND),
 *  such as bro (agent), or as NAME (KIND, you) if @p you is true. Returns
 *  the view of the text in @p text.
 */
extern struct cstr_view
member_text (struct cstr_view name,
             enum member_kind kind,
             bool             you,
             char             text[static MEMBER_TEXT]);

/** Returns a new JSON object of the member @p name of the kind @p kind,
 *  {"name": NAME, "kind": KIND}, such as the asker in the journal state of
 *  an ask call or a member in the result of members, or nullptr for a
 *  @p name without data, a @p kind past the kinds, or if out of memory.
 */
extern struct json_object *
member_json (struct cstr_view name,
             enum member_kind kind);

/** Initializes @p dest as the member of @p json, an object as member_json()
 *  writes it. Returns 0, EINVAL for a null @p dest, or EINVAL with no
 *  member in @p dest for an object whose name member_name_valid() refuses
 *  or whose kind member_kinds does not name.
 */
extern int
member_init_json (struct member      *dest,
                  struct json_object *json);

/** Writes to @p ref the ref of the branch @p branch of @p member, a member
 *  with a name: refs/heads/KIND/NAME/LEAF, such as
 *  refs/heads/agent/bro/findings.
 */
extern void
member_ref (struct member const *member,
            enum member_branch   branch,
            char                 ref[static MEMBER_REF]);

/** Reads @p ref as the ref of a branch of a member, as member_ref() writes
 *  it, and stores the kind of the member in @p *kind, its name, a view of
 *  @p ref, in @p *name, and the branch in @p *branch. Returns 0, or EINVAL
 *  for any other ref, such as a ref of an unknown kind or leaf, or of a
 *  name that member_name_valid() refuses.
 */
extern int
member_ref_parse (struct cstr_view    ref,
                  enum member_kind   *kind,
                  struct cstr_view   *name,
                  enum member_branch *branch);

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

/** Makes the ID of the ask call of @p call, and the ID of a new
 *  conversation unless call->conversation is the conversation that the ask
 *  call continues. Returns 0, EINVAL for a null @p call, or the error of
 *  hexid_make().
 */
extern int
member_call_ids (struct member_call *call);

/**
 * Creates the registry of the session in the repository @p store, as askai
 * starts, off the loops: it creates the repository if it does not exist,
 * creates the session record if it does not exist, and creates the record
 * of the agent @p agent or adopts it, with git on the calling thread.
 * Another askai can create the repository or a record at the same time.
 * @p service is the name of this askai, the committer of the records.
 * Both are names that member_name_valid() accepts. @p store and
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

/** Returns the repository of the session of @p registry, or nullptr for a
 *  null @p registry.
 */
extern struct store const *
member_registry_store (struct member_registry const *registry);

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
 * sets the other members before it calls done. done finds the owner with
 * container_of(). The owner calls member_table_cancel() before it frees a
 * wait that waits.
 */
struct member_wait {
	struct list      hook;    /**< on the lookup of the login while the wait
	                               waits */
	/** Called on the loop of the table once the lookup ends. */
	void           (*done) (struct member_wait *wait);
	struct tool_why  why;     /**< the reason for an error other than ENOENT
	                               and ENOMEM, a text for the client */
	int16_t          e;       /**< 0, or the error number for a login
	                               without a member, as member_table_find()
	                               returns it. With an int16_t, the struct
	                               is without padding. */
	struct member    member;  /**< the member of the login */
};

/** Creates the table of the members on @p loop, with the records that
 *  @p registry read at the start. @p registry must outlive the table. git
 *  runs for the lookups on @p loop, with its children on @p children
 *  (src/child.h), and the blockers @p blockers look up the names of Unix
 *  users. Returns the table, or returns nullptr with an error number in
 *  @p *e: EINVAL for a null argument, or ENOMEM.
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

/** Returns the registry that @p table was created with, or nullptr for a
 *  null @p table.
 */
extern struct member_registry const *
member_table_registry (struct member_table const *table);

/**
 * Finds the member of @p login for @p wait, and writes no record. If the
 * records of @p table name the member, the function writes the member to
 * wait->member and returns 0. Otherwise git reads the records again. If
 * they do not name the member either, the member is the member that
 * member_table_claim() makes, without a record: login->member, a person or
 * an agent, or for an empty name a person named like the Unix user, or
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
 * Returns as member_table_find() does, also EIO after STORE_TRIES writes
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

/** Returns the members of @p table, the persons first, then the agents,
 *  each by name, and stores their number in @p *n, and the length of the
 *  longest name in @p *width unless @p width is nullptr. The array stays
 *  valid until member_table_parse() replaces the records. Returns nullptr
 *  with 0 in @p *n and @p *width for a null @p table.
 */
extern struct member_info const *
member_table_members (struct member_table const *table,
                      size_t                    *n,
                      size_t                    *width);

/** The words of STORE_REFS (src/store.h) that list the records of the
 *  session and the branches of the members, for member_table_parse().
 */
extern char const *const member_list[STORE_WORDS];

/**
 * Replaces the records of @p table with the records that @p git listed
 * with member_list. The output of @p git moves to the table. Returns 0,
 * EIO if git failed or wrote output that is not a list of the records, or
 * ENOMEM, with the reason in the @p len bytes at @p why, a text for the
 * client. On failure the records stay as they are.
 */
extern int
member_table_parse (struct member_table *table,
                    struct child        *git,
                    char                *why,
                    size_t               len);

/** Writes to @p dest the member named @p name: the member of a record of
 *  the name, or else of a notebook of the name. Returns 0, ENOENT if
 *  @p table names no such member, or EINVAL for a null argument.
 */
extern int
member_table_name (struct member_table const *table,
                   struct cstr_view           name,
                   struct member             *dest);

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
 * session->committer, and the trailer lines name the ask call, the call,
 * the conversation and the asker of @p call, each if it is known. @p k is
 * 0 for a call that is not a turn's call. The views of @p commit point to
 * @p byline, @p self, @p call and @p session, which must outlive
 * @p commit.
 */
extern void
member_byline_put (struct member_byline        *byline,
                   struct store_commit         *commit,
                   struct member const         *self,
                   struct member_call const    *call,
                   uint32_t                     k,
                   struct member_session const *session);

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
