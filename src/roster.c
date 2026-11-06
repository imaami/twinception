/** @file roster.c
 *
 * The roster.
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "htab.h"
#include "loop.h"
#include "roster.h"

/* The number of buckets of the table, a power of two. */
enum { BUCKETS = 1024 };

struct roster {
	pthread_mutex_t lock;
	struct htab     table;
};

struct roster *
roster_create (int *e)
{
	struct roster *r = calloc(1, sizeof *r);
	if (!r) {
		*e = ENOMEM;
		return nullptr;
	}
	*e = htab_init(&r->table, BUCKETS);
	if (!*e && !(*e = pthread_mutex_init(&r->lock, nullptr)))
		return r;
	htab_fini(&r->table);
	free(r);
	return nullptr;
}

void
roster_destroy (struct roster **p_roster)
{
	struct roster *r = *p_roster;
	if (!r)
		return;
	*p_roster = nullptr;
	pthread_mutex_destroy(&r->lock);
	htab_fini(&r->table);
	free(r);
}

struct roster_call *
roster_enter (struct roster  *roster,
              struct span     key,
              uint64_t        hash,
              struct loop    *loop,
              struct handle   who,
              void          (*fn) (struct loop *loop,
                                   struct msg  *msg),
              int            *e)
{
	struct roster_call *c = malloc(sizeof *c + key.len);
	if (!c) {
		*e = ENOMEM;
		return nullptr;
	}
	c->cancel = (struct msg){ .fn = fn };
	c->node.hash = hash;
	c->hit = nullptr;
	c->loop = loop;
	c->len = key.len;
	c->who = who;
	atomic_init(&c->refs, 1);
	c->asked = 0;
	memcpy(c->key, key.ptr, key.len);
	pthread_mutex_lock(&roster->lock);
	htab_link(&roster->table, &c->node);
	pthread_mutex_unlock(&roster->lock);
	*e = 0;
	return c;
}

void
roster_leave (struct roster       *roster,
              struct roster_call **p_call)
{
	struct roster_call *c = *p_call;
	if (!c)
		return;
	*p_call = nullptr;
	pthread_mutex_lock(&roster->lock);
	htab_unlink(&roster->table, &c->node);
	pthread_mutex_unlock(&roster->lock);
	roster_call_put(c);
}

/* Returns true if no client has asked to cancel the call of the record of
 * @p node yet and the key of the record is @p key, a struct span.
 */
static bool
roster_call_match (struct htab_node const *node,
                   void const             *key)
{
	struct roster_call const *c = container_of(node, struct roster_call const, node);
	struct span const *k = key;
	return !c->asked && c->len == k->len && !memcmp(c->key, k->ptr, k->len);
}

void
roster_cancel (struct roster *roster,
               struct span    key,
               uint64_t       hash)
{
	struct roster_call *hits = nullptr;
	pthread_mutex_lock(&roster->lock);
	/* A matched record is asked, so the next search passes it by. */
	for (struct htab_node *n; (n = *htab_find(&roster->table, hash, roster_call_match, &key)); ) {
		struct roster_call *c = container_of(n, struct roster_call, node);
		c->asked = 1;
		atomic_fetch_add_explicit(&c->refs, 1, memory_order_relaxed);
		c->hit = hits;
		hits = c;
	}
	pthread_mutex_unlock(&roster->lock);
	/* The loop of a record can free the record as soon as the message is
	 * posted, so the function reads hit before it posts. A message that
	 * loop_post() refuses never runs, so the function drops the reference
	 * of that message itself. */
	while (hits) {
		struct roster_call *c = hits;
		hits = c->hit;
		if (loop_post(c->loop, &c->cancel))
			roster_call_put(c);
	}
}

void
roster_call_put (struct roster_call *call)
{
	if (call && atomic_fetch_sub_explicit(&call->refs, 1, memory_order_acq_rel) == 1)
		free(call);
}
