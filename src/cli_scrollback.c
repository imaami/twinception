#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "cli_render.h"
#include "cli_scrollback.h"
#include "compat.h"
#include "util.h"

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
	struct list                  hook;
	struct dstr                  text;
	UTIL_ARRAY(struct cli_scrollback_style) styles;
	size_t                      *checkpoints;
	size_t                       capacity;
	size_t                       scanned;
	size_t                       column;
	size_t                       columns;
	size_t                       rows;
};

void
cli_scrollback_init (struct cli_scrollback *dest)
{
	*dest = cli_scrollback(dest);
}

static void
cli_scrollback_line_destroy (struct cli_scrollback_line **ptr)
{
	struct cli_scrollback_line *line = *ptr;
	list_del(&line->hook);
	dstr_cleanup(&line->text);
	free(line->checkpoints);
	line->checkpoints = nullptr;
	free(line->styles.items);
	line->styles.items = nullptr;
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
	*line = (struct cli_scrollback_line){.hook = list(&line->hook), .text = dstr(nullptr)};
	list_append(&scrollback->lines, &line->hook);
	tally_add(&scrollback->used, 0);
	return 0;
}

static void
cli_scrollback_trim (struct cli_scrollback *scrollback)
{
	while (scrollback->used.n > 1 &&
	       (scrollback->used.n > CLI_SCROLLBACK_LINES || scrollback->used.bytes > CLI_SCROLLBACK_BYTES ||
	        scrollback->styles > CLI_SCROLLBACK_STYLES)) {
		struct cli_scrollback_line *line = container_of(scrollback->lines.next,
		                                                  struct cli_scrollback_line, hook);
		if (line->columns == scrollback->columns)
			scrollback->discarded += line->rows;
		tally_sub(&scrollback->used, dstr_len(&line->text));
		scrollback->styles -= line->styles.n;
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
	size_t high = line->styles.n;
	while (low < high) {
		size_t middle = low + (high - low) / 2;
		if (line->styles.items[middle].offset <= offset)
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
	skip = util_char_start(dstr_get(&line->text), old, skip);
	size_t discarded = 0;
	if (line->columns && line->columns == scrollback->columns) {
		int e = cli_scrollback_prefix(line, &skip, &discarded);
		if (e)
			return e;
	}
	(void)dstr_erase(&line->text, 0, skip);
	scrollback->used.bytes -= skip;
	scrollback->discarded += discarded;
	line->columns = 0;
	size_t first = cli_scrollback_style_after(line, skip);
	if (first && skip < old)
		--first;
	scrollback->styles -= first;
	line->styles.n -= first;
	for (size_t i = 0; i < line->styles.n; ++i) {
		struct cli_scrollback_style style = line->styles.items[first + i];
		style.offset = style.offset > skip ? style.offset - (uint32_t)skip : 0;
		line->styles.items[i] = style;
	}
	return 0;
}

int
cli_scrollback_append (struct cli_scrollback *scrollback,
                       struct cstr_view       text)
{
	return cli_scrollback_append_style(scrollback, text, CLI_STYLE_RESET);
}

/* Append part, which contains no newline, to the last line in style.
 * Returns 0 or an error number.
 */
static int
cli_scrollback_append_part (struct cli_scrollback *scrollback,
                            struct cstr_view       part,
                            enum cli_style         style)
{
	if (list_is_empty(&scrollback->lines)) {
		int e = cli_scrollback_add_line(scrollback);
		if (e)
			return e;
	}
	struct cli_scrollback_line *line = container_of(scrollback->lines.prev,
	                                                  struct cli_scrollback_line, hook);
	/* A single unterminated line must not bypass the transcript bound.
	 * Drop its oldest prefix, retaining complete UTF-8 code points. */
	if (part.len > CLI_SCROLLBACK_BYTES) {
		size_t skip = util_char_start(part.data, part.len, part.len - CLI_SCROLLBACK_BYTES);
		part = cstr_view_slice(part, skip, SIZE_MAX);
	}
	if (part.len && line->styles.n == CLI_SCROLLBACK_STYLES &&
	    style != line->styles.items[line->styles.n - 1].id) {
		int e = cli_scrollback_line_trim(scrollback, line,
		                                 line->styles.items[line->styles.n / 2].offset);
		if (e)
			return e;
	}
	size_t old = dstr_len(&line->text);
	if (old + part.len > CLI_SCROLLBACK_BYTES) {
		int e = cli_scrollback_line_trim(scrollback, line, old + part.len - CLI_SCROLLBACK_BYTES);
		if (e)
			return e;
		old = dstr_len(&line->text);
	}
	enum cli_style previous = line->styles.n ? line->styles.items[line->styles.n - 1].id : CLI_STYLE_RESET;
	bool change = part.len && style != previous;
	if (change) {
		if (util_grow(line->styles.items, line->styles.cap, line->styles.n + 1))
			return ENOMEM;
		/* The new entry is counted only after the text is appended. */
		line->styles.items[line->styles.n] = (struct cli_scrollback_style){(uint32_t)old, style};
	}
	if (cstr_view_cat(part, &line->text))
		return ENOMEM;
	line->styles.n += change;
	scrollback->styles += change;
	scrollback->used.bytes += part.len;
	if (scrollback->offset && !scrollback->unread)
		scrollback->unread = 1;
	return 0;
}

int
cli_scrollback_append_style (struct cli_scrollback *scrollback,
                             struct cstr_view       text,
                             enum cli_style         style)
{
	if (style >= CLI_STYLE_COUNT)
		return EINVAL;
	struct cstr_view line = {};
	while (cstr_view_cut(&text, '\n', &line)) {
		int e = cli_scrollback_append_part(scrollback, line, style);
		if (!e)
			e = cli_scrollback_add_line(scrollback);
		if (e)
			return e;
		if (scrollback->offset)
			++scrollback->unread;
		cli_scrollback_trim(scrollback);
	}
	if (!text.len)
		return 0;
	int e = cli_scrollback_append_part(scrollback, text, style);
	if (!e)
		cli_scrollback_trim(scrollback);
	return e;
}

/* The flags of a struct cli_scrollback_cell. */
enum {
	CLI_SCROLLBACK_REPLACE = 1u << 0,  /* shown as a replacement cell */
	CLI_SCROLLBACK_WRAP    = 1u << 1,  /* starts the next row */
};

/* A character of a row: its bytes, its cells and CLI_SCROLLBACK flags. */
struct cli_scrollback_cell {
	size_t   bytes;
	size_t   width;
	uint64_t flags;
};

/** Measure the character at @p offset of @p text, at @p column of a row of
 * @p columns cells. A tab that starts the next row is measured at that
 * row's start.
 */
static struct cli_scrollback_cell
cli_scrollback_cell (struct cstr_view text,
                     size_t           offset,
                     size_t           column,
                     size_t           columns)
{
	text = cstr_view_slice(text, offset, SIZE_MAX);
	bool tab = text.data[0] == '\t';
	int cells = 8 - (int)(column % 8);
	size_t bytes = tab ? 1 : cli_render_character(text, &cells);
	/* A combining character without a preceding cell must not reach left
	 * into another pane. Render one replacement cell at a row's origin,
	 * as for a character wider than the row. */
	bool invalid = cells < 0 || (!cells && !column);
	size_t cell = invalid ? 1 : (size_t)cells;
	size_t width = cell < columns ? cell : columns;
	bool wrap = column && column + width > columns;
	return (struct cli_scrollback_cell){
		.bytes = bytes,
		.width = wrap && tab ? (columns < 8 ? columns : 8) : width,
		.flags = (!tab && (invalid || cell > columns) ? CLI_SCROLLBACK_REPLACE : 0) |
		         (wrap ? CLI_SCROLLBACK_WRAP : 0),
	};
}

/* Start the next row of line at line->scanned, and record the offset of
 * every CLI_SCROLLBACK_CHECKPOINT-th row. Returns 0 or ENOMEM.
 */
static int
cli_scrollback_row_start (struct cli_scrollback_line *line)
{
	if (!(line->rows % CLI_SCROLLBACK_CHECKPOINT)) {
		size_t checkpoint = line->rows / CLI_SCROLLBACK_CHECKPOINT - 1;
		if (util_grow(line->checkpoints, line->capacity, checkpoint + 1))
			return ENOMEM;
		line->checkpoints[checkpoint] = line->scanned;
	}
	++line->rows;
	line->column = 0;
	return 0;
}

/** Extend cached wrapping only over newly appended bytes. Sparse byte
 * checkpoints let rendering seek near its first visible row.
 */
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
	struct cstr_view text = cstr_view_dstr(&line->text);
	while (line->scanned < text.len) {
		struct cli_scrollback_cell cell = cli_scrollback_cell(text, line->scanned, line->column, columns);
		int e = cell.flags & CLI_SCROLLBACK_WRAP ? cli_scrollback_row_start(line) : 0;
		if (e)
			return e;
		line->column += cell.width;
		line->scanned += cell.bytes;
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
	struct cstr_view text = cstr_view_dstr(&line->text);
	while (offset < text.len) {
		struct cli_scrollback_cell cell = cli_scrollback_cell(text, offset, column, line->columns);
		if (cell.flags & CLI_SCROLLBACK_WRAP) {
			++row;
			column = 0;
		}
		if (!column && offset >= *bytes) {
			*bytes = offset;
			*rows = row;
			return 0;
		}
		column += cell.width;
		offset += cell.bytes;
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

struct cli_scrollback_frame {
	struct dstr                  *output;
	struct dstr                  *cache;
	struct cli_style_entry const *palette;
	size_t                        columns;
	size_t                        height;
	size_t                        left;
	size_t                        row;
	size_t                        start;
	uint64_t                      style;  /* enum cli_style, widened over the padding */
};

static force_inline struct cli_scrollback_frame
cli_scrollback_frame (struct cli_scrollback_frame const f)
{
	return f;
}

#define cli_scrollback_frame(...) (cli_scrollback_frame)((struct cli_scrollback_frame){__VA_ARGS__})

static int
cli_scrollback_frame_style (struct cli_scrollback_frame *frame,
                            enum cli_style               style)
{
	if (style == CLI_STYLE_RESET)
		style = CLI_STYLE_TEXT;
	if (frame->style == style)
		return 0;
	struct cli_style_entry const *sequence = &frame->palette[style];
	if (!dstr_cat_n(frame->output, sequence->bytes, sequence->length))
		return ENOMEM;
	frame->style = style;
	return 0;
}

static int
cli_scrollback_frame_end (struct cli_scrollback_frame *frame,
                          size_t                       column)
{
	if (column < frame->columns) {
		int e = cli_scrollback_frame_style(frame, CLI_STYLE_RESET);
		if (e)
			return e;
		if (!dstr_printf(frame->output, "%*s", (int)(frame->columns - column), ""))
			return ENOMEM;
	}
	int e = cli_render_row(frame->cache, frame->output, frame->start, frame->row, frame->left);
	if (e)
		return e;
	if (frame->cache)
		++frame->cache;
	if (--frame->height)
		++frame->row;
	frame->start = dstr_len(frame->output);
	frame->style = CLI_STYLE_COUNT;
	return 0;
}

/** Copy contiguous text runs, emitting SGR only at visible style changes. */
static int
cli_scrollback_line_write (struct cli_scrollback_line const *line,
                           size_t                           start,
                           size_t                           end,
                           struct cli_scrollback_frame     *frame)
{
	size_t next = cli_scrollback_style_after(line, start);
	enum cli_style id = next ? line->styles.items[next - 1].id : CLI_STYLE_RESET;
	while (start < end) {
		size_t stop = next < line->styles.n && line->styles.items[next].offset < end
		              ? line->styles.items[next].offset : end;
		int e = cli_scrollback_frame_style(frame, id);
		if (e)
			return e;
		if (!dstr_cat_n(frame->output, dstr_get(&line->text) + start, stop - start))
			return ENOMEM;
		start = stop;
		if (next < line->styles.n)
			id = line->styles.items[next++].id;
	}
	return 0;
}

/* Write the row of line from *start to end, which ends at column *column,
 * or, while *skip is not 0, decrement *skip instead. Then set *start to
 * end and *column to 0 for the next row.
 */
static int
cli_scrollback_row (struct cli_scrollback_line const *line,
                    size_t                           *skip,
                    size_t                           *start,
                    size_t                            end,
                    size_t                           *column,
                    struct cli_scrollback_frame      *frame)
{
	size_t from = *start;
	size_t to = *column;
	*start = end;
	*column = 0;
	if (*skip) {
		--*skip;
		return 0;
	}
	int e = cli_scrollback_line_write(line, from, end, frame);
	return e ? e : cli_scrollback_frame_end(frame, to);
}

/* Write the text of line from *start to offset, and then the cell at
 * offset as the spaces of a tab or as "?". Set *start to the offset after
 * the cell.
 */
static int
cli_scrollback_cell_write (struct cli_scrollback_line const *line,
                           size_t                           *start,
                           size_t                            offset,
                           struct cli_scrollback_cell        cell,
                           struct cli_scrollback_frame      *frame)
{
	int e = cli_scrollback_line_write(line, *start, offset, frame);
	*start = offset + cell.bytes;
	size_t next = cli_scrollback_style_after(line, offset);
	if (!e)
		e = cli_scrollback_frame_style(frame, next ? line->styles.items[next - 1].id : CLI_STYLE_RESET);
	bool replace = cell.flags & CLI_SCROLLBACK_REPLACE;
	if (!e && !dstr_cat_n(frame->output, replace ? "?" : "        ", replace ? 1 : cell.width))
		e = ENOMEM;
	return e;
}

static int
cli_scrollback_line_render (struct cli_scrollback_line  *line,
                            size_t                      *skip,
                            struct cli_scrollback_frame *frame)
{
	size_t checkpoint = *skip / CLI_SCROLLBACK_CHECKPOINT;
	size_t start = checkpoint ? line->checkpoints[checkpoint - 1] : 0;
	*skip -= checkpoint * CLI_SCROLLBACK_CHECKPOINT;
	struct cstr_view text = cstr_view_dstr(&line->text);
	size_t column = 0;
	size_t offset = start;
	/* While rows are skipped, a row starts only where it wraps. */
	while (offset < text.len && frame->height) {
		struct cli_scrollback_cell cell = cli_scrollback_cell(text, offset, column, frame->columns);
		int e = cell.flags & CLI_SCROLLBACK_WRAP ? cli_scrollback_row(line, skip, &start, offset, &column, frame) : 0;
		bool shown = !*skip && (text.data[offset] == '\t' || (cell.flags & CLI_SCROLLBACK_REPLACE));
		if (!e && frame->height && shown)
			e = cli_scrollback_cell_write(line, &start, offset, cell, frame);
		if (e)
			return e;
		column += cell.width;
		offset += cell.bytes;
	}
	return frame->height ? cli_scrollback_row(line, skip, &start, offset, &column, frame) : 0;
}

int
cli_scrollback_render_pane (struct cli_scrollback        *scrollback,
                            size_t                        left,
                            size_t                        top,
                            size_t                        columns,
                            size_t                        height,
                            struct cli_style_entry const *palette,
                            struct dstr                  *cache,
                            struct dstr                  *output)
{
	if (!left || !top || !columns || columns > INT_MAX || !height || !palette ||
	    columns > SIZE_MAX - (left - 1) || height > SIZE_MAX - (top - 1))
		return EINVAL;
	struct cli_scrollback_frame frame = cli_scrollback_frame(
		.output = output, .cache = cache, .palette = palette, .columns = columns, .height = height,
		.left = left, .row = top, .start = dstr_len(output), .style = CLI_STYLE_COUNT
	);
	size_t rows = 0;
	struct list *hook;
	list_foreach(hook, &scrollback->lines) {
		struct cli_scrollback_line *line = container_of(hook, struct cli_scrollback_line, hook);
		int e = cli_scrollback_line_rows(line, frame.columns);
		if (e)
			return e;
		rows += line->rows;
	}
	if (scrollback->offset && scrollback->columns == frame.columns &&
	    rows + scrollback->discarded > scrollback->rows)
		scrollback->offset += rows + scrollback->discarded - scrollback->rows;
	scrollback->discarded = 0;
	scrollback->rows = rows;
	scrollback->columns = frame.columns;
	size_t maximum = rows > frame.height ? rows - frame.height : 0;
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
		int e = cli_scrollback_line_render(line, &skip, &frame);
		if (e)
			return e;
		if (!frame.height)
			break;
	}
	while (frame.height) {
		int e = cli_scrollback_frame_end(&frame, 0);
		if (e)
			return e;
	}
	return 0;
}
