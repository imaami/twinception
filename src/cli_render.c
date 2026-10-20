#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <utf8proc.h>

#include "cli_render.h"
#include "dstr.h"

size_t
cli_render_character (struct cstr_view text, int *width)
{
	unsigned char first = text.data[0];
	if (first < 0x80) {
		*width = first >= ' ' && first != 0x7f ? 1 : -1;
		return 1;
	}
	int32_t character;
	ptrdiff_t length = utf8proc_iterate((uint8_t const *)text.data,
	                                    text.len < 4 ? (ptrdiff_t)text.len : 4, &character);
	if (length < 0) {
		*width = -1;
		return 1;
	}
	*width = character < 0xa0 ? -1 : utf8proc_charwidth(character);
	return (size_t)length;
}

/** A negative width identifies a complete SGR sequence. */
static size_t
cli_render_token (struct cstr_view text, int *width)
{
	if (text.data[0] == '\033') {
		char const *end = memchr(text.data, 'm', text.len);
		*width = -1;
		return end ? (size_t)(end - text.data) + 1 : 0;
	}
	size_t length = cli_render_character(text, width);
	return *width < 0 ? 0 : length;
}

int
cli_render_row (struct dstr *cache, struct dstr *output,
                size_t begin, size_t row, size_t column)
{
	if (!row || !column || cache == output || begin > dstr_len(output) ||
	    dstr_len(output) - begin > INT_MAX)
		return EINVAL;
	struct cstr_view text = cstr_view_slice(cstr_view_dstr(output), begin, SIZE_MAX);
	struct cstr_view old = cache ? cstr_view_dstr(cache) : (struct cstr_view){};
	if (cache && cstr_view_equal(text, old)) {
		dstr_erase(output, begin, SIZE_MAX);
		return 0;
	}
	size_t start = 0, cells = 0, base = 0, base_cells = 0;
	struct cstr_view style = {}, base_style = {};
	while (start < text.len && start < old.len) {
		int width;
		size_t length = cli_render_token(cstr_view_slice(text, start, SIZE_MAX), &width);
		if (!length)
			return EILSEQ;
		if (length > old.len - start || text.data[start] != old.data[start] ||
		    (length > 1 && memcmp(text.data + start, old.data + start, length)))
			break;
		if (width < 0)
			style = (struct cstr_view){text.data + start, length};
		else if (width) {
			base = start;
			base_cells = cells;
			base_style = style;
			cells += (unsigned)width;
		}
		start += length;
	}
	/* A changed combining mark also changes its preceding base cell. */
	struct cstr_view tails[] = {cstr_view_slice(text, start, SIZE_MAX), cstr_view_slice(old, start, SIZE_MAX)};
	for (size_t i = 0; i < sizeof tails / sizeof *tails; ++i) {
		for (struct cstr_view tail = tails[i]; tail.len;) {
			int width;
			size_t length = cli_render_token(tail, &width);
			if (!length)
				return EILSEQ;
			if (!width) {
				start = base;
				cells = base_cells;
				style = base_style;
			}
			if (width >= 0)
				break;
			tail = cstr_view_slice(tail, length, SIZE_MAX);
		}
	}
	size_t end = text.len, old_end = old.len;
	while (end > start && old_end > start && text.data[end - 1] == ' ' && old.data[old_end - 1] == ' ') {
		--end;
		--old_end;
	}
	if (cells > SIZE_MAX - column)
		return EOVERFLOW;
	size_t style_start = style.len ? (size_t)(style.data - text.data) : 0;
	dstr_auto copy = dstr(nullptr);
	struct dstr *saved = cache ? cache : &copy;
	dstr_clear(saved);
	if (!dstr_cat_n(saved, text.data, text.len))
		return ENOMEM;
	text = cstr_view_dstr(saved);
	dstr_erase(output, begin, SIZE_MAX);
	if (end > start && !dstr_printf(output, "\033[%zu;%zuH%.*s%.*s", row, column + cells,
	                               text.data[start] == '\033' ? 0 : (int)style.len, text.data + style_start,
	                               (int)(end - start), text.data + start)) {
		dstr_clear(saved);
		return ENOMEM;
	}
	return 0;
}
