/** @file instance.c
 *
 * Plugin instances.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "brainstem.h"
#include "instance.h"
#include "plugin.h"
#include "util.h"

/* The table's record of one instance. The plugin's bytes follow the record
 * in the same allocation.
 */
struct instance {
	struct list          hook;        /* links the instance into one of the
	                                     table's lists: busy, idle or gone */
	struct htab_node     node;        /* in the buckets, if it is there */
	struct plugin const *plugin;
	uint64_t             principal;   /* the user of the instance */
	uint64_t             idle;        /* the loop's time when the last call
	                                     stopped using the instance */
	uint64_t             calls;       /* the number of calls that use the
	                                     instance */
	alignas(max_align_t) unsigned char bytes[];
};

/* Returns true if the plugin and the principal of the instance of node are
 * those of the instance key.
 */
static bool
instance_match (struct htab_node const *node,
                void const             *key)
{
	struct instance const *inst = container_of(node, struct instance, node);
	struct instance const *k = key;
	return inst->principal == k->principal && inst->plugin == k->plugin;
}

/* Returns the instance in the buckets with the plugin and the principal of
 * key, whose hash is set, or nullptr.
 */
static struct instance *
instance_table_find (struct instance_table const *t,
                     struct instance const       *key)
{
	struct htab_node *node = *htab_find(&t->listed, key->node.hash, instance_match, key);
	return node ? container_of(node, struct instance, node) : nullptr;
}

/* Removes inst from its bucket and moves it from the list idle to the list
 * gone. The table's step destroys it later.
 */
static void
instance_table_retire (struct instance_table *t,
                       struct instance       *inst)
{
	htab_unlink(&t->listed, &inst->node);
	list_move(&t->gone, &inst->hook);
	loop_ready_bulk(t->loop, &t->reap);
}

/* Destroys instances on the list gone, at most max. Returns the number of
 * instances destroyed.
 */
static size_t
instance_table_reap (struct instance_table *t,
                     size_t                 max)
{
	size_t n = 0;
	for (; n < max && !list_is_empty(&t->gone); ++n) {
		struct instance *inst = container_of(t->gone.next, struct instance, hook);
		list_del(&inst->hook);
		if (inst->plugin->instance_fini)
			inst->plugin->instance_fini(inst->bytes);
		free(inst);
	}
	return n;
}

/* The table's step. It destroys instances on the list gone, as many as
 * the step may still do of LOOP_RELEASE work. If instances are left on the
 * list, it queues itself again.
 */
static void
instance_table_step (struct loop      *loop,
                     struct loop_step *step)
{
	struct instance_table *t = container_of(step, struct instance_table, reap);
	loop_work(loop, LOOP_RELEASE, instance_table_reap(t, loop_room(loop, LOOP_RELEASE)));
	if (!list_is_empty(&t->gone))
		loop_ready_bulk(loop, step);
}

int
instance_table_init (struct instance_table *dest,
                     struct loop           *loop)
{
	*dest = (struct instance_table){ .reap.fn = instance_table_step, .busy = list(&dest->busy),
	                                 .idle = list(&dest->idle), .gone = list(&dest->gone),
	                                 .loop = loop };
	return htab_init(&dest->listed, INSTANCE_MAX);
}

void
instance_table_fini (struct instance_table *t)
{
	list_transplant(&t->gone, &t->idle);
	(void)instance_table_reap(t, SIZE_MAX);
	loop_unready(&t->reap);
	htab_fini(&t->listed);
}

/* Allocates an instance with the plugin, the principal and the hash of
 * key, and calls the plugin's instance_init() for its bytes. The new
 * instance has one call and is on the list busy. Stores the instance in
 * *p_inst. Returns 0, ENOMEM, or the error number of instance_init(). It
 * returns EIO if instance_init() returned EBUSY, because
 * instance_table_get() returns EBUSY only for a full table. It also
 * returns EIO if instance_init() returned a number that is not positive,
 * because such a number is no error number.
 */
static int
instance_table_make (struct instance_table  *t,
                     struct instance const  *key,
                     struct instance       **p_inst)
{
	struct instance *inst = malloc(sizeof *inst + key->plugin->instance_size);
	if (!inst)
		return ENOMEM;
	*inst = (struct instance){
		.node.hash = key->node.hash,
		.plugin = key->plugin,
		.principal = key->principal,
		.calls = 1,
	};
	int e = key->plugin->instance_init(inst->bytes, key->principal, t->loop);
	if (e) {
		free(inst);
		return e > 0 && e != EBUSY ? e : EIO;
	}
	list_append(&t->busy, &inst->hook);
	*p_inst = inst;
	return 0;
}

int
instance_table_get (struct instance_table  *t,
                    struct plugin const    *plugin,
                    uint64_t                principal,
                    uint64_t                generation,
                    void                  **p_instance)
{
	*p_instance = nullptr;
	if (!plugin->instance_size)
		return 0;
	uint64_t const words[] = { principal, (uintptr_t)plugin };
	struct instance const key = {
		.node.hash = util_hash(0, words, sizeof words),
		.plugin = plugin,
		.principal = principal,
	};
	struct instance *inst;
	/* The call of an anonymous client, or of a request that began before
	 * the last flush, gets an instance that is not in the buckets. */
	if (!principal || generation != t->generation) {
		int e = instance_table_make(t, &key, &inst);
		if (!e)
			*p_instance = inst->bytes;
		return e;
	}
	inst = instance_table_find(t, &key);
	if (inst) {
		++inst->calls;
		list_move(&t->busy, &inst->hook);
		*p_instance = inst->bytes;
		return 0;
	}
	/* If the buckets are full, the first instance on the list idle moves
	 * to the list gone. It has been unused for the longest time. The
	 * table's step destroys it later, because instance_fini() must not run
	 * inside this function. */
	if (t->listed.n == INSTANCE_MAX) {
		if (list_is_empty(&t->idle))
			return EBUSY;
		instance_table_retire(t, container_of(t->idle.next, struct instance, hook));
	}
	int e = instance_table_make(t, &key, &inst);
	if (e)
		return e;
	htab_link(&t->listed, &inst->node);
	*p_instance = inst->bytes;
	return 0;
}

void
instance_table_put (struct instance_table  *t,
                    void                  **p_instance)
{
	void *bytes = *p_instance;
	*p_instance = nullptr;
	if (!bytes)
		return;
	struct instance *inst = container_of(bytes, struct instance, bytes);
	if (--inst->calls)
		return;
	inst->idle = t->loop->now;
	/* An instance in the buckets waits for the next call of its user; a
	 * flush emptied the buckets of those before it. */
	if (instance_table_find(t, inst) == inst) {
		list_move(&t->idle, &inst->hook);
		return;
	}
	list_move(&t->gone, &inst->hook);
	loop_ready_bulk(t->loop, &t->reap);
}

void
instance_table_flush (struct instance_table *t)
{
	++t->generation;
	t->listed.n = 0;
	memset(t->listed.buckets, 0, (t->listed.mask + 1) * sizeof *t->listed.buckets);
	list_transplant(&t->gone, &t->idle);
	loop_ready_bulk(t->loop, &t->reap);
}

/* Calls relieve() of the plugin for each instance on list, until max units
 * are freed. Returns the number of units freed.
 */
static size_t
instance_list_relieve (struct list const *list,
                       enum buf_pressure  level,
                       size_t             max)
{
	size_t done = 0;
	for (struct list const *pos = list->next; pos != list && done < max; pos = pos->next) {
		struct instance *inst = container_of(pos, struct instance, hook);
		if (!inst->plugin->relieve)
			continue;
		/* A plugin's relieve() can return more than max - done. The count
		 * is cut at max - done, so done never passes max. */
		size_t n = inst->plugin->relieve(inst->bytes, level, max - done);
		done += n < max - done ? n : max - done;
	}
	return done;
}

size_t
instance_table_relieve (struct instance_table *t,
                        enum buf_pressure      level,
                        size_t                 max)
{
	/* keep[level] is the time in nanoseconds for which an instance must
	 * have been unused before a relief at that level destroys it. */
	static uint64_t const keep[] = {
		[BUF_EASY]     = UINT64_MAX,
		[BUF_HIGH]     = 2 * BRAINSTEM_RELIEF_MS * MS,
		[BUF_CRITICAL] = 0,
	};
	/* The first instance on the list idle has been unused for the longest
	 * time, so the loop stops at the first instance that is too recent. */
	for (size_t n = 0; n < max && !list_is_empty(&t->idle); ++n) {
		struct instance *inst = container_of(t->idle.next, struct instance, hook);
		if (t->loop->now - inst->idle < keep[level])
			break;
		instance_table_retire(t, inst);
	}
	size_t done = instance_table_reap(t, max);
	/* instance_list_relieve() returns at most max - done, so done never
	 * passes max. The comparisons skip a list when no unit of max is
	 * left. */
	if (done < max)
		done += instance_list_relieve(&t->busy, level, max - done);
	if (done < max)
		done += instance_list_relieve(&t->idle, level, max - done);
	return done;
}
