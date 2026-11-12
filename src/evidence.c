/** @file evidence.c
 *
 * The evidence of the findings (src/evidence.h).
 */
#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>

#include "evidence.h"
#include "jb.h"
#include "job.h"
#include "oid.h"
#include "range.h"
#include "seen.h"
#include "tool.h"
#include "xlist.h"

static_assert(SEEN_CALLS == 64 && TOOL_TEXT_MAX == 4U << 20, "update the refusals of "
              "evidence_cite()");

/* The names of the kinds. Each is NUL-terminated. */
static struct cstr_view const evidence_kind_names[EVIDENCE_KINDS] = { EVIDENCE_KIND_LIST(XL_VIEW) };

enum {
	EVIDENCE_SHORT   = 12,  /* digits of a commit in a summary */
	EVIDENCE_COMMAND = 33,  /* bytes of the command of a git call, NUL
	                           included */
};

struct evidence
evidence (void)
{
	return (struct evidence){
		.ids     = DSTR_INIT,
		.tool    = DSTR_INIT,
		.call_id = DSTR_INIT,
		.kind    = EVIDENCE_CALL,
	};
}

void
evidence_fini (struct evidence *e)
{
	dstr_cleanup(&e->ids);
	dstr_cleanup(&e->tool);
	dstr_cleanup(&e->call_id);
	free(e->ranges.items);
	*e = evidence();
}

/* Reads the line ranges @p text, such as 120-140,150, into @p dest in
 * ascending order. Ranges that overlap or touch become one range, so that
 * cite and recite copy the lines of all ranges in one forward pass. Returns
 * 0 or EINVAL.
 */
static int
evidence_ranges (struct evidence_ref *dest,
                 struct cstr_view     text)
{
	struct range *const r = dest->ranges;
	size_t n = 0;
	for (;;) {
		if (n == EVIDENCE_RANGES || util_ordinal(&text, &r[n].first))
			return EINVAL;
		r[n].last = r[n].first;
		if (text.len && *text.data == '-') {
			text = cstr_view_slice(text, 1, SIZE_MAX);
			if (util_ordinal(&text, &r[n].last) || r[n].last < r[n].first)
				return EINVAL;
		}
		++n;
		if (!text.len)
			break;
		if (*text.data != ',')
			return EINVAL;
		text = cstr_view_slice(text, 1, SIZE_MAX);
	}
	dest->n_ranges = (uint16_t)range_merge(r, n);
	return 0;
}

int
evidence_ref_parse (struct evidence_ref *dest,
                    struct cstr_view     text)
{
	if (!dest)
		return EINVAL;
	*dest = (struct evidence_ref){};
	if (!text.data || !text.len)
		return EINVAL;
	/* The part after the colon is ranges. A member name contains no colon
	 * and no #. */
	struct cstr_view head = text;
	struct cstr_view tail = {};
	bool const colon = cstr_view_cut(&head, ':', &head);
	if (colon)
		tail = cstr_view_slice(text, head.len + 1, SIZE_MAX);
	struct cstr_view name = head;
	if (!cstr_view_cut(&head, '#', &name)) {
		if (head.len < 2 || *head.data != 'c')
			return EINVAL;
		head = cstr_view_slice(head, 1, SIZE_MAX);
		if (util_ordinal(&head, &dest->number) || head.len)
			return EINVAL;
		if (!colon)
			dest->form = EVIDENCE_REF_CALL;
		else if (!evidence_ranges(dest, tail))
			dest->form = EVIDENCE_REF_LINES;
		else
			return EINVAL;
		return 0;
	}
	/* X#n, X#n/m or X#n/m:A-B. head is the part after the #. */
	if (!member_name_valid(name) || util_ordinal(&head, &dest->number))
		return EINVAL;
	memcpy(dest->author, name.data, name.len);
	if (!head.len && !colon) {
		dest->form = EVIDENCE_REF_FINDING;
		return 0;
	}
	if (!head.len || *head.data != '/')
		return EINVAL;
	head = cstr_view_slice(head, 1, SIZE_MAX);
	if (util_ordinal(&head, &dest->item) || head.len)
		return EINVAL;
	dest->form = EVIDENCE_REF_ITEM;
	if (colon) {
		if (evidence_ranges(dest, tail))
			return EINVAL;
		dest->form = EVIDENCE_REF_ITEM_LINES;
	}
	return 0;
}

void
evidence_path (unsigned char const sha256[static EVIDENCE_SHA256],
               char                path[static EVIDENCE_PATH])
{
	char hex[2 * EVIDENCE_SHA256];
	util_hex(hex, sha256, EVIDENCE_SHA256);
	(void)util_format(path, EVIDENCE_PATH, "archive/%.2s/%.62s", hex, hex + 2);
}

/* Stores the size and the SHA-256 of @p material in @p blob. */
static void
evidence_hash (struct evidence_blob *blob,
               struct cstr_view      material)
{
	(void)SHA256((unsigned char const *)(material.data ? material.data : ""), material.len,
	             blob->sha256);
	blob->bytes = material.len;
}

/* Returns the arguments of the call @p c as the upstream sent them. A call
 * that the ring stores as its number has none.
 */
static struct ai_call
evidence_parts (struct seen_call const *c)
{
	struct span const request = buf_peek(&c->request);
	struct job_call h;
	return request.len >= sizeof h ? job_call_parts(&h, request.ptr) : (struct ai_call){};
}

/* Returns the identifiers @p ids, a JSON object text, as a JSON object, or
 * nullptr if @p ids is empty or cannot be parsed.
 */
static struct json_object *
evidence_ids (struct dstr const *ids)
{
	struct cstr_view const text = cstr_view_dstr(ids);
	return text.len ? jb_parse(text.data, text.len, JSON_TOKENER_STRICT, 0) : nullptr;
}

/* Writes to @p command the command of the call @p c of the tool @p tool,
 * such as grep for a git grep, or "" for a call of another tool or a call
 * without a command.
 */
static void
evidence_command (struct seen_call const *c,
                  char const             *tool,
                  char                    command[static EVIDENCE_COMMAND])
{
	*command = '\0';
	if (strcmp(tool, "git"))
		return;
	struct ai_call const parts = evidence_parts(c);
	struct json_object *args = parts.args.len ? jb_parse(parts.args.data, parts.args.len,
	                                                     JSON_TOKENER_STRICT, 0) : nullptr;
	struct cstr_view const word = jb_text(jb_get(args, "command"), 1, EVIDENCE_COMMAND - 1,
	                                      "abcdefghijklmnopqrstuvwxyz-");
	memcpy(command, word.data ? word.data : "", word.len);
	command[word.len] = '\0';
	json_object_put(args);
}

/* Writes to @p what what a call of the tool @p tool with the command
 * @p command is, for a refusal: "a git grep" for a git call with a
 * command, else "a man call" with the name of the tool.
 */
static void
evidence_what (char const *tool,
               char const *command,
               char       *what,
               size_t      len)
{
	if (*command)
		(void)util_format(what, len, "a git %s", command);
	else
		(void)util_format(what, len, "a %s call", tool);
}

/* Drops the first line of @p *text, up to and with its line feed, or up to
 * the end of @p *text. Returns false if @p *text is empty.
 */
static bool
evidence_line (struct cstr_view *text)
{
	struct cstr_view line;
	if (!text->len)
		return false;
	if (!cstr_view_cut(text, '\n', &line))
		*text = cstr_view_slice(*text, text->len, SIZE_MAX);
	return true;
}

/* Drops @p skip lines from the start of @p *text, then appends the next
 * @p n lines to @p d and drops them too. A line ends with its line feed,
 * or at the end of @p *text. Returns 0, EINVAL if @p *text contains
 * fewer lines, or ENOMEM.
 */
static int
evidence_lines_copy (struct cstr_view *text,
                     uint64_t          skip,
                     uint64_t          n,
                     struct dstr      *d)
{
	while (skip && evidence_line(text))
		--skip;
	char const *const start = text->data;
	while (n && evidence_line(text))
		--n;
	if (skip || n)
		return EINVAL;
	return dstr_cat_n(d, start, (size_t)(text->data - start)) ? 0 : ENOMEM;
}

int
evidence_ranges_put (struct range const *r,
                     size_t              n,
                     char const         *sep,
                     char const         *to,
                     struct dstr        *d)
{
	for (size_t i = 0; i < n; ++i) {
		if (!dstr_printf(d, "%s%" PRIu32, i ? sep : "", r[i].first))
			return ENOMEM;
		if (r[i].last != r[i].first && !dstr_printf(d, "%s%" PRIu32, to, r[i].last))
			return ENOMEM;
	}
	return 0;
}

int
evidence_ref_put (struct evidence_ref const *ref,
                  struct dstr               *d)
{
	bool ok;
	if (*ref->author) {
		char id[SEEN_ID];
		evidence_ref_id(ref, id);
		ok = dstr_cat(d, id);
	} else {
		ok = dstr_printf(d, "c%" PRIu32, ref->number);
	}
	if (!ok || (ref->n_ranges && !dstr_cat(d, ":")))
		return ENOMEM;
	return evidence_ranges_put(ref->ranges, ref->n_ranges, ",", "-", d);
}

/* Appends to @p d the @p n line ranges at @p r of the file in the
 * identifiers @p ids: "lines A to B, C of PATH", or "the file" for no
 * path. Returns true, or false if out of memory.
 */
static bool
evidence_lines_of (struct range const *r,
                   size_t              n,
                   struct json_object *ids,
                   struct dstr        *d)
{
	struct cstr_view path = jb_view(jb_get(ids, "path"));
	if (!path.len)
		path = CSTR_VIEW("the file");
	return dstr_cat(d, n == 1 && r[0].first == r[0].last ? "line " : "lines ") &&
	       !evidence_ranges_put(r, n, ", ", " to ", d) &&
	       dstr_printf(d, " of %.*s", (int)path.len, path.data);
}

/* Refuses the reference @p ref to the call @p c, whose result showed no
 * line or other lines than @p ref names, with the reason in the @p len
 * bytes at @p why. Returns EINVAL or ENOMEM.
 */
static int
evidence_unshown (struct evidence_ref const *ref,
                  struct seen_call const    *c,
                  char const                *tool,
                  char                      *why,
                  size_t                     len)
{
	dstr_auto text = DSTR_INIT;
	if (!c->lines.range.first) {
		char command[EVIDENCE_COMMAND];
		char what[64];
		evidence_command(c, tool, command);
		evidence_what(tool, command, what, sizeof what);
		if (evidence_ref_put(ref, &text))
			return ENOMEM;
		return util_refuse(why, len, "%s needs a git read or a code describe. Call c%" PRIu32
		                   " is %s.", dstr_get(&text), c->stamp.number, what);
	}
	struct json_object *ids = evidence_ids(&c->ids);
	bool const ok = evidence_lines_of(&c->lines.range, 1, ids, &text);
	json_object_put(ids);
	if (!ok)
		return ENOMEM;
	return util_refuse(why, len, "Call c%" PRIu32 " showed %s whole. Give lines within that "
	                   "range.", c->stamp.number, dstr_get(&text));
}

/* Finds the call that @p ref names in the ring of @p cite. Returns 0 with
 * the call in @p *call, or EINVAL with the refusal in the @p len bytes at
 * @p why for a call that is not in the ring or cannot be evidence.
 */
static int
evidence_find (struct evidence_ref const  *ref,
               struct evidence_cite const *cite,
               struct seen_call const    **call,
               char                       *why,
               size_t                      len)
{
	uint32_t const n = ref->number;
	if (n == cite->call)
		return util_refuse(why, len, "c%" PRIu32 " is a findings call. findings calls cannot "
		                   "be evidence.", n);
	if (n > cite->call)
		return util_refuse(why, len, "No call c%" PRIu32 " was made before this call.", n);
	uint32_t const oldest = seen_oldest(cite->seen);
	switch (seen_find(cite->seen, n, call)) {
	case SEEN_KEPT:
		return 0;
	case SEEN_DROPPED:
		return util_refuse(why, len, "c%" PRIu32 " is no longer kept. askai keeps the last 64 "
		                   "calls of an ask call. The oldest kept call is c%" PRIu32 ".", n,
		                   oldest);
	case SEEN_RESTARTED:
		if (oldest)
			return util_refuse(why, len, "c%" PRIu32 " is not kept, because askai restarted "
			                   "after c%" PRIu32 ". The oldest kept call is c%" PRIu32 ".", n,
			                   n, oldest);
		return util_refuse(why, len, "c%" PRIu32 " is not kept, because askai restarted after "
		                   "c%" PRIu32 ". askai keeps no call of this ask call yet.", n, n);
	case SEEN_MISSING:
		break;
	}
	return util_refuse(why, len, "Call c%" PRIu32 " is a findings or compact call, or did not "
	                   "run. Cite another call.", n);
}

int
evidence_cite (struct evidence            *dest,
               struct evidence_material   *material,
               struct dstr                *lines,
               struct evidence_ref const  *ref,
               struct evidence_cite const *cite,
               bool                        strict,
               char                       *why,
               size_t                      len)
{
	if (!dest || !material || !lines || !ref || !cite || !cite->ask || !cite->name || !why ||
	    !len)
		return EINVAL;
	*material = (struct evidence_material){};
	*why = '\0';
	char const *author = ref->author;
	switch (ref->form) {
	case EVIDENCE_REF_FINDING:
		return util_refuse(why, len, "%s#%" PRIu32 " is a finding. evidence takes an evidence "
		                   "item such as %s#%" PRIu32 "/1, or a call such as c7. expand with "
		                   "id %s#%" PRIu32 " lists the items.", author, ref->number, author,
		                   ref->number, author, ref->number);
	case EVIDENCE_REF_ITEM:
	case EVIDENCE_REF_ITEM_LINES:
		/* evidence_recite() cites an item again. */
		return EINVAL;
	default:
		break;
	}
	struct seen_call const *c = nullptr;
	int e = evidence_find(ref, cite, &c, why, len);
	if (e)
		return e;
	struct seen_stamp const *stamp = &c->stamp;
	char const *tool = cite->name(cite->owner, stamp->tool);
	if (!tool)
		tool = "unknown";
	if (stamp->flags & SEEN_TOO_LARGE)
		return util_refuse(why, len, "Call c%" PRIu32 " was too large to keep. askai keeps no "
		                   "call of more than 4 MiB.", stamp->number);
	if (strict && stamp->turn >= cite->turn)
		return util_refuse(why, len, "Call c%" PRIu32 " was made in this turn. Cite c%" PRIu32
		                   " for verified or disproved in a later turn.", stamp->number,
		                   stamp->number);
	/* The lines of cN are all the lines that the call showed whole. The
	 * ranges ascend, so the first and the last range bound them. */
	struct evidence_ref shown = *ref;
	if (ref->form == EVIDENCE_REF_CALL && c->lines.range.first) {
		shown.form = EVIDENCE_REF_LINES;
		shown.ranges[0] = c->lines.range;
		shown.n_ranges = 1;
	}
	if (shown.form == EVIDENCE_REF_LINES) {
		struct range const bound = { shown.ranges[0].first,
		                             shown.ranges[shown.n_ranges - 1].last };
		if (!c->lines.range.first || !range_contains(c->lines.range, bound))
			return evidence_unshown(ref, c, tool, why, len);
	}
	static enum evidence_kind const kinds[] = {
		[EVIDENCE_REF_CALL]  = EVIDENCE_CALL,
		[EVIDENCE_REF_LINES] = EVIDENCE_LINES,
	};
	struct ai_call const parts = evidence_parts(c);
	evidence_fini(dest);
	dest->kind = kinds[shown.form];
	dest->call = stamp->number;
	dest->turn = stamp->turn;
	dest->ask = *cite->ask;
	dest->output = stamp->output;
	dest->skip = c->skip;
	dest->flags = stamp->flags & SEEN_FAILED ? EVIDENCE_FAILED : 0;
	char made[sizeof "call_4294967295_65535"];
	struct cstr_view const id = parts.id.len ? parts.id
	                          : util_format_view(made, sizeof made, "call_%" PRIu32 "_%u",
	                                             stamp->turn, (unsigned)stamp->index);
	e = cstr_view_store(cstr_view(tool), &dest->tool);
	e = e ? e : cstr_view_store(id, &dest->call_id);
	e = e ? e : cstr_view_store(cstr_view_dstr(&c->ids), &dest->ids);
	if (e)
		return e;
	if (dest->kind != EVIDENCE_LINES) {
		dest->flags |= EVIDENCE_REQUEST;
		*material = (struct evidence_material){ parts.args, cstr_view_dstr(&c->result) };
		evidence_hash(&dest->request, material->request);
		evidence_hash(&dest->result, material->result);
		return 0;
	}
	if (util_grow(dest->ranges.items, dest->ranges.cap, shown.n_ranges))
		return ENOMEM;
	/* The result starts with the lines c->lines.range that the window
	 * showed whole, from the offset c->lines.at. line is the number of the
	 * first line of rest. */
	struct cstr_view rest = cstr_view_slice(cstr_view_dstr(&c->result), c->lines.at, SIZE_MAX);
	uint32_t line = c->lines.range.first;
	(void)dstr_clear(lines);
	for (size_t i = 0; !e && i < shown.n_ranges; ++i) {
		struct range const r = shown.ranges[i];
		dest->ranges.items[dest->ranges.n++] = r;
		e = evidence_lines_copy(&rest, r.first - line, range_count(r), lines);
		line = r.last + 1;
	}
	if (e)
		return e;
	material->result = cstr_view_dstr(lines);
	evidence_hash(&dest->result, material->result);
	return 0;
}

int
evidence_external (struct cstr_view  text,
                   size_t            index,
                   char             *why,
                   size_t            len)
{
	bool control = false;
	bool hash = false;
	size_t run = 0;
	/* A run of exactly 40 or 64 digits of either case ends at a byte that
	 * is no digit, or at the end. */
	for (size_t i = 0; i < text.len; ++i) {
		unsigned char const b = (unsigned char)text.data[i];
		control |= b < 0x20 || b == 0x7f;
		if (util_hex_digit(b) < 16) {
			++run;
			continue;
		}
		hash |= run == OID_SHA1 || run == OID_SHA256;
		run = 0;
	}
	hash |= run == OID_SHA1 || run == OID_SHA256;
	static_assert(EVIDENCE_TEXT == 1024, "update the refusal");
	if (!text.len || text.len > EVIDENCE_TEXT || control || !jb_is_text(text))
		return util_refuse(why, len, "external[%zu] must be 1 byte to 1 KiB of text on one "
		                   "line.", index);
	if (!hash)
		return util_refuse(why, len, "external[%zu] needs an exact hash of the material: 40 or "
		                   "64 hexadecimal digits, such as sha256:9b1c...", index);
	return 0;
}

/* Returns the blob @p b as a JSON object {"sha256": HEX, "bytes": N}, or
 * nullptr if out of memory.
 */
static struct json_object *
evidence_blob_json (struct evidence_blob const *b)
{
	char hex[2 * EVIDENCE_SHA256];
	util_hex(hex, b->sha256, EVIDENCE_SHA256);
	return jb_object("sha256", json_object_new_string_len(hex, (int)sizeof hex),
	                 "bytes", json_object_new_uint64(b->bytes),
	                 nullptr);
}

/* Reads the JSON object @p obj of evidence_blob_json() into @p b. Returns 0
 * or EINVAL.
 */
static int
evidence_blob_parse (struct evidence_blob     *b,
                     struct json_object const *obj)
{
	struct cstr_view const hex = jb_view(jb_get(obj, "sha256"));
	if (jb_members(obj) != 2 || hex.len != 2 * EVIDENCE_SHA256 ||
	    util_unhex(b->sha256, hex.data, EVIDENCE_SHA256) ||
	    jb_uint_checked(jb_get(obj, "bytes"), UINT64_MAX, &b->bytes))
		return EINVAL;
	return 0;
}

struct json_object *
evidence_json (struct evidence const *e,
               int                   *err)
{
	if (!e) {
		*err = EFAULT;
		return nullptr;
	}
	struct json_object *obj = jb_object(
		"item", json_object_new_uint64(e->item),
		"kind", jb_string(evidence_kind_names[e->kind]),
		"tool", jb_string(cstr_view_dstr(&e->tool)),
		"call", json_object_new_uint64(e->call),
		"turn", json_object_new_uint64(e->turn),
		"call_id", jb_string(cstr_view_dstr(&e->call_id)),
		"failed", json_object_new_boolean(e->flags & EVIDENCE_FAILED),
		"output", json_object_new_uint64(e->output),
		"skip", json_object_new_uint64(e->skip),
		nullptr);
	if (*e->ask.hex)
		obj = jb_set_or_free(obj, "ask", json_object_new_string(e->ask.hex));
	if (*e->from)
		obj = jb_set_or_free(obj, "from", json_object_new_string(e->from));
	struct cstr_view const ids = cstr_view_dstr(&e->ids);
	if (ids.len)
		obj = jb_set_or_free(obj, "ids", jb_parse(ids.data, ids.len, JSON_TOKENER_STRICT, 0));
	if (e->kind == EVIDENCE_LINES) {
		struct json_object *lines = json_object_new_array();
		for (size_t i = 0; lines && i < e->ranges.n; ++i) {
			struct range const r = e->ranges.items[i];
			struct json_object *pair = json_object_new_array();
			pair = jb_push_or_free(pair, json_object_new_uint64(r.first));
			lines = jb_push_or_free(lines, jb_push_or_free(pair, json_object_new_uint64(r.last)));
		}
		obj = jb_set_or_free(obj, "lines", lines);
	}
	if (e->flags & EVIDENCE_REQUEST)
		obj = jb_set_or_free(obj, "request", evidence_blob_json(&e->request));
	obj = jb_set_or_free(obj, "result", evidence_blob_json(&e->result));
	*err = obj ? 0 : ENOMEM;
	return obj;
}

/* Reads the member lines of @p obj, an array of [first, last] pairs, into
 * the ranges of @p dest. askai stores the ranges in ascending order, apart
 * from each other. An item of an earlier askai can store them in the order
 * of the reference, and overlapping. Returns 0, EINVAL or ENOMEM.
 */
static int
evidence_ranges_parse (struct evidence          *dest,
                       struct json_object const *obj)
{
	struct json_object const *lines = jb_get(obj, "lines");
	size_t const n = jb_len(lines);
	if (!json_object_is_type(lines, json_type_array) || !n || n > EVIDENCE_RANGES)
		return EINVAL;
	if (util_grow(dest->ranges.items, dest->ranges.cap, n))
		return ENOMEM;
	for (size_t i = 0; i < n; ++i) {
		struct json_object const *pair = jb_at(lines, i);
		uint64_t first;
		uint64_t last;
		if (!json_object_is_type(pair, json_type_array) || jb_len(pair) != 2 ||
		    jb_uint_checked(jb_at(pair, 0), UINT32_MAX, &first) ||
		    jb_uint_checked(jb_at(pair, 1), UINT32_MAX, &last) || !first || last < first)
			return EINVAL;
		dest->ranges.items[dest->ranges.n++] = (struct range){ (uint32_t)first, (uint32_t)last };
	}
	return 0;
}

int
evidence_parse (struct evidence          *dest,
                struct json_object const *obj)
{
	if (!dest)
		return EINVAL;
	evidence_fini(dest);
	size_t const kind = cstr_view_index(evidence_kind_names, EVIDENCE_KINDS,
	                                    jb_view(jb_get(obj, "kind")));
	struct cstr_view const tool = jb_view(jb_get(obj, "tool"));
	struct cstr_view const call_id = jb_view(jb_get(obj, "call_id"));
	struct json_object *ask = jb_get(obj, "ask");
	struct json_object *from = jb_get(obj, "from");
	struct json_object *ids = jb_get(obj, "ids");
	struct json_object *request = jb_get(obj, "request");
	struct json_object *failed = jb_get(obj, "failed");
	uint64_t item;
	uint64_t call;
	uint64_t turn;
	uint64_t skip;
	int e = !json_object_is_type(obj, json_type_object) || kind == EVIDENCE_KINDS ||
	        !tool.data || !call_id.data || !json_object_is_type(failed, json_type_boolean) ||
	        jb_uint_checked(jb_get(obj, "item"), UINT32_MAX, &item) || !item ||
	        jb_uint_checked(jb_get(obj, "call"), UINT32_MAX, &call) ||
	        jb_uint_checked(jb_get(obj, "turn"), UINT32_MAX, &turn) ||
	        jb_uint_checked(jb_get(obj, "output"), UINT64_MAX, &dest->output) ||
	        jb_uint_checked(jb_get(obj, "skip"), TOOL_OUTPUT_MAX, &skip) ||
	        (ask && hexid_init(&dest->ask, jb_view(ask))) ||
	        (from && !jb_text(from, 1, SEEN_ID - 1, nullptr).data) ||
	        (ids && !json_object_is_type(ids, json_type_object)) ||
	        (request && evidence_blob_parse(&dest->request, request)) ||
	        evidence_blob_parse(&dest->result, jb_get(obj, "result")) ? EINVAL : 0;
	if (!e && kind == EVIDENCE_LINES)
		e = evidence_ranges_parse(dest, obj);
	if (!e) {
		dest->kind = (enum evidence_kind)kind;
		dest->skip = (uint32_t)skip;
		dest->item = (uint32_t)item;
		dest->call = (uint32_t)call;
		dest->turn = (uint32_t)turn;
		dest->flags = (jb_true(failed) ? EVIDENCE_FAILED : 0) | (request ? EVIDENCE_REQUEST : 0);
		if (from)
			memcpy(dest->from, jb_view(from).data, jb_view(from).len);
		struct cstr_view const wire = jb_wire(ids);
		e = ids && !wire.data ? ENOMEM : cstr_view_store(tool, &dest->tool);
		e = e ? e : cstr_view_store(call_id, &dest->call_id);
		e = e ? e : cstr_view_store(wire.data ? wire : CSTR_VIEW(""), &dest->ids);
	}
	if (e)
		evidence_fini(dest);
	return e;
}

int
evidence_parse_text (struct evidence  *dest,
                     struct cstr_view  json)
{
	struct json_object *obj = json.len ? jb_parse(json.data, json.len, JSON_TOKENER_STRICT, 0)
	                                   : nullptr;
	int const e = evidence_parse(dest, obj);
	json_object_put(obj);
	return e;
}

/* Appends to @p d the lines of the item @p e of the kind EVIDENCE_LINES
 * with the identifiers @p ids: "lines A to B, C of PATH in repository R at
 * commit C", each part that @p ids name, with at most @p clip digits of
 * the commit. Returns true, or false if out of memory.
 */
static bool
evidence_lines_summary (struct evidence const *e,
                        struct json_object    *ids,
                        size_t                 clip,
                        struct dstr           *d)
{
	struct cstr_view const repo = jb_view(jb_get(ids, "repo"));
	struct cstr_view const commit = jb_view(jb_get(ids, "commit"));
	bool ok = evidence_lines_of(e->ranges.items, e->ranges.n, ids, d);
	ok = ok && (!repo.len || dstr_printf(d, " in repository %.*s", (int)repo.len, repo.data));
	return ok && (!commit.len || dstr_printf(d, " at commit %.*s", (int)util_clip(commit.data,
	                                         commit.len, clip), commit.data));
}

int
evidence_summary (struct evidence const *e,
                  struct dstr           *d)
{
	if (e->kind == EVIDENCE_LINES) {
		struct json_object *ids = evidence_ids(&e->ids);
		bool const ok = evidence_lines_summary(e, ids, EVIDENCE_SHORT, d);
		json_object_put(ids);
		return ok ? 0 : ENOMEM;
	}
	struct cstr_view const tool = cstr_view_dstr(&e->tool);
	bool ok = dstr_printf(d, "call c%" PRIu32 ", %.*s", e->call, (int)tool.len, tool.data);
	ok = ok && (!(e->flags & EVIDENCE_FAILED) || dstr_cat(d, ", failed"));
	ok = ok && dstr_printf(d, ", %" PRIu64 " byte%s", e->result.bytes, util_s(e->result.bytes));
	return ok ? 0 : ENOMEM;
}

void
evidence_ref_id (struct evidence_ref const *ref,
                 char                       id[static SEEN_ID])
{
	(void)util_format(id, SEEN_ID, "%s#%" PRIu32 "/%" PRIu32, ref->author, ref->number,
	                  ref->item);
}

/* Returns true if @p m is the material that @p b records. */
static bool
evidence_matches (struct evidence_blob const *b,
                  struct cstr_view            m)
{
	unsigned char sha256[EVIDENCE_SHA256];
	(void)SHA256((unsigned char const *)(m.data ? m.data : ""), m.len, sha256);
	return m.len == b->bytes && !memcmp(sha256, b->sha256, sizeof sha256);
}

int
evidence_recite (struct evidence           *dest,
                 struct dstr               *lines,
                 struct evidence_ref const *ref,
                 struct cstr_view           record,
                 struct cstr_view           material,
                 char                      *why,
                 size_t                     len)
{
	if (why && len)
		*why = '\0';
	if (!dest || !lines || !ref || !why || !len ||
	    (ref->form != EVIDENCE_REF_ITEM && ref->form != EVIDENCE_REF_ITEM_LINES))
		return EINVAL;
	char id[SEEN_ID];
	evidence_ref_id(ref, id);
	int e = evidence_parse_text(dest, record);
	if (e == EINVAL)
		return util_refuse(why, len, "Could not read finding %s#%" PRIu32 ": unknown format.",
		                   ref->author, ref->number);
	if (e)
		return e;
	dest->item = 0;
	memcpy(dest->from, id, sizeof id);
	if (ref->form == EVIDENCE_REF_ITEM)
		return 0;
	dstr_auto text = DSTR_INIT;
	if (dest->kind != EVIDENCE_LINES) {
		if (evidence_ref_put(ref, &text))
			return ENOMEM;
		return util_refuse(why, len, "%s needs a lines item. %s is a %s item.", dstr_get(&text),
		                   id, evidence_kind_names[dest->kind].data);
	}
	if (!evidence_matches(&dest->result, material))
		return util_refuse(why, len, "The stored material of %s does not match the recorded "
		                   "SHA-256.", id);
	/* The material is the lines of the ranges of the item, one after
	 * another. evidence_ref_parse() makes one range of ranges that touch,
	 * so the lines of a range of @p ref can be in several ranges of the
	 * item. The loop copies a range of @p ref in parts. want is the lines
	 * of the range i of @p ref that are not copied yet, and part is the
	 * lines of want up to the last line of the range k of the item, which
	 * contains the first line of want. copied is the number of the lines
	 * of the range i that are copied.
	 *
	 * The ranges of @p ref ascend. If the ranges of the item ascend too and
	 * do not overlap, as askai stores them, the range k and the place of
	 * its lines in the material only move forward. The ranges of an item
	 * that an earlier askai stored can be out of order and can overlap. For
	 * such an item, the search for each part starts again at the first
	 * range of the item and at the start of the material. start is the
	 * index of the first line of the range k in the material, and done is
	 * the number of the lines that the loop dropped from rest. */
	struct range const *const r = dest->ranges.items;
	bool apart = true;
	for (size_t k = 1; apart && k < dest->ranges.n; ++k)
		apart = r[k].first > r[k - 1].last;
	struct cstr_view rest = material;
	uint64_t done = 0;
	uint64_t start = 0;
	size_t k = 0;
	size_t i = 0;
	uint32_t copied = 0;
	(void)dstr_clear(lines);
	while (!e && i < ref->n_ranges) {
		struct range const want = { ref->ranges[i].first + copied, ref->ranges[i].last };
		if (!apart) {
			rest = material;
			done = 0;
			start = 0;
			k = 0;
		}
		while (k < dest->ranges.n && (want.first < r[k].first || want.first > r[k].last)) {
			start += range_count(r[k]);
			++k;
		}
		if (k == dest->ranges.n) {
			e = EINVAL;
			break;
		}
		struct range const part = { want.first, want.last < r[k].last ? want.last : r[k].last };
		uint64_t const from = start + (part.first - r[k].first);
		uint64_t const n = range_count(part);
		e = evidence_lines_copy(&rest, from - done, n, lines);
		done = from + n;
		if (part.last < want.last) {
			copied += (uint32_t)n;
			continue;
		}
		copied = 0;
		++i;
	}
	if (e == EINVAL) {
		struct json_object *ids = evidence_ids(&dest->ids);
		bool const ok = evidence_lines_of(dest->ranges.items, dest->ranges.n, ids, &text);
		json_object_put(ids);
		if (!ok)
			return ENOMEM;
		return util_refuse(why, len, "%s shows %s. Give lines within %s.", id, dstr_get(&text),
		                   dest->ranges.n > 1 ? "those ranges" : "that range");
	}
	if (e)
		return e;
	dest->ranges.n = 0;
	if (util_grow(dest->ranges.items, dest->ranges.cap, ref->n_ranges))
		return ENOMEM;
	for (size_t i = 0; i < ref->n_ranges; ++i)
		dest->ranges.items[dest->ranges.n++] = ref->ranges[i];
	evidence_hash(&dest->result, cstr_view_dstr(lines));
	return 0;
}

/* Appends the material @p m to @p d, with a line feed after it if it does
 * not end with one. Returns true, or false if out of memory.
 */
static bool
evidence_material_put (struct cstr_view  m,
                       struct dstr      *d)
{
	return !m.len || (dstr_cat_n(d, m.data, m.len) &&
	                  (m.data[m.len - 1] == '\n' || dstr_cat_n(d, "\n", 1)));
}

/* Appends to @p d the part of the notice of the item @p e of the kind
 * EVIDENCE_CALL, as evidence_expand() prints it, from what the item is to
 * the end of the notice before the arguments. @p ids are the identifiers
 * of the item, or nullptr. Returns true, or false if out of memory.
 */
static bool
evidence_expand_call (struct evidence const *e,
                      char const            *author,
                      struct json_object    *ids,
                      struct dstr           *d)
{
	struct cstr_view const tool = cstr_view_dstr(&e->tool);
	struct cstr_view const repo = jb_view(jb_get(ids, "repo"));
	struct cstr_view const commit = jb_view(jb_get(ids, "commit"));
	if (!dstr_printf(d, "call c%" PRIu32, e->call))
		return false;
	/* The call of an item cited again is not a call of the author. */
	if (!*e->from && !dstr_printf(d, " of %s", author))
		return false;
	if (*e->ask.hex && !dstr_printf(d, " in ask %.8s", e->ask.hex))
		return false;
	if (!dstr_printf(d, ", turn %" PRIu32 ", tool %.*s%s, stored exactly as sent.", e->turn,
	                 (int)tool.len, tool.len ? tool.data : "",
	                 e->flags & EVIDENCE_FAILED ? ", failed" : ""))
		return false;
	if (repo.len && commit.len)
		return dstr_printf(d, " Repository %.*s, commit %.*s.", (int)repo.len, repo.data,
		                   (int)commit.len, commit.data);
	if (repo.len)
		return dstr_printf(d, " Repository %.*s.", (int)repo.len, repo.data);
	if (commit.len)
		return dstr_printf(d, " Commit %.*s.", (int)commit.len, commit.data);
	return true;
}

int
evidence_expand (struct evidence const          *e,
                 char const                     *id,
                 char const                     *author,
                 struct evidence_material const *material,
                 struct dstr                    *d)
{
	if (!e || !id || !author || !material || !d)
		return EFAULT;
	struct cstr_view const request = material->request;
	struct cstr_view const result = material->result;
	struct json_object *ids = evidence_ids(&e->ids);
	bool const asked = e->flags & EVIDENCE_REQUEST;
	bool const missing = !result.data || (asked && !request.data);
	bool differs = false;
	if (!missing)
		differs = !evidence_matches(&e->result, result) ||
		          (asked && !evidence_matches(&e->request, request));
	bool ok = false;
	if (!dstr_printf(d, "[askai: %s", id))
		goto done;
	if (*e->from && !dstr_printf(d, " from %s", e->from))
		goto done;
	if (!dstr_cat(d, ": "))
		goto done;
	if (e->kind == EVIDENCE_LINES) {
		if (!evidence_lines_summary(e, ids, SIZE_MAX, d))
			goto done;
		if (!dstr_cat(d, ", stored as shown:]\n"))
			goto done;
	} else {
		if (!evidence_expand_call(e, author, ids, d))
			goto done;
		if (asked) {
			if (!dstr_cat(d, " Arguments:]\n"))
				goto done;
			if (!evidence_material_put(request, d))
				goto done;
			if (!dstr_printf(d, "[askai: %s: result", id))
				goto done;
		} else if (!dstr_cat(d, " Result")) {
			goto done;
		}
		if (!dstr_printf(d, ", %" PRIu64 " byte%s", e->result.bytes, util_s(e->result.bytes)))
			goto done;
		if ((e->skip || e->output > e->result.bytes) &&
		    !dstr_printf(d, ", from byte %" PRIu32 " of an output of %" PRIu64 " bytes",
		                 e->skip, e->output))
			goto done;
		if (!dstr_cat(d, ":]\n"))
			goto done;
	}
	if (!evidence_material_put(result, d))
		goto done;
	if (missing && !dstr_printf(d, "[askai: the stored material of %s is missing.]\n", id))
		goto done;
	if (differs && !dstr_printf(d, "[askai: the stored material of %s does not match the "
	                            "recorded SHA-256.]\n", id))
		goto done;
	ok = true;
done:
	json_object_put(ids);
	return ok ? 0 : ENOMEM;
}
