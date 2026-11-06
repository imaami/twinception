/** @file ref.h
 *
 * Reference counts. An object counts the references to it in an atomic
 * unsigned integer of any width, and the holder of the last reference
 * frees the object.
 */
#ifndef ASKAI_SRC_REF_H_
#define ASKAI_SRC_REF_H_

#include <stdatomic.h>

/** Add a reference to the count at @p refs. The caller holds a reference,
 *  or a lock that keeps one from being dropped, so the object stays and
 *  the addition needs no ordering.
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

#endif /* ASKAI_SRC_REF_H_ */
