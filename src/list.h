/** @file Minimal intrusive circular doubly-linked list. */
#ifndef TWINCEPTION_SRC_LIST_H_
#define TWINCEPTION_SRC_LIST_H_

struct list {
	struct list *next;
	struct list *prev;
};

static inline void
list_init (struct list *list)
{
	list->next = list;
	list->prev = list;
}

static inline int
list_is_empty (struct list const *list)
{
	return list->next == list;
}

static inline void
list_append (struct list *list,
             struct list *node)
{
	node->prev = list->prev;
	node->next = list;
	list->prev->next = node;
	list->prev = node;
}

static inline void
list_del (struct list *node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
	list_init(node);
}

#define list_foreach(pos, head) \
	for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define list_foreach_safe(pos, tmp, head) \
	for ((pos) = (head)->next, (tmp) = (pos)->next; \
	     (pos) != (head); \
	     (pos) = (tmp), (tmp) = (pos)->next)

#endif /* TWINCEPTION_SRC_LIST_H_ */
