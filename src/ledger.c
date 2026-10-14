/** @file The ledger (ledger.h). */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#include "ledger.h"
#include "list.h"
#include "loop.h"
#include "util.h"

enum {
	BUCKETS = LEDGER_MAX,      /* the table's, a power of two */
	BITS    = LEDGER_ID / 2,   /* random bytes of an id */
};

static_assert(!(BUCKETS & (BUCKETS - 1)), "a mask picks the bucket");

static constexpr uint64_t NS = 1'000'000'000;

struct ledger {
	pthread_mutex_t        lock;
	struct list            ended;     /* records whose calls ended, oldest
	                                     first */
	struct ledger_metrics  metrics;
	size_t                 bytes;     /* of the ended records' results */
	uint64_t               n;         /* records kept */
	struct ledger_task    *buckets[BUCKETS];
};

static void
ledger_add (_Atomic uint64_t *p,
            uint64_t          n)
{
	atomic_fetch_add_explicit(p, n, memory_order_relaxed);
}

static void
ledger_sub (_Atomic uint64_t *p,
            uint64_t          n)
{
	atomic_fetch_sub_explicit(p, n, memory_order_relaxed);
}

/* The time of day, CLOCK_REALTIME ns; 0 if the clock cannot tell. */
static uint64_t
ledger_now (void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts))
		return 0;
	return (uint64_t)ts.tv_sec * NS + (uint64_t)ts.tv_nsec;
}

/* Take @p t, an ended record, out of the table and off the ended list,
 * the lock held: its reference is the ledger's no more. */
static void
ledger_drop (struct ledger      *l,
             struct ledger_task *t)
{
	struct ledger_task **p = &l->buckets[t->hash & (BUCKETS - 1)];
	while (*p != t)
		p = &(*p)->next;
	*p = t->next;
	list_del(&t->hook);
	l->bytes -= t->len;
	--l->n;
	ledger_sub(&l->metrics.kept, 1);
	ledger_sub(&l->metrics.bytes, t->len);
	ledger_add(&l->metrics.gone, 1);
}

/*
 * Let go of the ended records whose time is up, and of the oldest ended
 * while the results pass LEDGER_BYTES, or, with @p room, while the ledger
 * is full; never of @p keep.  The lock held: returns those let go of,
 * chained by next, for their references to be dropped once it is not.
 */
static struct ledger_task *
ledger_sweep (struct ledger      *l,
              struct ledger_task *keep,
              bool                room)
{
	uint64_t now = loop_clock();
	struct ledger_task *gone = nullptr;
	while (!list_is_empty(&l->ended)) {
		struct ledger_task *t = container_of(l->ended.next, struct ledger_task, hook);
		if (t == keep || (t->due > now && l->bytes <= LEDGER_BYTES &&
		                  !(room && l->n == LEDGER_MAX)))
			break;
		ledger_drop(l, t);
		t->next = gone;
		gone = t;
	}
	return gone;
}

/* Drop the references to the records chained at @p gone. */
static void
ledger_put_all (struct ledger_task *gone)
{
	while (gone) {
		struct ledger_task *next = gone->next;
		ledger_task_put(gone);
		gone = next;
	}
}

struct ledger *
ledger_create (int *e)
{
	struct ledger *l = malloc(sizeof *l);
	*e = l ? pthread_mutex_init(&l->lock, nullptr) : ENOMEM;
	if (*e) {
		free(l);
		return nullptr;
	}
	list_init(&l->ended);
	l->bytes = 0;
	l->n = 0;
	memset(l->buckets, 0, sizeof l->buckets);
	struct ledger_metrics *m = &l->metrics;
	atomic_init(&m->opened, 0);
	for (size_t i = 0; i < LEDGER_STATUSES; ++i)
		atomic_init(&m->ended[i], 0);
	atomic_init(&m->gone, 0);
	atomic_init(&m->full, 0);
	atomic_init(&m->kept, 0);
	atomic_init(&m->bytes, 0);
	return l;
}

void
ledger_destroy (struct ledger **p_ledger)
{
	struct ledger *l = *p_ledger;
	if (!l)
		return;
	*p_ledger = nullptr;
	for (size_t i = 0; i < BUCKETS; ++i)
		ledger_put_all(l->buckets[i]);
	pthread_mutex_destroy(&l->lock);
	free(l);
}

struct ledger_metrics const *
ledger_metrics (struct ledger const *ledger)
{
	return &ledger->metrics;
}

struct ledger_task *
ledger_open (struct ledger  *ledger,
             uint64_t        principal,
             struct loop    *loop,
             struct handle   who,
             void          (*fn) (struct loop *loop,
                                  struct msg  *msg),
             int            *e)
{
	static char const hex[] = "0123456789abcdef";
	unsigned char bits[BITS];
	ssize_t got = getrandom(bits, sizeof bits, 0);
	if (got != (ssize_t)sizeof bits) {
		*e = got < 0 ? errno : EAGAIN;
		return nullptr;
	}
	struct ledger_task *t = malloc(sizeof *t);
	if (!t) {
		*e = ENOMEM;
		return nullptr;
	}
	t->cancel.fn = fn;
	list_init(&t->hook);
	t->loop = loop;
	t->result = nullptr;
	t->len = t->size = 0;
	t->principal = principal;
	t->created = ledger_now();
	t->ended = t->due = 0;
	t->who = who;
	atomic_init(&t->status, LEDGER_WORKING);
	atomic_init(&t->refs, 2);
	atomic_init(&t->asked, 0);
	for (size_t i = 0; i < BITS; ++i) {
		t->id[2 * i] = hex[bits[i] >> 4];
		t->id[2 * i + 1] = hex[bits[i] & 15];
	}
	t->hash = util_hash(0, t->id, LEDGER_ID);

	pthread_mutex_lock(&ledger->lock);
	struct ledger_task *gone = ledger_sweep(ledger, nullptr, true);
	bool full = ledger->n == LEDGER_MAX;
	if (!full) {
		struct ledger_task **bucket = &ledger->buckets[t->hash & (BUCKETS - 1)];
		t->next = *bucket;
		*bucket = t;
		++ledger->n;
	}
	pthread_mutex_unlock(&ledger->lock);
	ledger_put_all(gone);
	if (full) {
		ledger_add(&ledger->metrics.full, 1);
		free(t);
		*e = EBUSY;
		return nullptr;
	}
	ledger_add(&ledger->metrics.opened, 1);
	ledger_add(&ledger->metrics.kept, 1);
	*e = 0;
	return t;
}

int
ledger_task_reserve (struct ledger_task *task,
                     size_t              len)
{
	free(task->result);
	task->result = malloc(len ? len : 1);
	task->len = 0;
	task->size = task->result ? len : 0;
	return task->result ? 0 : ENOMEM;
}

int
ledger_task_write (struct ledger_task *task,
                   struct span         bytes)
{
	if (bytes.len > task->size - task->len)
		return EPIPE;
	memcpy(task->result + task->len, bytes.ptr, bytes.len);
	task->len += bytes.len;
	return 0;
}

void
ledger_end (struct ledger      *ledger,
            struct ledger_task *task,
            enum ledger_status  status)
{
	if (!task->result || task->len != task->size || status == LEDGER_CANCELLED) {
		free(task->result);
		task->result = nullptr;
		task->len = task->size = 0;
	}
	task->ended = ledger_now();
	task->due = loop_clock() + LEDGER_KEEP * NS;
	pthread_mutex_lock(&ledger->lock);
	atomic_store_explicit(&task->status, status, memory_order_release);
	list_append(&ledger->ended, &task->hook);
	ledger->bytes += task->len;
	struct ledger_task *gone = ledger_sweep(ledger, task, false);
	pthread_mutex_unlock(&ledger->lock);
	ledger_add(&ledger->metrics.ended[status], 1);
	ledger_add(&ledger->metrics.bytes, task->len);
	ledger_put_all(gone);
}

struct ledger_task *
ledger_find (struct ledger *ledger,
             uint64_t       principal,
             char const    *id,
             size_t         len)
{
	if (len != LEDGER_ID)
		return nullptr;
	uint64_t hash = util_hash(0, id, len);
	pthread_mutex_lock(&ledger->lock);
	struct ledger_task *gone = ledger_sweep(ledger, nullptr, false);
	struct ledger_task *t = ledger->buckets[hash & (BUCKETS - 1)];
	while (t && (t->hash != hash || memcmp(t->id, id, len) || t->principal != principal))
		t = t->next;
	if (t)
		atomic_fetch_add_explicit(&t->refs, 1, memory_order_relaxed);
	pthread_mutex_unlock(&ledger->lock);
	ledger_put_all(gone);
	return t;
}

void
ledger_task_cancel (struct ledger_task *task)
{
	if (atomic_exchange_explicit(&task->asked, 1, memory_order_relaxed) ||
	    atomic_load_explicit(&task->status, memory_order_acquire) != LEDGER_WORKING)
		return;
	atomic_fetch_add_explicit(&task->refs, 1, memory_order_relaxed);
	/* A message that loop_post() refuses never runs, so the function drops
	 * the reference of that message itself. */
	if (loop_post(task->loop, &task->cancel))
		ledger_task_put(task);
}

void
ledger_task_put (struct ledger_task *task)
{
	if (!task || atomic_fetch_sub_explicit(&task->refs, 1, memory_order_acq_rel) != 1)
		return;
	free(task->result);
	free(task);
}
