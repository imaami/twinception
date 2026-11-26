/** @file The slime, a spinner: a blob that oozes back and forth along a
 *  track of terminal cells.  A frame is encoded as the cursor moves, SGR
 *  colours and block glyphs that turn the cells drawn last into the cells of
 *  the frame.  The slime owns no descriptor, clock or signal: the host
 *  passes the time, writes the bytes, and says when the track was blanked
 *  behind the slime's back. */
#ifndef TWINCEPTION_SRC_CLI_SLIME_H_
#define TWINCEPTION_SRC_CLI_SLIME_H_

#include <stddef.h>
#include <stdint.h>

/** The frame period: 20 frames per second. */
#define CLI_SLIME_FRAME_NS  50'000'000

/** The most bytes that a frame takes. */
#define CLI_SLIME_FRAME_MAX 512

/** The narrowest track, in cells. */
#define CLI_SLIME_MIN_COLS  12

/** The most columns that a track and its offset take together: the width
 *  of a terminal, an unsigned short in struct winsize. */
#define CLI_SLIME_MAX_COLS  65535

/** The size of the rings of struct cli_slime, which track columns index
 *  modulo it: a power of two, and more columns than a frame visits. */
#define CLI_SLIME_RING      32U

enum cli_slime_phase : uint8_t {
	CLI_SLIME_LAUNCH,   /**< off a wall: the head runs, the mass stays */
	CLI_SLIME_STRETCH,  /**< the head runs, the mass lags */
	CLI_SLIME_CONTRACT, /**< the tail catches up, the mass surges */
	CLI_SLIME_SQUASH,   /**< against a wall, wobbling */
};

/**
 * The blob, and what the screen shows of it.  Positions are eighths of a
 * cell from the wall the blob last left, so it always crawls toward higher
 * ones, and rev mirrors them onto the track.  The host reads due, and
 * leaves the rest to the functions below.
 */
struct cli_slime {
	uint64_t             due;                   /**< the next frame, in ns of
	                                                 CLOCK_MONOTONIC */
	uint32_t             x0;                    /**< the screen column of the
	                                                 track's first cell */
	uint32_t             cols;                  /**< the track's width */
	uint32_t             back;                  /**< the tail; the head is at
	                                                 back + len */
	uint32_t             len;
	uint32_t             lmax;                  /**< the length that ends
	                                                 this stretch */
	uint32_t             l0;                    /**< the length when this
	                                                 contraction began */
	uint32_t             rest;                  /**< the length that ends it */
	uint32_t             rng;                   /**< slime.sh's stride LCG */
	uint32_t             first;                 /**< the visible columns of
	                                                 the last frame are */
	uint32_t             end;                   /**< [first, end) */
	int32_t              jig;                   /**< wobble amplitude */
	uint16_t             shown[CLI_SLIME_RING]; /**< on the screen: glyph
	                                                 height << 8 | colour */
	uint8_t              age[CLI_SLIME_RING];   /**< trail frames left */
	uint8_t              hold  : 4;             /**< squashed frames left */
	uint8_t              tick  : 3;             /**< wobble phase, 0 to 7 */
	uint8_t              rev   : 1;             /**< crawling right to left */
	uint8_t              color : 1;             /**< colours and a trail */
	uint8_t              phase : 2;
};

/**
 * A slime on a track of @p cols cells from screen column @p x0, the first
 * column being 0, and its first frame due at once.  @p seed picks the
 * lengths of its strides.  Without @p color, the slime is drawn in the
 * terminal's colour and leaves no trail.  The error number in @p *e is
 * EINVAL if @p cols is below CLI_SLIME_MIN_COLS or @p x0 + @p cols above
 * CLI_SLIME_MAX_COLS, and 0 otherwise.
 */
extern struct cli_slime
cli_slime (uint32_t  x0,
           uint32_t  cols,
           uint32_t  seed,
           bool      color,
           int      *e);

/**
 * If a frame is due at @p now, in ns of CLOCK_MONOTONIC, advance the slime
 * and encode the cells that changed into @p buf, left to right.  The bytes
 * expect the cursor on the track's line, leave it there, and end in the
 * default colour.  The next frame is due a period after this one was, or a
 * period from @p now if that has passed: a stall skips frames rather than
 * replay them.  Returns the byte count, 0 if no frame was due or no cell
 * changed.
 */
extern size_t
cli_slime_tick (struct cli_slime *slime,
                uint64_t          now,
                char              buf[static CLI_SLIME_FRAME_MAX]);

/** The track's cells are blank on the screen now, cleared or written over:
 *  the next frame draws all of the slime. */
extern void
cli_slime_blank (struct cli_slime *slime);

/**
 * Move the slime to a track of @p cols cells from screen column @p x0, as
 * on a resized terminal: the tail goes to the same share of the way that
 * the body can travel between the walls. The cells of the new track are
 * blank on the screen, so the next frame draws all of the slime, which has
 * no trail then. Returns EINVAL, and changes nothing, if @p slime is not a
 * slime that cli_slime() made, if @p cols is below CLI_SLIME_MIN_COLS, or
 * if @p x0 + @p cols is above CLI_SLIME_MAX_COLS. Returns 0 otherwise.
 */
extern int
cli_slime_move (struct cli_slime *slime,
                uint32_t          x0,
                uint32_t          cols);

#endif /* TWINCEPTION_SRC_CLI_SLIME_H_ */
