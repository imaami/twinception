/** @file shelf.h
 *
 * Shelves: finished objects a thread keeps for reuse, and gives back
 * under memory pressure (design section 5).
 */
#ifndef TWINCEPTION_SRC_SHELF_H_
#define TWINCEPTION_SRC_SHELF_H_

#include <stddef.h>

#include "buf.h"
#include "list.h"

/** Finished objects kept for reuse, the last kept last, and the fewest kept
 *  through windows of reuse: those went unused. Zeroed and its list
 *  initialized, it is empty.
 */
struct shelf {
	struct list     list;
	struct buf_kept kept;
};

static inline void
shelf_put (struct shelf *shelf,
           struct list  *node)
{
	list_append(&shelf->list, node);
	++shelf->kept.n;
}

/** Take the object kept last off the shelf, whose storage is the likeliest to
 *  fit; nullptr if none is kept.
 */
static inline struct list *
shelf_take (struct shelf *shelf)
{
	struct list *node = shelf->kept.n ? shelf->list.prev : nullptr;
	if (node) {
		list_del(node);
		buf_kept_take(&shelf->kept);
	}
	return node;
}

/** Take the object kept first off the shelf, to free it: it counts as one that
 *  went unused, if any did.
 */
static inline struct list *
shelf_drop (struct shelf *shelf)
{
	struct list *node = shelf->list.next;
	list_del(node);
	buf_kept_drop(&shelf->kept);
	return node;
}

/** How many of the objects kept a relief at @p level frees
 *  (buf_kept_unused()).
 */
static inline size_t
shelf_unused (struct shelf const *shelf,
              enum buf_pressure   level)
{
	return buf_kept_unused(&shelf->kept, level);
}

/** Begin a window of reuse, as buf_stash_age() does for a stash. */
static inline void
shelf_age (struct shelf *shelf)
{
	buf_kept_age(&shelf->kept);
}

#endif /* TWINCEPTION_SRC_SHELF_H_ */
