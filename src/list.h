/** @file list.h
 *
 * Minimal intrusive circular doubly-linked list.
 */
#ifndef TWINCEPTION_SRC_LIST_H_
#define TWINCEPTION_SRC_LIST_H_

#include "compat.h"

struct list {
	struct list *next;
	struct list *prev;
};

static force_inline struct list
list (struct list *self_pointer)
{
	return (struct list){
		.next = self_pointer,
		.prev = self_pointer
	};
}

static force_inline void
list_init (struct list *dest)
{
	*dest = list(dest);
}

static force_inline bool
list_is_empty (struct list const *ptr)
{
	return ptr == (struct list const *)ptr->next;
}

static inline void
list_append (struct list *dest,
             struct list *node)
{
	node->prev = dest->prev;
	node->next = dest;
	dest->prev->next = node;
	dest->prev = node;
}

static force_inline void
list_del (struct list *node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
	list_init(node);
}

/** Move every node of @p src to the end of @p dest, leaving @p src empty. */
static inline void
list_transplant (struct list *dest,
                 struct list *src)
{
	if (!list_is_empty(src)) {
		src->next->prev = dest->prev;
		dest->prev->next = src->next;
		src->prev->next = dest;
		dest->prev = src->prev;
		list_init(src);
	}
}

#define list_foreach(pos, head) \
        for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define list_foreach_safe(pos, tmp, head) \
        for ((pos) = (head)->next, (tmp) = (pos)->next; \
             (pos) != (head); \
             (pos) = (tmp), (tmp) = (pos)->next)

#endif /* TWINCEPTION_SRC_LIST_H_ */
