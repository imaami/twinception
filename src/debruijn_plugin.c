/* debruijn_plugin.c - a plugin with one tool, debruijn, that explores the
 * tree of the binary de Bruijn sequences of the orders 2 to 6. The library
 * (src/debruijn.h) calls a sequence a word.
 *
 * A sequence of order n has 2^n bits, and every value of n bits is in the
 * sequence once, also across the end of the sequence to its start. The
 * sequences of debruijn.h start with n zero bits. The sequence 0x3 of order
 * 2 is the root of the tree. A sequence of order n from 2 to 5 has one
 * child of order n + 1 for each mask from 0 to 2^(2^(n-1)-1) - 1
 * (debruijn_expand()), and a sequence of order 3 to 6 is the child of
 * exactly one sequence (debruijn_collapse()). The address of a sequence is
 * the masks on the way from the root to the sequence, such as
 * 1.7.127.32767. The masks 0 lead to the numerically first sequence of each
 * order, and the largest masks to the last. The rank of a sequence is its
 * position among the sequences of its order, sorted as unsigned numbers
 * and counted from 0 (debruijn_rank()).
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
 * - windows: for each place in a sequence, the value of the order bits that
 *   start there, one on a line.
 * - rank: the rank of a sequence.
 * - unrank: the sequence at a rank of an order, and its address, as
 *   0x021d16a7b37e4ae3 0.1.100.10548.
 * - ordered: rows as for unrank, from rank first on, at most limit rows.
 *
 * A call gives the sequence of an action in one of three ways
 * (debruijn_plugin_needs()): the argument seq is the sequence as a number,
 * the argument addr is its address, and the arguments order and rank are
 * its rank in the order. unrank takes order and rank only, and ordered
 * takes no sequence.
 *
 * seq, and each mask of addr, is a number as strtoll() and strtoull() read
 * it with the base 0, but without a + sign (debruijn_plugin_number()):
 * hexadecimal after 0x, binary after 0b, octal after 0, else decimal. A
 * prefix on the leftmost mask of an address is the base of all its masks,
 * so 0b1.110.1010101.111110000011111 is binary. A negative number stands
 * for its two's complement in the bits of the sequence or mask: 2^n bits
 * for a sequence of order n, which then needs the argument order, and
 * 2^(n-1) - 1 bits for a mask that expands a sequence of order n. The
 * sequences in the output are hexadecimal.
 *
 * seq can be a sequence in any rotation. An action rotates it to start
 * with its n zero bits (debruijn_normalize()) before the library reads it;
 * windows reads seq as it is given. Without the argument order, the bit
 * width of seq gives the order: a rotation of a sequence of order n is
 * 2^n - n to 2^n bits wide.
 *
 * The plugin has no options, no instances and no data of its own, and a
 * call ends before its start() returns. check_debruijn (tests/askai.py)
 * runs it.
 */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbit.h>
#include <stdint.h>
#include <stdlib.h>

#include <json-c/json.h>

#include "compat.h"
#include "debruijn.h"
#include "jb.h"
#include "plugin.h"
#include "util.h"
#include "xlist.h"

PLUGIN_MANIFEST(.name = "debruijn", .tools = { "debruijn" });

enum {
	DEBRUIJN_PLUGIN_LIMIT = 64,     /* the default of the argument limit */
	DEBRUIJN_PLUGIN_LINE  = 160,    /* bytes of a line of the output, at most */
	DEBRUIJN_PLUGIN_DEPTH = DEBRUIJN_MAX_ORDER - DEBRUIJN_MIN_ORDER,
	                                /* masks in an address, at most */
};

/* The arguments of the tool: X(ID, name). */
#define DEBRUIJN_PLUGIN_ARG_LIST(X) \
        X(ACTION, "action")         \
        X(SEQ,    "seq")            \
        X(ORDER,  "order")          \
        X(ADDR,   "addr")           \
        X(RANK,   "rank")           \
        X(MASK,   "mask")           \
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

/* The actions of the tool: X(ID, name, needs, low, high, cap). needs is the
 * arguments that the action needs. An action that needs seq takes the
 * sequence in each of the three ways (debruijn_plugin_needs()), and order
 * with seq. The sequence of the action is of an order from low to high:
 * expand and children make a child, and collapse, path and address find a
 * parent. An action with a cap takes first and limit, and cap is the
 * largest limit. Every action takes offset.
 */
#define DEBRUIJN_PLUGIN_ACTION_LIST(X)                             \
        X(EXPAND,   "expand",   A(SEQ) | A(MASK),   2, 5, 0)       \
        X(COLLAPSE, "collapse", A(SEQ),             3, 6, 0)       \
        X(PATH,     "path",     A(SEQ),             3, 6, 0)       \
        X(ADDRESS,  "address",  A(SEQ),             3, 6, 0)       \
        X(CHILDREN, "children", A(SEQ),             2, 5, 32768)   \
        X(WINDOWS,  "windows",  A(SEQ),             2, 6, 0)       \
        X(RANK,     "rank",     A(SEQ),             2, 6, 0)       \
        X(UNRANK,   "unrank",   A(ORDER) | A(RANK), 2, 6, 0)       \
        X(ORDERED,  "ordered",  A(ORDER),           2, 6, 256)
static_assert(DEBRUIJN_MIN_ORDER == 2 && DEBRUIJN_MAX_ORDER == 6, "the orders in the list");

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
	uint16_t cap;
	uint8_t  low;
	uint8_t  high;
};

#define DEBRUIJN_PLUGIN_ACTION_RULE(id, name, needs_, low_, high_, cap_) \
        { .needs = needs_, .cap = cap_, .low = low_, .high = high_ },
static struct debruijn_plugin_rule const debruijn_plugin_rules[] = {
	DEBRUIJN_PLUGIN_ACTION_LIST(DEBRUIJN_PLUGIN_ACTION_RULE)
};
#undef DEBRUIJN_PLUGIN_ACTION_RULE

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

/* Returns the number of hexadecimal digits of a sequence of the order
 * @p order: a digit for each 4 of its 2^order bits.
 */
static int
debruijn_plugin_digits (unsigned order)
{
	return (int)(1U << order >> 2U);
}

/* Returns the largest mask of a sequence of the order @p order from 2 to
 * 5: a sequence has 2^(order-1) - 1 choice bits.
 */
static unsigned
debruijn_plugin_masks (unsigned order)
{
	return (1U << ((1U << (order - 1U)) - 1U)) - 1U;
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

/* Returns the base that the prefix of the number at the start of @p text
 * gives, after white space and a minus sign: 16 for 0x, 2 for 0b and 8 for
 * a 0 before a digit. Returns 0 for a number without a prefix, such as 0.
 */
static int
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

/* Returns the number @p text as strtoll() reads it in the base @p base, or
 * as strtoull() reads it if it is over INT64_MAX. White space can precede
 * the number, but a + sign cannot. The base 0 takes the base from the
 * prefix: hexadecimal after 0x, binary after 0b, octal after 0, else
 * decimal. A negative number stands for its two's complement in @p bits
 * bits, from 1 to 64. If it does not fit in them, it keeps all 64 bits of
 * its two's complement, which makes it wider than @p bits bits.
 *
 * Stores 0 in @p *e, or EDOM for a negative number if @p bits is 0, ERANGE
 * for a number that 64 bits cannot hold, or EINVAL if @p text is not one
 * number from its start to its end. strtoll() reads up to the first byte
 * that cannot be in a number, which can be after @p text: the 0 that json-c
 * writes after a string, or the dot after a mask. A number that goes on
 * after @p text is refused.
 */
static uint64_t
debruijn_plugin_number (struct cstr_view  text,
                        int               base,
                        unsigned          bits,
                        int              *e)
{
	/* strtoll() would take a + sign after the white space. */
	text = debruijn_plugin_space(text);
	if (!text.len || *text.data == '+') {
		*e = EINVAL;
		return 0;
	}
	char *end;
	/* strtoll() and strtoull() report a range error only in errno. */
	errno = 0;
	int64_t const i64 = _Generic(i64, long: strtol, long long: strtoll)(text.data, &end, base);
	uint64_t u64 = (uint64_t)i64;
	if (errno == ERANGE && i64 == INT64_MAX) {
		errno = 0;
		u64 = _Generic(u64, unsigned long: strtoul,
		                    unsigned long long: strtoull)(text.data, &end, base);
	}
	*e = errno;
	if (!*e && end != text.data + text.len)
		*e = EINVAL;
	if (*e || i64 >= 0)
		return u64;
	/* The sign bit is bit bits - 1, and a negative number fits if all
	 * the bits from it up are set. */
	if (!bits)
		*e = EDOM;
	else if (!(~u64 >> (bits - 1U)))
		u64 &= UINT64_MAX >> (64U - bits);
	return u64;
}

/* Reads the address @p text, 1 to 4 masks with a dot between two, each a
 * number as debruijn_plugin_number() reads it, into @p masks. A prefix on
 * the leftmost mask is the base of all the masks; without one, each mask
 * takes the base from its own prefix. Stores the number of masks in @p *n.
 * Returns 0 or EINVAL.
 */
static int
debruijn_plugin_address (struct cstr_view  text,
                         unsigned          masks[static DEBRUIJN_PLUGIN_DEPTH],
                         size_t           *n)
{
	int const base = debruijn_plugin_base(text);
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
		/* Mask n expands a sequence of order n + 2, which has
		 * 2^(n+1) - 1 choice bits. */
		int e;
		uint64_t const value = debruijn_plugin_number(part, *n ? base : 0,
		                                              (1U << (*n + 1U)) - 1U, &e);
		if (e || value > UINT32_MAX)
			return EINVAL;
		masks[(*n)++] = (unsigned)value;
	}
	return 0;
}

/* Returns the smallest order n from 2 to 6 with 2^n at least the bit width
 * of @p seq, or 0 for a sequence under 2 bits wide. A rotation of a
 * sequence of order n is 2^n - n to 2^n bits wide: the n zero bits of the
 * sequence can be at its top, in whole or in part, or not.
 */
static unsigned
debruijn_plugin_order (uint64_t seq)
{
	return stdc_bit_width(
		stdc_bit_width(seq)
		- (seq >= 1U << 2U)
		- (seq == 1U << 1U)
	);
}

/* Refuses the number @p seq, which is no rotation of a sequence of the
 * order @p order, or of any order if @p order is 0.
 */
static void
debruijn_plugin_no_sequence (struct mcp_slot *slot,
                             uint64_t         seq,
                             unsigned         order)
{
	if (order >= DEBRUIJN_MIN_ORDER)
		mcp_slot_tellf(slot, TOOL_FAILED, "0x%0*" PRIx64 " is no sequence of order %u.",
		               debruijn_plugin_digits(order), seq, order);
	else
		mcp_slot_tellf(slot, TOOL_FAILED, "0x%" PRIx64 " is no sequence of any order from "
		               "%u to %u.", seq, DEBRUIJN_MIN_ORDER, DEBRUIJN_MAX_ORDER);
}

/* expand: the child of @p seq for @p mask. */
static void
debruijn_plugin_expand (struct debruijn_plugin_out *out,
                        uint64_t                    seq,
                        unsigned                    order,
                        unsigned                    mask)
{
	uint64_t child;
	if (debruijn_expand(&child, seq, order, mask)) {
		debruijn_plugin_no_sequence(out->slot, seq, order);
		return;
	}
	debruijn_plugin_line(out, "0x%0*" PRIx64 "\n", debruijn_plugin_digits(order + 1U), child);
	debruijn_plugin_end(out);
}

/* collapse: the parent of @p seq and the mask of @p seq in the parent. */
static void
debruijn_plugin_collapse (struct debruijn_plugin_out *out,
                          uint64_t                    seq,
                          unsigned                    order)
{
	uint64_t parent;
	unsigned mask;
	if (debruijn_collapse(&parent, &mask, seq, order)) {
		debruijn_plugin_no_sequence(out->slot, seq, order);
		return;
	}
	debruijn_plugin_line(out, "0x%0*" PRIx64 " %u\n", debruijn_plugin_digits(order - 1U),
	                     parent, mask);
	debruijn_plugin_end(out);
}

/* rank: the rank of @p seq. */
static void
debruijn_plugin_rank (struct debruijn_plugin_out *out, uint64_t seq, unsigned order)
{
	unsigned rank;
	if (debruijn_rank(&rank, seq, order)) {
		debruijn_plugin_no_sequence(out->slot, seq, order);
		return;
	}
	debruijn_plugin_line(out, "%u\n", rank);
	debruijn_plugin_end(out);
}

/* Collapses @p seq of the order @p order, from 2 to 6, down to the root.
 * Stores the sequences on the way in @p seqs, seqs[0] the root and
 * seqs[order - 2] @p seq, and in @p masks[i] the mask of seqs[i + 1] in
 * seqs[i]. Returns 0 or the error of debruijn_collapse().
 */
static int
debruijn_plugin_ancestors (uint64_t seq,
                           unsigned order,
                           uint64_t seqs[static DEBRUIJN_PLUGIN_DEPTH + 1],
                           unsigned masks[static DEBRUIJN_PLUGIN_DEPTH])
{
	unsigned i = order - DEBRUIJN_MIN_ORDER;
	for (seqs[i] = seq; i; --i) {
		int const e = debruijn_collapse(&seqs[i - 1], &masks[i - 1], seqs[i],
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
                      uint64_t                    seq,
                      unsigned                    order)
{
	uint64_t seqs[DEBRUIJN_PLUGIN_DEPTH + 1];
	unsigned masks[DEBRUIJN_PLUGIN_DEPTH];
	if (debruijn_plugin_ancestors(seq, order, seqs, masks)) {
		debruijn_plugin_no_sequence(out->slot, seq, order);
		return;
	}
	for (unsigned i = DEBRUIJN_MIN_ORDER + 1; i <= order; ++i)
		debruijn_plugin_line(out, "%0*" PRIx64 "%c", debruijn_plugin_digits(i),
		                     seqs[i - DEBRUIJN_MIN_ORDER], i < order ? '.' : '\n');
	debruijn_plugin_end(out);
}

/* Appends the address of @p seq of the order @p order and a newline to the
 * output @p out, and nothing for the root, which has no masks. Returns 0
 * or the error of debruijn_collapse().
 */
static int
debruijn_plugin_locate (struct debruijn_plugin_out *out,
                        uint64_t                    seq,
                        unsigned                    order)
{
	uint64_t seqs[DEBRUIJN_PLUGIN_DEPTH + 1];
	unsigned masks[DEBRUIJN_PLUGIN_DEPTH];
	int const e = debruijn_plugin_ancestors(seq, order, seqs, masks);
	if (e)
		return e;
	unsigned const n = order - DEBRUIJN_MIN_ORDER;
	for (unsigned i = 0; i < n; ++i)
		debruijn_plugin_line(out, "%u%c", masks[i], i + 1 < n ? '.' : '\n');
	return 0;
}

/* Appends a row of unrank and ordered to the output @p arg: @p seq, a space
 * and the address of @p seq, or the root alone. Returns 0, the error of
 * debruijn_collapse() or the error of the output.
 */
static int
debruijn_plugin_emit (uint64_t  seq,
                      unsigned  order,
                      void     *arg)
{
	struct debruijn_plugin_out *out = arg;
	debruijn_plugin_line(out, "0x%0*" PRIx64 "%c", debruijn_plugin_digits(order), seq,
	                     order == DEBRUIJN_MIN_ORDER ? '\n' : ' ');
	int e = debruijn_plugin_locate(out, seq, order);
	return e ? e : out->e;
}

/* Stores @p seq in the uint64_t at @p arg: the sequence at a rank, as
 * debruijn_ordered() emits it.
 */
static int
debruijn_plugin_store (uint64_t  seq,
                       unsigned  order,
                       void     *arg)
{
	(void)order;
	*(uint64_t *)arg = seq;
	return 0;
}

/* children: the children of @p seq from the mask @p first on, at most
 * @p limit of them.
 */
static void
debruijn_plugin_children (struct debruijn_plugin_out *out,
                          uint64_t                    seq,
                          unsigned                    order,
                          uint64_t                    first,
                          uint64_t                    limit)
{
	struct debruijn_generator const seed = debruijn_generator(seq, order);
	if (seed.error) {
		debruijn_plugin_no_sequence(out->slot, seq, order);
		return;
	}
	uint64_t const all = (uint64_t)debruijn_plugin_masks(order) + 1;
	uint64_t const end = first < all && limit < all - first ? first + limit : all;
	for (uint64_t mask = first; !out->e && mask < end; ++mask) {
		uint64_t child = 0;
		out->e = debruijn_generate(&child, &seed, (unsigned)mask);
		debruijn_plugin_line(out, "0x%0*" PRIx64 "\n", debruijn_plugin_digits(order + 1U),
		                     child);
	}
	debruijn_plugin_end(out);
}

/* windows: for each place in @p seq, the value of the @p order bits that
 * start there. The sequence can be in any rotation. The bits after the end
 * of the sequence are the bits at its start.
 */
static void
debruijn_plugin_windows (struct debruijn_plugin_out *out,
                         uint64_t                    seq,
                         unsigned                    order)
{
	/* Place 0 is the most significant bit. A window that runs over the end
	 * of the sequence reads on at its start: the bit positions count down
	 * modulo the 2^order bits. */
	unsigned const bits = 1U << order;
	for (unsigned place = 0; place < bits; ++place) {
		unsigned const top = bits - 1U - place;
		unsigned value = 0;
		for (unsigned i = 0; i < order; ++i)
			value = value << 1 | (unsigned)(seq >> ((top - i) & (bits - 1U)) & 1U);
		debruijn_plugin_line(out, "%u\n", value);
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

/* Reads the argument @p arg of the arguments @p of of a call into @p *n and
 * returns 0, if the argument is a whole number from 0 to @p max, the
 * largest value for the order @p order. Otherwise it ends the call of
 * @p slot as failed with that range, and returns EINVAL.
 */
static int
debruijn_plugin_upto (struct mcp_slot           *slot,
                      struct json_object *const *of,
                      enum debruijn_plugin_arg   arg,
                      unsigned                   max,
                      unsigned                   order,
                      unsigned                  *n)
{
	int const e = debruijn_plugin_uint(of[arg], max, n);
	if (e)
		mcp_slot_tellf(slot, TOOL_FAILED, "The %s argument must be a whole number from 0 "
		               "to %u for order %u.", debruijn_plugin_arg_names[arg].data, max, order);
	return e;
}

/* Returns the arguments that a call needs whose action needs @p needs, if
 * the call gives the arguments @p seen. A call gives a sequence as seq, or
 * in place of seq as addr, or as order and rank.
 */
static uint64_t
debruijn_plugin_needs (uint64_t needs,
                       uint64_t seen)
{
	if (!(needs & A(SEQ)) || !(seen & (A(ADDR) | A(RANK))))
		return needs;
	return (needs & ~A(SEQ)) | (seen & A(ADDR) ? A(ADDR) : A(ORDER) | A(RANK));
}

/* Returns the arguments that a call takes whose action has the rule
 * @p rule and which needs the arguments @p needs: those, action and
 * offset, order with seq, and first and limit if the rule has a cap.
 */
static uint64_t
debruijn_plugin_takes (struct debruijn_plugin_rule const *rule,
                       uint64_t                           needs)
{
	return needs | A(ACTION) | A(OFFSET) | (needs & A(SEQ) ? A(ORDER) : 0) |
	       (rule->cap ? A(FIRST) | A(LIMIT) : 0);
}

/* Returns the definition of debruijn as a new reference, or nullptr if
 * json-c cannot allocate memory. Its _meta.askai.takes lists the arguments
 * that each action takes, for the completion of clients: a set for each
 * way to give the sequence of the action.
 */
static struct json_object *
debruijn_plugin_definition (void const *service)
{
	(void)service;
	static_assert(DEBRUIJN_PLUGIN_LIMIT == 64, "the description says");
	/* The three ways to give a sequence: a call with seq, addr or rank. */
	static uint64_t const ways[] = { A(SEQ), A(ADDR), A(RANK) };
	struct jb_form forms[array_size(ways) * DEBRUIJN_PLUGIN_ACTIONS];
	size_t n = 0;
	for (size_t i = 0; i < DEBRUIJN_PLUGIN_ACTIONS; ++i) {
		struct debruijn_plugin_rule const *const rule = &debruijn_plugin_rules[i];
		/* An action that needs no seq has one set of arguments. */
		size_t const sets = rule->needs & A(SEQ) ? array_size(ways) : 1;
		for (size_t way = 0; way < sets; ++way) {
			uint64_t const needs = debruijn_plugin_needs(rule->needs, ways[way]);
			forms[n++] = (struct jb_form){
				.args   = debruijn_plugin_takes(rule, needs) & ~A(ACTION),
				.action = i,
			};
		}
	}
	struct json_object *takes = jb_takes(debruijn_plugin_action_names, debruijn_plugin_arg_names,
	                                     forms, n);
	struct json_object *def = json_tokener_parse(
		"{\"name\":\"debruijn\",\"title\":\"De Bruijn sequences\","
		"\"description\":\"Binary de Bruijn sequences of the orders 2 to 6. A sequence of "
		"order n has 2^n bits and starts with n zero bits. Every value of n bits is in "
		"the sequence once, also across the end. An output sequence of order n is 0x and "
		"2^n/4 hexadecimal digits, such as 0x09af for order 4. The sequence 0x3 of order "
		"2 is the root of a tree. A sequence of order n from 2 to 5 has a child of order "
		"n+1 for each mask: 2, 8, 128 and 32768 children for the orders 2, 3, 4 and 5. "
		"The address of a sequence is the masks on the way from the root, such as "
		"1.7.127.32767. The masks 0 lead to the numerically first sequence of each order "
		"and the largest masks to the last. The rank of a sequence is the zero-based "
		"position among the sequences of the same order, sorted as unsigned numbers. A "
		"call gives the sequence of an action as seq, as addr, or as order and rank. The "
		"output is the result alone.\","
		"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		"\"action\":{\"type\":\"string\",\"enum\":["
		XL_JOIN(",", ",", DEBRUIJN_PLUGIN_ACTION_LIST(XL_JSON)) "],\"description\":"
		"\"expand: the child of the sequence for mask. collapse: the parent of the "
		"sequence and the mask of the sequence in the parent, as 0x0f65 127. path: the "
		"sequences on the way from the root to the sequence, without the root, in "
		"hexadecimal without 0x, as 1d.0f65.07dcd629.03f79d71b4cb0a89. address: the "
		"address of the sequence. children: the children of the sequence from the mask "
		"first on, at most limit, one on a line. windows: for each place in the sequence "
		"from the most significant bit, the value of the order bits that start there, "
		"one on a line. rank: the rank of the sequence. unrank: the sequence at rank in "
		"order, a space and the address of the sequence in decimal (the sequence alone "
		"for order 2). ordered: the same rows in ascending order from rank first on, at "
		"most limit rows.\"},"
		"\"seq\":{\"type\":\"string\",\"description\":\"The sequence as a number, as "
		"strtoll() reads a number with the base 0 but without a + sign: hexadecimal "
		"after 0x, such as 0x09af, binary after 0b, octal after 0, else decimal. A "
		"negative number needs order and stands for the two's complement of the number "
		"in 2^order bits. seq can be a rotation of a sequence, such as 0x9af0 for "
		"0x09af: windows reads the rotation as given, and the other actions read the "
		"sequence.\"},"
		"\"order\":{\"type\":\"integer\",\"minimum\":2,\"maximum\":6,\"description\":"
		"\"The order: 2 to 5 for expand and children, 3 to 6 for collapse, path and "
		"address, 2 to 6 for the other actions. With seq, order is optional: a sequence "
		"of order n in any rotation is 2^n - n to 2^n bits wide, and the bit width of "
		"seq gives the order.\"},"
		"\"addr\":{\"type\":\"string\",\"description\":\"The sequence as an address, in "
		"place of seq: 1 to 4 masks with a dot between two, each a number as for seq. A "
		"prefix on the first mask gives the base of all masks, as in "
		"0b1.110.1010101.111110000011111. The masks have 1, 3, 7 and 15 bits, in this "
		"order. A negative mask stands for the two's complement of the number in the "
		"bits of the mask, so -1 is the largest mask.\"},"
		"\"rank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":67108863,\"description\":"
		"\"The sequence as a rank in order, in place of seq: less than 1, 2, 16, 2048 or "
		"67108864 for the orders 2 to 6.\"},"
		"\"mask\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"For expand: "
		"at most 1, 7, 127 and 32767 for the orders 2, 3, 4 and 5.\"},"
		"\"first\":{\"type\":\"integer\",\"minimum\":0,\"default\":0,\"description\":"
		"\"For children: the first mask. For ordered: the first rank (empty past end).\"},"
		"\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32768,\"default\":64,"
		"\"description\":\"Maximum rows: up to 32768 for children or 256 for ordered.\"},"
		"\"offset\":{\"type\":\"integer\",\"minimum\":0,\"description\":\""
		TOOL_OFFSET_ABOUT "\"}},"
		"\"required\":[\"action\"],\"additionalProperties\":false},"
		"\"annotations\":{\"readOnlyHint\":true,\"openWorldHint\":false}}");
	struct json_object *meta = jb_set_or_free(json_object_new_object(), "takes", takes);
	return jb_set_or_free(def, "_meta", jb_set_or_free(json_object_new_object(), "askai", meta));
}

/* Starts a call of debruijn in @p slot with the arguments @p args: checks
 * the arguments of the action, reads the sequence in the way that the call
 * gives it, and runs the action, which ends the call.
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
	uint64_t const needs = debruijn_plugin_needs(rule->needs, seen);
	uint64_t const takes = debruijn_plugin_takes(rule, needs);
	uint64_t const wrong = (needs & ~seen) | (seen & ~takes);
	if (wrong) {
		unsigned const arg = stdc_trailing_zeros(wrong);
		mcp_slot_tellf(slot, TOOL_FAILED, needs >> arg & 1 ? "%s needs the argument %s."
		                                                    : "%s takes no argument %s.",
		               name, debruijn_plugin_arg_names[arg].data);
		return;
	}
	/* seq is the sequence as the call gives it, and normal is seq in the
	 * rotation that starts with order zero bits, which the library reads. */
	uint64_t seq = 0;
	uint64_t normal = 0;
	unsigned order = 0;
	unsigned mask = 0;
	unsigned first = 0;
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
		unsigned masks[DEBRUIJN_PLUGIN_DEPTH];
		size_t n;
		int e = debruijn_plugin_address(jb_view(of[DEBRUIJN_PLUGIN_ARG_ADDR]), masks, &n);
		normal = DEBRUIJN_ROOT_SEED;
		for (order = DEBRUIJN_MIN_ORDER; !e && order < DEBRUIJN_MIN_ORDER + n; ++order)
			e = debruijn_expand(&normal, normal, order, masks[order - DEBRUIJN_MIN_ORDER]);
		if (e) {
			mcp_slot_tell(slot, CSTR_VIEW("The addr argument must be 1 to 4 masks with a dot "
			                              "between two, such as 1.7.127.32767. The masks are "
			                              "at most 1, 7, 127 and 32767, in this order."),
			              TOOL_FAILED);
			return;
		}
		seq = normal;
	} else if (needs & A(RANK)) {
		/* The call gives order, which is an order of the action. */
		unsigned rank;
		if (debruijn_plugin_upto(slot, of, DEBRUIJN_PLUGIN_ARG_RANK,
		                         debruijn_word_count(order) - 1U, order, &rank))
			return;
		int const e = debruijn_ordered(order, rank, 1, debruijn_plugin_store, &normal);
		if (e) {
			mcp_slot_fail(slot, e);
			return;
		}
		seq = normal;
	} else if (needs & A(SEQ)) {
		/* A sequence of order n has 2^n bits. Without order, the sign bit
		 * of a negative number is unknown. */
		int e;
		seq = debruijn_plugin_number(jb_view(of[DEBRUIJN_PLUGIN_ARG_SEQ]), 0,
		                             given ? 1U << order : 0, &e);
		if (e) {
			mcp_slot_tell(slot, e == EDOM
			                    ? CSTR_VIEW("A negative sequence needs the argument order: a "
			                                "sequence of order n has 2^n bits.")
			                    : CSTR_VIEW("The seq argument must be a string of a 64-bit "
			                                "number without a + sign: hexadecimal after 0x, "
			                                "binary after 0b, octal after 0, else decimal."),
			              TOOL_FAILED);
			return;
		}
		if (!given)
			order = debruijn_plugin_order(seq);
		normal = seq;
		if (!order || debruijn_normalize(&normal, order)) {
			debruijn_plugin_no_sequence(slot, seq, order);
			return;
		}
	}
	/* The order of a sequence that the call gives without order. */
	if (!given && (order < rule->low || order > rule->high)) {
		mcp_slot_tellf(slot, TOOL_FAILED, "0x%0*" PRIx64 " is a sequence of order %u, and %s "
		               "takes the orders %u to %u.", debruijn_plugin_digits(order), seq, order,
		               name, rule->low, rule->high);
		return;
	}
	if ((needs & A(MASK)) && debruijn_plugin_upto(slot, of, DEBRUIJN_PLUGIN_ARG_MASK,
	                                              debruijn_plugin_masks(order), order, &mask))
		return;
	if (((seen & A(FIRST)) && debruijn_plugin_uint(of[DEBRUIJN_PLUGIN_ARG_FIRST], UINT32_MAX,
	                                               &first)) ||
	    ((seen & A(LIMIT)) && (debruijn_plugin_uint(of[DEBRUIJN_PLUGIN_ARG_LIMIT], rule->cap,
	                                                &limit) || !limit))) {
		mcp_slot_tellf(slot, TOOL_FAILED, "first must be a whole number from 0, and limit a "
		               "whole number from 1 to %u.", rule->cap);
		return;
	}
	if (mcp_slot_offset(slot, args))
		return;
	struct debruijn_plugin_out out = { .slot = slot };
	switch ((enum debruijn_plugin_action)action) {
	case DEBRUIJN_PLUGIN_EXPAND:
		debruijn_plugin_expand(&out, normal, order, mask);
		break;
	case DEBRUIJN_PLUGIN_COLLAPSE:
		debruijn_plugin_collapse(&out, normal, order);
		break;
	case DEBRUIJN_PLUGIN_PATH:
		debruijn_plugin_path(&out, normal, order);
		break;
	case DEBRUIJN_PLUGIN_ADDRESS:
		if (debruijn_plugin_locate(&out, normal, order))
			debruijn_plugin_no_sequence(slot, normal, order);
		else
			debruijn_plugin_end(&out);
		break;
	case DEBRUIJN_PLUGIN_CHILDREN:
		debruijn_plugin_children(&out, normal, order, first, limit);
		break;
	case DEBRUIJN_PLUGIN_WINDOWS:
		debruijn_plugin_windows(&out, seq, order);
		break;
	case DEBRUIJN_PLUGIN_RANK:
		debruijn_plugin_rank(&out, normal, order);
		break;
	case DEBRUIJN_PLUGIN_UNRANK:
		out.e = debruijn_plugin_emit(normal, order, &out);
		debruijn_plugin_end(&out);
		break;
	case DEBRUIJN_PLUGIN_ORDERED:
		out.e = debruijn_ordered(order, first, limit, debruijn_plugin_emit, &out);
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
