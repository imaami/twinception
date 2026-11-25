/** Numeric order for normalized Euler tours. At a first departure with bit
 * b, reserve exit 1-b for last. The reserved exits of all nonroot vertices
 * form a tree directed to zero. Conversely every such tree gives one tour:
 * take the unreserved exit first (at zero take 0 then 1). The outgoing bits
 * are the normalized word with its leading order-1 zeros moved to the end.
 * Thus taking first-departure branches 0 then 1 sorts by unsigned word.
 */
#include <errno.h>
#include <stdint.h>

#include "debruijn.h"

enum { VMAX = 1U << (DEBRUIJN_MAX_ORDER - 1) };
static_assert(VMAX == 32, "bitmap width and determinant bound");

/* allow[b], bit v: v may still reserve its outgoing edge b for last. */
struct exits {
	uint32_t allow[2];
};

unsigned
debruijn_word_count (unsigned order)
{
	if (order < DEBRUIJN_MIN_ORDER || order > DEBRUIJN_MAX_ORDER)
		return 0;
	return 1U << ((1U << (order - 1)) - order);
}

/* The directed Matrix-Tree theorem counts completions of this prefix:
 * delete row/column zero from the out-Laplacian and take its determinant.
 * Bareiss entries are minors. By row multilinearity a k-minor is a sum of
 * at most 2^k signed incidence minors, each 0 or +/-1. The numerator below
 * therefore fits int64_t (k <= 30 before the final division). No pivoting
 * is needed: if a completing tree exists, restricting it to any leading
 * vertex set gives a forest rooted outside that set, so its principal minor
 * is positive by the same theorem (contract the outside vertices to zero).
 */
static unsigned
debruijn_completions (unsigned     vertices,
                      struct exits exits)
{
	int64_t a[VMAX - 1][VMAX - 1] = {};
	unsigned const m = vertices - 1;
	for (unsigned v = 1; v < vertices; ++v) {
		for (unsigned b = 0; b < 2; ++b) {
			if (!(exits.allow[b] >> v & 1U))
				continue;
			unsigned u = (2 * v + b) & m;
			++a[v - 1][v - 1];
			if (u)
				--a[v - 1][u - 1];
		}
	}
	int64_t prev = 1;
	for (unsigned j = 0; j + 1 < m; ++j) {
		int64_t pivot = a[j][j];
		if (!pivot)
			return 0;
		for (unsigned i = j + 1; i < m; ++i)
			for (unsigned h = j + 1; h < m; ++h)
				a[i][h] = (pivot * a[i][h] - a[i][j] * a[j][h]) / prev;
		prev = pivot;
	}
	return (unsigned)a[m - 1][m - 1];
}

int
debruijn_rank (unsigned *rank,
               uint64_t  word,
               unsigned  order)
{
	if (!rank)
		return EFAULT;
	if (!debruijn_valid(word, order))
		return EINVAL;
	unsigned length = 1U << order;
	unsigned vertices = length >> 1U;
	struct exits exits = { { UINT32_MAX, UINT32_MAX } };
	uint64_t tour = word << (order - 1);
	unsigned k = 0;
	unsigned v = 0;
	for (unsigned i = 0; i < length; ++i, tour <<= 1) {
		unsigned b = (unsigned)(tour >> (length - 1)) & 1U;
		uint32_t bit = UINT32_C(1) << v;
		if (v && (exits.allow[0] & exits.allow[1] & bit)) {
			if (b) {
				struct exits zero = exits;
				zero.allow[0] &= ~bit;
				k += debruijn_completions(vertices, zero);
			}
			exits.allow[b] &= ~bit;
		}
		v = ((v << 1U) + b) & (vertices - 1);
	}
	*rank = k;
	return 0;
}

/* The parent prefix was feasible. After fixing v's last exit to u, it
 * suffices that u reach zero without v: any old path through v can then
 * use the new suffix. No determinants on the sequential part of a page.
 */
static bool
debruijn_feasible (unsigned     mask,
                   struct exits exits,
                   unsigned     v,
                   unsigned     u)
{
	uint32_t seen = UINT32_C(1) << v;
	uint32_t todo = (UINT32_C(1) << u) & ~seen;
	while (todo) {
		if (todo & 1U)
			return true;
		unsigned x = (unsigned)__builtin_ctz(todo);
		uint32_t bit = UINT32_C(1) << x;
		seen |= bit;
		todo &= ~bit;
		for (unsigned b = 0; b < 2; ++b)
			if (exits.allow[b] & bit)
				todo |= (UINT32_C(1) << ((2 * x + b) & mask)) & ~seen;
	}
	return false;
}

struct page {
	debruijn_emit_fn *emit;
	void             *arg;
	unsigned          vertices;
	unsigned          order;
	unsigned          skip;
	unsigned          left;
};

/* Recurse only on a new last-exit choice: depth <= vertices - 1. */
static int
debruijn_visit (struct page  *p,
                struct exits  exits,
                uint64_t      bits,
                unsigned      i)
{
	unsigned mask = p->vertices - 1U;
	for (; i < p->vertices << 1U; ++i) {
		unsigned v = (unsigned)bits & mask;
		uint32_t bit = UINT32_C(1) << v;
		if (v && (exits.allow[0] & exits.allow[1] & bit)) {
			for (unsigned b = 0; b < 2 && p->left; ++b) {
				struct exits child = exits;
				child.allow[b] &= ~bit;
				if (!b && p->skip) {
					unsigned z = debruijn_completions(p->vertices, child);
					if (p->skip >= z) {
						p->skip -= z;
						continue;
					}
				} else if (!debruijn_feasible(mask, child, v, ((v << 1U) + (1-b)) & mask)) {
					continue;
				}
				int e = debruijn_visit(p, child, bits << 1 | b, i + 1);
				if (e)
					return e;
			}
			return 0;
		}
		unsigned b = v ? !!(exits.allow[1] & bit) : i != 0;
		bits = bits << 1 | b;
	}
	--p->left;
	return p->emit(bits >> (p->order - 1), p->order, p->arg);
}

int
debruijn_ordered (unsigned          order,
                  unsigned          first,
                  unsigned          limit,
                  debruijn_emit_fn *emit,
                  void             *arg)
{
	if (!emit)
		return EFAULT;
	unsigned count = debruijn_word_count(order);
	if (!count)
		return EINVAL;
	if (!limit || first >= count)
		return 0;
	struct page p = { emit, arg, 1U << (order - 1), order, first, limit };
	return debruijn_visit(&p, (struct exits){ { UINT32_MAX, UINT32_MAX } }, 0, 0);
}
