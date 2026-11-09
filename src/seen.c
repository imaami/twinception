/** @file seen.c
 *
 * The calls whose results askai showed to the turns of an ask call
 * (src/seen.h).
 */
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "jb.h"
#include "review.h"
#include "seen.h"
#include "tool.h"
#include "util.h"

struct seen_lines
seen_lines (struct seen_mark           *mark,
            struct review_source const *src,
            struct cstr_view            out,
            size_t                      skip,
            size_t                      window,
            int                        *e)
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
	if (line + n - 1 > src->last) {
		*e = EINVAL;
		return (struct seen_lines){};
	}
	return (struct seen_lines){ (uint32_t)line, (uint32_t)(line + n - 1) };
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
	seen->head = (seen->head + 1) % SEEN_CALLS;
	--seen->n;
}

/* Appends the identifiers of the file @p src to @p ids as a JSON object
 * with the members repo, commit, path and total. Returns 0 or ENOMEM.
 */
static int
seen_ids (struct dstr                *ids,
          struct review_source const *src)
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
	    (window && r->parts[0].data != r->out.data + r->skip))
		return EINVAL;
	int e = 0;
	struct review_read *read = r->read;
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
	while (seen->n && (seen->n == SEEN_CALLS || seen->bytes + bytes > SEEN_BYTES))
		seen_drop(seen);
	struct seen_call *c = &seen->calls[(seen->head + seen->n) % SEEN_CALLS];
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
	c->skip = r->at + r->skip;
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

void
seen_clear (struct seen *seen)
{
	if (!seen)
		return;
	while (seen->n)
		seen_drop(seen);
	seen->head = 0;
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
	free(seen);
	*p_seen = nullptr;
}
