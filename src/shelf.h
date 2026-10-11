/** @file Shelves: finished objects a thread keeps for reuse, and gives back
 *  under memory pressure (design section 5). */
#ifndef TWINCEPTION_SRC_SHELF_H_
#define TWINCEPTION_SRC_SHELF_H_

#include <stddef.h>

#include "buf.h"
#include "list.h"

/** Finished objects kept for reuse, the last kept last, and the fewest kept
 *  through windows of reuse: those went unused.  Zeroed and its list
 *  initialized, it is empty. */
struct shelf {
	struct list list;
	size_t      n;
	size_t      low;   /**< fewest on it in this window */
	size_t      idle;  /**< fewest on it in the window before */
};

static inline void
shelf_put (struct shelf *shelf,
           struct list  *node)
{
	list_append(&shelf->list, node);
	++shelf->n;
}

/** Take the object kept last off the shelf, whose storage is the likeliest to
 *  fit; nullptr if none is kept. */
static inline struct list *
shelf_take (struct shelf *shelf)
{
	struct list *node = shelf->n ? shelf->list.prev : nullptr;
	if (node) {
		list_del(node);
		if (--shelf->n < shelf->low)
			shelf->low = shelf->n;
	}
	return node;
}

/** Take the object kept first off the shelf, to free it: it counts as one that
 *  went unused, if any did. */
static inline struct list *
shelf_drop (struct shelf *shelf)
{
	struct list *node = shelf->list.next;
	list_del(node);
	--shelf->n;
	shelf->low -= shelf->low > 0;
	shelf->idle -= shelf->idle > 0;
	return node;
}

/** How many of the objects kept a relief at @p level frees: at BUF_HIGH those
 *  unused through the window before and this one so far, at BUF_CRITICAL all. */
static inline size_t
shelf_unused (struct shelf const *shelf,
              enum buf_pressure   level)
{
	return level >= BUF_CRITICAL ? shelf->n
	     : shelf->low < shelf->idle ? shelf->low : shelf->idle;
}

/** Begin a window of reuse, as buf_stash_age() does for a stash. */
static inline void
shelf_age (struct shelf *shelf)
{
	shelf->idle = shelf->low;
	shelf->low = shelf->n;
}

#endif /* TWINCEPTION_SRC_SHELF_H_ */
