/**
 * The numeric order of the binary de Bruijn sequences of the orders 2 to
 * 8: debruijn_order.c for sequences of up to 256 bits. That file describes
 * the order of the Euler tours, and why the completions of a prefix of a
 * tour are counted by a determinant.
 *
 * debruijn_order.c computes each determinant anew. A sequence of order 8
 * needs up to 127 determinants of 127 rows, with values of up to 120 bits.
 * This file stores the inverse of the matrix. A step of a tour removes one
 * edge from the allowed last exits, which changes one row of the matrix.
 * The determinant after the change is the determinant before it times an
 * expression of two entries of the inverse, and the inverse after the
 * change costs one pass over the columns of the inverse that are read
 * later (the formula of Sherman and Morrison).
 *
 * The inverse of an integer matrix is not an integer matrix, so every
 * number here is a residue modulo the prime p = 2^127 - 1. A count is
 * under 2^121 and therefore equal to its residue, and the inverse modulo p
 * of a count that is not 0 exists.
 */
#include <errno.h>
#include <stdbit.h>
#include <stdint.h>
#include <stdlib.h>

#include "compat.h"
#include "debruijn_wide.h"

typedef unsigned _BitInt(128) u128_type;

/* A residue modulo p = 2^127 - 1, from 0 to p - 1: hi is under 2^63. */
struct debruijn_wide_mod {
	uint64_t lo;
	uint64_t hi;
};

/* Returns @p a + @p b modulo p. Each of them can also be p itself. */
static force_inline struct debruijn_wide_mod
debruijn_wide_mod_add (struct debruijn_wide_mod a,
                       struct debruijn_wide_mod b)
{
	/* The sum is under 2^128, and its bit 127 is worth 1 modulo p. */
	u128_type const sum = ((u128_type)a.hi << 64U | a.lo) + ((u128_type)b.hi << 64U | b.lo);
	uint64_t lo = (uint64_t)sum;
	uint64_t hi = (uint64_t)(sum >> 64U);
	uint64_t const carry = hi >> 63U;
	hi &= INT64_MAX;
	lo += carry;
	hi += lo < carry;
	if (lo == UINT64_MAX && hi == INT64_MAX)
		lo = hi = 0;
	return (struct debruijn_wide_mod){ lo, hi };
}

/* Returns @p a - @p b modulo p. */
static force_inline struct debruijn_wide_mod
debruijn_wide_mod_sub (struct debruijn_wide_mod a,
                       struct debruijn_wide_mod b)
{
	/* p is 127 set bits, so p - b is b with its 127 bits inverted. */
	return debruijn_wide_mod_add(a, (struct debruijn_wide_mod){ ~b.lo, ~b.hi & INT64_MAX });
}

/* Returns @p a times @p b modulo p. */
static force_inline struct debruijn_wide_mod
debruijn_wide_mod_mul (struct debruijn_wide_mod a,
                       struct debruijn_wide_mod b)
{
	/* The four products of the limbs, with the carries: t0 is the low
	 * limb, the low limb of t2 the second, and t3 the two high limbs. */
	u128_type const t0 = (u128_type)a.lo * b.lo;
	u128_type const t1 = (u128_type)a.lo * b.hi + (uint64_t)(t0 >> 64U);
	u128_type const t2 = (u128_type)a.hi * b.lo + (uint64_t)t1;
	u128_type const t3 = (u128_type)a.hi * b.hi + (uint64_t)(t1 >> 64U) + (uint64_t)(t2 >> 64U);
	uint64_t const p1 = (uint64_t)t2;
	uint64_t const p2 = (uint64_t)t3;
	uint64_t const p3 = (uint64_t)(t3 >> 64U);
	/* The product is under 2^254. 2^127 is 1 modulo p, so the product is
	 * its low 127 bits plus its other bits. */
	return debruijn_wide_mod_add(
		(struct debruijn_wide_mod){ (uint64_t)t0, p1 & INT64_MAX },
		(struct debruijn_wide_mod){ p2 << 1U | p1 >> 63U, p3 << 1U | p2 >> 63U });
}

/* Returns the inverse of @p a modulo p: @p a to the power p - 2. @p a is
 * not 0.
 */
static struct debruijn_wide_mod
debruijn_wide_mod_inverse (struct debruijn_wide_mod a)
{
	/* p - 2 is 2^127 - 3: 127 bits that are all set but bit 1. ret is a
	 * to the power of the bits from the top bit down to the bit bit. */
	struct debruijn_wide_mod ret = a;
	for (unsigned bit = 126; bit--;) {
		ret = debruijn_wide_mod_mul(ret, ret);
		if (bit != 1)
			ret = debruijn_wide_mod_mul(ret, a);
	}
	return ret;
}

/* The count of the completions of a prefix of a tour. det is the count:
 * the determinant of the matrix of the allowed last exits. inverse is the
 * inverse of that matrix, of size rows and size columns, row after row.
 * The row and the column of the vertex v are the row and column v - 1.
 */
struct debruijn_wide_counter {
	struct debruijn_wide_mod det;
	uint64_t                 size;
	struct debruijn_wide_mod inverse[];
};

/* Returns the entry of the inverse in the row of the vertex @p v and the
 * column of the vertex @p u, both not 0.
 */
static force_inline struct debruijn_wide_mod *
debruijn_wide_counter_at (struct debruijn_wide_counter *counter,
                          unsigned                      v,
                          unsigned                      u)
{
	return &counter->inverse[(v - 1U) * counter->size + (u - 1U)];
}

/* Allocates the counter of the empty prefix of the tours of the order
 * @p order, from 2 to 8: each exit of each nonroot vertex can be its last
 * exit. Returns the counter and 0 in @p *e, or nullptr and ENOMEM.
 */
static struct debruijn_wide_counter *
debruijn_wide_counter_create (unsigned  order,
                              int      *e)
{
	unsigned const vertices = 1U << (order - 1U);
	size_t const size = vertices - 1U;
	struct debruijn_wide_counter *counter =
		calloc(1, sizeof *counter + size * size * sizeof counter->inverse[0]);
	if (!counter) {
		*e = ENOMEM;
		return nullptr;
	}
	counter->size = size;
	struct debruijn_wide_mod const one = { 1, 0 };
	/* The matrix of debruijn_completions(). In the row of a vertex, the
	 * entry in the column of the vertex is the count of its exits, and the
	 * entry in the column of the vertex that an exit leads to is -1. The
	 * matrix is without the row and the column of vertex 0. */
	for (unsigned v = 1; v < vertices; ++v) {
		for (unsigned b = 0; b < 2; ++b) {
			unsigned const u = (2U * v + b) & (vertices - 1U);
			struct debruijn_wide_mod *const own =
				debruijn_wide_counter_at(counter, v, v);
			*own = debruijn_wide_mod_add(*own, one);
			if (!u)
				continue;
			struct debruijn_wide_mod *const to =
				debruijn_wide_counter_at(counter, v, u);
			*to = debruijn_wide_mod_sub(*to, one);
		}
	}
	/* The inverse in place, by the elimination of Gauss and Jordan. A
	 * pivot is the quotient of two leading minors. Those are counts of
	 * forests, which are not 0 and under p, so no pivot is 0 and the rows
	 * need no exchange. The determinant is the product of the pivots. */
	struct debruijn_wide_mod *const a = counter->inverse;
	counter->det = one;
	for (size_t k = 0; k < size; ++k) {
		struct debruijn_wide_mod *const row = &a[k * size];
		counter->det = debruijn_wide_mod_mul(counter->det, row[k]);
		struct debruijn_wide_mod const inverse = debruijn_wide_mod_inverse(row[k]);
		row[k] = one;
		for (size_t j = 0; j < size; ++j)
			row[j] = debruijn_wide_mod_mul(row[j], inverse);
		for (size_t i = 0; i < size; ++i) {
			struct debruijn_wide_mod *const other = &a[i * size];
			struct debruijn_wide_mod const factor = other[k];
			if (i == k || !(factor.lo | factor.hi))
				continue;
			other[k] = (struct debruijn_wide_mod){};
			for (size_t j = 0; j < size; ++j) {
				struct debruijn_wide_mod const part =
					debruijn_wide_mod_mul(factor, row[j]);
				other[j] = debruijn_wide_mod_sub(other[j], part);
			}
		}
	}
	*e = 0;
	return counter;
}

static void
debruijn_wide_counter_destroy (struct debruijn_wide_counter **p_counter)
{
	free(*p_counter);
	*p_counter = nullptr;
}

/* Returns the count without the exit @p b of the vertex @p v as an allowed
 * last exit, divided by the count with it.
 */
static struct debruijn_wide_mod
debruijn_wide_counter_ratio (struct debruijn_wide_counter *counter,
                             unsigned                      v,
                             unsigned                      b)
{
	/* The exit leads to u. size is the vertices minus one, a mask. */
	unsigned const u = (2U * v + b) & (unsigned)counter->size;
	struct debruijn_wide_mod const one = { 1, 0 };
	/* Without the exit, the row of v is smaller by 1 in the column of v
	 * and larger by 1 in the column of u. For a loop, u is v: the matrix
	 * is the same without it, and the ratio is 1. */
	struct debruijn_wide_mod ratio =
		debruijn_wide_mod_sub(one, *debruijn_wide_counter_at(counter, v, v));
	if (u)
		ratio = debruijn_wide_mod_add(ratio, *debruijn_wide_counter_at(counter, u, v));
	return ratio;
}

/* Returns the count without the exit @p b of the vertex @p v as an allowed
 * last exit.
 */
static u128_type
debruijn_wide_counter_without (struct debruijn_wide_counter *counter,
                               unsigned                      v,
                               unsigned                      b)
{
	struct debruijn_wide_mod const count =
		debruijn_wide_mod_mul(counter->det, debruijn_wide_counter_ratio(counter, v, b));
	return (u128_type)count.hi << 64U | count.lo;
}

/* allow[b] is a bitmap of the vertices whose exit b can still be their
 * last exit.
 */
struct debruijn_wide_exits {
	uint64_t allow[2][2];
};

/* Returns true if the tour has not left the nonroot vertex @p v yet: each
 * of its exits can still be its last exit.
 */
static force_inline bool
debruijn_wide_exits_open (struct debruijn_wide_exits const *exits,
                          unsigned                          v)
{
	return v && debruijn_wide_test(exits->allow[0], v) &&
	       debruijn_wide_test(exits->allow[1], v);
}

/* Removes the exit @p b of the vertex @p v from the allowed last exits. The
 * count without the exit is not 0. @p exits is the exits after the removal.
 *
 * The function changes only the columns of the vertices that the tour has
 * not left yet. A later call reads no other column: a ratio and a removal
 * read the column of the vertex that the tour leaves for the first time,
 * and a removal reads the other columns only to change them. The tour
 * leaves one more vertex before each removal, so the removals of a tour
 * change about half of the entries that they would change with all
 * columns.
 */
static void
debruijn_wide_counter_remove (struct debruijn_wide_counter     *counter,
                              unsigned                          v,
                              unsigned                          b,
                              struct debruijn_wide_exits const *exits)
{
	size_t const size = (size_t)counter->size;
	unsigned const u = (2U * v + b) & (unsigned)size;
	struct debruijn_wide_mod const ratio = debruijn_wide_counter_ratio(counter, v, b);
	struct debruijn_wide_mod const inverse = debruijn_wide_mod_inverse(ratio);
	struct debruijn_wide_mod *const a = counter->inverse;
	/* The inverse after the change is the inverse before it plus, in the
	 * row i and the column cols[k], column[i] times rows[k]: the column
	 * of v divided by the ratio, and the row of v minus the row of u.
	 * cols is the n columns that the function changes. For a loop, u is
	 * v, rows is zeros and nothing changes. */
	struct debruijn_wide_mod rows[DEBRUIJN_WIDE_VERTICES];
	struct debruijn_wide_mod column[DEBRUIJN_WIDE_VERTICES];
	uint8_t cols[DEBRUIJN_WIDE_VERTICES];
	size_t n = 0;
	for (size_t j = 0; j < size; ++j) {
		column[j] = debruijn_wide_mod_mul(a[j * size + (v - 1U)], inverse);
		/* The column j is the column of the vertex j + 1. */
		if (!debruijn_wide_exits_open(exits, (unsigned)j + 1U))
			continue;
		rows[n] = a[(v - 1U) * size + j];
		if (u)
			rows[n] = debruijn_wide_mod_sub(rows[n], a[(u - 1U) * size + j]);
		cols[n++] = (uint8_t)j;
	}
	for (size_t i = 0; i < size; ++i) {
		if (!(column[i].lo | column[i].hi))
			continue;
		struct debruijn_wide_mod *const row = &a[i * size];
		for (size_t k = 0; k < n; ++k) {
			struct debruijn_wide_mod const part =
				debruijn_wide_mod_mul(column[i], rows[k]);
			row[cols[k]] = debruijn_wide_mod_add(row[cols[k]], part);
		}
	}
	counter->det = debruijn_wide_mod_mul(counter->det, ratio);
}

/* Sets the bit of the step @p i of a tour of the order @p order in @p seq,
 * if the tour leaves with the bit @p b set. The bit of the step i is the
 * bit 2^order - order - i of the sequence. The last order - 1 steps of a
 * tour leave with 0 and are no bits of the sequence.
 */
static force_inline void
debruijn_wide_step (struct debruijn_wide *seq,
                    unsigned              order,
                    unsigned              i,
                    unsigned              b)
{
	unsigned const top = (1U << order) - order;
	if (b && i <= top)
		debruijn_wide_set(seq->limb, top - i);
}

int
debruijn_wide_rank (struct debruijn_wide       *rank,
                    struct debruijn_wide const *seq,
                    unsigned                    order)
{
	if (!rank || !seq)
		return EFAULT;
	if (!debruijn_wide_valid(seq, order))
		return EINVAL;
	int e;
	struct debruijn_wide_counter *counter = debruijn_wide_counter_create(order, &e);
	if (!counter)
		return e;
	unsigned const length = 1U << order;
	unsigned const vertices = length >> 1U;
	struct debruijn_wide_exits exits = {
		{ { UINT64_MAX, UINT64_MAX }, { UINT64_MAX, UINT64_MAX } }
	};
	u128_type k = 0;
	unsigned v = 0;
	/* The bits of the tour are the bits of the sequence from the bit
	 * length - order down, and then order - 1 zero bits. */
	for (unsigned i = 0; i < length; ++i) {
		unsigned const b = i <= length - order &&
		                   debruijn_wide_test(seq->limb, length - order - i);
		/* The first departure from v: the sequences that leave v with 0
		 * here are before those that leave it with 1. */
		if (debruijn_wide_exits_open(&exits, v)) {
			if (b)
				k += debruijn_wide_counter_without(counter, v, 0);
			debruijn_wide_clear(exits.allow[b], v);
			debruijn_wide_counter_remove(counter, v, b, &exits);
		}
		v = (v << 1U | b) & (vertices - 1U);
	}
	debruijn_wide_counter_destroy(&counter);
	*rank = (struct debruijn_wide){ { (uint64_t)k, (uint64_t)(k >> 64U) } };
	return 0;
}

/* debruijn_feasible() of debruijn_order.c: after the last exit of @p v is
 * the exit to @p u, it is enough that allowed exits lead from u to zero
 * without v. @p mask is the vertices minus one.
 */
static bool
debruijn_wide_exits_feasible (struct debruijn_wide_exits const *exits,
                              unsigned                          mask,
                              unsigned                          v,
                              unsigned                          u)
{
	uint64_t seen[2] = {};
	uint64_t todo[2] = {};
	debruijn_wide_set(seen, v);
	if (u != v)
		debruijn_wide_set(todo, u);
	while (todo[0] | todo[1]) {
		if (todo[0] & 1U)
			return true;
		unsigned const limb = !todo[0];
		unsigned const x = limb << 6U | stdc_trailing_zeros(todo[limb]);
		debruijn_wide_set(seen, x);
		todo[limb] &= todo[limb] - 1U;
		for (unsigned b = 0; b < 2; ++b) {
			unsigned const y = (2U * x + b) & mask;
			if (debruijn_wide_test(exits->allow[b], x) && !debruijn_wide_test(seen, y))
				debruijn_wide_set(todo, y);
		}
	}
	return false;
}

/* A page of debruijn_wide_ordered(): skip sequences are still to pass, and
 * left are still to emit. While skip is not 0, counter is the count of the
 * prefix of the tour.
 */
struct debruijn_wide_page {
	u128_type                     skip;
	debruijn_wide_emit_fn        *emit;
	void                         *arg;
	struct debruijn_wide_counter *counter;
	unsigned                      order;
	unsigned                      left;
};

/* debruijn_visit() of debruijn_order.c. @p seq is the bits of the sequence
 * of the first @p i steps of the tour, which is at the vertex @p v.
 */
static int
debruijn_wide_page_visit (struct debruijn_wide_page  *p,
                          struct debruijn_wide_exits  exits,
                          struct debruijn_wide        seq,
                          unsigned                    v,
                          unsigned                    i)
{
	unsigned const length = 1U << p->order;
	unsigned const mask = (length >> 1U) - 1U;
	for (; i < length; ++i) {
		if (debruijn_wide_exits_open(&exits, v)) {
			for (unsigned b = 0; b < 2 && p->left; ++b) {
				/* The tour leaves v with b now. The last exit of v
				 * is then the other bit, to the vertex last. */
				unsigned const last = (v << 1U | (1U - b)) & mask;
				struct debruijn_wide_exits child = exits;
				debruijn_wide_clear(child.allow[b], v);
				if (!b && p->skip) {
					u128_type const z =
						debruijn_wide_counter_without(p->counter, v, 0);
					if (p->skip >= z) {
						p->skip -= z;
						continue;
					}
				} else if (!debruijn_wide_exits_feasible(&child, mask, v, last)) {
					continue;
				}
				/* A tour that goes on with a skip counts again. */
				if (p->skip)
					debruijn_wide_counter_remove(p->counter, v, b, &child);
				struct debruijn_wide next = seq;
				debruijn_wide_step(&next, p->order, i, b);
				unsigned const to = (v << 1U | b) & mask;
				int const e = debruijn_wide_page_visit(p, child, next, to, i + 1U);
				if (e)
					return e;
			}
			return 0;
		}
		/* The second departure from a vertex is through its one allowed
		 * last exit. Zero leaves with 0, then with 1. */
		unsigned const b = v ? debruijn_wide_test(exits.allow[1], v) : i != 0;
		debruijn_wide_step(&seq, p->order, i, b);
		v = (v << 1U | b) & mask;
	}
	--p->left;
	return p->emit(&seq, p->order, p->arg);
}

int
debruijn_wide_ordered (unsigned                    order,
                       struct debruijn_wide const *first,
                       unsigned                    limit,
                       debruijn_wide_emit_fn      *emit,
                       void                       *arg)
{
	if (!emit || !first)
		return EFAULT;
	if (order < DEBRUIJN_WIDE_MIN_ORDER || order > DEBRUIJN_WIDE_MAX_ORDER)
		return EINVAL;
	/* The sequences of an order are 2^(2^(order-1) - order). */
	unsigned const exponent = (1U << (order - 1U)) - order;
	u128_type const skip = (u128_type)first->limb[1] << 64U | first->limb[0];
	if (!limit || first->limb[2] || first->limb[3] || skip >> exponent)
		return 0;
	int e = 0;
	struct debruijn_wide_page p = {
		.skip    = skip,
		.emit    = emit,
		.arg     = arg,
		.counter = skip ? debruijn_wide_counter_create(order, &e) : nullptr,
		.order   = order,
		.left    = limit,
	};
	if (e)
		return e;
	struct debruijn_wide_exits const all = {
		{ { UINT64_MAX, UINT64_MAX }, { UINT64_MAX, UINT64_MAX } }
	};
	e = debruijn_wide_page_visit(&p, all, (struct debruijn_wide){}, 0, 0);
	debruijn_wide_counter_destroy(&p.counter);
	return e;
}
