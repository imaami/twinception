/** @file Identities and roles (policy.h). */
#include <errno.h>
#include <grp.h>
#include <json-c/json.h>
#include <openssl/sha.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "policy.h"
#include "util.h"

enum { DIGEST = SHA256_DIGEST_LENGTH };

/* Principals are hashes of identities' names, which the configuration
 * picks, not clients. */
static constexpr uint64_t SEED = 0x9e3779b97f4a7c15;

/* The members of "anonymous", by the kind of client.  TOOL_TEXT() is a
 * compound literal, which a static initializer may not have. */
#define KIND(s) { "" s, sizeof s - 1 }
static struct tool_text const policy_kinds[POLICY_KINDS] = {
	[POLICY_LOOPBACK] = KIND("loopback"),
	[POLICY_REMOTE]   = KIND("remote"),
	[POLICY_UNIX]     = KIND("unix"),
};
#undef KIND

/* What the clients a role is given may see and do. */
struct policy_role {
	tool_mask        tools;
	tool_mask        upstream;  /* the tools that the upstream may call in
	                               the turns of the clients' ask calls */
	struct tool_text who;       /* "role NAME", for the log; empty in
	                               askai's own policy */
	uint64_t         metrics;   /* a bool value in a uint64_t that fills
	                               the padding */
};

/* A token, by its digest, and the identity it proves. */
struct policy_token {
	unsigned char    digest[DIGEST];
	struct tool_text who;        /* "identity NAME", for the log */
	uint64_t         principal;
	uint64_t         role;
};

/* A Unix user, by uid first, and the identity it is. */
struct policy_uid {
	uint32_t         uid;
	uint32_t         role;
	uint64_t         principal;
	struct tool_text who;        /* "identity NAME", for the log */
};

/* A Unix group, by gid first, and its role. */
struct policy_gid {
	uint32_t gid;
	uint32_t role;
};

/* One block has a policy and, after it, its tokens, users, roles, groups
 * and the names of its clients for the log, in that order, so that each is
 * aligned. */
struct policy {
	tool_mask            allowed;                  /* the tools that the
	                                                  roles may grant */
	_Atomic uint64_t     refs;
	struct policy_token *tokens;                   /* by digest */
	struct policy_uid   *users;                    /* by uid */
	struct policy_role  *roles;
	struct policy_gid   *groups;                   /* in the configuration's
	                                                  order, which the first
	                                                  that a user is in wins */
	size_t               n_tokens;
	size_t               n_users;
	size_t               n_roles;
	size_t               n_groups;
	int64_t              anonymous[POLICY_KINDS];  /* a role; -1: none */
	struct tool_text     strangers[POLICY_KINDS];  /* the names of anonymous
	                                                  clients for the log,
	                                                  "anonymous KIND (role
	                                                  NAME)"; empty in
	                                                  askai's own policy */
};

static_assert(alignof (struct policy_token) <= alignof (struct policy) &&
              alignof (struct policy_uid) <= alignof (struct policy_token) &&
              alignof (struct policy_role) <= alignof (struct policy_uid) &&
              alignof (struct policy_gid) <= alignof (struct policy_role));

/* What policy_create() works from, and where it says what is wrong. */
struct policy_build {
	tool_mask           upstream;  /* the tools that askai can offer to the
	                                  upstream */
	struct policy      *p;
	struct json_object *roles;    /* the file's, or nullptr */
	char const *const  *tools;
	char               *why;
	char               *buf;      /* for user and group lookups, kept while
	                                 the policy is made */
	char               *names;    /* where the next name for the log goes,
	                                 in the policy's block */
	size_t              n_tools;
	size_t              len;
	size_t              size;     /* of buf */
};

/* Say what is wrong; returns EINVAL. */
[[gnu::format(printf, 2, 3)]]
static int
policy_build_fail (struct policy_build *b,
                   char const          *fmt,
                   ...)
{
	va_list ap;
	va_start(ap, fmt);
	util_vformat(b->why, b->len, fmt, ap);
	va_end(ap);
	return EINVAL;
}

/* Say that @p what, such as "unix-groups: no group", names no user or group
 * @p name, as @p e says, ENOENT for none at all; returns EINVAL, or @p e if
 * that is ENOMEM. */
static int
policy_build_none (struct policy_build *b,
                   char const          *what,
                   char const          *name,
                   int                  e)
{
	if (e == ENOMEM)
		return e;
	if (e == ENOENT)
		return policy_build_fail(b, "%s %s", what, name);
	return policy_build_fail(b, "%s %s: %s", what, name, strerror(e));
}

/* The length of @p name, a role's or an identity's; 0 if it is not a
 * valid name. */
static size_t
policy_name_len (char const *name)
{
	size_t n = strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	                        "0123456789_.@-");
	return n <= POLICY_NAME && !name[n] ? n : 0;
}

/* The principal of the identity @p name of @p len bytes: never 0, which is
 * anonymous. */
static uint64_t
policy_principal (char const *name,
                  size_t      len)
{
	uint64_t h = util_hash(SEED, name, len);
	return h + !h;
}

/* Copy the @p n texts @p parts, one after another, into the policy's
 * block, and return the copy, which names a client in the log. */
static struct tool_text
policy_build_who (struct policy_build    *b,
                  size_t                  n,
                  struct tool_text const  parts[static n])
{
	char *who = b->names;
	for (size_t i = 0; i < n; ++i) {
		memcpy(b->names, parts[i].ptr, parts[i].len);
		b->names += parts[i].len;
	}
	return (struct tool_text){ who, (size_t)(b->names - who) };
}

/* The principal of the calls of Unix user @p uid that no identity is. */
static uint64_t
policy_principal_uid (uint32_t uid)
{
	uint64_t h = util_hash(~SEED, &uid, sizeof uid);
	return h + !h;
}

/* The index of the role that @p name, a string, names; -1 if none does. */
static int64_t
policy_build_role (struct policy_build const *b,
                   struct json_object        *name)
{
	char const *s = json_object_is_type(name, json_type_string)
	              ? json_object_get_string(name) : nullptr;
	if (!s || !b->roles)
		return -1;
	int64_t i = 0;
	struct json_object_iter it;
	json_object_object_foreachC(b->roles, it) {
		if (!strcmp(it.key, s))
			return i;
		++i;
	}
	return -1;
}

/* Append a token of @p digest for the identity @p who of @p principal and
 * @p role. */
static void
policy_token_add (struct policy       *p,
                  unsigned char const *digest,
                  struct tool_text     who,
                  uint64_t             principal,
                  uint64_t             role)
{
	struct policy_token *t = &p->tokens[p->n_tokens++];
	memcpy(t->digest, digest, DIGEST);
	t->who = who;
	t->principal = principal;
	t->role = role;
}

static int
policy_by_digest (void const *a,
                  void const *b)
{
	return memcmp(a, b, DIGEST);
}

/* Of uint32_t ids, or of structs that begin with one. */
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

/* @p name as a number, in @p *id, if it is one. */
static bool
policy_number (char const *name,
               uint32_t   *id)
{
	if (*name < '0' || *name > '9')
		return false;
	char *end;
	/* strtoul() leaves errno as it was on success. */
	errno = 0;
	unsigned long n = strtoul(name, &end, 10);
	if (*end || errno || n >= UINT32_MAX)
		return false;
	*id = (uint32_t)n;
	return true;
}

/* Double the lookups' buffer, up to 1 MiB: ERANGE past that, or ENOMEM. */
static int
policy_build_grow (struct policy_build *b)
{
	size_t size = b->size ? 2 * b->size : 1 << 10;
	if (size > 1 << 20)
		return ERANGE;
	char *buf = realloc(b->buf, size);
	if (!buf)
		return ENOMEM;
	b->buf = buf;
	b->size = size;
	return 0;
}

/* Unix user @p name, a number itself or a name the system knows, in
 * @p *uid; ENOENT if it knows none, ENOMEM, or why the lookup failed. */
static int
policy_build_uid (struct policy_build *b,
                  char const          *name,
                  uint32_t            *uid)
{
	if (policy_number(name, uid))
		return 0;
	int e = b->buf ? 0 : policy_build_grow(b);
	for (; !e; e = policy_build_grow(b)) {
		struct passwd pw;
		struct passwd *r = nullptr;
		int found = getpwnam_r(name, &pw, b->buf, b->size, &r);
		if (found == ERANGE)
			continue;
		if (found)
			return found;
		if (!r)
			return ENOENT;
		*uid = (uint32_t)r->pw_uid;
		return 0;
	}
	return e;
}

/* Unix group @p name, as policy_build_uid() finds a user. */
static int
policy_build_gid (struct policy_build *b,
                  char const          *name,
                  uint32_t            *gid)
{
	if (policy_number(name, gid))
		return 0;
	int e = b->buf ? 0 : policy_build_grow(b);
	for (; !e; e = policy_build_grow(b)) {
		struct group gr;
		struct group *r = nullptr;
		int found = getgrnam_r(name, &gr, b->buf, b->size, &r);
		if (found == ERANGE)
			continue;
		if (found)
			return found;
		if (!r)
			return ENOENT;
		*gid = (uint32_t)r->gr_gid;
		return 0;
	}
	return e;
}

/* Read @p spec, "sha256:HEX" or "env:NAME", into @p digest. */
static int
policy_build_token (struct policy_build *b,
                    char const          *who,
                    char const          *spec,
                    unsigned char       *digest)
{
	static char const sha[] = "sha256:";
	static char const env[] = "env:";
	if (!strncmp(spec, sha, sizeof sha - 1)) {
		char const *hex = spec + sizeof sha - 1;
		size_t n = strspn(hex, "0123456789abcdefABCDEF");
		if (n != 2 * DIGEST || hex[n])
			return policy_build_fail(b, "identity %s: sha256: needs %d hex digits",
			                         who, 2 * DIGEST);
		for (size_t i = 0; i < DIGEST; ++i)
			digest[i] = (unsigned char)(util_hex_digit(hex[2 * i]) << 4 |
			                            util_hex_digit(hex[2 * i + 1]));
		return 0;
	}
	if (strncmp(spec, env, sizeof env - 1))
		return policy_build_fail(b, "identity %s: a token is sha256:HEX or env:NAME",
		                         who);
	char const *name = spec + sizeof env - 1;
	if (!*name)
		return policy_build_fail(b, "identity %s: env: is not set", who);
	char const *token = getenv(name);
	if (!token || !*token)
		return policy_build_fail(b, "identity %s: %s is not set", who, name);
	SHA256((unsigned char const *)token, strlen(token), digest);
	return 0;
}

/* Write the names of the tools in @p mask of the @p n tools @p tools into
 * the @p size bytes at @p buf, separated by commas, or "none" for no tool;
 * returns the length. */
static size_t
policy_names (char              *buf,
              size_t             size,
              char const *const  tools[],
              size_t             n,
              tool_mask          mask)
{
	size_t len = 0;
	for (size_t i = 0; i < n; ++i)
		if (mask >> i & 1 && tools[i])
			len += util_format(buf + len, size - len, "%s%s", len ? ", " : "",
			                   tools[i]);
	return len ? len : util_format(buf, size, "none");
}

/* The mask of the tools that askai offers, of @p b's. */
static tool_mask
policy_build_all (struct policy_build const *b)
{
	tool_mask all = 0;
	for (size_t i = 0; i < b->n_tools; ++i)
		all |= b->tools[i] ? (tool_mask)1 << i : 0;
	return all;
}

/* Write the reason why role @p name may not name @p v: askai offers no such
 * tool, or with @p up lends no such tool to the upstream.  The reason lists
 * the tools that askai offers or lends.  Returns EINVAL. */
static int
policy_build_no_tool (struct policy_build *b,
                      char const          *name,
                      struct json_object  *v,
                      bool                 up)
{
	char names[1024];
	tool_mask mask = up ? b->upstream : policy_build_all(b);
	policy_names(names, sizeof names, b->tools, b->n_tools, mask);
	if (up && !mask)
		return policy_build_fail(b, "role %s: askai offers no tool %s to the "
		                         "upstream; it lends no tool", name,
		                         json_object_to_json_string(v));
	return policy_build_fail(b, "role %s: askai offers no tool %s%s; the tools %s"
	                         "are %s", name, json_object_to_json_string(v),
	                         up ? " to the upstream" : "", up ? "that it lends " : "",
	                         names);
}

/* Reads the list of tool names @p list of role @p name into @p r.  @p key
 * is the name of the list's member, "tools" or "upstream".  The member
 * "upstream" may name only the tools that askai can offer to the upstream. */
static int
policy_build_tools (struct policy_build *b,
                    struct policy_role  *r,
                    char const          *name,
                    char const          *key,
                    struct json_object  *list)
{
	if (!json_object_is_type(list, json_type_array))
		return policy_build_fail(b, "role %s: %s must be an array of names",
		                         name, key);
	bool up = *key == 'u';
	tool_mask *mask = up ? &r->upstream : &r->tools;
	size_t len = json_object_array_length(list);
	for (size_t i = 0; i < len; ++i) {
		struct json_object *v = json_object_array_get_idx(list, i);
		char const *tool = json_object_is_type(v, json_type_string)
		                 ? json_object_get_string(v) : "";
		size_t k = 0;
		while (k < b->n_tools && !(b->tools[k] && !strcmp(b->tools[k], tool)))
			++k;
		if (k == b->n_tools || (up && !(b->upstream >> k & 1)))
			return policy_build_no_tool(b, name, v, up);
		*mask |= (tool_mask)1 << k;
	}
	return 0;
}

/* Member @p key, @p val, of role @p name, into @p r. */
static int
policy_build_member (struct policy_build *b,
                     struct policy_role  *r,
                     char const          *name,
                     char const          *key,
                     struct json_object  *val)
{
	if (!strcmp(key, "tools") || !strcmp(key, "upstream"))
		return policy_build_tools(b, r, name, key, val);
	if (strcmp(key, "metrics"))
		return policy_build_fail(b, "role %s: unknown member %s", name, key);
	if (!json_object_is_type(val, json_type_boolean))
		return policy_build_fail(b, "role %s: metrics must be true or false", name);
	r->metrics = json_object_get_boolean(val);
	return 0;
}

/* Role @p name, its members @p members, into @p r. */
static int
policy_build_role_one (struct policy_build *b,
                       struct policy_role  *r,
                       char const          *name,
                       struct json_object  *members)
{
	size_t len = policy_name_len(name);
	if (!len)
		return policy_build_fail(b, "invalid role name: %s", name);
	if (!json_object_is_type(members, json_type_object))
		return policy_build_fail(b, "role %s must be an object", name);
	r->who = policy_build_who(b, 2, (struct tool_text const[]){
		TOOL_TEXT("role "), { name, len } });
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

static int
policy_build_roles (struct policy_build *b)
{
	if (!b->roles)
		return 0;
	if (!json_object_is_type(b->roles, json_type_object))
		return policy_build_fail(b, "roles must be an object");
	struct policy_role *r = b->p->roles;
	struct json_object_iter it;
	json_object_object_foreachC(b->roles, it) {
		int e = policy_build_role_one(b, r++, it.key, it.val);
		if (e)
			return e;
	}
	return 0;
}

/* Unix user @p user, a number or a name, in @p *uid. */
static int
policy_build_user (struct policy_build *b,
                   struct json_object  *user,
                   uint32_t            *uid)
{
	if (json_object_is_type(user, json_type_string))
		return policy_build_uid(b, json_object_get_string(user), uid);
	int64_t n = json_object_is_type(user, json_type_int) ? json_object_get_int64(user)
	                                                     : -1;
	if (n < 0 || n >= UINT32_MAX)
		return EINVAL;
	*uid = (uint32_t)n;
	return 0;
}

/* The members of identity @p who, @p id: its role, its tokens and its Unix
 * users. */
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
	struct json_object *users = nullptr;
	struct json_object_iter m;
	json_object_object_foreachC(id, m) {
		if (!strcmp(m.key, "role"))
			role = m.val;
		else if (!strcmp(m.key, "tokens"))
			tokens = m.val;
		else if (!strcmp(m.key, "unix-users"))
			users = m.val;
		else
			return policy_build_fail(b, "identity %s: unknown member %s", who, m.key);
	}
	int64_t r = policy_build_role(b, role);
	if (r < 0)
		return policy_build_fail(b, "identity %s: no role %s", who,
		                         role ? json_object_to_json_string(role) : "named");
	if ((tokens && !json_object_is_type(tokens, json_type_array)) ||
	    (users && !json_object_is_type(users, json_type_array)))
		return policy_build_fail(b, "identity %s: tokens and unix-users are arrays",
		                         who);
	size_t n = tokens ? json_object_array_length(tokens) : 0;
	size_t k = users ? json_object_array_length(users) : 0;
	if (!n && !k)
		return policy_build_fail(b, "identity %s needs tokens or unix-users", who);
	uint64_t principal = policy_principal(who, len);
	struct tool_text name = policy_build_who(b, 2, (struct tool_text const[]){
		TOOL_TEXT("identity "), { who, len } });
	char what[POLICY_NAME + 32];
	util_format(what, sizeof what, "identity %s: no user", who);
	for (size_t i = 0; i < k; ++i) {
		struct json_object *user = json_object_array_get_idx(users, i);
		uint32_t uid = 0;
		int e = policy_build_user(b, user, &uid);
		if (e)
			return policy_build_none(b, what, json_object_to_json_string(user), e);
		b->p->users[b->p->n_users++] = (struct policy_uid){
			.uid = uid,
			.role = (uint32_t)r,
			.principal = principal,
			.who = name
		};
	}
	for (size_t i = 0; i < n; ++i) {
		struct json_object *spec = json_object_array_get_idx(tokens, i);
		unsigned char digest[DIGEST];
		if (!json_object_is_type(spec, json_type_string))
			return policy_build_fail(b, "identity %s: a token is sha256:HEX or "
			                         "env:NAME", who);
		int e = policy_build_token(b, who, json_object_get_string(spec), digest);
		if (e)
			return e;
		policy_token_add(b->p, digest, name, principal, (uint64_t)r);
	}
	return 0;
}

static int
policy_build_identities (struct policy_build *b,
                         struct json_object  *identities)
{
	if (!identities)
		return 0;
	if (!json_object_is_type(identities, json_type_object))
		return policy_build_fail(b, "identities must be an object");
	struct json_object_iter it;
	json_object_object_foreachC(identities, it) {
		int e = policy_build_identity(b, it.key, it.val);
		if (e)
			return e;
	}
	struct policy *p = b->p;
	if (p->n_tokens)
		qsort(p->tokens, p->n_tokens, sizeof *p->tokens, policy_by_digest);
	for (size_t i = 1; i < p->n_tokens; ++i)
		if (!policy_by_digest(&p->tokens[i - 1], &p->tokens[i]))
			return policy_build_fail(b, "two tokens of the identities are the same");
	if (p->n_users)
		qsort(p->users, p->n_users, sizeof *p->users, policy_by_id);
	for (size_t i = 1; i < p->n_users; ++i)
		if (p->users[i - 1].uid == p->users[i].uid)
			return policy_build_fail(b, "two identities are Unix user %u",
			                         p->users[i].uid);
	return 0;
}

static int
policy_build_groups (struct policy_build *b,
                     struct json_object  *groups)
{
	if (!groups)
		return 0;
	if (!json_object_is_type(groups, json_type_object))
		return policy_build_fail(b, "unix-groups must be an object");
	struct policy_gid *g = b->p->groups;
	struct json_object_iter it;
	json_object_object_foreachC(groups, it) {
		int64_t r = policy_build_role(b, it.val);
		if (r < 0)
			return policy_build_fail(b, "unix-groups: %s: no role %s", it.key,
			                         json_object_to_json_string(it.val));
		int e = policy_build_gid(b, it.key, &g->gid);
		if (e)
			return policy_build_none(b, "unix-groups: no group", it.key, e);
		g++->role = (uint32_t)r;
		++b->p->n_groups;
	}
	return 0;
}

/* The role, if any, of a client that shows no token and connects as
 * @p kind, which @p val, null for none, names. */
static int
policy_build_kind (struct policy_build *b,
                   char const          *kind,
                   struct json_object  *val,
                   int64_t             *role)
{
	*role = -1;
	if (!val)
		return 0;
	*role = policy_build_role(b, val);
	if (*role < 0)
		return policy_build_fail(b, "anonymous: %s: no role %s", kind,
		                         json_object_to_json_string(val));
	return 0;
}

static int
policy_build_anonymous (struct policy_build *b,
                        struct json_object  *anonymous)
{
	if (!anonymous)
		return 0;
	if (!json_object_is_type(anonymous, json_type_object))
		return policy_build_fail(b, "anonymous must be an object");
	struct json_object_iter it;
	json_object_object_foreachC(anonymous, it) {
		size_t k = 0;
		while (k < POLICY_KINDS && strcmp(policy_kinds[k].ptr, it.key))
			++k;
		if (k == POLICY_KINDS)
			return policy_build_fail(b, "anonymous: unknown member %s", it.key);
		struct policy *p = b->p;
		int e = policy_build_kind(b, it.key, it.val, &p->anonymous[k]);
		if (e)
			return e;
		if (p->anonymous[k] >= 0)
			p->strangers[k] = policy_build_who(b, 5, (struct tool_text const[]){
				TOOL_TEXT("anonymous "), policy_kinds[k], TOOL_TEXT(" ("),
				p->roles[p->anonymous[k]].who, TOOL_TEXT(")") });
	}
	return 0;
}

/* The file's policy: its roles, identities, groups and anonymous roles. */
static int
policy_build_file (struct policy_build *b,
                   struct json_object  *identities,
                   struct json_object  *groups,
                   struct json_object  *anonymous)
{
	int e = policy_build_roles(b);
	if (e)
		return e;
	if ((e = policy_build_identities(b, identities)))
		return e;
	if ((e = policy_build_groups(b, groups)))
		return e;
	return policy_build_anonymous(b, anonymous);
}

/* askai's own policy: with ASKAI_TOKEN, @p token, an identity of
 * everything, that clients must be; else everything for clients on loopback,
 * and the tools for others. */
static void
policy_build_own (struct policy_build *b,
                  char const          *token)
{
	struct policy *p = b->p;
	tool_mask all = policy_build_all(b);
	p->roles[0] = (struct policy_role){ .tools = all, .upstream = all, .metrics = true };
	p->roles[1] = (struct policy_role){ .tools = all, .upstream = all };
	if (!token) {
		p->anonymous[POLICY_LOOPBACK] = 0;
		p->anonymous[POLICY_REMOTE] = 1;
		p->anonymous[POLICY_UNIX] = 0;
		return;
	}
	static char const id[] = "ASKAI_TOKEN";
	unsigned char digest[DIGEST];
	SHA256((unsigned char const *)token, strlen(token), digest);
	policy_token_add(p, digest, TOOL_TEXT("identity ASKAI_TOKEN"),
	                 policy_principal(id, sizeof id - 1), 0);
}

/* The members of @p obj, if an object. */
static size_t
policy_members (struct json_object *obj)
{
	return json_object_is_type(obj, json_type_object)
	     ? (size_t)json_object_object_length(obj) : 0;
}

/* The elements of the member @p key of @p obj, if an object's array. */
static size_t
policy_elements (struct json_object *obj,
                 char const         *key)
{
	struct json_object *v;
	return json_object_object_get_ex(obj, key, &v) &&
	       json_object_is_type(v, json_type_array) ? json_object_array_length(v) : 0;
}

/* The members of the configuration file's object @p file that make the
 * policy, each left as it is if absent; whether @p file has any of them,
 * which turns askai's own policy off. */
static bool
policy_file (struct json_object  *file,
             struct json_object **roles,
             struct json_object **ids,
             struct json_object **groups,
             struct json_object **anon)
{
	/* Each looked up, so not ||. */
	return json_object_object_get_ex(file, "roles", roles) |
	       json_object_object_get_ex(file, "identities", ids) |
	       json_object_object_get_ex(file, "unix-groups", groups) |
	       json_object_object_get_ex(file, "anonymous", anon);
}

struct policy *
policy_create (struct json_object *file,
               char const *const   tools[],
               size_t              n_tools,
               tool_mask           allowed,
               tool_mask           upstream,
               char               *why,
               size_t              len,
               int                *e)
{
	struct policy_build b = {
		.upstream = upstream,
		.tools = tools,
		.why = why,
		.n_tools = n_tools,
		.len = len
	};
	struct json_object *ids = nullptr;
	struct json_object *groups = nullptr;
	struct json_object *anon = nullptr;
	bool mine = policy_file(file, &b.roles, &ids, &groups, &anon);
	char const *token = mine ? nullptr : getenv("ASKAI_TOKEN");
	token = token && *token ? token : nullptr;

	/* Room for as many of each as the file names, or askai's own policy. */
	size_t n_tokens = token != nullptr;
	size_t n_users = 0;
	size_t n_roles = mine ? policy_members(b.roles) : 2;
	size_t n_groups = policy_members(groups);
	/* Room for the longest name of each role, identity and kind of
	 * anonymous client. */
	size_t n_names = mine ? n_roles * (sizeof "role " - 1 + POLICY_NAME) +
	                        policy_members(ids) * (sizeof "identity " - 1 + POLICY_NAME) +
	                        POLICY_KINDS * (POLICY_WHO - 1)
	                      : 0;
	struct json_object_iter it;
	if (policy_members(ids))
		json_object_object_foreachC(ids, it) {
			n_tokens += policy_elements(it.val, "tokens");
			n_users += policy_elements(it.val, "unix-users");
		}
	struct policy *p = calloc(1, sizeof *p + n_tokens * sizeof *p->tokens +
	                             n_users * sizeof *p->users +
	                             n_roles * sizeof *p->roles +
	                             n_groups * sizeof *p->groups + n_names);
	if (!p) {
		*e = ENOMEM;
		return nullptr;
	}
	atomic_init(&p->refs, 1);
	p->tokens = (struct policy_token *)(p + 1);
	p->users = (struct policy_uid *)(p->tokens + n_tokens);
	p->roles = (struct policy_role *)(p->users + n_users);
	p->groups = (struct policy_gid *)(p->roles + n_roles);
	p->n_roles = n_roles;
	b.names = (char *)(p->groups + n_groups);
	for (size_t k = 0; k < POLICY_KINDS; ++k)
		p->anonymous[k] = -1;
	b.p = p;
	*e = 0;
	if (mine)
		*e = policy_build_file(&b, ids, groups, anon);
	else
		policy_build_own(&b, token);
	free(b.buf);
	if (*e) {
		policy_put(p);
		return nullptr;
	}
	/* Clients get only the allowed tools.  A role without the member
	 * "upstream" lends the upstream its tools as the file lists them. */
	p->allowed = allowed;
	for (size_t i = 0; i < n_roles; ++i)
		p->roles[i].tools &= allowed;
	return p;
}

struct policy *
policy_get (struct policy *policy)
{
	atomic_fetch_add_explicit(&policy->refs, 1, memory_order_relaxed);
	return policy;
}

void
policy_put (struct policy *policy)
{
	if (!policy || atomic_fetch_sub_explicit(&policy->refs, 1,
	                                         memory_order_acq_rel) != 1)
		return;
	free(policy);
}

/* Log the tools of @p mask, of the names @p tools of @p n tools, one a
 * line, the first after @p key, which is padded to @p width columns; or
 * none. */
static void
policy_log_tools (char const        *key,
                  int                width,
                  char const *const  tools[],
                  size_t             n,
                  tool_mask          mask)
{
	char const *first = key;
	for (size_t i = 0; i < n; ++i)
		if (mask >> i & 1 && tools[i]) {
			LOG_ALL(LOG_INFO, "  %-*s  %s", width, key, tools[i]);
			key = "";
		}
	if (key == first)
		LOG_ALL(LOG_INFO, "  %-*s  none", width, key);
}

/* Log each role of the file's policy @p policy with the names @p tools of
 * the @p n_tools tools, and with the tools that the upstream may call,
 * which @p lent returns with @p ctx, if @p lent is not nullptr; returns
 * the tools that the roles grant or lend. */
static tool_mask
policy_log_roles (struct policy const *policy,
                  char const *const    tools[],
                  size_t               n_tools,
                  policy_lent_fn      *lent,
                  void const          *ctx)
{
	if (!policy->n_roles) {
		LOG_ALL(LOG_INFO, "roles: none");
		return 0;
	}
	tool_mask listed = 0;
	for (size_t i = 0; i < policy->n_roles; ++i) {
		struct policy_role const *r = &policy->roles[i];
		tool_mask up = lent ? lent(ctx, r->tools, r->upstream) : 0;
		LOG_ALL(LOG_INFO, "%.*s:", (int)r->who.len, r->who.ptr);
		policy_log_tools("tools", 14, tools, n_tools, r->tools);
		if (lent)
			policy_log_tools("upstream tools", 14, tools, n_tools, up);
		LOG_ALL(LOG_INFO, "  metrics         %s", r->metrics ? "yes" : "no");
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
 * its role. */
static void
policy_log_identities (struct json_object *ids)
{
	if (!policy_members(ids)) {
		LOG_ALL(LOG_INFO, "identities: none");
		return;
	}
	int width = policy_width(ids);
	LOG_ALL(LOG_INFO, "identities:");
	struct json_object_iter it;
	json_object_object_foreachC(ids, it) {
		struct json_object *role = nullptr;
		json_object_object_get_ex(it.val, "role", &role);
		char const *name = json_object_get_string(role);
		LOG_ALL(LOG_INFO, "  %-*s  role %s", width, it.key, name ? name : "none");
	}
}

/* Log the role of each Unix group of the file's policy @p policy, @p groups
 * in the file, for at most the groups of the policy. */
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
		struct tool_text who = policy->roles[g++->role].who;
		LOG_ALL(LOG_INFO, "  %-*s  %.*s", width, it.key, (int)who.len, who.ptr);
	}
}

/* Log askai's own policy @p policy, and if @p lent is not nullptr, the
 * tools of the names @p tools of @p n_tools that the upstream may call,
 * which @p lent returns with @p ctx.  The keys are padded to the width of
 * the longest. */
static void
policy_log_own (struct policy const *policy,
                char const *const    tools[],
                size_t               n_tools,
                policy_lent_fn      *lent,
                void const          *ctx)
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
	struct policy_role const *r = &policy->roles[0];
	if (lent)
		policy_log_tools("upstream tools", policy->n_tokens ? 22 : 14, tools, n_tools,
		                 lent(ctx, r->tools, r->upstream));
}

void
policy_log (struct policy const *policy,
            struct json_object  *file,
            char const *const    tools[],
            size_t               n_tools,
            size_t               n_granted,
            policy_lent_fn      *lent,
            void const          *ctx)
{
	if (!policy || !tools || n_tools > TOOL_MAX || n_granted > n_tools)
		return;
	struct json_object *roles = nullptr;
	struct json_object *ids = nullptr;
	struct json_object *groups = nullptr;
	struct json_object *anon = nullptr;
	if (!policy_file(file, &roles, &ids, &groups, &anon)) {
		policy_log_own(policy, tools, n_tools, lent, ctx);
		return;
	}
	LOG_ALL(LOG_INFO, "policy: from the configuration file");
	tool_mask listed = policy_log_roles(policy, tools, n_tools, lent, ctx);
	policy_log_identities(ids);
	policy_log_groups(policy, groups);
	LOG_ALL(LOG_INFO, "anonymous clients:");
	for (size_t k = 0; k < POLICY_KINDS; ++k) {
		int64_t role = policy->anonymous[k];
		struct tool_text who = role < 0 ? TOOL_TEXT("refused") : policy->roles[role].who;
		LOG_ALL(LOG_INFO, "  %-8s  %.*s", policy_kinds[k].ptr, (int)who.len, who.ptr);
	}
	tool_mask unlisted = 0;
	for (size_t i = 0; i < n_granted; ++i)
		unlisted |= tools[i] && (policy->allowed & ~listed) >> i & 1
		          ? (tool_mask)1 << i : 0;
	if (unlisted)
		LOG_ALL(LOG_WARN, "warning: no role lists these tools:");
	for (size_t i = 0; i < n_granted; ++i)
		if (unlisted >> i & 1)
			LOG_ALL(LOG_WARN, "  %s", tools[i]);
}

/* Grant @p role, if it is one, to the client of @p principal, which the
 * log names as @p who, or else as its role. */
static bool
policy_grant_role (struct policy const *policy,
                   int64_t              role,
                   uint64_t             principal,
                   struct tool_text     who,
                   struct policy_grant *g)
{
	if (role < 0)
		return false;
	struct policy_role const *r = &policy->roles[role];
	*g = (struct policy_grant){
		.principal = principal,
		.tools = r->tools,
		.upstream = r->upstream,
		.who = who.len ? who : r->who,
		.metrics = r->metrics
	};
	return true;
}

/* Grant the client that shows the token of @p len bytes at @p token the
 * role of the identity it proves, if any. */
static bool
policy_grant_token (struct policy const *policy,
                    char const          *token,
                    size_t               len,
                    struct policy_grant *g)
{
	unsigned char digest[DIGEST];
	SHA256((unsigned char const *)token, len, digest);
	struct policy_token const *t = bsearch(digest, policy->tokens, policy->n_tokens,
	                                       sizeof *policy->tokens, policy_by_digest);
	return t && policy_grant_role(policy, (int64_t)t->role, t->principal, t->who, g);
}

/* Grant Unix user @p peer, who shows no token, the role of the identity it
 * is, or else of its first group the policy names, or else the anonymous
 * one over a Unix socket. */
static bool
policy_grant_unix (struct policy const      *policy,
                   struct policy_peer const *peer,
                   struct policy_grant      *g)
{
	struct policy_uid const *u = !policy->n_users ? nullptr
	                           : bsearch(&peer->uid, policy->users, policy->n_users,
	                                     sizeof *policy->users, policy_by_id);
	if (u)
		return policy_grant_role(policy, u->role, u->principal, u->who, g);
	int64_t role = policy->anonymous[POLICY_UNIX];
	struct tool_text who = policy->strangers[POLICY_UNIX];
	for (size_t i = 0; i < policy->n_groups; ++i) {
		if (bsearch(&policy->groups[i].gid, peer->groups, peer->n_groups,
		            sizeof *peer->groups, policy_by_id)) {
			role = policy->groups[i].role;
			who = (struct tool_text){};
			break;
		}
	}
	return policy_grant_role(policy, role, policy_principal_uid(peer->uid), who, g);
}

bool
policy_grant (struct policy const      *policy,
              char const               *token,
              size_t                    len,
              enum policy_kind          kind,
              struct policy_peer const *peer,
              struct policy_grant      *g)
{
	if (token && policy->n_tokens)
		return policy_grant_token(policy, token, len, g);
	if (kind == POLICY_UNIX && peer)
		return policy_grant_unix(policy, peer, g);
	return policy_grant_role(policy, policy->anonymous[kind], 0, policy->strangers[kind],
	                         g);
}
