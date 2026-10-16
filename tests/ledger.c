/** @file Unit tests of the ledger (src/ledger.c): the owners of records, the
 *  limits of one owner, the removal of records, and the timers and waits of
 *  owners.  The test includes src/loop.c and src/ledger.c, so that it can
 *  read the ledger's own structures, and replaces clock_gettime() in them
 *  with a fake clock, as tests/turn.c does.  The ledger's source is named by
 *  its path, because "ledger.c" alone would name this file. */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
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

static struct loop *test_loop;   /* the loop of the calls of the records
                                    and of the waits */
static struct loop *other_loop;  /* the loop of the calls of the records
                                    that test_race_ender() ends */
static uint64_t     cancels;     /* cancel messages handled */
static uint64_t     wakes;       /* messages of waits handled */

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

/* The handler of a wait's message.  It calls ledger_wait_heard() and drops
 * the reference that the message came with. */
static void
test_wake (struct loop *loop,
           struct msg  *msg)
{
	(void)loop;
	struct ledger_wait *w = container_of(msg, struct ledger_wait, wake);
	++wakes;
	ledger_wait_heard(w);
	ledger_wait_put(w);
}

/* Handles the messages that were posted to the test's loop. */
static void
test_deliver (void)
{
	for (loop_collect(test_loop); test_loop->mail; loop_collect(test_loop))
		loop_deliver(test_loop);
}

/* Puts a wait of @p owner into the ledger. */
static struct ledger_wait *
test_wait (struct ledger *l,
           uint64_t       owner,
           int           *e)
{
	return ledger_wait_enter(l, owner, test_loop, (struct handle){}, test_wake, e);
}

/* Sets a timer of @p owner that fires @p secs seconds from now, with the
 * note @p note; returns its number, or 0. */
static uint32_t
test_timer (struct ledger *l,
            uint64_t       owner,
            uint64_t       secs,
            char const    *note,
            int           *e)
{
	uint32_t id = 0;
	*e = ledger_timer_set(l, owner, clock_ns + secs * NS, note, note ? strlen(note) : 0,
	                      &id);
	return id;
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

	char note[LEDGER_NOTE + 1] = {};
	uint32_t id = 0;
	CHECK(ledger_timer_set(nullptr, A, 1, nullptr, 0, &id) == EINVAL &&
	      ledger_timer_set(l, A, 0, nullptr, 0, &id) == EINVAL &&
	      ledger_timer_set(l, A, 1, nullptr, 1, &id) == EINVAL &&
	      ledger_timer_set(l, A, 1, note, sizeof note, &id) == EINVAL &&
	      ledger_timer_set(l, A, 1, nullptr, 0, nullptr) == EINVAL && !id);
	CHECK(ledger_timer_cancel(nullptr, A, 1) == EINVAL &&
	      ledger_timer_cancel(l, A, 1) == ENOENT);
	CHECK(!ledger_wait_enter(nullptr, A, test_loop, (struct handle){}, test_wake, &e) &&
	      e == EINVAL);
	CHECK(!ledger_wait_enter(l, A, nullptr, (struct handle){}, test_wake, &e) &&
	      e == EINVAL);
	CHECK(!ledger_wait_enter(l, A, test_loop, (struct handle){}, nullptr, &e) &&
	      e == EINVAL);
	CHECK(!ledger_wait_enter(l, A, test_loop, (struct handle){}, test_wake, nullptr));
	struct ledger_look look;
	CHECK(ledger_list(nullptr, A, &look) == EINVAL && !look.n_tasks && !look.n_timers &&
	      !look.more && look.due == UINT64_MAX);
	CHECK(ledger_list(l, A, nullptr) == EINVAL);
	CHECK(ledger_tell(l, nullptr, nullptr, 0, &look) == EINVAL && !look.n_tasks);
	ledger_wait_heard(nullptr);
	ledger_wait_put(nullptr);
	ledger_wait_leave(l, nullptr);
	ledger_look_put(nullptr);
	CHECK(!l->n && !test_owners(l) && !l->n_owners);
}

/* A restored record preserves its identity, timestamps and copied result.
 * The current handle receives cancellation for a working record; no
 * restored id can replace any owner's record. */
static void
test_restore (void)
{
	int e = 0;
	struct ledger *l = ledger_create(&e);
	CHECK(l && !e);
	if (!l)
		return;
	unsigned char id[] = "0123456789abcdef0123456789abcdef";
	unsigned char result[] = "\"result\":{\"content\":[]}}";
	struct span key = { id, LEDGER_ID };
	struct span body = { result, sizeof result - 1 };
	struct ledger_wait *w = test_wait(l, A, &e);
	struct ledger_task *done = ledger_restore(l, A, LEDGER_JOB, 7, key, 10, 20,
	                                         LEDGER_COMPLETED, body, nullptr,
	                                         (struct handle){}, nullptr, &e);
	CHECK(done && !e);
	if (!done) {
		ledger_wait_leave(l, &w);
		ledger_destroy(&l);
		return;
	}
	CHECK(done->created == 10 && done->ended == 20 && done->tool == 7 &&
	      done->kind == LEDGER_JOB && atomic_load(&done->status) == LEDGER_COMPLETED &&
	      done->len == body.len && !memcmp(done->result, body.ptr, body.len) &&
	      done->result != (char *)result && test_found(l, A, done->id) &&
	      !test_found(l, B, done->id) && !ledger_owner_find(l, A)->running);
	CHECK(w && atomic_load(&w->posted));
	test_deliver();
	struct ledger_look look;
	CHECK(!ledger_tell(l, w, nullptr, 0, &look) && look.n_tasks == 1 &&
	      look.tasks[0] == done);
	ledger_look_put(&look);
	CHECK(!ledger_restore(l, A, LEDGER_JOB, 9, key, 99, 0, LEDGER_WORKING,
	                      (struct span){}, test_loop, (struct handle){}, test_cancel,
	                      &e) && e == EEXIST);
	CHECK(!ledger_restore(l, B, LEDGER_JOB, 9, key, 99, 0, LEDGER_FAILED,
	                      (struct span){}, nullptr, (struct handle){}, nullptr,
	                      &e) && e == EEXIST);
	CHECK(l->n == 1 && done->tool == 7 && done->created == 10 &&
	      atomic_load(&done->status) == LEDGER_COMPLETED &&
	      !ledger_owner_find(l, B));

	/* Restore in reverse creation order; listing still orders by creation.
	 * The recovered handle, rather than the one from the former process,
	 * receives the cancel. */
	id[0] = '1';
	struct handle who = { .bits = UINT64_C(0x0100000300000004) };
	struct ledger_task *running = ledger_restore(l, A, LEDGER_JOB, 8, key, 5, 0,
	                                            LEDGER_WORKING, (struct span){},
	                                            test_loop, who, test_cancel, &e);
	CHECK(running && !e);
	if (running) {
		CHECK(running->who.bits == who.bits &&
		      ledger_owner_find(l, A)->running == 1);
		CHECK(!ledger_list(l, A, &look) && look.n_tasks == 2 &&
		      look.tasks[0] == done && look.tasks[1] == running);
		ledger_look_put(&look);
		uint64_t seen = cancels;
		ledger_task_cancel(running);
		ledger_task_cancel(running);
		test_deliver();
		CHECK(cancels == seen + 1 && atomic_load(&running->refs) == 2);
		ledger_end(l, running, LEDGER_CANCELLED);
		ledger_task_put(running);
	}
	test_deliver();
	ledger_wait_leave(l, &w);
	clock_ns += (LEDGER_KEEP - 1) * NS;
	CHECK(test_found(l, A, done->id));
	clock_ns += 2 * NS;
	CHECK(!test_found(l, A, done->id) && !l->n);
	ledger_task_put(done);
	ledger_destroy(&l);
}

/* Restoring never bypasses validation or working-job admission limits,
 * but terminal records do not consume a running-job slot. */
static void
test_restore_limits (void)
{
	int e = 0;
	struct ledger *l = ledger_create(&e);
	CHECK(l && !e);
	if (!l)
		return;
	unsigned char id[LEDGER_ID];
	memset(id, '0', sizeof id);
	struct span key = { id, sizeof id };
	CHECK(!ledger_restore(l, A, LEDGER_JOB, 1, (struct span){ id, LEDGER_ID - 1 },
	                      1, 0, LEDGER_WORKING, (struct span){}, test_loop,
	                      (struct handle){}, test_cancel, &e) && e == EINVAL);
	id[0] = 'G';
	CHECK(!ledger_restore(l, A, LEDGER_JOB, 1, key, 1, 0, LEDGER_WORKING,
	                      (struct span){}, test_loop, (struct handle){}, test_cancel,
	                      &e) && e == EINVAL);
	id[0] = '0';
	CHECK(!ledger_restore(l, A, LEDGER_JOB, 1, key, 1, 0,
	                      (enum ledger_status)-1, (struct span){}, nullptr,
	                      (struct handle){}, nullptr, &e) && e == EINVAL);
	CHECK(!ledger_restore(l, A, LEDGER_JOB, 1, key, 1, 0, LEDGER_FAILED,
	                      (struct span){ nullptr, 1 }, nullptr, (struct handle){},
	                      nullptr, &e) && e == EINVAL);
	CHECK(!ledger_restore(l, A, LEDGER_JOB, 1, key, 1, 0, LEDGER_WORKING,
	                      (struct span){}, nullptr, (struct handle){}, test_cancel,
	                      &e) && e == EINVAL);
	CHECK(!ledger_restore(l, A, LEDGER_JOB, 1, key, 1, 2, LEDGER_WORKING,
	                      (struct span){}, test_loop, (struct handle){}, test_cancel,
	                      &e) && e == EINVAL);
	CHECK(!ledger_restore(l, A, LEDGER_JOB, 1, key, 1, 0, LEDGER_WORKING,
	                      key, test_loop, (struct handle){}, test_cancel,
	                      &e) && e == EINVAL);
	CHECK(!ledger_restore(l, A, LEDGER_JOB, 1, key, 1, 2, LEDGER_CANCELLED,
	                      key, nullptr, (struct handle){}, nullptr,
	                      &e) && e == EINVAL);
	CHECK(!l->n && !l->n_owners);
	struct ledger_task *running[LEDGER_RUNNING];
	for (size_t k = 0; k < LEDGER_RUNNING; ++k) {
		id[0] = '0' + k;
		running[k] = ledger_restore(l, A, LEDGER_JOB, 1, key, 1, 0, LEDGER_WORKING,
		                            (struct span){}, test_loop, (struct handle){},
		                            test_cancel, &e);
		CHECK(running[k] && !e);
	}
	id[0] = 'f';
	CHECK(!ledger_restore(l, A, LEDGER_JOB, 1, key, 1, 0, LEDGER_WORKING,
	                      (struct span){}, test_loop, (struct handle){}, test_cancel,
	                      &e) && e == EBUSY);
	struct ledger_task *done = ledger_restore(l, A, LEDGER_JOB, 1, key, 1, 2,
	                                         LEDGER_FAILED, (struct span){}, nullptr,
	                                         (struct handle){}, nullptr, &e);
	CHECK(done && !e && ledger_owner_find(l, A)->running == LEDGER_RUNNING &&
	      l->n == LEDGER_RUNNING + 1);
	ledger_task_put(done);
	for (size_t k = 0; k < LEDGER_RUNNING; ++k)
		test_end(l, running[k]);
	ledger_destroy(&l);
}

/* Removing an ended record allows its durable job to be restored under
 * the same id without invalidating readers of the earlier record. */
static void
test_remove (void)
{
	int e = 0;
	struct ledger *l = ledger_create(&e);
	CHECK(l && !e);
	if (!l)
		return;
	struct ledger_task *old = test_open(l, A, LEDGER_JOB, &e);
	CHECK(old && !e);
	if (!old) {
		ledger_destroy(&l);
		return;
	}
	struct span id = { (unsigned char *)old->id, LEDGER_ID };
	CHECK(ledger_remove(nullptr, A, id) == EINVAL &&
	      ledger_remove(l, A, (struct span){}) == EINVAL &&
	      ledger_remove(l, A, (struct span){ id.ptr, LEDGER_ID - 1 }) == EINVAL &&
	      ledger_remove(l, B, id) == ENOENT &&
	      ledger_remove(l, A, id) == EBUSY && test_found(l, A, old->id));
	unsigned char result[] = "\"error\":{\"message\":\"interrupted\"}}";
	struct span body = { result, sizeof result - 1 };
	CHECK(!ledger_task_reserve(old, body.len) && !ledger_task_write(old, body));
	ledger_end(l, old, LEDGER_FAILED);
	struct ledger_task *reader = ledger_find(l, A, old->id, LEDGER_ID);
	CHECK(reader == old && atomic_load(&old->refs) == 3);
	CHECK(ledger_remove(l, B, id) == ENOENT && !ledger_remove(l, A, id) &&
	      ledger_remove(l, A, id) == ENOENT && !test_found(l, A, old->id) &&
	      !old->owner && atomic_load(&old->refs) == 2 &&
	      atomic_load(&old->status) == LEDGER_FAILED &&
	      old->len == body.len && !memcmp(old->result, body.ptr, body.len) &&
	      !l->n && !l->n_owners && !l->bytes);
	struct ledger_task *next = ledger_restore(l, A, LEDGER_JOB, 5, id, old->created, 0,
	                                         LEDGER_WORKING, (struct span){}, test_loop,
	                                         (struct handle){}, test_cancel, &e);
	CHECK(next && !e && next != old && test_found(l, A, old->id));
	if (next) {
		ledger_end(l, next, LEDGER_COMPLETED);
		CHECK(!ledger_remove(l, A, id) && !l->n);
		ledger_task_put(next);
	}
	ledger_task_put(reader);
	ledger_task_put(old);
	ledger_destroy(&l);
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

	/* A list returns the newest LEDGER_MINE records of A, the newest
	 * first: t[0], then t[LEDGER_MINE - 1] to t[1].  The oldest record,
	 * t[LEDGER_MINE], is the one record that it leaves out. */
	struct ledger_look look;
	CHECK(!ledger_list(l, A, &look) && look.n_tasks == LEDGER_MINE && look.more == 1 &&
	      look.tasks[0] == t[0] && look.tasks[1] == t[LEDGER_MINE - 1] &&
	      look.tasks[LEDGER_MINE - 1] == t[1]);
	ledger_look_put(&look);
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

/* An owner has at most LEDGER_TIMERS timers.  One counter numbers the
 * timers of all owners.  A list returns them by number, with the time of
 * the first that has not fired.
 * A cancel removes a timer that has not fired, once.  A fired timer stays
 * until LEDGER_KEEP seconds after it fired, and the ledger removes the
 * owner with the last of its timers. */
static void
test_timers (struct ledger *l)
{
	struct ledger_metrics const *m = ledger_metrics(l);
	int e = 0;
	/* Timer base + k fires in 10 - k seconds, so timer base + 8 fires
	 * first.  The timers of B get the next numbers, also after the ledger
	 * has freed B and made it again. */
	uint64_t start = clock_ns;
	uint32_t base = l->n_timers;
	for (uint32_t k = 1; k <= LEDGER_TIMERS; ++k)
		CHECK(test_timer(l, A, 10 - k, k == 2 ? "two" : nullptr, &e) == base + k && !e);
	CHECK(!test_timer(l, A, 5, nullptr, &e) && e == EBUSY);
	CHECK(atomic_load(&m->timers) == LEDGER_TIMERS && test_owners(l) == 1 &&
	      l->n_owners == 1);
	uint32_t b = test_timer(l, B, 5, nullptr, &e);
	CHECK(b == base + LEDGER_TIMERS + 1 && !e && l->n_owners == 2 &&
	      ledger_timer_cancel(l, A, b) == ENOENT);
	CHECK(!ledger_timer_cancel(l, B, b) && ledger_timer_cancel(l, B, b) == ENOENT &&
	      l->n_owners == 1 && !ledger_owner_find(l, B));
	CHECK(test_timer(l, B, 5, nullptr, &e) == b + 1 && !e &&
	      !ledger_timer_cancel(l, B, b + 1) && !ledger_owner_find(l, B));

	CHECK(!ledger_timer_cancel(l, A, base + 3) &&
	      ledger_timer_cancel(l, A, base + 3) == ENOENT &&
	      ledger_timer_cancel(l, A, 0) == ENOENT && ledger_timer_cancel(l, C, 1) == ENOENT);
	struct ledger_look look;
	CHECK(!ledger_list(l, A, &look) && look.n_timers == LEDGER_TIMERS - 1 &&
	      !look.n_tasks && !look.more && look.due == start + 2 * NS);
	for (uint32_t k = 0; k < look.n_timers; ++k)
		CHECK(look.timers[k].id == base + (k < 2 ? k + 1 : k + 2));
	CHECK(look.timers[1].len == 3 && !memcmp(look.timers[1].note, "two", 3) &&
	      !look.timers[0].len);

	/* Timer base + 8 fires.  A cancel cannot remove it and returns
	 * EALREADY, and a list still returns it, but not as the first that has
	 * not fired. */
	clock_ns = start + 2 * NS;
	CHECK(ledger_timer_cancel(l, A, base + 8) == EALREADY);
	CHECK(!ledger_list(l, A, &look) && look.n_timers == LEDGER_TIMERS - 1 &&
	      look.timers[6].id == base + 8 && look.due == start + 3 * NS);
	CHECK(test_timer(l, A, 1, nullptr, &e) == base + LEDGER_TIMERS + 3 && !e);
	CHECK(!test_timer(l, A, 1, nullptr, &e) && e == EBUSY);

	/* The new timer is in the place of timer base + 3, but a list still
	 * returns the timers by number. */
	CHECK(!ledger_list(l, A, &look) && look.n_timers == LEDGER_TIMERS &&
	      look.timers[LEDGER_TIMERS - 1].id == base + LEDGER_TIMERS + 3);
	for (uint32_t k = 1; k < look.n_timers; ++k)
		CHECK(look.timers[k - 1].id < look.timers[k].id);

	/* LEDGER_KEEP seconds after timer base + 1 fired, every timer has
	 * expired.  A new timer of A removes them, and once it is cancelled,
	 * the owner has nothing left. */
	clock_ns = start + (9 + LEDGER_KEEP) * NS;
	CHECK(test_timer(l, A, 1, nullptr, &e) == base + LEDGER_TIMERS + 4 && !e &&
	      !ledger_timer_cancel(l, A, base + LEDGER_TIMERS + 4));
	CHECK(!ledger_list(l, A, &look) && !look.n_timers && look.due == UINT64_MAX &&
	      !test_owners(l) && !l->n_owners);
}

/* A wait gets its message when a record of its owner ends or its owner
 * sets a timer, once until the handler calls ledger_wait_heard().
 * ledger_tell() returns each ended record that no one has read, and each
 * fired timer, to one wait only.  A wait that names records gets those
 * that have ended, read or not.  A wait that leaves with its message
 * posted is freed when the handler drops the message's reference. */
static void
test_waits (struct ledger *l)
{
	struct ledger_metrics const *m = ledger_metrics(l);
	int e = 0;
	struct ledger_wait *w[LEDGER_WAITS];
	for (size_t k = 0; k < LEDGER_WAITS; ++k)
		CHECK((w[k] = test_wait(l, A, &e)) && !e);
	CHECK(!test_wait(l, A, &e) && e == EBUSY && atomic_load(&m->waits) == LEDGER_WAITS);
	struct ledger_wait *b = test_wait(l, B, &e);
	CHECK(b && !e && l->n_owners == 2);
	for (size_t k = 2; k < LEDGER_WAITS; ++k)
		ledger_wait_leave(l, &w[k]);
	CHECK(!w[2] && !w[3] && atomic_load(&m->waits) == 3);

	/* Two ends post each wait of A once, and no wait of B. */
	struct ledger_task *t[5];
	for (size_t k = 0; k < ARRAY_SIZE(t); ++k)
		t[k] = test_open(l, A, LEDGER_JOB, &e);
	uint64_t seen = wakes;
	ledger_end(l, t[0], LEDGER_COMPLETED);
	ledger_end(l, t[1], LEDGER_FAILED);
	CHECK(atomic_load(&w[0]->posted) && atomic_load(&w[0]->refs) == 2 &&
	      !atomic_load(&b->posted) && atomic_load(&b->refs) == 1);
	test_deliver();
	CHECK(wakes == seen + 2 && !atomic_load(&w[0]->posted) &&
	      atomic_load(&w[0]->refs) == 1 && atomic_load(&w[1]->refs) == 1);

	/* One wait gets both ended records, as references, and marks them
	 * read; the other gets neither. */
	struct ledger_look look;
	CHECK(!ledger_tell(l, w[1], nullptr, 0, &look) && look.n_tasks == 2 &&
	      look.tasks[0] == t[0] && look.tasks[1] == t[1] && !look.n_timers &&
	      look.due == UINT64_MAX && atomic_load(&t[0]->told) &&
	      atomic_load(&t[1]->refs) == 3);
	ledger_look_put(&look);
	CHECK(!look.n_tasks && atomic_load(&t[1]->refs) == 2);
	CHECK(!ledger_tell(l, w[0], nullptr, 0, &look) && !look.n_tasks);

	/* A wait that names t[0], t[2] and t[4], of which t[4] works, gets
	 * t[0] and t[2]; the other wait then gets t[3] only. */
	ledger_end(l, t[2], LEDGER_COMPLETED);
	ledger_end(l, t[3], LEDGER_COMPLETED);
	char ids[3][LEDGER_ID];
	memcpy(ids[0], t[0]->id, LEDGER_ID);
	memcpy(ids[1], t[2]->id, LEDGER_ID);
	memcpy(ids[2], t[4]->id, LEDGER_ID);
	CHECK(!ledger_tell(l, w[0], ids[0], 3, &look) && look.n_tasks == 2 &&
	      look.tasks[0] == t[0] && look.tasks[1] == t[2]);
	ledger_look_put(&look);
	CHECK(!ledger_tell(l, w[1], nullptr, 0, &look) && look.n_tasks == 1 &&
	      look.tasks[0] == t[3]);
	ledger_look_put(&look);
	test_deliver();

	/* A timer set posts the owner's waits.  A fired timer goes to one wait,
	 * also to a wait that names records, and the ledger removes it. */
	seen = wakes;
	uint32_t first = l->n_timers + 1;
	uint32_t one = test_timer(l, A, 1, "n", &e);
	uint32_t two = test_timer(l, A, 5, nullptr, &e);
	CHECK(one == first && two == first + 1 && atomic_load(&w[0]->posted) &&
	      atomic_load(&w[1]->posted));
	test_deliver();
	CHECK(wakes == seen + 2);
	CHECK(!ledger_tell(l, w[0], nullptr, 0, &look) && !look.n_timers &&
	      look.due == clock_ns + NS);
	clock_ns += NS;
	CHECK(!ledger_tell(l, w[0], ids[2], 1, &look) && look.n_timers == 1 &&
	      look.timers[0].id == one && look.timers[0].len == 1 &&
	      look.timers[0].note[0] == 'n' && look.due == clock_ns + 4 * NS && !look.n_tasks);
	CHECK(!ledger_tell(l, w[1], nullptr, 0, &look) && !look.n_timers &&
	      look.due == clock_ns + 4 * NS);
	CHECK(ledger_timer_cancel(l, A, one) == ENOENT && !ledger_timer_cancel(l, A, two));

	/* A wait leaves with its message posted, and the handler's drop frees
	 * it.  A wait out of the ledger gets nothing. */
	seen = wakes;
	ledger_end(l, t[4], LEDGER_COMPLETED);
	CHECK(atomic_load(&w[0]->refs) == 2);
	ledger_wait_leave(l, &w[0]);
	CHECK(!w[0] && atomic_load(&m->waits) == 2);
	test_deliver();
	CHECK(wakes == seen + 2);
	struct ledger_wait *left = w[1];
	atomic_fetch_add(&left->refs, 1);
	ledger_wait_leave(l, &w[1]);
	CHECK(ledger_tell(l, left, nullptr, 0, &look) == EINVAL && !look.n_tasks);
	ledger_wait_put(left);
	ledger_wait_leave(l, &b);
	CHECK(!atomic_load(&m->waits) && !ledger_owner_find(l, B) && ledger_owner_find(l, A));
	for (size_t k = 0; k < ARRAY_SIZE(t); ++k)
		ledger_task_put(t[k]);
}

/* An owner stays while it has a record, a timer or a wait.  The ledger has
 * at most LEDGER_OWNERS owners, and an owner that has only timers counts.
 * A full table refuses a new owner with ENOSPC, until the removal of
 * expired timers leaves owners with nothing. */
static void
test_owners_table (struct ledger *l)
{
	struct ledger_metrics const *m = ledger_metrics(l);
	int e = 0;
	struct ledger_task *t = test_open(l, C, LEDGER_TASK, &e);
	uint32_t id = test_timer(l, C, 10, nullptr, &e);
	struct ledger_wait *w = test_wait(l, C, &e);
	test_end(l, t);
	clock_ns += (LEDGER_KEEP + 1) * NS;
	test_sweep(l);
	CHECK(!l->n && ledger_owner_find(l, C) && l->n_owners == 1);
	ledger_wait_leave(l, &w);
	CHECK(ledger_owner_find(l, C) && ledger_timer_cancel(l, C, id) == EALREADY);
	struct ledger_look look;
	CHECK(!ledger_list(l, C, &look) && look.n_timers == 1 && look.due == UINT64_MAX &&
	      ledger_owner_find(l, C));
	/* Once the fired timer has expired, a cancel returns ENOENT, and the
	 * ledger frees the owner. */
	clock_ns += 10 * NS;
	CHECK(ledger_timer_cancel(l, C, id) == ENOENT && !ledger_owner_find(l, C));
	CHECK(!ledger_list(l, C, &look) && !look.n_timers && !ledger_owner_find(l, C));
	w = test_wait(l, C, &e);
	CHECK(w && l->n_owners == 1);
	ledger_wait_leave(l, &w);
	CHECK(!l->n_owners && !test_owners(l));

	uint32_t base = l->n_timers;
	for (uint32_t k = 0; k < LEDGER_OWNERS; ++k)
		CHECK(test_timer(l, 100 + k, 1, nullptr, &e) == base + k + 1 && !e);
	CHECK(l->n_owners == LEDGER_OWNERS && test_owners(l) == LEDGER_OWNERS);
	uint64_t full = atomic_load(&m->full);
	CHECK(!test_timer(l, A, 1, nullptr, &e) && e == ENOSPC);
	CHECK(!test_wait(l, A, &e) && e == ENOSPC);
	CHECK(!test_open(l, A, LEDGER_TASK, &e) && e == ENOSPC &&
	      atomic_load(&m->full) == full + 1);
	CHECK(test_timer(l, 100, 5, nullptr, &e) == base + LEDGER_OWNERS + 1 && !e);

	/* LEDGER_KEEP seconds after the first timers fired, a new owner makes
	 * the ledger remove them.  Owner 100 has a timer that has not expired. */
	clock_ns += (LEDGER_KEEP + 1) * NS;
	CHECK(test_timer(l, A, 1, nullptr, &e) == base + LEDGER_OWNERS + 2 && !e &&
	      l->n_owners == 2 && ledger_owner_find(l, 100) && !ledger_owner_find(l, 101));
	CHECK(!ledger_timer_cancel(l, A, base + LEDGER_OWNERS + 2) && l->n_owners == 1);
	clock_ns += 10 * NS;
	CHECK(!ledger_list(l, 100, &look) && !look.n_timers && !l->n_owners &&
	      !test_owners(l));
}

/* Owners whose keys have the bit LEDGER_ANONYMOUS fill at most half of the
 * table of owners, and another owner still gets a place.  The removal of
 * expired timers makes room for them again. */
static void
test_anonymous (struct ledger *l)
{
	int e = 0;
	uint64_t k = 0;
	for (; k < LEDGER_OWNERS / 2; ++k)
		CHECK(test_timer(l, LEDGER_ANONYMOUS | k, 1, nullptr, &e) && !e);
	CHECK(!test_timer(l, LEDGER_ANONYMOUS | k, 1, nullptr, &e) && e == ENOSPC);
	CHECK(!test_wait(l, LEDGER_ANONYMOUS | k, &e) && e == ENOSPC);
	CHECK(!test_open(l, LEDGER_ANONYMOUS | k, LEDGER_TASK, &e) && e == ENOSPC);
	uint32_t id = test_timer(l, B, 1, nullptr, &e);
	CHECK(id && !e && l->n_owners == LEDGER_OWNERS / 2 + 1 &&
	      l->n_anonymous == LEDGER_OWNERS / 2);
	CHECK(!ledger_timer_cancel(l, B, id) && l->n_owners == LEDGER_OWNERS / 2);
	clock_ns += (LEDGER_KEEP + 1) * NS;
	CHECK(test_timer(l, LEDGER_ANONYMOUS | k, 1, nullptr, &e) && !e &&
	      l->n_owners == 1 && l->n_anonymous == 1);
	clock_ns += (LEDGER_KEEP + 1) * NS;
	struct ledger_look look;
	CHECK(!ledger_list(l, LEDGER_ANONYMOUS | k, &look) && !look.n_timers &&
	      !l->n_owners && !l->n_anonymous && !test_owners(l));
}

enum {
	RACE_BATCH   = LEDGER_MINE / 2,  /* records that the ender ends at once */
	RACE_BATCHES = 200,
	RACE_STALL   = 5,                /* seconds without a record for the
	                                    wait: a lost message */
};

static _Atomic uint64_t race_told;  /* records that the wait got */
static _Atomic bool     race_stop;  /* the main thread waits no more */

/* The thread that ends records of B: RACE_BATCHES batches of RACE_BATCH
 * records, each batch after the wait got the records of the one before. */
static void *
test_race_ender (void *arg)
{
	struct ledger *l = arg;
	int e = 0;
	for (uint64_t k = 1; k <= RACE_BATCHES && !atomic_load(&race_stop); ++k) {
		for (size_t i = 0; i < RACE_BATCH; ++i)
			test_end(l, ledger_open(l, B, LEDGER_TASK, 5, other_loop, (struct handle){},
			                        test_cancel, &e));
		while (atomic_load(&race_told) < k * RACE_BATCH && !atomic_load(&race_stop))
			sched_yield();
	}
	return nullptr;
}

/* Ends of records on another thread reach a wait whose loop is the main
 * thread.  The calls of the records are on another loop, so a message that
 * goes to the loop of a record's call does not reach the wait.  The main
 * thread calls ledger_tell() only after a message of the wait, as a waiting
 * call does, so a lost message stops the records from reaching the wait. */
static void
test_race (struct ledger *l)
{
	int e = 0;
	struct ledger_wait *w = test_wait(l, B, &e);
	pthread_t ender;
	if (!w || pthread_create(&ender, nullptr, test_race_ender, l)) {
		CHECK(!"the race starts");
		ledger_wait_leave(l, &w);
		return;
	}
	uint64_t const total = RACE_BATCH * RACE_BATCHES;
	time_t got = time(nullptr);
	while (atomic_load(&race_told) < total && time(nullptr) - got < RACE_STALL) {
		uint64_t seen = wakes;
		test_deliver();
		if (wakes == seen) {
			sched_yield();
			continue;
		}
		struct ledger_look look;
		CHECK(!ledger_tell(l, w, nullptr, 0, &look));
		atomic_fetch_add(&race_told, look.n_tasks);
		got = look.n_tasks ? time(nullptr) : got;
		ledger_look_put(&look);
	}
	CHECK(atomic_load(&race_told) == total);
	atomic_store(&race_stop, true);
	pthread_join(ender, nullptr);
	test_deliver();
	ledger_wait_leave(l, &w);
}

/* ledger_task_cancel() posts the record's message to the loop of its call
 * once, with a reference, and the handler drops that reference. */
static void
test_cancels (struct ledger *l)
{
	int e = 0;
	uint64_t seen = cancels;
	struct ledger_task *t = test_open(l, A, LEDGER_JOB, &e);
	ledger_task_cancel(t);
	ledger_task_cancel(t);
	CHECK(atomic_load(&t->refs) == 3);
	loop_fini(test_loop);
	CHECK(cancels == seen + 1 && atomic_load(&t->refs) == 2);
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
	struct loop loop_of_others = loop();
	if (loop_init(&loop_of_others)) {
		fprintf(stderr, "loop_init failed\n");
		return 1;
	}
	other_loop = &loop_of_others;
	int e = 0;
	struct ledger *l = ledger_create(&e);
	if (!l) {
		fprintf(stderr, "ledger_create failed: %d\n", e);
		return 1;
	}
	test_invalid(l);
	test_restore();
	test_restore_limits();
	test_remove();
	test_caps(l);
	clock_ns += 2 * LEDGER_KEEP * NS;
	test_sweep(l);
	CHECK(!l->n && !test_owners(l));
	test_fair(l);
	test_mine(l);
	clock_ns += 2 * LEDGER_KEEP * NS;
	test_keep(l);
	test_timers(l);
	test_waits(l);
	test_owners_table(l);
	test_anonymous(l);
	test_race(l);
	test_cancels(l);
	ledger_destroy(&l);
	CHECK(!l);
	loop_fini(other_loop);
	if (failures) {
		fprintf(stderr, "ledger: %u checks failed\n", failures);
		return 1;
	}
	printf("ledger: all checks passed\n");
	return 0;
}
