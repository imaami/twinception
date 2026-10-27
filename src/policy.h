/** @file Identities and roles (design section 9, S7): who a client is, by
 *  the bearer token it shows, or by how it connects when it shows none, and
 *  what the role it is given lets it see and do.
 *
 * The configuration file's "roles" name what each role may see and do: the
 * tools it may list and call, and whether it may read the metrics.  A
 * role's member "upstream" lists the tools that the upstream may call in
 * the turns of the role's ask calls.  Without it, the upstream may call
 * the tools that the role's clients may call.  Its
 * "identities" name who may connect, each with a role and the tokens that
 * prove it, each a SHA-256 digest ("sha256:HEX") or an environment variable
 * that holds the token ("env:NAME"), or the Unix users ("unix-users", names
 * or uids) it is when it connects over a Unix socket.  Its "unix-groups"
 * give the other users of a Unix socket the role of the first of their
 * groups it names; its "anonymous" names the role, if any, of a client that
 * shows no token, by how it connects.  A token no identity has is refused,
 * as is a client without one that no role is named for; a policy that knows
 * no tokens takes every client as one that shows none.  A Unix socket's
 * user is the principal of its calls but for an identity's.  Users and
 * groups are looked up as the policy is made, which may block.  A file
 * without those sections keeps askai's own policy: with ASKAI_TOKEN set,
 * clients must show it, and may do everything; else a client on loopback
 * that names a loopback host, or over a Unix socket, may do everything, and
 * any other may call the tools.
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

/** Bytes of the longest name of a client in the log, struct policy_grant's
 *  member who, with a terminating NUL. */
static constexpr size_t POLICY_WHO = sizeof "anonymous loopback (role )" + POLICY_NAME;

/** The type of a function that returns the tools that the upstream may call
 *  in the turns of an ask call, for a client whose role grants @p tools and
 *  lets the upstream call @p upstream: the tools of the role's member
 *  "upstream", or else the role's tools.  @p ctx is the context that the
 *  caller of policy_log() passes. */
typedef tool_mask policy_lent_fn (void const *ctx,
                                  tool_mask   tools,
                                  tool_mask   upstream);

/** The user of a Unix socket's client, as the kernel says (SO_PEERCRED,
 *  SO_PEERGROUPS). */
struct policy_peer {
	uint32_t const *groups;    /**< its groups, sorted (policy_sort_ids()) */
	uint32_t        n_groups;
	uint32_t        uid;
};

/** What a client may see and do, and whose share of the upstream its calls
 *  take. */
struct policy_grant {
	tool_mask        tools;      /**< the tools it may list and call */
	tool_mask        upstream;   /**< the tools that the upstream may call in
	                                  the turns of its ask calls */
	struct tool_text who;        /**< the client for the log, at most
	                                  POLICY_WHO - 1 bytes: "identity NAME",
	                                  "role NAME" for a member of a Unix
	                                  group, or "anonymous KIND (role
	                                  NAME)"; empty for a client without an
	                                  identity under askai's own policy.  The
	                                  text is in the policy's memory. */
	uint64_t         principal;  /**< its identity's, never 0; 0 if
	                                  anonymous */
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
 * Returns the policy with one reference.  On failure it returns nullptr and
 * stores the error number in @p *e.  The error number is ENOMEM, or EINVAL
 * if the file is wrong.  For EINVAL the function writes a message of at
 * most @p len bytes to @p why.
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
 *  replaces. */
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
 * role grants and no role lends to the upstream.  The lines have no rate
 * limit.
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
 * The grant, in @p *grant, of a client that shows the bearer token of
 * @p len bytes at @p token, or none if that is nullptr, and connects as
 * @p kind, as the user @p peer if over a Unix socket; false if it may not
 * connect.
 */
extern bool
policy_grant (struct policy const      *policy,
              char const               *token,
              size_t                    len,
              enum policy_kind          kind,
              struct policy_peer const *peer,
              struct policy_grant      *grant);

/** Sort the @p n ids at @p ids, such as a peer's groups, as policy_grant()
 *  looks them up. */
extern void
policy_sort_ids (uint32_t *ids,
                 size_t    n);

#endif /* TWINCEPTION_SRC_POLICY_H_ */
