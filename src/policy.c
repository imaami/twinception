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
struct role {
	uint32_t tools;
	uint32_t metrics;
};

/* A token, by its digest, and the identity it proves. */
struct token {
	unsigned char digest[DIGEST];
	uint64_t      principal;
	uint64_t      role;
};

/* A Unix user, by uid first, and the identity it is. */
struct uid_role {
	uint32_t uid;
	uint32_t role;
	uint64_t principal;
};

/* A Unix group, by gid first, and its role. */
struct gid_role {
	uint32_t gid;
	uint32_t role;
};

/* One block holds a policy and, after it, its tokens, users, roles and
 * groups, in that order, which keeps each aligned. */
struct policy {
	_Atomic uint64_t refs;
	struct token    *tokens;                   /* by digest */
	struct uid_role *users;                    /* by uid */
	struct role     *roles;
	struct gid_role *groups;                   /* in the configuration's
	                                              order, which the first
	                                              that a user is in wins */
	size_t           n_tokens;
	size_t           n_users;
	size_t           n_groups;
	int64_t          anonymous[POLICY_KINDS];  /* a role; -1: none */
};

static_assert(alignof (struct token) <= alignof (struct policy) &&
              alignof (struct uid_role) <= alignof (struct token) &&
              alignof (struct role) <= alignof (struct uid_role) &&
              alignof (struct gid_role) <= alignof (struct role));

/* What policy_new() works from, and where it says what is wrong. */
struct build {
	struct policy            *p;
	struct json_object       *roles;  /* the file's, or nullptr */
	char const *const        *tools;
	char                     *why;
	char                     *buf;    /* id_of()'s, kept while the policy is
	                                     made */
	size_t                    n_tools;
	size_t                    len;
	size_t                    size;   /* of buf */
};

[[gnu::format(printf, 2, 3)]]
static int
fail (struct build *b,
      char const   *fmt,
      ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b->why, b->len, fmt, ap);
	va_end(ap);
	return EINVAL;
}

static bool
valid_name (char const *name)
{
	size_t n = strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	                        "0123456789_.@-");
	return n && n <= NAME && !name[n];
}

/* The principal of the identity @p name: never 0, which is anonymous. */
static uint64_t
principal_of (char const *name)
{
	uint64_t h = hash_bytes(SEED, name, strlen(name));
	return h + !h;
}

/* The index of the role named @p name; -1 if there is none. */
static int64_t
role_of (struct build const *b,
         char const         *name)
{
	int64_t i = 0;
	struct json_object_iter it;
	if (b->roles)
		json_object_object_foreachC(b->roles, it) {
			if (!strcmp(it.key, name))
				return i;
			++i;
		}
	return -1;
}

/* Append a token of @p digest for the identity of @p principal and
 * @p role. */
static void
token_add (struct policy       *p,
           unsigned char const *digest,
           uint64_t             principal,
           uint64_t             role)
{
	struct token *t = &p->tokens[p->n_tokens++];
	memcpy(t->digest, digest, DIGEST);
	t->principal = principal;
	t->role = role;
}

static int
by_digest (void const *a,
           void const *b)
{
	return memcmp(a, b, DIGEST);
}

/* Of uint32_t ids, or of structs that begin with one. */
static int
by_id (void const *a,
       void const *b)
{
	uint32_t x;
	uint32_t y;
	memcpy(&x, a, sizeof x);
	memcpy(&y, b, sizeof y);
	return (x > y) - (x < y);
}

/* The principal of the calls of Unix user @p uid that no identity is. */
static uint64_t
principal_of_uid (uint32_t uid)
{
	uint64_t h = hash_bytes(~SEED, &uid, sizeof uid);
	return h + !h;
}

/* Look Unix user @p name, or group if @p group, up into @p *id: a number
 * itself, or a name the system knows; ENOENT if it knows none, ENOMEM, or
 * why the lookup failed. */
static int
id_of (struct build *b,
       char const   *name,
       bool          group,
       uint32_t     *id)
{
	char *end;
	errno = 0;
	unsigned long n = strtoul(name, &end, 10);
	if (*name >= '0' && *name <= '9' && !*end && !errno && n < UINT32_MAX) {
		*id = (uint32_t)n;
		return 0;
	}
	for (;;) {
		struct passwd pw;
		struct passwd *pwr = nullptr;
		struct group gr;
		struct group *grr = nullptr;
		int e = !b->buf ? ERANGE
		      : group ? getgrnam_r(name, &gr, b->buf, b->size, &grr)
		      : getpwnam_r(name, &pw, b->buf, b->size, &pwr);
		if (!e && (grr || pwr))
			*id = grr ? (uint32_t)grr->gr_gid : (uint32_t)pwr->pw_uid;
		if (e != ERANGE)
			return e ? e : grr || pwr ? 0 : ENOENT;
		size_t size = b->size ? 2 * b->size : 1 << 10;
		char *buf = size <= 1 << 20 ? realloc(b->buf, size) : nullptr;
		if (!buf)
			return size <= 1 << 20 ? ENOMEM : ERANGE;
		b->buf = buf;
		b->size = size;
	}
}

/* Read @p spec, "sha256:HEX" or "env:NAME", into @p digest. */
static int
token_spec (struct build  *b,
            char const    *who,
            char const    *spec,
            unsigned char *digest)
{
	static char const sha[] = "sha256:";
	static char const env[] = "env:";
	if (!strncmp(spec, sha, sizeof sha - 1)) {
		char const *hex = spec + sizeof sha - 1;
		bool ok = strlen(hex) == 2 * DIGEST &&
		          strspn(hex, "0123456789abcdefABCDEF") == 2 * DIGEST;
		for (size_t i = 0; ok && i < DIGEST; ++i) {
			unsigned v = 0;
			sscanf(hex + 2 * i, "%2x", &v);
			digest[i] = (unsigned char)v;
		}
		return ok ? 0 : fail(b, "identity %s: sha256: needs %d hex digits", who,
		                     2 * DIGEST);
	}
	if (!strncmp(spec, env, sizeof env - 1)) {
		char const *name = spec + sizeof env - 1;
		char const *token = *name ? getenv(name) : nullptr;
		if (!token || !*token)
			return fail(b, "identity %s: %s is not set", who, *name ? name : "env:");
		SHA256((unsigned char const *)token, strlen(token), digest);
		return 0;
	}
	return fail(b, "identity %s: a token is sha256:HEX or env:NAME", who);
}

static int
roles (struct build *b)
{
	struct json_object *roles = b->roles;
	if (!roles)
		return 0;
	if (!json_object_is_type(roles, json_type_object))
		return fail(b, "roles must be an object");
	struct role *r = b->p->roles;
	struct json_object_iter it;
	json_object_object_foreachC(roles, it) {
		if (!valid_name(it.key))
			return fail(b, "invalid role name: %s", it.key);
		if (!json_object_is_type(it.val, json_type_object))
			return fail(b, "role %s must be an object", it.key);
		struct json_object_iter m;
		json_object_object_foreachC(it.val, m) {
			if (!strcmp(m.key, "metrics")) {
				if (!json_object_is_type(m.val, json_type_boolean))
					return fail(b, "role %s: metrics must be true or false",
					            it.key);
				r->metrics = json_object_get_boolean(m.val);
				continue;
			}
			if (strcmp(m.key, "tools"))
				return fail(b, "role %s: unknown member %s", it.key, m.key);
			if (!json_object_is_type(m.val, json_type_array))
				return fail(b, "role %s: tools must be an array of names", it.key);
			size_t len = json_object_array_length(m.val);
			for (size_t i = 0; i < len; ++i) {
				struct json_object *v = json_object_array_get_idx(m.val, i);
				char const *name = json_object_is_type(v, json_type_string)
				                 ? json_object_get_string(v) : "";
				size_t k = 0;
				while (k < b->n_tools && !(b->tools[k] && !strcmp(b->tools[k], name)))
					++k;
				if (k == b->n_tools)
					return fail(b, "role %s: askai offers no tool %s", it.key,
					            json_object_to_json_string(v));
				r->tools |= 1u << k;
			}
		}
		++r;
	}
	return 0;
}

static int
identities (struct build       *b,
            struct json_object *identities)
{
	if (!identities)
		return 0;
	if (!json_object_is_type(identities, json_type_object))
		return fail(b, "identities must be an object");
	struct json_object_iter it;
	json_object_object_foreachC(identities, it) {
		char const *who = it.key;
		if (!valid_name(who))
			return fail(b, "invalid identity name: %s", who);
		if (!json_object_is_type(it.val, json_type_object))
			return fail(b, "identity %s must be an object", who);
		struct json_object *role = nullptr;
		struct json_object *tokens = nullptr;
		struct json_object *users = nullptr;
		struct json_object_iter m;
		json_object_object_foreachC(it.val, m) {
			if (!strcmp(m.key, "role"))
				role = m.val;
			else if (!strcmp(m.key, "tokens"))
				tokens = m.val;
			else if (!strcmp(m.key, "unix-users"))
				users = m.val;
			else
				return fail(b, "identity %s: unknown member %s", who, m.key);
		}
		int64_t r = json_object_is_type(role, json_type_string)
		          ? role_of(b, json_object_get_string(role)) : -1;
		if (r < 0)
			return fail(b, "identity %s: no role %s", who,
			            role ? json_object_to_json_string(role) : "named");
		if ((tokens && !json_object_is_type(tokens, json_type_array)) ||
		    (users && !json_object_is_type(users, json_type_array)))
			return fail(b, "identity %s: tokens and unix-users are arrays", who);
		size_t n = tokens ? json_object_array_length(tokens) : 0;
		size_t k = users ? json_object_array_length(users) : 0;
		if (!n && !k)
			return fail(b, "identity %s needs tokens or unix-users", who);
		uint64_t principal = principal_of(who);
		for (size_t i = 0; i < k; ++i) {
			struct json_object *user = json_object_array_get_idx(users, i);
			int64_t number = json_object_is_type(user, json_type_int)
			               ? json_object_get_int64(user) : -1;
			uint32_t uid = number >= 0 && number < UINT32_MAX ? (uint32_t)number : 0;
			int e = number >= 0 && number < UINT32_MAX ? 0
			      : json_object_is_type(user, json_type_string)
			      ? id_of(b, json_object_get_string(user), false, &uid) : EINVAL;
			if (e == ENOMEM)
				return e;
			if (e)
				return fail(b, "identity %s: no user %s%s%s", who,
				            json_object_to_json_string(user), e == ENOENT ? "" : ": ",
				            e == ENOENT ? "" : strerror(e));
			b->p->users[b->p->n_users++] = (struct uid_role){
				.uid = uid,
				.role = (uint32_t)r,
				.principal = principal
			};
		}
		for (size_t i = 0; i < n; ++i) {
			struct json_object *spec = json_object_array_get_idx(tokens, i);
			unsigned char digest[DIGEST];
			int e = json_object_is_type(spec, json_type_string)
			      ? token_spec(b, who, json_object_get_string(spec), digest)
			      : fail(b, "identity %s: a token is sha256:HEX or env:NAME", who);
			if (e)
				return e;
			token_add(b->p, digest, principal, (uint64_t)r);
		}
	}
	struct policy *p = b->p;
	if (p->n_tokens)
		qsort(p->tokens, p->n_tokens, sizeof *p->tokens, by_digest);
	for (size_t i = 1; i < p->n_tokens; ++i)
		if (!by_digest(&p->tokens[i - 1], &p->tokens[i]))
			return fail(b, "two tokens of the identities are the same");
	if (p->n_users)
		qsort(p->users, p->n_users, sizeof *p->users, by_id);
	for (size_t i = 1; i < p->n_users; ++i)
		if (p->users[i - 1].uid == p->users[i].uid)
			return fail(b, "two identities are Unix user %u", p->users[i].uid);
	return 0;
}

static int
unix_groups (struct build       *b,
             struct json_object *groups)
{
	if (!groups)
		return 0;
	if (!json_object_is_type(groups, json_type_object))
		return fail(b, "unix-groups must be an object");
	struct gid_role *g = b->p->groups;
	struct json_object_iter it;
	json_object_object_foreachC(groups, it) {
		int64_t r = json_object_is_type(it.val, json_type_string)
		          ? role_of(b, json_object_get_string(it.val)) : -1;
		if (r < 0)
			return fail(b, "unix-groups: %s: no role %s", it.key,
			            json_object_to_json_string(it.val));
		int e = id_of(b, it.key, true, &g->gid);
		if (e == ENOMEM)
			return e;
		if (e)
			return fail(b, "unix-groups: no group %s%s%s", it.key,
			            e == ENOENT ? "" : ": ", e == ENOENT ? "" : strerror(e));
		g++->role = (uint32_t)r;
		++b->p->n_groups;
	}
	return 0;
}

static int
anonymous (struct build       *b,
           struct json_object *anonymous)
{
	static char const *const kinds[POLICY_KINDS] = {
		[POLICY_LOOPBACK] = "loopback",
		[POLICY_REMOTE] = "remote",
		[POLICY_UNIX] = "unix",
	};
	if (!anonymous)
		return 0;
	if (!json_object_is_type(anonymous, json_type_object))
		return fail(b, "anonymous must be an object");
	struct json_object_iter it;
	json_object_object_foreachC(anonymous, it) {
		size_t k = 0;
		while (k < POLICY_KINDS && strcmp(kinds[k], it.key))
			++k;
		if (k == POLICY_KINDS)
			return fail(b, "anonymous: unknown member %s", it.key);
		/* null: none may connect so. */
		int64_t r = !it.val ? -1 : json_object_is_type(it.val, json_type_string)
		          ? role_of(b, json_object_get_string(it.val)) : -1;
		if (it.val && r < 0)
			return fail(b, "anonymous: %s: no role %s", it.key,
			            json_object_to_json_string(it.val));
		b->p->anonymous[k] = r;
	}
	return 0;
}

/* askai's own policy: with ASKAI_TOKEN, @p token, an identity of
 * everything, that clients must be; else everything for clients on loopback,
 * and the tools for others. */
static void
own (struct build *b,
     char const   *token)
{
	struct policy *p = b->p;
	uint32_t all = 0;
	for (size_t i = 0; i < b->n_tools; ++i)
		all |= b->tools[i] ? 1u << i : 0;
	p->roles[0] = (struct role){ .tools = all, .metrics = true };
	p->roles[1] = (struct role){ .tools = all };
	if (!token) {
		p->anonymous[POLICY_LOOPBACK] = 0;
		p->anonymous[POLICY_REMOTE] = 1;
		p->anonymous[POLICY_UNIX] = 0;
		return;
	}
	unsigned char digest[DIGEST];
	SHA256((unsigned char const *)token, strlen(token), digest);
	token_add(p, digest, principal_of("ASKAI_TOKEN"), 0);
}

/* The members of @p obj, if an object. */
static size_t
members (struct json_object *obj)
{
	return json_object_is_type(obj, json_type_object)
	     ? (size_t)json_object_object_length(obj) : 0;
}

/* The elements of the member @p key of @p obj, if an object's array. */
static size_t
elements (struct json_object *obj,
          char const         *key)
{
	struct json_object *v;
	return json_object_object_get_ex(obj, key, &v) &&
	       json_object_is_type(v, json_type_array) ? json_object_array_length(v) : 0;
}

int
policy_new (struct policy           **p_policy,
            struct json_object       *file,
            char const *const         tools[],
            size_t                    n_tools,
            char                     *why,
            size_t                    len)
{
	struct build b = {
		.tools = tools,
		.why = why,
		.n_tools = n_tools,
		.len = len
	};
	struct json_object *ids = nullptr;
	struct json_object *groups = nullptr;
	struct json_object *anon = nullptr;
	bool mine = json_object_object_get_ex(file, "roles", &b.roles) |
	            json_object_object_get_ex(file, "identities", &ids) |
	            json_object_object_get_ex(file, "unix-groups", &groups) |
	            json_object_object_get_ex(file, "anonymous", &anon);
	char const *token = mine ? nullptr : getenv("ASKAI_TOKEN");
	token = token && *token ? token : nullptr;

	/* Room for as many of each as the file names, or askai's own policy. */
	size_t n_tokens = token != nullptr;
	size_t n_users = 0;
	size_t n_roles = mine ? members(b.roles) : 2;
	size_t n_groups = members(groups);
	struct json_object_iter it;
	if (members(ids))
		json_object_object_foreachC(ids, it) {
			n_tokens += elements(it.val, "tokens");
			n_users += elements(it.val, "unix-users");
		}
	struct policy *p = calloc(1, sizeof *p + n_tokens * sizeof *p->tokens +
	                             n_users * sizeof *p->users +
	                             n_roles * sizeof *p->roles +
	                             n_groups * sizeof *p->groups);
	int e = p ? 0 : ENOMEM;
	if (p) {
		atomic_init(&p->refs, 1);
		p->tokens = (struct token *)(p + 1);
		p->users = (struct uid_role *)(p->tokens + n_tokens);
		p->roles = (struct role *)(p->users + n_users);
		p->groups = (struct gid_role *)(p->roles + n_roles);
		for (size_t k = 0; k < POLICY_KINDS; ++k)
			p->anonymous[k] = -1;
		b.p = p;
		if (!mine)
			own(&b, token);
		else if (!(e = roles(&b)) && !(e = identities(&b, ids)) &&
		         !(e = unix_groups(&b, groups)))
			e = anonymous(&b, anon);
	}
	free(b.buf);
	if (e) {
		policy_put(p);
		return e;
	}
	*p_policy = p;
	return 0;
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

static struct policy_grant
grant (struct policy const *p,
       uint64_t             role,
       uint64_t             principal)
{
	return (struct policy_grant){
		.principal = principal,
		.tools = p->roles[role].tools,
		.metrics = p->roles[role].metrics
	};
}

bool
policy_grant (struct policy const      *policy,
              char const               *token,
              size_t                    len,
              enum policy_kind          kind,
              struct policy_peer const *peer,
              struct policy_grant      *g)
{
	if (token && policy->n_tokens) {
		unsigned char digest[DIGEST];
		SHA256((unsigned char const *)token, len, digest);
		struct token const *t = bsearch(digest, policy->tokens, policy->n_tokens,
		                                sizeof *policy->tokens, by_digest);
		if (t)
			*g = grant(policy, t->role, t->principal);
		return t;
	}
	int64_t role = policy->anonymous[kind];
	uint64_t principal = 0;
	if (kind == POLICY_UNIX && peer) {
		struct uid_role const *u = !policy->n_users ? nullptr
		                         : bsearch(&peer->uid, policy->users, policy->n_users,
		                                   sizeof *policy->users, by_id);
		if (u) {
			*g = grant(policy, u->role, u->principal);
			return true;
		}
		principal = principal_of_uid(peer->uid);
		for (size_t i = 0; i < policy->n_groups; ++i) {
			if (bsearch(&policy->groups[i].gid, peer->groups, peer->n_groups,
			            sizeof *peer->groups, by_id)) {
				role = policy->groups[i].role;
				break;
			}
		}
	}
	if (role >= 0)
		*g = grant(policy, (uint64_t)role, principal);
	return role >= 0;
}
