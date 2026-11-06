/** @file htab.h
 *
 * An intrusive hash table with chained buckets. A record embeds a struct
 * htab_node, and the nodes of each bucket form a singly linked chain. The
 * number of buckets is a power of two, and a node is in the bucket whose
 * index is its hash masked with the number of buckets minus 1. The caller
 * computes the hashes, compares the keys and does the locking.
 */
#ifndef TWINCEPTION_SRC_HTAB_H_
#define TWINCEPTION_SRC_HTAB_H_

#include <errno.h>
#include <stdbit.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "compat.h"

/** The part of a record that links it into a bucket. */
struct htab_node {
	uint64_t          hash;  /**< the hash of the record's key */
	struct htab_node *next;  /**< the next node of the bucket, or nullptr */
};

/** A hash table. htab_init() allocates its buckets, or the caller
 *  provides them to htab().
 */
struct htab {
	struct htab_node **buckets;  /**< the first node of each bucket */
	size_t             mask;     /**< the number of buckets minus 1 */
	size_t             n;        /**< the number of nodes in the table */
};

/** Returns true if the key of the record of @p node equals @p key. */
typedef bool htab_match_fn (struct htab_node const *node,
                            void const             *key);

/** Returns a table with the @p n buckets at @p buckets, a power of two of
 *  them, which the caller provides empty and keeps for the life of the
 *  table. The table must not grow, and htab_fini() must not be called on
 *  it.
 */
static inline struct htab
htab (struct htab_node **buckets,
      size_t             n)
{
	return (struct htab){ .buckets = buckets, .mask = n - 1 };
}

/** Initializes @p dest with @p n empty buckets. Returns 0, EINVAL if @p n
 *  is not a power of two, or ENOMEM.
 */
static inline int
htab_init (struct htab *dest,
           size_t       n)
{
	if (!stdc_has_single_bit(n))
		return EINVAL;
	struct htab_node **buckets = calloc(n, sizeof *buckets);
	if (!buckets)
		return ENOMEM;
	*dest = (struct htab){ .buckets = buckets, .mask = n - 1 };
	return 0;
}

/** Frees the buckets of @p t and zeroes @p t. The function does not
 *  change or free the nodes.
 */
static inline void
htab_fini (struct htab *t)
{
	free(t->buckets);
	*t = (struct htab){};
}

/** Returns the address of the pointer to the first node of the bucket of
 *  @p hash.
 */
static inline struct htab_node **
htab_bucket (struct htab const *t,
             uint64_t           hash)
{
	return &t->buckets[hash & t->mask];
}

/** Inserts @p node, whose hash is set, as the first node of its bucket. */
static inline void
htab_link (struct htab      *t,
           struct htab_node *node)
{
	struct htab_node **first = htab_bucket(t, node->hash);
	node->next = *first;
	*first = node;
	++t->n;
}

/** Removes @p node, which is in @p t, from its bucket. The function walks
 *  the bucket up to the node.
 */
static inline void
htab_unlink (struct htab      *t,
             struct htab_node *node)
{
	struct htab_node **link = htab_bucket(t, node->hash);
	while (*link != node)
		link = &(*link)->next;
	*link = node->next;
	--t->n;
}

/**
 * Returns the address of the pointer to the first node of the bucket of
 * @p hash whose hash is @p hash and for which @p match returns true with
 * @p key. A nullptr @p match matches each node with the hash, for keys
 * that are their own hashes. If no node matches, the pointer at that
 * address is nullptr, and it is the end of the bucket's chain.
 */
static inline struct htab_node **
htab_find (struct htab const *t,
           uint64_t           hash,
           htab_match_fn     *match,
           void const        *key)
{
	struct htab_node **link = htab_bucket(t, hash);
	while (*link && ((*link)->hash != hash || (match && !match(*link, key))))
		link = &(*link)->next;
	return link;
}

/** Returns the node of @p t after @p node, or the first node if @p node
 *  is nullptr; nullptr after the last node. A walk can unlink or free each
 *  node once it has the node after it.
 */
static inline struct htab_node *
htab_next (struct htab const      *t,
           struct htab_node const *node)
{
	size_t i = 0;
	if (node) {
		if (node->next)
			return node->next;
		i = (node->hash & t->mask) + 1;
	}
	for (; i <= t->mask; ++i)
		if (t->buckets[i])
			return t->buckets[i];
	return nullptr;
}

/** Doubles the number of buckets of @p t and moves each node to its new
 *  bucket. Returns 0, or ENOMEM with @p t unchanged.
 */
static inline int
htab_grow (struct htab *t)
{
	size_t const n = 2 * (t->mask + 1);
	struct htab_node **buckets = calloc(n, sizeof *buckets);
	if (!buckets)
		return ENOMEM;
	for (size_t i = 0; i <= t->mask; ++i) {
		for (struct htab_node *node = t->buckets[i], *next; node; node = next) {
			next = node->next;
			struct htab_node **first = &buckets[node->hash & (n - 1)];
			node->next = *first;
			*first = node;
		}
	}
	free(t->buckets);
	t->buckets = buckets;
	t->mask = n - 1;
	return 0;
}

#endif /* TWINCEPTION_SRC_HTAB_H_ */
