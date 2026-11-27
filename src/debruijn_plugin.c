/* debruijn_plugin.c - a plugin with one tool, debruijn, that explores the
 * tree of the binary de Bruijn sequences of the orders 2 to 8. In the
 * library, a sequence is named a word. src/debruijn.h is the library of
 * the orders 2 to 6, where a sequence is one uint64_t. src/debruijn_wide.h
 * is the library of the orders 7 and 8, where a sequence is 128 or 256
 * bits in 64-bit limbs.
 *
 * A sequence of order n is 2^n bits long, and every value of n bits is in
 * the sequence once, also across the end of the sequence to its start. The
 * sequences of the library start with n zero bits. The sequence 0x3 of
 * order 2 is the root of the tree. A sequence of order n from 2 to 7 is the
 * parent of one sequence of order n + 1 for each mask from 0 to
 * 2^(2^(n-1)-1) - 1 (debruijn_expand()), and a sequence of order 3 to 8 is
 * the child of exactly one sequence (debruijn_collapse()). The address of
 * a sequence is the masks on the way from the root to the sequence, such
 * as 1.7.127.32767. The masks 0 lead to the numerically first sequence of
 * each order, and the largest masks to the last. The rank of a sequence is
 * its position among the sequences of its order, sorted as unsigned
 * numbers and counted from 0 (debruijn_rank()).
 *
 * The output of an action is its result and nothing else, as the output of
 * a calculator:
 *
 * - expand: the child of a sequence for a mask, such as 0x07dcd629.
 * - collapse: the parent of a sequence and the mask of the sequence in the
 *   parent, such as 0x0f65 127.
 * - path: the sequences on the way from the root to a sequence, without
 *   the root, as 1d.0f65.07dcd629.03f79d71b4cb0a89.
 * - address: the address of a sequence, such as 1.7.127.32767.
 * - children: the children of a sequence from the mask first on, at most
 *   limit of them, one on a line.
 * - windows: for each bit index of a sequence, from the least significant
 *   bit, the value of the order bits from the index up, one on a line.
 * - cycles: the cycles of the bit indexes of a sequence that was rotated
 *   left by rot bits. The value that windows prints for an index is the
 *   next index. One cycle on a line, as 0 15 14 12.
 * - rank: the rank of a sequence.
 * - unrank: the sequence at a rank of an order, and its address, as
 *   0x021d16a7b37e4ae3 0.1.100.10548.
 * - ordered: rows as for unrank, from rank first on, at most limit rows.
 *
 * The sequence of an action is in the arguments of a call in one of three
 * forms (debruijn_plugin_rule_needs()): the argument seq is the sequence as
 * a number, the argument addr is the address of the sequence, and the
 * arguments order and rank are the rank of the sequence in the order. A
 * call with two of the forms is refused. The root is no address, because
 * an address is at least one mask. unrank takes order and rank only, and
 * ordered takes no sequence.
 *
 * A number in a string is of up to 256 bits, in the notation that
 * strtoll() reads with the base 0, but without a + sign
 * (debruijn_plugin_number()): hexadecimal after 0x, binary after 0b, octal
 * after 0, else decimal. seq and each mask of addr are such numbers, and
 * rank, mask, rot and first are JSON integers or such numbers. A prefix on
 * the leftmost mask of an address is the base of all its masks, so
 * 0b1.110.1010101.111110000011111 is binary. A negative seq, or a negative
 * mask of addr, stands for its two's complement in the bits of the
 * sequence or mask: 2^n bits for a sequence of order n, which then needs
 * the argument order, and 2^(n-1) - 1 bits for a mask that expands a
 * sequence of order n. rank, mask, rot and first are not negative. The
 * sequences in the output are hexadecimal. libgmp converts the digits of a
 * number to limbs and the limbs of a rank to decimal digits, with its
 * functions for limbs, which allocate no memory for numbers of this size.
 *
 * seq can be a sequence in any rotation. An action rotates seq to start
 * with its n zero bits (debruijn_normalize()) before the library reads it,
 * and windows and cycles read seq without that rotation. Without the
 * argument order, the order is computed from the bit width of seq: a
 * rotation of a sequence of order n is 2^n - n to 2^n bits wide.
 *
 * The plugin defines no options, creates no instances and stores no data,
 * and a call ends before its start() returns. check_debruijn
 * (tests/askai.py) runs it.
 */
#include <ctype.h>
#include <errno.h>
#include <gmp.h>
#include <inttypes.h>
#include <stdbit.h>
#include <stdckdint.h>
#include <stdint.h>

#include <json-c/json.h>

#include "compat.h"
#include "debruijn.h"
#include "debruijn_wide.h"
#include "jb.h"
#include "plugin.h"
#include "util.h"
#include "xlist.h"

PLUGIN_MANIFEST(.name = "debruijn", .tools = { "debruijn" });

enum {
	DEBRUIJN_PLUGIN_LIMIT = 64,     /* the default of the argument limit */
	DEBRUIJN_PLUGIN_LINE  = 160,    /* bytes of a line of the output, at most */
	DEBRUIJN_PLUGIN_BITS  = 64 * DEBRUIJN_WIDE_LIMBS,
	                                /* bits of a number, at most */
	DEBRUIJN_PLUGIN_DEPTH = DEBRUIJN_WIDE_MAX_ORDER - DEBRUIJN_MIN_ORDER,
	                                /* masks in an address, at most */
};
static_assert(GMP_NAIL_BITS == 0 && GMP_NUMB_BITS <= 64, "the limbs of libgmp");

/* The arguments of the tool: X(ID, name). */
#define DEBRUIJN_PLUGIN_ARG_LIST(X) \
        X(ACTION, "action")         \
        X(SEQ,    "seq")            \
        X(ORDER,  "order")          \
        X(ADDR,   "addr")           \
        X(RANK,   "rank")           \
        X(MASK,   "mask")           \
        X(ROT,    "rot")            \
        X(FIRST,  "first")          \
        X(LIMIT,  "limit")          \
        X(OFFSET, "offset")

#define DEBRUIJN_PLUGIN_ARG_ENUM(id, ...) DEBRUIJN_PLUGIN_ARG_##id,
enum debruijn_plugin_arg : uint8_t {
	DEBRUIJN_PLUGIN_ARG_LIST(DEBRUIJN_PLUGIN_ARG_ENUM) DEBRUIJN_PLUGIN_ARGS
};
#undef DEBRUIJN_PLUGIN_ARG_ENUM

static struct cstr_view const debruijn_plugin_arg_names[] = {
	DEBRUIJN_PLUGIN_ARG_LIST(XL_VIEW)
};

/* The bit of the argument @p id in a set of arguments. */
#define A(id) (UINT64_C(1) << DEBRUIJN_PLUGIN_ARG_##id)

/* The actions of the tool: X(ID, name, needs, low, high, cap).
 * - needs: the arguments that a call of the action must contain. If needs
 *   contains seq, the call contains the sequence in one of the three forms
 *   (debruijn_plugin_rule_needs()), and order is optional beside seq.
 * - low, high: the smallest and the largest order of the sequence of the
 *   action. expand and children compute sequences of the next order, so
 *   their largest order is 7. collapse, path and address compute the
 *   parent of the sequence, so their smallest order is 3.
 * - cap: the largest limit of an action that takes first and limit, and 0
 *   for an action that takes neither. The largest limit of children is
 *   smaller from order 6 (debruijn_plugin_cap()).
 * A call of every action can contain offset.
 */
#define DEBRUIJN_PLUGIN_ACTION_LIST(X)                             \
        X(EXPAND,   "expand",   A(SEQ) | A(MASK),   2, 7, 0)       \
        X(COLLAPSE, "collapse", A(SEQ),             3, 8, 0)       \
        X(PATH,     "path",     A(SEQ),             3, 8, 0)       \
        X(ADDRESS,  "address",  A(SEQ),             3, 8, 0)       \
        X(CHILDREN, "children", A(SEQ),             2, 7, 32768)   \
        X(WINDOWS,  "windows",  A(SEQ),             2, 8, 0)       \
        X(CYCLES,   "cycles",   A(SEQ) | A(ROT),    2, 8, 0)       \
        X(RANK,     "rank",     A(SEQ),             2, 8, 0)       \
        X(UNRANK,   "unrank",   A(ORDER) | A(RANK), 2, 8, 0)       \
        X(ORDERED,  "ordered",  A(ORDER),           2, 8, 256)
static_assert(DEBRUIJN_MIN_ORDER == 2 && DEBRUIJN_MAX_ORDER == 6 &&
              DEBRUIJN_WIDE_MAX_ORDER == 8, "the orders in the list and in the texts");

/* A call of each action contains seq, in one of its forms, or order, so
 * the order of each call is known. low to high are orders of the library.
 */
#define DEBRUIJN_PLUGIN_ACTION_CHECK(id, name, needs, low, high, cap)          \
        static_assert(((needs) & (A(SEQ) | A(ORDER))) &&                       \
                      DEBRUIJN_MIN_ORDER <= (low) && (low) <= (high) &&        \
                      (high) <= DEBRUIJN_WIDE_MAX_ORDER, name);
DEBRUIJN_PLUGIN_ACTION_LIST(DEBRUIJN_PLUGIN_ACTION_CHECK)
#undef DEBRUIJN_PLUGIN_ACTION_CHECK

#define DEBRUIJN_PLUGIN_ACTION_ENUM(id, ...) DEBRUIJN_PLUGIN_##id,
enum debruijn_plugin_action : uint8_t {
	DEBRUIJN_PLUGIN_ACTION_LIST(DEBRUIJN_PLUGIN_ACTION_ENUM) DEBRUIJN_PLUGIN_ACTIONS
};
#undef DEBRUIJN_PLUGIN_ACTION_ENUM

static struct cstr_view const debruijn_plugin_action_names[] = {
	DEBRUIJN_PLUGIN_ACTION_LIST(XL_VIEW)
};

/* The rule of an action: the columns of its row in the list of actions. */
struct debruijn_plugin_rule {
	uint32_t needs;
	unsigned cap;
	unsigned low;
	unsigned high;
};

#define DEBRUIJN_PLUGIN_ACTION_RULE(id, name, needs_, low_, high_, cap_) \
        { .needs = (needs_), .cap = (cap_), .low = (low_), .high = (high_) },
static struct debruijn_plugin_rule const debruijn_plugin_rules[] = {
	DEBRUIJN_PLUGIN_ACTION_LIST(DEBRUIJN_PLUGIN_ACTION_RULE)
};
#undef DEBRUIJN_PLUGIN_ACTION_RULE

/* Returns the largest limit of a call of the action @p action for a
 * sequence of the order @p order, which is an order of the action: the cap
 * of the rule of the action, and less for the children of a sequence of
 * order 6 or 7. A call runs on the thread of a client loop, where the
 * watchdog of askai reports a turn of more than 20 ms. 32768 children of a
 * sequence of order 5 take 11 ms. A child of a sequence of order 6 or 7 is
 * two or four limbs, and 32768 of them take 28 or 69 ms. 8192 and 4096 of
 * them take 7 and 9 ms.
 */
static unsigned
debruijn_plugin_cap (size_t   action,
                     unsigned order)
{
	static uint16_t const wide[] = { 8192, 4096 };
	static_assert(DEBRUIJN_MAX_ORDER + array_size(wide) == DEBRUIJN_WIDE_MAX_ORDER,
	              "a cap for each order of a sequence with wide children");
	if (action == DEBRUIJN_PLUGIN_CHILDREN && order >= DEBRUIJN_MAX_ORDER)
		return wide[order - DEBRUIJN_MAX_ORDER];
	return debruijn_plugin_rules[action].cap;
}

/* The output of a call. The lines gather in text and go to the call in
 * pieces. e is the first error of mcp_slot_write().
 */
struct debruijn_plugin_out {
	struct mcp_slot *slot;
	int              e;
	uint32_t         len;
	char             text[4096 - 16];
};

/* Writes the gathered lines of @p out to the call. */
static void
debruijn_plugin_flush (struct debruijn_plugin_out *out)
{
	size_t taken;
	if (!out->e && out->len)
		out->e = mcp_slot_write(out->slot, cstr_view_n(out->text, out->len), 0, &taken);
	out->len = 0;
}

/* Appends a line of at most DEBRUIJN_PLUGIN_LINE bytes to the output @p out,
 * as printf() formats it.
 */
#define debruijn_plugin_line(out, ...)                                            \
        ((out)->len + DEBRUIJN_PLUGIN_LINE > sizeof (out)->text                   \
         ? debruijn_plugin_flush(out) : (void)0,                                  \
         util_text_printf_(&(out)->len, (out)->text, sizeof (out)->text, __VA_ARGS__))

/* Ends the call of @p out with its output, or as failed with the error of
 * mcp_slot_write().
 */
static void
debruijn_plugin_end (struct debruijn_plugin_out *out)
{
	debruijn_plugin_flush(out);
	if (out->e)
		mcp_slot_fail(out->slot, out->e);
	else
		mcp_slot_finish(out->slot, 0);
}

/* Returns the largest mask of a sequence of the order @p order from 2 to
 * 7: a mask of that order is 2^(order-1) - 1 bits wide.
 */
static uint64_t
debruijn_plugin_masks (unsigned order)
{
	return (UINT64_C(1) << ((1U << (order - 1U)) - 1U)) - 1U;
}

/* Returns the number whose @p bits lowest bits are set, at most 256. */
static struct debruijn_wide
debruijn_plugin_ones (unsigned bits)
{
	struct debruijn_wide ones = {};
	for (unsigned limb = 0; bits; ++limb) {
		unsigned const part = bits < 64U ? bits : 64U;
		ones.limb[limb] = UINT64_MAX >> (64U - part);
		bits -= part;
	}
	return ones;
}

/* Returns the largest rank of the order @p order from 2 to 8: the
 * sequences of an order are 2^(2^(order-1) - order).
 */
static struct debruijn_wide
debruijn_plugin_ranks (unsigned order)
{
	return debruijn_plugin_ones((1U << (order - 1U)) - order);
}

/* Returns true if @p a is over @p b. */
static bool
debruijn_plugin_over (struct debruijn_wide const *a,
                      struct debruijn_wide const *b)
{
	unsigned limb = DEBRUIJN_WIDE_LIMBS - 1;
	while (limb && a->limb[limb] == b->limb[limb])
		--limb;
	return a->limb[limb] > b->limb[limb];
}

/* Returns the bit width of @p n: 0 for 0, else the index of its highest
 * set bit plus 1.
 */
static unsigned
debruijn_plugin_width (struct debruijn_wide const *n)
{
	unsigned limb = DEBRUIJN_WIDE_LIMBS - 1;
	while (limb && !n->limb[limb])
		--limb;
	return 64U * limb + stdc_bit_width(n->limb[limb]);
}

/* The digits of a number and a NUL: at most the 78 decimal digits of a
 * number of 256 bits.
 */
struct debruijn_plugin_text {
	char text[80];
};

/* Returns the hexadecimal digits of @p n at the width of the order
 * @p order: a digit for each 4 of the 2^order bits of a sequence. A number
 * that is wider is returned with all its digits, and so is a number of an
 * order under 2, which is the order of no sequence.
 */
static struct debruijn_plugin_text
debruijn_plugin_hex (struct debruijn_wide const *n,
                     unsigned                    order)
{
	unsigned const needed = (debruijn_plugin_width(n) + 3U) >> 2U;
	unsigned const fixed = order >= DEBRUIJN_MIN_ORDER ? 1U << order >> 2U : 1U;
	struct debruijn_plugin_text ret = {};
	/* From the last digit back, 16 digits of each limb. */
	char *at = &ret.text[needed > fixed ? needed : fixed];
	for (unsigned limb = 0; at > ret.text; ++limb) {
		uint64_t value = n->limb[limb];
		for (unsigned i = 0; i < 16U && at > ret.text; ++i, value >>= 4U)
			*--at = "0123456789abcdef"[value & 15U];
	}
	return ret;
}

/* Returns the decimal digits of @p n. libgmp converts a number of more
 * than 64 bits.
 */
static struct debruijn_plugin_text
debruijn_plugin_decimal (struct debruijn_wide const *n)
{
	struct debruijn_plugin_text ret = {};
	if (debruijn_plugin_width(n) <= 64U) {
		uint32_t len = 0;
		util_text_printf_(&len, ret.text, sizeof ret.text, "%" PRIu64, n->limb[0]);
		return ret;
	}
	/* The limbs of libgmp can be narrower than 64 bits. The top limb
	 * that mpn_get_str() reads must not be 0. */
	mp_limb_t limbs[DEBRUIJN_PLUGIN_BITS / GMP_NUMB_BITS];
	mp_size_t used = 0;
	for (unsigned k = 0; k < array_size(limbs); ++k) {
		unsigned const bit = k * GMP_NUMB_BITS;
		limbs[k] = (mp_limb_t)(n->limb[bit >> 6U] >> (bit & 63U));
		if (limbs[k])
			used = (mp_size_t)k + 1;
	}
	/* mpn_get_str() writes digit values, the highest first, and can
	 * write zeros before them. It needs one byte more than the digits. */
	unsigned char digits[sizeof ret.text];
	size_t const count = mpn_get_str(digits, 10, limbs, used);
	size_t zeros = 0;
	while (zeros + 1 < count && !digits[zeros])
		++zeros;
	for (size_t i = zeros; i < count; ++i)
		ret.text[i - zeros] = (char)('0' + digits[i]);
	return ret;
}

/* Drops the white space that strtoll() skips from the start of @p text. */
static struct cstr_view
debruijn_plugin_space (struct cstr_view text)
{
	while (text.len && isspace((unsigned char)*text.data)) {
		++text.data;
		--text.len;
	}
	return text;
}

/* Returns the value of the digit @p c of a base up to 16, or 16 for a byte
 * that is no digit.
 */
static unsigned
debruijn_plugin_digit (char c)
{
	if (c >= '0' && c <= '9')
		return (unsigned)(c - '0');
	char const letter = (char)(c | 0x20);
	return letter >= 'a' && letter <= 'f' ? (unsigned)(letter - 'a') + 10U : 16U;
}

/* Returns the base of the prefix of the number at the start of @p text,
 * after white space and a minus sign: 16 for 0x, 2 for 0b and 8 for a 0
 * before a digit. Returns 0 for a number without a prefix, such as 0.
 */
static unsigned
debruijn_plugin_base (struct cstr_view text)
{
	text = debruijn_plugin_space(text);
	if (text.len && *text.data == '-')
		text = cstr_view_slice(text, 1, SIZE_MAX);
	if (text.len < 2 || *text.data != '0')
		return 0;
	switch (text.data[1]) {
	case 'x':
	case 'X':
		return 16;
	case 'b':
	case 'B':
		return 2;
	default:
		return isdigit((unsigned char)text.data[1]) ? 8 : 0;
	}
}

/* Returns the number @p text of up to 256 bits, in the notation that
 * strtoll() reads in the base @p base: 0, 2, 8, 10 or 16. White space can
 * precede the number, but a + sign cannot. With the base 0, the prefix of
 * the number is the base: hexadecimal after 0x, binary after 0b, octal
 * after 0, else decimal. A number in the base 16 can start with 0x, and
 * one in the base 2 with 0b. A negative number stands for its two's
 * complement in @p bits bits, from 1 to 256.
 *
 * Stores 0 in @p *e, or EINVAL if @p text is not one number from its start
 * to its end, EDOM for a negative number of any size if @p bits is 0,
 * ERANGE for another number of more than 256 bits, or EOVERFLOW for a
 * negative number under -2^(bits-1), whose two's complement does not fit
 * in @p bits bits. Returns 0 with an error.
 */
static struct debruijn_wide
debruijn_plugin_number (struct cstr_view  text,
                        unsigned          base,
                        unsigned          bits,
                        int              *e)
{
	/* The digits of a number of 256 bits in each base, at most. */
	static uint16_t const most[] = { [2] = 256, [8] = 86, [10] = 78, [16] = 64 };
	struct debruijn_wide ret = {};
	*e = EINVAL;
	text = debruijn_plugin_space(text);
	bool const negative = text.len && *text.data == '-';
	text = cstr_view_slice(text, negative, SIZE_MAX);
	/* A prefix is 0x or 0b before a digit of its base. Without the digit,
	 * strtoll() reads the 0 alone, and the letter ends the number. */
	if (text.len > 2 && *text.data == '0') {
		char const letter = (char)(text.data[1] | 0x20);
		unsigned const prefix = 16U * (letter == 'x') + 2U * (letter == 'b');
		if (prefix && (!base || base == prefix) &&
		    debruijn_plugin_digit(text.data[2]) < prefix) {
			base = prefix;
			text = cstr_view_slice(text, 2, SIZE_MAX);
		}
	}
	if (!base)
		base = text.len && *text.data == '0' ? 8 : 10;
	if (!text.len)
		return ret;
	/* low is the number while it fits in 64 bits. digits is the digit
	 * values without the zeros before them, which libgmp converts if the
	 * number is over 64 bits. */
	unsigned char digits[DEBRUIJN_PLUGIN_BITS];
	size_t n = 0;
	uint64_t low = 0;
	bool wide = false;
	bool over = false;
	for (size_t i = 0; i < text.len; ++i) {
		unsigned const digit = debruijn_plugin_digit(text.data[i]);
		if (digit >= base)
			return ret;
		if (!n && !digit)
			continue;
		if (n == most[base])
			over = true;
		else
			digits[n++] = (unsigned char)digit;
		wide = wide || ckd_mul(&low, low, base) || ckd_add(&low, low, digit);
	}
	*e = negative && !bits ? EDOM : ERANGE;
	if (over)
		return ret;
	ret.limb[0] = low;
	if (wide) {
		/* The most digits of a base are a number under 2^260.
		 * mpn_set_str() needs one limb more than the number. */
		mp_limb_t limbs[(260 + GMP_NUMB_BITS - 1) / GMP_NUMB_BITS + 1];
		mp_size_t const used = mpn_set_str(limbs, digits, n, (int)base);
		ret = (struct debruijn_wide){};
		for (mp_size_t k = 0; k < used; ++k) {
			unsigned const bit = (unsigned)k * GMP_NUMB_BITS;
			if (bit < DEBRUIJN_PLUGIN_BITS)
				ret.limb[bit >> 6U] |= (uint64_t)limbs[k] << (bit & 63U);
			else if (limbs[k])
				return (struct debruijn_wide){};
		}
	}
	*e = 0;
	if (!negative || !debruijn_plugin_width(&ret))
		return ret;
	*e = EDOM;
	if (!bits)
		return (struct debruijn_wide){};
	/* 2^bits minus the number is the number minus 1 with its bits
	 * inverted, if the number minus 1 is under 2^(bits-1). */
	for (unsigned limb = 0; limb < DEBRUIJN_WIDE_LIMBS && !ret.limb[limb]--; ++limb)
		;
	*e = EOVERFLOW;
	if (debruijn_plugin_width(&ret) >= bits)
		return (struct debruijn_wide){};
	struct debruijn_wide const ones = debruijn_plugin_ones(bits);
	for (unsigned limb = 0; limb < DEBRUIJN_WIDE_LIMBS; ++limb)
		ret.limb[limb] = ~ret.limb[limb] & ones.limb[limb];
	*e = 0;
	return ret;
}

/* Reads the address @p text, 1 to 6 masks with a dot between two, each a
 * number as debruijn_plugin_number() reads it, into @p masks. A prefix on
 * the leftmost mask is the base of all the masks. Without it, each mask is
 * read in the base of its own prefix. Stores the number of masks in
 * @p *n. Returns 0 or EINVAL.
 */
static int
debruijn_plugin_address (struct cstr_view  text,
                         uint64_t          masks[static DEBRUIJN_PLUGIN_DEPTH],
                         unsigned         *n)
{
	unsigned const base = debruijn_plugin_base(text);
	*n = 0;
	for (struct cstr_view part = {}; text.len || !*n; ) {
		if (*n == DEBRUIJN_PLUGIN_DEPTH)
			return EINVAL;
		if (!cstr_view_cut(&text, '.', &part)) {
			part = text;
			text = cstr_view(nullptr);
		} else if (!text.len) {
			/* A dot ends no address. */
			return EINVAL;
		}
		/* Mask n expands a sequence of order n + 2, and is
		 * 2^(n+1) - 1 bits wide. */
		int e;
		struct debruijn_wide const value =
			debruijn_plugin_number(part, *n ? base : 0, (1U << (*n + 1U)) - 1U, &e);
		if (e || debruijn_plugin_width(&value) > 64U)
			return EINVAL;
		masks[(*n)++] = value.limb[0];
	}
	return 0;
}

/* Returns the smallest order n from 2 to 8 with 2^n at least the bit width
 * of @p seq, for @p seq from 3. A rotation of a sequence of order n is
 * 2^n - n to 2^n bits wide: the n zero bits of the sequence can be at its
 * top, in whole or in part, or not. Returns 0 for 0, and 1 for 1 and 2:
 * these numbers are rotations of no sequence, and their orders are no
 * orders of the library.
 */
static unsigned
debruijn_plugin_order (struct debruijn_wide const *seq)
{
	unsigned const width = debruijn_plugin_width(seq);
	return stdc_bit_width(width - (width > 2U) - (width == 2U && seq->limb[0] == 2U));
}

/* Refuses the number @p seq, which is no rotation of a sequence of the
 * order @p order, or of any order if @p order is under 2.
 */
static void
debruijn_plugin_no_sequence (struct mcp_slot            *slot,
                             struct debruijn_wide const *seq,
                             unsigned                    order)
{
	struct debruijn_plugin_text const hex = debruijn_plugin_hex(seq, order);
	if (order >= DEBRUIJN_MIN_ORDER)
		mcp_slot_tellf(slot, TOOL_FAILED, "0x%s is no sequence of order %u.", hex.text, order);
	else
		mcp_slot_tellf(slot, TOOL_FAILED, "0x%s is no sequence of any order from %d to %d.",
		               hex.text, DEBRUIJN_MIN_ORDER, DEBRUIJN_WIDE_MAX_ORDER);
}

/* Returns the refusal of a seq for the error @p e of
 * debruijn_plugin_number().
 */
static struct cstr_view
debruijn_plugin_no_number (int e)
{
	switch (e) {
	case ERANGE:
		return CSTR_VIEW("The seq argument must be a number of at most 256 bits.");
	case EDOM:
		return CSTR_VIEW("A negative seq needs the argument order: a sequence of order n "
		                 "is 2^n bits long.");
	case EOVERFLOW:
		return CSTR_VIEW("A negative seq stands for its two's complement in the 2^order "
		                 "bits of the sequence, and this number is under -2^(2^order - 1).");
	default:
		return CSTR_VIEW("The seq argument must be a string of a number without a + sign: "
		                 "hexadecimal after 0x, binary after 0b, octal after 0, else "
		                 "decimal.");
	}
}

/* Rotates @p seq of the order @p order, from 2 to 8, to start with its
 * zero bits: debruijn_normalize() for a sequence of one limb, and
 * debruijn_wide_normalize() above it. Returns 0 or EINVAL.
 */
static int
debruijn_plugin_normalize (struct debruijn_wide *seq,
                           unsigned              order)
{
	if (order > DEBRUIJN_MAX_ORDER)
		return debruijn_wide_normalize(seq, order);
	return debruijn_plugin_width(seq) > 64U ? EINVAL : debruijn_normalize(&seq->limb[0], order);
}

/* Stores in @p *child the child of @p seed of the order @p order, from 2
 * to 7, for @p mask: debruijn_expand() for a child of one limb, and
 * debruijn_wide_expand() above it. @p child can be @p seed. Returns 0 or
 * EINVAL.
 */
static int
debruijn_plugin_child (struct debruijn_wide       *child,
                       struct debruijn_wide const *seed,
                       unsigned                    order,
                       uint64_t                    mask)
{
	if (order >= DEBRUIJN_MAX_ORDER)
		return debruijn_wide_expand(child, seed, order, mask);
	/* seed can be child. */
	uint64_t const word = seed->limb[0];
	*child = (struct debruijn_wide){};
	return mask > UINT32_MAX ? EINVAL
	                         : debruijn_expand(&child->limb[0], word, order, (unsigned)mask);
}

/* Stores in @p *parent the parent of @p seq of the order @p order, from 3
 * to 8, and in @p *mask the mask of @p seq in the parent: debruijn_collapse()
 * for a sequence of one limb, and debruijn_wide_collapse() above it.
 * Returns 0 or EINVAL.
 */
static int
debruijn_plugin_parent (struct debruijn_wide       *parent,
                        uint64_t                   *mask,
                        struct debruijn_wide const *seq,
                        unsigned                    order)
{
	if (order > DEBRUIJN_MAX_ORDER)
		return debruijn_wide_collapse(parent, mask, seq, order);
	unsigned choices = 0;
	*parent = (struct debruijn_wide){};
	int const e = debruijn_collapse(&parent->limb[0], &choices, seq->limb[0], order);
	*mask = choices;
	return e;
}

/* The function and the argument that debruijn_plugin_narrow() passes a
 * sequence of one limb to.
 */
struct debruijn_plugin_emit {
	debruijn_wide_emit_fn *emit;
	void                  *arg;
};

/* Passes the sequence @p word of debruijn_ordered() to the function of the
 * struct debruijn_plugin_emit at @p arg.
 */
static int
debruijn_plugin_narrow (uint64_t  word,
                        unsigned  order,
                        void     *arg)
{
	struct debruijn_plugin_emit const *const to = arg;
	struct debruijn_wide const seq = { { word } };
	return to->emit(&seq, order, to->arg);
}

/* Passes at most @p limit sequences of the order @p order, from 2 to 8, to
 * @p emit in numeric order, from the rank @p first on: debruijn_ordered()
 * for sequences of one limb, and debruijn_wide_ordered() above it. Returns
 * 0 or the first error.
 */
static int
debruijn_plugin_ordered (unsigned                    order,
                         struct debruijn_wide const *first,
                         unsigned                    limit,
                         debruijn_wide_emit_fn      *emit,
                         void                       *arg)
{
	if (order > DEBRUIJN_MAX_ORDER)
		return debruijn_wide_ordered(order, first, limit, emit, arg);
	/* debruijn_ordered() emits nothing from a rank past the last. */
	struct debruijn_plugin_emit to = { emit, arg };
	return debruijn_ordered(order, debruijn_plugin_width(first) > 32U ? UINT32_MAX
	                                                                 : (unsigned)first->limb[0],
	                        limit, debruijn_plugin_narrow, &to);
}

/* expand: the child of @p seq for @p mask. */
static void
debruijn_plugin_expand (struct debruijn_plugin_out *out,
                        struct debruijn_wide const *seq,
                        unsigned                    order,
                        uint64_t                    mask)
{
	struct debruijn_wide child;
	if (debruijn_plugin_child(&child, seq, order, mask)) {
		debruijn_plugin_no_sequence(out->slot, seq, order);
		return;
	}
	debruijn_plugin_line(out, "0x%s\n", debruijn_plugin_hex(&child, order + 1U).text);
	debruijn_plugin_end(out);
}

/* collapse: the parent of @p seq and the mask of @p seq in the parent. */
static void
debruijn_plugin_collapse (struct debruijn_plugin_out *out,
                          struct debruijn_wide const *seq,
                          unsigned                    order)
{
	struct debruijn_wide parent;
	uint64_t mask;
	if (debruijn_plugin_parent(&parent, &mask, seq, order)) {
		debruijn_plugin_no_sequence(out->slot, seq, order);
		return;
	}
	debruijn_plugin_line(out, "0x%s %" PRIu64 "\n", debruijn_plugin_hex(&parent, order - 1U).text,
	                     mask);
	debruijn_plugin_end(out);
}

/* rank: the rank of @p seq. */
static void
debruijn_plugin_rank (struct debruijn_plugin_out *out,
                      struct debruijn_wide const *seq,
                      unsigned                    order)
{
	struct debruijn_wide rank = {};
	unsigned narrow = 0;
	int const e = order > DEBRUIJN_MAX_ORDER ? debruijn_wide_rank(&rank, seq, order)
	                                         : debruijn_rank(&narrow, seq->limb[0], order);
	if (e == EINVAL) {
		debruijn_plugin_no_sequence(out->slot, seq, order);
		return;
	}
	rank.limb[0] += narrow;
	out->e = e;
	debruijn_plugin_line(out, "%s\n", debruijn_plugin_decimal(&rank).text);
	debruijn_plugin_end(out);
}

/* Collapses @p seq of the order @p order, from 2 to 8, down to the root.
 * Stores the sequences on the way in @p seqs, seqs[0] the root and
 * seqs[order - 2] @p seq, and in @p masks[i] the mask of seqs[i + 1] in
 * seqs[i]. Returns 0 or EINVAL.
 */
static int
debruijn_plugin_ancestors (struct debruijn_wide const *seq,
                           unsigned                    order,
                           struct debruijn_wide        seqs[static DEBRUIJN_PLUGIN_DEPTH + 1],
                           uint64_t                    masks[static DEBRUIJN_PLUGIN_DEPTH])
{
	unsigned i = order - DEBRUIJN_MIN_ORDER;
	for (seqs[i] = *seq; i; --i) {
		int const e = debruijn_plugin_parent(&seqs[i - 1], &masks[i - 1], &seqs[i],
		                                     DEBRUIJN_MIN_ORDER + i);
		if (e)
			return e;
	}
	return 0;
}

/* path: the sequences on the way from the root to @p seq, without the
 * root, with a dot between two, each at the width of its order:
 * 17.09af.04653adf.
 */
static void
debruijn_plugin_path (struct debruijn_plugin_out *out,
                      struct debruijn_wide const *seq,
                      unsigned                    order)
{
	struct debruijn_wide seqs[DEBRUIJN_PLUGIN_DEPTH + 1];
	uint64_t masks[DEBRUIJN_PLUGIN_DEPTH];
	if (debruijn_plugin_ancestors(seq, order, seqs, masks)) {
		debruijn_plugin_no_sequence(out->slot, seq, order);
		return;
	}
	for (unsigned i = DEBRUIJN_MIN_ORDER + 1; i <= order; ++i)
		debruijn_plugin_line(out, "%s%c",
		                     debruijn_plugin_hex(&seqs[i - DEBRUIJN_MIN_ORDER], i).text,
		                     i < order ? '.' : '\n');
	debruijn_plugin_end(out);
}

/* Appends the address of @p seq of the order @p order and a newline to the
 * output @p out, and nothing for the root, whose address is empty. Returns
 * 0 or EINVAL.
 */
static int
debruijn_plugin_locate (struct debruijn_plugin_out *out,
                        struct debruijn_wide const *seq,
                        unsigned                    order)
{
	struct debruijn_wide seqs[DEBRUIJN_PLUGIN_DEPTH + 1];
	uint64_t masks[DEBRUIJN_PLUGIN_DEPTH];
	int const e = debruijn_plugin_ancestors(seq, order, seqs, masks);
	if (e)
		return e;
	unsigned const n = order - DEBRUIJN_MIN_ORDER;
	for (unsigned i = 0; i < n; ++i)
		debruijn_plugin_line(out, "%" PRIu64 "%c", masks[i], i + 1 < n ? '.' : '\n');
	return 0;
}

/* Appends a row of unrank and ordered to the output @p arg: @p seq, a space
 * and the address of @p seq, or the root alone. Returns 0, EINVAL or the
 * error of the output.
 */
static int
debruijn_plugin_row (struct debruijn_wide const *seq,
                     unsigned                    order,
                     void                       *arg)
{
	struct debruijn_plugin_out *out = arg;
	debruijn_plugin_line(out, "0x%s%c", debruijn_plugin_hex(seq, order).text,
	                     order == DEBRUIJN_MIN_ORDER ? '\n' : ' ');
	int e = debruijn_plugin_locate(out, seq, order);
	return e ? e : out->e;
}

/* Stores @p seq in the struct debruijn_wide at @p arg: the sequence at a
 * rank, as debruijn_plugin_ordered() emits it.
 */
static int
debruijn_plugin_store (struct debruijn_wide const *seq,
                       unsigned                    order,
                       void                       *arg)
{
	(void)order;
	*(struct debruijn_wide *)arg = *seq;
	return 0;
}

/* children: the children of @p seq from the mask @p first on, at most
 * @p limit of them.
 */
static void
debruijn_plugin_children (struct debruijn_plugin_out *out,
                          struct debruijn_wide const *seq,
                          unsigned                    order,
                          uint64_t                    first,
                          unsigned                    limit)
{
	/* A child of up to order 6 is one limb. */
	bool const narrow = order < DEBRUIJN_MAX_ORDER;
	int e = 0;
	struct debruijn_generator const seed =
		narrow ? debruijn_generator(seq->limb[0], order) : (struct debruijn_generator){};
	struct debruijn_wide_generator const wide =
		narrow ? (struct debruijn_wide_generator){}
		       : debruijn_wide_generator(seq, order, &e);
	if (seed.error | e) {
		debruijn_plugin_no_sequence(out->slot, seq, order);
		return;
	}
	/* The masks of a seed of order 7 are 2^63. */
	uint64_t const all = debruijn_plugin_masks(order) + 1U;
	uint64_t const end = first < all && limit < all - first ? first + limit : all;
	for (uint64_t mask = first; !out->e && mask < end; ++mask) {
		struct debruijn_wide child = {};
		out->e = narrow ? debruijn_generate(&child.limb[0], &seed, (unsigned)mask)
		                : debruijn_wide_generate(&child, &wide, mask);
		debruijn_plugin_line(out, "0x%s\n", debruijn_plugin_hex(&child, order + 1U).text);
	}
	debruijn_plugin_end(out);
}

/* windows: for each bit index of @p seq, from 0, the least significant
 * bit, the value of the @p order bits from the index up
 * (debruijn_wide_window()). The sequence can be in any rotation.
 */
static void
debruijn_plugin_windows (struct debruijn_plugin_out *out,
                         struct debruijn_wide const *seq,
                         unsigned                    order)
{
	/* A copy that the output cannot change, so that the compiler can
	 * leave its limbs in registers. */
	struct debruijn_wide const copy = *seq;
	unsigned const bits = 1U << order;
	for (unsigned index = 0; index < bits; ++index)
		debruijn_plugin_line(out, "%u\n", debruijn_wide_window(&copy, order, index));
	debruijn_plugin_end(out);
}

/* cycles: the cycles of the bit indexes of @p seq after a rotation to the
 * left by @p rot bits. Index 0 is the least significant bit. The value of
 * the @p order bits from an index up (debruijn_wide_window()) is the index
 * to read next. Each value of order bits is in the sequence at exactly one
 * index, so the reading returns to the index where it started, and each
 * index is in exactly one cycle. A line of the output is a cycle: the
 * indexes in the order of the reading, from the smallest index that is in
 * no line before it.
 */
static void
debruijn_plugin_cycles (struct debruijn_plugin_out *out,
                        struct debruijn_wide const *seq,
                        unsigned                    order,
                        unsigned                    rot)
{
	/* A copy that the output cannot change, as in windows. */
	struct debruijn_wide const copy = *seq;
	unsigned const bits = 1U << order;
	/* A set bit of read is an index that is in a cycle. */
	uint64_t read[DEBRUIJN_WIDE_LIMBS] = {};
	for (unsigned start = 0; start < bits; ++start) {
		if (debruijn_wide_test(read, start))
			continue;
		unsigned index = start;
		bool more;
		do {
			debruijn_wide_set(read, index);
			/* The rotation moves the bit index - rot of seq to the bit
			 * index, modulo bits. */
			unsigned const next = debruijn_wide_window(&copy, order,
			                                           (index - rot) & (bits - 1U));
			/* The cycle is complete if the next index is in a cycle
			 * already: it is the index where the cycle started. */
			more = !debruijn_wide_test(read, next);
			debruijn_plugin_line(out, "%u%c", index, more ? ' ' : '\n');
			index = next;
		} while (more);
	}
	debruijn_plugin_end(out);
}

/* Stores the JSON integer @p val in @p *n and returns 0 if it is from 0 to
 * @p max. Otherwise returns EINVAL and leaves @p *n unchanged.
 */
static int
debruijn_plugin_uint (struct json_object const *val,
                      unsigned                  max,
                      unsigned                 *n)
{
	uint64_t value;
	int const e = jb_uint_checked(val, max, &value);
	if (!e)
		*n = (unsigned)value;
	return e;
}

/* Stores the whole number @p val in @p *n and returns 0: a JSON integer
 * from 0, or a string of a number as debruijn_plugin_number() reads it,
 * without a minus sign. A number of more than 256 bits is stored as
 * 2^256 - 1, which is over the largest value of every argument and past
 * the last row of every list. Otherwise returns EINVAL. askai parses the
 * JSON of a call strictly, so a JSON integer over 2^64 - 1 is an error of
 * the request and no integer here is cut.
 */
static int
debruijn_plugin_whole (struct json_object   *val,
                       struct debruijn_wide *n)
{
	*n = (struct debruijn_wide){};
	if (!json_object_is_type(val, json_type_string))
		return jb_uint_checked(val, UINT64_MAX, &n->limb[0]);
	int e;
	*n = debruijn_plugin_number(jb_view(val), 0, 0, &e);
	if (e != ERANGE)
		return e ? EINVAL : 0;
	*n = debruijn_plugin_ones(DEBRUIJN_PLUGIN_BITS);
	return 0;
}

/* Reads the argument @p arg of the arguments @p of of a call into @p *n and
 * returns 0, if the argument is a whole number from 0 to @p max, the
 * largest value for the order @p order. Otherwise it ends the call of
 * @p slot as failed with that range, and returns EINVAL.
 */
static int
debruijn_plugin_upto (struct mcp_slot            *slot,
                      struct json_object *const  *of,
                      enum debruijn_plugin_arg    arg,
                      struct debruijn_wide const *max,
                      unsigned                    order,
                      struct debruijn_wide       *n)
{
	if (!debruijn_plugin_whole(of[arg], n) && !debruijn_plugin_over(n, max))
		return 0;
	mcp_slot_tellf(slot, TOOL_FAILED, "The %s argument must be a whole number from 0 to %s for "
	               "order %u.", debruijn_plugin_arg_names[arg].data,
	               debruijn_plugin_decimal(max).text, order);
	return EINVAL;
}

/* Returns the arguments that a call must contain, for an action with the
 * rule @p rule and a call that contains the arguments @p seen: the needs
 * of the rule. If they contain seq, a call that contains addr must
 * contain addr in place of seq, and a call that contains rank must contain
 * order and rank in place of seq.
 */
static uint64_t
debruijn_plugin_rule_needs (struct debruijn_plugin_rule const *rule,
                            uint64_t                           seen)
{
	uint64_t const needs = rule->needs;
	if (!(needs & A(SEQ)) || !(seen & (A(ADDR) | A(RANK))))
		return needs;
	return (needs & ~A(SEQ)) | (seen & A(ADDR) ? A(ADDR) : A(ORDER) | A(RANK));
}

/* Returns the arguments that a call can contain, for an action with the
 * rule @p rule and a call that must contain @p needs: the arguments of
 * @p needs, action and offset, order if @p needs contains seq, and first
 * and limit if the cap of the rule is not 0.
 */
static uint64_t
debruijn_plugin_rule_takes (struct debruijn_plugin_rule const *rule,
                            uint64_t                           needs)
{
	return needs | A(ACTION) | A(OFFSET) | (needs & A(SEQ) ? A(ORDER) : 0) |
	       (rule->cap ? A(FIRST) | A(LIMIT) : 0);
}

/* Returns the definition of debruijn as a new reference, or nullptr if
 * json-c cannot allocate memory. Its _meta.askai.takes lists the arguments
 * that each action takes, for the completion of clients: a set for each
 * form of the sequence of the action.
 */
static struct json_object *
debruijn_plugin_definition (void const *service)
{
	(void)service;
	static_assert(DEBRUIJN_PLUGIN_LIMIT == 64, "the default limit in the description");
	/* A call that contains seq, addr or rank, for each form of a sequence. */
	static uint64_t const seen[] = { A(SEQ), A(ADDR), A(RANK) };
	struct jb_form forms[array_size(seen) * DEBRUIJN_PLUGIN_ACTIONS];
	size_t n = 0;
	for (size_t i = 0; i < DEBRUIJN_PLUGIN_ACTIONS; ++i) {
		struct debruijn_plugin_rule const *const rule = &debruijn_plugin_rules[i];
		/* An action is in one set of arguments, or in one set for each
		 * form if its rule lists seq. */
		size_t const sets = rule->needs & A(SEQ) ? array_size(seen) : 1;
		for (size_t form = 0; form < sets; ++form) {
			uint64_t const needs = debruijn_plugin_rule_needs(rule, seen[form]);
			forms[n++] = (struct jb_form){
				.args   = debruijn_plugin_rule_takes(rule, needs) & ~A(ACTION),
				.action = i,
			};
		}
	}
	struct json_object *takes = jb_takes(debruijn_plugin_action_names, debruijn_plugin_arg_names,
	                                     forms, n);
	struct json_object *def = json_tokener_parse(
		"{\"name\":\"debruijn\",\"title\":\"De Bruijn sequences\","
		"\"description\":\"Binary de Bruijn sequences of the orders 2 to 8. A sequence of "
		"order n is 2^n bits long and starts with n zero bits. Every value of n bits is "
		"in the sequence once, also across the end. An output sequence of order n is 0x "
		"and 2^n/4 hexadecimal digits, such as 0x09af for order 4. The sequence 0x3 of "
		"order 2 is the root of a tree. A sequence of order n from 2 to 7 is the parent "
		"of one sequence of order n+1 for each mask: 2, 8, 128, 32768, 2^31 and 2^63 "
		"children for the orders 2 to 7. The address of a sequence is the masks on the "
		"way from the root, such as 1.7.127.32767. The masks 0 lead to the numerically "
		"first sequence of each order and the largest masks to the last. The rank of a "
		"sequence is the zero-based position among the sequences of the same order, "
		"sorted as unsigned numbers. Pass the sequence of an action as seq, as addr, or "
		"as order and rank. rank, mask, rot and first are whole numbers: JSON integers, "
		"or strings in the notation of seq, which a number from 2^53 on should be and "
		"one from 2^64 on must be. The output is the result alone.\","
		"\"annotations\":{\"readOnlyHint\":true,\"openWorldHint\":false}}");
	/* The schema is a string literal of its own. The translation limits of
	 * C require a compiler to accept a literal of 4095 bytes and no more,
	 * and -Wpedantic warns of a longer one. */
	def = jb_set_or_free(def, "inputSchema", json_tokener_parse(
		"{\"type\":\"object\",\"properties\":{"
		"\"action\":{\"type\":\"string\",\"enum\":["
		XL_JOIN(",", ",", DEBRUIJN_PLUGIN_ACTION_LIST(XL_JSON)) "],\"description\":"
		"\"expand: the child of the sequence for mask. collapse: the parent of the "
		"sequence and the mask of the sequence in the parent, as 0x0f65 127. path: the "
		"sequences on the way from the root to the sequence, without the root, in "
		"hexadecimal without 0x, as 1d.0f65.07dcd629.03f79d71b4cb0a89. address: the "
		"address of the sequence. children: the children of the sequence from the mask "
		"first on, at most limit, one on a line. windows: the subsequence at each index "
		"from 0, one on a line. Index 0 is the least significant bit, and the "
		"subsequence at index i is the value of the bits i to i+order-1, bit i the "
		"lowest, with bit 0 after the top bit. 0x09af: 15, 7, 11, 5 and so on. cycles: "
		"rotates the sequence left by rot bits. The subsequence at an index is the next "
		"index to read. One cycle of indexes on a line, in reading order, each cycle "
		"from the smallest index in no earlier line. 0x09af with rot 0: 0 15 14 12, "
		"then 1 7 3 5 13 8 9 4 10 2 11, then 6. "
		"rank: the rank of the sequence, in decimal. unrank: the sequence at rank in "
		"order, a space and the address of the sequence in decimal (the sequence alone "
		"for order 2). ordered: the same rows in ascending order from rank first on, at "
		"most limit rows.\"},"
		"\"seq\":{\"type\":\"string\",\"description\":\"The sequence as a number of up "
		"to 256 bits, in the notation that strtoll() reads with the base 0 but without "
		"a + sign: hexadecimal after 0x, such as 0x09af, binary after 0b, octal after "
		"0, else decimal. A negative number needs order and stands for the two's "
		"complement of the number in 2^order bits. seq can be a rotation of a sequence, "
		"such as 0x9af0 for 0x09af. windows and cycles read the rotation unchanged. The "
		"other actions first rotate seq to start with the zero bits.\"},"
		"\"order\":{\"type\":\"integer\",\"minimum\":2,\"maximum\":8,\"description\":"
		"\"The order: 2 to 7 for expand and children, 3 to 8 for collapse, path and "
		"address, 2 to 8 for the other actions. With seq, order is optional: a sequence "
		"of order n in any rotation is 2^n - n to 2^n bits wide, and the default order "
		"is the smallest n with 2^n at least the bit width of seq.\"},"
		"\"addr\":{\"type\":\"string\",\"description\":\"The sequence as an address, in "
		"place of seq: 1 to 6 masks with a dot between two, each a number as for seq. A "
		"prefix on the first mask is the base of all masks, as in "
		"0b1.110.1010101.111110000011111. The masks are 1, 3, 7, 15, 31 and 63 bits "
		"wide, in this order. A negative mask stands for the two's complement of the "
		"number in the bits of the mask, so -1 is the largest mask. The root 0x3 is no "
		"address: pass it as seq, or as order 2 and rank 0.\"},"
		"\"rank\":{\"type\":[\"integer\",\"string\"],\"description\":\"The sequence as a "
		"rank in order, in place of seq: less than 1, 2, 16, 2048, 67108864, 2^57 or "
		"2^120 for the orders 2 to 8.\"},"
		"\"mask\":{\"type\":[\"integer\",\"string\"],\"description\":\"For expand: at "
		"most 1, 7, 127, 32767, 2^31 - 1 and 2^63 - 1 for the orders 2 to 7.\"},"
		"\"rot\":{\"type\":[\"integer\",\"string\"],\"description\":\"For cycles: the "
		"number of bits to rotate the sequence left, from 0 to 2^order - 1.\"},"
		"\"first\":{\"type\":[\"integer\",\"string\"],\"default\":0,\"description\":"
		"\"For children: the first mask. For ordered: the first rank (empty past end).\"},"
		"\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32768,\"default\":64,"
		"\"description\":\"Maximum rows: up to 256 for ordered. For children up to "
		"32768 for the orders 2 to 5, 8192 for order 6 and 4096 for order 7.\"},"
		"\"offset\":{\"type\":\"integer\",\"minimum\":0,\"description\":\""
		TOOL_OFFSET_ABOUT "\"}},"
		"\"required\":[\"action\"],\"additionalProperties\":false}"));
	struct json_object *meta = jb_set_or_free(json_object_new_object(), "takes", takes);
	return jb_set_or_free(def, "_meta", jb_set_or_free(json_object_new_object(), "askai", meta));
}

/* Starts a call of debruijn in @p slot with the arguments @p args: checks
 * the arguments of the action, reads the sequence from the form of it in
 * the call, and runs the action, which ends the call.
 */
static void
debruijn_plugin_start (struct mcp_slot    *slot,
                       struct json_object *args)
{
	if (!slot)
		return;
	struct json_object *of[DEBRUIJN_PLUGIN_ARGS] = {};
	uint64_t seen = 0;
	struct cstr_view other = {};
	if (args && !json_object_is_type(args, json_type_object)) {
		mcp_slot_tell(slot, CSTR_VIEW("The arguments must be a JSON object."), TOOL_FAILED);
		return;
	}
	if (jb_args(args, debruijn_plugin_arg_names, DEBRUIJN_PLUGIN_ARGS, UINT64_MAX, of, &seen,
	            &other)) {
		mcp_slot_tellf(slot, TOOL_FAILED, "%.*s is not an argument of the tool.",
		               (int)other.len, other.data);
		return;
	}
	size_t const action = cstr_view_index(debruijn_plugin_action_names, DEBRUIJN_PLUGIN_ACTIONS,
	                                      jb_view(of[DEBRUIJN_PLUGIN_ARG_ACTION]));
	if (action == DEBRUIJN_PLUGIN_ACTIONS) {
		static struct cstr_view const actions = CSTR_VIEW_INIT(
			"The action argument must be one of: "
			XL_JOIN(", ", ", ", DEBRUIJN_PLUGIN_ACTION_LIST(XL_TEXT)) ".");
		mcp_slot_tell(slot, actions, TOOL_FAILED);
		return;
	}
	struct debruijn_plugin_rule const *const rule = &debruijn_plugin_rules[action];
	char const *const name = debruijn_plugin_action_names[action].data;
	if ((rule->needs & A(SEQ)) && stdc_count_ones(seen & (A(SEQ) | A(ADDR) | A(RANK))) > 1) {
		mcp_slot_tellf(slot, TOOL_FAILED, "%s takes one sequence: seq, or addr, or order "
		               "and rank.", name);
		return;
	}
	uint64_t const needs = debruijn_plugin_rule_needs(rule, seen);
	uint64_t const takes = debruijn_plugin_rule_takes(rule, needs);
	uint64_t const wrong = (needs & ~seen) | (seen & ~takes);
	if (wrong) {
		unsigned const arg = stdc_trailing_zeros(wrong);
		mcp_slot_tellf(slot, TOOL_FAILED, needs >> arg & 1 ? "%s needs the argument %s."
		                                                    : "%s takes no argument %s.",
		               name, debruijn_plugin_arg_names[arg].data);
		return;
	}
	/* seq is the sequence in the rotation of the argument seq, and normal
	 * is seq rotated to start with order zero bits, as the library
	 * requires. */
	struct debruijn_wide seq = {};
	struct debruijn_wide normal = {};
	struct debruijn_wide number = {};
	struct debruijn_wide first = {};
	unsigned order = 0;
	unsigned limit = DEBRUIJN_PLUGIN_LIMIT;
	bool const given = seen & A(ORDER);
	if (given && (debruijn_plugin_uint(of[DEBRUIJN_PLUGIN_ARG_ORDER], rule->high, &order) ||
	              order < rule->low)) {
		mcp_slot_tellf(slot, TOOL_FAILED, "The order argument must be a whole number from "
		               "%u to %u for %s.", rule->low, rule->high, name);
		return;
	}
	if (needs & A(ADDR)) {
		/* The masks of addr lead from the root to the sequence. */
		uint64_t masks[DEBRUIJN_PLUGIN_DEPTH];
		unsigned n;
		int e = debruijn_plugin_address(jb_view(of[DEBRUIJN_PLUGIN_ARG_ADDR]), masks, &n);
		normal.limb[0] = DEBRUIJN_ROOT_SEED;
		for (unsigned i = 0; !e && i < n; ++i)
			e = debruijn_plugin_child(&normal, &normal, DEBRUIJN_MIN_ORDER + i, masks[i]);
		if (e) {
			static struct cstr_view const text = CSTR_VIEW_INIT(
				"The addr argument must be 1 to 6 masks with a dot between two, such "
				"as 1.7.127.32767. The masks are at most 1, 7, 127, 32767, 2147483647 "
				"and 9223372036854775807, in this order.");
			mcp_slot_tell(slot, text, TOOL_FAILED);
			return;
		}
		order = DEBRUIJN_MIN_ORDER + n;
		seq = normal;
	} else if (needs & A(RANK)) {
		/* The call contains order, which is from low to high of the rule. */
		struct debruijn_wide const last = debruijn_plugin_ranks(order);
		if (debruijn_plugin_upto(slot, of, DEBRUIJN_PLUGIN_ARG_RANK, &last, order, &number))
			return;
		int const e = debruijn_plugin_ordered(order, &number, 1, debruijn_plugin_store,
		                                      &normal);
		if (e) {
			mcp_slot_fail(slot, e);
			return;
		}
		seq = normal;
	} else if (needs & A(SEQ)) {
		/* A sequence of order n is 2^n bits long. Without order, the
		 * place of the sign bit of a negative number is unknown. */
		int e;
		seq = debruijn_plugin_number(jb_view(of[DEBRUIJN_PLUGIN_ARG_SEQ]), 0,
		                             given ? 1U << order : 0, &e);
		if (e) {
			mcp_slot_tell(slot, debruijn_plugin_no_number(e), TOOL_FAILED);
			return;
		}
		if (!given)
			order = debruijn_plugin_order(&seq);
		normal = seq;
		if (order < DEBRUIJN_MIN_ORDER || debruijn_plugin_normalize(&normal, order)) {
			debruijn_plugin_no_sequence(slot, &seq, order);
			return;
		}
	}
	/* The order was computed from seq or addr, and can be outside the
	 * orders of the action. */
	if (!given && (order < rule->low || order > rule->high)) {
		mcp_slot_tellf(slot, TOOL_FAILED, "0x%s is a sequence of order %u, and %s takes the "
		               "orders %u to %u.", debruijn_plugin_hex(&seq, order).text, order, name,
		               rule->low, rule->high);
		return;
	}
	/* The number of an action: the mask of expand or the rot of cycles. */
	if (needs & A(MASK)) {
		struct debruijn_wide const max = { { debruijn_plugin_masks(order) } };
		if (debruijn_plugin_upto(slot, of, DEBRUIJN_PLUGIN_ARG_MASK, &max, order, &number))
			return;
	}
	if (needs & A(ROT)) {
		struct debruijn_wide const max = { { (1U << order) - 1U } };
		if (debruijn_plugin_upto(slot, of, DEBRUIJN_PLUGIN_ARG_ROT, &max, order, &number))
			return;
	}
	/* first can be any whole number: a first past the last row is no
	 * error. */
	unsigned const cap = debruijn_plugin_cap(action, order);
	if (((seen & A(FIRST)) && debruijn_plugin_whole(of[DEBRUIJN_PLUGIN_ARG_FIRST], &first)) ||
	    ((seen & A(LIMIT)) && (debruijn_plugin_uint(of[DEBRUIJN_PLUGIN_ARG_LIMIT], cap,
	                                                &limit) || !limit))) {
		mcp_slot_tellf(slot, TOOL_FAILED, "first must be a whole number from 0, and limit a "
		               "whole number from 1 to %u.", cap);
		return;
	}
	if (mcp_slot_offset(slot, args))
		return;
	struct debruijn_plugin_out out = { .slot = slot };
	switch ((enum debruijn_plugin_action)action) {
	case DEBRUIJN_PLUGIN_EXPAND:
		debruijn_plugin_expand(&out, &normal, order, number.limb[0]);
		break;
	case DEBRUIJN_PLUGIN_COLLAPSE:
		debruijn_plugin_collapse(&out, &normal, order);
		break;
	case DEBRUIJN_PLUGIN_PATH:
		debruijn_plugin_path(&out, &normal, order);
		break;
	case DEBRUIJN_PLUGIN_ADDRESS:
		if (debruijn_plugin_locate(&out, &normal, order))
			debruijn_plugin_no_sequence(slot, &normal, order);
		else
			debruijn_plugin_end(&out);
		break;
	case DEBRUIJN_PLUGIN_CHILDREN:
		/* A mask is under 2^63, so a first of more than 64 bits is past
		 * the last mask, as UINT64_MAX is. */
		if (debruijn_plugin_width(&first) > 64U)
			first.limb[0] = UINT64_MAX;
		debruijn_plugin_children(&out, &normal, order, first.limb[0], limit);
		break;
	case DEBRUIJN_PLUGIN_WINDOWS:
		debruijn_plugin_windows(&out, &seq, order);
		break;
	case DEBRUIJN_PLUGIN_CYCLES:
		debruijn_plugin_cycles(&out, &seq, order, (unsigned)number.limb[0]);
		break;
	case DEBRUIJN_PLUGIN_RANK:
		/* The rank of a sequence that the call contains as a rank is
		 * that argument, which no later argument replaced in number. */
		if (needs & A(RANK)) {
			debruijn_plugin_line(&out, "%s\n", debruijn_plugin_decimal(&number).text);
			debruijn_plugin_end(&out);
		} else {
			debruijn_plugin_rank(&out, &normal, order);
		}
		break;
	case DEBRUIJN_PLUGIN_UNRANK:
		out.e = debruijn_plugin_row(&normal, order, &out);
		debruijn_plugin_end(&out);
		break;
	case DEBRUIJN_PLUGIN_ORDERED:
		out.e = debruijn_plugin_ordered(order, &first, limit, debruijn_plugin_row, &out);
		debruijn_plugin_end(&out);
		break;
	case DEBRUIJN_PLUGIN_ACTIONS:
		break;
	}
}

#undef A

struct plugin const askai_plugin = {
	.tools = {
		{
			.definition = debruijn_plugin_definition,
			.start = debruijn_plugin_start,
			.flags = TOOL_LENT | TOOL_PAGED,
		},
	},
};
