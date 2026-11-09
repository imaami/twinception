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
seen_source_within (struct call_source const *src,
                    struct cstr_view          out)
{
	return src->len && src->len <= out.len && src->at <= out.len - src->len;
}

struct seen_read *
seen_read_create (struct call_source const *src,
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
	if (!oid_valid(src->commit) ||
	    !seen_name_valid(src->repo) || !seen_name_valid(src->path) || !src->first ||
	    src->last < src->first || (src->total && src->total < src->last) ||
	    src->total > UINT32_MAX || !seen_source_within(src, out))
		return nullptr;
	/* Each numbered line ends with a newline, except perhaps the last. */
	struct cstr_view const lines = { out.data + src->at, src->len };
	if (cstr_view_count(lines, '\n') + (lines.data[lines.len - 1] != '\n') !=
	    (uint64_t)src->last - src->first + 1)
		return nullptr;
	struct seen_read *read = malloc(sizeof *read + src->repo.len + src->commit.len +
	                                src->path.len);
	if (!read) {
		*e = ENOMEM;
		return nullptr;
	}
	*read = (struct seen_read){ .src = *src, .mark = { .line = src->first, .at = src->at } };
	read->src.repo.data = read->text;
	read->src.commit.data = mempcpy(read->text, src->repo.data, src->repo.len);
	read->src.path.data = mempcpy((char *)read->src.commit.data, src->commit.data,
	                              src->commit.len);
	memcpy((char *)read->src.path.data, src->path.data, src->path.len);
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
	struct call_source const *src = &read->src;
	struct json_object *obj = jb_object("repo", jb_string(src->repo),
	                                    "commit", jb_string(src->commit),
	                                    "path", jb_string(src->path),
	                                    "at", json_object_new_uint64(src->at),
	                                    "len", json_object_new_uint64(src->len),
	                                    "first", json_object_new_uint64(src->first),
	                                    "last", json_object_new_uint64(src->last),
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
	if (jb_uint_checked(jb_get(obj, "at"), SIZE_MAX, &at) ||
	    jb_uint_checked(jb_get(obj, "len"), SIZE_MAX, &len) ||
	    jb_uint_checked(jb_get(obj, "first"), UINT32_MAX, &first) ||
	    jb_uint_checked(jb_get(obj, "last"), UINT32_MAX, &last) ||
	    jb_uint_checked(jb_get(obj, "total"), UINT64_MAX, &total)) {
		*e = EINVAL;
		return nullptr;
	}
	/* seen_read_create() checks the texts. */
	struct call_source const src = {
		.repo   = jb_view(jb_get(obj, "repo")),
		.commit = jb_view(jb_get(obj, "commit")),
		.path   = jb_view(jb_get(obj, "path")),
		.total  = total,
		.at     = (size_t)at,
		.len    = (size_t)len,
		.first  = (uint32_t)first,
		.last   = (uint32_t)last,
	};
	return seen_read_create(&src, out, e);
}

struct seen_lines
seen_lines (struct seen_mark         *mark,
            struct call_source const *src,
            struct cstr_view          out,
            size_t                    skip,
            size_t                    window,
            int                      *e)
{
	if (!e)
		return (struct seen_lines){};
	if (!mark || !src || (!out.data && out.len)) {
		*e = EFAULT;
		return (struct seen_lines){};
	}
	*e = !src->len || src->len > out.len || src->at > out.len - src->len ? EINVAL : 0;
	size_t const end = src->at + src->len;
	if (*e || skip >= end)
		return (struct seen_lines){};
	size_t lo = skip > src->at ? skip : src->at;
	size_t const hi = window < end - skip ? skip + window : end;
	if (lo >= hi)
		return (struct seen_lines){};
	char const *p = out.data;
	/* Count the lines from the mark of the last window, if there is one
	 * before this window. */
	if (!mark->line || mark->at > lo)
		*mark = (struct seen_mark){ .line = src->first, .at = src->at };
	mark->line += cstr_view_count((struct cstr_view){ p + mark->at, lo - mark->at }, '\n');
	mark->at = lo;
	uint64_t line = mark->line;
	/* Skip a line whose start is before the window. */
	if (lo > src->at && p[lo - 1] != '\n') {
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
	if (line + n - 1 > src->last || lo - skip > UINT32_MAX) {
		*e = EINVAL;
		return (struct seen_lines){};
	}
	return (struct seen_lines){ (uint32_t)line, (uint32_t)(line + n - 1), (uint32_t)(lo - skip) };
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
	seen->head = (uint16_t)((seen->head + 1) % SEEN_CALLS);
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
	return &seen->calls[(seen->head + seen->n) % SEEN_CALLS];
}

/* Appends the identifiers of the file @p src to @p ids as a JSON object
 * with the members repo, commit, path and total. Returns 0 or ENOMEM.
 */
static int
seen_ids (struct dstr              *ids,
          struct call_source const *src)
{
	int e = cstr_view_cat(CSTR_VIEW("{\"repo\":\""), ids);
	e = e ? e : jb_escape_cat(src->repo, ids);
	e = e ? e : cstr_view_cat(CSTR_VIEW("\",\"commit\":\""), ids);
	e = e ? e : cstr_view_cat(src->commit, ids);
	e = e ? e : cstr_view_cat(CSTR_VIEW("\",\"path\":\""), ids);
	e = e ? e : jb_escape_cat(src->path, ids);
	return e ? e : dstr_printf(ids, "\",\"total\":%" PRIu64 "}", src->total) ? 0 : ENOMEM;
}

/* Returns a new ring without calls, or nullptr with ENOMEM in @p *e. */
static struct seen *
seen_create (int *e)
{
	struct seen *seen = malloc(sizeof *seen);
	if (!seen) {
		*e = ENOMEM;
		return nullptr;
	}
	*seen = (struct seen){};
	for (size_t i = 0; i < SEEN_CALLS; ++i)
		seen->calls[i] = (struct seen_call){ .result = DSTR_INIT, .ids = DSTR_INIT,
		                                     .tool = TOOL_MAX };
	*e = 0;
	return seen;
}

int
seen_keep (struct seen              **p_seen,
           struct buf                *request,
           struct seen_result const  *r,
           struct seen_lines         *lines)
{
	if (lines)
		*lines = (struct seen_lines){};
	if (!p_seen || !request || !r || !r->parts || (!r->out.data && r->out.len))
		return EFAULT;
	size_t const window = r->parts[0].len;
	if (r->skip > r->out.len || window > r->out.len - r->skip ||
	    (window && r->parts[0].data != r->out.data + r->skip) ||
	    r->at > TOOL_OUTPUT_MAX || r->skip > TOOL_OUTPUT_MAX - r->at)
		return EINVAL;
	int e = 0;
	struct seen_read *read = r->read;
	struct seen_lines const whole = read ? seen_lines(&read->mark, &read->src, r->out, r->skip,
	                                                  window, &e)
	                                     : (struct seen_lines){};
	if (!e && !*p_seen)
		*p_seen = seen_create(&e);
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
	if (lines)
		*lines = whole;
	return 0;
}

int
seen_keep_number (struct seen **p_seen,
                  uint32_t      number,
                  uint32_t      tool)
{
	if (!p_seen)
		return EFAULT;
	int e = 0;
	if (!*p_seen)
		*p_seen = seen_create(&e);
	if (e)
		return e;
	struct seen *seen = *p_seen;
	if (seen->numbers.n == SEEN_NUMBERS) {
		--seen->numbers.n;
		memmove(seen->numbers.items, seen->numbers.items + 1,
		        seen->numbers.n * sizeof *seen->numbers.items);
	}
	if (util_grow(seen->numbers.items, seen->numbers.cap, seen->numbers.n + 1))
		return ENOMEM;
	seen->numbers.items[seen->numbers.n++] = (struct seen_number){ number, tool };
	return 0;
}

int
seen_resume (struct seen **p_seen,
             uint32_t      calls)
{
	if (!p_seen)
		return EFAULT;
	int e = 0;
	if (!*p_seen)
		*p_seen = seen_create(&e);
	if (!e)
		(*p_seen)->resumed = calls;
	return e;
}

enum seen_where
seen_find (struct seen const       *seen,
           uint32_t                 number,
           struct seen_call const **call,
           uint32_t                *tool)
{
	*call = nullptr;
	*tool = TOOL_MAX;
	if (!seen || !number)
		return SEEN_MISSING;
	for (size_t i = 0; i < seen->n; ++i) {
		struct seen_call const *c = &seen->calls[(seen->head + i) % SEEN_CALLS];
		if (c->number == number) {
			*call = c;
			return SEEN_KEPT;
		}
	}
	for (size_t i = 0; i < seen->numbers.n; ++i)
		if (seen->numbers.items[i].number == number) {
			*tool = seen->numbers.items[i].tool;
			return SEEN_NUMBER;
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

int
seen_notice (struct seen const *seen,
             struct dstr       *d)
{
	if (!seen)
		return 0;
	uint32_t oldest = 0;
	uint32_t newest = 0;
	for (size_t i = 0; i < seen->n; ++i) {
		struct seen_call const *c = &seen->calls[(seen->head + i) % SEEN_CALLS];
		if (c->flags & SEEN_TOO_LARGE)
			continue;
		if (!oldest)
			oldest = c->number;
		newest = c->number;
	}
	if (!oldest)
		return 0;
	bool ok;
	if (oldest == newest)
		ok = dstr_printf(d, "[askai: askai keeps your call c%" PRIu32 " of this ask call. "
		                 "Cite the call as evidence by number.]\n", oldest);
	else
		ok = dstr_printf(d, "[askai: askai keeps your calls c%" PRIu32 " to c%" PRIu32 " of "
		                 "this ask call. Cite the calls as evidence by number.]\n", oldest,
		                 newest);
	return ok ? 0 : ENOMEM;
}

void
seen_clear (struct seen *seen)
{
	if (!seen)
		return;
	while (seen->n)
		seen_drop(seen);
	seen->numbers.n = 0;
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
	bytes += seen->numbers.cap * sizeof *seen->numbers.items;
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
	free(seen->numbers.items);
	seen->numbers.items = nullptr;
	free(seen);
	*p_seen = nullptr;
}
