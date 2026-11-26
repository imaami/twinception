/** @file The slime draws exactly the cells that changed, wherever its track
 *  starts and with or without colour; a frame comes when it is due, and a
 *  stall skips frames instead of replaying them; the animation is pinned. */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cli_slime.h"

/** A screen line as frames leave it: per column the glyph height << 8 and
 *  the colour, cell[0] being column base. */
struct cli_slime_test_line {
	uint16_t cell[256];
	uint32_t base;
	uint32_t x;   /**< the cursor's column */
	uint32_t ink; /**< the colour in effect */
};

/* Apply @p n bytes of a frame to @p line as a terminal would.  Only the
 * sequences that a slime writes are understood. */
static void
cli_slime_test_apply (struct cli_slime_test_line *line,
                      char const                 *p,
                      size_t                      n)
{
	char const *const end = p + n;
	while (p < end) {
		if (*p == '\033') {
			assert(end - p >= 3 && p[1] == '[');
			uint32_t v[3] = {};
			size_t i = 0;
			bool any = false;
			for (p += 2; p < end && ((*p >= '0' && *p <= '9') || *p == ';'); ++p) {
				any = true;
				if (*p != ';') {
					v[i] = v[i] * 10 + (uint32_t)(*p - '0');
					continue;
				}
				++i;
				assert(i < 3);
			}
			assert(p < end);
			size_t const count = any ? i + 1 : 0;
			switch (*p++) {
			case 'G':
				assert(count <= 1);
				line->x = (v[0] ? v[0] : 1) - 1;
				break;
			case 'C':
				assert(count <= 1);
				line->x += v[0] ? v[0] : 1;
				break;
			case 'm':
				assert(!count || (count == 3 && v[0] == 38 && v[1] == 5 &&
				                  v[2] && v[2] < 256));
				line->ink = count ? v[2] : 0;
				break;
			default:
				assert(!"a sequence that the slime does not write");
			}
			continue;
		}
		assert(line->x >= line->base &&
		       line->x - line->base < sizeof line->cell / sizeof *line->cell);
		uint16_t *const cell = &line->cell[line->x++ - line->base];
		if (*p == ' ') {
			assert(!line->ink);
			*cell = 0;
			++p;
			continue;
		}
		assert(end - p >= 3 && (unsigned char)p[0] == 0xe2 &&
		       (unsigned char)p[1] == 0x96);
		uint32_t const h = (unsigned char)p[2] - 0x80U;
		assert(h >= 1 && h <= 8);
		*cell = (uint16_t)(h << 8U | line->ink);
		p += 3;
	}
	assert(!line->ink);
}

static void
cli_slime_test_errors (void)
{
	int e = -1;
	(void)cli_slime(0, CLI_SLIME_MIN_COLS - 1, 1, true, &e);
	assert(e == EINVAL);
	(void)cli_slime(0, UINT32_MAX, 1, true, &e);
	assert(e == EINVAL);
	(void)cli_slime(UINT32_MAX, CLI_SLIME_MIN_COLS, 1, true, &e);
	assert(e == EINVAL);
	(void)cli_slime(1, CLI_SLIME_MAX_COLS, 1, true, &e);
	assert(e == EINVAL);
	(void)cli_slime(0, CLI_SLIME_MAX_COLS, 1, true, &e);
	assert(!e);
}

/* A frame moves the next one a period on, from its own due time while that
 * keeps the pace, else from the time it came: a stall skips frames. */
static void
cli_slime_test_tick (void)
{
	int e;
	struct cli_slime slime = cli_slime(0, 24, 1, true, &e);
	assert(!e && !slime.due);
	char buf[CLI_SLIME_FRAME_MAX];
	uint64_t due = 1'000'000'000;
	assert(cli_slime_tick(&slime, due, buf));
	assert(slime.due == due + CLI_SLIME_FRAME_NS);
	struct cli_slime const before = slime;
	assert(!cli_slime_tick(&slime, slime.due - 1, buf));
	assert(!memcmp(&slime, &before, sizeof slime));
	due = slime.due;
	(void)cli_slime_tick(&slime, due + CLI_SLIME_FRAME_NS / 2, buf);
	assert(slime.due == due + CLI_SLIME_FRAME_NS);
	due = slime.due;
	(void)cli_slime_tick(&slime, due + 10 * (uint64_t)CLI_SLIME_FRAME_NS, buf);
	assert(slime.due == due + 11 * (uint64_t)CLI_SLIME_FRAME_NS);
}

/* The rightmost track that a terminal can have draws as one at column 0. */
static void
cli_slime_test_edge (void)
{
	uint32_t const x0 = CLI_SLIME_MAX_COLS - CLI_SLIME_MIN_COLS;
	int e;
	struct cli_slime far = cli_slime(x0, CLI_SLIME_MIN_COLS, 5, true, &e);
	assert(!e);
	struct cli_slime near = cli_slime(0, CLI_SLIME_MIN_COLS, 5, true, &e);
	assert(!e);
	struct cli_slime_test_line lf = {.base = x0};
	struct cli_slime_test_line ln = {};
	for (uint32_t k = 0; k < 5'000; ++k) {
		char buf[CLI_SLIME_FRAME_MAX];
		cli_slime_test_apply(&lf, buf, cli_slime_tick(&far, far.due, buf));
		cli_slime_test_apply(&ln, buf, cli_slime_tick(&near, near.due, buf));
		assert(!memcmp(lf.cell, ln.cell, sizeof lf.cell));
		for (uint32_t i = CLI_SLIME_MIN_COLS; i < 256; ++i)
			assert(!ln.cell[i]);
	}
}

/* Crawl @p slime until @p done returns true, at most 100000 frames, and
 * apply the frames to @p line. */
static void
cli_slime_test_crawl (struct cli_slime           *slime,
                      struct cli_slime_test_line *line,
                      bool                      (*done) (struct cli_slime const *slime))
{
	for (uint32_t k = 0; !done(slime); ++k) {
		assert(k < 100000);
		char buf[CLI_SLIME_FRAME_MAX];
		cli_slime_test_apply(line, buf, cli_slime_tick(slime, slime->due, buf));
	}
}

static bool
cli_slime_test_back_half (struct cli_slime const *slime)
{
	return slime->rev && slime->phase == CLI_SLIME_STRETCH && slime->back > slime->cols * 3;
}

static bool
cli_slime_test_squashed (struct cli_slime const *slime)
{
	return slime->phase == CLI_SLIME_SQUASH;
}

/* The direction and the shape of a moved slime do not change, and its tail
 * is at the same share of the way between the walls, to an eighth of a
 * cell. On the blank track it draws all of its body at once, and in
 * thousands of frames after that, no cell past the track. Against a wall,
 * it stays there. A track that does not fit, or a slime that cli_slime()
 * did not make, changes nothing. */
static void
cli_slime_test_move (void)
{
	int e;
	struct cli_slime slime = cli_slime(4, 80, 3, true, &e);
	assert(!e);
	struct cli_slime const fresh = slime;
	struct cli_slime none = {};
	assert(cli_slime_move(&slime, 0, CLI_SLIME_MIN_COLS - 1) == EINVAL);
	assert(cli_slime_move(&slime, CLI_SLIME_MAX_COLS - 11, CLI_SLIME_MIN_COLS) == EINVAL);
	assert(cli_slime_move(nullptr, 0, 40) == EINVAL);
	assert(cli_slime_move(&none, 0, 40) == EINVAL);
	assert(!memcmp(&slime, &fresh, sizeof slime));

	struct cli_slime_test_line line = {.base = 4};
	cli_slime_test_crawl(&slime, &line, cli_slime_test_back_half);
	static uint32_t const widths[] = {30, 200, CLI_SLIME_MIN_COLS, 80};
	for (size_t i = 0; i < sizeof widths / sizeof *widths; ++i) {
		struct cli_slime const old = slime;
		uint32_t const cols = widths[i];
		assert(!cli_slime_move(&slime, 2, cols));
		assert(slime.x0 == 2 && slime.cols == cols && slime.len == old.len &&
		       slime.rev == old.rev && slime.phase == old.phase && slime.due == old.due &&
		       slime.lmax == old.lmax && slime.rng == old.rng);
		uint64_t const before = (uint64_t)old.back * (cols * 8 - old.len);
		uint64_t const after = (uint64_t)slime.back * (old.cols * 8 - old.len);
		assert(after <= before && before - after < old.cols * 8 - old.len);
		struct cli_slime_test_line moved = {.base = 2};
		char buf[CLI_SLIME_FRAME_MAX];
		cli_slime_test_apply(&moved, buf, cli_slime_tick(&slime, slime.due, buf));
		uint32_t drawn = 0;
		for (uint32_t c = 0; c < cols; ++c)
			drawn += moved.cell[c] >> 8U;
		assert(drawn);
		for (uint32_t k = 0; k < 3000; ++k) {
			cli_slime_test_apply(&moved, buf, cli_slime_tick(&slime, slime.due, buf));
			for (uint32_t c = cols; c < 254; ++c)
				assert(!moved.cell[c]);
		}
	}

	struct cli_slime_test_line last = {.base = 2};
	cli_slime_test_crawl(&slime, &last, cli_slime_test_squashed);
	assert(!cli_slime_move(&slime, 0, 50));
	assert(slime.phase == CLI_SLIME_SQUASH && slime.back + slime.len == 50 * 8);
}

/*
 * Four slimes from one seed: one drawn as usual, one on a line cleared and
 * blanked before each frame, one with its track from column 9, and one
 * without colour.  The first shows what the second shows, the third the
 * same nine columns on, and the fourth the body alone, uncoloured: 22 and
 * 236 colour only the trail, 28 the trail and the body's lowest cells, and
 * the other colours only the body.  Returns a hash of the frames.
 */
static uint64_t
cli_slime_test_frames (uint32_t cols,
                       uint32_t seed,
                       uint32_t frames)
{
	int e;
	struct cli_slime a = cli_slime(0, cols, seed, true, &e);
	assert(!e);
	struct cli_slime b = cli_slime(0, cols, seed, true, &e);
	assert(!e);
	struct cli_slime c = cli_slime(9, cols, seed, true, &e);
	assert(!e);
	struct cli_slime d = cli_slime(0, cols, seed, false, &e);
	assert(!e);
	struct cli_slime_test_line la = {};
	struct cli_slime_test_line lc = {};
	struct cli_slime_test_line ld = {};
	uint64_t hash = 14695981039346656037U;
	uint64_t now = 1'000'000'000;
	for (uint32_t k = 0; k < frames; ++k) {
		char buf[CLI_SLIME_FRAME_MAX];
		cli_slime_test_apply(&la, buf, cli_slime_tick(&a, now, buf));
		cli_slime_blank(&b);
		struct cli_slime_test_line lb = {};
		cli_slime_test_apply(&lb, buf, cli_slime_tick(&b, now, buf));
		cli_slime_test_apply(&lc, buf, cli_slime_tick(&c, now, buf));
		cli_slime_test_apply(&ld, buf, cli_slime_tick(&d, now, buf));
		assert(a.due == b.due && a.due == c.due && a.due == d.due);
		now = a.due;
		for (uint32_t i = 0; i < 9; ++i)
			assert(!lc.cell[i]);
		for (uint32_t i = 0; i < cols; ++i) {
			uint32_t const on = la.cell[i];
			uint32_t const off = ld.cell[i];
			uint32_t const ink = on & 0xffU;
			assert(on == lb.cell[i] && on == lc.cell[9 + i]);
			assert(!(off & 0xffU) && (!off || off >> 8U == on >> 8U));
			assert(off || !on || ink == 22 || ink == 28 || ink == 236);
			assert(!off || (ink != 22 && ink != 236));
			hash = (hash ^ on) * 1099511628211U;
		}
	}
	return hash;
}

int
main (void)
{
	static struct {
		uint32_t cols;
		uint32_t seed;
		uint64_t hash;
	} const runs[] = {
		{ 12,     1, 0x12fc24347085e387},
		{ 13,     2, 0xe112f3508357e7c3},
		{ 24, 12345, 0x1d0555765ff352c7},
		{ 80, 99991, 0xb50570d110b1a9af},
		{200,     7, 0x9b4726bcd46ff899},
	};
	cli_slime_test_errors();
	cli_slime_test_tick();
	cli_slime_test_edge();
	cli_slime_test_move();
	int failed = 0;
	for (size_t i = 0; i < sizeof runs / sizeof *runs; ++i) {
		uint64_t const hash = cli_slime_test_frames(runs[i].cols, runs[i].seed, 20'000);
		if (hash != runs[i].hash) {
			fprintf(stderr, "cli_slime: %" PRIu32 " columns, seed %" PRIu32
			        ": frame hash %#" PRIx64 ", not %#" PRIx64 "\n",
			        runs[i].cols, runs[i].seed, hash, runs[i].hash);
			failed = 1;
		}
	}
	return failed;
}
