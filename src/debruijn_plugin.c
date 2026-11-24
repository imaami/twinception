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
 * as 1.7.127.32767.
 *
 * The actions of the tool:
 *
 * - expand prints the child of a word for a mask.
 * - collapse prints the parent of a word, and the mask of the word in the
 *   parent.
 * - path prints the words on the way from the root for an address.
 * - address prints the address of a word, and the words on the way from
 *   the root.
 * - children lists children of a word from the mask first on, at most
 *   limit of them.
 * - windows prints the bits of a word, and for each place in the word the
 *   value of the order bits that start there.
 *
 * The plugin has no options, no instances and no data of its own, and a
 * call ends before its start() returns. check_debruijn (tests/askai.py)
 * runs it.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdbit.h>
#include <stdint.h>
#include <string.h>

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

#define A(id) (1u << DEBRUIJN_PLUGIN_ARG_##id)

/* The actions of the tool: X(ID, name, arguments), with the arguments that
 * the action needs. children also takes first and limit. Every action
 * takes offset.
 */
#define DEBRUIJN_PLUGIN_ACTION_LIST(X)                        \
        X(EXPAND,   "expand",   A(WORD) | A(ORDER) | A(MASK)) \
        X(COLLAPSE, "collapse", A(WORD) | A(ORDER))           \
        X(PATH,     "path",     A(ADDRESS))                   \
        X(ADDRESS,  "address",  A(WORD) | A(ORDER))           \
        X(CHILDREN, "children", A(WORD) | A(ORDER))           \
        X(WINDOWS,  "windows",  A(WORD) | A(ORDER))

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
	return (int)(1u << order >> 2);
}

/* Returns the largest mask of a word of the order @p order from 2 to 5:
 * a word has 2^(order-1) - 1 choice bits.
 */
static unsigned
debruijn_plugin_masks (unsigned order)
{
	return (1u << ((1u << (order - 1)) - 1)) - 1;
}

/* Reads the hexadecimal digits @p text, with or without 0x before them,
 * into @p *word. Returns 0, or EINVAL if @p text is not 1 to 16 digits.
 */
static int
debruijn_plugin_hex (struct cstr_view  text,
                     uint64_t         *word)
{
	if (cstr_view_starts(text, CSTR_VIEW("0x")) || cstr_view_starts(text, CSTR_VIEW("0X")))
		text = cstr_view_slice(text, 2, SIZE_MAX);
	if (!text.len || text.len > 16)
		return EINVAL;
	uint64_t value = 0;
	for (size_t i = 0; i < text.len; ++i) {
		unsigned const digit = util_hex_digit((unsigned char)text.data[i]);
		if (digit > 15)
			return EINVAL;
		value = value << 4 | digit;
	}
	*word = value;
	return 0;
}

/* Reads the address @p text, 1 to 4 masks with a dot between two, each a
 * decimal number or hexadecimal digits after 0x, into @p masks. Stores the
 * number of masks in @p *n. Returns 0 or EINVAL.
 */
static int
debruijn_plugin_address (struct cstr_view  text,
                         unsigned          masks[static DEBRUIJN_PLUGIN_DEPTH],
                         size_t           *n)
{
	*n = 0;
	for (struct cstr_view part = {}; text.len || !*n; ) {
		if (*n == DEBRUIJN_PLUGIN_DEPTH)
			return EINVAL;
		if (!cstr_view_cut(&text, '.', &part)) {
			part = text;
			text = cstr_view(nullptr);
		}
		uint64_t value = 0;
		bool const hex = cstr_view_starts(part, CSTR_VIEW("0x"));
		if (hex ? debruijn_plugin_hex(part, &value)
		        : !part.len || util_number(part.data, part.len, UINT32_MAX, &value))
			return EINVAL;
		if (value > UINT32_MAX)
			return EINVAL;
		masks[(*n)++] = (unsigned)value;
	}
	return 0;
}

/* Refuses a word that debruijn.h does not accept as a word of its order. */
static void
debruijn_plugin_no_word (struct mcp_slot *slot,
                         uint64_t         word,
                         unsigned         order)
{
	mcp_slot_tellf(slot, TOOL_FAILED, "0x%0*" PRIx64 " is no de Bruijn word of order %u "
	               "that starts with %u zero bits. Such a word has %u bits, and every "
	               "value of %u bits is in the word once.", debruijn_plugin_digits(order),
	               word, order, order, 1u << order, order);
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
	debruijn_plugin_line(out, "0x%0*" PRIx64 " is the child of 0x%0*" PRIx64 " (order %u) for "
	                     "the mask %u. Its order is %u.\n", debruijn_plugin_digits(order + 1),
	                     child, debruijn_plugin_digits(order), word, order, mask, order + 1);
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
	debruijn_plugin_line(out, "0x%0*" PRIx64 " is the parent of 0x%0*" PRIx64 " (order %u). "
	                     "Its order is %u, and the mask of the child is %u.\n",
	                     debruijn_plugin_digits(order - 1), parent,
	                     debruijn_plugin_digits(order), word, order, order - 1, mask);
	debruijn_plugin_end(out);
}

/* Writes the @p n words of an address as rows: the order, the mask and the
 * word of each, from the root on. words[0] is the root, and masks[i] is the
 * mask of words[i + 1].
 */
static void
debruijn_plugin_rows (struct debruijn_plugin_out *out,
                      uint64_t const             *words,
                      unsigned const             *masks,
                      size_t                      n)
{
	debruijn_plugin_line(out, "order  mask   word\n    2  -      0x%" PRIx64 "\n", words[0]);
	for (size_t i = 1; i < n; ++i) {
		unsigned const order = DEBRUIJN_MIN_ORDER + (unsigned)i;
		debruijn_plugin_line(out, "    %u  %-5u  0x%0*" PRIx64 "\n", order, masks[i - 1],
		                     debruijn_plugin_digits(order), words[i]);
	}
}

/* path: the words on the way from the root for the address @p text. */
static void
debruijn_plugin_path (struct debruijn_plugin_out *out,
                      struct cstr_view            text)
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
	debruijn_plugin_rows(out, words, masks, n + 1);
	debruijn_plugin_end(out);
}

/* address: the address of @p word, and the words on the way from the root. */
static void
debruijn_plugin_locate (struct debruijn_plugin_out *out,
                        uint64_t                    word,
                        unsigned                    order)
{
	unsigned masks[DEBRUIJN_PLUGIN_DEPTH];
	uint64_t words[DEBRUIJN_PLUGIN_DEPTH + 1];
	size_t const n = order - DEBRUIJN_MIN_ORDER;
	words[n] = word;
	int e = debruijn_valid(word, order) ? 0 : EINVAL;
	for (size_t i = n; !e && i; --i)
		e = debruijn_collapse(&words[i - 1], &masks[i - 1], words[i],
		                      DEBRUIJN_MIN_ORDER + (unsigned)i);
	if (e) {
		debruijn_plugin_no_word(out->slot, word, order);
		return;
	}
	debruijn_plugin_line(out, "address: ");
	for (size_t i = 0; i < n; ++i)
		debruijn_plugin_line(out, "%s%u", i ? "." : "", masks[i]);
	debruijn_plugin_line(out, "%s\n", n ? "" : "(the root has no address)");
	debruijn_plugin_rows(out, words, masks, n + 1);
	debruijn_plugin_end(out);
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
	debruijn_plugin_line(out, "mask   child (order %u)\n", order + 1);
	for (uint64_t mask = first; mask < end; ++mask) {
		uint64_t child = 0;
		(void)debruijn_generate(&child, &seed, (unsigned)mask);
		debruijn_plugin_line(out, "%-5" PRIu64 "  0x%0*" PRIx64 "\n", mask,
		                     debruijn_plugin_digits(order + 1), child);
	}
	uint64_t const listed = first < end ? end - first : 0;
	debruijn_plugin_line(out, "Listed %" PRIu64 " of %" PRIu64 " children", listed, all);
	if (listed && listed < all)
		debruijn_plugin_line(out, ", from mask %" PRIu64, first);
	debruijn_plugin_line(out, ".");
	if (listed && end < all)
		debruijn_plugin_line(out, " For the next, call again with first: %" PRIu64 ".",
		                     end);
	debruijn_plugin_line(out, "\n");
	debruijn_plugin_end(out);
}

/* windows: the bits of @p word, and for each place in the word the value of
 * the order bits that start there. The bits after the end of the word are
 * the bits at its start.
 */
static void
debruijn_plugin_windows (struct debruijn_plugin_out *out,
                         uint64_t                    word,
                         unsigned                    order)
{
	if (!debruijn_valid(word, order)) {
		debruijn_plugin_no_word(out->slot, word, order);
		return;
	}
	unsigned const bits = 1u << order;
	char text[65];
	for (unsigned i = 0; i < bits; ++i)
		text[i] = (char)('0' + (word >> (bits - 1 - i) & 1));
	text[bits] = '\0';
	debruijn_plugin_line(out, "0x%0*" PRIx64 " (order %u), most significant bit first:\n%s\n"
	                     "place  bits%*s  value\n", debruijn_plugin_digits(order), word, order,
	                     text, (int)order - 4 > 0 ? (int)order - 4 : 0, "");
	for (unsigned place = 0; place < bits; ++place) {
		char window[DEBRUIJN_MAX_ORDER + 1];
		unsigned value = 0;
		for (unsigned i = 0; i < order; ++i) {
			window[i] = text[(place + i) % bits];
			value = value << 1 | (unsigned)(window[i] - '0');
		}
		window[order] = '\0';
		debruijn_plugin_line(out, "%5u  %-4s  %5u\n", place, window, value);
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
		"\"description\":\"Explore the binary de Bruijn words of the orders 2 to 6. A "
		"word of order n has 2^n bits, and every value of n bits is in the word once, "
		"also across the end of the word to its start. The words of this tool start "
		"with n zero bits. Write a word as hexadecimal digits, such as 0x09af for order "
		"4. The word 0x3 of order 2 is the root of a tree. A word of order n from 2 to 5 "
		"has one child of order n+1 for each mask: 2, 8, 128 and 32768 children for the "
		"orders 2, 3, 4 and 5. A word of order 3 to 6 is the child of one word. The "
		"address of a word is the masks on the way from the root, such as "
		"1.7.127.32767.\","
		"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		"\"action\":{\"type\":\"string\",\"enum\":["
		XL_JOIN(",", ",", DEBRUIJN_PLUGIN_ACTION_LIST(XL_JSON)) "],\"description\":"
		"\"expand prints the child of word for mask. collapse prints the parent of word "
		"and the mask of word in the parent. path prints the words on the way from the "
		"root for address. address prints the address of word and the words on the way "
		"from the root. children lists children of word from the mask first on. windows "
		"prints the bits of word, and for each place in word the value of the order "
		"bits that start there.\"},"
		"\"word\":{\"type\":\"string\",\"description\":\"A word as 1 to 16 hexadecimal "
		"digits, with or without 0x, such as 0x04653adf.\"},"
		"\"order\":{\"type\":\"integer\",\"minimum\":2,\"maximum\":6,\"description\":"
		"\"The order of word: 2 to 5 for expand and children, 3 to 6 for collapse, 2 to "
		"6 for address and windows.\"},"
		"\"mask\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"For expand: the "
		"mask of the child, at most 1, 7, 127 and 32767 for the orders 2, 3, 4 and "
		"5.\"},"
		"\"address\":{\"type\":\"string\",\"description\":\"For path: 1 to 4 masks with "
		"a dot between two, such as 1.7.127.32767. A mask is a decimal number or "
		"hexadecimal digits after 0x.\"},"
		"\"first\":{\"type\":\"integer\",\"minimum\":0,\"default\":0,\"description\":"
		"\"For children: the mask of the first child in the output.\"},"
		"\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32768,\"default\":64,"
		"\"description\":\"For children: the maximum number of children in the "
		"output.\"},"
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
	/* An action takes the arguments that it needs, and offset. children
	 * also takes first and limit. */
	uint32_t const needs = debruijn_plugin_needs[action];
	uint32_t const takes = needs | A(ACTION) | A(OFFSET) |
	                       (action == DEBRUIJN_PLUGIN_CHILDREN ? A(FIRST) | A(LIMIT) : 0);
	uint32_t const wrong = (needs & ~(uint32_t)seen) | ((uint32_t)seen & ~takes);
	if (wrong) {
		unsigned const arg = (unsigned)stdc_trailing_zeros(wrong);
		mcp_slot_tellf(slot, TOOL_FAILED, needs >> arg & 1 ? "%s needs the argument %s."
		                                                    : "%s takes no argument %s.",
		               debruijn_plugin_action_names[action].data,
		               debruijn_plugin_arg_names[arg].data);
		return;
	}
	uint64_t word = 0;
	uint64_t order = 0;
	uint64_t mask = 0;
	uint64_t first = 0;
	uint64_t limit = DEBRUIJN_PLUGIN_LIMIT;
	/* The orders of an action: expand and children make a child, collapse
	 * finds a parent. */
	unsigned const low = action == DEBRUIJN_PLUGIN_COLLAPSE ? DEBRUIJN_MIN_ORDER + 1
	                                                        : DEBRUIJN_MIN_ORDER;
	unsigned const high = action == DEBRUIJN_PLUGIN_EXPAND || action == DEBRUIJN_PLUGIN_CHILDREN
	                    ? DEBRUIJN_MAX_ORDER - 1 : DEBRUIJN_MAX_ORDER;
	struct cstr_view const digits = jb_view(of[DEBRUIJN_PLUGIN_ARG_WORD]);
	if ((needs & A(WORD)) && debruijn_plugin_hex(digits, &word)) {
		mcp_slot_tell(slot, CSTR_VIEW("The word argument must be a string of 1 to 16 "
		                              "hexadecimal digits, with or without 0x."),
		              TOOL_FAILED);
		return;
	}
	if ((needs & A(ORDER)) && (jb_uint_checked(of[DEBRUIJN_PLUGIN_ARG_ORDER], high, &order) ||
	                           order < low)) {
		mcp_slot_tellf(slot, TOOL_FAILED, "The order argument must be a whole number from "
		               "%u to %u for %s.", low, high,
		               debruijn_plugin_action_names[action].data);
		return;
	}
	if ((needs & A(MASK)) && jb_uint_checked(of[DEBRUIJN_PLUGIN_ARG_MASK],
	                                         debruijn_plugin_masks((unsigned)order), &mask)) {
		mcp_slot_tellf(slot, TOOL_FAILED, "The mask argument must be a whole number from 0 "
		               "to %u for order %u.", debruijn_plugin_masks((unsigned)order),
		               (unsigned)order);
		return;
	}
	if (((seen >> DEBRUIJN_PLUGIN_ARG_FIRST & 1) &&
	     jb_uint_checked(of[DEBRUIJN_PLUGIN_ARG_FIRST], UINT32_MAX, &first)) ||
	    ((seen >> DEBRUIJN_PLUGIN_ARG_LIMIT & 1) &&
	     !(limit = jb_count(of[DEBRUIJN_PLUGIN_ARG_LIMIT], 32768)))) {
		mcp_slot_tell(slot, CSTR_VIEW("first must be a whole number from 0, and limit a "
		                              "whole number from 1 to 32768."), TOOL_FAILED);
		return;
	}
	if (mcp_slot_offset(slot, args))
		return;
	struct debruijn_plugin_out out = { .slot = slot };
	switch ((enum debruijn_plugin_action)action) {
	case DEBRUIJN_PLUGIN_EXPAND:
		debruijn_plugin_expand(&out, word, (unsigned)order, (unsigned)mask);
		break;
	case DEBRUIJN_PLUGIN_COLLAPSE:
		debruijn_plugin_collapse(&out, word, (unsigned)order);
		break;
	case DEBRUIJN_PLUGIN_PATH:
		debruijn_plugin_path(&out, jb_view(of[DEBRUIJN_PLUGIN_ARG_ADDRESS]));
		break;
	case DEBRUIJN_PLUGIN_ADDRESS:
		debruijn_plugin_locate(&out, word, (unsigned)order);
		break;
	case DEBRUIJN_PLUGIN_CHILDREN:
		debruijn_plugin_children(&out, word, (unsigned)order, first, limit);
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
			.flags = TOOL_LENT | TOOL_PAGED
		},
	},
};
