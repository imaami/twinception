/** @file finding.c
 *
 * The findings of the members of the session (src/finding.h).
 */
#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "finding.h"
#include "jb.h"
#include "tool.h"
#include "xlist.h"

struct cstr_view const finding_state_names[FINDING_STATES] = { FINDING_STATE_LIST(XL_VIEW) };

enum {
	FINDING_ASK     = 8,  /* digits of an ask ID in show */
	FINDING_STATE   = 9,  /* the width of the state column of a list */
	FINDING_VERSION = 1,  /* the version of the file that askai writes and
	                         reads */
	FINDING_JSON    = JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED |
	                  JSON_C_TO_STRING_NOSLASHESCAPE,
	                      /* the options of json-c for the file of a finding */
};

struct finding
finding (void)
{
	return (struct finding){ .claim = DSTR_INIT };
}

struct finding_change
finding_change (void)
{
	return (struct finding_change){
		.reason       = DSTR_INIT,
		.claim_before = DSTR_INIT,
		.askai        = DSTR_INIT,
		.state        = FINDING_CANDIDATE,
	};
}

void
finding_change_fini (struct finding_change *c)
{
	dstr_cleanup(&c->reason);
	dstr_cleanup(&c->claim_before);
	dstr_cleanup(&c->askai);
	for (size_t i = 0; i < c->evidence.n; ++i)
		evidence_fini(&c->evidence.items[i]);
	free(c->evidence.items);
	for (size_t i = 0; i < c->external.n; ++i)
		dstr_cleanup(&c->external.items[i]);
	free(c->external.items);
	*c = finding_change();
}

int
finding_change_external (struct finding_change *c,
                         struct cstr_view       text)
{
	if (util_push(c->external, (struct dstr)DSTR_INIT))
		return ENOMEM;
	return cstr_view_store(text, &c->external.items[c->external.n - 1]);
}

void
finding_fini (struct finding *f)
{
	dstr_cleanup(&f->claim);
	for (size_t i = 0; i < f->changes.n; ++i)
		finding_change_fini(&f->changes.items[i]);
	free(f->changes.items);
	*f = finding();
}

int
finding_id_parse (struct cstr_view  text,
                  char              author[static MEMBER_NAME],
                  uint32_t         *number)
{
	*author = '\0';
	*number = 0;
	struct cstr_view name = text;
	uint32_t n;
	if (!cstr_view_cut(&text, '#', &name) || !member_name_valid(name) ||
	    util_ordinal(&text, &n) || text.len)
		return EINVAL;
	memcpy(author, name.data, name.len);
	author[name.len] = '\0';
	*number = n;
	return 0;
}

void
finding_id_format (char const *author,
                   uint32_t    number,
                   char        id[static FINDING_ID])
{
	(void)util_format(id, FINDING_ID, "%s#%" PRIu32, author, number);
}

void
finding_id (struct finding const *f,
            char                  id[static FINDING_ID])
{
	finding_id_format(f->author, f->number, id);
}

struct cstr_view
finding_path (uint32_t number,
              char     path[static FINDING_FILE])
{
	return util_format_view(path, FINDING_FILE, FINDING_DIR "%" PRIu32 "/%" PRIu32 FINDING_EXT,
	                        number / 100, number);
}

uint32_t
finding_path_number (struct cstr_view path)
{
	/* findings/D/N.json, as finding_path() writes it: D and N without
	 * leading zeros, N from 1, and D = N / 100. */
	static struct cstr_view const head = CSTR_VIEW_INIT(FINDING_DIR);
	static struct cstr_view const tail = CSTR_VIEW_INIT(FINDING_EXT);
	if (path.len < head.len + tail.len || !cstr_view_starts(path, head) ||
	    !cstr_view_ends(path, tail))
		return 0;
	struct cstr_view n = cstr_view_slice(path, head.len, path.len - head.len - tail.len);
	struct cstr_view d;
	uint64_t dir;
	uint32_t number;
	if (!cstr_view_cut(&n, '/', &d) || util_number(d.data, d.len, UINT32_MAX, &dir) ||
	    (d.len > 1 && *d.data == '0') || util_ordinal(&n, &number) || n.len ||
	    dir != number / 100)
		return 0;
	return number;
}

/* Reads the change @p obj of a file into @p c, which finding_change()
 * initialized. The numbers of its evidence items must grow from @p *items
 * on, and @p *items becomes the number of the last item. Returns 0, EINVAL
 * or ENOMEM.
 */
static int
finding_change_read (struct finding_change    *c,
                     struct json_object const *obj,
                     uint32_t                 *items)
{
	size_t const state = cstr_view_index(finding_state_names, FINDING_STATES,
	                                     jb_view(jb_get(obj, "state")));
	struct json_object *ask = jb_get(obj, "ask");
	struct json_object *asked_by = jb_get(obj, "asked_by");
	struct json_object *askai = jb_get(obj, "askai");
	struct json_object *reason = jb_get(obj, "reason");
	struct json_object *before = jb_get(obj, "claim_before");
	struct json_object const *cited = jb_get(obj, "evidence");
	struct json_object const *external = jb_get(obj, "external");
	struct cstr_view const by = jb_view(asked_by);
	struct cstr_view const version = jb_view(askai);
	struct cstr_view const why = jb_view(reason);
	struct cstr_view const was = jb_view(before);
	size_t const k = jb_len(cited);
	size_t const m = jb_len(external);
	uint64_t turn;
	uint64_t call;
	if (state == FINDING_STATES ||
	    jb_uint_checked(jb_get(obj, "turn"), UINT32_MAX, &turn) ||
	    jb_uint_checked(jb_get(obj, "call"), UINT32_MAX, &call) ||
	    (ask && hexid_init(&c->ask, jb_view(ask))) ||
	    (asked_by && !member_name_valid(by)) || (askai && !version.data) ||
	    (reason && !why.data) || (before && !was.data) ||
	    !json_object_is_type(cited, json_type_array) || k > EVIDENCE_REFS ||
	    !json_object_is_type(external, json_type_array) || m > EVIDENCE_EXTERNAL)
		return EINVAL;
	c->state = (enum finding_state)state;
	c->turn = (uint32_t)turn;
	c->call = (uint32_t)call;
	if (asked_by)
		memcpy(c->asked_by, by.data, by.len);
	if ((askai && cstr_view_store(version, &c->askai)) ||
	    (reason && cstr_view_store(why, &c->reason)) ||
	    (before && cstr_view_store(was, &c->claim_before)))
		return ENOMEM;
	if (util_grow(c->evidence.items, c->evidence.cap, k))
		return ENOMEM;
	for (size_t i = 0; i < k; ++i) {
		struct evidence *e = &c->evidence.items[c->evidence.n++];
		*e = evidence();
		int const r = evidence_parse(e, jb_at(cited, i));
		if (r)
			return r;
		if (e->item <= *items)
			return EINVAL;
		*items = e->item;
	}
	c->items = *items;
	for (size_t i = 0; i < m; ++i) {
		struct cstr_view const t = jb_view(jb_at(external, i));
		if (!t.data)
			return EINVAL;
		if (finding_change_external(c, t))
			return ENOMEM;
	}
	return 0;
}

/* Reads the JSON object @p obj of a file into @p f, which finding()
 * initialized. Returns 0, EINVAL or ENOMEM.
 */
static int
finding_read (struct finding           *f,
              struct json_object const *obj)
{
	struct json_object *about = jb_get(obj, "about");
	struct json_object *conversation = jb_get(obj, "conversation");
	struct json_object const *changes = jb_get(obj, "changes");
	struct cstr_view const claim = jb_view(jb_get(obj, "claim"));
	struct cstr_view const about_id = jb_view(about);
	char author[MEMBER_NAME];
	uint32_t number;
	size_t const k = jb_len(changes);
	if (jb_int(jb_get(obj, "version"), 0) != FINDING_VERSION ||
	    finding_id_parse(jb_view(jb_get(obj, "id")), f->author, &f->number) ||
	    (about && finding_id_parse(about_id, author, &number)) ||
	    (conversation && hexid_init(&f->conversation, jb_view(conversation))) ||
	    !claim.len || !json_object_is_type(changes, json_type_array) || !k)
		return EINVAL;
	if (about)
		memcpy(f->about, about_id.data, about_id.len);
	if (cstr_view_store(claim, &f->claim) || util_grow(f->changes.items, f->changes.cap, k))
		return ENOMEM;
	uint32_t items = 0;
	for (size_t i = 0; i < k; ++i) {
		struct finding_change *c = &f->changes.items[f->changes.n++];
		*c = finding_change();
		int const e = finding_change_read(c, jb_at(changes, i), &items);
		if (e)
			return e;
	}
	return 0;
}

int
finding_parse (struct finding   *f,
               struct cstr_view  json)
{
	finding_fini(f);
	struct json_object *obj = json.len ? jb_parse(json.data, json.len, JSON_TOKENER_STRICT, 0)
	                                   : nullptr;
	int const e = json_object_is_type(obj, json_type_object) ? finding_read(f, obj) : EINVAL;
	json_object_put(obj);
	if (e)
		finding_fini(f);
	return e;
}

/* Returns the change @p c with the claim before @p before as a JSON
 * object, or nullptr if out of memory.
 */
static struct json_object *
finding_change_json (struct finding_change const *c,
                     struct cstr_view             before)
{
	struct json_object *obj = jb_object("state", jb_string(finding_state_names[c->state]),
	                                    nullptr);
	if (*c->ask.hex)
		obj = jb_set_or_free(obj, "ask", json_object_new_string(c->ask.hex));
	obj = jb_set_or_free(obj, "turn", json_object_new_uint64(c->turn));
	obj = jb_set_or_free(obj, "call", json_object_new_uint64(c->call));
	if (*c->asked_by)
		obj = jb_set_or_free(obj, "asked_by", json_object_new_string(c->asked_by));
	struct cstr_view const version = cstr_view_dstr(&c->askai);
	struct cstr_view const reason = cstr_view_dstr(&c->reason);
	if (version.len)
		obj = jb_set_or_free(obj, "askai", jb_string(version));
	if (reason.len)
		obj = jb_set_or_free(obj, "reason", jb_string(reason));
	if (before.len)
		obj = jb_set_or_free(obj, "claim_before", jb_string(before));
	struct json_object *evidence = json_object_new_array();
	int e = 0;
	for (size_t i = 0; evidence && i < c->evidence.n; ++i)
		evidence = jb_push_or_free(evidence, evidence_json(&c->evidence.items[i], &e));
	obj = jb_set_or_free(obj, "evidence", evidence);
	struct json_object *external = json_object_new_array();
	for (size_t i = 0; external && i < c->external.n; ++i)
		external = jb_push_or_free(external, jb_string(cstr_view_dstr(&c->external.items[i])));
	return jb_set_or_free(obj, "external", external);
}

int
finding_render (struct finding const        *f,
                struct finding_change const *c,
                struct cstr_view             claim,
                struct dstr                 *d)
{
	char id[FINDING_ID];
	finding_id(f, id);
	/* A new claim makes the claim of f the claim before of the change c. */
	struct cstr_view now = cstr_view_dstr(&f->claim);
	struct cstr_view was = c ? cstr_view_dstr(&c->claim_before) : (struct cstr_view){};
	if (claim.len) {
		was = now;
		now = claim;
	}
	struct json_object *obj = jb_object("version", json_object_new_int(FINDING_VERSION),
	                                    "id", json_object_new_string(id),
	                                    nullptr);
	if (*f->about)
		obj = jb_set_or_free(obj, "about", json_object_new_string(f->about));
	if (*f->conversation.hex)
		obj = jb_set_or_free(obj, "conversation", json_object_new_string(f->conversation.hex));
	obj = jb_set_or_free(obj, "claim", jb_string(now));
	struct json_object *changes = json_object_new_array();
	for (size_t i = 0; changes && i < f->changes.n; ++i) {
		struct finding_change const *k = &f->changes.items[i];
		changes = jb_push_or_free(changes,
		                          finding_change_json(k, cstr_view_dstr(&k->claim_before)));
	}
	if (changes && c)
		changes = jb_push_or_free(changes, finding_change_json(c, was));
	obj = jb_set_or_free(obj, "changes", changes);
	size_t len = 0;
	size_t const had = dstr_len(d);
	char const *text = obj ? json_object_to_json_string_length(obj, FINDING_JSON, &len) : nullptr;
	int e = 0;
	if (!text)
		e = ENOMEM;
	else if (len >= TOOL_TEXT_MAX)
		e = EFBIG;
	else if (!dstr_cat_n(d, text, len) || !dstr_cat_n(d, "\n", 1))
		e = ENOMEM;
	if (e)
		(void)dstr_erase(d, had, SIZE_MAX);
	json_object_put(obj);
	return e;
}

int
finding_check (struct finding_change const *c,
               char                        *why,
               size_t                       len)
{
	char const *state = finding_state_names[c->state].data;
	if (FINDING_SHOWN >> c->state & 1 && !c->evidence.n)
		return util_refuse(why, len, "%s needs at least one evidence item besides external.",
		                   state);
	if (c->state != FINDING_CANDIDATE && !dstr_len(&c->reason))
		return util_refuse(why, len, "%s needs reason: %s.", state,
		                   c->state == FINDING_RETRACTED ? "why you retract the finding"
		                                                 : "why the evidence shows the state");
	return 0;
}

/* Orders two findings by the name of the author, and the findings of one
 * author newest first.
 */
static int
finding_order_id (void const *a,
                  void const *b)
{
	struct finding const *x = a;
	struct finding const *y = b;
	int const by = strcmp(x->author, y->author);
	return by ? by : (x->number < y->number) - (x->number > y->number);
}

/* Orders two findings for finding_sort(): the open findings first, then
 * as finding_order_id() orders them.
 */
static int
finding_order (void const *a,
               void const *b)
{
	bool const xo = FINDING_OPEN >> finding_state(a) & 1;
	bool const yo = FINDING_OPEN >> finding_state(b) & 1;
	if (xo != yo)
		return xo ? -1 : 1;
	return finding_order_id(a, b);
}

void
finding_sort (struct finding *list,
              size_t          n)
{
	if (n > 1)
		qsort(list, n, sizeof *list, finding_order);
}

struct finding const *
finding_find (struct finding const *list,
              size_t                n,
              char const           *author,
              uint32_t              number)
{
	if (!list || !author)
		return nullptr;
	/* The open findings come first. open becomes their number. */
	size_t open = 0;
	for (size_t end = n; open < end; ) {
		size_t const mid = open + (end - open) / 2;
		if (FINDING_OPEN >> finding_state(&list[mid]) & 1)
			open = mid + 1;
		else
			end = mid;
	}
	struct finding key = { .number = number };
	(void)util_format(key.author, sizeof key.author, "%s", author);
	struct finding const *f = bsearch(&key, list, open, sizeof *list, finding_order_id);
	return f ? f : bsearch(&key, list + open, n - open, sizeof *list, finding_order_id);
}

struct evidence const *
finding_item (struct finding const *f,
              uint32_t              item)
{
	if (!f)
		return nullptr;
	/* The items of a change are numbered after the items of the changes
	 * before it, up to the number c->items. */
	for (size_t k = 0; k < f->changes.n; ++k) {
		struct finding_change const *c = &f->changes.items[k];
		if (c->items < item)
			continue;
		for (size_t i = 0; i < c->evidence.n; ++i)
			if (c->evidence.items[i].item == item)
				return &c->evidence.items[i];
		break;
	}
	return nullptr;
}

struct cstr_view
finding_claim_line (struct finding const *f)
{
	struct cstr_view claim = cstr_view_dstr(&f->claim);
	char const *nl = claim.len ? memchr(claim.data, '\n', claim.len) : nullptr;
	if (nl)
		claim.len = (size_t)(nl - claim.data);
	claim.len = util_clip(claim.data, claim.len, FINDING_CLAIM);
	return claim;
}

/* Returns the length of the ID of @p f, such as 5 for bro#5. */
static int
finding_id_width (struct finding const *f)
{
	char digits[UTIL_DECIMAL];
	return (int)(strlen(f->author) + 1 + util_decimal(digits + sizeof digits, f->number, 0).len);
}

/* Appends to @p d the line of @p f in a list: the ID in a column of
 * @p width bytes, the state, the about in a column of @p about bytes
 * unless @p about is 0, and the claim (finding_claim_line()). Returns 0
 * or ENOMEM.
 */
static int
finding_row (struct finding const *f,
             int                   width,
             int                   about,
             struct dstr          *d)
{
	char id[FINDING_ID];
	finding_id(f, id);
	struct cstr_view const claim = finding_claim_line(f);
	if (!dstr_printf(d, "%-*s  %-*s  ", width, id, FINDING_STATE,
	                 finding_state_names[finding_state(f)].data))
		return ENOMEM;
	if (about && !dstr_printf(d, "%-*s  ", about, *f->about ? f->about : "-"))
		return ENOMEM;
	return dstr_printf(d, "%.*s\n", (int)claim.len, claim.len ? claim.data : "") ? 0 : ENOMEM;
}

int
finding_list (struct finding const *list,
              size_t                n,
              struct cstr_view      session,
              char const           *author,
              struct dstr          *d)
{
	int width = 0;
	int about = 1;
	for (size_t i = 0; i < n; ++i) {
		int const w = finding_id_width(&list[i]);
		int const a = (int)strlen(list[i].about);
		width = w > width ? w : width;
		about = a > about ? a : about;
	}
	bool ok;
	if (!n)
		ok = *author ? dstr_printf(d, "%s has no findings yet.\n", author)
		             : dstr_printf(d, "No member of the session %.*s has findings yet.\n",
		                           (int)session.len, session.data);
	else if (*author)
		ok = dstr_printf(d, "Findings of %s, open first (id, state, about, claim):\n", author);
	else
		ok = dstr_printf(d, "Findings of the session %.*s, open first (id, state, about, "
		                 "claim):\n", (int)session.len, session.data);
	/* finding_sort() puts the open findings first. */
	int e = ok ? 0 : ENOMEM;
	bool open = true;
	for (size_t i = 0; !e && i < n; ++i) {
		struct finding const *f = &list[i];
		if (open && !(FINDING_OPEN >> finding_state(f) & 1)) {
			open = false;
			e = dstr_cat(d, "Closed findings:\n") ? 0 : ENOMEM;
		}
		e = e ? e : finding_row(f, width, about, d);
	}
	return e;
}

/* Appends to @p d the change @p k of @p f with the ID @p id, its reason
 * and its evidence, as show writes them. Returns true, or false if out of
 * memory.
 */
static bool
finding_show_change (struct finding const *f,
                     char const           *id,
                     size_t                k,
                     struct dstr          *d)
{
	struct finding_change const *c = &f->changes.items[k];
	struct cstr_view const reason = cstr_view_dstr(&c->reason);
	struct cstr_view const before = cstr_view_dstr(&c->claim_before);
	if (!dstr_printf(d, "Change %zu: %s", k + 1, finding_state_names[c->state].data))
		return false;
	if (*c->ask.hex && !dstr_printf(d, ", ask %.*s", FINDING_ASK, c->ask.hex))
		return false;
	if (!dstr_printf(d, ", turn %" PRIu32, c->turn))
		return false;
	if (*c->asked_by && !dstr_printf(d, ", asked by %s", c->asked_by))
		return false;
	if (!dstr_cat(d, "\n"))
		return false;
	if (before.len && !dstr_printf(d, "  claim before: %.*s\n", (int)before.len, before.data))
		return false;
	if (reason.len && !dstr_printf(d, "  reason: %.*s\n", (int)reason.len, reason.data))
		return false;
	for (size_t i = 0; i < c->evidence.n; ++i) {
		struct evidence const *e = &c->evidence.items[i];
		if (!dstr_printf(d, "  evidence %s/%" PRIu32, id, e->item))
			return false;
		if (*e->from && !dstr_printf(d, " from %s", e->from))
			return false;
		if (!dstr_cat(d, ": "))
			return false;
		if (evidence_summary(e, d))
			return false;
		if (!dstr_cat(d, "\n"))
			return false;
	}
	for (size_t i = 0; i < c->external.n; ++i) {
		struct cstr_view const t = cstr_view_dstr(&c->external.items[i]);
		if (!dstr_printf(d, "  external: %.*s\n", (int)t.len, t.data))
			return false;
	}
	return true;
}

int
finding_show (struct finding const *f,
              char const           *id,
              struct finding const *list,
              size_t                n,
              struct dstr          *d)
{
	struct cstr_view const claim = cstr_view_dstr(&f->claim);
	bool ok = dstr_printf(d, "Finding %s by %s: %s\nclaim: %.*s\n", id, f->author,
	                      finding_state_names[finding_state(f)].data, (int)claim.len, claim.data);
	if (ok && *f->about)
		ok = dstr_printf(d, "about: %s\n", f->about);
	size_t abouts = 0;
	for (size_t i = 0; ok && i < n; ++i) {
		struct finding const *other = &list[i];
		if (strcmp(other->about, id))
			continue;
		char by[FINDING_ID];
		finding_id(other, by);
		if (abouts)
			ok = dstr_cat(d, ", ");
		else
			ok = dstr_printf(d, "Findings about %s: ", id);
		if (ok)
			ok = dstr_printf(d, "%s (%s)", by, finding_state_names[finding_state(other)].data);
		++abouts;
	}
	if (ok && abouts)
		ok = dstr_cat(d, "\n");
	for (size_t k = 0; ok && k < f->changes.n; ++k)
		ok = finding_show_change(f, id, k, d);
	return ok ? 0 : ENOMEM;
}

int
finding_notice (struct finding const *list,
                size_t                n,
                struct hexid const   *conversation,
                size_t                most,
                struct dstr          *d)
{
	/* finding_sort() puts the open findings first: open is their number. */
	size_t open = 0;
	size_t here = 0;
	int width = 0;
	for (; open < n && FINDING_OPEN >> finding_state(&list[open]) & 1; ++open) {
		if (!hexid_equal(&list[open].conversation, conversation))
			continue;
		int const w = finding_id_width(&list[open]);
		width = w > width ? w : width;
		++here;
	}
	size_t const other = open - here;
	bool ok = here ? dstr_cat(d, "[askai: Your open findings in this conversation (id, state, "
	                          "claim):]\n")
	               : dstr_cat(d, "[askai: You have no open findings in this conversation.]\n");
	int e = ok ? 0 : ENOMEM;
	size_t used = 0;
	size_t left = here;
	for (size_t i = 0; !e && left && i < open; ++i) {
		if (!hexid_equal(&list[i].conversation, conversation))
			continue;
		size_t const at = dstr_len(d);
		e = finding_row(&list[i], width, 0, d);
		size_t const len = dstr_len(d) - at;
		if (!e && len > most - used) {
			(void)dstr_erase(d, at, SIZE_MAX);
			break;
		}
		used += len;
		--left;
	}
	if (!e && left &&
	    !dstr_printf(d, "[askai: %zu more open finding%s. Call findings with action list.]\n",
	                 left, util_s(left)))
		e = ENOMEM;
	if (!e && other &&
	    !dstr_printf(d, "[askai: You have %zu open finding%s in other conversations. Call "
	                 "findings with action list to see the findings of every member.]\n",
	                 other, util_s(other)))
		e = ENOMEM;
	return e;
}
