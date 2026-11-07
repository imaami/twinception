/** @file ref.h
 *
 * Reference counts. An object counts the references to it in an
 * _Atomic uint32_t or _Atomic uint64_t, and the holder of the last
 * reference frees the object. 0 and a set top bit mean "don't touch":
 * ref_tryget() refuses both, and ref_put() never returns true for them.
 * The top bit is set by too many references, or by a ref_put() of more
 * than the count.
 */
#ifndef ASKAI_SRC_REF_H_
#define ASKAI_SRC_REF_H_

#include <stdatomic.h>
#include <stdint.h>

/** Add a reference to the count at @p refs. The caller holds a reference,
 *  so the count is at least 1 and the addition needs no check and no
 *  ordering.
 */
#define ref_get(refs) \
        ((void)atomic_fetch_add_explicit((refs), 1, memory_order_relaxed))

/** Drop @p n references from the count at @p refs. Evaluates to true if
 *  they were the last, and then the caller frees the object. The release
 *  orders each holder's accesses before its drop, and the acquire orders
 *  the free after them. @p n is evaluated twice.
 */
#define ref_put(refs, n) \
        (atomic_fetch_sub_explicit((refs), (n), memory_order_acq_rel) == (n))

/** Add a reference to the count at @p refs unless it means "don't touch".
 *  Evaluates to true if it did. Use it for a pointer found in a table or a
 *  list, before unlocking the lock under which the last holder unlinks the
 *  object. On success the ordering is acquire, as in ref_put().
 */
#define ref_tryget(refs)                                   \
        _Generic((refs), _Atomic uint32_t *: ref_tryget32, \
                         _Atomic uint64_t *: ref_tryget64)(refs)

static inline bool
ref_tryget32 (_Atomic uint32_t *refs)
{
	uint32_t n = atomic_load_explicit(refs, memory_order_relaxed);
	do {
		if (!n || n >> 31)
			return false;
	} while (!atomic_compare_exchange_weak_explicit(refs, &n, n + 1, memory_order_acquire,
	                                                memory_order_relaxed));
	return true;
}

static inline bool
ref_tryget64 (_Atomic uint64_t *refs)
{
	uint64_t n = atomic_load_explicit(refs, memory_order_relaxed);
	do {
		if (!n || n >> 63)
			return false;
	} while (!atomic_compare_exchange_weak_explicit(refs, &n, n + 1, memory_order_acquire,
	                                                memory_order_relaxed));
	return true;
}

#endif /* ASKAI_SRC_REF_H_ */
