/**
 * Binary de Bruijn sequences of the orders 2 to 8 in up to four 64-bit
 * limbs: the tree-array bijection of debruijn.c with wider arrays and
 * bitmaps. debruijn.c describes the graph, the three stages of an
 * expansion and the order of the labels.
 *
 * A sequence of order 8 is a tour of a graph of 128 vertices and 256
 * edges, so an edge label needs 8 bits, a bitmap of the vertices needs two
 * limbs and a bitmap of the edges needs four.
 */

#include <errno.h>
#include <stdbit.h>
#include <stdint.h>

#include "compat.h"
#include "debruijn_wide.h"

enum {
	DEBRUIJN_WIDE_ROOT = 1 << DEBRUIJN_WIDE_MAX_ORDER, /* no edge label */
};

/* A last-exit tree: edge[v] is the last edge out of the nonroot vertex v,
 * and edge[0] is DEBRUIJN_WIDE_ROOT.
 */
struct debruijn_wide_tree {
	uint16_t edge[DEBRUIJN_WIDE_VERTICES];
};

/* The two edges that a lift takes at a vertex, and how many of them it
 * took.
 */
struct debruijn_wide_queue {
	uint16_t edge[2];
	uint16_t used;
};

/* Clears the lowest set bit of the two limbs of @p set and returns its
 * position. At least one bit of @p set is set.
 */
static force_inline unsigned
debruijn_wide_take (uint64_t set[static 2])
{
	unsigned const limb = !set[0];
	unsigned const first = stdc_trailing_zeros(set[limb]);
	set[limb] &= set[limb] - 1U;
	return limb << 6U | first;
}

/* Returns true if a bit of @p seq above its 2^order bits is set. */
static bool
debruijn_wide_over (struct debruijn_wide const *seq,
                    unsigned                    order)
{
	unsigned const bits = 1U << order;
	uint64_t over = bits < 64U ? seq->limb[0] >> bits : 0;
	for (unsigned limb = (bits + 63U) >> 6U; limb < DEBRUIJN_WIDE_LIMBS; ++limb)
		over |= seq->limb[limb];
	return over;
}

/* Checks the normalized sequence @p seq of the order @p order, from 2 to
 * 8, and returns the last exit of each nonroot vertex. Stores 0 in @p *e,
 * or EINVAL for a number that is no normalized sequence of the order. The
 * tree of such a number is incomplete.
 */
static struct debruijn_wide_tree
debruijn_wide_tree (struct debruijn_wide const *seq,
                    unsigned                    order,
                    int                        *e)
{
	unsigned const length = 1U << order;
	struct debruijn_wide_tree ret = { .edge = { DEBRUIJN_WIDE_ROOT } };
	*e = EINVAL;
	if (debruijn_wide_over(seq, order) || debruijn_wide_window(seq, order, length - order))
		return ret;

	uint64_t seen[DEBRUIJN_WIDE_LIMBS] = {};
	/* The edge i of the tour is the order bits that start i bits under the
	 * top bit, with the first of them as the highest bit of the label. */
	for (unsigned i = 0; i < length; ++i) {
		unsigned const edge = debruijn_wide_window(seq, order,
		                                           (length - order - i) & (length - 1U));
		if (debruijn_wide_test(seen, edge))
			return ret;
		debruijn_wide_set(seen, edge);
		if (edge >> 1U)
			ret.edge[edge >> 1U] = (uint16_t)edge;
	}
	*e = 0;
	return ret;
}

/* Returns the sequence of the order @p order of the Euler tour that takes
 * the edge of the tree @p src last at each nonroot vertex.
 */
static struct debruijn_wide
debruijn_wide_tree_seq (struct debruijn_wide_tree const *src,
                        unsigned                         order)
{
	unsigned const vertices = 1U << (order - 1U);
	uint64_t visited[2] = {};
	struct debruijn_wide seq = {};
	unsigned vertex = 0;
	for (unsigned i = 0; i < 2U * vertices; ++i) {
		/* The first bit of each edge is a bit of the sequence, from the
		 * top bit down. */
		if (vertex >> (order - 2U))
			debruijn_wide_set(seq.limb, 2U * vertices - 1U - i);
		bool const again = debruijn_wide_test(visited, vertex);
		debruijn_wide_set(visited, vertex);
		/* At zero the edge 0, then the edge 1. At another vertex the edge
		 * that is not in the tree, then the edge of the tree. */
		unsigned const edge = vertex ? src->edge[vertex] ^ (again ? 0U : 1U) : again;
		vertex = edge & (vertices - 1U);
	}
	return seq;
}

/* Lifts the tree of the seed @p src to the next order, with one choice bit
 * of @p choices for each nonroot vertex of the seed. The order of @p src is
 * from 2 to 7. Stores 0 in @p *e, or EINVAL if the edges of @p src are no
 * last-exit tree, which the constructor never returns. The tree of such a
 * generator is incomplete.
 */
static struct debruijn_wide_tree
debruijn_wide_generator_lift (struct debruijn_wide_generator const *src,
                              uint64_t                              choices,
                              int                                  *e)
{
	unsigned const vertices = 1U << (src->order - 1U);
	struct debruijn_wide_queue queues[DEBRUIJN_WIDE_VERTICES / 2] = {
		[0] = { .edge = { 0, DEBRUIJN_WIDE_ROOT } }
	};
	uint8_t occurrences[DEBRUIJN_WIDE_VERTICES] = { [0] = 1 };
	struct debruijn_wide_tree ret = { .edge = { DEBRUIJN_WIDE_ROOT } };

	*e = EINVAL;
	for (unsigned vertex = 1; vertex < vertices; ++vertex) {
		/* first is the out-edge that ends in 1 for a clear choice bit and
		 * the one that ends in 0 for a set bit (debruijn.c). */
		unsigned const choice = (unsigned)(choices >> (vertex - 1U)) & 1U;
		unsigned const first = vertex << 1U | (choice ^ 1U);
		unsigned const last = src->edge[vertex];
		/* The last exit of a vertex is an edge out of the vertex. */
		if (last >> 1U != vertex)
			return ret;
		queues[vertex] = (struct debruijn_wide_queue){
			.edge = { (uint16_t)first, (uint16_t)last }
		};
		++occurrences[first];
		++occurrences[last];
	}

	/* A label can be taken when it is in no queue. */
	uint64_t ready[2] = {};
	for (unsigned edge = 0; edge < 2U * vertices; ++edge) {
		if (!occurrences[edge])
			debruijn_wide_set(ready, edge);
	}

	/* The loop needs no check of ready or of a queue. A label is taken
	 * once, and it reads one entry of the queue of the vertex where it
	 * ends. Two labels end at each vertex, so no queue is read past its
	 * two entries. While the root is in its queue, the entries with a
	 * label that are not read yet are one fewer than the labels that are
	 * not taken yet. One of those labels is therefore in no queue, and
	 * ready is not empty. The label that reads the root ends the loop. */
	for (unsigned taken = 1;; ++taken) {
		unsigned const edge = debruijn_wide_take(ready);
		struct debruijn_wide_queue *queue = &queues[edge & (vertices - 1U)];
		unsigned const next = queue->edge[queue->used++];
		if (next == DEBRUIJN_WIDE_ROOT) {
			/* The tree edge of a label leads to the label that it
			 * read, which is taken later. If all labels were taken,
			 * their edges are therefore a tree to the last label. If
			 * some last exits of src are a cycle, their labels never
			 * leave the queues, and fewer labels were taken. */
			*e = taken == 2U * vertices ? 0 : EINVAL;
			return ret;
		}
		/* The edges of the seed are the vertices of the next order. */
		ret.edge[edge] = (uint16_t)(edge << 1U | (next & 1U));
		if (!--occurrences[next])
			debruijn_wide_set(ready, next);
	}
}

bool
debruijn_wide_valid (struct debruijn_wide const *seq,
                     unsigned                    order)
{
	if (!seq || order < DEBRUIJN_WIDE_MIN_ORDER || order > DEBRUIJN_WIDE_MAX_ORDER)
		return false;
	int e;
	(void)debruijn_wide_tree(seq, order, &e);
	return !e;
}

int
debruijn_wide_normalize (struct debruijn_wide *seq,
                         unsigned              order)
{
	if (!seq)
		return EFAULT;
	if (order < DEBRUIJN_WIDE_MIN_ORDER || order > DEBRUIJN_WIDE_MAX_ORDER ||
	    debruijn_wide_over(seq, order))
		return EINVAL;

	/* Every value of order bits is at exactly one index, the value 0 too:
	 * its index is the lowest of the order zero bits. */
	unsigned const bits = 1U << order;
	uint64_t seen[DEBRUIJN_WIDE_LIMBS] = {};
	unsigned zeros = 0;
	for (unsigned index = 0; index < bits; ++index) {
		unsigned const value = debruijn_wide_window(seq, order, index);
		if (debruijn_wide_test(seen, value))
			return EINVAL;
		debruijn_wide_set(seen, value);
		if (!value)
			zeros = index;
	}

	/* A rotation to the left by turn bits moves the zero bits to the top
	 * of the 2^order bits. */
	unsigned const turn = (bits - order - zeros) & (bits - 1U);
	struct debruijn_wide out = {};
	if (bits < 64U) {
		uint64_t const low = seq->limb[0];
		out.limb[0] = (low << turn | low >> ((bits - turn) & (bits - 1U))) &
		              (UINT64_MAX >> (64U - bits));
	} else {
		unsigned const limbs = bits >> 6U;
		for (unsigned limb = 0; limb < limbs; ++limb) {
			/* The lowest bit of the limb of the result is the bit from
			 * of seq. */
			unsigned const from = (limb * 64U - turn) & (bits - 1U);
			unsigned const shift = from & 63U;
			out.limb[limb] = seq->limb[from >> 6U] >> shift;
			if (shift)
				out.limb[limb] |= seq->limb[((from >> 6U) + 1U) & (limbs - 1U)]
				                  << (64U - shift);
		}
	}
	*seq = out;
	return 0;
}

struct debruijn_wide_generator
debruijn_wide_generator (struct debruijn_wide const *seed,
                         unsigned                    order,
                         int                        *e)
{
	struct debruijn_wide_generator ret = {};
	if (!e)
		return ret;
	*e = seed ? EINVAL : EFAULT;
	if (!seed || order < DEBRUIJN_WIDE_MIN_ORDER || order >= DEBRUIJN_WIDE_MAX_ORDER)
		return ret;

	struct debruijn_wide_tree const input = debruijn_wide_tree(seed, order, e);
	if (*e)
		return ret;
	ret.order = order;
	for (unsigned vertex = 0; vertex < 1U << (order - 1U); ++vertex)
		ret.edge[vertex] = input.edge[vertex];
	return ret;
}

int
debruijn_wide_generate (struct debruijn_wide                 *seq,
                        struct debruijn_wide_generator const *src,
                        uint64_t                              choices)
{
	if (!seq || !src)
		return EFAULT;
	if (src->order < DEBRUIJN_WIDE_MIN_ORDER || src->order >= DEBRUIJN_WIDE_MAX_ORDER ||
	    choices >> ((1U << (src->order - 1U)) - 1U))
		return EINVAL;

	int e;
	struct debruijn_wide_tree const output = debruijn_wide_generator_lift(src, choices, &e);
	if (e)
		return e;
	*seq = debruijn_wide_tree_seq(&output, src->order + 1U);
	return 0;
}

int
debruijn_wide_expand (struct debruijn_wide       *seq,
                      struct debruijn_wide const *seed,
                      unsigned                    order,
                      uint64_t                    choices)
{
	if (!seq)
		return EFAULT;
	int e;
	struct debruijn_wide_generator const src = debruijn_wide_generator(seed, order, &e);
	return e ? e : debruijn_wide_generate(seq, &src, choices);
}

int
debruijn_wide_collapse (struct debruijn_wide       *seed,
                        uint64_t                   *choices,
                        struct debruijn_wide const *seq,
                        unsigned                    order)
{
	if (!seed || !choices || !seq)
		return EFAULT;
	if (order <= DEBRUIJN_WIDE_MIN_ORDER || order > DEBRUIJN_WIDE_MAX_ORDER)
		return EINVAL;

	int e;
	struct debruijn_wide_tree const input = debruijn_wide_tree(seq, order, &e);
	if (e)
		return e;

	unsigned const vertices = 1U << (order - 1U);
	uint8_t incoming[DEBRUIJN_WIDE_VERTICES] = {};
	for (unsigned vertex = 1; vertex < vertices; ++vertex)
		++incoming[input.edge[vertex] & (vertices - 1U)];

	uint64_t leaves[2] = {};
	for (unsigned vertex = 0; vertex < vertices; ++vertex) {
		if (!incoming[vertex])
			debruijn_wide_set(leaves, vertex);
	}

	struct debruijn_wide_queue queues[DEBRUIJN_WIDE_VERTICES / 2] = {};
	unsigned const lower = vertices >> 1U;
	/* The lift in reverse: remove the smallest leaf, and store its parent
	 * in the queue that the lift took the leaf from. The last exit of a
	 * vertex leads to a vertex that the tour leaves later, so the last
	 * exits are a tree, and a tree of the vertices that are left contains
	 * a leaf: leaves is not empty. */
	for (unsigned i = 0; i < vertices; ++i) {
		unsigned const leaf = debruijn_wide_take(leaves);
		unsigned parent = DEBRUIJN_WIDE_ROOT;
		if (leaf) {
			parent = input.edge[leaf] & (vertices - 1U);
			if (!--incoming[parent])
				debruijn_wide_set(leaves, parent);
		}
		struct debruijn_wide_queue *queue = &queues[leaf & (lower - 1U)];
		queue->edge[queue->used++] = (uint16_t)parent;
	}

	struct debruijn_wide_tree output = { .edge = { DEBRUIJN_WIDE_ROOT } };
	uint64_t recovered = 0;
	for (unsigned vertex = 1; vertex < lower; ++vertex) {
		/* The choice bit is set if the first edge of the queue ends in 0. */
		recovered |= (uint64_t)(~queues[vertex].edge[0] & 1U) << (vertex - 1U);
		output.edge[vertex] = queues[vertex].edge[1];
	}
	*seed = debruijn_wide_tree_seq(&output, order - 1U);
	*choices = recovered;
	return 0;
}
