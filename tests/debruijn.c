/** @file Unit tests of the de Bruijn sequences of the orders 2 to 8 in
 *  src/debruijn_wide.c and src/debruijn_wide_order.c: for the orders 2 to
 *  6 against src/debruijn.c and src/debruijn_order.c, which are the same
 *  algorithms in one uint64_t, and for the orders 7 and 8 against values
 *  of a line-by-line port of those files to Python integers. Then the
 *  arguments that the functions of the four files refuse. */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "debruijn.h"
#include "debruijn_wide.h"

/* The sequences of the orders 6, 7 and 8 that the masks 0 lead to, which
 * are the numerically first of their orders, and those of the largest
 * masks, which are the last. */
#define FIRST6 "0218a392cd3d5dbf"
#define FIRST7 "01061438916347932a5cd9d3ead7b77f"
#define FIRST8 "00818283848586878898a8b8c8d8e8f929395969799a9b9d9e9faaeb6bedeeff"
#define LAST6  "03f79d71b4cb0a89"
#define LAST7  "01fdf3d78edd3970d9ab464c582a5091"
#define LAST8  "00ff7e7d7c7b7a79787767574737271706d6c6a6968665646261605514941211"

/* Sequences of the orders 7 and 8 with their parents, masks and ranks.
 * The masks and the ranks are decimal.
 */
static struct {
	char const *seq;
	char const *parent;
	char const *mask;
	char const *rank;
} const rows[] = {
	{ "01c1729d21af768ac9be7896c6611fd5", "034e4aef0b6623f5",
	  "647892279", "108577079836213153" },
	{ "011641989d73e56f0f497f768535471b", "03990ad4ba278dfb",
	  "207388624", "11688136508633938" },
	{ "0188d25d9ca6dc27d6af1efe643a0a8b", "02571b53d161df99",
	  "404285457", "75829137854331870" },
	{ "01e357ac48b96f9fdd0c9b4a70547661", "0378b951fad27661",
	  "249103477", "127634579173237018" },
	{ "009f2fb8a24c8b9c0f4eca56e8c6f6bbf0eb106a690b36c157984725a19e3fd5",
	  "01c1729d21af768ac9be7896c6611fd5",
	  "691672907343361484", "323763803835137070802578622364566864" },
	{ "00b1e655d52618b827d7842b2281ad0ff241df119a74b7397eecf44a6f6d471b",
	  "011641989d73e56f0f497f768535471b",
	  "7999350999753991002", "517393984693196390533858588431234738" },
};

static uint64_t random_state = UINT64_C(88172645463325252);

/* Returns the next number of a xorshift generator. */
static uint64_t
random64 (void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return random_state;
}

static struct debruijn_wide
wide (uint64_t low)
{
	return (struct debruijn_wide){ { low } };
}

static bool
equal (struct debruijn_wide const *a,
       struct debruijn_wide const *b)
{
	return !memcmp(a, b, sizeof *a);
}

/* Returns the number of the hexadecimal digits @p text. */
static struct debruijn_wide
hex (char const *text)
{
	struct debruijn_wide ret = {};
	size_t const n = strlen(text);
	for (size_t i = 0; i < n; ++i) {
		char const c = text[n - 1 - i];
		uint64_t const digit = (uint64_t)(c <= '9' ? c - '0' : c - 'a' + 10);
		ret.limb[i / 16] |= digit << (4 * (i % 16));
	}
	return ret;
}

/* Returns the number of the decimal digits @p text, under 2^128. */
static struct debruijn_wide
decimal (char const *text)
{
	unsigned _BitInt(128) value = 0;
	for (; *text; ++text)
		value = value * 10 + (unsigned)(*text - '0');
	return (struct debruijn_wide){ { (uint64_t)value, (uint64_t)(value >> 64) } };
}

/* Returns @p seq of the order @p order rotated left by @p r bits, bit by
 * bit.
 */
static struct debruijn_wide
rotated (struct debruijn_wide const *seq,
         unsigned                    order,
         unsigned                    r)
{
	unsigned const bits = 1U << order;
	struct debruijn_wide ret = {};
	for (unsigned i = 0; i < bits; ++i) {
		if (debruijn_wide_test(seq->limb, i))
			debruijn_wide_set(ret.limb, (i + r) % bits);
	}
	return ret;
}

/* The wide functions for one sequence of an order from 2 to 6 against the
 * functions of debruijn.h.
 */
static void
test_narrow (uint64_t word,
             unsigned order)
{
	unsigned const bits = 1U << order;
	struct debruijn_wide const seq = wide(word);
	struct debruijn_wide got;
	CHECK(debruijn_wide_valid(&seq, order));
	/* Each rotation is normalized to the sequence, and no other rotation
	 * is valid. windows are the bits of two copies of the sequence. */
	for (unsigned r = 0; r < bits; r += order < 6 ? 1 : 5) {
		got = rotated(&seq, order, r);
		CHECK(debruijn_wide_valid(&got, order) == !r);
		CHECK(!debruijn_wide_normalize(&got, order) && equal(&got, &seq));
	}
	for (unsigned index = 0; index < bits; ++index) {
		unsigned value = 0;
		for (unsigned k = 0; k < order; ++k)
			value |= (unsigned)(word >> ((index + k) % bits) & 1U) << k;
		CHECK(debruijn_wide_window(&seq, order, index) == value);
	}
	/* A changed bit: the library and the wide function refuse the same
	 * numbers, and normalize the others to the same sequence. */
	for (unsigned k = 0; k < 4; ++k) {
		uint64_t other = word ^ UINT64_C(1) << (random64() % bits);
		got = wide(other);
		int const e = debruijn_normalize(&other, order);
		CHECK(debruijn_wide_normalize(&got, order) == e && (e || got.limb[0] == other));
	}
	if (order > DEBRUIJN_MIN_ORDER) {
		uint64_t seed;
		uint64_t choices;
		unsigned mask;
		CHECK(!debruijn_collapse(&seed, &mask, word, order));
		struct debruijn_wide const parent = wide(seed);
		CHECK(!debruijn_wide_collapse(&got, &choices, &seq, order) &&
		      equal(&got, &parent) && choices == mask);
	}
	if (order < DEBRUIJN_MAX_ORDER) {
		unsigned const masks = (1U << ((1U << (order - 1)) - 1U)) - 1U;
		int e = -1;
		struct debruijn_wide_generator const generator =
			debruijn_wide_generator(&seq, order, &e);
		CHECK(!e && generator.order == order);
		for (unsigned k = 0; k < 4; ++k) {
			unsigned const mask = k == 0 ? 0 : k == 1 ? masks
			                    : (unsigned)random64() & masks;
			uint64_t child;
			CHECK(!debruijn_expand(&child, word, order, mask));
			struct debruijn_wide const want = wide(child);
			CHECK(!debruijn_wide_expand(&got, &seq, order, mask) &&
			      equal(&got, &want));
			CHECK(!debruijn_wide_generate(&got, &generator, mask) &&
			      equal(&got, &want));
		}
		CHECK(debruijn_wide_expand(&got, &seq, order, (uint64_t)masks + 1) == EINVAL);
	}
	unsigned rank;
	CHECK(!debruijn_rank(&rank, word, order));
	struct debruijn_wide const want = wide(rank);
	CHECK(!debruijn_wide_rank(&got, &seq, order) && equal(&got, &want));
}

struct narrow_page {
	uint64_t words[64];
	uint64_t n;
};

static int
narrow_emit (uint64_t  word,
             unsigned  order,
             void     *arg)
{
	(void)order;
	struct narrow_page *page = arg;
	page->words[page->n++ % 64] = word;
	return 0;
}

struct wide_page {
	struct debruijn_wide seqs[64];
	uint64_t             n;
};

static int
wide_emit (struct debruijn_wide const *seq,
           unsigned                    order,
           void                       *arg)
{
	(void)order;
	struct wide_page *page = arg;
	page->seqs[page->n++ % 64] = *seq;
	return 0;
}

/* A page from each rank of the orders 2 to 5 and from random ranks of
 * order 6, and all checks of the first sequence of each page.
 */
static void
test_narrow_orders (void)
{
	for (unsigned order = DEBRUIJN_MIN_ORDER; order <= DEBRUIJN_MAX_ORDER; ++order) {
		unsigned const count = debruijn_word_count(order);
		unsigned const pages = order < 6 ? count + 2 : 300;
		for (unsigned k = 0; k < pages; ++k) {
			unsigned const first = order < 6 ? k : (unsigned)(random64() % count);
			unsigned const limit = 1 + (unsigned)(random64() % 40);
			struct narrow_page narrow = {};
			struct wide_page page = {};
			struct debruijn_wide const from = wide(first);
			CHECK(!debruijn_ordered(order, first, limit, narrow_emit, &narrow));
			CHECK(!debruijn_wide_ordered(order, &from, limit, wide_emit, &page));
			CHECK(page.n == narrow.n);
			for (unsigned i = 0; i < narrow.n && i < page.n; ++i)
				CHECK(page.seqs[i].limb[0] == narrow.words[i] &&
				      !page.seqs[i].limb[1]);
			if (narrow.n)
				test_narrow(narrow.words[0], order);
		}
	}
}

/* The orders 7 and 8: the first and the last sequence of each order, and
 * the rows of the Python port.
 */
static void
test_values (void)
{
	struct debruijn_wide const first[] = { hex(FIRST6), hex(FIRST7), hex(FIRST8) };
	struct debruijn_wide const last[] = { hex(LAST6), hex(LAST7), hex(LAST8) };
	struct debruijn_wide got;
	uint64_t choices;
	for (unsigned order = 6; order < 8; ++order) {
		uint64_t const masks = (UINT64_C(1) << ((1U << (order - 1)) - 1U)) - 1U;
		CHECK(!debruijn_wide_expand(&got, &first[order - 6], order, 0) &&
		      equal(&got, &first[order - 5]));
		CHECK(!debruijn_wide_expand(&got, &last[order - 6], order, masks) &&
		      equal(&got, &last[order - 5]));
		CHECK(debruijn_wide_expand(&got, &last[order - 6], order, masks + 1) == EINVAL);
		CHECK(!debruijn_wide_collapse(&got, &choices, &last[order - 5], order + 1) &&
		      equal(&got, &last[order - 6]) && choices == masks);
		/* The ranks 0 and the largest of the next order. */
		unsigned const exponent = (1U << order) - order - 1U;
		struct debruijn_wide end = {};
		debruijn_wide_set(end.limb, exponent);
		struct debruijn_wide largest = end;
		largest.limb[0] -= 1U;
		largest.limb[1] -= !end.limb[0];
		struct debruijn_wide const zero = {};
		CHECK(!debruijn_wide_rank(&got, &first[order - 5], order + 1) &&
		      equal(&got, &zero));
		CHECK(!debruijn_wide_rank(&got, &last[order - 5], order + 1) &&
		      equal(&got, &largest));
		struct wide_page page = {};
		CHECK(!debruijn_wide_ordered(order + 1, &largest, 3, wide_emit, &page) &&
		      page.n == 1 && equal(&page.seqs[0], &last[order - 5]));
		CHECK(!debruijn_wide_ordered(order + 1, &end, 3, wide_emit, &page) && page.n == 1);
		CHECK(!debruijn_wide_ordered(order + 1, &zero, 0, wide_emit, &page) && page.n == 1);
	}
	CHECK(debruijn_wide_expand(&got, &last[2], 8, 0) == EINVAL);
	CHECK(debruijn_wide_collapse(&got, &choices, &last[0], 2) == EINVAL);
	CHECK(!debruijn_wide_valid(&last[1], 8) && !debruijn_wide_valid(&last[2], 7) &&
	      !debruijn_wide_valid(&last[2], 9) && !debruijn_wide_valid(nullptr, 8));
	for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
		struct debruijn_wide const seq = hex(rows[i].seq);
		struct debruijn_wide const parent = hex(rows[i].parent);
		struct debruijn_wide const rank = decimal(rows[i].rank);
		uint64_t const mask = decimal(rows[i].mask).limb[0];
		unsigned const order = strlen(rows[i].seq) == 32 ? 7 : 8;
		CHECK(!debruijn_wide_expand(&got, &parent, order - 1, mask) && equal(&got, &seq));
		CHECK(!debruijn_wide_collapse(&got, &choices, &seq, order) &&
		      equal(&got, &parent) && choices == mask);
		CHECK(!debruijn_wide_rank(&got, &seq, order) && equal(&got, &rank));
		struct wide_page page = {};
		CHECK(!debruijn_wide_ordered(order, &rank, 1, wide_emit, &page) && page.n == 1 &&
		      equal(&page.seqs[0], &seq));
	}
	/* Pages of the Python port from a rank of each order. */
	struct wide_page page = {};
	struct debruijn_wide from = wide(UINT64_C(123456789012345));
	struct debruijn_wide const page7[] = { hex("0106285fde8d61c96d3e3ceaecca9137"),
	                                       hex("0106285fde8d61c96d3e3ceca9133757") };
	CHECK(!debruijn_wide_ordered(7, &from, 2, wide_emit, &page) && page.n == 2 &&
	      !memcmp(page.seqs, page7, sizeof page7));
	from = decimal("987654321098765432109876543210987654");
	struct debruijn_wide const page8[] = {
		hex("00df1ca4cd4db982464ad8b5977542f4f79c431a051fb3ebfc83b4b089d1570f"),
		hex("00df1ca4cd4db982464ad8b5977542f4f79c431a051fb3ebfc83b4b0f089d157"),
		hex("00df1ca4cd4db982464ad8b5977542f4f79c431a051fb3ebfc83c2276961d157"),
	};
	page.n = 0;
	CHECK(!debruijn_wide_ordered(8, &from, 3, wide_emit, &page) && page.n == 3 &&
	      !memcmp(page.seqs, page8, sizeof page8));
	/* A first rank from 2^128 is past the last rank of each order. */
	for (unsigned limb = 2; limb < DEBRUIJN_WIDE_LIMBS; ++limb) {
		from = (struct debruijn_wide){};
		from.limb[limb] = 1;
		page.n = 0;
		CHECK(!debruijn_wide_ordered(7, &from, 3, wide_emit, &page) && !page.n);
		CHECK(!debruijn_wide_ordered(8, &from, 3, wide_emit, &page) && !page.n);
	}
}

/* Random sequences of the orders 7 and 8: children and parents, each
 * rotation, the windows, and a page from a random rank with the rank of
 * its sequences.
 */
static void
test_wide_orders (void)
{
	struct debruijn_wide got;
	uint64_t choices;
	for (unsigned k = 0; k < 400; ++k) {
		struct wide_page six = {};
		struct debruijn_wide const from = wide(random64() % debruijn_word_count(6));
		uint64_t const masks[] = { random64() >> 33, random64() >> 1 };
		struct debruijn_wide seqs[3];
		CHECK(!debruijn_wide_ordered(6, &from, 1, wide_emit, &six) && six.n == 1);
		seqs[0] = six.seqs[0];
		for (unsigned order = 6; order < 8; ++order) {
			CHECK(!debruijn_wide_expand(&seqs[order - 5], &seqs[order - 6], order,
			                            masks[order - 6]));
			CHECK(!debruijn_wide_collapse(&got, &choices, &seqs[order - 5],
			                              order + 1) &&
			      equal(&got, &seqs[order - 6]) && choices == masks[order - 6]);
			CHECK(debruijn_wide_valid(&seqs[order - 5], order + 1));
		}
		if (k % 40)
			continue;
		for (unsigned order = 7; order <= 8; ++order) {
			struct debruijn_wide const *const seq = &seqs[order - 6];
			unsigned const bits = 1U << order;
			for (unsigned r = 0; r < bits; ++r) {
				struct debruijn_wide const turned = rotated(seq, order, r);
				got = turned;
				CHECK(debruijn_wide_valid(&turned, order) == !r);
				CHECK(!debruijn_wide_normalize(&got, order) && equal(&got, seq));
				/* The value at an index, bit by bit. */
				unsigned value = 0;
				for (unsigned i = 0; i < order; ++i)
					value |= (unsigned)debruijn_wide_test(turned.limb,
					                                      (r + i) % bits) << i;
				CHECK(debruijn_wide_window(&turned, order, r) == value);
				/* One changed bit is a rotation of no sequence. */
				got = turned;
				got.limb[(r * 5 % bits) / 64] ^= UINT64_C(1) << (r * 5 % 64);
				CHECK(debruijn_wide_normalize(&got, order) == EINVAL);
				CHECK(!debruijn_wide_valid(&got, order));
			}
		}
	}
	for (unsigned order = 7; order <= 8; ++order) {
		unsigned const exponent = (1U << (order - 1)) - order;
		for (unsigned k = 0; k < 24; ++k) {
			/* A random rank under 2^exponent. */
			struct debruijn_wide from = { { random64(), random64() } };
			from.limb[exponent >> 6U] &= (UINT64_C(1) << (exponent & 63U)) - 1U;
			if (exponent < 64)
				from.limb[1] = 0;
			struct wide_page page = {};
			CHECK(!debruijn_wide_ordered(order, &from, 12, wide_emit, &page) &&
			      page.n == 12);
			for (unsigned i = 0; i < page.n && i < 12; ++i) {
				/* The ranks of a page are consecutive, and its
				 * sequences ascend. */
				struct debruijn_wide want = from;
				want.limb[0] += i;
				want.limb[1] += want.limb[0] < i;
				if (i == 0 || i == 11)
					CHECK(!debruijn_wide_rank(&got, &page.seqs[i], order) &&
					      equal(&got, &want));
				for (unsigned limb = DEBRUIJN_WIDE_LIMBS; i && limb--;) {
					uint64_t const a = page.seqs[i - 1].limb[limb];
					uint64_t const b = page.seqs[i].limb[limb];
					if (a != b) {
						CHECK(a < b);
						break;
					}
					CHECK(limb);
				}
			}
		}
	}
}

/* Returns true if the last exits @p edge of the vertices 1 to
 * @p vertices - 1 are a tree: they lead from each vertex to the vertex 0.
 */
static bool
is_tree (uint16_t const *edge,
         unsigned        vertices)
{
	for (unsigned start = 1; start < vertices; ++start) {
		unsigned v = start;
		for (unsigned steps = 0; v && steps < vertices; ++steps)
			v = edge[v] & (vertices - 1U);
		if (v)
			return false;
	}
	return true;
}

/* debruijn_wide_generate() with the generator @p generator and @p mask: a
 * sequence if the last exits of the generator are a tree, else EINVAL and
 * an unchanged output.
 */
static void
test_generator (struct debruijn_wide_generator const *generator,
                uint64_t                              mask)
{
	struct debruijn_wide const before = hex("a5a5a5a5");
	struct debruijn_wide got = before;
	int const e = debruijn_wide_generate(&got, generator, mask);
	if (is_tree(generator->edge, 1U << (generator->order - 1)))
		CHECK(!e && debruijn_wide_valid(&got, generator->order + 1));
	else
		CHECK(e == EINVAL && equal(&got, &before));
}

/* Generators that a caller made without the constructor: each choice of a
 * last exit for each nonroot vertex of the orders 2 to 4 with each mask,
 * and generators of the constructor with changed exits for the orders 5
 * to 7. Then an exit of another vertex, orders that are none of a seed,
 * and the errors of the constructor.
 */
static void
test_generators (void)
{
	for (unsigned order = 2; order <= 4; ++order) {
		unsigned const vertices = 1U << (order - 1);
		unsigned const masks = 1U << (vertices - 1U);
		for (unsigned exits = 0; exits < masks; ++exits) {
			struct debruijn_wide_generator generator = { .order = order };
			for (unsigned v = 1; v < vertices; ++v)
				generator.edge[v] = (uint16_t)(v << 1 | (exits >> (v - 1) & 1U));
			for (unsigned mask = 0; mask < masks; ++mask)
				test_generator(&generator, mask);
		}
	}
	struct debruijn_wide seq = hex("04653adf");
	int e = -1;
	for (unsigned order = 5; order <= 7; ++order) {
		unsigned const vertices = 1U << (order - 1);
		uint64_t const masks = (UINT64_C(1) << (vertices - 1U)) - 1U;
		struct debruijn_wide_generator const made =
			debruijn_wide_generator(&seq, order, &e);
		CHECK(!e && is_tree(made.edge, vertices));
		for (unsigned k = 0; k < 2000; ++k) {
			/* The other exit of one to three vertices. */
			struct debruijn_wide_generator generator = made;
			for (unsigned n = 1 + k % 3; n--;)
				generator.edge[1 + random64() % (vertices - 1U)] ^= 1U;
			test_generator(&generator, random64() & masks);
		}
		CHECK(!debruijn_wide_generate(&seq, &made, random64() & masks));
	}

	struct debruijn_wide const before = hex("a5a5a5a5");
	struct debruijn_wide got = before;
	struct debruijn_wide_generator other = {
		.order = 3,
		.edge  = { [1] = 2, [2] = 4, [3] = 4 },
	};
	CHECK(debruijn_wide_generate(&got, &other, 0) == EINVAL);
	other.edge[3] = UINT16_MAX;
	CHECK(debruijn_wide_generate(&got, &other, 0) == EINVAL);
	other.edge[3] = 6;
	CHECK(!debruijn_wide_generate(&got, &other, 0) && debruijn_wide_valid(&got, 4));
	got = before;
	static uint32_t const orders[] = { 0, 1, 8, 9, UINT32_MAX };
	struct debruijn_wide const root = wide(DEBRUIJN_ROOT_SEED);
	for (size_t i = 0; i < sizeof orders / sizeof orders[0]; ++i) {
		other.order = orders[i];
		CHECK(debruijn_wide_generate(&got, &other, 0) == EINVAL);
		e = -1;
		struct debruijn_wide_generator const failed =
			debruijn_wide_generator(&root, orders[i], &e);
		CHECK(e == EINVAL && !failed.order &&
		      debruijn_wide_generate(&got, &failed, 0) == EINVAL);
		CHECK(debruijn_wide_expand(&got, &root, orders[i], 0) == EINVAL);
	}
	/* A number that is no seed, a null seed and a null e. */
	e = -1;
	other = debruijn_wide_generator(&root, 3, &e);
	CHECK(e == EINVAL && !other.order);
	other = debruijn_wide_generator(nullptr, 2, &e);
	CHECK(e == EFAULT && !other.order);
	other = debruijn_wide_generator(&root, 2, nullptr);
	CHECK(!other.order && debruijn_wide_generate(&got, &other, 0) == EINVAL);
	other = debruijn_wide_generator(&root, 2, &e);
	CHECK(!e && other.order == 2);
	CHECK(debruijn_wide_generate(nullptr, &other, 0) == EFAULT &&
	      debruijn_wide_generate(&got, nullptr, 0) == EFAULT &&
	      debruijn_wide_expand(nullptr, &root, 2, 0) == EFAULT &&
	      debruijn_wide_expand(&got, nullptr, 2, 0) == EFAULT);
	CHECK(equal(&got, &before));
}

/* A set bit above the 2^order bits of a sequence: each function refuses
 * the number and changes no output.
 */
static void
test_high_bits (void)
{
	static uint8_t const places[] = { 4, 5, 8, 16, 31, 32, 63, 64, 127, 128, 191, 192, 255 };
	struct debruijn_wide const zero = {};
	struct debruijn_wide const before = hex("a5a5a5a5");
	for (unsigned order = 2; order <= 7; ++order) {
		struct wide_page page = {};
		CHECK(!debruijn_wide_ordered(order, &zero, 1, wide_emit, &page) && page.n == 1);
		for (size_t k = 0; k < sizeof places / sizeof places[0]; ++k) {
			if (places[k] < 1U << order)
				continue;
			struct debruijn_wide seq = page.seqs[0];
			debruijn_wide_set(seq.limb, places[k]);
			struct debruijn_wide const high = seq;
			struct debruijn_wide got = before;
			uint64_t choices = 77;
			int e = -1;
			CHECK(!debruijn_wide_valid(&seq, order));
			CHECK(debruijn_wide_normalize(&seq, order) == EINVAL && equal(&seq, &high));
			CHECK(debruijn_wide_rank(&got, &seq, order) == EINVAL);
			CHECK(debruijn_wide_collapse(&got, &choices, &seq, order) == EINVAL);
			CHECK(debruijn_wide_expand(&got, &seq, order, 0) == EINVAL);
			CHECK(!debruijn_wide_generator(&seq, order, &e).order && e == EINVAL);
			CHECK(equal(&got, &before) && choices == 77);
		}
	}
}

/* Counts the calls in the unsigned at @p arg and fails the third call. */
static int
failing_emit (struct debruijn_wide const *seq,
              unsigned                    order,
              void                       *arg)
{
	(void)seq;
	(void)order;
	unsigned *const calls = arg;
	return ++*calls == 3 ? ECANCELED : 0;
}

/* The arguments that the wide functions refuse: orders that are none of
 * the library, null pointers, a number that is no sequence, and an emit
 * function that fails.
 */
static void
test_wide_refusals (void)
{
	static unsigned const orders[] = { 0, 1, 9, UINT_MAX };
	struct debruijn_wide const zero = {};
	struct debruijn_wide const root = wide(DEBRUIJN_ROOT_SEED);
	struct debruijn_wide const seq = wide(0x17);
	struct debruijn_wide const before = hex("a5a5a5a5");
	struct debruijn_wide got = before;
	struct wide_page page = {};
	uint64_t choices = 77;
	for (size_t i = 0; i < sizeof orders / sizeof orders[0]; ++i) {
		struct debruijn_wide turned = root;
		CHECK(!debruijn_wide_valid(&root, orders[i]));
		CHECK(debruijn_wide_normalize(&turned, orders[i]) == EINVAL &&
		      equal(&turned, &root));
		CHECK(debruijn_wide_rank(&got, &root, orders[i]) == EINVAL);
		CHECK(debruijn_wide_collapse(&got, &choices, &root, orders[i]) == EINVAL);
		CHECK(debruijn_wide_ordered(orders[i], &zero, 1, wide_emit, &page) == EINVAL);
	}
	/* The root is the child of no sequence, and 0x5 is no sequence. */
	CHECK(debruijn_wide_collapse(&got, &choices, &root, 2) == EINVAL);
	struct debruijn_wide const five = wide(0x5);
	CHECK(debruijn_wide_rank(&got, &five, 3) == EINVAL &&
	      debruijn_wide_collapse(&got, &choices, &five, 3) == EINVAL);
	CHECK(debruijn_wide_normalize(nullptr, 3) == EFAULT &&
	      debruijn_wide_rank(nullptr, &seq, 3) == EFAULT &&
	      debruijn_wide_rank(&got, nullptr, 3) == EFAULT &&
	      debruijn_wide_collapse(nullptr, &choices, &seq, 3) == EFAULT &&
	      debruijn_wide_collapse(&got, nullptr, &seq, 3) == EFAULT &&
	      debruijn_wide_collapse(&got, &choices, nullptr, 3) == EFAULT &&
	      debruijn_wide_ordered(3, nullptr, 1, wide_emit, &page) == EFAULT &&
	      debruijn_wide_ordered(3, &zero, 1, nullptr, &page) == EFAULT);
	CHECK(equal(&got, &before) && choices == 77 && !page.n);
	/* The error of emit ends a page, from the rank 0 and from a rank
	 * that the function counts up to. */
	struct debruijn_wide const from = wide(5);
	for (unsigned order = 4; order <= 8; order += 2) {
		unsigned calls = 0;
		CHECK(debruijn_wide_ordered(order, &zero, 9, failing_emit, &calls) == ECANCELED &&
		      calls == 3);
		calls = 0;
		CHECK(debruijn_wide_ordered(order, &from, 9, failing_emit, &calls) == ECANCELED &&
		      calls == 3);
	}
}

/* The arguments that the functions of debruijn.h refuse. */
static void
test_narrow_refusals (void)
{
	static unsigned const orders[] = { 0, 1, 7, 8, UINT_MAX };
	uint64_t word = 77;
	uint64_t seed = 77;
	unsigned mask = 77;
	for (size_t i = 0; i < sizeof orders / sizeof orders[0]; ++i) {
		CHECK(!debruijn_valid(DEBRUIJN_ROOT_SEED, orders[i]));
		CHECK(debruijn_normalize(&word, orders[i]) == EINVAL);
		CHECK(debruijn_expand(&word, DEBRUIJN_ROOT_SEED, orders[i], 0) == EINVAL);
		CHECK(debruijn_collapse(&seed, &mask, DEBRUIJN_ROOT_SEED, orders[i]) == EINVAL);
		CHECK(debruijn_generator(DEBRUIJN_ROOT_SEED, orders[i]).error == EINVAL);
	}
	/* The root is the child of no word, order 6 is the order of no seed,
	 * 0x5 is no word, and the masks of the root are 0 and 1. */
	CHECK(debruijn_collapse(&seed, &mask, DEBRUIJN_ROOT_SEED, 2) == EINVAL);
	CHECK(debruijn_expand(&word, UINT64_C(0x0218a392cd3d5dbf), 6, 0) == EINVAL);
	CHECK(debruijn_expand(&word, 0x5, 2, 0) == EINVAL);
	CHECK(debruijn_expand(&word, DEBRUIJN_ROOT_SEED, 2, 2) == EINVAL);
	CHECK(debruijn_collapse(&seed, &mask, 0x5, 3) == EINVAL);
	/* A generator with the error of the constructor, and generators
	 * with an order that is none of a seed. */
	struct debruijn_generator const failed = debruijn_generator(0x5, 2);
	CHECK(failed.error == EINVAL && debruijn_generate(&word, &failed, 0) == EINVAL);
	struct debruijn_generator made = debruijn_generator(DEBRUIJN_ROOT_SEED, 2);
	CHECK(!made.error && debruijn_generate(&word, &made, 2) == EINVAL);
	for (size_t i = 0; i < sizeof orders / sizeof orders[0]; ++i) {
		made.order = orders[i];
		CHECK(debruijn_generate(&word, &made, 0) == EINVAL);
	}
	made.order = 6;
	CHECK(debruijn_generate(&word, &made, 0) == EINVAL);
	struct debruijn_generator const broken = { .edge = { 64, 2 }, .order = 2, .error = ENOMEM };
	CHECK(debruijn_generate(&word, &broken, 0) == ENOMEM);
	CHECK(debruijn_normalize(nullptr, 2) == EFAULT &&
	      debruijn_expand(nullptr, DEBRUIJN_ROOT_SEED, 2, 0) == EFAULT &&
	      debruijn_collapse(nullptr, &mask, 0x17, 3) == EFAULT &&
	      debruijn_collapse(&seed, nullptr, 0x17, 3) == EFAULT &&
	      debruijn_generate(nullptr, &made, 0) == EFAULT &&
	      debruijn_generate(&word, nullptr, 0) == EFAULT);
	CHECK(word == 77 && seed == 77 && mask == 77);
	made.order = 2;
	CHECK(!debruijn_generate(&word, &made, 1) && word == 0x1d);
}

int
main (void)
{
	test_narrow_orders();
	test_values();
	test_wide_orders();
	test_generators();
	test_high_bits();
	test_wide_refusals();
	test_narrow_refusals();
	return check_report("debruijn");
}
