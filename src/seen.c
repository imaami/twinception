/** @file seen.c
 *
 * The calls whose results askai showed to the turns of an ask call
 * (src/seen.h).
 */
#define _GNU_SOURCE /* mempcpy() */
#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "jb.h"
#include "oid.h"
#include "range.h"
#include "seen.h"
#include "tool.h"
#include "util.h"

/** Returns true if @p t is 1 to SEEN_NAME_MAX bytes of UTF-8 text without
 *  NUL, as a repository name or a path must be.
 */
static bool
seen_name_valid (struct cstr_view t)
{
	return t.len && t.len <= SEEN_NAME_MAX && jb_is_text(t);
}

/** Returns true if the numbered lines of @p src are not empty and are
 *  within @p out.
 */
static bool
seen_source_within (struct seen_source const *src,
                    struct cstr_view          out)
{
	struct extent const lines = src->output;
	return lines.len && lines.len <= out.len && lines.at <= out.len - lines.len;
}

struct seen_read *
seen_read_create (struct seen_source const *src,
                  struct cstr_view          out,
                  int                      *e)
{
	if (!e)
		return nullptr;
	if (!src || (!out.data && out.len)) {
		*e = EFAULT;
		return nullptr;
	}
	*e = EINVAL;
	if (!oid_len(&src->commit) ||
	    !seen_name_valid(src->repo) || !seen_name_valid(src->path) || !src->lines.first ||
	    src->lines.last < src->lines.first || (src->total && src->total < src->lines.last) ||
	    src->total > UINT32_MAX || !seen_source_within(src, out))
		return nullptr;
	/* Each numbered line ends with a newline, except perhaps the last. */
	struct cstr_view const lines = cstr_view_slice(out, src->output.at, src->output.len);
	if (cstr_view_count(lines, '\n') + (lines.data[lines.len - 1] != '\n') !=
	    range_count(src->lines))
		return nullptr;
	struct seen_read *read = malloc(sizeof *read + src->repo.len + src->path.len);
	if (!read) {
		*e = ENOMEM;
		return nullptr;
	}
	*read = (struct seen_read){ .src = *src,
	                            .mark = { .line = src->lines.first, .at = src->output.at } };
	char *path = mempcpy(read->text, src->repo.data, src->repo.len);
	memcpy(path, src->path.data, src->path.len);
	read->src.repo.data = read->text;
	read->src.path.data = path;
	*e = 0;
	return read;
}

void
seen_read_destroy (struct seen_read **p_read)
{
	if (!p_read)
		return;
	free(*p_read);
	*p_read = nullptr;
}

struct json_object *
seen_read_json (struct seen_read const *read,
                int                    *e)
{
	if (!e)
		return nullptr;
	if (!read) {
		*e = EFAULT;
		return nullptr;
	}
	struct seen_source const *src = &read->src;
	struct json_object *obj = jb_object("repo", jb_string(src->repo),
	                                    "commit", jb_string(oid_view(&src->commit)),
	                                    "path", jb_string(src->path),
	                                    "at", json_object_new_uint64(src->output.at),
	                                    "len", json_object_new_uint64(src->output.len),
	                                    "first", json_object_new_uint64(src->lines.first),
	                                    "last", json_object_new_uint64(src->lines.last),
	                                    "total", json_object_new_uint64(src->total),
	                                    nullptr);
	*e = obj ? 0 : ENOMEM;
	return obj;
}

struct seen_read *
seen_read_create_json (struct json_object const *obj,
                       struct cstr_view          out,
                       int                      *e)
{
	if (!e)
		return nullptr;
	if (!obj) {
		*e = EFAULT;
		return nullptr;
	}
	uint64_t at;
	uint64_t len;
	uint64_t first;
	uint64_t last;
	uint64_t total;
	struct oid commit;
	if (jb_uint_checked(jb_get(obj, "at"), SIZE_MAX, &at) ||
	    jb_uint_checked(jb_get(obj, "len"), SIZE_MAX, &len) ||
	    jb_uint_checked(jb_get(obj, "first"), UINT32_MAX, &first) ||
	    jb_uint_checked(jb_get(obj, "last"), UINT32_MAX, &last) ||
	    jb_uint_checked(jb_get(obj, "total"), UINT64_MAX, &total) ||
	    oid_init(&commit, jb_view(jb_get(obj, "commit")))) {
		*e = EINVAL;
		return nullptr;
	}
	/* seen_read_create() checks the repository name and the path. */
	struct seen_source const src = {
		.repo   = jb_view(jb_get(obj, "repo")),
		.commit = commit,
		.path   = jb_view(jb_get(obj, "path")),
		.total  = total,
		.output = { (size_t)at, (size_t)len },
		.lines  = { (uint32_t)first, (uint32_t)last },
	};
	return seen_read_create(&src, out, e);
}

struct seen_lines
seen_lines (struct seen_read *read,
            struct cstr_view  out,
            struct extent     window,
            int              *e)
{
	if (!e)
		return (struct seen_lines){};
	if (!read || (!out.data && out.len)) {
		*e = EFAULT;
		return (struct seen_lines){};
	}
	struct seen_source const *src = &read->src;
	*e = seen_source_within(src, out) ? 0 : EINVAL;
	size_t const end = src->output.at + src->output.len;
	if (*e || window.at >= end)
		return (struct seen_lines){};
	size_t lo = window.at > src->output.at ? window.at : src->output.at;
	size_t const hi = window.len < end - window.at ? window.at + window.len : end;
	if (lo >= hi)
		return (struct seen_lines){};
	char const *p = out.data;
	/* Count the lines from the mark of the last window, or from the start
	 * of the numbered lines if the mark is after this window. */
	struct seen_mark *mark = &read->mark;
	if (mark->at > lo)
		*mark = (struct seen_mark){ .line = src->lines.first, .at = src->output.at };
	mark->line += cstr_view_count((struct cstr_view){ p + mark->at, lo - mark->at }, '\n');
	mark->at = lo;
	uint64_t line = mark->line;
	/* Skip a line whose start is before the window. */
	if (lo > src->output.at && p[lo - 1] != '\n') {
		char const *nl = memchr(p + lo, '\n', hi - lo);
		if (!nl)
			return (struct seen_lines){};
		lo = (size_t)(nl - p) + 1;
		++line;
	}
	/* Count the lines that end within the window. The last line may end
	 * without a newline, and it counts if the window ends at the end of
	 * the numbered lines. */
	uint64_t const n = cstr_view_count((struct cstr_view){ p + lo, hi - lo }, '\n') +
	                   (hi == end && hi > lo && p[hi - 1] != '\n');
	if (!n)
		return (struct seen_lines){};
	if (line + n - 1 > src->lines.last || lo - window.at > UINT32_MAX) {
		*e = EINVAL;
		return (struct seen_lines){};
	}
	return (struct seen_lines){ { (uint32_t)line, (uint32_t)(line + n - 1) },
	                            (uint32_t)(lo - window.at) };
}

/* Returns the index in seen->calls of the call @p i of @p seen, from the
 * oldest.
 */
static size_t
seen_call_place (struct seen const *seen,
                 size_t             i)
{
	return (seen->head + i) % SEEN_CALLS;
}

/* Returns the index in seen->items.items of the item @p i of @p seen, from
 * the oldest.
 */
static size_t
seen_item_place (struct seen const *seen,
                 size_t             i)
{
	return (seen->items_head + i) % SEEN_ITEMS;
}

/* Drops the oldest call of @p seen, which must contain a call. The storage
 * of the result and of the identifiers stays in place.
 */
static void
seen_drop (struct seen *seen)
{
	struct seen_call *c = &seen->calls[seen->head];
	seen->bytes -= buf_peek(&c->request).len + dstr_len(&c->result);
	buf_fini(&c->request);
	(void)dstr_clear(&c->result);
	(void)dstr_clear(&c->ids);
	seen->head = (uint16_t)seen_call_place(seen, 1);
	--seen->n;
}

/* Drops the oldest calls of @p seen until a call of @p bytes bytes fits,
 * and returns the place of the newest call. The newest call always fits.
 */
static struct seen_call *
seen_room (struct seen *seen,
           size_t       bytes)
{
	while (seen->n && (seen->n == SEEN_CALLS || seen->bytes + bytes > SEEN_BYTES))
		seen_drop(seen);
	return &seen->calls[seen_call_place(seen, seen->n)];
}

/* Appends the identifiers of the file @p src to @p ids as a JSON object
 * with the members repo, commit, path and total. Returns 0 or ENOMEM.
 */
static int
seen_ids (struct dstr              *ids,
          struct seen_source const *src)
{
	int e = cstr_view_cat(CSTR_VIEW("{\"repo\":\""), ids);
	e = e ? e : jb_escape_cat(src->repo, ids);
	e = e ? e : cstr_view_cat(CSTR_VIEW("\",\"commit\":\""), ids);
	e = e ? e : cstr_view_cat(oid_view(&src->commit), ids);
	e = e ? e : cstr_view_cat(CSTR_VIEW("\",\"path\":\""), ids);
	e = e ? e : jb_escape_cat(src->path, ids);
	return e ? e : dstr_printf(ids, "\",\"total\":%" PRIu64 "}", src->total) ? 0 : ENOMEM;
}

/* Allocates a ring without calls into @p *p_seen if @p *p_seen is
 * nullptr. Returns 0 or ENOMEM.
 */
static int
seen_get (struct seen **p_seen)
{
	if (*p_seen)
		return 0;
	struct seen *seen = malloc(sizeof *seen);
	if (!seen)
		return ENOMEM;
	*seen = (struct seen){};
	for (size_t i = 0; i < SEEN_CALLS; ++i)
		seen->calls[i] = (struct seen_call){ .result = DSTR_INIT, .ids = DSTR_INIT,
		                                     .tool = TOOL_MAX };
	*p_seen = seen;
	return 0;
}

int
seen_keep (struct seen              **p_seen,
           struct buf                *request,
           struct seen_result const  *r)
{
	if (!p_seen || !request || !r || !r->parts || (!r->out.data && r->out.len))
		return EFAULT;
	struct extent const window = { r->skip, r->parts[0].len };
	if (window.at > r->out.len || window.len > r->out.len - window.at ||
	    (window.len && r->parts[0].data != r->out.data + window.at) ||
	    r->at > TOOL_OUTPUT_MAX || window.at > TOOL_OUTPUT_MAX - r->at)
		return EINVAL;
	int e = 0;
	struct seen_read *read = r->read;
	struct seen_lines const whole = read ? seen_lines(read, r->out, window, &e)
	                                     : (struct seen_lines){};
	e = e ? e : seen_get(p_seen);
	if (e)
		return e;
	struct seen *seen = *p_seen;
	size_t const shown = cstr_view_total(r->parts, SEEN_PARTS);
	size_t const asked = buf_peek(request).len;
	bool const large = asked + shown > TOOL_TEXT_MAX;
	size_t const bytes = large ? 0 : asked + shown;
	struct seen_call *c = seen_room(seen, bytes);
	e = read ? seen_ids(&c->ids, &read->src) : 0;
	if (!e && !large && !dstr_reserve(&c->result, shown))
		e = ENOMEM;
	if (e) {
		(void)dstr_clear(&c->ids);
		return e;
	}
	if (!large) {
		/* dstr_reserve() made room for the parts, so the appends do not
		 * allocate. */
		for (size_t i = 0; i < SEEN_PARTS; ++i)
			(void)dstr_cat_n(&c->result, r->parts[i].data, r->parts[i].len);
		c->request = *request;
		*request = (struct buf){};
	}
	c->output = r->output;
	c->skip = (uint32_t)(r->at + r->skip);
	c->lines = whole;
	c->number = r->number;
	c->turn = r->turn;
	c->tool = r->tool;
	c->index = r->index;
	c->flags = (r->flags & SEEN_FAILED) | (large ? SEEN_TOO_LARGE : 0);
	seen->bytes += bytes;
	++seen->n;
	return 0;
}

/* Drops the items of @p seen, and with them the list of the items that
 * calls printed. The storage of the arrays stays.
 */
static void
seen_items_drop (struct seen *seen)
{
	for (size_t i = 0; i < seen->items.n; ++i)
		dstr_cleanup(&seen->items.items[seen_item_place(seen, i)].record);
	seen->items.n = 0;
	seen->items_head = 0;
	seen->printed.n = 0;
}

/* Returns true if @p obj is an object of the array of seen_items_json(). */
static bool
seen_item_valid (struct json_object *obj)
{
	struct json_object *blobs = jb_get(obj, "blobs");
	uint64_t bytes;
	uint64_t shown;
	uint64_t turn;
	if (jb_uint_checked(jb_get(obj, "bytes"), TOOL_OUTPUT_MAX, &bytes) || !bytes ||
	    jb_uint_checked(jb_get(obj, "shown"), bytes, &shown) ||
	    jb_uint_checked(jb_get(obj, "turn"), UINT32_MAX, &turn) || (turn && shown < bytes) ||
	    !jb_text(jb_get(obj, "id"), 1, SEEN_ID - 1, nullptr).data ||
	    !jb_text(jb_get(obj, "record"), 1, TOOL_TEXT_MAX, nullptr).data ||
	    jb_len(blobs) != array_size((struct seen_item){}.blobs))
		return false;
	for (size_t k = 0; k < array_size((struct seen_item){}.blobs); ++k) {
		struct cstr_view const hex = jb_view(jb_at(blobs, k));
		if (!hex.data || (hex.len && !oid_valid(hex)))
			return false;
	}
	return true;
}

int
seen_resume (struct seen              **p_seen,
             struct json_object const  *calls,
             struct json_object const  *items)
{
	if (!p_seen)
		return EFAULT;
	uint64_t made = 0;
	size_t const n = jb_len(items);
	if ((calls && jb_uint_checked(calls, UINT32_MAX, &made)) ||
	    (items && (!json_object_is_type(items, json_type_array) || n > SEEN_ITEMS)))
		return EINVAL;
	for (size_t i = 0; i < n; ++i)
		if (!seen_item_valid(jb_at(items, i)))
			return EINVAL;
	int e = seen_get(p_seen);
	if (e)
		return e;
	struct seen *seen = *p_seen;
	seen_items_drop(seen);
	e = util_grow(seen->items.items, seen->items.cap, n);
	for (size_t i = 0; !e && i < n; ++i) {
		struct json_object *obj = jb_at(items, i);
		struct json_object *blobs = jb_get(obj, "blobs");
		struct cstr_view const id = jb_view(jb_get(obj, "id"));
		struct seen_item *it = &seen->items.items[seen->items.n++];
		*it = (struct seen_item){
			.record = DSTR_INIT,
			.output = { .len = (size_t)jb_uint(jb_get(obj, "bytes"), 0) },
			.shown  = (uint32_t)jb_uint(jb_get(obj, "shown"), 0),
			.turn   = (uint32_t)jb_uint(jb_get(obj, "turn"), 0),
		};
		memcpy(it->id, id.data, id.len);
		/* seen_item_valid() checked the git IDs, and "" is no name. */
		for (size_t k = 0; k < array_size(it->blobs); ++k)
			(void)oid_init(&it->blobs[k], jb_view(jb_at(blobs, k)));
		e = cstr_view_store(jb_view(jb_get(obj, "record")), &it->record);
	}
	if (e) {
		seen_items_drop(seen);
		return e;
	}
	seen->resumed = (uint32_t)made;
	return 0;
}

enum seen_where
seen_find (struct seen const       *seen,
           uint32_t                 number,
           struct seen_call const **call)
{
	*call = nullptr;
	if (!seen || !number)
		return SEEN_MISSING;
	for (size_t i = 0; i < seen->n; ++i) {
		struct seen_call const *c = &seen->calls[seen_call_place(seen, i)];
		if (c->number == number) {
			*call = c;
			return SEEN_KEPT;
		}
	}
	if (number <= seen->resumed)
		return SEEN_RESTARTED;
	return seen->n && number < seen_oldest(seen) ? SEEN_DROPPED : SEEN_MISSING;
}

uint32_t
seen_oldest (struct seen const *seen)
{
	return seen && seen->n ? seen->calls[seen->head].number : 0;
}

/* Returns the index in seen->items.items of the item of @p seen with the
 * ID @p id, or SEEN_ITEMS for no such item.
 */
static size_t
seen_item_index (struct seen const *seen,
                 struct cstr_view   id)
{
	for (size_t i = 0; i < seen->items.n; ++i) {
		size_t const k = seen_item_place(seen, i);
		if (cstr_view_equal(cstr_view(seen->items.items[k].id), id))
			return k;
	}
	return SEEN_ITEMS;
}

int
seen_print (struct seen            **p_seen,
            struct seen_item const  *item,
            uint32_t                 call,
            struct extent            output)
{
	if (!p_seen || !item)
		return EFAULT;
	struct cstr_view const id = { item->id, strnlen(item->id, sizeof item->id) };
	if (!call || !id.len || id.len == sizeof item->id || !output.len ||
	    output.len > TOOL_OUTPUT_MAX || output.at > TOOL_OUTPUT_MAX - output.len)
		return EINVAL;
	int e = seen_get(p_seen);
	if (e)
		return e;
	struct seen *seen = *p_seen;
	if (util_grow(seen->printed.items, seen->printed.cap, seen->printed.n + 1))
		return ENOMEM;
	size_t k = seen_item_index(seen, id);
	bool const added = k == SEEN_ITEMS;
	if (added) {
		/* A new item goes after the newest item. In a full ring, that is
		 * the place of the oldest item, which leaves the ring. */
		k = seen_item_place(seen, seen->items.n);
		if (seen->items.n == SEEN_ITEMS) {
			dstr_cleanup(&seen->items.items[k].record);
			seen->items_head = seen_item_place(seen, 1);
			--seen->items.n;
		}
		if (util_grow(seen->items.items, seen->items.cap, k + 1))
			return ENOMEM;
		seen->items.items[k] = (struct seen_item){ .record = DSTR_INIT };
	}
	struct seen_item *it = &seen->items.items[k];
	/* A new item, or an item of another size, which is not the item that
	 * the turns saw, starts with no byte shown. */
	if (it->output.len != output.len) {
		it->output.len = 0;
		it->shown = 0;
		it->turn = 0;
		it->call = 0;
		if (cstr_view_store(cstr_view_dstr(&item->record), &it->record))
			return ENOMEM;
		memcpy(it->blobs, item->blobs, sizeof it->blobs);
		memcpy(it->id, id.data, id.len + 1);
		seen->items.n += added;
	}
	if (it->call != call)
		seen->printed.items[seen->printed.n++] = k;
	it->output = output;
	it->call = call;
	return 0;
}

void
seen_shown (struct seen   *seen,
            uint32_t       call,
            struct extent  window,
            uint32_t       turn)
{
	if (!seen || !call)
		return;
	size_t kept = 0;
	for (size_t i = 0; i < seen->printed.n; ++i) {
		size_t const k = seen->printed.items[i];
		struct seen_item *it = &seen->items.items[k];
		if (it->call != call) {
			/* An item that another call printed last stays on the list
			 * for the result of that call. */
			if (it->call)
				seen->printed.items[kept++] = k;
			continue;
		}
		it->call = 0;
		/* The window continues the bytes shown before if it starts at
		 * or before their end and ends after it. */
		size_t const end = it->output.at + it->shown;
		if (window.at > end || window.len <= end - window.at)
			continue;
		size_t const to = window.at + window.len - it->output.at;
		it->shown = (uint32_t)(to < it->output.len ? to : it->output.len);
		if (it->shown == it->output.len && !it->turn)
			it->turn = turn;
	}
	seen->printed.n = kept;
}

struct json_object *
seen_items_json (struct seen const *seen,
                 int               *e)
{
	if (!e)
		return nullptr;
	struct json_object *list = json_object_new_array();
	if (!seen) {
		*e = list ? 0 : ENOMEM;
		return list;
	}
	for (size_t i = 0; list && i < seen->items.n; ++i) {
		struct seen_item const *it = &seen->items.items[seen_item_place(seen, i)];
		if (!it->shown)
			continue;
		struct json_object *blobs = jb_push_or_free(jb_list(json_object_new_string(
			it->blobs[0].hex)), json_object_new_string(it->blobs[1].hex));
		list = jb_push_or_free(list, jb_object(
			"id", json_object_new_string(it->id),
			"record", jb_string(cstr_view_dstr(&it->record)),
			"blobs", blobs,
			"bytes", json_object_new_uint64(it->output.len),
			"shown", json_object_new_uint64(it->shown),
			"turn", json_object_new_uint64(it->turn),
			nullptr));
	}
	*e = list ? 0 : ENOMEM;
	return list;
}

struct seen_item const *
seen_find_item (struct seen const *seen,
                struct cstr_view   id)
{
	if (!seen)
		return nullptr;
	size_t const k = seen_item_index(seen, id);
	return k < SEEN_ITEMS ? &seen->items.items[k] : nullptr;
}

/* Appends to @p d the notice of the items of @p seen that results showed
 * whole, if there are any. Returns 0 or ENOMEM.
 */
static int
seen_notice_items (struct seen const *seen,
                   struct dstr       *d)
{
	struct cstr_view ids[SEEN_LISTED];
	size_t whole = 0;
	for (size_t i = 0; i < seen->items.n; ++i) {
		struct seen_item const *it = &seen->items.items[seen_item_place(seen, i)];
		if (!it->turn)
			continue;
		if (whole < array_size(ids))
			ids[whole] = cstr_view(it->id);
		++whole;
	}
	if (!whole)
		return 0;
	size_t const listed = whole < array_size(ids) ? whole : array_size(ids);
	bool const ok = dstr_cat(d, "[askai: Evidence items that you expanded in this ask call: ") &&
	                !cstr_view_join_more(ids, listed, whole, d) &&
	                dstr_cat(d, ". These items count as seen.]\n");
	return ok ? 0 : ENOMEM;
}

int
seen_notice (struct seen const *seen,
             struct dstr       *d)
{
	if (!seen)
		return 0;
	uint32_t oldest = 0;
	uint32_t newest = 0;
	for (size_t i = 0; i < seen->n; ++i) {
		struct seen_call const *c = &seen->calls[seen_call_place(seen, i)];
		if (c->flags & SEEN_TOO_LARGE)
			continue;
		if (!oldest)
			oldest = c->number;
		newest = c->number;
	}
	if (!oldest)
		return seen_notice_items(seen, d);
	/* The range of the calls, written once, and the plural for more than
	 * one call. */
	char span[sizeof "c4294967295 to c4294967295"];
	char const *s = "";
	size_t const n = util_format(span, sizeof span, "c%" PRIu32, oldest);
	if (oldest != newest) {
		(void)util_format(span + n, sizeof span - n, " to c%" PRIu32, newest);
		s = "s";
	}
	if (!dstr_printf(d, "[askai: askai keeps your call%s %s of this ask call. Cite the call%s "
	                 "as evidence by number.]\n", s, span, s))
		return ENOMEM;
	return seen_notice_items(seen, d);
}

void
seen_clear (struct seen *seen)
{
	if (!seen)
		return;
	while (seen->n)
		seen_drop(seen);
	seen_items_drop(seen);
	seen->head = 0;
	seen->resumed = 0;
}

size_t
seen_kept (struct seen const *seen)
{
	if (!seen)
		return 0;
	size_t bytes = sizeof *seen;
	for (size_t i = 0; i < SEEN_CALLS; ++i) {
		struct seen_call const *c = &seen->calls[i];
		bytes += c->request.cap + (dstr_get_block_count(&c->result) +
		                           dstr_get_block_count(&c->ids)) * DSTR_BLK_SIZE;
	}
	bytes += seen->items.cap * sizeof *seen->items.items +
	         seen->printed.cap * sizeof *seen->printed.items;
	for (size_t i = 0; i < seen->items.n; ++i) {
		struct seen_item const *it = &seen->items.items[seen_item_place(seen, i)];
		bytes += dstr_get_block_count(&it->record) * DSTR_BLK_SIZE;
	}
	return bytes;
}

void
seen_destroy (struct seen **p_seen)
{
	if (!p_seen || !*p_seen)
		return;
	struct seen *seen = *p_seen;
	for (size_t i = 0; i < SEEN_CALLS; ++i) {
		buf_fini(&seen->calls[i].request);
		dstr_cleanup(&seen->calls[i].result);
		dstr_cleanup(&seen->calls[i].ids);
	}
	seen_items_drop(seen);
	free(seen->items.items);
	seen->items.items = nullptr;
	free(seen->printed.items);
	seen->printed.items = nullptr;
	free(seen);
	*p_seen = nullptr;
}
