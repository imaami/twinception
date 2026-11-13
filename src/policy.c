/** @file policy.c
 *
 * Identities and roles.
 */
#define _GNU_SOURCE /* mempcpy() */
#include <errno.h>
#include <grp.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "jb.h"
#include "log.h"
#include "policy.h"
#include "range.h"
#include "ref.h"
#include "registry.h"
#include "util.h"

enum { DIGEST = SHA256_DIGEST_LENGTH };

/* The bit of a principal that marks an identity. A Unix user's principal
 * has it clear, and no principal has bit 63 set, so the principals of the
 * two kinds differ, and none has the bit LEDGER_ANONYMOUS (src/ledger.h).
 */
#define POLICY_IDENTITY (UINT64_C(1) << 62)

/* The index of no role, of a client that the policy refuses. */
#define POLICY_NO_ROLE SIZE_MAX

/* The members of the configuration file that make the policy. */
enum policy_part : uint8_t {
	POLICY_ROLES,
	POLICY_IDENTITIES,
	POLICY_GROUPS,
	POLICY_ANONYMOUS,
	POLICY_PARTS
};

static struct cstr_view const policy_parts[POLICY_PARTS] = {
	[POLICY_ROLES]      = CSTR_VIEW_INIT("roles"),
	[POLICY_IDENTITIES] = CSTR_VIEW_INIT("identities"),
	[POLICY_GROUPS]     = CSTR_VIEW_INIT("unix-groups"),
	[POLICY_ANONYMOUS]  = CSTR_VIEW_INIT("anonymous"),
};

/* The members of "anonymous", by the kind of client. */
static struct cstr_view const policy_kinds[POLICY_KINDS] = {
	[POLICY_LOOPBACK] = CSTR_VIEW_INIT("loopback"),
	[POLICY_REMOTE]   = CSTR_VIEW_INIT("remote"),
	[POLICY_UNIX]     = CSTR_VIEW_INIT("unix"),
};

/* A token, by its digest, and the identity it proves. */
struct policy_token {
	unsigned char    digest[DIGEST];
	struct cstr_view who;        /* "identity NAME", for the log */
	uint64_t         principal;
	size_t           role;
	uint64_t         flags;      /* POLICY_AGENT and POLICY_OPERATOR of
	                                enum policy_flag */
};

/* A Unix group, by gid first, and its role. */
struct policy_gid {
	uint32_t gid;
	uint32_t role;
};

/* The files that list the ranges of subordinate ids that are delegated to
 * users for user namespaces (newuidmap, newgidmap), and the files of the
 * ids that the kernel reports for a uid or a gid that the user namespace of
 * askai cannot map.
 */
static char const POLICY_SUBUID[] = "/etc/subuid";
static char const POLICY_SUBGID[] = "/etc/subgid";
static char const POLICY_OVERFLOWUID[] = "/proc/sys/kernel/overflowuid";
static char const POLICY_OVERFLOWGID[] = "/proc/sys/kernel/overflowgid";

/* One block contains a policy followed by its roles, tokens, groups,
 * subordinate uids and the names of its clients for the log, in that
 * order, so that each is aligned.
 */
struct policy {
	tool_mask            allowed;                  /* the tools that the
	                                                  roles may grant */
	_Atomic uint64_t     refs;
	struct policy_token *tokens;                   /* by digest */
	struct policy_grant *roles;                    /* each with principal 0 */
	struct policy_gid   *groups;                   /* in the order of the
	                                                  configuration. A user
	                                                  receives the role of
	                                                  the first group that
	                                                  contains the user. */
	struct range        *subuids;                  /* the ranges of
	                                                  /etc/subuid, sorted and
	                                                  merged */
	size_t               n_tokens;
	size_t               n_roles;
	size_t               n_groups;
	size_t               n_subuids;
	size_t               anonymous[POLICY_KINDS];  /* a role, or
	                                                  POLICY_NO_ROLE */
	struct cstr_view     strangers[POLICY_KINDS];  /* the names of anonymous
	                                                  TCP clients for the
	                                                  log, "anonymous KIND
	                                                  (role NAME)"; empty in
	                                                  askai's own policy */
	unsigned char        key[POLICY_KEY];          /* The key that the
	                                                  principals are
	                                                  computed with. It is
	                                                  all zero until
	                                                  policy_key() sets
	                                                  it. */
	uint32_t             overflow_uid;             /* the kernel's overflow
	                                                  uid, which
	                                                  policy_admit_uid()
	                                                  refuses */
	uint32_t             overflow_gid;             /* the kernel's overflow
	                                                  gid, which unix-groups
	                                                  may not name */
	int                  overflow_errors[2];       /* The errors of reading
	                                                  the overflow uid and
	                                                  gid, or 0 for an id
	                                                  that was read. An id
	                                                  that was not read is
	                                                  65534. */
	uint64_t             unix_token;               /* Nonzero if a client
	                                                  on a Unix socket may
	                                                  send ASKAI_TOKEN. It
	                                                  is a bool, widened to
	                                                  fill the padding at
	                                                  the end of the
	                                                  struct. */
};

static_assert(sizeof "anonymous loopback (role )" + POLICY_NAME <= POLICY_WHO);
static_assert(alignof (struct policy_grant) <= alignof (struct policy) &&
              alignof (struct policy_token) <= alignof (struct policy_grant) &&
              alignof (struct policy_gid) <= alignof (struct policy_token) &&
              alignof (struct range) <= alignof (struct policy_gid));

/* The input of policy_create() and the buffer for its error message. */
struct policy_build {
	tool_mask              upstream;  /* the tools that askai can offer to
	                                     the upstream */
	struct policy         *p;
	struct json_object    *roles;     /* the member roles of the file, or
	                                     nullptr */
	struct registry const *reg;
	char                  *why;
	char                  *names;     /* the place in the policy's block
	                                     where the next name for the log is
	                                     written */
	struct range          *subgids;   /* the ranges of /etc/subgid, sorted
	                                     and merged, which policy_create()
	                                     frees at the end */
	size_t                 len;
	size_t                 n_subgids;
	struct buf             nss;       /* the storage of group lookups, which
	                                     policy_create() frees at the end */
};

/* Say what is wrong; returns EINVAL. */
#define policy_build_fail(b, ...) util_refuse((b)->why, (b)->len, __VA_ARGS__)

/* The length of @p name, a role's or an identity's; 0 if it is not a
 * valid name.
 */
static size_t
policy_name_len (char const *name)
{
	size_t n = strspn(name, UTIL_ALNUM "_.@-");
	return n <= POLICY_NAME && !name[n] ? n : 0;
}

/* Compute in @p *principal the principal of the anchor @p text of @p len
 * bytes, "uid:N" or "identity:NAME", with the key of @p policy. It is the
 * first 8 bytes of HMAC-SHA-256 as a big-endian number, with bits 63 and 62
 * cleared and @p tag added, and 1 in place of 0, which means anonymous.
 * Returns 0, or EIO if HMAC() fails.
 */
static int
policy_principal_of (struct policy const *policy,
                     uint64_t             tag,
                     char const          *text,
                     size_t               len,
                     uint64_t            *principal)
{
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int n = 0;
	if (!HMAC(EVP_sha256(), policy->key, (int)sizeof policy->key,
	          (unsigned char const *)text, len, md, &n) || n < sizeof *principal)
		return EIO;
	uint64_t h = util_be_get(md, sizeof *principal);
	h = (h & (POLICY_IDENTITY - 1)) | tag;
	*principal = h + !h;
	return 0;
}

/* Compute in @p *principal the principal of the identity in @p who,
 * "identity NAME", with the key of @p policy. The anchor is @p who with a
 * colon in place of the space. Returns 0, EINVAL if @p who is in another
 * form or NAME is longer than POLICY_NAME, or EIO if HMAC() fails.
 */
static int
policy_principal_identity (struct policy const *policy,
                           struct cstr_view     who,
                           uint64_t            *principal)
{
	static char const prefix[] = "identity ";
	char anchor[sizeof prefix - 1 + POLICY_NAME];
	if (!cstr_view_starts(who, cstr_view_n(prefix, sizeof prefix - 1)) ||
	    who.len > sizeof anchor)
		return EINVAL;
	memcpy(anchor, who.data, who.len);
	anchor[sizeof prefix - 2] = ':';
	return policy_principal_of(policy, POLICY_IDENTITY, anchor, who.len, principal);
}

#undef POLICY_IDENTITY

/* Compute the principal of each token of @p policy from the name of its
 * identity, with the key of @p policy. Returns 0, or the error of
 * policy_principal_identity().
 */
static int
policy_principals (struct policy *policy)
{
	for (size_t i = 0; i < policy->n_tokens; ++i) {
		struct policy_token *t = &policy->tokens[i];
		int e = policy_principal_identity(policy, t->who, &t->principal);
		if (e)
			return e;
	}
	return 0;
}

/* Copy the @p n texts @p parts, one after another, into the block of the
 * policy, and return the copy, which is the name of a client in the log.
 */
static struct cstr_view
policy_build_who (struct policy_build    *b,
                  size_t                  n,
                  struct cstr_view const  parts[static n])
{
	char *who = b->names;
	for (size_t i = 0; i < n; ++i)
		b->names = mempcpy(b->names, parts[i].data, parts[i].len);
	return cstr_view_n(who, (size_t)(b->names - who));
}

/* Return the index of the role whose name is the string @p name, or
 * POLICY_NO_ROLE if there is no such role.
 */
static size_t
policy_build_role (struct policy_build const *b,
                   struct json_object        *name)
{
	char const *s = jb_view(name).data;
	if (!s || !b->roles)
		return POLICY_NO_ROLE;
	size_t i = 0;
	struct json_object_iter it;
	json_object_object_foreachC(b->roles, it) {
		if (!strcmp(it.key, s))
			return i;
		++i;
	}
	return POLICY_NO_ROLE;
}

/* Append a token of @p digest for the identity @p who of @p role, with the
 * flags @p flags of enum policy_flag. policy_principals() computes its
 * principal.
 */
static void
policy_token_add (struct policy       *p,
                  unsigned char const *digest,
                  struct cstr_view     who,
                  size_t               role,
                  uint64_t             flags)
{
	struct policy_token *t = &p->tokens[p->n_tokens++];
	memcpy(t->digest, digest, DIGEST);
	t->who = who;
	t->role = role;
	t->flags = flags;
}

static int
policy_by_digest (void const *a,
                  void const *b)
{
	return memcmp(a, b, DIGEST);
}

/* Orders two uint32_t ids, for qsort() and bsearch(). */
static int
policy_by_id (void const *a,
              void const *b)
{
	uint32_t x;
	uint32_t y;
	memcpy(&x, a, sizeof x);
	memcpy(&y, b, sizeof y);
	return (x > y) - (x < y);
}

/* @p text as a decimal number below UINT32_MAX, in @p *id, if it is one. */
static bool
policy_number (struct cstr_view  text,
               uint32_t         *id)
{
	uint64_t n;
	if (util_number(text.data, text.len, UINT32_MAX - 1, &n))
		return false;
	*id = (uint32_t)n;
	return true;
}

/* Read the number at @p s, a field of /etc/subuid or /etc/subgid, into
 * @p *n. newuidmap and newgidmap read the fields with strtoul() and base
 * 0, so this function reads them with strtoull() and base 0 too: decimal,
 * hexadecimal after "0x", or octal after "0". Returns the end of the
 * number, or nullptr if there is no number, if it has a minus sign, or if
 * it does not fit in 64 bits.
 */
static char const *
policy_range_number (char const *s,
                     uint64_t   *n)
{
	char *end;
	/* strtoull() leaves errno as it was on success. */
	errno = 0;
	unsigned long long v = strtoull(s, &end, 0);
	if (end == s || errno || memchr(s, '-', (size_t)(end - s)))
		return nullptr;
	*n = v;
	return end;
}

/* Parse @p line of /etc/subuid or /etc/subgid, "OWNER:START:COUNT", into
 * @p *r. A range that would end above UINT32_MAX ends at UINT32_MAX,
 * because some versions of newuidmap accept a COUNT that large. Returns
 * false for a line without that form, with a START above UINT32_MAX, or
 * with a COUNT of 0. The parser accepts text after COUNT, which newuidmap
 * refuses. For such a line askai refuses uids that newuidmap does not
 * map, so askai can refuse more clients than needed, but never fewer.
 */
static bool
policy_range_parse (char const   *line,
                    struct range *r)
{
	uint64_t start = 0;
	uint64_t count = 0;
	char const *s = strchr(line, ':');
	if (!s || !(s = policy_range_number(s + 1, &start)) || *s != ':' ||
	    !policy_range_number(s + 1, &count) || !count || start > UINT32_MAX)
		return false;
	/* The number of ids above START, up to UINT32_MAX. Neither this
	 * subtraction nor the addition below can wrap around. */
	uint64_t room = UINT32_MAX - start;
	*r = range((uint32_t)start,
	           (uint32_t)(start + (count - 1 < room ? count - 1 : room)));
	return true;
}

/* Write to b->why that the file @p path cannot be read because of @p e,
 * unless @p e is ENOMEM. Returns @p e.
 */
static int
policy_build_unreadable (struct policy_build *b,
                         char const          *path,
                         int                  e)
{
	if (e != ENOMEM)
		util_format(b->why, b->len, "cannot read %s: %s", path, strerror(e));
	return e;
}

/* Read the ranges of subordinate ids in @p path, /etc/subuid or
 * /etc/subgid, into @p *p_ranges and @p *p_n, sorted by their first id and
 * merged where they overlap or touch. Lines that policy_range_parse()
 * refuses are skipped. If the file does not exist, @p *p_n is 0. Returns
 * 0, ENOMEM, or the error of reading the file, with the reason in b->why.
 */
static int
policy_build_ranges (struct policy_build  *b,
                     char const           *path,
                     struct range        **p_ranges,
                     size_t               *p_n)
{
	*p_ranges = nullptr;
	*p_n = 0;
	FILE *f = fopen(path, "re");
	if (!f)
		return errno == ENOENT ? 0 : policy_build_unreadable(b, path, errno);
	int e = 0;
	struct range *ranges = nullptr;
	size_t n = 0;
	size_t room = 0;
	char *line = nullptr;
	size_t size = 0;
	while (getline(&line, &size, f) >= 0) {
		struct range r = {};
		if (!policy_range_parse(line, &r))
			continue;
		e = util_grow(ranges, room, n + 1);
		if (e)
			break;
		ranges[n++] = r;
	}
	/* getline() also fails at the end of the file, which is no error. */
	if (!e && !feof(f))
		e = errno ? errno : EIO;
	free(line);
	line = nullptr;
	(void)fclose(f);
	if (e) {
		free(ranges);
		return policy_build_unreadable(b, path, e);
	}
	*p_ranges = ranges;
	*p_n = range_merge(ranges, n);
	return 0;
}

/* Whether @p id lies in one of the @p n sorted and disjoint @p ranges. */
static bool
policy_ranges_have (struct range const *ranges,
                    size_t              n,
                    uint32_t            id)
{
	size_t lo = 0;
	size_t hi = n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (ranges[mid].last < id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < n && ranges[lo].first <= id;
}

/* Read the decimal id in @p path, /proc/sys/kernel/overflowuid or
 * overflowgid, into @p *id. If the file cannot be read, @p *id is 65534,
 * the kernel's default. Returns 0, or the error that kept the file from
 * being read, EILSEQ if it holds no id.
 */
static int
policy_read_overflow (char const *path,
                      uint32_t   *id)
{
	*id = 65534;
	char text[16];
	int e = util_read_text(path, text, sizeof text);
	if (e)
		return e;
	return policy_number(cstr_view_n(text, strcspn(text, "\n")), id) ? 0 : EILSEQ;
}

/* Bytes of the buffer for one group entry from NSS, at most. A group
 * entry lists the names of its members, so the entry of a large directory
 * group can take megabytes.
 */
#define POLICY_ENTRY_MAX ((size_t)64 << 20)

/* Look up the Unix group @p name of unix-groups, a gid in decimal or a
 * group name that NSS resolves, and store its gid in @p *gid. The lookup
 * storage is b->nss. It starts at the smallest size class of buf.h and
 * doubles while getgrnam_r() reports ERANGE, up to POLICY_ENTRY_MAX bytes.
 * The message is "no group" only if the lookup succeeded and found no
 * such group. Returns 0, ENOMEM, or EINVAL with the reason in b->why.
 */
static int
policy_build_gid (struct policy_build *b,
                  char const          *name,
                  uint32_t            *gid)
{
	if (policy_number(cstr_view(name), gid))
		return 0;
	struct group gr;
	struct group *r = nullptr;
	struct span room = {};
	int e = buf_reserve(&b->nss, 1, POLICY_ENTRY_MAX, &room);
	while (!e && (e = getgrnam_r(name, &gr, (char *)room.ptr, room.len, &r)) == ERANGE)
		e = buf_reserve(&b->nss, room.len + 1, POLICY_ENTRY_MAX, &room);
	if (e == ENOBUFS)
		return policy_build_fail(b, "unix-groups: the entry of group %s is larger "
		                         "than %zu MiB; give its gid instead", name,
		                         POLICY_ENTRY_MAX >> 20);
	if (e == ENOMEM)
		return e;
	if (e)
		return policy_build_fail(b, "unix-groups: cannot look up group %s: %s",
		                         name, strerror(e));
	if (!r)
		return policy_build_fail(b, "unix-groups: no group %s", name);
	*gid = (uint32_t)r->gr_gid;
	return 0;
}

#undef POLICY_ENTRY_MAX

/* Read @p spec, a string "sha256:HEX" or "env:NAME", into @p digest. */
static int
policy_build_token (struct policy_build *b,
                    char const          *who,
                    struct json_object  *spec,
                    unsigned char       *digest)
{
	static char const sha[] = "sha256:";
	static char const env[] = "env:";
	char const *s = jb_view(spec).data;
	if (s && !strncmp(s, sha, sizeof sha - 1)) {
		char const *hex = s + sizeof sha - 1;
		if (strlen(hex) != 2 * DIGEST || util_unhex(digest, hex, DIGEST))
			return policy_build_fail(b, "identity %s: sha256: needs %d hex digits",
			                         who, 2 * DIGEST);
		return 0;
	}
	if (!s || strncmp(s, env, sizeof env - 1))
		return policy_build_fail(b, "identity %s: a token is sha256:HEX or env:NAME",
		                         who);
	char const *name = s + sizeof env - 1;
	if (!*name)
		return policy_build_fail(b, "identity %s: env: is not set", who);
	char const *token = getenv(name);
	if (!token || !*token)
		return policy_build_fail(b, "identity %s: %s is not set", who, name);
	SHA256((unsigned char const *)token, strlen(token), digest);
	return 0;
}

/* The mask of the tools of @p reg that a role can grant. */
static tool_mask
policy_all (struct registry const *reg)
{
	return reg->n_granted < TOOL_MAX ? ((tool_mask)1 << reg->n_granted) - 1
	                                 : ~(tool_mask)0;
}

/* Write the reason why role @p name may not name @p v: askai offers no such
 * tool, or with @p up lends no such tool to the upstream. The reason lists
 * the tools that askai offers or lends. Returns EINVAL.
 */
static int
policy_build_no_tool (struct policy_build *b,
                      char const          *name,
                      struct json_object  *v,
                      bool                 up)
{
	tool_mask const mask = up ? b->upstream : policy_all(b->reg);
	if (up && !mask)
		return policy_build_fail(b, "role %s: askai offers no tool %s to the "
		                         "upstream; it lends no tool", name,
		                         json_object_to_json_string(v));
	return policy_build_fail(b, "role %s: askai offers no tool %s%s; the tools %s"
	                         "are %s", name, json_object_to_json_string(v),
	                         up ? " to the upstream" : "", up ? "that it lends " : "",
	                         registry_names(b->reg, mask).data);
}

/* Read the list of tool names @p list of role @p name into @p r. @p key is
 * the name of the member of the list, "tools" or "upstream", and @p up is
 * true for "upstream". The member "upstream" may list only the tools that
 * askai can offer to the upstream.
 */
static int
policy_build_tools (struct policy_build *b,
                    struct policy_grant *r,
                    char const          *name,
                    char const          *key,
                    struct json_object  *list,
                    bool                 up)
{
	if (!json_object_is_type(list, json_type_array))
		return policy_build_fail(b, "role %s: %s must be an array of names",
		                         name, key);
	tool_mask *mask = up ? &r->upstream : &r->tools;
	tool_mask offered = up ? b->upstream : ~(tool_mask)0;
	size_t len = json_object_array_length(list);
	for (size_t i = 0; i < len; ++i) {
		struct json_object *v = json_object_array_get_idx(list, i);
		uint32_t k = registry_find(b->reg, jb_view(v), b->reg->n_granted);
		if (k == b->reg->n_granted || !(offered >> k & 1))
			return policy_build_no_tool(b, name, v, up);
		*mask |= (tool_mask)1 << k;
	}
	return 0;
}

/* Read the member @p key with the value @p val of role @p name into @p r. */
static int
policy_build_member (struct policy_build *b,
                     struct policy_grant *r,
                     char const          *name,
                     char const          *key,
                     struct json_object  *val)
{
	bool up = !strcmp(key, "upstream");
	if (up || !strcmp(key, "tools"))
		return policy_build_tools(b, r, name, key, val, up);
	if (strcmp(key, "metrics"))
		return policy_build_fail(b, "role %s: unknown member %s", name, key);
	if (!json_object_is_type(val, json_type_boolean))
		return policy_build_fail(b, "role %s: metrics must be true or false", name);
	r->flags = json_object_get_boolean(val) ? POLICY_METRICS : 0;
	return 0;
}

/* Read role @p name with the members @p members into @p r. */
static int
policy_build_role_one (struct policy_build *b,
                       struct policy_grant *r,
                       char const          *name,
                       struct json_object  *members)
{
	size_t len = policy_name_len(name);
	if (!len)
		return policy_build_fail(b, "invalid role name: %s", name);
	if (!json_object_is_type(members, json_type_object))
		return policy_build_fail(b, "role %s must be an object", name);
	struct cstr_view const parts[] = { CSTR_VIEW("role "), cstr_view_n(name, len) };
	r->who = policy_build_who(b, array_size(parts), parts);
	struct json_object_iter m;
	json_object_object_foreachC(members, m) {
		int e = policy_build_member(b, r, name, m.key, m.val);
		if (e)
			return e;
	}
	/* Without the member "upstream", the upstream may call the tools that
	 * the role's clients may call. */
	if (!json_object_object_get_ex(members, "upstream", nullptr))
		r->upstream = r->tools;
	return 0;
}

/* Builds the roles of b->roles into b->p->roles. b->roles must be nullptr
 * or a JSON object. policy_build_file() tests the type before the call.
 */
static int
policy_build_roles (struct policy_build *b)
{
	if (!b->roles)
		return 0;
	struct policy_grant *r = b->p->roles;
	struct json_object_iter it;
	json_object_object_foreachC(b->roles, it) {
		int e = policy_build_role_one(b, r++, it.key, it.val);
		if (e)
			return e;
	}
	return 0;
}

/* Read the role and the tokens of identity @p who from @p id. */
static int
policy_build_identity (struct policy_build *b,
                       char const          *who,
                       struct json_object  *id)
{
	size_t len = policy_name_len(who);
	if (!len)
		return policy_build_fail(b, "invalid identity name: %s", who);
	if (!json_object_is_type(id, json_type_object))
		return policy_build_fail(b, "identity %s must be an object", who);
	struct json_object *role = nullptr;
	struct json_object *tokens = nullptr;
	struct json_object *agent = nullptr;
	struct json_object_iter m;
	json_object_object_foreachC(id, m) {
		if (!strcmp(m.key, "role"))
			role = m.val;
		else if (!strcmp(m.key, "tokens"))
			tokens = m.val;
		else if (!strcmp(m.key, "agent"))
			agent = m.val;
		else
			return policy_build_fail(b, "identity %s: unknown member %s", who, m.key);
	}
	if (agent && !json_object_is_type(agent, json_type_boolean))
		return policy_build_fail(b, "identity %s: agent must be true or false", who);
	size_t const r = policy_build_role(b, role);
	if (r == POLICY_NO_ROLE)
		return policy_build_fail(b, "identity %s: no role %s", who,
		                         role ? json_object_to_json_string(role) : "named");
	if (tokens && !json_object_is_type(tokens, json_type_array))
		return policy_build_fail(b, "identity %s: tokens must be an array", who);
	size_t n = jb_len(tokens);
	if (!n)
		return policy_build_fail(b, "identity %s needs tokens", who);
	struct cstr_view const parts[] = { CSTR_VIEW("identity "), cstr_view_n(who, len) };
	struct cstr_view name = policy_build_who(b, array_size(parts), parts);
	uint64_t const flags = json_object_get_boolean(agent) ? POLICY_AGENT : 0;
	for (size_t i = 0; i < n; ++i) {
		unsigned char digest[DIGEST];
		int e = policy_build_token(b, who, json_object_array_get_idx(tokens, i), digest);
		if (e)
			return e;
		policy_token_add(b->p, digest, name, r, flags);
	}
	return 0;
}

/* Builds the tokens of @p identities into b->p->tokens and sorts the
 * tokens. @p identities must be nullptr or a JSON object.
 * policy_build_file() tests the type before the call.
 */
static int
policy_build_identities (struct policy_build *b,
                         struct json_object  *identities)
{
	if (!identities)
		return 0;
	struct json_object_iter it;
	json_object_object_foreachC(identities, it) {
		int e = policy_build_identity(b, it.key, it.val);
		if (e)
			return e;
	}
	struct policy *p = b->p;
	qsort(p->tokens, p->n_tokens, sizeof *p->tokens, policy_by_digest);
	for (size_t i = 1; i < p->n_tokens; ++i)
		if (!policy_by_digest(&p->tokens[i - 1], &p->tokens[i]))
			return policy_build_fail(b, "two tokens of the identities are the same");
	return 0;
}

/* Builds the groups of @p groups into b->p->groups. @p groups must be
 * nullptr or a JSON object. policy_build_file() tests the type before the
 * call.
 */
static int
policy_build_groups (struct policy_build *b,
                     struct json_object  *groups)
{
	if (!groups)
		return 0;
	struct policy_gid *g = b->p->groups;
	struct json_object_iter it;
	json_object_object_foreachC(groups, it) {
		size_t const r = policy_build_role(b, it.val);
		if (r == POLICY_NO_ROLE)
			return policy_build_fail(b, "unix-groups: %s: no role %s", it.key,
			                         json_object_to_json_string(it.val));
		int e = policy_build_gid(b, it.key, &g->gid);
		if (e)
			return e;
		/* A process in a user namespace can have such a gid without
		 * being a member of the group, so the gid must not give a role.
		 * The kernel reports the overflow gid for every gid that the user
		 * namespace of askai cannot map. */
		if (g->gid == b->p->overflow_gid)
			return policy_build_fail(b, "unix-groups: %s: gid %" PRIu32 " is the "
			                         "kernel's overflow gid", it.key, g->gid);
		if (policy_ranges_have(b->subgids, b->n_subgids, g->gid))
			return policy_build_fail(b, "unix-groups: %s: gid %" PRIu32 " is a "
			                         "subordinate id of another user (%s)", it.key,
			                         g->gid, POLICY_SUBGID);
		g++->role = r;
		++b->p->n_groups;
	}
	return 0;
}

/* Builds the roles and the names of the anonymous clients in @p anonymous
 * into b->p->anonymous and b->p->strangers. @p anonymous must be nullptr or
 * a JSON object. policy_build_file() tests the type before the call.
 */
static int
policy_build_anonymous (struct policy_build *b,
                        struct json_object  *anonymous)
{
	if (!anonymous)
		return 0;
	struct policy *p = b->p;
	struct json_object_iter it;
	json_object_object_foreachC(anonymous, it) {
		size_t k = cstr_view_index(policy_kinds, POLICY_KINDS, cstr_view(it.key));
		if (k == POLICY_KINDS)
			return policy_build_fail(b, "anonymous: unknown member %s", it.key);
		if (!it.val)
			continue;
		size_t const role = policy_build_role(b, it.val);
		if (role == POLICY_NO_ROLE)
			return policy_build_fail(b, "anonymous: %s: no role %s", it.key,
			                         json_object_to_json_string(it.val));
		p->anonymous[k] = role;
		struct cstr_view const parts[] = {
			CSTR_VIEW("anonymous "), policy_kinds[k], CSTR_VIEW(" ("),
			p->roles[role].who, CSTR_VIEW(")")
		};
		p->strangers[k] = policy_build_who(b, array_size(parts), parts);
	}
	return 0;
}

/* Builds the policy of the configuration file into b->p. @p of contains
 * the policy members of the file, indexed by enum policy_part, and each
 * member that is not nullptr must be a JSON object. The function builds the
 * roles, the identities, the groups and the anonymous roles in this order.
 * Returns 0 or the error number of the first step that fails.
 */
static int
policy_build_file (struct policy_build *b,
                   struct json_object **of)
{
	for (size_t i = 0; i < array_size(policy_parts); ++i)
		if (of[i] && !json_object_is_type(of[i], json_type_object))
			return policy_build_fail(b, "%s must be an object", policy_parts[i].data);
	int e = policy_build_roles(b);
	e = e ? e : policy_build_identities(b, of[POLICY_IDENTITIES]);
	e = e ? e : policy_build_groups(b, of[POLICY_GROUPS]);
	return e ? e : policy_build_anonymous(b, of[POLICY_ANONYMOUS]);
}

#undef policy_build_fail

/* Build askai's own policy. With ASKAI_TOKEN, @p token, every client must
 * send the token, also on a Unix socket, and may do everything. Without
 * it, clients on loopback and on a Unix socket may do everything, and other
 * clients may call the tools.
 */
static void
policy_build_own (struct policy_build *b,
                  char const          *token)
{
	struct policy *p = b->p;
	tool_mask all = policy_all(b->reg);
	p->roles[0] = (struct policy_grant){ .tools = all, .upstream = all, .flags = POLICY_METRICS };
	p->roles[1] = (struct policy_grant){ .tools = all, .upstream = all };
	if (!token) {
		p->anonymous[POLICY_LOOPBACK] = 0;
		p->anonymous[POLICY_REMOTE] = 1;
		p->anonymous[POLICY_UNIX] = 0;
		return;
	}
	unsigned char digest[DIGEST];
	SHA256((unsigned char const *)token, strlen(token), digest);
	policy_token_add(p, digest, CSTR_VIEW("identity ASKAI_TOKEN"), 0, POLICY_OPERATOR);
	p->unix_token = true;
}

/* The members of the configuration file's object @p file that make the
 * policy, in @p of by enum policy_part, each nullptr if absent; whether
 * any of them is in @p file, which turns askai's own policy off.
 */
static bool
policy_file (struct json_object  *file,
             struct json_object **of)
{
	bool any = false;
	for (size_t i = 0; i < array_size(policy_parts); ++i)
		any |= json_object_object_get_ex(file, policy_parts[i].data, &of[i]);
	return any;
}

bool
policy_member (char const *key)
{
	return cstr_view_index(policy_parts, array_size(policy_parts), cstr_view(key)) <
	       array_size(policy_parts);
}

struct policy *
policy_create (struct json_object    *file,
               struct registry const *reg,
               tool_mask              allowed,
               tool_mask              upstream,
               char                  *why,
               size_t                 len,
               int                   *e)
{
	struct json_object *of[array_size(policy_parts)];
	bool const from_file = policy_file(file, of);
	struct policy_build b = {
		.upstream = upstream,
		.roles = of[POLICY_ROLES],
		.reg = reg,
		.why = why,
		.len = len
	};
	size_t n_groups = jb_members(of[POLICY_GROUPS]);
	size_t n_ids = jb_members(of[POLICY_IDENTITIES]);
	/* The sizes below are the room in the block of the policy for the
	 * policy of the file. askai's own policy uses two roles, no names, and
	 * the token of ASKAI_TOKEN if that is set. */
	char const *token = nullptr;
	size_t n_tokens = 0;
	size_t n_roles = 2;
	size_t n_names = 0;
	if (from_file) {
		n_roles = jb_members(b.roles);
		/* Room for the longest name of each role, identity and kind of
		 * anonymous client. */
		n_names = n_roles * (sizeof "role " - 1 + POLICY_NAME) +
		          n_ids * (sizeof "identity " - 1 + POLICY_NAME) +
		          POLICY_KINDS * (POLICY_WHO - 1);
	} else {
		char const *env = getenv("ASKAI_TOKEN");
		token = env && *env ? env : nullptr;
		n_tokens = token != nullptr;
	}
	struct json_object_iter it;
	if (n_ids)
		json_object_object_foreachC(of[POLICY_IDENTITIES], it)
			n_tokens += jb_len(jb_get(it.val, "tokens"));
	/* The subordinate uids go into the policy's block, and the
	 * subordinate gids are needed only to check unix-groups. */
	struct range *subuids;
	size_t n_subuids;
	*e = policy_build_ranges(&b, POLICY_SUBUID, &subuids, &n_subuids);
	if (!*e && n_groups)
		*e = policy_build_ranges(&b, POLICY_SUBGID, &b.subgids, &b.n_subgids);
	struct policy *p = nullptr;
	if (!*e && !(p = calloc(1, sizeof *p + n_tokens * sizeof *p->tokens +
	                           n_roles * sizeof *p->roles +
	                           n_groups * sizeof *p->groups +
	                           n_subuids * sizeof *p->subuids + n_names)))
		*e = ENOMEM;
	if (*e) {
		free(subuids);
		free(b.subgids);
		return nullptr;
	}
	atomic_init(&p->refs, 1);
	p->roles = (struct policy_grant *)(p + 1);
	p->tokens = (struct policy_token *)(p->roles + n_roles);
	p->groups = (struct policy_gid *)(p->tokens + n_tokens);
	p->subuids = (struct range *)(p->groups + n_groups);
	p->n_roles = n_roles;
	p->n_subuids = n_subuids;
	if (n_subuids)
		memcpy(p->subuids, subuids, n_subuids * sizeof *subuids);
	free(subuids);
	subuids = nullptr;
	p->overflow_errors[0] = policy_read_overflow(POLICY_OVERFLOWUID, &p->overflow_uid);
	p->overflow_errors[1] = policy_read_overflow(POLICY_OVERFLOWGID, &p->overflow_gid);
	b.names = (char *)(p->subuids + n_subuids);
	for (size_t k = 0; k < POLICY_KINDS; ++k)
		p->anonymous[k] = POLICY_NO_ROLE;
	b.p = p;
	if (from_file)
		*e = policy_build_file(&b, of);
	else
		policy_build_own(&b, token);
	buf_fini(&b.nss);
	free(b.subgids);
	b.subgids = nullptr;
	/* The tokens get the principals of a key of zero bytes, until
	 * policy_key() sets the real key. A failure is reported as EIO, so
	 * that EINVAL always means that the file is wrong. */
	if (!*e && policy_principals(p)) {
		*e = EIO;
		util_format(why, len, "cannot compute the principals of the identities");
	}
	if (*e) {
		policy_put(p);
		return nullptr;
	}
	/* Clients get only the allowed tools. A role without the member
	 * "upstream" lends the upstream its tools as the file lists them. */
	p->allowed = allowed;
	for (size_t i = 0; i < n_roles; ++i)
		p->roles[i].tools &= allowed;
	return p;
}

struct policy *
policy_get (struct policy *policy)
{
	ref_get(&policy->refs);
	return policy;
}

void
policy_put (struct policy *policy)
{
	if (!policy || !ref_put(&policy->refs, 1))
		return;
	free(policy);
}

/* Log the tools of @p reg in @p mask, one a line, the first after @p key,
 * which is padded to @p width columns; or none.
 */
static void
policy_log_tools (char const            *key,
                  int                    width,
                  struct registry const *reg,
                  tool_mask              mask)
{
	char const *first = key;
	for (uint32_t id = 0; id < reg->n_tools; ++id)
		if (mask >> id & 1) {
			LOG_ALL(LOG_INFO, "  %-*s  %s", width, key, reg->tools[id].name.data);
			key = "";
		}
	if (key == first)
		LOG_ALL(LOG_INFO, "  %-*s  none", width, key);
}

/* Log each role of @p policy, the policy of the file, with the tools of
 * @p reg. If @p lent is not nullptr, also log the tools that the upstream
 * may call, which lent() returns with @p ctx. Return the tools that the
 * roles grant or lend.
 */
static tool_mask
policy_log_roles (struct policy const   *policy,
                  struct registry const *reg,
                  policy_lent_fn        *lent,
                  void const            *ctx)
{
	if (!policy->n_roles) {
		LOG_ALL(LOG_INFO, "roles: none");
		return 0;
	}
	tool_mask listed = 0;
	for (size_t i = 0; i < policy->n_roles; ++i) {
		struct policy_grant const *r = &policy->roles[i];
		LOG_ALL(LOG_INFO, "%.*s:", (int)r->who.len, r->who.data);
		policy_log_tools("tools", 14, reg, r->tools);
		tool_mask up = 0;
		if (lent) {
			up = lent(ctx, r->tools, r->upstream);
			policy_log_tools("upstream tools", 14, reg, up);
		}
		LOG_ALL(LOG_INFO, "  metrics         %s", r->flags & POLICY_METRICS ? "yes" : "no");
		listed |= r->tools | up;
	}
	return listed;
}

/* The width of the longest member name of @p obj, an object. */
static int
policy_width (struct json_object *obj)
{
	int width = 0;
	struct json_object_iter it;
	json_object_object_foreachC(obj, it) {
		int len = (int)strlen(it.key);
		width = len > width ? len : width;
	}
	return width;
}

/* Log the identities of the file's policy, @p ids in the file, each with
 * its role.
 */
static void
policy_log_identities (struct json_object *ids)
{
	if (!jb_members(ids)) {
		LOG_ALL(LOG_INFO, "identities: none");
		return;
	}
	int width = policy_width(ids);
	LOG_ALL(LOG_INFO, "identities:");
	struct json_object_iter it;
	json_object_object_foreachC(ids, it) {
		char const *name = json_object_get_string(jb_get(it.val, "role"));
		bool const agent = json_object_get_boolean(jb_get(it.val, "agent"));
		LOG_ALL(LOG_INFO, "  %-*s  role %s%s", width, it.key, name ? name : "none",
		        agent ? ", agent" : "");
	}
}

/* Log the role of each Unix group of the file's policy @p policy, @p groups
 * in the file, for at most the groups of the policy.
 */
static void
policy_log_groups (struct policy const *policy,
                   struct json_object  *groups)
{
	if (!policy->n_groups) {
		LOG_ALL(LOG_INFO, "Unix groups: none");
		return;
	}
	int width = policy_width(groups);
	LOG_ALL(LOG_INFO, "Unix groups:");
	struct policy_gid const *g = policy->groups;
	struct json_object_iter it;
	json_object_object_foreachC(groups, it) {
		if (g == policy->groups + policy->n_groups)
			return;
		struct cstr_view who = policy->roles[g++->role].who;
		LOG_ALL(LOG_INFO, "  %-*s  %.*s", width, it.key, (int)who.len, who.data);
	}
}

/* Log askai's own policy @p policy, and if @p lent is not nullptr, the
 * tools of @p reg that the upstream may call, which @p lent returns with
 * @p ctx. The keys are padded to the width of the longest.
 */
static void
policy_log_own (struct policy const   *policy,
                struct registry const *reg,
                policy_lent_fn        *lent,
                void const            *ctx)
{
	if (policy->n_tokens) {
		LOG_ALL(LOG_INFO, "policy: built in, with ASKAI_TOKEN");
		LOG_ALL(LOG_INFO, "  clients with the token  all tools and metrics");
		LOG_ALL(LOG_INFO, "  other clients           refused");
	} else {
		LOG_ALL(LOG_INFO, "policy: built in, without ASKAI_TOKEN");
		LOG_ALL(LOG_INFO, "  loopback        all tools and metrics");
		LOG_ALL(LOG_INFO, "  remote          all tools");
		LOG_ALL(LOG_INFO, "  unix            all tools and metrics");
	}
	struct policy_grant const *r = &policy->roles[0];
	if (lent)
		policy_log_tools("upstream tools", policy->n_tokens ? 22 : 14, reg,
		                 lent(ctx, r->tools, r->upstream));
}

/* Write the label of the uids of @p r into the @p size bytes at @p buf:
 * "uid N" for one uid, else "uids FIRST-LAST". Returns its length.
 */
static size_t
policy_range_label (char         *buf,
                    size_t        size,
                    struct range  r)
{
	if (r.first == r.last)
		return util_format(buf, size, "uid %" PRIu32, r.first);
	return util_format(buf, size, "uids %" PRIu32 "-%" PRIu32, r.first, r.last);
}

/* Log the uids of the clients on a Unix socket that policy_admit_uid()
 * refuses, each with the reason, after a warning for each overflow id that
 * could not be read. The first column is as wide as the longest label.
 */
static void
policy_log_peers (struct policy const *policy)
{
	static char const *const files[] = { POLICY_OVERFLOWUID, POLICY_OVERFLOWGID };
	for (size_t i = 0; i < array_size(files); ++i)
		if (policy->overflow_errors[i])
			LOG_ALL(LOG_WARN, "warning: cannot read %s: %s", files[i],
			        strerror(policy->overflow_errors[i]));
	char label[32];
	int width = (int)sizeof "subordinate uids" - 1;
	for (size_t i = 0; i < policy->n_subuids; ++i) {
		int n = (int)policy_range_label(label, sizeof label, policy->subuids[i]);
		width = n > width ? n : width;
	}
	LOG_ALL(LOG_INFO, "Unix socket peers refused:");
	LOG_ALL(LOG_INFO, "  %-*s  no credentials", width, "uid 4294967295");
	(void)util_format(label, sizeof label, "uid %" PRIu32, policy->overflow_uid);
	LOG_ALL(LOG_INFO, "  %-*s  overflow uid", width, label);
	if (!policy->n_subuids)
		LOG_ALL(LOG_INFO, "  %-*s  none", width, "subordinate uids");
	for (size_t i = 0; i < policy->n_subuids; ++i) {
		(void)policy_range_label(label, sizeof label, policy->subuids[i]);
		LOG_ALL(LOG_INFO, "  %-*s  subordinate (%s)", width, label, POLICY_SUBUID);
	}
}

void
policy_log (struct policy const   *policy,
            struct json_object    *file,
            struct registry const *reg,
            policy_lent_fn        *lent,
            void const            *ctx)
{
	if (!policy || !reg)
		return;
	struct json_object *of[array_size(policy_parts)];
	if (!policy_file(file, of)) {
		policy_log_own(policy, reg, lent, ctx);
		policy_log_peers(policy);
		return;
	}
	LOG_ALL(LOG_INFO, "policy: from the configuration file");
	tool_mask listed = policy_log_roles(policy, reg, lent, ctx);
	policy_log_identities(of[POLICY_IDENTITIES]);
	policy_log_groups(policy, of[POLICY_GROUPS]);
	LOG_ALL(LOG_INFO, "anonymous clients:");
	for (size_t k = 0; k < POLICY_KINDS; ++k) {
		size_t const role = policy->anonymous[k];
		struct cstr_view who = role == POLICY_NO_ROLE ? CSTR_VIEW("refused")
		                                              : policy->roles[role].who;
		LOG_ALL(LOG_INFO, "  %-8s  %.*s", policy_kinds[k].data, (int)who.len, who.data);
	}
	tool_mask unlisted = policy->allowed & ~listed & policy_all(reg);
	if (unlisted)
		LOG_ALL(LOG_WARN, "warning: no role lists these tools:");
	for (uint32_t id = 0; id < reg->n_granted; ++id)
		if (unlisted >> id & 1)
			LOG_ALL(LOG_WARN, "  %s", reg->tools[id].name.data);
	policy_log_peers(policy);
}

enum policy_admit
policy_admit_uid (struct policy const *policy,
                  uint32_t             uid)
{
	if (!policy || uid == UINT32_MAX || uid == policy->overflow_uid)
		return POLICY_UNMAPPED;
	return policy_ranges_have(policy->subuids, policy->n_subuids, uid)
	     ? POLICY_SUBORDINATE : POLICY_ADMIT;
}

int
policy_key (struct policy       *policy,
            unsigned char const  key[POLICY_KEY])
{
	if (!policy || !key)
		return EINVAL;
	if (atomic_load_explicit(&policy->refs, memory_order_acquire) != 1)
		return EBUSY;
	memcpy(policy->key, key, sizeof policy->key);
	return policy_principals(policy);
}

uint64_t
policy_principal_uid (struct policy const *policy,
                      uint32_t             uid,
                      int                 *e)
{
	if (!e)
		return 0;
	if (!policy) {
		*e = EINVAL;
		return 0;
	}
	char anchor[sizeof "uid:4294967295"];
	int n = snprintf(anchor, sizeof anchor, "uid:%" PRIu32, uid);
	uint64_t principal = 0;
	*e = n < 0 || n >= (int)sizeof anchor ? EINVAL
	   : policy_principal_of(policy, 0, anchor, (size_t)n, &principal);
	return principal;
}

/* Grant @p role to the client of @p principal, or refuse the client if
 * @p role is POLICY_NO_ROLE. The log shows the client as @p who, or as its
 * role if @p who is empty.
 */
static enum policy_verdict
policy_grant_role (struct policy const *policy,
                   size_t               role,
                   uint64_t             principal,
                   struct cstr_view     who,
                   struct policy_grant *g)
{
	if (role == POLICY_NO_ROLE)
		return POLICY_REFUSED;
	*g = policy->roles[role];
	g->principal = principal;
	if (who.len)
		g->who = who;
	return POLICY_GRANTED;
}

#undef POLICY_NO_ROLE

/* Grant the client that shows @p token the role of the identity that the
 * token proves, if any.
 */
static enum policy_verdict
policy_grant_token (struct policy const *policy,
                    struct cstr_view     token,
                    struct policy_grant *g)
{
	unsigned char digest[DIGEST];
	SHA256((unsigned char const *)token.data, token.len, digest);
	struct policy_token const *t = bsearch(digest, policy->tokens, policy->n_tokens,
	                                       sizeof *policy->tokens, policy_by_digest);
	if (!t || policy_grant_role(policy, t->role, t->principal, t->who, g) != POLICY_GRANTED)
		return POLICY_REFUSED;
	g->flags |= t->flags;
	return POLICY_GRANTED;
}

/* Grant Unix user @p peer, who shows no token, the role of the first group
 * of the policy that contains the user, or else the anonymous role over a
 * Unix socket. The log shows the client by its role, and mcp.c adds the
 * uid.
 */
static enum policy_verdict
policy_grant_unix (struct policy const      *policy,
                   struct policy_peer const *peer,
                   struct policy_grant      *g)
{
	size_t role = policy->anonymous[POLICY_UNIX];
	for (size_t i = 0; i < policy->n_groups; ++i) {
		if (bsearch(&policy->groups[i].gid, peer->groups, peer->n_groups,
		            sizeof *peer->groups, policy_by_id)) {
			role = policy->groups[i].role;
			break;
		}
	}
	return policy_grant_role(policy, role, peer->principal,
	                         cstr_view(nullptr), g);
}

enum policy_verdict
policy_grant (struct policy const      *policy,
              struct cstr_view          token,
              enum policy_kind          kind,
              struct policy_peer const *peer,
              struct policy_grant      *g)
{
	if (!policy || !g || kind >= POLICY_KINDS)
		return POLICY_REFUSED;
	/* Bit 1 is set for a client on a Unix socket, and bit 0 for a token. */
	switch ((kind == POLICY_UNIX && peer) << 1 | (token.data != nullptr)) {
	case 1:
		if (policy->n_tokens)
			return policy_grant_token(policy, token, g);
		[[fallthrough]];
	case 0:
		return policy_grant_role(policy, policy->anonymous[kind], 0,
		                         policy->strangers[kind], g);
	case 2:
		return policy_grant_unix(policy, peer, g);
	default:
		break;
	}
	if (!policy->unix_token)
		return POLICY_UNIX_TOKEN;
	/* ASKAI_TOKEN under askai's own policy grants its role, and the calls
	 * remain those of the Unix user. */
	if (policy_grant_token(policy, token, g) != POLICY_GRANTED)
		return POLICY_REFUSED;
	g->principal = peer->principal;
	return POLICY_GRANTED;
}

void
policy_sort_ids (uint32_t *ids,
                 size_t    n)
{
	qsort(ids, n, sizeof *ids, policy_by_id);
}
