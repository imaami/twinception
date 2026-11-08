/** @file notes.c
 *
 * The notes tool.
 */
#define _GNU_SOURCE /* memrchr() */
#include <errno.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hexid.h"
#include "notes.h"
#include "oid.h"
#include "store.h"
#include "util.h"

/* Only this namespace is exposed by the notes tool. */
#define BRANCH "refs/heads/notes/"

struct notes {
	struct store *store;  /* the shared session repository */
	char         *agent;  /* the PARTICIPANT of the refs of this askai */
	size_t        named;  /* the bytes of agent */
};

/* The stages of a historical read or diff, in their order. */
enum notes_revision_phase : uint8_t {
	REVISION_HEAD,  /* git pins the tip of the notebook */
	REVISION_FROM,  /* git checks that from is an ancestor of the tip */
	REVISION_TO,    /* git checks that to is one too */
	REVISION_DATA,  /* git reads the note at from, or compares it with to */
};

/* The kinds of the objects in the output of git cat-file. */
enum notes_kind {
	KIND_MISSING,
	KIND_BLOB,
	KIND_TREE,
	KIND_COMMIT,
	KIND_OTHER,
};

struct notes *
notes_create (char const *path,
              char const *agent,
              char       *why,
              size_t      len,
              int        *e)
{
	if (!agent || !member_name_valid(cstr_view(agent))) {
		*e = util_refuse(why, len, "the agent needs a member name");
		return nullptr;
	}
	size_t const named = strlen(agent);
	struct notes *notes = malloc(sizeof *notes);
	char *copy = strdup(agent);
	if (!notes || !copy) {
		free(notes);
		free(copy);
		util_format(why, len, "out of memory");
		*e = ENOMEM;
		return nullptr;
	}
	*notes = (struct notes){ .agent = copy, .named = named };
	if (!(notes->store = store_create(path, why, len, e)))
		notes_destroy(&notes);
	return notes;
}

void
notes_destroy (struct notes **p_notes)
{
	if (!p_notes || !*p_notes)
		return;
	struct notes *notes = *p_notes;
	*p_notes = nullptr;
	store_destroy(&notes->store);
	free(notes->agent);
	notes->agent = nullptr;
	free(notes);
}

struct store const *
notes_store (struct notes const *notes)
{
	return notes ? notes->store : nullptr;
}

char const *
notes_writer (struct notes const *notes)
{
	return notes->agent;
}

void
notes_book (struct cstr_view participant,
            uint64_t         principal,
            enum notes_side  side,
            char             repo[NOTES_REPO])
{
	static_assert(NOTES_REPO >= sizeof BRANCH + UTIL_WORD_MAX + 1 + NOTES_WHO + 1 +
	                            sizeof "upstream" - 1);
	char hex[NOTES_WHO + 1];
	struct cstr_view const who = principal
	                           ? util_format_view(hex, sizeof hex, "%0*" PRIx64, NOTES_WHO,
	                                              principal)
	                           : notes_anonymous;
	util_format(repo, NOTES_REPO, BRANCH "%.*s/%.*s/%s", (int)participant.len,
	            participant.data, (int)who.len, who.data, notes_sides[side].data);
}

void
notes_repo (struct notes const *notes,
            uint64_t            principal,
            enum notes_side     side,
            char                repo[NOTES_REPO])
{
	notes_book((struct cstr_view){ notes->agent, notes->named }, principal, side, repo);
}

int
notes_select (struct cstr_view name,
              char             repo[NOTES_REPO])
{
	if (!name.data || sizeof BRANCH + name.len > NOTES_REPO || !notes_selector_valid(name))
		return EINVAL;
	if (repo)
		util_format(repo, NOTES_REPO, BRANCH "%.*s", (int)name.len, name.data);
	return 0;
}

char const *
notes_selector (char const *repo)
{
	return repo + sizeof BRANCH - 1;
}

char const *const notes_books[] = { "--format=%(refname:strip=3)", BRANCH };

#undef BRANCH

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
	enum store_cmd const phases[] = {
		[REVISION_HEAD] = STORE_HEAD,
		[REVISION_FROM] = STORE_ANCESTOR,
		[REVISION_TO]   = STORE_ANCESTOR,
		[REVISION_DATA] = cmd,
	};
	char const *const rows[][STORE_WORDS] = {
		[REVISION_HEAD] = { repo },
		[REVISION_FROM] = { revision->from.hex, revision->tip.hex },
		[REVISION_TO]   = { revision->to.hex, revision->tip.hex },
		[REVISION_DATA] = { revision->from.hex, revision->to.hex, revision->file.data },
	};
	memcpy(words, rows[revision->phase], sizeof rows[0]);
	return phases[revision->phase];
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
		if (!oid_equal(&revision->from, tip)) {
			revision->phase = REVISION_FROM;
			return 0;
		}
		break;
	case REVISION_FROM:
		/* git merge-base --is-ancestor writes nothing. */
		if (out.len)
			return EIO;
		break;
	case REVISION_TO:
		if (out.len)
			return EIO;
		revision->phase = REVISION_DATA;
		return 0;
	case REVISION_DATA:
		return EINVAL;
	}
	revision->phase = oid_equal(&revision->to, tip) || oid_equal(&revision->to, &revision->from)
	                ? REVISION_DATA : REVISION_TO;
	return 0;
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
notes_check_input (struct buf       *in,
                   char const       *repo,
                   struct cstr_view  name)
{
	if (!in || !repo || !name.data)
		return EINVAL;
	/* Write a line for the branch, a line for each directory in the name,
	 * and a line for the name. */
	int e = buf_printf(in, "%s\n", repo);
	for (size_t i = 0; !e && i <= name.len; ++i)
		if (i == name.len || name.data[i] == '/')
			e = buf_printf(in, "%s:%.*s\n", repo, (int)i, name.data);
	return e;
}

/* Returns the kind of the object of @p line, a line without its newline in
 * the output of git cat-file: "OID TYPE SIZE", or "NAME missing". Stores
 * OID in @p *oid, or no name if the line is KIND_MISSING or OID is not a
 * name, which makes the line KIND_OTHER. Stores SIZE in @p *size if the
 * line has a TYPE.
 */
static enum notes_kind
notes_kind (struct cstr_view  line,
            struct oid       *oid,
            struct cstr_view *size)
{
	static struct cstr_view const types[] = {
		[KIND_BLOB] = CSTR_VIEW_INIT("blob"), [KIND_TREE] = CSTR_VIEW_INIT("tree"),
		[KIND_COMMIT] = CSTR_VIEW_INIT("commit"),
	};
	*oid = (struct oid){};
	if (cstr_view_ends(line, CSTR_VIEW(" missing")))
		return KIND_MISSING;
	struct cstr_view type;
	if (oid_init_field(oid, &line, ' ') || !cstr_view_cut(&line, ' ', &type))
		return KIND_OTHER;
	*size = line;
	return KIND_BLOB + cstr_view_index(types + KIND_BLOB, KIND_OTHER - KIND_BLOB, type);
}

int
notes_checked (struct span        out,
               struct oid        *tip,
               struct notes_file *note)
{
	if (!tip || !note)
		return EINVAL;
	*tip = (struct oid){};
	*note = (struct notes_file){};
	/* The first line is about the branch, the last line is about the name,
	 * and the lines between are about the directories in the name. */
	size_t lines = 0;
	bool dir_note = false;
	enum notes_kind kind = KIND_OTHER;
	struct cstr_view rest = { (char const *)out.ptr, out.len };
	struct cstr_view line, size = {};
	struct oid oid;
	for (; rest.len; ++lines) {
		if (!cstr_view_cut(&rest, '\n', &line))
			return EIO;
		dir_note |= lines > 1 && kind == KIND_BLOB;
		kind = notes_kind(line, &oid, &size);
		if (lines || kind == KIND_MISSING)
			continue;
		if (kind != KIND_COMMIT)
			return EIO;
		*tip = oid;
	}
	int r = lines < 2 || kind == KIND_OTHER || kind == KIND_COMMIT ? EIO
	      : dir_note ? ENOTDIR : kind == KIND_TREE ? EISDIR
	      : kind == KIND_BLOB ? EEXIST : 0;
	if (r != EEXIST)
		return r;
	/* The last line is "OID blob SIZE". */
	uint64_t bytes;
	if (util_number(size.data, size.len, UINT64_MAX, &bytes))
		return EIO;
	*note = (struct notes_file){ .bytes = bytes, .id = oid };
	return EEXIST;
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
	*text = (struct span){};
	if (id)
		*id = (struct oid){};
	struct cstr_view rest = { (char const *)out.ptr, out.len };
	struct cstr_view line;
	if (!cstr_view_cut(&rest, '\n', &line))
		return EIO;
	struct oid oid;
	struct cstr_view digits;
	enum notes_kind kind = notes_kind(line, &oid, &digits);
	if (kind == KIND_MISSING)
		return ENOENT;
	if (kind == KIND_TREE)
		return EISDIR;
	if (kind != KIND_BLOB)
		return EIO;
	/* A line feed comes after the text. */
	uint64_t size;
	if (util_number(digits.data, digits.len, UINT64_MAX, &size) ||
	    !rest.len || size != rest.len - 1 || rest.data[size] != '\n')
		return EIO;
	*text = (struct span){ out.ptr + line.len + 1, (size_t)size };
	if (id)
		*id = oid;
	return 0;
}

void
notes_review (char const         *repo,
              struct hexid const *thread,
              char                ref[NOTES_REPO])
{
	static_assert(NOTES_REPO >= sizeof "refs/review/" + UTIL_WORD_MAX + 1 + NOTES_WHO + 1 +
	                            HEXID_DIGITS);
	char const *who = notes_selector(repo);
	util_format(ref, NOTES_REPO, "refs/review/%.*s/%s", (int)(strrchr(who, '/') - who), who,
	            thread->hex);
}

int
notes_review_out (struct span  out,
                  struct oid  *tip,
                  struct span *text)
{
	struct cstr_view rest = { (char const *)out.ptr, out.len };
	struct cstr_view line, size;
	if (!cstr_view_cut(&rest, '\n', &line))
		return EIO;
	enum notes_kind kind = notes_kind(line, tip, &size);
	if (kind == KIND_MISSING)
		return ENOENT;
	/* The commit's text and a line feed come before the line of the blob. */
	uint64_t n;
	if (kind != KIND_COMMIT || util_number(size.data, size.len, UINT64_MAX, &n) ||
	    n >= rest.len || rest.data[n] != '\n')
		return EIO;
	size_t const at = line.len + 1 + (size_t)n + 1;
	return notes_read_out((struct span){ out.ptr + at, out.len - at }, text, nullptr) ? EIO : 0;
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
 * @p out, the output of STORE_LOG, and stores its commit, time, subject
 * and end in @p cursor. Returns false if no header precedes @p stat, as in
 * an output cut short.
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
		if (!oid_init_field(&cursor->commit, &rest, ' ') &&
		    !util_number(s + token, digits, UINT64_MAX, &cursor->time)) {
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
	/* Git emits TIMESTAMP SPACE COMMIT SPACE SUBJECT\0, then one numstat
	 * token per file. Only the first file token starts with a newline. A
	 * numstat token has no space before its first tab, and a header has
	 * one, so neither can pass as the other. A cut output may end within
	 * a token. */
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
