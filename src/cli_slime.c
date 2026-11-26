/** @file The slime (cli_slime.h).  Each stride the head shoots ahead while
 *  the mass lags, then the tail pulls in and the mass surges.  Area is
 *  conserved: stretched is low, bunched is tall.  At a wall the blob
 *  squashes and wobbles before it turns round, and the cells it leaves dry
 *  from green to grey.  The model is slime.sh's integer arithmetic, frame
 *  for frame, in positions folded so that the blob always crawls away from
 *  the wall it last left: turning round only flips the mirroring. */
#include <errno.h>
#include <string.h>

#include "cli_slime.h"

#define ONE    1024 /* fixed-point 1.0 */
#define AFT    307  /* the peak at 0.3 of the length */
#define FORE   717  /* the peak at 0.7 of the length */
#define SWING  (FORE - AFT)
#define REST   28   /* the length between strides */
#define SQUASH 18   /* the length against a wall */
#define STRIDE 48   /* the LCG adds 0, 8 or 16 */
#define AREA   224  /* peak height times length */
#define SPEED  3    /* head or tail travel per frame */
#define HOLD   8    /* frames squashed */
#define TRAIL  28   /* frames a trail cell lasts */

/* A frame visits at most WINDOW adjacent columns: those the body covered
 * in the last TRAIL + 1 frames, with one end moving at most SPEED per frame
 * and the body at most LONGEST long, as a stretch overshoots its target by
 * less than SPEED.  A frame takes a CHA, then per column a CUF, an SGR and
 * a glyph, then an SGR reset.
 */
#define LONGEST (STRIDE + 16 + SPEED - 1)
#define WINDOW  ((LONGEST + SPEED * (TRAIL + 1)) / 8 + 1)

static_assert(WINDOW <= CLI_SLIME_RING);
static_assert(!(CLI_SLIME_RING & (CLI_SLIME_RING - 1)));
static_assert(CLI_SLIME_FRAME_MAX >= 8 + WINDOW * (5 + 11 + 3) + 3);
static_assert((uint64_t)CLI_SLIME_MAX_COLS * 8 + SPEED <= UINT32_MAX);
static_assert(TRAIL % 4 == 0);

/* xterm-256 colours: the body by glyph height, the trail by age quarter,
 * oldest first.  0 is the terminal's default. */
static uint8_t const cli_slime_body_ink[9] = {0, 28, 34, 34, 40, 40, 46, 82, 120};
static uint8_t const cli_slime_trail_ink[4] = {236, 22, 22, 28};

/* sin(k * 45 degrees), ONE = 1 */
static int16_t const cli_slime_wobble[8] = {0, 724, 1024, 724, 0, -724, -1024, -724};

/** One frame's body profile. */
struct cli_slime_shape {
	uint32_t len;  /**< eighths of a cell */
	uint32_t up;   /**< the peak's position from the tail, ONE = len */
	uint32_t peak; /**< the peak's height, ONE = one eighth */
};

/* The length of the next stretch, from slime.sh's LCG. */
static void
cli_slime_stride (struct cli_slime *slime)
{
	slime->rng = (slime->rng * 1103515245U + 12345U) & 0x7fffffffU;
	slime->lmax = STRIDE + 8 * ((slime->rng >> 16U) % 3);
}

struct cli_slime
cli_slime (uint32_t  x0,
           uint32_t  cols,
           uint32_t  seed,
           bool      color,
           int      *e)
{
	struct cli_slime slime = {
		.x0    = x0,
		.cols  = cols,
		.len   = REST,
		.rng   = seed,
		.first = UINT32_MAX,
		.phase = CLI_SLIME_STRETCH,
		.color = color,
	};
	cli_slime_stride(&slime);
	*e = cols < CLI_SLIME_MIN_COLS || cols > CLI_SLIME_MAX_COLS ||
	     x0 > CLI_SLIME_MAX_COLS - cols ? EINVAL : 0;
	return slime;
}

/* Advance one frame; returns the body's profile to draw. */
static struct cli_slime_shape
cli_slime_step (struct cli_slime *slime)
{
	uint32_t const wall = slime->cols * 8;

	switch (slime->phase) {
	case CLI_SLIME_LAUNCH:
	case CLI_SLIME_STRETCH:
		if (slime->back + slime->len + SPEED >= wall) {
			slime->len = wall - slime->back;
			slime->rest = SQUASH;
		} else {
			slime->len += SPEED;
			if (slime->len < slime->lmax)
				break;
			slime->rest = REST;
		}
		slime->l0 = slime->len;
		slime->phase = CLI_SLIME_CONTRACT;
		break;

	case CLI_SLIME_CONTRACT:
		if (slime->len - slime->rest > SPEED) {
			slime->back += SPEED;
			slime->len -= SPEED;
			break;
		}
		slime->back += slime->len - slime->rest;
		slime->len = slime->rest;
		if (slime->rest == SQUASH) {
			slime->hold = HOLD;
			slime->jig = ONE / 3;
			slime->phase = CLI_SLIME_SQUASH;
		} else {
			slime->phase = CLI_SLIME_STRETCH;
			cli_slime_stride(slime);
		}
		break;

	case CLI_SLIME_SQUASH:
		if (--slime->hold)
			break;
		slime->back = 0;
		slime->rev ^= 1U;
		slime->phase = CLI_SLIME_LAUNCH;
		cli_slime_stride(slime);
		break;
	}

	/* The peak, measured from the tail, moves from 0.7 to 0.3 of the
	 * length while stretching, the mass lagging, and back while
	 * contracting.  Leaving a wall, the mass stays at it.
	 */
	uint32_t up = FORE;
	if (slime->phase == CLI_SLIME_LAUNCH) {
		up = AFT;
	} else if (slime->phase == CLI_SLIME_STRETCH) {
		uint32_t const g = (slime->len - REST) * ONE / (slime->lmax - REST);
		up -= SWING * g / ONE;
	} else if (slime->phase == CLI_SLIME_CONTRACT) {
		uint32_t const g = (slime->l0 - slime->len) * ONE / (slime->l0 - slime->rest);
		up = AFT + SWING * g / ONE;
	}

	uint32_t peak = AREA * ONE / slime->len;
	if (peak > 8 * ONE)
		peak = 8 * ONE;
	int32_t const amp = slime->jig * cli_slime_wobble[slime->tick] / ONE;
	slime->jig = slime->jig * 7 / 8;
	slime->tick = (slime->tick + 1U) & 7U;
	return (struct cli_slime_shape){
		.len  = slime->len,
		.up   = up,
		.peak = peak * (ONE + (uint32_t)amp) / ONE,
	};
}

/* The glyph height in eighths at @p d from the tail, which wraps round when
 * behind it, or 0 off the body. */
static uint32_t
cli_slime_shape_height (struct cli_slime_shape const *shape,
                        uint32_t                      d)
{
	if (d - 1 >= shape->len - 1)
		return 0;
	/* u: where along the body; q: how far from the peak, ONE at an end */
	uint32_t const u = d * ONE / shape->len;
	uint32_t const q = u <= shape->up ? (shape->up - u) * ONE / shape->up
	                                  : (u - shape->up) * ONE / (ONE - shape->up);
	uint32_t const v = shape->peak * (ONE - q * q / ONE) / ONE;
	uint32_t const h = (v + ONE / 2) / ONE;
	return h < 8 ? h : 8;
}

static char *
cli_slime_num (char     *p,
               uint32_t  n)
{
	char tmp[10];
	char *t = tmp + sizeof tmp;
	do
		*--t = (char)('0' + n % 10);
	while (n /= 10);
	size_t const len = (size_t)(tmp + sizeof tmp - t);
	memcpy(p, t, len);
	return p + len;
}

/* A CSI sequence; the count is left out when it is the default, 1, or 0
 * for SGR. */
static char *
cli_slime_csi (char     *p,
               uint32_t  n,
               char      f)
{
	*p++ = '\033';
	*p++ = '[';
	if (n > 1)
		p = cli_slime_num(p, n);
	*p++ = f;
	return p;
}

/* SGR to colour @p ink, or to the default for 0. */
static char *
cli_slime_sgr (char     *p,
               uint32_t  ink)
{
	if (!ink)
		return cli_slime_csi(p, 0, 'm');
	memcpy(p, "\033[38;5;", 7);
	p = cli_slime_num(p + 7, ink);
	*p++ = 'm';
	return p;
}

/* Encode the columns whose look changed, left to right: a CHA to the first,
 * a CUF over unchanged ones, an SGR where the colour changes, the default
 * colour at the end.  Returns the byte count, 0 if nothing changed. */
static size_t
cli_slime_draw (struct cli_slime             *slime,
                struct cli_slime_shape const *shape,
                char                          buf[static CLI_SLIME_FRAME_MAX])
{
	/* The body's columns and the visible ones of the last frame */
	uint32_t lo = (slime->back + 4) / 8;
	uint32_t hi = (slime->back + shape->len + 3) / 8;
	if (slime->rev) {
		uint32_t const flo = lo;
		lo = slime->cols - hi;
		hi = slime->cols - flo;
	}
	lo = lo < slime->first ? lo : slime->first;
	hi = hi > slime->end ? hi : slime->end;

	/* Column lo's centre from the tail, 8 more or less per column */
	uint32_t const dd = slime->rev ? -8U : 8U;
	uint32_t d = (slime->rev ? slime->cols - 1 - lo : lo) * 8 + 4 - slime->back;

	char *p = buf;
	uint32_t at = UINT32_MAX; /* the cursor's column, unknown at first */
	uint32_t pen = 0;         /* the colour in effect */
	slime->first = UINT32_MAX;
	slime->end = 0;
	for (uint32_t c = lo; c < hi; ++c, d += dd) {
		uint32_t const k = c % CLI_SLIME_RING;
		uint32_t h = cli_slime_shape_height(shape, d);
		uint32_t ink = 0;
		if (h) {
			slime->age[k] = slime->color ? TRAIL : 0;
			ink = slime->color ? cli_slime_body_ink[h] : 0;
		} else if (slime->age[k]) {
			/* Age 1 -> 0 still draws; the next frame erases. */
			h = 1;
			ink = cli_slime_trail_ink[--slime->age[k] / (TRAIL / 4)];
		}
		if (h) {
			if (slime->first > c)
				slime->first = c;
			slime->end = c + 1;
		}

		uint16_t const look = (uint16_t)(h << 8U | ink);
		if (look == slime->shown[k])
			continue;
		slime->shown[k] = look;

		if (at > c)
			p = cli_slime_csi(p, slime->x0 + c + 1, 'G');
		else if (at < c)
			p = cli_slime_csi(p, c - at, 'C');
		at = c + 1;
		if (ink != pen) {
			p = cli_slime_sgr(p, ink);
			pen = ink;
		}
		if (h) {
			/* U+2581 to U+2588 in UTF-8, whatever the locale */
			*p++ = '\xe2';
			*p++ = '\x96';
			*p++ = (char)(0x80 + h);
		} else {
			*p++ = ' ';
		}
	}
	if (pen)
		p = cli_slime_sgr(p, 0);
	return (size_t)(p - buf);
}

size_t
cli_slime_tick (struct cli_slime *slime,
                uint64_t          now,
                char              buf[static CLI_SLIME_FRAME_MAX])
{
	if (now < slime->due)
		return 0;
	uint64_t const next = slime->due + CLI_SLIME_FRAME_NS;
	slime->due = next > now ? next : now + CLI_SLIME_FRAME_NS;
	struct cli_slime_shape const shape = cli_slime_step(slime);
	return cli_slime_draw(slime, &shape, buf);
}

void
cli_slime_blank (struct cli_slime *slime)
{
	memset(slime->shown, 0, sizeof slime->shown);
}

int
cli_slime_move (struct cli_slime *slime,
                uint32_t          x0,
                uint32_t          cols)
{
	if (!slime || slime->cols < CLI_SLIME_MIN_COLS || slime->cols > CLI_SLIME_MAX_COLS ||
	    slime->len >= slime->cols * 8 || cols < CLI_SLIME_MIN_COLS || cols > CLI_SLIME_MAX_COLS ||
	    x0 > CLI_SLIME_MAX_COLS - cols)
		return EINVAL;
	/* The tail can travel from the wall that it left to the length of the
	 * body before the far wall. It moves to the same share of that way on
	 * the new track. */
	uint64_t const way = slime->cols * 8 - slime->len;
	slime->back = (uint32_t)(slime->back * (uint64_t)(cols * 8 - slime->len) / way);
	slime->x0 = x0;
	slime->cols = cols;
	slime->first = UINT32_MAX;
	slime->end = 0;
	memset(slime->age, 0, sizeof slime->age);
	memset(slime->shown, 0, sizeof slime->shown);
	return 0;
}

#undef WINDOW
#undef LONGEST
#undef TRAIL
#undef HOLD
#undef SPEED
#undef AREA
#undef STRIDE
#undef SQUASH
#undef REST
#undef SWING
#undef FORE
#undef AFT
#undef ONE
