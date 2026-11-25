/**
 * Binary de Bruijn sequences by the tree-array bijection.
 *
 * A graph vertex is a (n - 1)-bit word; an edge is an n-bit word.
 * For an edge e: source = e >> 1, target = e & (vertex_count - 1).
 *
 * Expansion has three stages:
 *   1. Extract a last-exit tree from the seed's Euler tour.
 *   2. Lift the tree using independent bits at the nonroot vertices.
 *   3. Traverse the larger graph, taking each tree edge last.
 *
 * Stage 2 specializes Section 3 of Bidkhori and Kishore (2009):
 * https://arxiv.org/abs/0910.3442
 *
 * The order of eligible labels is numeric, exactly as in debruijn-lift.py.
 * Occurrence counts and a bitmap implement that ordering without rescanning
 * all queues. There is no candidate rejection or backtracking.
 */

#include <errno.h>
#include <stdint.h>

#include "compat.h"
#include "debruijn.h"

enum {
	MAX_VERTICES = 32,
	ROOT         = 64, /* Sentinel: valid edge labels are 0..63. */
};

/** A last-exit tree; edge[0] is ROOT rather than an outgoing edge. */
struct tree {
	unsigned edge[MAX_VERTICES];
	unsigned order;
	int      error;
};

/** Two-entry queue; used records how many entries have been consumed. */
struct queue {
	unsigned edge[2];
	unsigned used;
};

/** Extract and remove the smallest label in a nonempty bitmap. */
static force_inline unsigned
take_first (uint64_t *set)
{
	unsigned first = (unsigned)__builtin_ctzll(*set);
	*set &= *set - 1U;
	return first;
}

/** Validate a normalized word while recording each nonroot vertex's last exit. */
static struct tree
tree (uint64_t word,
      unsigned order)
{
	unsigned length = 1U << order;
	uint64_t mask = UINT64_MAX >> (64 - length);

	if ((word & ~mask) || (word >> (length - order)))
		return (struct tree){ .error = EINVAL };

	struct tree ret = { .edge = { ROOT }, .order = order };
	uint64_t seen = 0;

	for (unsigned i = 0; i < length; ++i) {
		unsigned edge = (unsigned)(word >> (length - order));
		uint64_t bit = UINT64_C(1) << edge;

		if (seen & bit) {
			ret.error = EINVAL;
			return ret;
		}
		seen |= bit;

		unsigned source = edge >> 1U;
		if (source)
			ret.edge[source] = edge;

		/* Rotate within the word's width, including the full 64-bit case. */
		word = ((word << 1U) & mask) | (word >> (length - 1U));
	}
	return ret;
}

/** Build an Euler tour by reserving every nonroot tree edge for the last exit. */
static uint64_t
tree_word (struct tree const *src)
{
	unsigned vertices = 1U << (src->order - 1);
	unsigned visits[MAX_VERTICES] = {};
	unsigned vertex = 0;
	uint64_t word = 0;

	for (unsigned i = 0; i < 2 * vertices; ++i) {
		/* The first bit of each edge gives the normalized cyclic word. */
		word = (word << 1) | (vertex >> (src->order - 2));

		unsigned visit = visits[vertex]++;

		/* At zero use edge 0, then edge 1. Else use non-tree, then tree. */
		unsigned edge = visit;
		if (vertex)
			edge = src->edge[vertex] ^ (visit ? 0U : 1U);

		vertex = edge & (vertices - 1);
	}
	return word;
}

/** Lift a zero-rooted tree using one arbitrary choice bit per nonroot vertex. */
static struct tree
tree_lift (struct debruijn_generator const *src,
           unsigned                         choices)
{
	unsigned vertices = 1U << (src->order - 1);
	struct queue queues[MAX_VERTICES / 2] = {
		[0] = { .edge = { 0, ROOT } }
	};
	unsigned occurrences[MAX_VERTICES] = { [0] = 1 };

	for (unsigned vertex = 1; vertex < vertices; ++vertex) {
		/* first is the out-edge that ends in 1 for a clear choice bit
		 * and the one that ends in 0 for a set bit, so that the masks 0
		 * from the root give the numerically first word of each order
		 * and the largest masks the last. */
		unsigned choice = (choices >> (vertex - 1)) & 1U;
		unsigned first = (vertex << 1) | (choice ^ 1U);
		unsigned last = src->edge[vertex];

		queues[vertex] = (struct queue){ .edge = { first, last } };
		++occurrences[first];
		++occurrences[last];
	}

	/* A label is eligible exactly when it is absent from all remaining queues. */
	uint64_t ready = 0;
	for (unsigned edge = 0; edge < 2 * vertices; ++edge) {
		if (!occurrences[edge])
			ready |= UINT64_C(1) << edge;
	}

	struct tree ret = { .edge = { ROOT }, .order = src->order + 1 };

	for (unsigned i = 0; i < 2 * vertices; ++i) {
		unsigned edge = take_first(&ready);
		struct queue *queue = &queues[edge & (vertices - 1)];
		unsigned next = queue->edge[queue->used++];

		if (next == ROOT)
			return ret;

		/* Old edges become new vertices. Store the new edge edge -> next. */
		ret.edge[edge] = (edge << 1) | (next & 1U);

		/* A label becomes eligible once, when its final occurrence is popped. */
		if (!--occurrences[next])
			ready |= UINT64_C(1) << next;
	}
	return (struct tree){ .error = EINVAL };
}

bool
debruijn_valid (uint64_t word,
                unsigned order)
{
	return order >= DEBRUIJN_MIN_ORDER && order <= DEBRUIJN_MAX_ORDER &&
	       !tree(word, order).error;
}

/** Test a bit of a bitmap and set it; return whether it was set before. */
static force_inline bool
seen (uint64_t *map,
      unsigned  pos)
{
	uint64_t bit = UINT64_C(1) << pos;
	if (*map & bit)
		return true;
	*map |= bit;
	return false;
}

/**
 * Mark num windows of seq in map, from the least significant bit on, each
 * window the bits of seq under the mask window. Return the map, or zero if a
 * window is in it already. num must be at least 1.
 */
static force_inline uint64_t
windows (uint64_t seq,
         uint64_t map,
         uint64_t window,
         unsigned num)
{
	for (; !seen(&map, (unsigned)(seq & window)); seq >>= 1U) {
		if (!--num)
			return map;
	}
	return 0;
}

int
debruijn_normalize (uint64_t *word,
                    unsigned  order)
{
	if (!word)
		return EFAULT;
	if (order < DEBRUIJN_MIN_ORDER || order > DEBRUIJN_MAX_ORDER)
		return EINVAL;

	unsigned length = 1U << order;
	uint64_t mask = UINT64_MAX >> (64 - length);
	uint64_t seq = *word;
	if (seq & ~mask)
		return EINVAL;

	/* First the windows that start at the length - order + 1 lowest places,
	 * then in a rotation the order - 1 that run over the end of the word. */
	uint64_t window = (UINT64_C(1) << order) - 1U;
	uint64_t over = ((seq << (order - 1U)) | (seq >> (length - order + 1U))) & mask;
	uint64_t map = windows(seq, 0, window, length - order + 1U);
	if (!map || !windows(over, map, window, order - 1U))
		return EINVAL;

	/* Every value is in exactly one window, zero too, so exactly one rotation
	 * starts with order zero bits. Rotate the word to it. */
	while (seq >> (length - order))
		seq = ((seq << 1U) & mask) | (seq >> (length - 1U));
	*word = seq;
	return 0;
}

struct debruijn_generator
debruijn_generator (uint64_t seed,
                    unsigned order)
{
	if (order < DEBRUIJN_MIN_ORDER || order >= DEBRUIJN_MAX_ORDER)
		return (struct debruijn_generator){ .error = EINVAL };

	struct tree input = tree(seed, order);
	if (input.error)
		return (struct debruijn_generator){ .error = input.error };

	struct debruijn_generator ret = { .order = order };
	unsigned vertices = 1U << (order - 1U);
	for (unsigned vertex = 0; vertex < vertices; ++vertex)
		ret.edge[vertex] = input.edge[vertex];
	return ret;
}

int
debruijn_generate (uint64_t                         *word,
                   struct debruijn_generator const *src,
                   unsigned                         choices)
{
	if (!word || !src)
		return EFAULT;
	if (src->error)
		return src->error;

	unsigned choice_bits = (1U << (src->order - 1)) - 1;
	if (choices >> choice_bits)
		return EINVAL;

	struct tree output = tree_lift(src, choices);
	if (output.error)
		return output.error;

	*word = tree_word(&output);
	return 0;
}

int
debruijn_expand (uint64_t *word,
                 uint64_t  seed,
                 unsigned  order,
                 unsigned  choices)
{
	if (!word)
		return EFAULT;
	struct debruijn_generator src = debruijn_generator(seed, order);
	return debruijn_generate(word, &src, choices);
}

int
debruijn_collapse (uint64_t *seed,
                   unsigned *choices,
                   uint64_t  word,
                   unsigned  order)
{
	if (!seed || !choices)
		return EFAULT;
	if (order <= DEBRUIJN_MIN_ORDER || order > DEBRUIJN_MAX_ORDER)
		return EINVAL;

	struct tree input = tree(word, order);
	if (input.error)
		return input.error;

	unsigned vertices = 1U << (order - 1);
	unsigned incoming[MAX_VERTICES] = {};

	for (unsigned vertex = 1; vertex < vertices; ++vertex)
		++incoming[input.edge[vertex] & (vertices - 1)];

	uint64_t leaves = 0;
	for (unsigned vertex = 0; vertex < vertices; ++vertex) {
		if (!incoming[vertex])
			leaves |= UINT64_C(1) << vertex;
	}

	struct queue queues[MAX_VERTICES / 2] = {};
	unsigned lower_vertices = vertices >> 1U;

	/* Reverse the lift: prune the smallest leaf and rebuild the popped queues. */
	for (unsigned i = 0; i < vertices; ++i) {
		unsigned leaf = take_first(&leaves);
		unsigned parent = ROOT;

		if (leaf) {
			parent = input.edge[leaf] & (vertices - 1);
			if (!--incoming[parent])
				leaves |= UINT64_C(1) << parent;
		}

		struct queue *queue = &queues[leaf & (lower_vertices - 1)];
		queue->edge[queue->used++] = parent;
	}

	struct tree output = { .edge = { ROOT }, .order = order - 1 };
	unsigned recovered = 0;

	for (unsigned vertex = 1; vertex < lower_vertices; ++vertex) {
		/* The bit is set if the first edge ends in 0 (tree_lift()). */
		recovered |= (~queues[vertex].edge[0] & 1U) << (vertex - 1);
		output.edge[vertex] = queues[vertex].edge[1];
	}

	*seed = tree_word(&output);
	*choices = recovered;
	return 0;
}
