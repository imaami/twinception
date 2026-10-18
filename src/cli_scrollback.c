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
};

struct cli_scrollback_line {
	struct list hook;
	struct dstr text;
	size_t     *checkpoints;
	size_t      capacity;
	size_t      scanned;
	size_t      column;
	size_t      columns;
	size_t      rows;
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
	       (scrollback->count > CLI_SCROLLBACK_LINES || scrollback->bytes > CLI_SCROLLBACK_BYTES)) {
		struct cli_scrollback_line *line = container_of(scrollback->lines.next,
		                                                  struct cli_scrollback_line, hook);
		if (line->columns == scrollback->columns)
			scrollback->discarded += line->rows;
		scrollback->bytes -= dstr_len(&line->text);
		--scrollback->count;
		cli_scrollback_line_destroy(&line);
	}
}

static int cli_scrollback_prefix (struct cli_scrollback_line *line,
                                  size_t                     *bytes,
                                  size_t                     *rows);

int
cli_scrollback_append (struct cli_scrollback *scrollback,
                       struct cstr_view       text)
{
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
		size_t old = dstr_len(&line->text);
		if (old + length > CLI_SCROLLBACK_BYTES) {
			size_t skip = old + length - CLI_SCROLLBACK_BYTES;
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
		}
		if (!dstr_cat_n(&line->text, text.data, length))
			return ENOMEM;
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
cli_scrollback_line_render (struct cli_scrollback_line *line,
                            size_t                      columns,
                            size_t                     *skip,
                            size_t                     *height,
                            struct dstr                *output)
{
	size_t checkpoint = *skip / CLI_SCROLLBACK_CHECKPOINT;
	size_t start = checkpoint ? line->checkpoints[checkpoint - 1] : 0;
	*skip -= checkpoint * CLI_SCROLLBACK_CHECKPOINT;
	struct cstr_view text = cstr_view_slice(cstr_view_dstr(&line->text), start, SIZE_MAX);
	size_t column = 0;
	while (text.len && *height) {
		size_t width;
		size_t length = cli_scrollback_character(text, column, &width);
		if (width > columns)
			width = columns;
		if (column && column + width > columns) {
			if (*skip)
				--*skip;
			else {
				--*height;
				if (!dstr_cat(output, "\033[K\r\n"))
					return ENOMEM;
			}
			column = 0;
			if (text.data[0] == '\t')
				width = columns < 8 ? columns : 8;
			if (!*height)
				break;
		}
		if (!*skip) {
			if (text.data[0] == '\t') {
				for (size_t i = 0; i < width; ++i)
					if (!dstr_cat(output, " "))
						return ENOMEM;
			} else if (!dstr_cat_n(output, text.data, length)) {
				return ENOMEM;
			}
		}
		column += width;
		text = cstr_view_slice(text, length, SIZE_MAX);
	}
	if (*height) {
		if (*skip)
			--*skip;
		else {
			--*height;
			if (!dstr_cat(output, "\033[K\r\n"))
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
	list_foreach(hook, &scrollback->lines) {
		struct cli_scrollback_line *line = container_of(hook, struct cli_scrollback_line, hook);
		if (skip >= line->rows) {
			skip -= line->rows;
			continue;
		}
		int e = cli_scrollback_line_render(line, columns, &skip, &height, output);
		if (e)
			return e;
		if (!height)
			break;
	}
	while (height--)
		if (!dstr_cat(output, "\033[K\r\n"))
			return ENOMEM;
	return 0;
}
