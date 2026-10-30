/** @file roster.c
 *
 * The roster.
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "loop.h"
#include "roster.h"

/* The number of buckets of the table.  It is a power of two, because
 * BUCKETS - 1 is the mask that selects a bucket.
 */
enum { BUCKETS = 1024 };

static_assert(!(BUCKETS & (BUCKETS - 1)), "BUCKETS - 1 is a mask");

struct roster {
	pthread_mutex_t     lock;
	struct roster_call *buckets[BUCKETS];
};

struct roster *
roster_create (int *e)
{
	struct roster *r = malloc(sizeof *r);
	*e = r ? pthread_mutex_init(&r->lock, nullptr) : ENOMEM;
	if (*e) {
		free(r);
		return nullptr;
	}
	memset(r->buckets, 0, sizeof r->buckets);
	return r;
}

void
roster_destroy (struct roster **p_roster)
{
	struct roster *r = *p_roster;
	if (!r)
		return;
	*p_roster = nullptr;
	pthread_mutex_destroy(&r->lock);
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
	c->hit = nullptr;
	c->loop = loop;
	c->len = key.len;
	c->hash = hash;
	c->who = who;
	atomic_init(&c->refs, 1);
	c->asked = false;
	memcpy(c->key, key.ptr, key.len);
	struct roster_call **bucket = &roster->buckets[hash & (BUCKETS - 1)];
	pthread_mutex_lock(&roster->lock);
	c->next = *bucket;
	*bucket = c;
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
	struct roster_call **p = &roster->buckets[c->hash & (BUCKETS - 1)];
	pthread_mutex_lock(&roster->lock);
	while (*p != c)
		p = &(*p)->next;
	*p = c->next;
	pthread_mutex_unlock(&roster->lock);
	roster_call_put(c);
}

void
roster_cancel (struct roster *roster,
               struct span    key,
               uint64_t       hash)
{
	struct roster_call *hits = nullptr;
	pthread_mutex_lock(&roster->lock);
	for (struct roster_call *c = roster->buckets[hash & (BUCKETS - 1)]; c; c = c->next) {
		if (c->asked || c->hash != hash || c->len != key.len ||
		    memcmp(c->key, key.ptr, key.len))
			continue;
		c->asked = true;
		atomic_fetch_add_explicit(&c->refs, 1, memory_order_relaxed);
		c->hit = hits;
		hits = c;
	}
	pthread_mutex_unlock(&roster->lock);
	/* The loop of a record can free the record as soon as the message is
	 * posted, so the function reads hit before it posts.  A message that
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
