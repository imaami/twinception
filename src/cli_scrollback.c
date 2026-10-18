#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cli_scrollback.h"
#include "compat.h"

enum {
	CLI_SCROLLBACK_BYTES      = 16 * 1024 * 1024,
	CLI_SCROLLBACK_LINES      = 10000,
	CLI_SCROLLBACK_CHECKPOINT = 64,
	CLI_SCROLLBACK_STYLES     = 65536,
};

struct cli_scrollback_style {
	uint32_t       offset;
	enum cli_style id;
};

static_assert(CLI_SCROLLBACK_BYTES <= UINT32_MAX);
static_assert(sizeof(struct cli_scrollback_style) == 8);

struct cli_scrollback_line {
	struct list                 hook;
	struct dstr                 text;
	struct cli_scrollback_style *styles;
	size_t                     *checkpoints;
	size_t                      capacity;
	size_t                      scanned;
	size_t                      column;
	size_t                      columns;
	size_t                      rows;
	size_t                      style_count;
	size_t                      style_capacity;
};

void
cli_scrollback_init (struct cli_scrollback *scrollback)
{
	*scrollback = (struct cli_scrollback){};
	list_init(&scrollback->lines);
}

static void
cli_scrollback_line_destroy (struct cli_scrollback_line **ptr)
{
	struct cli_scrollback_line *line = *ptr;
	list_del(&line->hook);
	dstr_cleanup(&line->text);
	free(line->checkpoints);
	line->checkpoints = nullptr;
	free(line->styles);
	line->styles = nullptr;
	free(line);
	*ptr = nullptr;
}

void
cli_scrollback_fini (struct cli_scrollback *scrollback)
{
	while (!list_is_empty(&scrollback->lines)) {
		struct cli_scrollback_line *line = container_of(scrollback->lines.next,
		                                                  struct cli_scrollback_line, hook);
		cli_scrollback_line_destroy(&line);
	}
	cli_scrollback_init(scrollback);
}

static int
cli_scrollback_add_line (struct cli_scrollback *scrollback)
{
	struct cli_scrollback_line *line = malloc(sizeof *line);
	if (!line)
		return ENOMEM;
	*line = (struct cli_scrollback_line){.text = dstr(nullptr)};
	list_init(&line->hook);
	list_append(&scrollback->lines, &line->hook);
	++scrollback->count;
	return 0;
}

static void
cli_scrollback_trim (struct cli_scrollback *scrollback)
{
	while (scrollback->count > 1 &&
	       (scrollback->count > CLI_SCROLLBACK_LINES || scrollback->bytes > CLI_SCROLLBACK_BYTES ||
	        scrollback->styles > CLI_SCROLLBACK_STYLES)) {
		struct cli_scrollback_line *line = container_of(scrollback->lines.next,
		                                                  struct cli_scrollback_line, hook);
		if (line->columns == scrollback->columns)
			scrollback->discarded += line->rows;
		scrollback->bytes -= dstr_len(&line->text);
		scrollback->styles -= line->style_count;
		--scrollback->count;
		cli_scrollback_line_destroy(&line);
	}
}

static int cli_scrollback_prefix (struct cli_scrollback_line *line,
                                  size_t                     *bytes,
                                  size_t                     *rows);

/** Find the first style change strictly after a byte offset. */
static size_t
cli_scrollback_style_after (struct cli_scrollback_line const *line,
                            size_t                           offset)
{
	size_t low = 0;
	size_t high = line->style_count;
	while (low < high) {
		size_t middle = low + (high - low) / 2;
		if (line->styles[middle].offset <= offset)
			low = middle + 1;
		else
			high = middle;
	}
	return low;
}

static int
cli_scrollback_line_trim (struct cli_scrollback      *scrollback,
                          struct cli_scrollback_line *line,
                          size_t                      skip)
{
	size_t old = dstr_len(&line->text);
	char const *data = dstr_get(&line->text);
	while (skip < old && ((unsigned char)data[skip] & 0xc0) == 0x80)
		++skip;
	size_t discarded = 0;
	if (line->columns && line->columns == scrollback->columns) {
		int e = cli_scrollback_prefix(line, &skip, &discarded);
		if (e)
			return e;
	}
	dstr_auto tail = dstr(nullptr);
	if (!dstr_cat_n(&tail, data + skip, old - skip))
		return ENOMEM;
	dstr_cleanup(&line->text);
	line->text = dstr_move(&tail);
	scrollback->bytes -= skip;
	scrollback->discarded += discarded;
	line->columns = 0;
	size_t first = cli_scrollback_style_after(line, skip);
	if (first && skip < old)
		--first;
	scrollback->styles -= first;
	line->style_count -= first;
	for (size_t i = 0; i < line->style_count; ++i) {
		struct cli_scrollback_style style = line->styles[first + i];
		style.offset = style.offset > skip ? style.offset - (uint32_t)skip : 0;
		line->styles[i] = style;
	}
	return 0;
}

int
cli_scrollback_append (struct cli_scrollback *scrollback,
                       struct cstr_view       text)
{
	return cli_scrollback_append_style(scrollback, text, CLI_STYLE_RESET);
}

int
cli_scrollback_append_style (struct cli_scrollback *scrollback,
                             struct cstr_view       text,
                             enum cli_style         style)
{
	if (style >= CLI_STYLE_COUNT)
		return EINVAL;
	while (text.len) {
		if (list_is_empty(&scrollback->lines)) {
			int e = cli_scrollback_add_line(scrollback);
			if (e)
				return e;
		}
		struct cli_scrollback_line *line = container_of(scrollback->lines.prev,
		                                                  struct cli_scrollback_line, hook);
		char const *newline = memchr(text.data, '\n', text.len);
		size_t length = newline ? (size_t)(newline - text.data) : text.len;
		/* A single unterminated line must not bypass the transcript bound.
		 * Drop its oldest prefix, retaining complete UTF-8 code points. */
		if (length > CLI_SCROLLBACK_BYTES) {
			size_t skip = length - CLI_SCROLLBACK_BYTES;
			while (skip < length && ((unsigned char)text.data[skip] & 0xc0) == 0x80)
				++skip;
			text = cstr_view_slice(text, skip, SIZE_MAX);
			length -= skip;
		}
		if (length && line->style_count == CLI_SCROLLBACK_STYLES &&
		    style != line->styles[line->style_count - 1].id) {
			int e = cli_scrollback_line_trim(scrollback, line,
			                                 line->styles[line->style_count / 2].offset);
			if (e)
				return e;
		}
		size_t old = dstr_len(&line->text);
		if (old + length > CLI_SCROLLBACK_BYTES) {
			int e = cli_scrollback_line_trim(scrollback, line, old + length - CLI_SCROLLBACK_BYTES);
			if (e)
				return e;
			old = dstr_len(&line->text);
		}
		enum cli_style previous = line->style_count ? line->styles[line->style_count - 1].id : CLI_STYLE_RESET;
		bool change = length && style != previous;
		if (change && line->style_count == line->style_capacity) {
			size_t capacity = line->style_capacity ? line->style_capacity * 2 : 4;
			struct cli_scrollback_style *styles = realloc(line->styles, capacity * sizeof *styles);
			if (!styles)
				return ENOMEM;
			line->styles = styles;
			line->style_capacity = capacity;
		}
		if (!dstr_cat_n(&line->text, text.data, length))
			return ENOMEM;
		if (change) {
			line->styles[line->style_count++] = (struct cli_scrollback_style){(uint32_t)old, style};
			++scrollback->styles;
		}
		scrollback->bytes += length;
		if (scrollback->offset && !scrollback->unread)
			scrollback->unread = 1;
		if (newline) {
			int e = cli_scrollback_add_line(scrollback);
			if (e)
				return e;
			if (scrollback->offset)
				++scrollback->unread;
		}
		text = cstr_view_slice(text, length + (newline != nullptr), SIZE_MAX);
		cli_scrollback_trim(scrollback);
	}
	return 0;
}

static size_t
cli_scrollback_character (struct cstr_view text,
                          size_t           column,
                          size_t          *width)
{
	if (text.data[0] == '\t') {
		*width = 8 - column % 8;
		return 1;
	}
	mbstate_t state = {};
	wchar_t character;
	size_t bytes = mbrtowc(&character, text.data, text.len, &state);
	if (!bytes || bytes == (size_t)-1 || bytes == (size_t)-2) {
		*width = 1;
		return 1;
	}
	int cells = wcwidth(character);
	*width = cells < 0 ? 1 : (size_t)cells;
	return bytes;
}

/** Extend cached wrapping only over newly appended bytes. Sparse byte
 * checkpoints let rendering seek near its first visible row. */
static int
cli_scrollback_line_rows (struct cli_scrollback_line *line,
                          size_t                      columns)
{
	if (line->columns != columns) {
		line->columns = columns;
		line->rows = 1;
		line->scanned = 0;
		line->column = 0;
	}
	struct cstr_view text = cstr_view_slice(cstr_view_dstr(&line->text), line->scanned, SIZE_MAX);
	while (text.len) {
		size_t width;
		size_t length = cli_scrollback_character(text, line->column, &width);
		if (width > columns)
			width = columns;
		if (line->column && line->column + width > columns) {
			if (!(line->rows % CLI_SCROLLBACK_CHECKPOINT)) {
				size_t checkpoint = line->rows / CLI_SCROLLBACK_CHECKPOINT - 1;
				if (checkpoint == line->capacity) {
					size_t capacity = line->capacity ? line->capacity * 2 : 16;
					size_t *checkpoints = realloc(line->checkpoints, capacity * sizeof *checkpoints);
					if (!checkpoints)
						return ENOMEM;
					line->checkpoints = checkpoints;
					line->capacity = capacity;
				}
				line->checkpoints[checkpoint] = line->scanned;
			}
			++line->rows;
			line->column = 0;
			if (text.data[0] == '\t')
				width = columns < 8 ? columns : 8;
		}
		line->column += width;
		line->scanned += length;
		text = cstr_view_slice(text, length, SIZE_MAX);
	}
	return 0;
}

/** Trim to a wrapped-row boundary so retained rows keep their alignment. */
static int
cli_scrollback_prefix (struct cli_scrollback_line *line,
                       size_t                     *bytes,
                       size_t                     *rows)
{
	int e = cli_scrollback_line_rows(line, line->columns);
	if (e)
		return e;
	size_t low = 0;
	size_t high = (line->rows - 1) / CLI_SCROLLBACK_CHECKPOINT;
	while (low < high) {
		size_t middle = low + (high - low) / 2;
		if (line->checkpoints[middle] <= *bytes)
			low = middle + 1;
		else
			high = middle;
	}
	size_t offset = low ? line->checkpoints[low - 1] : 0;
	size_t row = low * CLI_SCROLLBACK_CHECKPOINT;
	size_t column = 0;
	struct cstr_view text = cstr_view_slice(cstr_view_dstr(&line->text), offset, SIZE_MAX);
	while (text.len) {
		if (!column && offset >= *bytes) {
			*bytes = offset;
			*rows = row;
			return 0;
		}
		size_t width;
		size_t length = cli_scrollback_character(text, column, &width);
		if (width > line->columns)
			width = line->columns;
		if (column && column + width > line->columns) {
			++row;
			column = 0;
			continue;
		}
		column += width;
		offset += length;
		text = cstr_view_slice(text, length, SIZE_MAX);
	}
	*bytes = offset;
	*rows = line->rows;
	return 0;
}

void
cli_scrollback_scroll (struct cli_scrollback *scrollback,
                       size_t                 rows,
                       bool                   backward)
{
	if (backward)
		scrollback->offset += rows < scrollback->rows - scrollback->offset
		                      ? rows : scrollback->rows - scrollback->offset;
	else if (rows < scrollback->offset)
		scrollback->offset -= rows;
	else
		cli_scrollback_bottom(scrollback);
}

void
cli_scrollback_bottom (struct cli_scrollback *scrollback)
{
	scrollback->offset = 0;
	scrollback->unread = 0;
}

static int
cli_scrollback_style_write (enum cli_style *current,
                            enum cli_style  style,
                            struct dstr    *output)
{
	if (*current == style)
		return 0;
	struct cstr_view sequence = cli_style_sequence(style);
	if (!dstr_cat_n(output, sequence.data, sequence.len))
		return ENOMEM;
	*current = style;
	return 0;
}

/** Copy contiguous text runs, emitting SGR only at visible style changes. */
static int
cli_scrollback_line_write (struct cli_scrollback_line const *line,
                           size_t                           start,
                           size_t                           end,
                           enum cli_style                  *style,
                           struct dstr                     *output)
{
	size_t next = cli_scrollback_style_after(line, start);
	enum cli_style id = next ? line->styles[next - 1].id : CLI_STYLE_RESET;
	while (start < end) {
		size_t stop = next < line->style_count && line->styles[next].offset < end
		              ? line->styles[next].offset : end;
		int e = cli_scrollback_style_write(style, id, output);
		if (e)
			return e;
		if (!dstr_cat_n(output, dstr_get(&line->text) + start, stop - start))
			return ENOMEM;
		start = stop;
		if (next < line->style_count)
			id = line->styles[next++].id;
	}
	return 0;
}

static int
cli_scrollback_line_render (struct cli_scrollback_line *line,
                            size_t                      columns,
                            size_t                     *skip,
                            size_t                     *height,
                            enum cli_style              *style,
                            struct dstr                *output)
{
	size_t checkpoint = *skip / CLI_SCROLLBACK_CHECKPOINT;
	size_t start = checkpoint ? line->checkpoints[checkpoint - 1] : 0;
	*skip -= checkpoint * CLI_SCROLLBACK_CHECKPOINT;
	struct cstr_view text = cstr_view_slice(cstr_view_dstr(&line->text), start, SIZE_MAX);
	size_t column = 0;
	size_t offset = start;
	while (text.len && *height) {
		size_t width;
		size_t length = cli_scrollback_character(text, column, &width);
		if (width > columns)
			width = columns;
		if (column && column + width > columns) {
			if (*skip)
				--*skip;
			else {
				int e = cli_scrollback_line_write(line, start, offset, style, output);
				if (e)
					return e;
				--*height;
				/* With autowrap disabled a full row leaves the cursor on its
				 * last cell. EL0 there would erase the final character. */
				if (!dstr_cat(output, column < columns ? "\033[K\r\n" : "\r\n"))
					return ENOMEM;
			}
			start = offset;
			column = 0;
			if (text.data[0] == '\t')
				width = columns < 8 ? columns : 8;
			if (!*height)
				break;
		}
		if (!*skip) {
			if (text.data[0] == '\t') {
				int e = cli_scrollback_line_write(line, start, offset, style, output);
				size_t next = cli_scrollback_style_after(line, offset);
				if (!e)
					e = cli_scrollback_style_write(style, next ? line->styles[next - 1].id : CLI_STYLE_RESET, output);
				if (e)
					return e;
				if (!dstr_cat_n(output, "        ", width))
					return ENOMEM;
				start = offset + length;
			}
		} else
			start = offset + length;
		column += width;
		offset += length;
		text = cstr_view_slice(text, length, SIZE_MAX);
	}
	if (*height) {
		if (*skip)
			--*skip;
		else {
			int e = cli_scrollback_line_write(line, start, offset, style, output);
			if (e)
				return e;
			--*height;
			if (!dstr_cat(output, column < columns ? "\033[K\r\n" : "\r\n"))
				return ENOMEM;
		}
	}
	return 0;
}

int
cli_scrollback_render (struct cli_scrollback *scrollback,
                       size_t                 columns,
                       size_t                 height,
                       struct dstr           *output)
{
	if (!columns || !height)
		return EINVAL;
	size_t rows = 0;
	struct list *hook;
	list_foreach(hook, &scrollback->lines) {
		struct cli_scrollback_line *line = container_of(hook, struct cli_scrollback_line, hook);
		int e = cli_scrollback_line_rows(line, columns);
		if (e)
			return e;
		rows += line->rows;
	}
	if (scrollback->offset && scrollback->columns == columns &&
	    rows + scrollback->discarded > scrollback->rows)
		scrollback->offset += rows + scrollback->discarded - scrollback->rows;
	scrollback->discarded = 0;
	scrollback->rows = rows;
	scrollback->columns = columns;
	size_t maximum = rows > height ? rows - height : 0;
	if (scrollback->offset > maximum)
		scrollback->offset = maximum;
	if (!scrollback->offset)
		scrollback->unread = 0;
	size_t skip = maximum - scrollback->offset;
	enum cli_style style = CLI_STYLE_RESET;
	list_foreach(hook, &scrollback->lines) {
		struct cli_scrollback_line *line = container_of(hook, struct cli_scrollback_line, hook);
		if (skip >= line->rows) {
			skip -= line->rows;
			continue;
		}
		int e = cli_scrollback_line_render(line, columns, &skip, &height, &style, output);
		if (e)
			return e;
		if (!height)
			break;
	}
	while (height--)
		if (!dstr_cat(output, "\033[K\r\n"))
			return ENOMEM;
	return cli_scrollback_style_write(&style, CLI_STYLE_RESET, output);
}
