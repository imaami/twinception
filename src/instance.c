/** @file Plugin instances (instance.h). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "brainstem.h"
#include "instance.h"
#include "plugin.h"
#include "util.h"

static_assert(!(INSTANCE_MAX & (INSTANCE_MAX - 1)),
              "INSTANCE_MAX - 1 is the mask that selects a bucket");

/* The table's record of one instance.  The plugin's bytes follow the record
 * in the same allocation. */
struct instance {
	struct list          hook;        /* links the instance into one of the
	                                     table's lists: busy, idle or gone */
	struct instance     *next;        /* the next instance in the same
	                                     bucket */
	struct plugin const *plugin;
	uint64_t             principal;   /* the user of the instance */
	uint64_t             generation;  /* the generation of the request whose
	                                     call made the instance */
	uint64_t             idle;        /* the loop's time when the last call
	                                     stopped using the instance */
	uint64_t             calls;       /* the number of calls that use the
	                                     instance */
	alignas(max_align_t) unsigned char bytes[];
};

/* Returns true if an instance for principal, made for a request of
 * generation, goes into the buckets, so that later calls of the user get
 * the same instance.  It returns false for an anonymous client and for a
 * request that began before the last flush. */
static bool
instance_table_lists (struct instance_table const *t,
                      uint64_t                     principal,
                      uint64_t                     generation)
{
	return principal && generation == t->generation;
}

/* Returns the address of the pointer that links the instance of plugin for
 * principal into its bucket.  If the bucket has no such instance, it
 * returns the address of the null pointer at the end of the bucket's
 * chain. */
static struct instance **
instance_table_find (struct instance_table *t,
                     struct plugin const   *plugin,
                     uint64_t               principal)
{
	uint64_t const key[] = { principal, (uintptr_t)plugin };
	struct instance **link = &t->buckets[util_hash(0, key, sizeof key) &
	                                     (INSTANCE_MAX - 1)];
	while (*link && ((*link)->principal != principal || (*link)->plugin != plugin))
		link = &(*link)->next;
	return link;
}

/* Removes inst from its bucket and moves it from the list idle to the list
 * gone.  The table's step destroys it later. */
static void
instance_table_retire (struct instance_table *t,
                       struct instance       *inst)
{
	*instance_table_find(t, inst->plugin, inst->principal) = inst->next;
	--t->listed;
	list_del(&inst->hook);
	list_append(&t->gone, &inst->hook);
	loop_ready_bulk(t->loop, &t->reap);
}

/* Destroys instances on the list gone, at most max.  Returns the number of
 * instances destroyed. */
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

/* The table's step.  It destroys instances on the list gone, as many as
 * the step may still do of LOOP_RELEASE work.  If instances are left on the
 * list, it queues itself again. */
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
	*dest = (struct instance_table){ .reap.fn = instance_table_step, .loop = loop };
	list_init(&dest->busy);
	list_init(&dest->idle);
	list_init(&dest->gone);
	return 0;
}

void
instance_table_fini (struct instance_table *t)
{
	list_transplant(&t->gone, &t->idle);
	(void)instance_table_reap(t, SIZE_MAX);
	loop_unready(&t->reap);
}

/* Allocates an instance of plugin for principal and calls the plugin's
 * instance_init() for its bytes.  The new instance has one call and is on
 * the list busy.  Stores the instance in *p_inst.  Returns 0, ENOMEM, or
 * the error number of instance_init().  It returns EIO if instance_init()
 * returned EBUSY, because instance_table_get() returns EBUSY only for a
 * full table.  It also returns EIO if instance_init() returned a number
 * that is not positive, because such a number is no error number. */
static int
instance_table_make (struct instance_table  *t,
                     struct plugin const    *plugin,
                     uint64_t                principal,
                     uint64_t                generation,
                     struct instance       **p_inst)
{
	struct instance *inst = malloc(sizeof *inst + plugin->instance_size);
	if (!inst)
		return ENOMEM;
	*inst = (struct instance){
		.plugin = plugin,
		.principal = principal,
		.generation = generation,
		.calls = 1,
	};
	int e = plugin->instance_init(inst->bytes, principal, t->loop);
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
	struct instance *inst;
	if (!instance_table_lists(t, principal, generation)) {
		int e = instance_table_make(t, plugin, principal, generation, &inst);
		if (!e)
			*p_instance = inst->bytes;
		return e;
	}
	inst = *instance_table_find(t, plugin, principal);
	if (inst) {
		++inst->calls;
		list_del(&inst->hook);
		list_append(&t->busy, &inst->hook);
		*p_instance = inst->bytes;
		return 0;
	}
	/* If the buckets are full, the first instance on the list idle moves
	 * to the list gone.  It has been unused for the longest time.  The
	 * table's step destroys it later, because instance_fini() must not run
	 * inside this function. */
	if (t->listed == INSTANCE_MAX) {
		if (list_is_empty(&t->idle))
			return EBUSY;
		instance_table_retire(t, container_of(t->idle.next, struct instance, hook));
	}
	int e = instance_table_make(t, plugin, principal, generation, &inst);
	if (e)
		return e;
	/* The address of the bucket's link is looked up only now, because
	 * instance_table_retire() above can change the same bucket. */
	*instance_table_find(t, plugin, principal) = inst;
	++t->listed;
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
	list_del(&inst->hook);
	if (instance_table_lists(t, inst->principal, inst->generation)) {
		list_append(&t->idle, &inst->hook);
		return;
	}
	list_append(&t->gone, &inst->hook);
	loop_ready_bulk(t->loop, &t->reap);
}

void
instance_table_flush (struct instance_table *t)
{
	++t->generation;
	t->listed = 0;
	memset(t->buckets, 0, sizeof t->buckets);
	list_transplant(&t->gone, &t->idle);
	loop_ready_bulk(t->loop, &t->reap);
}

/* Calls relieve() of the plugin for each instance on list, until max units
 * are freed.  Returns the number of units freed. */
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
		/* A plugin's relieve() can return more than max - done.  The count
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
		[BUF_HIGH]     = 2 * BRAINSTEM_RELIEF_MS * 1'000'000ULL,
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
	 * passes max.  The comparisons skip a list when no unit of max is
	 * left. */
	if (done < max)
		done += instance_list_relieve(&t->busy, level, max - done);
	if (done < max)
		done += instance_list_relieve(&t->idle, level, max - done);
	return done;
}
