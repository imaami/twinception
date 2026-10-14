/** @file Plugin instances: the memory that a plugin (src/plugin.h) has for
 *  each user whose calls run on one client loop.
 *
 * Each client loop has one table.  Only the thread of the loop calls the
 * functions of the table, and askai passes an instance only to functions
 * that it calls on that thread.  So the table has no lock and no atomic
 * object.
 *
 * If a plugin has instances, each call of its tools uses one of them.  The
 * table counts the calls that use an instance.  instance_table_get() adds
 * 1 to the count, and instance_table_put() subtracts 1.
 *
 * Most instances are in the table's buckets.  The key of an instance in
 * the buckets is its plugin and the principal of its user (src/policy.h).
 * With the two exceptions that are described below, a call gets the
 * instance that the buckets have for its key.  If the buckets have none,
 * the table makes one and adds it to the buckets.  The buckets have at
 * most INSTANCE_MAX instances.  The table destroys an instance of the
 * buckets only when no call uses it, in three cases:
 * - instance_table_get() needs a new instance in the buckets, and the
 *   buckets already have INSTANCE_MAX instances.  The table then destroys
 *   the instance that has been unused for the longest time.  If calls use
 *   all INSTANCE_MAX instances, instance_table_get() returns EBUSY.
 * - askai is under memory pressure (instance_table_relieve()).
 * - The loop has a new policy (instance_table_flush()).  The flush removes
 *   every instance from the buckets.  The table's step then destroys the
 *   ones that no call uses.  An instance that calls use is destroyed after
 *   the last of these calls has been dropped.
 *
 * The exceptions are two sorts of call.  The first is a call with the
 * principal 0, which is the principal of an anonymous client.  The second
 * is a call of a request that began before the last
 * instance_table_flush().  Such a call gets a new instance that is not in
 * the buckets.  No other call gets that instance, and it does not count
 * toward INSTANCE_MAX.  The table destroys it after the call has been
 * dropped.
 *
 * instance_table_get() and instance_table_put() never call instance_fini()
 * of a plugin.  instance_fini() is called only from the table's own step,
 * from instance_table_relieve() and from instance_table_fini().  No
 * function of a plugin is running at those times.  A kind can therefore
 * use its instance after it has ended the call, until the kind's function
 * returns.
 */
#ifndef TWINCEPTION_SRC_INSTANCE_H_
#define TWINCEPTION_SRC_INSTANCE_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "list.h"
#include "loop.h"

struct instance;
struct plugin;

enum {
	/** The largest number of instances that a table has for later calls
	 *  of their users.  It is a power of two, because INSTANCE_MAX - 1 is
	 *  the mask that selects a bucket. */
	INSTANCE_MAX = 256,
};

struct instance_table {
	struct loop_step  reap;        /**< the table's step, which destroys the
	                                    instances on the list gone */
	struct list       busy;        /**< the instances that a call uses */
	struct list       idle;        /**< the instances that no call uses and
	                                    that a later call of their user
	                                    gets.  The first one has been
	                                    unused for the longest time. */
	struct list       gone;        /**< the instances that no call uses and
	                                    that no later call gets.  The
	                                    table's step destroys them. */
	struct loop      *loop;
	/** The instances that a later call of their user gets.  A hash of the
	 *  principal and of the plugin's address selects the bucket. */
	struct instance  *buckets[INSTANCE_MAX];
	uint64_t          listed;      /**< the number of instances in buckets */
	uint64_t          generation;  /**< the number of calls of
	                                    instance_table_flush() so far */
};

/** Initializes @p dest as the empty table of @p loop.  It allocates nothing,
 *  so it always returns 0. */
extern int
instance_table_init (struct instance_table *dest,
                     struct loop           *loop);

/** Destroys every instance of @p table.  No call may use an instance when
 *  this function is called. */
extern void
instance_table_fini (struct instance_table *table);

/** Returns the generation of @p table.  A request stores this number when
 *  it begins, and passes it to instance_table_get() for each of its
 *  calls. */
static inline uint64_t
instance_table_generation (struct instance_table const *table)
{
	return table->generation;
}

/**
 * Stores in @p *p_instance the instance of @p plugin for the user
 * @p principal.  The instance is the bytes that instance_init() of the
 * plugin initialized.  The table counts the caller's call as a user of the
 * instance until instance_table_put().  If @p plugin has no instances, the
 * function stores nullptr and returns 0.
 *
 * @p generation is the value that instance_table_generation() returned
 * when the request of the call began.
 *
 * If the table has no instance of the plugin for the user, it makes one.
 * If @p principal is 0, or if @p generation is not the table's generation,
 * it always makes a new instance, and no other call gets that instance.
 *
 * Returns 0 on success.  Returns EBUSY only if the buckets have
 * INSTANCE_MAX instances and calls use all of them.  Returns ENOMEM if it
 * cannot allocate the instance.  If the plugin's instance_init() returns
 * an error number, the function returns that number.  It returns EIO if
 * instance_init() returns EBUSY or a number that is not positive.
 */
extern int
instance_table_get (struct instance_table  *table,
                    struct plugin const    *plugin,
                    uint64_t                principal,
                    uint64_t                generation,
                    void                  **p_instance);

/** Removes the caller's call from the users of the instance
 *  @p *p_instance, and sets @p *p_instance to nullptr.  It does nothing if
 *  @p *p_instance is nullptr. */
extern void
instance_table_put (struct instance_table  *table,
                    void                  **p_instance);

/** Starts a new generation.  askai calls it when the loop has a new policy.
 *  After this call, no call of a later request gets an instance that
 *  exists now.  The table's step destroys the instances that no call uses.
 *  An instance that a call uses is destroyed after its last call has been
 *  dropped. */
extern void
instance_table_flush (struct instance_table *table);

/**
 * Frees memory under memory pressure, at most @p max LOOP_RELEASE units.
 *
 * First it destroys instances that no call uses, and counts one unit for
 * each.  At BUF_HIGH it destroys the instances that have been unused for
 * two relief periods (BRAINSTEM_RELIEF_MS).  At BUF_CRITICAL it destroys
 * all of them.  Then it calls relieve() of the plugin for each instance
 * that is left, while units are left.
 *
 * Returns the number of units freed.  It returns 0 when it can free
 * nothing more.
 */
extern size_t
instance_table_relieve (struct instance_table *table,
                        enum buf_pressure      level,
                        size_t                 max);

#endif /* TWINCEPTION_SRC_INSTANCE_H_ */
