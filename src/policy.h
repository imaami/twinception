/** @file policy.h
 *
 * Identities and roles (design section 9, S7).  The policy decides who a
 * client is and what it may see and do.  A client is identified by the
 * bearer token that it sends, or, if it sends none, by how it connects.
 *
 * The member "roles" of the configuration file names what each role may
 * see and do: the tools that its clients may list and call, and whether
 * they may read the metrics.  The member "upstream" of a role lists the
 * tools that the upstream may call in the turns of the role's ask calls.
 * Without it, the upstream may call the tools that the role's clients may
 * call.
 *
 * The member "identities" names the clients that prove who they are with
 * a token.  Each identity has a role and a list of tokens.  A token is
 * given as its SHA-256 digest ("sha256:HEX") or as the name of an
 * environment variable that holds it ("env:NAME").
 *
 * The member "unix-groups" gives a client on a Unix socket the role of the
 * first of its groups that the member names.  The member "anonymous" names
 * the role, if any, of a client that sends no token, by how it connects.
 *
 * Over TCP, a token that no identity has is refused, and a policy without
 * tokens treats every client as one that sends no token.  A client on a
 * Unix socket may not send a token, except ASKAI_TOKEN under askai's own
 * policy.  A client without a token is refused if no role is named for it.
 * Group names are looked up through NSS when the policy is made, which may
 * block.
 *
 * A principal tells whose calls they are.  It is the owner of notes
 * branches, jobs, tasks and plugin instances, and it selects the queue of
 * the scheduler.  The principal is derived from the first 8 bytes of
 * HMAC-SHA-256 with a secret key (policy_key()) over "uid:N" for a Unix
 * user, or over "identity:NAME" for an identity.  Bit 63 is never set.  Bit
 * 62 is set for an identity and clear for a Unix user, so the two kinds
 * never share a principal.  A client on a Unix socket always has the
 * principal of its uid.  The principal 0 means an anonymous client over
 * TCP.
 *
 * A file without these members keeps askai's own policy.  With ASKAI_TOKEN
 * set, clients must send it, and may do everything.  Without it, a client
 * on loopback that names a loopback host, or a client on a Unix socket, may
 * do everything, and any other client may call the tools.
 */
#ifndef TWINCEPTION_SRC_POLICY_H_
#define TWINCEPTION_SRC_POLICY_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "tool.h"

struct json_object;
struct policy;

/** How a client that shows no token connects. */
enum policy_kind {
	POLICY_LOOPBACK,  /**< over TCP on loopback, naming a loopback host */
	POLICY_REMOTE,    /**< over TCP otherwise */
	POLICY_UNIX,      /**< over a Unix socket */
	POLICY_KINDS
};

/** Bytes of a role's or an identity's name, at most. */
static constexpr size_t POLICY_NAME = 64;

/** Bytes of the key of the principals (policy_key()). */
static constexpr size_t POLICY_KEY = 32;

/** Bytes of the longest name of a client in the log, with a terminating
 *  NUL.  mcp.c names a client on a Unix socket "uid N (WHO)", where WHO is
 *  the member who of struct policy_grant, and other clients by who alone.
 */
static constexpr size_t POLICY_WHO = sizeof "uid 4294967295 (identity )" + POLICY_NAME;

/** Whether a client on a Unix socket may connect at all, by its uid, as
 *  policy_admit_uid() answers.
 */
enum policy_admit {
	POLICY_ADMIT,        /**< The client may connect, and policy_grant()
	                          decides what it may do. */
	POLICY_UNMAPPED,     /**< The client is refused.  Its uid is
	                          4294967295, which a socket without
	                          credentials reports, or the kernel's overflow
	                          uid, which the kernel reports for every uid
	                          that the user namespace of askai cannot
	                          map. */
	POLICY_SUBORDINATE,  /**< The client is refused.  Its uid is in a
	                          range of /etc/subuid, so the user that the
	                          range is delegated to can use the uid in a
	                          user namespace. */
};

/** The answer of policy_grant(). */
enum policy_verdict {
	POLICY_GRANTED,     /**< the client may connect */
	POLICY_REFUSED,     /**< an unknown token, or no role for a client
	                         without one: HTTP 401 */
	POLICY_UNIX_TOKEN,  /**< a token on a Unix socket, which the policy
	                         does not accept there: HTTP 400 */
};

/** The type of a function that returns the tools that the upstream may call
 *  in the turns of an ask call, for a client whose role grants @p tools and
 *  lets the upstream call @p upstream: the tools of the role's member
 *  "upstream", or else the role's tools.  @p ctx is the context that the
 *  caller of policy_log() passes.
 */
typedef tool_mask policy_lent_fn (void const *ctx,
                                  tool_mask   tools,
                                  tool_mask   upstream);

/** The user of a Unix socket's client, as the kernel says (SO_PEERCRED,
 *  SO_PEERGROUPS).
 */
struct policy_peer {
	uint32_t const *groups;     /**< its groups, sorted (policy_sort_ids()) */
	uint64_t        principal;  /**< the principal of its uid
	                                 (policy_principal_uid()) */
	uint64_t        n_groups;   /**< the number of groups, at most
	                                 UINT32_MAX, in a uint64_t that fills
	                                 the padding at the end of the struct */
};

/** What a client may see and do, and whose share of the upstream its calls
 *  take.
 */
struct policy_grant {
	tool_mask        tools;      /**< the tools it may list and call */
	tool_mask        upstream;   /**< the tools that the upstream may call in
	                                  the turns of its ask calls */
	struct tool_text who;        /**< the client for the log, at most
	                                  POLICY_WHO - 1 bytes: "identity NAME"
	                                  for a client that sent a token, "role
	                                  NAME" for another client on a Unix
	                                  socket, or "anonymous KIND (role
	                                  NAME)" for another client over TCP.
	                                  It is empty for a client without a
	                                  token under askai's own policy.  The
	                                  text is in the policy's memory. */
	uint64_t         principal;  /**< the principal of its identity or of
	                                  its Unix user, never 0, or 0 for an
	                                  anonymous client over TCP */
	uint64_t         metrics;    /**< bool: it may read the metrics; widened
	                                  over the padding */
};

/**
 * Creates the policy that the configuration file's object @p file
 * describes.  If @p file is nullptr or has none of the policy's members,
 * the function creates askai's own policy.
 *
 * @p tools has the names of the @p n_tools tools that a role can grant, at
 * most TOOL_MAX.  Bit i of a tool mask stands for the tool tools[i].  An
 * element is nullptr if askai does not offer that tool.  @p allowed is the
 * mask of the tools that a client may get at all: a role may name the
 * others, but grants them to no client.  @p upstream is the mask of the
 * tools that askai can offer to the upstream.
 *
 * The function reads the subordinate uids of /etc/subuid, which
 * policy_admit_uid() refuses, and the kernel's overflow uid and gid.  If
 * "unix-groups" names a group, it also reads /etc/subgid.  It refuses a
 * group in "unix-groups" whose gid is subordinate or the overflow gid.
 *
 * Returns the policy with one reference.  On failure it returns nullptr and
 * stores the error number in @p *e.  The error number is ENOMEM, EINVAL if
 * the file is wrong, EIO if HMAC-SHA-256 fails as the function computes
 * the principals of the identities, or the error that kept /etc/subuid or
 * /etc/subgid from being read.  For any error but ENOMEM, the function
 * writes a message of at most @p len bytes to @p why.
 */
extern struct policy *
policy_create (struct json_object *file,
               char const *const   tools[],
               size_t              n_tools,
               tool_mask           allowed,
               tool_mask           upstream,
               char               *why,
               size_t              len,
               int                *e);

/** Another reference to @p policy, which a reload does not change, but
 *  replaces.
 */
extern struct policy *
policy_get (struct policy *policy);

/** Let go of a reference to @p policy, if not nullptr: the last frees it. */
extern void
policy_put (struct policy *policy);

/**
 * Logs @p policy, which policy_create() made from @p file.  @p tools has
 * the names of the @p n_tools tools of askai, at most TOOL_MAX, by id.  The
 * first @p n_granted of them are those that policy_create() got, which a
 * role can grant.  @p lent, with @p ctx, returns the tools that the
 * upstream may call for a role, and is nullptr if askai lends the upstream
 * no tool.
 *
 * The function logs at LOG_INFO, as lists with one item a line.  For
 * askai's own policy, it logs whether ASKAI_TOKEN is set, what each kind of
 * client may do, and the tools that the upstream may call if @p lent is not
 * nullptr.  For the policy of the file, it logs each role with the tools
 * that its clients may call, the tools that the upstream may call in their
 * ask calls if @p lent is not nullptr, and whether they may read the
 * metrics; each identity with its role; each Unix group with its role; and
 * the role of each kind of anonymous client.  Then it logs at LOG_WARN a
 * list of the first @p n_granted tools that a client may get, but that no
 * role grants and no role lends to the upstream.  For both kinds of
 * policy, it ends with the list of the uids that policy_admit_uid()
 * refuses, after a warning at LOG_WARN for an overflow id that it could
 * not read.  The lines have no rate limit.
 */
extern void
policy_log (struct policy const *policy,
            struct json_object  *file,
            char const *const    tools[],
            size_t               n_tools,
            size_t               n_granted,
            policy_lent_fn      *lent,
            void const          *ctx);

/**
 * Decides what a client may do.  The client sends the bearer token of
 * @p len bytes at @p token, or none if @p token is nullptr, and connects
 * as @p kind.  On a Unix socket, @p peer is its user.
 *
 * Over TCP, a token counts if the policy has any token, and otherwise the
 * role of @p kind in "anonymous" applies.  On a Unix socket, a client
 * without a token gets the role of its first group in "unix-groups", or
 * else the role of "unix" in "anonymous".  A token on a Unix socket is
 * accepted only under askai's own policy with ASKAI_TOKEN.  It then grants
 * the role of ASKAI_TOKEN, and the principal stays that of the Unix user.
 *
 * Returns POLICY_GRANTED with the grant in @p *grant, or why the client
 * may not connect.
 */
extern enum policy_verdict
policy_grant (struct policy const      *policy,
              char const               *token,
              size_t                    len,
              enum policy_kind          kind,
              struct policy_peer const *peer,
              struct policy_grant      *grant);

/** Sort the @p n ids at @p ids, such as a peer's groups, as policy_grant()
 *  looks them up.
 */
extern void
policy_sort_ids (uint32_t *ids,
                 size_t    n);

/**
 * Decides whether a client on a Unix socket whose uid is @p uid, as
 * SO_PEERCRED reports it, may connect at all.  The answer does not depend
 * on the roles: a refused uid is refused under every policy, askai's own
 * included.  A null @p policy admits no uid, and the answer is then
 * POLICY_UNMAPPED.
 */
extern enum policy_admit
policy_admit_uid (struct policy const *policy,
                  uint32_t             uid);

/**
 * Sets the key of the principals of @p policy to @p key, and computes the
 * principals of its identities again.  policy_create() computes them with
 * a key of zero bytes, so the caller sets the real key before anyone else
 * takes a reference to @p policy.  Returns 0, EINVAL if an argument is
 * nullptr, EBUSY if @p policy has more than one reference, or EIO if
 * HMAC-SHA-256 fails.
 */
extern int
policy_key (struct policy       *policy,
            unsigned char const  key[POLICY_KEY]);

/**
 * Returns the principal of the calls of a client on a Unix socket whose uid
 * is @p uid, with the key of @p policy, and stores 0 in @p *e.  On failure
 * it returns 0 and stores EINVAL in @p *e if @p policy is nullptr, or EIO
 * if HMAC-SHA-256 fails.
 */
extern uint64_t
policy_principal_uid (struct policy const *policy,
                      uint32_t             uid,
                      int                 *e);

#endif /* TWINCEPTION_SRC_POLICY_H_ */
