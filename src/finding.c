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
#include "version.h"
#include "xlist.h"

struct cstr_view const finding_state_names[FINDING_STATES] = { FINDING_STATE_LIST(XL_VIEW) };

enum {
	FINDING_ASK   = 8,   /* digits of an ask ID in show */
	FINDING_STATE = 9,   /* the width of the state column of a list */
	FINDING_ROW   = FINDING_ID + FINDING_STATE + FINDING_CLAIM + 8,  /* bytes of a line
	                                                                    of the notice */
};

/* The options of json-c for the file of a finding. */
static constexpr int FINDING_JSON = JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED |
                                    JSON_C_TO_STRING_NOSLASHESCAPE;

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
	uint64_t n;
	if (!cstr_view_cut(&text, '#', &name) || !member_name_valid(name) || !text.len ||
	    *text.data == '0' || util_number(text.data, text.len, UINT32_MAX, &n))
		return EINVAL;
	memcpy(author, name.data, name.len);
	author[name.len] = '\0';
	*number = (uint32_t)n;
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

void
finding_ref (char const *author,
             char        ref[static FINDING_REF])
{
	(void)util_format(ref, FINDING_REF, "refs/heads/agent/%s/findings", author);
}

void
finding_path (uint32_t number,
              char     path[static FINDING_FILE])
{
	(void)util_format(path, FINDING_FILE, "findings/%" PRIu32 "/%" PRIu32 ".json", number / 100,
	                  number);
}

uint32_t
finding_path_number (struct cstr_view path)
{
	/* The number is between the last slash and .json. Only the path that
	 * finding_path() writes for it is the file of a finding. */
	size_t at = path.len;
	while (at && path.data[at - 1] != '/')
		--at;
	struct cstr_view const name = cstr_view_slice(path, at, SIZE_MAX);
	uint64_t n;
	if (!cstr_view_ends(name, CSTR_VIEW(".json")) || !name.len ||
	    util_number(name.data, name.len - (sizeof ".json" - 1), UINT32_MAX, &n) || !n)
		return 0;
	char want[FINDING_FILE];
	finding_path((uint32_t)n, want);
	return cstr_view_equal(path, cstr_view(want)) ? (uint32_t)n : 0;
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
	uint64_t turn;
	uint64_t call;
	if (state == FINDING_STATES ||
	    jb_uint_checked(jb_get(obj, "turn"), UINT32_MAX, &turn) ||
	    jb_uint_checked(jb_get(obj, "call"), UINT32_MAX, &call) ||
	    (ask && hexid_init(&c->ask, jb_view(ask))) ||
	    (asked_by && !member_name_valid(by)) || (askai && !jb_view(askai).data) ||
	    (reason && !jb_view(reason).data) || (before && !jb_view(before).data) ||
	    !json_object_is_type(cited, json_type_array) || jb_len(cited) > EVIDENCE_REFS ||
	    !json_object_is_type(external, json_type_array) ||
	    jb_len(external) > EVIDENCE_EXTERNAL)
		return EINVAL;
	c->state = (enum finding_state)state;
	c->turn = (uint32_t)turn;
	c->call = (uint32_t)call;
	if (asked_by)
		memcpy(c->asked_by, by.data, by.len);
	if ((askai && cstr_view_store(jb_view(askai), &c->askai)) ||
	    (reason && cstr_view_store(jb_view(reason), &c->reason)) ||
	    (before && cstr_view_store(jb_view(before), &c->claim_before)))
		return ENOMEM;
	size_t const k = jb_len(cited);
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
	size_t const m = jb_len(external);
	if (util_grow(c->external.items, c->external.cap, m))
		return ENOMEM;
	for (size_t i = 0; i < m; ++i) {
		struct cstr_view const t = jb_view(jb_at(external, i));
		struct dstr *x = &c->external.items[c->external.n++];
		*x = (struct dstr)DSTR_INIT;
		if (!t.data)
			return EINVAL;
		if (cstr_view_store(t, x))
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
	char author[MEMBER_NAME];
	uint32_t number;
	size_t const k = jb_len(changes);
	if (jb_int(jb_get(obj, "version"), 0) != FINDING_VERSION ||
	    finding_id_parse(jb_view(jb_get(obj, "id")), f->author, &f->number) ||
	    (about && finding_id_parse(jb_view(about), author, &number)) ||
	    (conversation && hexid_init(&f->conversation, jb_view(conversation))) ||
	    !claim.len || !json_object_is_type(changes, json_type_array) || !k)
		return EINVAL;
	if (about)
		memcpy(f->about, jb_view(about).data, jb_view(about).len);
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

/* Returns the change @p c as a JSON object, or nullptr if out of memory. */
static struct json_object *
finding_change_json (struct finding_change const *c)
{
	struct json_object *obj = jb_object("state", jb_string(finding_state_names[c->state]),
	                                    nullptr);
	if (*c->ask.hex)
		obj = jb_set_or_free(obj, "ask", json_object_new_string(c->ask.hex));
	obj = jb_set_or_free(obj, "turn", json_object_new_uint64(c->turn));
	obj = jb_set_or_free(obj, "call", json_object_new_uint64(c->call));
	if (*c->asked_by)
		obj = jb_set_or_free(obj, "asked_by", json_object_new_string(c->asked_by));
	if (dstr_len(&c->askai))
		obj = jb_set_or_free(obj, "askai", jb_string(cstr_view_dstr(&c->askai)));
	if (dstr_len(&c->reason))
		obj = jb_set_or_free(obj, "reason", jb_string(cstr_view_dstr(&c->reason)));
	if (dstr_len(&c->claim_before))
		obj = jb_set_or_free(obj, "claim_before", jb_string(cstr_view_dstr(&c->claim_before)));
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
finding_render (struct finding const *f,
                struct dstr          *d)
{
	char id[FINDING_ID];
	finding_id(f, id);
	struct json_object *obj = jb_object("version", json_object_new_int(FINDING_VERSION),
	                                    "id", json_object_new_string(id),
	                                    nullptr);
	if (*f->about)
		obj = jb_set_or_free(obj, "about", json_object_new_string(f->about));
	if (*f->conversation.hex)
		obj = jb_set_or_free(obj, "conversation", json_object_new_string(f->conversation.hex));
	obj = jb_set_or_free(obj, "claim", jb_string(cstr_view_dstr(&f->claim)));
	struct json_object *changes = json_object_new_array();
	for (size_t i = 0; changes && i < f->changes.n; ++i)
		changes = jb_push_or_free(changes, finding_change_json(&f->changes.items[i]));
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

int
finding_apply (struct finding        *f,
               struct finding_change *c,
               struct cstr_view       claim)
{
	if (util_grow(f->changes.items, f->changes.cap, f->changes.n + 1))
		return ENOMEM;
	bool const corrects = claim.len && !cstr_view_equal(claim, cstr_view_dstr(&f->claim));
	struct dstr copy = DSTR_INIT;
	if (corrects && cstr_view_store(claim, &copy)) {
		dstr_cleanup(&copy);
		return ENOMEM;
	}
	uint32_t items = f->changes.n ? f->changes.items[f->changes.n - 1].items : 0;
	for (size_t i = 0; i < c->evidence.n; ++i)
		c->evidence.items[i].item = ++items;
	c->items = items;
	if (corrects) {
		dstr_cleanup(&c->claim_before);
		c->claim_before = dstr_move(&f->claim);
		f->claim = dstr_move(&copy);
	}
	f->changes.items[f->changes.n++] = *c;
	*c = finding_change();
	return 0;
}

void
finding_unapply (struct finding        *f,
                 struct finding_change *c)
{
	if (!f->changes.n)
		return;
	*c = f->changes.items[--f->changes.n];
	f->changes.items[f->changes.n] = finding_change();
	if (dstr_len(&c->claim_before)) {
		dstr_cleanup(&f->claim);
		f->claim = dstr_move(&c->claim_before);
	}
}

/* Orders two findings for finding_sort(). */
static int
finding_order (void const *a,
               void const *b)
{
	struct finding const *x = a;
	struct finding const *y = b;
	int const by = strcmp(x->author, y->author);
	return by ? by : (x->number < y->number) - (x->number > y->number);
}

void
finding_sort (struct finding *list,
              size_t          n)
{
	if (n > 1)
		qsort(list, n, sizeof *list, finding_order);
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

/* Appends to @p d the line of @p f in a list, with the ID in a column of
 * @p width bytes and the about in a column of @p about bytes. Returns true,
 * or false if out of memory.
 */
static bool
finding_row (struct finding const *f,
             int                   width,
             int                   about,
             struct dstr          *d)
{
	char id[FINDING_ID];
	finding_id(f, id);
	struct cstr_view const claim = finding_claim_line(f);
	return dstr_printf(d, "%-*s  %-*s  %-*s  %.*s\n", width, id, FINDING_STATE,
	                   finding_state_names[finding_state(f)].data, about,
	                   *f->about ? f->about : "-",
	                   (int)claim.len, claim.len ? claim.data : "");
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
	size_t listed = 0;
	size_t closed = 0;
	for (size_t i = 0; i < n; ++i) {
		struct finding const *f = &list[i];
		if (*author && strcmp(author, f->author))
			continue;
		char id[FINDING_ID];
		finding_id(f, id);
		int const w = (int)strlen(id);
		int const a = (int)strlen(f->about);
		width = w > width ? w : width;
		about = a > about ? a : about;
		++listed;
		if (!(FINDING_OPEN >> finding_state(f) & 1))
			++closed;
	}
	bool ok = true;
	if (!listed)
		ok = *author ? dstr_printf(d, "%s has no findings yet.\n", author)
		             : dstr_printf(d, "No member of the session %.*s has findings yet.\n",
		                           (int)session.len, session.data);
	else if (*author)
		ok = dstr_printf(d, "Findings of %s, open first (id, state, about, claim):\n", author);
	else
		ok = dstr_printf(d, "Findings of the session %.*s, open first (id, state, about, "
		                 "claim):\n", (int)session.len, session.data);
	/* The open findings, then the closed ones. */
	for (int pass = 0; ok && listed && pass < 2; ++pass) {
		if (pass && closed)
			ok = dstr_cat(d, "Closed findings:\n");
		for (size_t i = 0; ok && i < n; ++i) {
			struct finding const *f = &list[i];
			bool const open = FINDING_OPEN >> finding_state(f) & 1;
			if ((!*author || !strcmp(author, f->author)) && open == !pass)
				ok = finding_row(f, width, about, d);
		}
	}
	return ok ? 0 : ENOMEM;
}

/* Appends to @p d the change @p k of @p f, its reason and its evidence, as
 * show writes them. Returns true, or false if out of memory.
 */
static bool
finding_show_change (struct finding const *f,
                     size_t                k,
                     struct dstr          *d)
{
	struct finding_change const *c = &f->changes.items[k];
	struct cstr_view const reason = cstr_view_dstr(&c->reason);
	struct cstr_view const before = cstr_view_dstr(&c->claim_before);
	char id[FINDING_ID];
	finding_id(f, id);
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
              struct finding const *list,
              size_t                n,
              struct dstr          *d)
{
	char id[FINDING_ID];
	finding_id(f, id);
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
		ok = finding_show_change(f, k, d);
	return ok ? 0 : ENOMEM;
}

int
finding_notice (struct finding const *list,
                size_t                n,
                struct hexid const   *conversation,
                size_t                most,
                struct dstr          *d)
{
	size_t here = 0;
	size_t other = 0;
	int width = 0;
	for (size_t i = 0; i < n; ++i) {
		struct finding const *f = &list[i];
		if (!(FINDING_OPEN >> finding_state(f) & 1))
			continue;
		if (!hexid_equal(&f->conversation, conversation)) {
			++other;
			continue;
		}
		char id[FINDING_ID];
		finding_id(f, id);
		int const w = (int)strlen(id);
		width = w > width ? w : width;
		++here;
	}
	bool ok = here ? dstr_cat(d, "[askai: Your open findings in this conversation (id, state, "
	                          "claim):]\n")
	               : dstr_cat(d, "[askai: You have no open findings in this conversation.]\n");
	size_t used = 0;
	size_t left = here;
	for (size_t i = 0; ok && left && i < n; ++i) {
		struct finding const *f = &list[i];
		if (!(FINDING_OPEN >> finding_state(f) & 1) ||
		    !hexid_equal(&f->conversation, conversation))
			continue;
		char id[FINDING_ID];
		finding_id(f, id);
		struct cstr_view const claim = finding_claim_line(f);
		char row[FINDING_ROW];
		size_t const len = util_format(row, sizeof row, "%-*s  %-*s  %.*s\n", width, id,
		                               FINDING_STATE, finding_state_names[finding_state(f)].data,
		                               (int)claim.len, claim.len ? claim.data : "");
		if (used + len > most)
			break;
		ok = dstr_cat_n(d, row, len);
		used += len;
		--left;
	}
	if (ok && left)
		ok = dstr_printf(d, "[askai: %zu more open finding%s. Call findings with action list.]\n",
		                 left, util_s(left));
	if (ok && other)
		ok = dstr_printf(d, "[askai: You have %zu open finding%s in other conversations. Call "
		                 "findings with action list to see the findings of every member.]\n",
		                 other, util_s(other));
	return ok ? 0 : ENOMEM;
}
