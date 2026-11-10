#include <string.h>

#include "u8.h"

/* Parse the next character of the @p n bytes at @p p, at least one, with
 * @p parser, reading no byte past them, which the parser could: it reads on
 * until a character is whole or a byte breaks it. Returns the bytes taken.
 * With parser->error set and all @p n bytes taken, they end inside the
 * character, which the parser goes on with when given the bytes after.
 */
static size_t
u8_next (struct utf8   *parser,
         uint8_t const *p,
         size_t         n)
{
	if (n >= 4)
		return (size_t)(utf8_parse_next_code_point(parser, p) - p);
	/* 0xff is no byte of UTF-8, so the parser stops at the first. */
	uint8_t tail[4] = { 0xff, 0xff, 0xff, 0xff };
	memcpy(tail, p, n);
	return (size_t)(utf8_parse_next_code_point(parser, tail) - tail);
}

int
u8_gate_pass (struct u8_gate   *gate,
              struct cstr_view  bytes)
{
	struct utf8 *const parser = &gate->parser;
	if (parser->error || !bytes.len)
		return parser->error;
	if (!bytes.data)
		return parser->error = EFAULT;
	uint8_t const *p = (uint8_t const *)bytes.data;
	uint8_t const *const end = p + bytes.len;
	size_t len = gate->len;
	size_t count = gate->count;
	while (p < end) {
		/* Between characters ASCII passes without the parser, which
		 * takes a NUL for text. Its state after a character, the
		 * initial one and the one after ASCII lead on alike. */
		if (utf8_expects_leading_byte(parser)) {
			uint8_t const *const ascii = p;
			while (p < end && *p - 1u < 0x7fu)
				++p;
			len += (size_t)(p - ascii);
			count += (size_t)(p - ascii);
			if (p == end)
				break;
			if (!*p) {
				parser->error = EILSEQ;
				break;
			}
		}
		size_t const n = (size_t)(end - p);
		size_t const used = u8_next(parser, p, n);
		p += used;
		if (parser->error) {
			if (used == n)
				parser->error = 0;
			break;
		}
		len += utf8_size(parser);
		++count;
	}
	gate->len = len;
	gate->count = count;
	return parser->error;
}

struct u8_text
u8_text (struct cstr_view  bytes,
         int              *e)
{
	struct u8_gate gate = u8_gate();
	*e = u8_gate_pass(&gate, bytes);
	if (!*e)
		*e = u8_gate_end(&gate);
	return (struct u8_text){
		.view = { bytes.data, gate.len },
		.count = gate.count
	};
}

size_t
u8_char (struct cstr_view  bytes,
         uint32_t         *value)
{
	if (!bytes.len || !bytes.data)
		return 0;
	uint8_t const *const s = (uint8_t const *)bytes.data;
	if (*s < 0x80) {
		if (!*s)
			return 0;
		*value = *s;
		return 1;
	}
	struct utf8 parser = utf8();
	size_t const n = u8_next(&parser, s, bytes.len);
	if (parser.error)
		return 0;
	uint8_t const *const b = (uint8_t const *)utf8_result(&parser);
	uint32_t c = b[0] & (0x7fu >> n);
	for (size_t i = 1; i < n; ++i)
		c = c << 6 | (b[i] & 0x3fu);
	*value = c;
	return n;
}

size_t
u8_mend (char   *data,
         size_t  len,
         bool    last)
{
	uint8_t *const s = (uint8_t *)data;
	struct utf8 parser = utf8();
	for (size_t i = 0; i < len;) {
		if (s[i] < 0x80) {
			++i;
			continue;
		}
		size_t const n = len - i;
		size_t const used = u8_next(&parser, s + i, n);
		if (!parser.error) {
			i += used;
			continue;
		}
		if (used == n && !last)
			return i;
		/* The bytes a character began with, which a byte broke, and
		 * the byte, if it began none, are no character's. */
		size_t const bad = used ? used : 1;
		memset(s + i, '?', bad);
		i += bad;
		utf8_reset(&parser);
	}
	return len;
}
