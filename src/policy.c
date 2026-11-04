/** @file policy.c
 *
 * Identities and roles.
 */
#include <errno.h>
#include <grp.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jb.h"
#include "log.h"
#include "policy.h"
#include "util.h"

enum { DIGEST = SHA256_DIGEST_LENGTH };

/* The bit of a principal that marks an identity. A Unix user's principal
 * has it clear, and no principal has bit 63 set, so the principals of the
 * two kinds differ, and none has the bit LEDGER_ANONYMOUS (src/ledger.h).
 */
static constexpr uint64_t POLICY_IDENTITY = UINT64_C(1) << 62;

/* The members of "anonymous", by the kind of client. TOOL_TEXT() is a
 * compound literal, which a static initializer may not have.
 */
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

/* A Unix group, by gid first, and its role. */
struct policy_gid {
	uint32_t gid;
	uint32_t role;
};

/* A range of subordinate ids from /etc/subuid or /etc/subgid, by its first
 * id first.
 */
struct policy_range {
	uint32_t first;
	uint32_t last;
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

/* One block has a policy and, after it, its tokens, roles, groups,
 * subordinate uids and the names of its clients for the log, in that
 * order, so that each is aligned.
 */
struct policy {
	tool_mask            allowed;                  /* the tools that the
	                                                  roles may grant */
	_Atomic uint64_t     refs;
	struct policy_token *tokens;                   /* by digest */
	struct policy_role  *roles;
	struct policy_gid   *groups;                   /* in the configuration's
	                                                  order, which the first
	                                                  that a user is in wins */
	struct policy_range *subuids;                  /* the ranges of
	                                                  /etc/subuid, sorted and
	                                                  merged */
	size_t               n_tokens;
	size_t               n_roles;
	size_t               n_groups;
	size_t               n_subuids;
	int64_t              anonymous[POLICY_KINDS];  /* a role; -1: none */
	struct tool_text     strangers[POLICY_KINDS];  /* the names of anonymous
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
	int                  overflow_errors[2];       /* The errors that kept
	                                                  the overflow uid and
	                                                  gid from being read,
	                                                  or 0 for an id that
	                                                  was read. An id that
	                                                  was not read is
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
static_assert(alignof (struct policy_token) <= alignof (struct policy) &&
              alignof (struct policy_role) <= alignof (struct policy_token) &&
              alignof (struct policy_gid) <= alignof (struct policy_role) &&
              alignof (struct policy_range) <= alignof (struct policy_gid));

/* What policy_create() works from, and where it says what is wrong. */
struct policy_build {
	tool_mask            upstream;  /* the tools that askai can offer to
	                                   the upstream */
	struct policy       *p;
	struct json_object  *roles;     /* the member roles of the file, or
	                                   nullptr */
	char const *const   *tools;
	char                *why;
	char                *buf;       /* the buffer for group lookups, which
	                                   policy_create() frees at the end */
	char                *names;     /* the place in the policy's block
	                                   where the next name for the log is
	                                   written */
	struct policy_range *subgids;   /* the ranges of /etc/subgid, sorted
	                                   and merged, which policy_create()
	                                   frees at the end */
	size_t               n_tools;
	size_t               len;
	size_t               size;      /* the size of buf */
	size_t               n_subgids;
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

/* The length of @p name, a role's or an identity's; 0 if it is not a
 * valid name.
 */
static size_t
policy_name_len (char const *name)
{
	size_t n = strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	                        "0123456789_.@-");
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
	uint64_t h = 0;
	for (size_t i = 0; i < sizeof h; ++i)
		h = h << 8 | md[i];
	h = (h & (POLICY_IDENTITY - 1)) | tag;
	*principal = h + !h;
	return 0;
}

/* Compute in @p *principal the principal of the identity that @p who,
 * "identity NAME", names, with the key of @p policy. Returns 0, EINVAL if
 * @p who has another form, or EIO if HMAC() fails.
 */
static int
policy_principal_identity (struct policy const *policy,
                           struct tool_text     who,
                           uint64_t            *principal)
{
	static char const prefix[] = "identity ";
	if (who.len < sizeof prefix - 1 || memcmp(who.ptr, prefix, sizeof prefix - 1))
		return EINVAL;
	char anchor[sizeof "identity:" + POLICY_NAME];
	int n = snprintf(anchor, sizeof anchor, "identity:%.*s",
	                 (int)(who.len - (sizeof prefix - 1)), who.ptr + sizeof prefix - 1);
	if (n < 0 || n >= (int)sizeof anchor)
		return EINVAL;
	return policy_principal_of(policy, POLICY_IDENTITY, anchor, (size_t)n, principal);
}

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

/* Copy the @p n texts @p parts, one after another, into the policy's
 * block, and return the copy, which names a client in the log.
 */
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

/* Append a token of @p digest for the identity @p who of @p role.
 * policy_principals() computes its principal.
 */
static void
policy_token_add (struct policy       *p,
                  unsigned char const *digest,
                  struct tool_text     who,
                  uint64_t             role)
{
	struct policy_token *t = &p->tokens[p->n_tokens++];
	memcpy(t->digest, digest, DIGEST);
	t->who = who;
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
policy_range_parse (char const          *line,
                    struct policy_range *r)
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
	*r = (struct policy_range){
		.first = (uint32_t)start,
		.last = (uint32_t)(start + (count - 1 < room ? count - 1 : room))
	};
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
 * refuses are skipped. A missing file has no ranges. Returns 0, ENOMEM,
 * or the error that kept the file from being read, with the reason in
 * b->why.
 */
static int
policy_build_ranges (struct policy_build  *b,
                     char const           *path,
                     struct policy_range **p_ranges,
                     size_t               *p_n)
{
	*p_ranges = nullptr;
	*p_n = 0;
	FILE *f = fopen(path, "re");
	if (!f)
		return errno == ENOENT ? 0 : policy_build_unreadable(b, path, errno);
	int e = 0;
	struct policy_range *ranges = nullptr;
	size_t n = 0;
	size_t room = 0;
	char *line = nullptr;
	size_t size = 0;
	while (getline(&line, &size, f) >= 0) {
		struct policy_range r;
		if (!policy_range_parse(line, &r))
			continue;
		if (n == room) {
			room = room ? 2 * room : 16;
			struct policy_range *more = reallocarray(ranges, room, sizeof *ranges);
			if (!more) {
				e = ENOMEM;
				break;
			}
			ranges = more;
		}
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
		ranges = nullptr;
		return policy_build_unreadable(b, path, e);
	}
	if (n)
		qsort(ranges, n, sizeof *ranges, policy_by_id);
	size_t k = 0;
	for (size_t i = 1; i < n; ++i) {
		if (ranges[i].first > (uint64_t)ranges[k].last + 1)
			ranges[++k] = ranges[i];
		else if (ranges[i].last > ranges[k].last)
			ranges[k].last = ranges[i].last;
	}
	*p_ranges = ranges;
	*p_n = n ? k + 1 : 0;
	return 0;
}

/* Whether @p id lies in one of the @p n sorted and disjoint @p ranges. */
static bool
policy_ranges_have (struct policy_range const *ranges,
                    size_t                     n,
                    uint32_t                   id)
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
	FILE *f = fopen(path, "re");
	if (!f)
		return errno;
	char text[16];
	int e = fgets(text, sizeof text, f) ? 0 : ferror(f) ? errno : EILSEQ;
	(void)fclose(f);
	if (e)
		return e;
	text[strcspn(text, "\n")] = '\0';
	return policy_number(text, id) ? 0 : EILSEQ;
}

/* Bytes of the buffer for one group entry from NSS, at most. A group
 * entry lists the names of its members, so the entry of a large directory
 * group can take megabytes.
 */
static constexpr size_t POLICY_ENTRY_MAX = 64 << 20;

/* Look up the Unix group @p name of unix-groups, a gid in decimal or a
 * group name that NSS resolves, and store its gid in @p *gid. The lookup
 * buffer starts at 1 KiB and doubles while getgrnam_r() reports ERANGE, up
 * to POLICY_ENTRY_MAX bytes. The message says "no group" only if the
 * lookup succeeded and found no such group. Returns 0, ENOMEM, or EINVAL
 * with the reason in b->why.
 */
static int
policy_build_gid (struct policy_build *b,
                  char const          *name,
                  uint32_t            *gid)
{
	if (policy_number(name, gid))
		return 0;
	struct group gr;
	struct group *r = nullptr;
	int e = 0;
	while (!b->buf || (e = getgrnam_r(name, &gr, b->buf, b->size, &r)) == ERANGE) {
		size_t size = b->size ? 2 * b->size : 1 << 10;
		if (size > POLICY_ENTRY_MAX)
			return policy_build_fail(b, "unix-groups: the entry of group %s is "
			                         "larger than %zu MiB; give its gid instead",
			                         name, POLICY_ENTRY_MAX >> 20);
		char *buf = realloc(b->buf, size);
		if (!buf)
			return ENOMEM;
		b->buf = buf;
		b->size = size;
	}
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
 * returns the length.
 */
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
 * tool, or with @p up lends no such tool to the upstream. The reason lists
 * the tools that askai offers or lends. Returns EINVAL.
 */
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

/* Reads the list of tool names @p list of role @p name into @p r. @p key
 * is the name of the list's member, "tools" or "upstream". The member
 * "upstream" may name only the tools that askai can offer to the upstream.
 */
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

/* The members of identity @p who, @p id: its role and its tokens. */
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
	struct json_object_iter m;
	json_object_object_foreachC(id, m) {
		if (!strcmp(m.key, "role"))
			role = m.val;
		else if (!strcmp(m.key, "tokens"))
			tokens = m.val;
		else
			return policy_build_fail(b, "identity %s: unknown member %s", who, m.key);
	}
	int64_t r = policy_build_role(b, role);
	if (r < 0)
		return policy_build_fail(b, "identity %s: no role %s", who,
		                         role ? json_object_to_json_string(role) : "named");
	if (tokens && !json_object_is_type(tokens, json_type_array))
		return policy_build_fail(b, "identity %s: tokens must be an array", who);
	size_t n = jb_len(tokens);
	if (!n)
		return policy_build_fail(b, "identity %s needs tokens", who);
	struct tool_text name = policy_build_who(b, 2, (struct tool_text const[]){
		TOOL_TEXT("identity "), { who, len } });
	for (size_t i = 0; i < n; ++i) {
		struct json_object *spec = json_object_array_get_idx(tokens, i);
		unsigned char digest[DIGEST];
		if (!json_object_is_type(spec, json_type_string))
			return policy_build_fail(b, "identity %s: a token is sha256:HEX or "
			                         "env:NAME", who);
		int e = policy_build_token(b, who, json_object_get_string(spec), digest);
		if (e)
			return e;
		policy_token_add(b->p, digest, name, (uint64_t)r);
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
		g++->role = (uint32_t)r;
		++b->p->n_groups;
	}
	return 0;
}

/* The role, if any, of a client that shows no token and connects as
 * @p kind, which @p val, null for none, names.
 */
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
	tool_mask all = policy_build_all(b);
	p->roles[0] = (struct policy_role){ .tools = all, .upstream = all, .metrics = true };
	p->roles[1] = (struct policy_role){ .tools = all, .upstream = all };
	if (!token) {
		p->anonymous[POLICY_LOOPBACK] = 0;
		p->anonymous[POLICY_REMOTE] = 1;
		p->anonymous[POLICY_UNIX] = 0;
		return;
	}
	unsigned char digest[DIGEST];
	SHA256((unsigned char const *)token, strlen(token), digest);
	policy_token_add(p, digest, TOOL_TEXT("identity ASKAI_TOKEN"), 0);
	p->unix_token = true;
}

/* The members of @p obj, if an object. */
static size_t
policy_members (struct json_object *obj)
{
	return json_object_is_type(obj, json_type_object)
	     ? (size_t)json_object_object_length(obj) : 0;
}

/* The members of the configuration file's object @p file that make the
 * policy, each left as it is if absent; whether @p file has any of them,
 * which turns askai's own policy off.
 */
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
		json_object_object_foreachC(ids, it)
			n_tokens += jb_len(jb_get(it.val, "tokens"));
	/* The subordinate uids go into the policy's block, and the
	 * subordinate gids are needed only to check unix-groups. */
	struct policy_range *subuids;
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
		subuids = nullptr;
		free(b.subgids);
		b.subgids = nullptr;
		return nullptr;
	}
	atomic_init(&p->refs, 1);
	p->tokens = (struct policy_token *)(p + 1);
	p->roles = (struct policy_role *)(p->tokens + n_tokens);
	p->groups = (struct policy_gid *)(p->roles + n_roles);
	p->subuids = (struct policy_range *)(p->groups + n_groups);
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
		p->anonymous[k] = -1;
	b.p = p;
	if (mine)
		*e = policy_build_file(&b, ids, groups, anon);
	else
		policy_build_own(&b, token);
	free(b.buf);
	b.buf = nullptr;
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
 * none.
 */
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
 * the tools that the roles grant or lend.
 */
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
 * its role.
 */
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
		struct tool_text who = policy->roles[g++->role].who;
		LOG_ALL(LOG_INFO, "  %-*s  %.*s", width, it.key, (int)who.len, who.ptr);
	}
}

/* Log askai's own policy @p policy, and if @p lent is not nullptr, the
 * tools of the names @p tools of @p n_tools that the upstream may call,
 * which @p lent returns with @p ctx. The keys are padded to the width of
 * the longest.
 */
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

/* Write the label of the uids of @p r into the @p size bytes at @p buf:
 * "uid N" for one uid, else "uids FIRST-LAST". Returns its length.
 */
static size_t
policy_range_label (char                *buf,
                    size_t               size,
                    struct policy_range  r)
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
	for (size_t i = 0; i < sizeof files / sizeof *files; ++i)
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
		policy_log_peers(policy);
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

/* Grant @p role, if it is one, to the client of @p principal, which the
 * log names as @p who, or else as its role.
 */
static enum policy_verdict
policy_grant_role (struct policy const *policy,
                   int64_t              role,
                   uint64_t             principal,
                   struct tool_text     who,
                   struct policy_grant *g)
{
	if (role < 0)
		return POLICY_REFUSED;
	struct policy_role const *r = &policy->roles[role];
	*g = (struct policy_grant){
		.principal = principal,
		.tools = r->tools,
		.upstream = r->upstream,
		.who = who.len ? who : r->who,
		.metrics = r->metrics
	};
	return POLICY_GRANTED;
}

/* Grant the client that shows the token of @p len bytes at @p token the
 * role of the identity it proves, if any.
 */
static enum policy_verdict
policy_grant_token (struct policy const *policy,
                    char const          *token,
                    size_t               len,
                    struct policy_grant *g)
{
	unsigned char digest[DIGEST];
	SHA256((unsigned char const *)token, len, digest);
	struct policy_token const *t = bsearch(digest, policy->tokens, policy->n_tokens,
	                                       sizeof *policy->tokens, policy_by_digest);
	if (!t)
		return POLICY_REFUSED;
	return policy_grant_role(policy, (int64_t)t->role, t->principal, t->who, g);
}

/* Grant Unix user @p peer, who shows no token, the role of its first group
 * that the policy names, or else the anonymous role over a Unix socket.
 * The log names the client by its role, and mcp.c adds the uid.
 */
static enum policy_verdict
policy_grant_unix (struct policy const      *policy,
                   struct policy_peer const *peer,
                   struct policy_grant      *g)
{
	int64_t role = policy->anonymous[POLICY_UNIX];
	for (size_t i = 0; i < policy->n_groups; ++i) {
		if (bsearch(&policy->groups[i].gid, peer->groups, peer->n_groups,
		            sizeof *peer->groups, policy_by_id)) {
			role = policy->groups[i].role;
			break;
		}
	}
	return policy_grant_role(policy, role, peer->principal,
	                         (struct tool_text){}, g);
}

enum policy_verdict
policy_grant (struct policy const      *policy,
              char const               *token,
              size_t                    len,
              enum policy_kind          kind,
              struct policy_peer const *peer,
              struct policy_grant      *g)
{
	if (kind != POLICY_UNIX || !peer) {
		if (token && policy->n_tokens)
			return policy_grant_token(policy, token, len, g);
		return policy_grant_role(policy, policy->anonymous[kind], 0,
		                         policy->strangers[kind], g);
	}
	if (!token)
		return policy_grant_unix(policy, peer, g);
	if (!policy->unix_token)
		return POLICY_UNIX_TOKEN;
	/* ASKAI_TOKEN under askai's own policy grants its role, and the calls
	 * remain those of the Unix user. */
	if (policy_grant_token(policy, token, len, g) != POLICY_GRANTED)
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
