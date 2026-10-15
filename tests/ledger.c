/** @file Unit tests of the ledger (src/ledger.c): the owners of records, the
 *  limits of one owner, and the removal of records.  The test includes
 *  src/loop.c and src/ledger.c, so that it can read the ledger's own
 *  structures, and replaces clock_gettime() in them with a fake clock, as
 *  tests/turn.c does.  The ledger's source is named by its path, because
 *  "ledger.c" alone would name this file. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* clock_ns is the time of the fake clock in nanoseconds. */
static uint64_t clock_ns = 1'000'000'000;

/* This function replaces clock_gettime() in src/loop.c and src/ledger.c.
 * It ignores @p id and stores the time clock_ns in @p ts. */
static int
fake_clock_gettime (clockid_t        id,
                    struct timespec *ts)
{
	(void)id;
	*ts = (struct timespec){
		.tv_sec = (time_t)(clock_ns / 1'000'000'000),
		.tv_nsec = (long)(clock_ns % 1'000'000'000)
	};
	return 0;
}

#define clock_gettime fake_clock_gettime
#include "../src/ledger.c"
#include "loop.c"
#undef clock_gettime

static unsigned failures;

#define CHECK(cond) \
	((cond) ? (void)0 \
	        : (void)(++failures, fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	                                     __FILE__, __LINE__, #cond)))

enum { A = 1, B = 2, C = 3 };  /* owners */

static struct loop *test_loop;  /* the loop of every record's call */
static uint64_t     cancels;    /* cancel messages handled */

/* The handler of a record's cancel.  It drops the reference that
 * ledger_task_cancel() took for the message. */
static void
test_cancel (struct loop *loop,
             struct msg  *msg)
{
	(void)loop;
	++cancels;
	ledger_task_put(container_of(msg, struct ledger_task, cancel));
}

/* Opens a record of @p kind for @p owner, of the tool 5. */
static struct ledger_task *
test_open (struct ledger    *l,
           uint64_t          owner,
           enum ledger_kind  kind,
           int              *e)
{
	return ledger_open(l, owner, kind, 5, test_loop, (struct handle){}, test_cancel, e);
}

/* Ends the call of @p t as completed, and drops the call's reference. */
static void
test_end (struct ledger      *l,
          struct ledger_task *t)
{
	ledger_end(l, t, LEDGER_COMPLETED);
	ledger_task_put(t);
}

/* The number of owners in the ledger's table. */
static size_t
test_owners (struct ledger const *l)
{
	size_t n = 0;
	for (size_t i = 0; i < OWNER_BUCKETS; ++i)
		for (struct ledger_owner const *o = l->owners[i]; o; o = o->next)
			++n;
	return n;
}

/* Whether ledger_find() returns the record with the id @p id to @p owner. */
static bool
test_found (struct ledger *l,
            uint64_t       owner,
            char const    *id)
{
	struct ledger_task *found = ledger_find(l, owner, id, LEDGER_ID);
	ledger_task_put(found);
	return found;
}

/* Runs the sweep of ended records, as every ledger_find() does. */
static void
test_sweep (struct ledger *l)
{
	char id[LEDGER_ID];
	memset(id, 'x', sizeof id);
	CHECK(!test_found(l, A, id));
}

/* Invalid arguments.  ledger_open() fails with EINVAL, and the other
 * functions do nothing. */
static void
test_invalid (struct ledger *l)
{
	int e = 0;
	CHECK(!ledger_open(nullptr, A, LEDGER_TASK, 0, test_loop, (struct handle){},
	                   test_cancel, &e) && e == EINVAL);
	CHECK(!ledger_open(l, A, (enum ledger_kind)7, 0, test_loop, (struct handle){},
	                   test_cancel, &e) && e == EINVAL);
	CHECK(!ledger_open(l, A, LEDGER_TASK, 0, nullptr, (struct handle){}, test_cancel,
	                   &e) && e == EINVAL);
	CHECK(!ledger_open(l, A, LEDGER_TASK, 0, test_loop, (struct handle){}, nullptr,
	                   &e) && e == EINVAL);
	CHECK(!ledger_open(l, A, LEDGER_TASK, 0, test_loop, (struct handle){}, test_cancel,
	                   nullptr));
	CHECK(!ledger_find(nullptr, A, "0", 1) && !ledger_find(l, A, nullptr, LEDGER_ID));
	ledger_end(nullptr, nullptr, LEDGER_COMPLETED);
	ledger_destroy(nullptr);
	CHECK(!ledger_create(nullptr));
	CHECK(!l->n && !test_owners(l));
}

/* The limit of LEDGER_RUNNING records whose calls run applies to the jobs
 * of one owner.  A task of the owner is opened past it, and so is a job of
 * another owner.  A ledger full of records whose calls run refuses every
 * record. */
static void
test_caps (struct ledger *l)
{
	struct ledger_metrics const *m = ledger_metrics(l);
	static struct ledger_task *t[LEDGER_MAX + 2];
	size_t n = 0;
	int e = 0;
	for (; n < LEDGER_RUNNING; ++n) {
		t[n] = test_open(l, A, LEDGER_JOB, &e);
		CHECK(t[n] && !e && t[n]->kind == LEDGER_JOB && t[n]->tool == 5 &&
		      !atomic_load(&t[n]->told));
	}
	CHECK(!test_open(l, A, LEDGER_JOB, &e) && e == EBUSY);
	CHECK(atomic_load(&m->refused) == 1 && !atomic_load(&m->full));
	t[n++] = test_open(l, A, LEDGER_TASK, &e);
	CHECK(t[n - 1] && !e && t[n - 1]->kind == LEDGER_TASK);
	t[n++] = test_open(l, B, LEDGER_JOB, &e);
	CHECK(t[n - 1] && !e);
	CHECK(ledger_owner_find(l, A)->running == LEDGER_RUNNING + 1);

	/* The task counts toward the limit too.  A job of A opens once fewer
	 * than LEDGER_RUNNING of A's calls run.  A second end of a call changes
	 * nothing. */
	ledger_end(l, t[0], LEDGER_COMPLETED);
	ledger_end(l, t[0], LEDGER_FAILED);
	CHECK(atomic_load(&t[0]->status) == LEDGER_COMPLETED &&
	      atomic_load(&m->ended[LEDGER_COMPLETED]) == 1 &&
	      !atomic_load(&m->ended[LEDGER_FAILED]) &&
	      ledger_owner_find(l, A)->running == LEDGER_RUNNING);
	CHECK(!test_open(l, A, LEDGER_JOB, &e) && e == EBUSY && atomic_load(&m->refused) == 2);
	ledger_end(l, t[LEDGER_RUNNING], LEDGER_COMPLETED);
	t[n++] = test_open(l, A, LEDGER_JOB, &e);
	CHECK(t[n - 1] && !e);
	CHECK(atomic_load(&m->jobs) == LEDGER_RUNNING + 2 && atomic_load(&m->opened) == 1);

	/* Tasks of C fill the ledger.  To make room, the ledger removes the two
	 * records whose calls have ended, the oldest first. */
	while (n < ARRAY_SIZE(t) && (t[n] = test_open(l, C, LEDGER_TASK, &e)))
		++n;
	CHECK(n == ARRAY_SIZE(t) && !e && l->n == LEDGER_MAX && !t[0]->owner &&
	      !t[LEDGER_RUNNING]->owner);
	CHECK(!test_open(l, B, LEDGER_TASK, &e) && e == ENOSPC);
	CHECK(!test_open(l, B, LEDGER_JOB, &e) && e == ENOSPC);
	CHECK(atomic_load(&m->full) == 2 && atomic_load(&m->gone) == 2 &&
	      ledger_owner_find(l, C)->n == LEDGER_MAX - LEDGER_RUNNING - 1);
	ledger_task_put(t[0]);
	for (size_t k = 1; k < n; ++k)
		test_end(l, t[k]);
	CHECK(atomic_load(&m->kept) == LEDGER_MAX && test_owners(l) == 3);
}

/* When an owner with LEDGER_MINE records opens another, the ledger removes
 * the owner's oldest ended record and no record of another owner.  The
 * ledger never removes a record whose call runs. */
static void
test_mine (struct ledger *l)
{
	struct ledger_metrics const *m = ledger_metrics(l);
	uint64_t gone = atomic_load(&m->gone);
	struct ledger_task *t[LEDGER_MINE + 1];
	int e = 0;
	struct ledger_task *b = test_open(l, B, LEDGER_TASK, &e);
	ledger_end(l, b, LEDGER_COMPLETED);
	for (size_t k = 0; k < LEDGER_MINE; ++k) {
		t[k] = test_open(l, A, LEDGER_TASK, &e);
		ledger_end(l, t[k], LEDGER_COMPLETED);
	}
	CHECK(ledger_owner_find(l, A)->n == LEDGER_MINE);
	t[LEDGER_MINE] = test_open(l, A, LEDGER_TASK, &e);
	CHECK(t[LEDGER_MINE] && !e && ledger_owner_find(l, A)->n == LEDGER_MINE);
	CHECK(!t[0]->owner && !test_found(l, A, t[0]->id) && test_found(l, A, t[1]->id) &&
	      test_found(l, B, b->id) && atomic_load(&m->gone) == gone + 1);

	/* Each new record of A replaces its oldest ended record, until the
	 * calls of all its records run.  The owner can then have more than
	 * LEDGER_MINE records. */
	ledger_task_put(t[0]);
	for (size_t k = 1; k < LEDGER_MINE; ++k) {
		ledger_task_put(t[k]);
		t[k] = test_open(l, A, LEDGER_TASK, &e);
	}
	t[0] = test_open(l, A, LEDGER_TASK, &e);
	CHECK(t[0] && ledger_owner_find(l, A)->n == LEDGER_MINE + 1 &&
	      ledger_owner_find(l, A)->running == LEDGER_MINE + 1 &&
	      atomic_load(&m->gone) == gone + LEDGER_MINE);
	for (size_t k = 0; k <= LEDGER_MINE; ++k)
		test_end(l, t[k]);
	ledger_task_put(b);
}

/* A ledger with LEDGER_MAX records makes room for a new record by removing
 * the oldest ended record of the owner with the most ended records.
 * Sixteen owners fill the ledger, and a hundred new owners each open a
 * record, but the ledger does not remove the one ended record of A, which
 * is older than all of theirs. */
static void
test_fair (struct ledger *l)
{
	struct ledger_metrics const *m = ledger_metrics(l);
	uint64_t gone = atomic_load(&m->gone);
	int e = 0;
	struct ledger_task *a = test_open(l, A, LEDGER_TASK, &e);
	ledger_end(l, a, LEDGER_COMPLETED);
	for (uint64_t k = 0; k < LEDGER_MAX / LEDGER_MINE; ++k)
		for (uint64_t i = !k; i < LEDGER_MINE; ++i)
			test_end(l, test_open(l, 100 + k, LEDGER_TASK, &e));
	CHECK(l->n == LEDGER_MAX && atomic_load(&m->gone) == gone);
	for (uint64_t k = 0; k < 100; ++k)
		test_end(l, test_open(l, 200 + k, LEDGER_JOB, &e));
	CHECK(!e && l->n == LEDGER_MAX && atomic_load(&m->gone) == gone + 100 &&
	      a->owner && test_found(l, A, a->id) &&
	      test_owners(l) == 1 + LEDGER_MAX / LEDGER_MINE + 100);
	ledger_task_put(a);
	clock_ns += 2 * LEDGER_KEEP * NS;
	test_sweep(l);
	CHECK(!l->n && !test_owners(l));
}

/* ledger_find() returns a record only to its owner.  The ledger removes
 * an ended record LEDGER_KEEP seconds after its call ends, and frees an
 * owner without records. */
static void
test_keep (struct ledger *l)
{
	int e = 0;
	struct ledger_task *a = test_open(l, A, LEDGER_TASK, &e);
	struct ledger_task *b = test_open(l, B, LEDGER_JOB, &e);
	CHECK(test_found(l, A, a->id) && !test_found(l, B, a->id) &&
	      !test_found(l, A, b->id) && !ledger_find(l, A, a->id, LEDGER_ID - 1));
	ledger_end(l, a, LEDGER_COMPLETED);
	clock_ns += (LEDGER_KEEP - 1) * NS;
	ledger_end(l, b, LEDGER_FAILED);
	CHECK(test_found(l, A, a->id) && test_found(l, B, b->id) && test_owners(l) == 2);
	clock_ns += 2 * NS;
	CHECK(!test_found(l, A, a->id) && !a->owner && test_found(l, B, b->id) &&
	      test_owners(l) == 1 && !ledger_owner_find(l, A));
	ledger_task_put(a);
	ledger_task_put(b);
	clock_ns += LEDGER_KEEP * NS;
	test_sweep(l);
	CHECK(!l->n && !test_owners(l));
}

/* ledger_task_cancel() posts the record's message to the loop of its call
 * once, with a reference, and the handler drops that reference. */
static void
test_cancels (struct ledger *l)
{
	int e = 0;
	struct ledger_task *t = test_open(l, A, LEDGER_JOB, &e);
	ledger_task_cancel(t);
	ledger_task_cancel(t);
	CHECK(atomic_load(&t->refs) == 3);
	loop_fini(test_loop);
	CHECK(cancels == 1 && atomic_load(&t->refs) == 2);
	test_end(l, t);
}

int
main (void)
{
	struct loop loop_of_calls = loop();
	if (loop_init(&loop_of_calls)) {
		fprintf(stderr, "loop_init failed\n");
		return 1;
	}
	test_loop = &loop_of_calls;
	int e = 0;
	struct ledger *l = ledger_create(&e);
	if (!l) {
		fprintf(stderr, "ledger_create failed: %d\n", e);
		return 1;
	}
	test_invalid(l);
	test_caps(l);
	clock_ns += 2 * LEDGER_KEEP * NS;
	test_sweep(l);
	CHECK(!l->n && !test_owners(l));
	test_fair(l);
	test_mine(l);
	clock_ns += 2 * LEDGER_KEEP * NS;
	test_keep(l);
	test_cancels(l);
	ledger_destroy(&l);
	CHECK(!l);
	if (failures) {
		fprintf(stderr, "ledger: %u checks failed\n", failures);
		return 1;
	}
	printf("ledger: all checks passed\n");
	return 0;
}
