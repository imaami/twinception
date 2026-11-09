/** @file notes.c
 *
 * The notes of the session (src/notes.h).
 */
#define _GNU_SOURCE /* memrchr() */
#include <errno.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "notes.h"
#include "oid.h"
#include "store.h"
#include "util.h"

/* The stages of a historical read or diff, in their order. */
enum notes_revision_phase : uint8_t {
	REVISION_HEAD,   /* git pins the tip of the notebook */
	REVISION_REACH,  /* git checks that the tip reaches from and to */
	REVISION_DATA,   /* git reads the note at from, or compares it with to */
};

int
notes_revision_init (struct notes_revision *dest,
                     struct cstr_view       file,
                     struct oid const      *from,
                     struct oid const      *to)
{
	if (!dest || !from || !to || notes_name(&file))
		return EINVAL;
	/* No name stands for the tip until the first stage pins it. */
	*dest = (struct notes_revision){ .from = *from, .to = *to, .phase = REVISION_HEAD };
	(void)util_text_store(&dest->file, file);
	return 0;
}

void
notes_revision_pin (struct notes_revision *revision,
                    struct oid const      *from,
                    struct oid const      *to)
{
	revision->from = *from;
	revision->to = *to;
	revision->phase = REVISION_DATA;
}

enum store_cmd
notes_revision_words (struct notes_revision const *revision,
                      char const                  *repo,
                      enum store_cmd               cmd,
                      char const                  *words[static STORE_WORDS])
{
	switch (revision->phase) {
	case REVISION_HEAD:
		words[0] = repo;
		return STORE_HEAD;
	case REVISION_REACH:
		return STORE_REACH;
	case REVISION_DATA:
		break;
	}
	words[0] = revision->from.hex;
	words[1] = revision->to.hex;
	words[2] = revision->file.data;
	return cmd;
}

int
notes_revision_input (struct notes_revision const *revision,
                      struct buf                  *in)
{
	if (!revision || !in)
		return EINVAL;
	return buf_printf(in, "%s^{commit}\n%s^{commit}\n^%s\n", revision->from.hex,
	                  revision->to.hex, revision->tip.hex);
}

int
notes_revision_next (struct notes_revision *revision,
                     struct span            out)
{
	struct oid const *const tip = &revision->tip;
	switch (revision->phase) {
	case REVISION_HEAD:
		if (oid_init_line(&revision->tip,
		                  (struct cstr_view){ (char const *)out.ptr, out.len }))
			return EIO;
		if (!oid_len(&revision->from))
			revision->from = *tip;
		if (!oid_len(&revision->to))
			revision->to = *tip;
		if (oid_len(&revision->from) != oid_len(tip) || oid_len(&revision->to) != oid_len(tip))
			return EINVAL;
		/* The tip reaches itself. If from and to are the tip, as for
		 * search, the check does not run. */
		revision->phase = oid_equal(&revision->from, tip) && oid_equal(&revision->to, tip)
		                ? REVISION_DATA : REVISION_REACH;
		return 0;
	case REVISION_REACH:
		/* git rev-list writes no commit if the tip reaches both. */
		if (out.len)
			return EIO;
		revision->phase = REVISION_DATA;
		return 0;
	case REVISION_DATA:
		break;
	}
	return EINVAL;
}

bool
notes_revision_ready (struct notes_revision const *revision)
{
	return revision->phase == REVISION_DATA;
}

/* Returns true if the part of a name of @p n bytes at @p part is a name
 * that NTFS would read as .git: ".git" or "git~1" in any case, followed
 * only by spaces and dots up to the end, a backslash or a colon. The check
 * looks at the part and at each piece of it after a backslash. git refuses
 * such names on every system (core.protectNTFS), so askai refuses them
 * before git runs.
 */
static bool
notes_is_dotgit (char const *part,
                 size_t      n)
{
	for (size_t at = 0; at <= n; ) {
		char const *s = part + at;
		struct cstr_view const rest = { s, n - at };
		size_t k = cstr_view_starts_case(rest, CSTR_VIEW(".git")) ? 4
		         : cstr_view_starts_case(rest, CSTR_VIEW("git~1")) ? 5 : 0;
		while (k && k < rest.len && (s[k] == ' ' || s[k] == '.'))
			++k;
		if (k && (k == rest.len || s[k] == ':' || s[k] == '\\'))
			return true;
		char const *bs = memchr(s, '\\', rest.len);
		if (!bs)
			return false;
		at += (size_t)(bs - s) + 1;
	}
	return false;
}

int
notes_name (struct cstr_view *name)
{
	size_t k = 0;
	while (k < name->len && name->data[k] == '/')
		++k;
	*name = cstr_view_slice(*name, k, SIZE_MAX);
	char const *s = name->data;
	size_t const len = name->len;
	if (!len || len > NOTES_NAME)
		return EINVAL;
	for (size_t i = 0; i < len; ++i)
		if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f)
			return EINVAL;
	/* The last part of a name that ends with a slash is empty. */
	for (size_t at = 0; at <= len; ) {
		char const *part = s + at;
		char const *slash = memchr(part, '/', len - at);
		size_t n = slash ? (size_t)(slash - part) : len - at;
		if (!n || (n == 1 && part[0] == '.') ||
		    (n == 2 && part[0] == '.' && part[1] == '.') || notes_is_dotgit(part, n))
			return EINVAL;
		at += n + 1;
	}
	return 0;
}

int
notes_file_same (struct notes_file const *file,
                 struct span              data,
                 bool                    *same)
{
	if (!file || !same || !oid_len(&file->id))
		return EINVAL;
	*same = false;
	if (data.len != file->bytes)
		return 0;
	/* The id of a blob is the digest of "blob SIZE", a NUL and the bytes:
	 * SHA-1 for 40 digits, SHA-256 for 64. */
	char head[sizeof "blob " + UTIL_DECIMAL];
	int h = snprintf(head, sizeof head, "blob %zu", data.len);
	if (h < 0 || h >= (int)sizeof head)
		return EINVAL;
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned n = 0;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	bool ok = ctx &&
	          EVP_DigestInit_ex(ctx, oid_len(&file->id) == OID_SHA1 ? EVP_sha1() : EVP_sha256(),
	                            nullptr) == 1 &&
	          EVP_DigestUpdate(ctx, head, (size_t)h + 1) == 1 &&
	          EVP_DigestUpdate(ctx, data.ptr, data.len) == 1 &&
	          EVP_DigestFinal_ex(ctx, digest, &n) == 1 && 2 * n == oid_len(&file->id);
	EVP_MD_CTX_free(ctx);
	if (!ok)
		return EIO;
	struct oid got = {};
	util_hex(got.hex, digest, n);
	*same = oid_equal(&got, &file->id);
	return 0;
}

int
notes_read_input (struct buf       *in,
                  char const       *repo,
                  struct cstr_view  name)
{
	if (!in || !repo || !name.data)
		return EINVAL;
	return buf_printf(in, "%s:%.*s\n", repo, (int)name.len, name.data);
}

int
notes_read_out (struct span  out,
                struct span *text,
                struct oid  *id)
{
	struct oid oid;
	int e = store_object_next(&out, &oid, text);
	/* The output is the one object. */
	if (!e && out.len) {
		*text = (struct span){};
		e = EIO;
	}
	if (id)
		*id = e ? (struct oid){} : oid;
	return e;
}

/* Returns the offset of the start of the token that ends with the NUL at
 * the offset @p nul in @p s.
 */
static size_t
notes_token (char const *s,
             size_t      nul)
{
	char const *prev = memrchr(s, '\0', nul);
	return prev ? (size_t)(prev - s) + 1 : 0;
}

int
notes_file_next (struct span       *out,
                 struct notes_file *file)
{
	*file = (struct notes_file){};
	if (!out->len)
		return ENOENT;
	char const *s = (char const *)out->ptr;
	char const *nul = memchr(s, '\0', out->len);
	char const *tab = nul ? memchr(s, '\t', (size_t)(nul - s)) : nullptr;
	/* The record is "MODE blob OID SIZE\tNAME\0", with spaces before SIZE. */
	struct cstr_view rest = { s, tab ? (size_t)(tab - s) : 0 };
	struct cstr_view mode, type;
	struct oid id;
	if (!tab || tab + 1 == nul || !cstr_view_cut(&rest, ' ', &mode) ||
	    !cstr_view_cut(&rest, ' ', &type) || !cstr_view_equal(type, CSTR_VIEW("blob")) ||
	    oid_init_field(&id, &rest, ' '))
		return EIO;
	struct cstr_view const size = cstr_view_trim(rest);
	uint64_t bytes;
	if (util_number(size.data, size.len, UINT64_MAX, &bytes))
		return EIO;
	*file = (struct notes_file){
		.name = { out->ptr + (tab + 1 - s), (size_t)(nul - tab - 1) },
		.bytes = bytes,
		.id = id,
	};
	size_t consumed = (size_t)(nul + 1 - s);
	out->ptr += consumed;
	out->len -= consumed;
	return 0;
}

/* Finds the header of the commit before the numstat token at @p stat in
 * @p out, the output of STORE_LOG, and stores its commit, time, asker,
 * subject and end in @p cursor. Returns false if no header precedes
 * @p stat, as in an output cut short.
 */
static bool
notes_cursor_header (struct span          out,
                     struct notes_cursor *cursor,
                     size_t               stat)
{
	char const *s = (char const *)out.ptr;
	for (size_t before = stat; before; ) {
		size_t token = notes_token(s, before - 1);
		size_t len = before - 1 - token;
		char const *space = memchr(s + token, ' ', len);
		size_t digits = space ? (size_t)(space - s - token) : len;
		struct cstr_view rest = cstr_view_slice((struct cstr_view){ s + token, len },
		                                        digits + 1, SIZE_MAX);
		struct cstr_view asker;
		if (!oid_init_field(&cursor->commit, &rest, ' ') && cstr_view_cut(&rest, ' ', &asker) &&
		    !util_number(s + token, digits, UINT64_MAX, &cursor->time)) {
			cursor->asker = (struct span){ out.ptr + (asker.data - s), asker.len };
			cursor->subject = (struct span){ out.ptr + (rest.data - s), rest.len };
			cursor->first = before;
			return true;
		}
		before = token;
	}
	return false;
}

bool
notes_entry_before (struct span          out,
                    struct notes_cursor *cursor,
                    struct notes_entry  *entry)
{
	char const *s = (char const *)out.ptr;
	size_t at = cursor->end;
	/* Git emits TIMESTAMP SPACE COMMIT SPACE ASKER SPACE SUBJECT\0, then
	 * one numstat token per file. ASKER can be empty. Only the first file
	 * token starts with a newline. A numstat token has no space before its
	 * first tab, and a header has one, so neither can pass as the other. A
	 * cut output may end within a token. */
	if (at && s[at - 1]) {
		char const *nul = memrchr(s, '\0', at);
		at = nul ? (size_t)(nul - s) + 1 : 0;
	}
	while (at) {
		size_t stat = notes_token(s, at - 1);
		size_t start = stat + (s[stat] == '\n');
		char const *tab = memchr(s + start, '\t', at - 1 - start);
		char const *name = tab ? memchr(tab + 1, '\t', (size_t)(s + at - 1 - tab - 1))
		                       : nullptr;
		uint64_t added = 0, removed = 0;
		size_t digits = tab ? (size_t)(tab - s - start) : 0;
		bool binary = digits == 1 && s[start] == '-';
		if (!name || name + 1 == s + at - 1 ||
		    (!binary && (util_number(s + start, digits, UINT64_MAX, &added) ||
		                 util_number(tab + 1, (size_t)(name - tab - 1), UINT64_MAX,
		                             &removed)))) {
			at = stat;
			cursor->first = 0;
			continue;
		}
		/* Find this commit's header once, and reuse it for the other
		 * files of the commit, which the reverse iterator reads later. */
		if ((!cursor->first || stat < cursor->first) &&
		    !notes_cursor_header(out, cursor, stat))
			break;
		*entry = (struct notes_entry){
			.name = { (unsigned char *)(uintptr_t)(name + 1),
			          (size_t)(s + at - 1 - name - 1) },
			.commit = cursor->commit,
			.subject = cursor->subject,
			.asker = cursor->asker,
			.time = cursor->time,
			.added = binary ? NOTES_BINARY : added,
			.removed = removed,
		};
		cursor->end = stat;
		return true;
	}
	cursor->end = 0;
	return false;
}
