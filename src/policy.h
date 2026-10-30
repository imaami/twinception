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
 * A token that no identity has is refused.  A client without a token is
 * refused if no role is named for it.  A policy without tokens treats
 * every client as one that sends no token.  The principal of the calls of
 * a client on a Unix socket is derived from its uid, unless it sends the
 * token of an identity.  Group names are looked up through NSS when the
 * policy is made, which may block.
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

/** Bytes of the longest name of a client in the log, struct policy_grant's
 *  member who, with a terminating NUL.
 */
static constexpr size_t POLICY_WHO = sizeof "anonymous loopback (role )" + POLICY_NAME;

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
	uint32_t const *groups;    /**< its groups, sorted (policy_sort_ids()) */
	uint32_t        n_groups;
	uint32_t        uid;
};

/** What a client may see and do, and whose share of the upstream its calls
 *  take.
 */
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
 *  looks them up.
 */
extern void
policy_sort_ids (uint32_t *ids,
                 size_t    n);

#endif /* TWINCEPTION_SRC_POLICY_H_ */
