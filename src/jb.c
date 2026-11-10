/** @file jb.c
 *
 * Terse, leak-safe helpers for building and reading json-c values.
 */
#include <errno.h>
#include <json-c/printbuf.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dstr.h"
#include "jb.h"
#include "util.h"

struct json_object *
jb_object (char const *key,
           ...)
{
	struct json_object *obj = json_object_new_object();
	va_list ap;
	va_start(ap, key);
	for (; key; key = va_arg(ap, char const *))
		obj = jb_set_or_free(obj, key, va_arg(ap, struct json_object *));
	va_end(ap);
	return obj;
}

struct json_object *
jb_list (struct json_object *val)
{
	return jb_push_or_free(json_object_new_array(), val);
}

struct json_object *
jb_parse (char const *data,
          size_t      len,
          int         flags,
          uint32_t    depth)
{
	struct jb_pool pool = { .depth = depth, .flags = flags };
	struct jb_parser parser = {};
	while (jb_parser_step(&parser, &pool, data, len, SIZE_MAX, UINT32_MAX) == EAGAIN)
		;
	struct json_object *root = jb_parser_take(&parser);
	jb_parser_fini(&parser, &pool, nullptr);
	(void)jb_pool_release(&pool, 0);
	return root;
}

/* Opens a container, and counts its members if @p members limits them.
 * Returns 0 or ENOMEM.
 */
static int
jb_scan_open (struct jb_scan *scan,
              uint32_t        members)
{
	if (members != UINT32_MAX) {
		if (util_grow(scan->members, scan->cap, scan->depth + 1))
			return ENOMEM;
		scan->members[scan->depth] = 0;
	}
	++scan->depth;
	++scan->values;
	scan->state = JB_SCAN_GAP;
	return 0;
}

/* Skip @p data, within a string, to its next quote, or to a backslash before
 * it, counting the bytes; @p *quote is the next quote once looked for, else
 * @p end. Returns where it stopped, @p end if the string goes on past it.
 */
static char const *
jb_scan_string (struct jb_scan  *scan,
                char const      *data,
                char const      *end,
                char const     **quote)
{
	if (!*quote || *quote < data) {
		*quote = memchr(data, '"', (size_t)(end - data));
		if (!*quote)
			*quote = end;
	}
	char const *slash = memchr(data, '\\', (size_t)(*quote - data));
	if (slash) {
		scan->run += (size_t)(slash - data);
		scan->state = JB_SCAN_ESCAPE;
		return slash;
	}
	scan->run += (size_t)(*quote - data);
	if (*quote == end)
		return end;
	scan->copied += scan->run;
	scan->state = JB_SCAN_GAP;
	return *quote;
}

size_t
jb_scan (struct jb_scan *scan,
         char const     *data,
         size_t          len,
         uint32_t        depth,
         uint32_t        values,
         uint32_t        members,
         uint32_t        scalar)
{
	char const *start = data;
	char const *end = data + len;
	char const *quote = nullptr; /* the next '"' once looked for, else end */
	for (; data < end; ++data) {
		char c = *data;
		switch (scan->state) {
		case JB_SCAN_STRING:
			data = jb_scan_string(scan, data, end, &quote);
			if (data == end)
				return len;
			continue;
		case JB_SCAN_ESCAPE:
			scan->run += 2;
			scan->state = JB_SCAN_STRING;
			continue;
		default:
			break;
		}
		switch (c) {
		case '{':
		case '[':
			if (scan->depth == depth || scan->values == values ||
			    jb_scan_open(scan, members))
				return (size_t)(data - start);
			break;
		case '"':
			if (scan->values == values)
				return (size_t)(data - start);
			++scan->values;
			scan->run = 0;
			scan->state = JB_SCAN_STRING;
			break;
		case ':':
			/* Each member of an object has one, outside strings. */
			if (members != UINT32_MAX && scan->depth &&
			    scan->members[scan->depth - 1]++ == members) {
				--scan->members[scan->depth - 1];
				return (size_t)(data - start);
			}
			scan->state = JB_SCAN_GAP;
			break;
		case '}':
		case ']':
			scan->depth -= scan->depth > 0;
			[[fallthrough]];
		case ',':
		case ' ':
		case '\t':
		case '\r':
		case '\n':
			scan->state = JB_SCAN_GAP;
			break;
		default:
			if (scan->state != JB_SCAN_SCALAR) {
				if (scan->values == values)
					return (size_t)(data - start);
				++scan->values;
				scan->run = 0;
				scan->state = JB_SCAN_SCALAR;
			}
			if (scan->run == scalar)
				return (size_t)(data - start);
			++scan->run;
		}
	}
	return len;
}

void
jb_scan_fini (struct jb_scan *scan)
{
	free(scan->members);
	*scan = (struct jb_scan){};
}

void
jb_reaper_add (struct jb_reaper   *reaper,
               struct json_object *val)
{
	bool container = json_object_is_type(val, json_type_array) ||
	                 json_object_is_type(val, json_type_object);
	if (!container || util_grow(reaper->items, reaper->cap, reaper->n + 1)) {
		json_object_put(val);
		return;
	}
	reaper->items[reaper->n++] = val;
}

/* Take a value out of the container @p top, in @p *val, a reference of its
 * own: an array's last element, or an object's first member, each O(1).
 * False if @p top is empty.
 */
static bool
jb_pop (struct json_object  *top,
        struct json_object **val)
{
	if (json_object_is_type(top, json_type_array)) {
		size_t len = json_object_array_length(top);
		if (!len)
			return false;
		*val = json_object_get(json_object_array_get_idx(top, len - 1));
		json_object_array_del_idx(top, len - 1, 1);
		return true;
	}
	struct lh_entry *e = lh_table_head(json_object_get_object(top));
	if (!e)
		return false;
	*val = json_object_get(lh_entry_v(e));
	json_object_object_del(top, lh_entry_k(e));
	return true;
}

/*
 * Take containers apart from the innermost: each value taken out of one is
 * freed, or taken apart in turn, and a container is freed once empty.
 */
size_t
jb_reaper_step (struct jb_reaper *reaper,
                size_t            values)
{
	size_t freed = 0;
	while (freed < values && reaper->n) {
		struct json_object *val = nullptr;
		if (!jb_pop(reaper->items[reaper->n - 1], &val)) {
			json_object_put(reaper->items[--reaper->n]);
			reaper->items[reaper->n] = nullptr;
			++freed;
			continue;
		}
		/* A container goes onto the stack, and any other value is freed. */
		size_t const n = reaper->n;
		jb_reaper_add(reaper, val);
		freed += reaper->n == n;
	}
	return freed;
}

void
jb_reaper_fini (struct jb_reaper *reaper)
{
	while (reaper->n)
		json_object_put(reaper->items[--reaper->n]);
	free(reaper->items);
	*reaper = (struct jb_reaper){};
}

size_t
jb_pool_release (struct jb_pool *pool,
                 size_t          keep)
{
	for (uint32_t i = pool->n; i--; ) {
		struct json_tokener *tok = pool->free[i];
		size_t size = (size_t)tok->pb->size;
		if (size > keep) {
			pool->bytes -= size;
			pool->free[i] = pool->free[--pool->n];
			pool->free[pool->n] = nullptr;
			json_tokener_free(tok);
			return size;
		}
	}
	return 0;
}

/* A tokener whose stack holds nothing, from @p pool or new; nullptr if out of
 * memory.
 */
static struct json_tokener *
jb_pool_take (struct jb_pool *pool)
{
	if (pool->n) {
		struct json_tokener *tok = pool->free[--pool->n];
		pool->free[pool->n] = nullptr;
		pool->bytes -= (size_t)tok->pb->size;
		return tok;
	}
	/* json-c counts a value inside the innermost container as a level too. */
	struct json_tokener *tok = pool->depth && pool->depth < INT_MAX
	                           ? json_tokener_new_ex((int)pool->depth + 1)
	                           : json_tokener_new();
	if (tok)
		json_tokener_set_flags(tok, pool->flags);
	return tok;
}

/* End @p parser's parse with the error @p e, and return it. */
static int
jb_parser_end (struct jb_parser *parser,
               int               e)
{
	parser->done = true;
	parser->error = e;
	return e;
}

int
jb_parser_step (struct jb_parser *parser,
                struct jb_pool   *pool,
                char const       *data,
                size_t            len,
                size_t            bytes,
                uint32_t          values)
{
	if (parser->done)
		return parser->error;
	if (!parser->tok && !(parser->tok = jb_pool_take(pool)))
		return jb_parser_end(parser, ENOMEM);

	/* Up to the value that would be one too many; within a string, not
	 * within a character: json-c finds UTF-8 split between slices invalid. */
	size_t off = parser->off;
	size_t room = len - off < bytes ? len - off : bytes;
	if (room > INT_MAX - 1)
		room = INT_MAX - 1;
	uint32_t most = UINT32_MAX - parser->cut.values < values ? UINT32_MAX
	              : parser->cut.values + values;
	uint32_t scalar = pool->scalar ? pool->scalar : UINT32_MAX;
	size_t end = off + jb_scan(&parser->cut, data + off, room, UINT32_MAX, most,
	                           UINT32_MAX, scalar);
	if (end < off + room && parser->cut.state == JB_SCAN_SCALAR &&
	    parser->cut.run == scalar)
		return jb_parser_end(parser, EINVAL);
	for (int i = 0; i < 3 && end > off + 1 && end < len &&
	                ((unsigned char)data[end] & 0xc0) == 0x80; ++i)
		--end;

	struct json_tokener *tok = parser->tok;
	if (!parser->root) {
		parser->root = json_tokener_parse_ex(tok, data + off, (int)(end - off));
		if (parser->root)
			parser->off += json_tokener_get_parse_end(tok);
		/* A number or literal at the very end of the data only ends at a
		 * terminator, so the tokener gets one there. */
		else if (json_tokener_get_error(tok) != json_tokener_continue ||
		         ((parser->off = end) == len &&
		          !(parser->root = json_tokener_parse_ex(tok, "", 1))))
			return jb_parser_end(parser, json_tokener_get_error(tok) ==
			                             json_tokener_error_memory ? ENOMEM : EINVAL);
	}

	/* What follows the value must be white space, however it is sliced. */
	while (parser->root && parser->off < end && cstr_view_space(data[parser->off]))
		++parser->off;
	if (parser->off != end)
		return jb_parser_end(parser, EINVAL);
	return end == len ? jb_parser_end(parser, 0) : EAGAIN;
}

/*
 * json-c keeps a parse's unfinished containers on its stack, each holding the
 * values finished inside it, and json_tokener_reset() frees them all at once:
 * take them first. This reads fields json-c documents as its own; they have
 * been stable since 0.12, and with them gone this would not compile.
 */
void
jb_parser_fini (struct jb_parser *parser,
                struct jb_pool   *pool,
                struct jb_reaper *reaper)
{
	struct json_tokener *tok = parser->tok;
	if (reaper) {
		for (int i = 0; tok && i <= tok->depth; ++i)
			if (tok->stack[i].current)
				jb_reaper_add(reaper, json_object_get(tok->stack[i].current));
		if (parser->root)
			jb_reaper_add(reaper, parser->root);
	} else {
		json_object_put(parser->root);
	}
	if (tok) {
		json_tokener_reset(tok);
		if (pool->n < array_size(pool->free)) {
			pool->bytes += (size_t)tok->pb->size;
			pool->free[pool->n++] = tok;
		} else {
			json_tokener_free(tok);
		}
	}
	*parser = (struct jb_parser){};
}

size_t
jb_utf8 (char const *str,
         size_t      len)
{
	unsigned char const *s = (unsigned char const *)str;
	size_t i = 0;
	for (size_t n; i < len; i += n) {
		unsigned c = s[i];
		n = c < 0x80 ? 1 : c < 0xc2 ? 0 : c < 0xe0 ? 2 : c < 0xf0 ? 3 :
		    c < 0xf5 ? 4 : 0;
		if (!n || n > len - i)
			break;
		if (n == 1)
			continue;
		/* The second byte's range rules out overlong forms, surrogates
		 * and code points past U+10FFFF. */
		unsigned lo = c == 0xe0 ? 0xa0 : c == 0xf0 ? 0x90 : 0x80;
		unsigned hi = c == 0xed ? 0x9f : c == 0xf4 ? 0x8f : 0xbf;
		if (s[i + 1] < lo || s[i + 1] > hi)
			break;
		for (size_t k = 2; k < n; ++k)
			if ((s[i + k] & 0xc0) != 0x80)
				return i;
	}
	return i;
}

size_t
jb_text_len (struct cstr_view t)
{
	if (!t.data)
		return 0;
	size_t const run = jb_utf8(t.data, t.len);
	char const *nul = memchr(t.data, '\0', run);
	return nul ? (size_t)(nul - t.data) : run;
}

bool
jb_is_text (struct cstr_view t)
{
	return jb_text_len(t) == t.len;
}

size_t
jb_mend (char   *p,
         size_t  n,
         bool    last)
{
	size_t i = 0;
	while ((i += jb_utf8(p + i, n - i)) < n) {
		if (!last && n - i < 4)
			return i;
		p[i++] = '?';
	}
	return n;
}

/* The letter of the two-byte escape of each byte that has one; the other
 * bytes below a space are escaped as \u00XX. The table ends at '\\', the
 * last byte that JSON escapes.
 */
static char const jb_letters['\\' + 1] = {
	['\b'] = 'b', ['\t'] = 't', ['\n'] = 'n', ['\f'] = 'f', ['\r'] = 'r',
	['"'] = '"', ['\\'] = '\\',
};

/* Whether JSON escapes the byte @p c in a string. */
static inline bool
jb_escaped (unsigned char c)
{
	return c < ' ' || c == '"' || c == '\\';
}

size_t
jb_escape (char       *dst,
           char const *src,
           size_t      len)
{
	if (!dst || !src)
		return 0;
	char *out = dst;
	for (size_t i = 0; i < len; ++i) {
		unsigned char c = (unsigned char)src[i];
		if (!jb_escaped(c)) {
			*out++ = (char)c;
			continue;
		}
		*out++ = '\\';
		if (jb_letters[c]) {
			*out++ = jb_letters[c];
			continue;
		}
		*out++ = 'u';
		*out++ = '0';
		*out++ = '0';
		util_hex(out, &c, 1);
		out += 2;
	}
	return (size_t)(out - dst);
}

int
jb_escape_cat (struct cstr_view  text,
               struct dstr      *dest)
{
	/* Escape the text a part at a time: jb_escape() writes at most 6 bytes
	 * for a byte. */
	char part[6 * 64];
	for (size_t i = 0, n; i < text.len; i += n) {
		n = text.len - i < sizeof part / 6 ? text.len - i : sizeof part / 6;
		if (!dstr_cat_n(dest, part, jb_escape(part, text.data + i, n)))
			return ENOMEM;
	}
	return 0;
}

size_t
jb_escape_fit (char const *src,
               size_t      len,
               size_t      room)
{
	if (!src)
		return 0;
	size_t i = 0;
	for (size_t used = 0; i < len; ++i) {
		unsigned char c = (unsigned char)src[i];
		used += !jb_escaped(c) ? 1 : jb_letters[c] ? 2 : 6;
		if (used > room)
			break;
	}
	return jb_utf8(src, i);
}
