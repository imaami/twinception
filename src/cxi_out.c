/** @file cxi_out.c
 *
 * The texts of the code tool (src/cxi_out.h).
 */
#define _GNU_SOURCE /* memrchr() */
#include <string.h>

#include "cxi_out.h"
#include "jb.h"
#include "tool.h"
#include "util.h"

void
cxi_out_fini (struct cxi_out *out)
{
	dstr_cleanup(&out->norm);
	dstr_cleanup(&out->text);
}

void
cxi_out_put (struct cxi_out *out,
             char const     *p,
             size_t          n)
{
	if (!dstr_cat_n(&out->text, p, n))
		out->e = ENOMEM;
}

void
cxi_out_num (struct cxi_out *out,
             uint64_t        n)
{
	char digits[UTIL_DECIMAL];
	cxi_out_view(out, util_decimal(digits + sizeof digits, n, 0));
}

void
cxi_out_hex (struct cxi_out *out,
             unsigned char   c)
{
	char text[4] = { '\\', 'x' };
	util_hex(text + 2, &c, 1);
	cxi_out_put(out, text, sizeof text);
}

size_t
cxi_out_char (char const *p,
              size_t      n)
{
	unsigned char const c = (unsigned char)*p;
	size_t const k = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
	if (c < 0x20 || c == 0x7f || k > n || (c == 0xc2 && (unsigned char)p[1] < 0xa0))
		return 0;
	return k == 1 || jb_utf8(p, k) == k ? k : 0;
}

void
cxi_out_text (struct cxi_out *out,
              char const     *s)
{
	size_t const n = strlen(s);
	/* The bytes from from to i are appended as they are, in one piece. */
	size_t from = 0;
	for (size_t i = 0; i < n; ) {
		size_t const col = s[i] == ':' && i && s[i - 1] >= '0' && s[i - 1] <= '9'
		                   ? strspn(s + i + 1, "0123456789") : 0;
		struct cstr_view const rest = cstr_view_n(s + i, n - i);
		/* The test of the first byte spares most calls of memcmp(). */
		size_t const skip = out->tree.len && s[i] == *out->tree.data &&
		                    cstr_view_starts(rest, out->tree) ? out->tree.len
		                  : col && s[i + 1 + col] == ')' ? 1 + col : 0;
		size_t k;
		if (skip) {
			cxi_out_put(out, s + from, i - from);
			i += skip;
			from = i;
		} else if ((k = cxi_out_char(s + i, n - i))) {
			i += k;
		} else {
			cxi_out_put(out, s + from, i - from);
			unsigned char const c = (unsigned char)s[i++];
			if (c == '\n')
				cxi_out_lit(out, "\\n");
			else if (c == '\t')
				cxi_out_lit(out, "\\t");
			else
				cxi_out_hex(out, c);
			from = i;
		}
	}
	cxi_out_put(out, s + from, n - from);
}

void
cxi_out_path (struct cxi_out   *out,
              struct cstr_view  p)
{
	size_t i = 0;
	for (size_t k; i < p.len
	            && (k = cxi_out_char(p.data + i, p.len - i))
	            && !memchr("\"\\", p.data[i], 2); i += k)
		;

	bool const quote = i < p.len;
	if (quote)
		cxi_out_put(out, "\"", 1);
	cxi_out_put(out, p.data, i);

	while (i < p.len) {
		unsigned char const c = (unsigned char)p.data[i];
		/* A backslash and the byte, or three octal digits. */
		char esc[4] = {
			'\\',
			(char)c,
			(char)('0' + (c >> 3 & 7)),
			(char)('0' + (c & 7))
		};
		size_t k = 1;
		if (c == '"' || c == '\\') {
			cxi_out_put(out, esc, 2);
		} else if (c >= '\a' && c <= '\r') {
			esc[1] = "abtnvfr"[c - '\a'];
			cxi_out_put(out, esc, 2);
		} else if ((k = cxi_out_char(p.data + i, p.len - i))) {
			cxi_out_put(out, p.data + i, k);
		} else {
			esc[1] = (char)('0' + (c >> 6));
			cxi_out_put(out, esc, sizeof esc);
		}
		i += k ? k : 1;
	}

	if (quote)
		cxi_out_put(out, "\"", 1);
}

void
cxi_out_range (struct cxi_out *out,
               uint32_t        first,
               uint32_t        last)
{
	cxi_out_num(out, first);
	if (last != first) {
		cxi_out_lit(out, "-");
		cxi_out_num(out, last);
	}
}

struct cstr_view
cxi_out_answer (struct cxi_out *out)
{
	static char const cut[] = "\n[The answer is cut at 4 MiB.]";
	static_assert(TOOL_TEXT_MAX == 4 << 20, "update the text of cut");
	char *text = (char *)dstr_raw(&out->text);
	size_t len = dstr_len(&out->text);
	if (len > TOOL_TEXT_MAX) {
		/* The note is shorter than the text that it replaces. */
		char const *end = memrchr(text, '\n', TOOL_TEXT_MAX - (sizeof cut - 1));
		len = end ? (size_t)(end - text) : 0;
		memcpy(text + len, cut, sizeof cut - 1);
		len += sizeof cut - 1;
	}
	return cstr_view_n(text, len);
}
