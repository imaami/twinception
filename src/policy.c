/** @file Identities and roles (policy.h). */
#include <errno.h>
#include <grp.h>
#include <json-c/json.h>
#include <openssl/sha.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "policy.h"
#include "util.h"

enum {
	DIGEST = SHA256_DIGEST_LENGTH,
	NAME   = 64,  /* bytes of a role's or an identity's name, at most */
};

/* Principals are hashes of identities' names, which the configuration
 * picks, not clients. */
static constexpr uint64_t SEED = 0x9e3779b97f4a7c15;

/* What the clients a role is given may see and do. */
struct policy_role {
	uint32_t tools;
	uint32_t metrics;
};

/* A token, by its digest, and the identity it proves. */
struct policy_token {
	unsigned char digest[DIGEST];
	uint64_t      principal;
	uint64_t      role;
};

/* A Unix user, by uid first, and the identity it is. */
struct policy_uid {
	uint32_t uid;
	uint32_t role;
	uint64_t principal;
};

/* A Unix group, by gid first, and its role. */
struct policy_gid {
	uint32_t gid;
	uint32_t role;
};

/* One block holds a policy and, after it, its tokens, users, roles and
 * groups, in that order, which keeps each aligned. */
struct policy {
	_Atomic uint64_t     refs;
	struct policy_token *tokens;                   /* by digest */
	struct policy_uid   *users;                    /* by uid */
	struct policy_role  *roles;
	struct policy_gid   *groups;                   /* in the configuration's
	                                                  order, which the first
	                                                  that a user is in wins */
	size_t               n_tokens;
	size_t               n_users;
	size_t               n_groups;
	int64_t              anonymous[POLICY_KINDS];  /* a role; -1: none */
};

static_assert(alignof (struct policy_token) <= alignof (struct policy) &&
              alignof (struct policy_uid) <= alignof (struct policy_token) &&
              alignof (struct policy_role) <= alignof (struct policy_uid) &&
              alignof (struct policy_gid) <= alignof (struct policy_role));

/* What policy_create() works from, and where it says what is wrong. */
struct policy_build {
	struct policy      *p;
	struct json_object *roles;    /* the file's, or nullptr */
	char const *const  *tools;
	char               *why;
	char               *buf;      /* for user and group lookups, kept while
	                                 the policy is made */
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
	(void)util_vformat(b->why, b->len, fmt, ap);
	va_end(ap);
	return EINVAL;
}

/* Say that @p what of @p who names no user or group @p name, as @p e says,
 * ENOENT for none at all; returns EINVAL, or @p e if that is ENOMEM. */
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

static bool
policy_name_valid (char const *name)
{
	size_t n = strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	                        "0123456789_.@-");
	return n && n <= NAME && !name[n];
}

/* The principal of the identity @p name: never 0, which is anonymous. */
static uint64_t
policy_principal (char const *name)
{
	uint64_t h = util_hash(SEED, name, strlen(name));
	return h + !h;
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

/* Append a token of @p digest for the identity of @p principal and
 * @p role. */
static void
policy_token_add (struct policy       *p,
                  unsigned char const *digest,
                  uint64_t             principal,
                  uint64_t             role)
{
	struct policy_token *t = &p->tokens[p->n_tokens++];
	memcpy(t->digest, digest, DIGEST);
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

/* The value of hexadecimal digit @p c. */
static unsigned
policy_nibble (char c)
{
	return c <= '9' ? (unsigned)(c - '0') : (unsigned)((c | 0x20) - 'a' + 10);
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
			digest[i] = (unsigned char)(policy_nibble(hex[2 * i]) << 4 |
			                            policy_nibble(hex[2 * i + 1]));
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

/* The tools of role @p name, @p tools, into @p r. */
static int
policy_build_tools (struct policy_build *b,
                    struct policy_role  *r,
                    char const          *name,
                    struct json_object  *tools)
{
	if (!json_object_is_type(tools, json_type_array))
		return policy_build_fail(b, "role %s: tools must be an array of names", name);
	size_t len = json_object_array_length(tools);
	for (size_t i = 0; i < len; ++i) {
		struct json_object *v = json_object_array_get_idx(tools, i);
		char const *tool = json_object_is_type(v, json_type_string)
		                 ? json_object_get_string(v) : "";
		size_t k = 0;
		while (k < b->n_tools && !(b->tools[k] && !strcmp(b->tools[k], tool)))
			++k;
		if (k == b->n_tools)
			return policy_build_fail(b, "role %s: askai offers no tool %s", name,
			                         json_object_to_json_string(v));
		r->tools |= 1u << k;
	}
	return 0;
}

/* Role @p name, its members @p members, into @p r. */
static int
policy_build_role_one (struct policy_build *b,
                       struct policy_role  *r,
                       char const          *name,
                       struct json_object  *members)
{
	if (!policy_name_valid(name))
		return policy_build_fail(b, "invalid role name: %s", name);
	if (!json_object_is_type(members, json_type_object))
		return policy_build_fail(b, "role %s must be an object", name);
	struct json_object_iter m;
	json_object_object_foreachC(members, m) {
		if (!strcmp(m.key, "tools")) {
			int e = policy_build_tools(b, r, name, m.val);
			if (e)
				return e;
		} else if (strcmp(m.key, "metrics")) {
			return policy_build_fail(b, "role %s: unknown member %s", name, m.key);
		} else if (!json_object_is_type(m.val, json_type_boolean)) {
			return policy_build_fail(b, "role %s: metrics must be true or false", name);
		} else {
			r->metrics = json_object_get_boolean(m.val);
		}
	}
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
	if (!policy_name_valid(who))
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
	uint64_t principal = policy_principal(who);
	char what[NAME + 32];
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
			.principal = principal
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
		policy_token_add(b->p, digest, principal, (uint64_t)r);
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
	static char const *const kinds[POLICY_KINDS] = {
		[POLICY_LOOPBACK] = "loopback",
		[POLICY_REMOTE] = "remote",
		[POLICY_UNIX] = "unix",
	};
	if (!anonymous)
		return 0;
	if (!json_object_is_type(anonymous, json_type_object))
		return policy_build_fail(b, "anonymous must be an object");
	struct json_object_iter it;
	json_object_object_foreachC(anonymous, it) {
		size_t k = 0;
		while (k < POLICY_KINDS && strcmp(kinds[k], it.key))
			++k;
		if (k == POLICY_KINDS)
			return policy_build_fail(b, "anonymous: unknown member %s", it.key);
		int e = policy_build_kind(b, it.key, it.val, &b->p->anonymous[k]);
		if (e)
			return e;
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
	uint32_t all = 0;
	for (size_t i = 0; i < b->n_tools; ++i)
		all |= b->tools[i] ? 1u << i : 0;
	p->roles[0] = (struct policy_role){ .tools = all, .metrics = true };
	p->roles[1] = (struct policy_role){ .tools = all };
	if (!token) {
		p->anonymous[POLICY_LOOPBACK] = 0;
		p->anonymous[POLICY_REMOTE] = 1;
		p->anonymous[POLICY_UNIX] = 0;
		return;
	}
	unsigned char digest[DIGEST];
	SHA256((unsigned char const *)token, strlen(token), digest);
	policy_token_add(p, digest, policy_principal("ASKAI_TOKEN"), 0);
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

struct policy *
policy_create (struct json_object *file,
               char const *const   tools[],
               size_t              n_tools,
               char               *why,
               size_t              len,
               int                *e)
{
	struct policy_build b = {
		.tools = tools,
		.why = why,
		.n_tools = n_tools,
		.len = len
	};
	struct json_object *ids = nullptr;
	struct json_object *groups = nullptr;
	struct json_object *anon = nullptr;
	/* Each looked up, so not ||. */
	bool mine = json_object_object_get_ex(file, "roles", &b.roles) |
	            json_object_object_get_ex(file, "identities", &ids) |
	            json_object_object_get_ex(file, "unix-groups", &groups) |
	            json_object_object_get_ex(file, "anonymous", &anon);
	char const *token = mine ? nullptr : getenv("ASKAI_TOKEN");
	token = token && *token ? token : nullptr;

	/* Room for as many of each as the file names, or askai's own policy. */
	size_t n_tokens = token != nullptr;
	size_t n_users = 0;
	size_t n_roles = mine ? policy_members(b.roles) : 2;
	size_t n_groups = policy_members(groups);
	struct json_object_iter it;
	if (policy_members(ids))
		json_object_object_foreachC(ids, it) {
			n_tokens += policy_elements(it.val, "tokens");
			n_users += policy_elements(it.val, "unix-users");
		}
	struct policy *p = calloc(1, sizeof *p + n_tokens * sizeof *p->tokens +
	                             n_users * sizeof *p->users +
	                             n_roles * sizeof *p->roles +
	                             n_groups * sizeof *p->groups);
	if (!p) {
		*e = ENOMEM;
		return nullptr;
	}
	atomic_init(&p->refs, 1);
	p->tokens = (struct policy_token *)(p + 1);
	p->users = (struct policy_uid *)(p->tokens + n_tokens);
	p->roles = (struct policy_role *)(p->users + n_users);
	p->groups = (struct policy_gid *)(p->roles + n_roles);
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

/* Grant @p role, if it is one, to the client of @p principal. */
static bool
policy_grant_role (struct policy const *policy,
                   int64_t              role,
                   uint64_t             principal,
                   struct policy_grant *g)
{
	if (role < 0)
		return false;
	*g = (struct policy_grant){
		.principal = principal,
		.tools = policy->roles[role].tools,
		.metrics = policy->roles[role].metrics
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
	return t && policy_grant_role(policy, (int64_t)t->role, t->principal, g);
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
		return policy_grant_role(policy, u->role, u->principal, g);
	int64_t role = policy->anonymous[POLICY_UNIX];
	for (size_t i = 0; i < policy->n_groups; ++i) {
		if (bsearch(&policy->groups[i].gid, peer->groups, peer->n_groups,
		            sizeof *peer->groups, policy_by_id)) {
			role = policy->groups[i].role;
			break;
		}
	}
	return policy_grant_role(policy, role, policy_principal_uid(peer->uid), g);
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
	return policy_grant_role(policy, policy->anonymous[kind], 0, g);
}
