/** @file Unit tests of the reference counts in src/ref.h: the values that
 *  mean "don't touch", and ref_tryget() on other threads against the last
 *  ref_put() of an object that a table points to without a reference. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "ref.h"

/* Only the drop that decrements the count to 0 returns true, and
 * ref_tryget() refuses the count after it. */
static void
test_last (void)
{
	_Atomic uint32_t refs;
	atomic_init(&refs, 1);
	ref_get(&refs);
	CHECK(ref_tryget(&refs));
	CHECK(!ref_put(&refs, 2));
	CHECK(ref_put(&refs, 1));
	CHECK(!ref_tryget(&refs) && !atomic_load(&refs));
}

/* A drop without a reference sets the top bit. ref_tryget() refuses the
 * count, and no later drop returns true. ref_tryget() also refuses a count
 * whose top bit an increment set. */
static void
test_broken (void)
{
	_Atomic uint32_t refs;
	atomic_init(&refs, 1);
	CHECK(!ref_put(&refs, 2));
	CHECK(atomic_load(&refs) >> 31 && !ref_tryget(&refs));
	CHECK(!ref_put(&refs, 1));
	_Atomic uint64_t wide;
	atomic_init(&wide, 0);
	CHECK(!ref_put(&wide, 1) && !ref_tryget(&wide));
	atomic_store(&wide, UINT64_MAX >> 1);
	CHECK(ref_tryget(&wide) && !ref_tryget(&wide));
}

enum { ROUNDS = 1 << 17, READERS = 3 };

/* An object that the table points to. The pointer in the table is not a
 * reference, so the last drop must unlink the object before it frees it. */
struct obj {
	_Atomic uint32_t refs;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct obj     *table;  /* changed only with lock locked */
static _Atomic bool    done;

/* Drop a reference to @p o. The last drop unlinks @p o under the lock and
 * then frees it, so a lookup that locks the lock either does not find
 * @p o or finds it with a count of 0, which ref_tryget() refuses. */
static void
obj_put (struct obj *o)
{
	if (!ref_put(&o->refs, 1))
		return;
	pthread_mutex_lock(&lock);
	if (table == o)
		table = nullptr;
	pthread_mutex_unlock(&lock);
	free(o);
}

static void *
test_reader (void *arg)
{
	(void)arg;
	while (!atomic_load_explicit(&done, memory_order_relaxed)) {
		pthread_mutex_lock(&lock);
		struct obj *o = table && ref_tryget(&table->refs) ? table : nullptr;
		pthread_mutex_unlock(&lock);
		if (o)
			obj_put(o);
	}
	return nullptr;
}

/* The readers look the object up and drop their references while this
 * thread drops the creator's reference, often the last one. */
static void
test_race (void)
{
	pthread_t t[READERS];
	size_t n = 0;
	while (n < READERS && !pthread_create(&t[n], nullptr, test_reader, nullptr))
		++n;
	CHECK(n == READERS);
	for (uint32_t i = 0; i < ROUNDS; ++i) {
		struct obj *o = malloc(sizeof *o);
		if (!o)
			break;
		atomic_init(&o->refs, 1);
		pthread_mutex_lock(&lock);
		table = o;
		pthread_mutex_unlock(&lock);
		obj_put(o);
	}
	atomic_store(&done, true);
	while (n)
		pthread_join(t[--n], nullptr);
	CHECK(!table);
}

int
main (void)
{
	test_last();
	test_broken();
	test_race();
	return check_report("ref");
}
