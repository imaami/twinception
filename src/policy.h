/** @file Identities and roles (design section 9, S7): who a client is, by
 *  the bearer token it shows, or by how it connects when it shows none, and
 *  what the role it is given lets it see and do.
 *
 * The configuration file's "roles" name what each role may see and do: the
 * tools it may list and call, and whether it may read the metrics.  Its
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

/** The user of a Unix socket's client, as the kernel says (SO_PEERCRED,
 *  SO_PEERGROUPS). */
struct policy_peer {
	uint32_t const *groups;    /**< its groups, sorted */
	uint32_t        n_groups;
	uint32_t        uid;
};

/** What a client may see and do, and whose share of the upstream its calls
 *  take. */
struct policy_grant {
	tool_mask tools;      /**< the tools it may list and call */
	uint64_t  principal;  /**< its identity's, never 0; 0 if anonymous */
	uint64_t  metrics;    /**< bool: it may read the metrics; widened over
	                           the padding */
};

/**
 * The policy of @p file, the configuration file's object, or askai's own if
 * that is nullptr or names none, for the @p n_tools tools, at most TOOL_MAX,
 * that @p tools names: bit i is tool i, nullptr for one askai does not
 * offer; a reference to it.  nullptr,
 * with the error in @p *e: ENOMEM, or EINVAL with @p why, of @p len bytes,
 * saying what is wrong.
 */
extern struct policy *
policy_create (struct json_object *file,
               char const *const   tools[],
               size_t              n_tools,
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

#endif /* TWINCEPTION_SRC_POLICY_H_ */
