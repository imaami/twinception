/* debruijn_plugin.c - a plugin with one tool, debruijn, that explores the
 * tree of the binary de Bruijn words of the orders 2 to 6 (src/debruijn.h).
 *
 * A word of order n has 2^n bits, and every value of n bits is in the word
 * once, also across the end of the word to its start. The words of
 * debruijn.h start with n zero bits. The word 0x3 of order 2 is the root of
 * the tree. A word of order n from 2 to 5 has one child of order n + 1 for
 * each mask from 0 to 2^(2^(n-1)-1) - 1 (debruijn_expand()), and a word of
 * order 3 to 6 is the child of exactly one word (debruijn_collapse()). The
 * address of a word is the masks on the way from the root to the word, such
 * as 1.7.127.32767. The masks 0 lead to the numerically first word of each
 * order, and the largest masks to the last.
 *
 * The output of an action is its result and nothing else, as the output of
 * a calculator:
 *
 * - expand: the child of a word for a mask, such as 0x07dcd629.
 * - collapse: the parent of a word and the mask of the word in the parent,
 *   such as 0x0f65 127.
 * - path: the words on the way from the root for an address, without the
 *   root, as 1d.0f65.07dcd629.03f79d71b4cb0a89.
 * - address: the address of a word, such as 1.7.127.32767.
 * - children: the children of a word from the mask first on, at most limit
 *   of them, one on a line.
 * - windows: for each place in a word, the value of the order bits that
 *   start there, one on a line.
 * - rank: the numeric position of a word (or address) within its order.
 * - unrank: the word and address at that zero-based rank.
 * - ordered: rows as for unrank, from rank first on, at most limit rows.
 *
 * A word, and each mask of an address, is a number as strtoll() and
 * strtoull() read it with the base 0, but without a + sign
 * (debruijn_plugin_number()): hexadecimal after 0x, binary after 0b, octal
 * after 0, else decimal. A prefix on the leftmost mask of an address is the
 * base of all its masks, so 0b1.110.1010101.111110000011111 is binary. A
 * negative number stands for its two's complement in the bits of the word
 * or mask: 2^n bits for a word of order n, which then needs the argument
 * order, and 2^(n-1) - 1 bits for a mask that expands a word of order n.
 * The words in the output are hexadecimal.
 *
 * An action takes a word in any rotation, and rotates it to start with its
 * n zero bits (debruijn_normalize()) before the library reads it. windows
 * reads the word as it is given. Without the argument order, the bit width
 * of the word gives the order: a rotation of a word of order n is 2^n - n to
 * 2^n bits wide.
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
        X(ACTION,  "action")        \
        X(WORD,    "word")          \
        X(ORDER,   "order")         \
        X(MASK,    "mask")          \
        X(ADDRESS, "address")       \
        X(RANK,    "rank")          \
        X(FIRST,   "first")         \
        X(LIMIT,   "limit")         \
        X(OFFSET,  "offset")

#define DEBRUIJN_PLUGIN_ARG_ENUM(id, ...) DEBRUIJN_PLUGIN_ARG_##id,
enum debruijn_plugin_arg : uint8_t {
	DEBRUIJN_PLUGIN_ARG_LIST(DEBRUIJN_PLUGIN_ARG_ENUM) DEBRUIJN_PLUGIN_ARGS
};
#undef DEBRUIJN_PLUGIN_ARG_ENUM

static struct cstr_view const debruijn_plugin_arg_names[] = {
	DEBRUIJN_PLUGIN_ARG_LIST(XL_VIEW)
};

#define A(id) (1U << DEBRUIJN_PLUGIN_ARG_##id)

/* The actions of the tool: X(ID, name, arguments), with the arguments that
 * the action needs. An action that needs word also takes order. rank
 * alternatively takes an address. children and ordered also take first and
 * limit. Every action takes offset.
 */
#define DEBRUIJN_PLUGIN_ACTION_LIST(X)                \
        X(EXPAND,   "expand",   A(WORD) | A(MASK))    \
        X(COLLAPSE, "collapse", A(WORD))              \
        X(PATH,     "path",     A(ADDRESS))           \
        X(ADDRESS,  "address",  A(WORD))              \
        X(CHILDREN, "children", A(WORD))              \
        X(WINDOWS,  "windows",  A(WORD))              \
        X(RANK,     "rank",     A(WORD))              \
        X(UNRANK,   "unrank",   A(ORDER) | A(RANK))   \
        X(ORDERED,  "ordered",  A(ORDER))

#define DEBRUIJN_PLUGIN_ACTION_ENUM(id, ...) DEBRUIJN_PLUGIN_##id,
enum debruijn_plugin_action : uint8_t {
	DEBRUIJN_PLUGIN_ACTION_LIST(DEBRUIJN_PLUGIN_ACTION_ENUM) DEBRUIJN_PLUGIN_ACTIONS
};
#undef DEBRUIJN_PLUGIN_ACTION_ENUM

static struct cstr_view const debruijn_plugin_action_names[] = {
	DEBRUIJN_PLUGIN_ACTION_LIST(XL_VIEW)
};

#define DEBRUIJN_PLUGIN_ACTION_NEEDS(id, name, needs) needs,
static uint32_t const debruijn_plugin_needs[] = {
	DEBRUIJN_PLUGIN_ACTION_LIST(DEBRUIJN_PLUGIN_ACTION_NEEDS)
};
#undef DEBRUIJN_PLUGIN_ACTION_NEEDS

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

/* Returns the number of hexadecimal digits of a word of the order @p order:
 * a digit for each 4 of its 2^order bits.
 */
static int
debruijn_plugin_digits (unsigned order)
{
	return (int)(1U << order >> 2U);
}

/* Returns the largest mask of a word of the order @p order from 2 to 5:
 * a word has 2^(order-1) - 1 choice bits.
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
		/* Mask n expands a word of order n + 2, which has 2^(n+1) - 1
		 * choice bits. */
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
 * of @p word, or 0 for a word under 2 bits wide. A rotation of a word of
 * order n is 2^n - n to 2^n bits wide: the n zero bits of the word can be at
 * its top, in whole or in part, or not.
 */
static unsigned
debruijn_plugin_order (uint64_t word)
{
	return stdc_bit_width(
		stdc_bit_width(word)
		- (word >= 1U << 2U)
		- (word == 1U << 1U)
	);
}

/* Refuses a word that is no rotation of a word of the order @p order, or of
 * any order if @p order is 0.
 */
static void
debruijn_plugin_no_word (struct mcp_slot *slot,
                         uint64_t         word,
                         unsigned         order)
{
	if (order >= DEBRUIJN_MIN_ORDER)
		mcp_slot_tellf(slot, TOOL_FAILED, "0x%0*" PRIx64 " is no word of order %u.",
		               debruijn_plugin_digits(order), word, order);
	else
		mcp_slot_tellf(slot, TOOL_FAILED, "0x%" PRIx64 " is no word of any order from %u "
		               "to %u.", word, DEBRUIJN_MIN_ORDER, DEBRUIJN_MAX_ORDER);
}

/* expand: the child of @p word for @p mask. */
static void
debruijn_plugin_expand (struct debruijn_plugin_out *out,
                        uint64_t                    word,
                        unsigned                    order,
                        unsigned                    mask)
{
	uint64_t child;
	if (debruijn_expand(&child, word, order, mask)) {
		debruijn_plugin_no_word(out->slot, word, order);
		return;
	}
	debruijn_plugin_line(out, "0x%0*" PRIx64 "\n", debruijn_plugin_digits(order + 1U), child);
	debruijn_plugin_end(out);
}

/* collapse: the parent of @p word and the mask of @p word in the parent. */
static void
debruijn_plugin_collapse (struct debruijn_plugin_out *out,
                          uint64_t                    word,
                          unsigned                    order)
{
	uint64_t parent;
	unsigned mask;
	if (debruijn_collapse(&parent, &mask, word, order)) {
		debruijn_plugin_no_word(out->slot, word, order);
		return;
	}
	debruijn_plugin_line(out, "0x%0*" PRIx64 " %u\n", debruijn_plugin_digits(order - 1U),
	                     parent, mask);
	debruijn_plugin_end(out);
}

/* Writes the words @p words[1] to @p words[n - 1] of an address, with a dot
 * between two, each at the width of its order: 17.09af.04653adf. words[0]
 * is the root, which the line leaves out.
 */
static void
debruijn_plugin_chain (struct debruijn_plugin_out *out,
                       uint64_t const             *words,
                       size_t                      n)
{
	for (size_t i = 1; i < n; ++i) {
		int const digits = debruijn_plugin_digits(DEBRUIJN_MIN_ORDER + (unsigned)i);
		debruijn_plugin_line(out, "%s%0*" PRIx64, i > 1 ? "." : "", digits, words[i]);
	}
	debruijn_plugin_line(out, "\n");
}

static void
debruijn_plugin_rank (struct debruijn_plugin_out *out, uint64_t word, unsigned order)
{
	unsigned rank;
	if (debruijn_rank(&rank, word, order)) {
		debruijn_plugin_no_word(out->slot, word, order);
		return;
	}
	debruijn_plugin_line(out, "%u\n", rank);
	debruijn_plugin_end(out);
}

/* Resolve an address once, for path or rank. */
static void
debruijn_plugin_from_address (struct debruijn_plugin_out *out,
                              struct cstr_view            text,
                              enum debruijn_plugin_action action)
{
	unsigned masks[DEBRUIJN_PLUGIN_DEPTH];
	size_t n;
	uint64_t words[DEBRUIJN_PLUGIN_DEPTH + 1] = { DEBRUIJN_ROOT_SEED };
	int e = debruijn_plugin_address(text, masks, &n);
	for (size_t i = 0; !e && i < n; ++i)
		e = debruijn_expand(&words[i + 1], words[i], DEBRUIJN_MIN_ORDER + (unsigned)i,
		                    masks[i]);
	if (e) {
		mcp_slot_tell(out->slot, CSTR_VIEW("The address argument must be 1 to 4 masks with "
		                                   "a dot between two, such as 1.7.127.32767. The "
		                                   "masks are at most 1, 7, 127 and 32767, in this "
		                                   "order."), TOOL_FAILED);
		return;
	}
	if (action == DEBRUIJN_PLUGIN_RANK) {
		debruijn_plugin_rank(out, words[n], DEBRUIJN_MIN_ORDER + (unsigned)n);
		return;
	}
	debruijn_plugin_chain(out, words, n + 1);
	debruijn_plugin_end(out);
}

/* Append the address and newline. An order-2 row has no masks. */
static int
debruijn_plugin_locate (struct debruijn_plugin_out *out,
                        uint64_t                    word,
                        unsigned                    order)
{
	unsigned masks[DEBRUIJN_PLUGIN_DEPTH];
	size_t const n = order - DEBRUIJN_MIN_ORDER;
	for (size_t i = n; i; --i) {
		int e = debruijn_collapse(&word, &masks[i - 1], word, DEBRUIJN_MIN_ORDER + (unsigned)i);
		if (e)
			return e;
	}
	for (size_t i = 0; i < n; ++i)
		debruijn_plugin_line(out, "%s%u", i ? "." : "", masks[i]);
	debruijn_plugin_line(out, "\n");
	return 0;
}

static int
debruijn_plugin_emit (uint64_t  word,
                      unsigned  order,
                      void     *arg)
{
	struct debruijn_plugin_out *out = arg;
	debruijn_plugin_line(out, "0x%0*" PRIx64 "%s", debruijn_plugin_digits(order), word,
	                     order == DEBRUIJN_MIN_ORDER ? "" : " ");
	int e = debruijn_plugin_locate(out, word, order);
	return e ? e : out->e;
}

/* children: the children of @p word from the mask @p first on, at most
 * @p limit of them.
 */
static void
debruijn_plugin_children (struct debruijn_plugin_out *out,
                          uint64_t                    word,
                          unsigned                    order,
                          uint64_t                    first,
                          uint64_t                    limit)
{
	struct debruijn_generator const seed = debruijn_generator(word, order);
	if (seed.error) {
		debruijn_plugin_no_word(out->slot, word, order);
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

/* windows: for each place in @p word, the value of the @p order bits that
 * start there. The word can be in any rotation. The bits after the end of
 * the word are the bits at its start.
 */
static void
debruijn_plugin_windows (struct debruijn_plugin_out *out,
                         uint64_t                    word,
                         unsigned                    order)
{
	/* Place 0 is the most significant bit. A window that runs over the end
	 * of the word reads on at its start: the bit positions count down
	 * modulo the 2^order bits. */
	unsigned const bits = 1U << order;
	for (unsigned place = 0; place < bits; ++place) {
		unsigned const top = bits - 1U - place;
		unsigned value = 0;
		for (unsigned i = 0; i < order; ++i)
			value = value << 1 | (unsigned)(word >> ((top - i) & (bits - 1U)) & 1U);
		debruijn_plugin_line(out, "%u\n", value);
	}
	debruijn_plugin_end(out);
}

/* Returns the definition of debruijn as a new reference, or nullptr if
 * json-c cannot allocate memory.
 */
static struct json_object *
debruijn_plugin_definition (void const *service)
{
	(void)service;
	static_assert(DEBRUIJN_PLUGIN_LIMIT == 64, "the description says");
	return json_tokener_parse(
		"{\"name\":\"debruijn\",\"title\":\"De Bruijn words\","
		"\"description\":\"Binary de Bruijn words of the orders 2 to 6. A word of "
		"order n has 2^n bits and starts with n zero bits. Every value of n bits is in "
		"the word once, also across the end of the word. Output words are hexadecimal "
		"digits at the width of their order, such as 0x09af for order 4. An action also "
		"takes a rotation of a word, such as 0x9af0 for 0x09af, and reads it as the word; "
		"windows reads the rotation as given. The word 0x3 of order 2 is the root of a "
		"tree. A word of order n from 2 to 5 has a child of order n+1 for each mask: 2, "
		"8, 128 and 32768 children for the orders 2, 3, 4 and 5. The address of a word "
		"is the masks on the way from the root, such as 1.7.127.32767. The masks 0 lead "
		"to the numerically first word of each order and the largest masks to the last. "
		"The output is the result alone.\","
		"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		"\"action\":{\"type\":\"string\",\"enum\":["
		XL_JOIN(",", ",", DEBRUIJN_PLUGIN_ACTION_LIST(XL_JSON)) "],\"description\":"
		"\"expand: the child of word for mask. collapse: the parent of word and the mask "
		"of word in the parent, as 0x0f65 127. path: the words of address without the "
		"root, in hexadecimal without 0x, as 1d.0f65.07dcd629.03f79d71b4cb0a89. "
		"address: the address of word. "
		"children: the children of word from the mask first on, at most limit, one on a "
		"line. windows: for each place in word from the most significant bit, the value "
		"of the order bits that start there, one on a line. rank: the zero-based position "
		"in unsigned word order; give either address or word and order. unrank: at rank "
		"in order, return word and decimal dotted address separated by a space (word "
		"alone for order 2). ordered: the same rows in ascending order from rank first "
		"on, at most limit rows.\"},"
		"\"word\":{\"type\":\"string\",\"description\":\"A word as a number, as strtoll() "
		"reads it with the base 0 but without a + sign: hexadecimal after 0x, such as "
		"0x09af, binary after 0b, octal after 0, else decimal. A negative word needs "
		"order and stands for its two's complement in 2^order bits.\"},"
		"\"order\":{\"type\":\"integer\",\"minimum\":2,\"maximum\":6,\"description\":"
		"\"The order of word: 2 to 5 for expand and children, 3 to 6 for collapse and "
		"address, 2 to 6 for windows, rank, unrank and ordered. With word, order is "
		"optional: a word of order n in any rotation is 2^n - n to 2^n bits wide, and "
		"the bit width of word gives the order.\"},"
		"\"mask\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"For expand: "
		"at most 1, 7, 127 and 32767 for the orders 2, 3, 4 and 5.\"},"
		"\"address\":{\"type\":\"string\",\"description\":\"For path or rank: 1 to 4 masks "
		"with a dot between two, each a number as for word. A prefix on the first mask "
		"gives the base of all, as in 0b1.110.1010101.111110000011111. A negative mask "
		"stands for its two's complement in the 1, 3, 7 or 15 bits of its place, so -1 "
		"is the largest mask.\"},"
		"\"rank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":67108863,\"description\":"
		"\"For unrank: less than 1, 2, 16, 2048 or 67108864 for orders 2 through 6.\"},"
		"\"first\":{\"type\":\"integer\",\"minimum\":0,\"default\":0,\"description\":"
		"\"For children: the first mask. For ordered: the first rank (empty past end).\"},"
		"\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32768,\"default\":64,"
		"\"description\":\"Maximum rows: up to 32768 for children or 256 for ordered.\"},"
		"\"offset\":{\"type\":\"integer\",\"minimum\":0,\"description\":\""
		TOOL_OFFSET_ABOUT "\"}},"
		"\"required\":[\"action\"],\"additionalProperties\":false},"
		"\"annotations\":{\"readOnlyHint\":true,\"openWorldHint\":false}}");
}

/* Starts a call of debruijn in @p slot with the arguments @p args: checks
 * the arguments of the action, and runs the action, which ends the call.
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
	/* The address form of rank infers order and excludes word and order. */
	uint32_t const needs = action == DEBRUIJN_PLUGIN_RANK && (seen & A(ADDRESS))
	                     ? A(ADDRESS) : debruijn_plugin_needs[action];
	bool const sequence = action == DEBRUIJN_PLUGIN_CHILDREN || action == DEBRUIJN_PLUGIN_ORDERED;
	uint32_t const takes = needs | A(ACTION) | A(OFFSET) |
	                       (needs & A(WORD) ? A(ORDER) : 0) |
	                       (sequence ? A(FIRST) | A(LIMIT) : 0);
	uint32_t const wrong = (needs & ~(uint32_t)seen) | ((uint32_t)seen & ~takes);
	if (wrong) {
		unsigned const arg = stdc_trailing_zeros(wrong);
		mcp_slot_tellf(slot, TOOL_FAILED, needs >> arg & 1 ? "%s needs the argument %s."
		                                                    : "%s takes no argument %s.",
		               debruijn_plugin_action_names[action].data,
		               debruijn_plugin_arg_names[arg].data);
		return;
	}
	uint64_t word = 0;
	uint64_t normal = 0;
	uint64_t order = 0;
	uint64_t mask = 0;
	uint64_t first = 0;
	uint64_t limit = DEBRUIJN_PLUGIN_LIMIT;
	/* The orders of an action: expand and children make a child, collapse
	 * finds a parent. */
	unsigned const low = action == DEBRUIJN_PLUGIN_COLLAPSE || action == DEBRUIJN_PLUGIN_ADDRESS
	                   ? DEBRUIJN_MIN_ORDER + 1 : DEBRUIJN_MIN_ORDER;
	unsigned const high = action == DEBRUIJN_PLUGIN_EXPAND || action == DEBRUIJN_PLUGIN_CHILDREN
	                    ? DEBRUIJN_MAX_ORDER - 1 : DEBRUIJN_MAX_ORDER;
	bool const given = seen & A(ORDER);
	if (given && (jb_uint_checked(of[DEBRUIJN_PLUGIN_ARG_ORDER], high, &order) ||
	              order < low)) {
		mcp_slot_tellf(slot, TOOL_FAILED, "The order argument must be a whole number from "
		               "%u to %u for %s.", low, high,
		               debruijn_plugin_action_names[action].data);
		return;
	}
	if (needs & A(WORD)) {
		/* A word of order n has 2^n bits. Without order, the sign bit of a
		 * negative word is unknown. */
		int e;
		word = debruijn_plugin_number(jb_view(of[DEBRUIJN_PLUGIN_ARG_WORD]), 0,
		                              given ? 1U << order : 0, &e);
		if (e) {
			mcp_slot_tell(slot, e == EDOM
			                    ? CSTR_VIEW("A negative word needs the argument order: a word "
			                                "of order n has 2^n bits.")
			                    : CSTR_VIEW("The word argument must be a string of a 64-bit "
			                                "number without a + sign: hexadecimal after 0x, "
			                                "binary after 0b, octal after 0, else decimal."),
			              TOOL_FAILED);
			return;
		}
		/* The library reads a word whose first order bits are zero: normal
		 * is word in that rotation. */
		if (!given)
			order = debruijn_plugin_order(word);
		normal = word;
		if (!order || debruijn_normalize(&normal, (unsigned)order)) {
			debruijn_plugin_no_word(slot, word, (unsigned)order);
			return;
		}
		if (!given && (order < low || order > high)) {
			mcp_slot_tellf(slot, TOOL_FAILED, "0x%0*" PRIx64 " is a word of order %u, and %s "
			               "takes the orders %u to %u.", debruijn_plugin_digits((unsigned)order),
			               word, (unsigned)order, debruijn_plugin_action_names[action].data,
			               low, high);
			return;
		}
	}
	if ((needs & A(MASK)) && jb_uint_checked(of[DEBRUIJN_PLUGIN_ARG_MASK],
	                                         debruijn_plugin_masks((unsigned)order), &mask)) {
		mcp_slot_tellf(slot, TOOL_FAILED, "The mask argument must be a whole number from 0 "
		               "to %u for order %u.", debruijn_plugin_masks((unsigned)order),
		               (unsigned)order);
		return;
	}
	/* unrank is the strict one-row form of ordered. order is validated. */
	if (needs & A(RANK)) {
		unsigned count = debruijn_word_count((unsigned)order);
		if (jb_uint_checked(of[DEBRUIJN_PLUGIN_ARG_RANK], count - 1U, &first)) {
			mcp_slot_tellf(slot, TOOL_FAILED, "rank must be an integer from 0 to %u for order %u.",
			               count - 1U, (unsigned)order);
			return;
		}
		limit = 1;
	}
	unsigned cap = action == DEBRUIJN_PLUGIN_ORDERED ? 256 : 32768;
	if (((seen >> DEBRUIJN_PLUGIN_ARG_FIRST & 1) &&
	     jb_uint_checked(of[DEBRUIJN_PLUGIN_ARG_FIRST], UINT32_MAX, &first)) ||
	    ((seen >> DEBRUIJN_PLUGIN_ARG_LIMIT & 1) &&
	     !(limit = jb_count(of[DEBRUIJN_PLUGIN_ARG_LIMIT], cap)))) {
		mcp_slot_tellf(slot, TOOL_FAILED, "first must be a whole number from 0, and limit a "
		               "whole number from 1 to %u.", cap);
		return;
	}
	if (mcp_slot_offset(slot, args))
		return;
	struct debruijn_plugin_out out = { .slot = slot };
	switch ((enum debruijn_plugin_action)action) {
	case DEBRUIJN_PLUGIN_EXPAND:
		debruijn_plugin_expand(&out, normal, (unsigned)order, (unsigned)mask);
		break;
	case DEBRUIJN_PLUGIN_COLLAPSE:
		debruijn_plugin_collapse(&out, normal, (unsigned)order);
		break;
	case DEBRUIJN_PLUGIN_RANK:
		if (!(needs & A(ADDRESS))) {
			debruijn_plugin_rank(&out, normal, (unsigned)order);
			break;
		}
		[[fallthrough]];
	case DEBRUIJN_PLUGIN_PATH:
		debruijn_plugin_from_address(&out, jb_view(of[DEBRUIJN_PLUGIN_ARG_ADDRESS]),
		                             (enum debruijn_plugin_action)action);
		break;
	case DEBRUIJN_PLUGIN_ADDRESS:
		if (debruijn_plugin_locate(&out, normal, (unsigned)order))
			debruijn_plugin_no_word(slot, normal, (unsigned)order);
		else
			debruijn_plugin_end(&out);
		break;
	case DEBRUIJN_PLUGIN_UNRANK:
	case DEBRUIJN_PLUGIN_ORDERED:
		out.e = debruijn_ordered((unsigned)order, (unsigned)first, (unsigned)limit,
		                         debruijn_plugin_emit, &out);
		debruijn_plugin_end(&out);
		break;
	case DEBRUIJN_PLUGIN_CHILDREN:
		debruijn_plugin_children(&out, normal, (unsigned)order, first, limit);
		break;
	case DEBRUIJN_PLUGIN_WINDOWS:
		debruijn_plugin_windows(&out, word, (unsigned)order);
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
