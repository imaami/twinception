/** @file Terse, leak-safe helpers for building and reading json-c values. */
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jb.h"
#include "util.h"

struct json_object *
jb_object (char const *key,
           ...)
{
	struct json_object *obj = json_object_new_object();
	va_list ap;
	va_start(ap, key);
	for (; key; key = va_arg(ap, char const *)) {
		struct json_object *val = va_arg(ap, struct json_object *);
		if (!obj) {
			json_object_put(val);
		} else if (!jb_set(obj, key, val)) {
			json_object_put(obj);
			obj = nullptr;
		}
	}
	va_end(ap);
	return obj;
}

struct json_object *
jb_list (struct json_object *val)
{
	struct json_object *arr = json_object_new_array();
	if (!arr) {
		json_object_put(val);
		return nullptr;
	}
	if (!jb_push(arr, val)) {
		json_object_put(arr);
		return nullptr;
	}
	return arr;
}

bool
jb_set (struct json_object *obj,
        char const         *key,
        struct json_object *val)
{
	if (obj && val && !json_object_object_add(obj, key, val))
		return true;
	json_object_put(val);
	return false;
}

bool
jb_push (struct json_object *arr,
         struct json_object *val)
{
	if (arr && val && !json_object_array_add(arr, val))
		return true;
	json_object_put(val);
	return false;
}

struct json_object *
jb_get (struct json_object const *obj,
        char const               *key)
{
	struct json_object *val;
	return json_object_object_get_ex(obj, key, &val) ? val : nullptr;
}

struct json_object *
jb_at (struct json_object const *arr,
       size_t                    idx)
{
	return json_object_is_type(arr, json_type_array) &&
	       idx < json_object_array_length(arr)
		? json_object_array_get_idx(arr, idx) : nullptr;
}

char const *
jb_text (struct json_object *val)
{
	return json_object_is_type(val, json_type_string)
		? json_object_get_string(val) : nullptr;
}

bool
jb_is (struct json_object *val,
       char const         *str)
{
	char const *text = jb_text(val);
	return text && !strcmp(text, str);
}

struct json_object *
jb_parse (char const *data,
          size_t      len,
          int         flags,
          uint32_t    depth)
{
	struct jb_pool pool = { .depth = depth, .flags = flags };
	struct jb_parser parser = {};
	while (!jb_parse_some(&parser, &pool, data, len, SIZE_MAX, UINT32_MAX))
		;
	struct json_object *root = jb_parse_take(&parser);
	jb_parse_end(&parser, &pool, nullptr);
	jb_pool_fini(&pool);
	return root;
}

/* Open a container, counting its members if @p members limits them; false
 * if out of memory. */
static bool
scan_open (struct jb_scan *scan,
           uint32_t        members)
{
	if (members != UINT32_MAX) {
		if (scan->depth == scan->cap) {
			uint32_t cap = scan->cap ? 2 * scan->cap : 32;
			uint32_t *m = realloc(scan->members, cap * sizeof *m);
			if (!m)
				return false;
			scan->members = m;
			scan->cap = cap;
		}
		scan->members[scan->depth] = 0;
	}
	++scan->depth;
	++scan->values;
	scan->state = JB_SCAN_GAP;
	return true;
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
		case JB_SCAN_STRING: {
			/* Skip to the next quote, or a backslash before it. */
			if (!quote || quote < data) {
				quote = memchr(data, '"', (size_t)(end - data));
				quote = quote ? quote : end;
			}
			char const *slash = memchr(data, '\\', (size_t)(quote - data));
			char const *stop = slash ? slash : quote;
			scan->run += (size_t)(stop - data);
			if (stop == end)
				return len;
			data = stop;
			scan->state = slash ? JB_SCAN_ESCAPE : JB_SCAN_GAP;
			if (!slash)
				scan->copied += scan->run;
			continue;
		}
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
			    !scan_open(scan, members))
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
jb_scan_end (struct jb_scan *scan)
{
	free(scan->members);
	*scan = (struct jb_scan){};
}

void
jb_reap (struct jb_reaper   *reaper,
         struct json_object *val)
{
	if (!json_object_is_type(val, json_type_array) &&
	    !json_object_is_type(val, json_type_object)) {
		json_object_put(val);
		return;
	}
	if (reaper->n == reaper->cap) {
		size_t cap = reaper->cap ? 2 * reaper->cap : 16;
		struct json_object **stack = realloc(reaper->stack, cap * sizeof *stack);
		if (!stack) {
			json_object_put(val);
			return;
		}
		reaper->stack = stack;
		reaper->cap = cap;
	}
	reaper->stack[reaper->n++] = val;
}

/*
 * Take containers apart from the innermost: each value taken out of one is
 * freed, or taken apart in turn, and a container is freed once empty.  Taking
 * the last element of an array, or the first member of an object, costs O(1).
 */
size_t
jb_reap_some (struct jb_reaper *reaper,
              size_t            values)
{
	size_t freed = 0;
	while (freed < values && reaper->n) {
		struct json_object *top = reaper->stack[reaper->n - 1];
		struct json_object *val = nullptr;
		bool took = false;
		if (json_object_is_type(top, json_type_array)) {
			size_t len = json_object_array_length(top);
			if ((took = len)) {
				val = json_object_get(json_object_array_get_idx(top, len - 1));
				json_object_array_del_idx(top, len - 1, 1);
			}
		} else {
			struct lh_entry *e = lh_table_head(json_object_get_object(top));
			if ((took = e)) {
				val = json_object_get(lh_entry_v(e));
				json_object_object_del(top, lh_entry_k(e));
			}
		}
		if (!took) {
			--reaper->n;
			json_object_put(top);
			++freed;
		} else if (json_object_is_type(val, json_type_array) ||
		           json_object_is_type(val, json_type_object)) {
			jb_reap(reaper, val);
		} else {
			json_object_put(val);
			++freed;
		}
	}
	return freed;
}

void
jb_reaper_fini (struct jb_reaper *reaper)
{
	while (reaper->n)
		json_object_put(reaper->stack[--reaper->n]);
	free(reaper->stack);
	*reaper = (struct jb_reaper){};
}

void
jb_pool_fini (struct jb_pool *pool)
{
	while (pool->n)
		json_tokener_free(pool->free[--pool->n]);
}

/* A tokener whose stack holds nothing, from @p pool or new; nullptr if out of
 * memory. */
static struct json_tokener *
pool_take (struct jb_pool *pool)
{
	if (pool->n)
		return pool->free[--pool->n];
	/* json-c counts a value inside the innermost container as a level too. */
	struct json_tokener *tok = pool->depth && pool->depth < INT_MAX
	                           ? json_tokener_new_ex((int)pool->depth + 1)
	                           : json_tokener_new();
	if (tok)
		json_tokener_set_flags(tok, pool->flags);
	return tok;
}

bool
jb_parse_some (struct jb_parser *parser,
               struct jb_pool   *pool,
               char const       *data,
               size_t            len,
               size_t            bytes,
               uint32_t          values)
{
	if (parser->done)
		return true;
	if (!parser->tok && !(parser->tok = pool_take(pool))) {
		parser->done = parser->failed = true;
		return true;
	}

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
	    parser->cut.run == scalar) {
		parser->done = parser->failed = true;
		return true;
	}
	for (int i = 0; i < 3 && end > off + 1 && end < len &&
	                ((unsigned char)data[end] & 0xc0) == 0x80; ++i)
		--end;

	struct json_tokener *tok = parser->tok;
	bool ok = true;
	if (!parser->root) {
		parser->root = json_tokener_parse_ex(tok, data + off, (int)(end - off));
		if (parser->root)
			parser->off += json_tokener_get_parse_end(tok);
		else if (json_tokener_get_error(tok) != json_tokener_continue)
			ok = false;
		/* A number or literal at the very end only ends at a terminator. */
		else if ((parser->off = end) == len)
			ok = (parser->root = json_tokener_parse_ex(tok, "", 1)) != nullptr;
	}

	/* What follows the value must be white space, however it is sliced. */
	while (ok && parser->root && parser->off < end &&
	       (data[parser->off] == ' ' || data[parser->off] == '\t' ||
	        data[parser->off] == '\r' || data[parser->off] == '\n'))
		++parser->off;
	parser->failed = !ok || parser->off != end;
	parser->done = parser->failed || end == len;
	return parser->done;
}

/*
 * json-c keeps a parse's unfinished containers on its stack, each holding the
 * values finished inside it, and json_tokener_reset() frees them all at once:
 * take them first.  This reads fields json-c documents as its own; they have
 * been stable since 0.12, and with them gone this would not compile.
 */
void
jb_parse_end (struct jb_parser *parser,
              struct jb_pool   *pool,
              struct jb_reaper *reaper)
{
	struct json_tokener *tok = parser->tok;
	if (reaper) {
		for (int i = 0; tok && i <= tok->depth; ++i)
			if (tok->stack[i].current)
				jb_reap(reaper, json_object_get(tok->stack[i].current));
		if (parser->root)
			jb_reap(reaper, parser->root);
	} else {
		json_object_put(parser->root);
	}
	if (tok) {
		json_tokener_reset(tok);
		if (pool->n < ARRAY_SIZE(pool->free) && parser->off <= pool->keep)
			pool->free[pool->n++] = tok;
		else
			json_tokener_free(tok);
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
jb_escape (char       *dst,
           char const *src,
           size_t      len)
{
	static char const hex[] = "0123456789abcdef";
	char *out = dst;
	for (char const *end = src + len; src < end; ++src) {
		unsigned char c = (unsigned char)*src;
		if (c >= ' ' && c != '"' && c != '\\') {
			*out++ = (char)c;
			continue;
		}
		*out++ = '\\';
		switch (c) {
		case '\b': *out++ = 'b'; break;
		case '\t': *out++ = 't'; break;
		case '\n': *out++ = 'n'; break;
		case '\f': *out++ = 'f'; break;
		case '\r': *out++ = 'r'; break;
		case '"':
		case '\\': *out++ = (char)c; break;
		default:
			*out++ = 'u';
			*out++ = '0';
			*out++ = '0';
			*out++ = hex[c >> 4];
			*out++ = hex[c & 15];
		}
	}
	return (size_t)(out - dst);
}
