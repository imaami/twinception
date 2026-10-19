/** @file Changed-span output for independently positioned terminal rows. */
#ifndef TWINCEPTION_SRC_CLI_RENDER_H_
#define TWINCEPTION_SRC_CLI_RENDER_H_

#include <stddef.h>

#include "cstr_view.h"

struct dstr;

/** Decode nonempty UTF-8 input. Invalid input consumes one byte, width -1. */
extern size_t cli_render_character(struct cstr_view text, int *width);

/** Replace output[begin:] with its change from cache, then update cache.
 * Rows contain terminal-safe UTF-8, complete resetting SGR sequences and
 * padding to the same cell width. Geometry changes require clearing cache.
 * A null cache paints the complete row. Coordinates are one-based. */
extern int
cli_render_row (struct dstr *cache, struct dstr *output,
                size_t begin, size_t row, size_t column);

#endif /* TWINCEPTION_SRC_CLI_RENDER_H_ */
