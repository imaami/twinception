/** @file Unit tests for the table of plugin instances (src/instance.c).  The
 *  test includes the sources of the loop and of the table to reach their
 *  static functions.  The table's source is named by its path, because
 *  "instance.c" alone would name this file. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "loop.c"
#include "../src/instance.c"

static unsigned failures;

#define CHECK(cond) \
        ((cond) ? (void)0 \
                : (void)(++failures, fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                                             __FILE__, __LINE__, #cond)))

static uint64_t made;
static uint64_t destroyed;
static uint64_t relieved;
static int      fail_with;

struct object {
	uint64_t  principal;
	uint64_t  keeps;
	void     *block;  /* allocated in object_init(), so that a missing
	                     instance_fini() is a memory leak */
};

static int
object_init (void *instance, uint64_t principal, struct loop *loop)
{
	(void)loop;
	if (fail_with)
		return fail_with;
	struct object *o = instance;
	*o = (struct object){ .principal = principal, .keeps = 3, .block = malloc(8) };
	++made;
	return 0;
}

static void
object_fini (void *instance)
{
	struct object *o = instance;
	free(o->block);
	++destroyed;
}

static size_t
object_relieve (void *instance, enum buf_pressure level, size_t max)
{
	(void)level;
	struct object *o = instance;
	size_t n = o->keeps < max ? o->keeps : max;
	o->keeps -= n;
	relieved += n;
	return n;
}

/* A relieve() that returns more units than it was given. */
static size_t
greedy_relieve (void *instance, enum buf_pressure level, size_t max)
{
	(void)instance;
	(void)level;
	(void)max;
	return SIZE_MAX;
}

static struct plugin const with = {
	.instance_init = object_init,
	.instance_fini = object_fini,
	.relieve = object_relieve,
	.instance_size = sizeof (struct object),
};
static struct plugin const other = {
	.instance_init = object_init,
	.instance_fini = object_fini,
	.instance_size = sizeof (struct object),
};
static struct plugin const greedy = {
	.instance_init = object_init,
	.instance_fini = object_fini,
	.relieve = greedy_relieve,
	.instance_size = sizeof (struct object),
};
static struct plugin const without = {};

/* Runs the table's step until the list gone is empty. */
static void
settle (struct instance_table *t)
{
	while (!list_is_empty(&t->gone)) {
		/* The list gone is not empty, so the table must have queued its
		 * step as a bulk step of the loop.  The test queues no other
		 * step. */
		CHECK(!list_is_empty(&t->loop->ready[BULK]));
		memset(t->loop->work, 0, sizeof t->loop->work);
		loop_unready(&t->reap);
		instance_table_step(t->loop, &t->reap);
	}
	loop_unready(&t->reap);
}

static size_t
count (struct list const *list)
{
	size_t n = 0;
	for (struct list const *pos = list->next; pos != list; pos = pos->next)
		++n;
	return n;
}

int
main (void)
{
	struct loop l = loop();
	l.id = 7;
	if (loop_init(&l)) {
		fprintf(stderr, "loop_init failed\n");
		return 1;
	}
	static struct instance_table t;
	CHECK(!instance_table_init(&t, &l));
	uint64_t g = instance_table_generation(&t);
	void *a = &t;
	void *b;

	/* For a plugin without instances, instance_table_get() stores nullptr
	 * and returns 0. */
	CHECK(!instance_table_get(&t, &without, 5, g, &a) && !a);
	instance_table_put(&t, &a);

	/* Two calls of one user get the same instance of one plugin, both at
	 * the same time and one after the other.  A call of another plugin or
	 * of another user gets another instance. */
	CHECK(!instance_table_get(&t, &with, 5, g, &a) && a);
	CHECK(!instance_table_get(&t, &with, 5, g, &b) && b == a);
	void *c;
	void *d;
	CHECK(!instance_table_get(&t, &other, 5, g, &c) && c != a);
	CHECK(!instance_table_get(&t, &with, 6, g, &d) && d != a && d != c);
	CHECK(((struct object *)d)->principal == 6);
	CHECK((uintptr_t)a % alignof (max_align_t) == 0);
	CHECK(made == 3 && t.listed == 3 && count(&t.busy) == 3);
	void *was = a;
	instance_table_put(&t, &a);
	CHECK(!a && count(&t.busy) == 3 && list_is_empty(&t.idle));
	instance_table_put(&t, &b);
	CHECK(!b && list_is_empty(&t.gone) && count(&t.idle) == 1 && count(&t.busy) == 2);
	CHECK(!instance_table_get(&t, &with, 5, g, &a) && a == was && made == 3);
	instance_table_put(&t, &a);
	instance_table_put(&t, &c);
	instance_table_put(&t, &d);
	CHECK(count(&t.idle) == 3 && list_is_empty(&t.busy));

	/* If instance_init() fails, instance_table_get() returns its error
	 * number, and the table has no new instance.  instance_table_get()
	 * returns EBUSY only for a full table, so it returns EIO if
	 * instance_init() returns EBUSY.  It also returns EIO for a number that
	 * is not positive, because that is no error number. */
	fail_with = EACCES;
	CHECK(instance_table_get(&t, &with, 7, g, &a) == EACCES && !a);
	CHECK(instance_table_get(&t, &with, 0, g, &a) == EACCES && !a);
	fail_with = EBUSY;
	CHECK(instance_table_get(&t, &with, 7, g, &a) == EIO && !a);
	fail_with = -1;
	CHECK(instance_table_get(&t, &with, 7, g, &a) == EIO && !a);
	fail_with = 0;
	CHECK(t.listed == 3 && list_is_empty(&t.busy) && list_is_empty(&t.gone));

	/* The instance of an anonymous client is for one call only.  The same
	 * is true of a request with an older generation.  instance_table_get()
	 * and instance_table_put() destroy nothing.  The table's step destroys
	 * the instance. */
	CHECK(!instance_table_get(&t, &with, 0, g, &a) && !instance_table_get(&t, &with, 0, g, &b));
	CHECK(a != b && t.listed == 3);
	instance_table_put(&t, &a);
	CHECK(destroyed == 0 && count(&t.gone) == 1);
	settle(&t);
	CHECK(destroyed == 1);
	instance_table_put(&t, &b);
	settle(&t);
	CHECK(destroyed == 2);
	CHECK(!instance_table_get(&t, &with, 5, g - 1, &a) && a != was && t.listed == 3);
	CHECK(((struct object *)a)->principal == 5);
	instance_table_put(&t, &a);
	CHECK(destroyed == 2 && count(&t.gone) == 1);
	settle(&t);
	CHECK(destroyed == 3 && made == 6);

	/* Many users call one after the other, and the table's step does not
	 * run in between.  The buckets never have more than INSTANCE_MAX
	 * instances, no call is refused, and nothing is destroyed before the
	 * step runs. */
	uint64_t before = destroyed;
	for (uint64_t p = 100; p < 100 + 4 * INSTANCE_MAX; ++p) {
		CHECK(!instance_table_get(&t, &with, p, g, &a));
		instance_table_put(&t, &a);
		CHECK(t.listed <= INSTANCE_MAX);
	}
	CHECK(destroyed == before && count(&t.gone) == 3 * INSTANCE_MAX + 3);
	CHECK(count(&t.idle) == INSTANCE_MAX && t.listed == INSTANCE_MAX);
	settle(&t);
	CHECK(destroyed - before == 3 * INSTANCE_MAX + 3);
	/* The user of the last call gets the same instance again.  The user
	 * of the first call gets a new instance. */
	uint64_t m = made;
	CHECK(!instance_table_get(&t, &with, 100 + 4 * INSTANCE_MAX - 1, g, &a) && made == m);
	instance_table_put(&t, &a);
	CHECK(!instance_table_get(&t, &with, 100, g, &a) && made == m + 1);
	instance_table_put(&t, &a);
	settle(&t);

	/* instance_table_get() returns EBUSY only when calls use all
	 * INSTANCE_MAX instances in the buckets.  An anonymous call still gets
	 * an instance then. */
	static void *busy[INSTANCE_MAX];
	size_t n = 0;
	int e = 0;
	for (; n < INSTANCE_MAX && !(e = instance_table_get(&t, &other, 10000 + n, g, &busy[n])); ++n)
		;
	CHECK(!e && n == INSTANCE_MAX && list_is_empty(&t.idle) && t.listed == INSTANCE_MAX);
	CHECK(instance_table_get(&t, &other, 5, g, &a) == EBUSY && !a);
	CHECK(!instance_table_get(&t, &with, 0, g, &a) && a);
	instance_table_put(&t, &a);
	settle(&t);

	/* After a flush, no call of a later request gets an instance that
	 * existed before the flush.  An instance that a call uses stays until
	 * the call is dropped. */
	for (size_t i = 0; i < INSTANCE_MAX / 2; ++i)
		instance_table_put(&t, &busy[i]);
	before = destroyed;
	instance_table_flush(&t);
	CHECK(instance_table_generation(&t) == g + 1 && destroyed == before);
	settle(&t);
	CHECK(destroyed - before == INSTANCE_MAX / 2 && list_is_empty(&t.idle) && !t.listed);
	was = busy[INSTANCE_MAX - 1];
	/* A request that began before the flush gets an instance of its own. */
	CHECK(!instance_table_get(&t, &other, 10000 + INSTANCE_MAX - 1, g, &a) && a != was);
	CHECK(!t.listed);
	instance_table_put(&t, &a);
	g = instance_table_generation(&t);
	CHECK(!instance_table_get(&t, &other, 10000 + INSTANCE_MAX - 1, g, &a) && a != was);
	CHECK(t.listed == 1 && ((struct object *)was)->principal == 10000 + INSTANCE_MAX - 1);
	instance_table_put(&t, &a);
	for (size_t i = INSTANCE_MAX / 2; i < INSTANCE_MAX; ++i)
		instance_table_put(&t, &busy[i]);
	CHECK(count(&t.idle) == 1);
	settle(&t);
	CHECK(made - destroyed == 1 && t.listed == 1);
	CHECK(instance_table_relieve(&t, BUF_CRITICAL, 100) == 1 && made == destroyed);

	/* A relief at BUF_HIGH destroys the instances that have been unused for
	 * two relief periods.  A relief at BUF_CRITICAL destroys every unused
	 * instance.  Both call relieve() for the instances that are left. */
	l.now = 1;
	CHECK(!instance_table_get(&t, &with, 1, g, &a));
	CHECK(!instance_table_get(&t, &with, 2, g, &b));
	instance_table_put(&t, &b);
	before = destroyed;
	relieved = 0;
	CHECK(instance_table_relieve(&t, BUF_HIGH, 100) == 6 && destroyed == before);
	CHECK(relieved == 6 && instance_table_relieve(&t, BUF_HIGH, 100) == 0);
	l.now += 2 * BRAINSTEM_RELIEF_MS * 1'000'000ULL - 1;
	CHECK(instance_table_relieve(&t, BUF_HIGH, 100) == 0);
	l.now += 1;
	CHECK(instance_table_relieve(&t, BUF_HIGH, 100) == 1 && destroyed == before + 1);
	CHECK(instance_table_relieve(&t, BUF_CRITICAL, 100) == 0 && count(&t.busy) == 1);
	instance_table_put(&t, &a);
	CHECK(instance_table_relieve(&t, BUF_CRITICAL, 100) == 1);
	CHECK(made == destroyed && !t.listed);
	/* A relief destroys at most max instances. */
	for (uint64_t p = 1; p <= 10; ++p) {
		CHECK(!instance_table_get(&t, &other, p, g, &a));
		instance_table_put(&t, &a);
	}
	CHECK(instance_table_relieve(&t, BUF_CRITICAL, 4) == 4 && made - destroyed == 6);
	CHECK(instance_table_relieve(&t, BUF_CRITICAL, 100) == 6 && made == destroyed);
	/* A relieve() that returns more units than it was given counts as
	 * the units that it was given. */
	CHECK(!instance_table_get(&t, &greedy, 1, g, &a));
	CHECK(instance_table_relieve(&t, BUF_EASY, 5) == 5);
	instance_table_put(&t, &a);
	CHECK(instance_table_relieve(&t, BUF_CRITICAL, 100) == 1 && made == destroyed);

	/* instance_table_fini() destroys the instances that are left. */
	CHECK(!instance_table_get(&t, &with, 1, g, &a));
	instance_table_put(&t, &a);
	CHECK(!instance_table_get(&t, &with, 0, g, &a));
	instance_table_put(&t, &a);
	instance_table_fini(&t);
	CHECK(made == destroyed);

	loop_fini(&l);
	printf("instance: %s (%u failures; %llu made)\n", failures ? "FAIL" : "PASS",
	       failures, (unsigned long long)made);
	return !!failures;
}
