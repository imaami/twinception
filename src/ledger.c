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
	BUCKETS       = LEDGER_MAX,     /* the table's, a power of two */
	OWNER_BUCKETS = 256,            /* the owners' table's, likewise */
	BITS          = LEDGER_ID / 2,  /* random bytes of an id */
};

static_assert(!(BUCKETS & (BUCKETS - 1)), "a mask picks the bucket");
static_assert(!(OWNER_BUCKETS & (OWNER_BUCKETS - 1)), "a mask picks the bucket");
static_assert(LEDGER_MINE > 1, "ledger_insert() must not free the owner as it "
                               "removes a record of the owner");

static constexpr uint64_t NS = 1'000'000'000;

/* The records of one owner.  The ledger frees the owner when it has no
 * records left. */
struct ledger_owner {
	struct list          records;  /* by their member mine, oldest first */
	struct ledger_owner *next;     /* in its bucket */
	uint64_t             key;
	uint32_t             n;        /* records */
	uint32_t             running;  /* records whose calls run */
};

struct ledger {
	pthread_mutex_t        lock;
	struct list            ended;     /* records whose calls ended, oldest
	                                     first */
	struct ledger_metrics  metrics;
	size_t                 bytes;     /* of the ended records' results */
	uint64_t               n;         /* records in the ledger */
	struct ledger_task    *buckets[BUCKETS];
	struct ledger_owner   *owners[OWNER_BUCKETS];
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

/* The owner whose key is @p key, the lock held; nullptr if the ledger has
 * no such owner. */
static struct ledger_owner *
ledger_owner_find (struct ledger *l,
                   uint64_t       key)
{
	struct ledger_owner *o = l->owners[key & (OWNER_BUCKETS - 1)];
	while (o && o->key != key)
		o = o->next;
	return o;
}

/* The owner whose key is @p key, the lock held, made if the ledger has no
 * such owner; nullptr if out of memory. */
static struct ledger_owner *
ledger_owner_get (struct ledger *l,
                  uint64_t       key)
{
	struct ledger_owner *o = ledger_owner_find(l, key);
	if (o)
		return o;
	o = malloc(sizeof *o);
	if (!o)
		return nullptr;
	struct ledger_owner **bucket = &l->owners[key & (OWNER_BUCKETS - 1)];
	*o = (struct ledger_owner){ .next = *bucket, .key = key };
	list_init(&o->records);
	*bucket = o;
	return o;
}

/* Free @p o, the lock held, if it has no records. */
static void
ledger_owner_gc (struct ledger       *l,
                 struct ledger_owner *o)
{
	if (o->n)
		return;
	struct ledger_owner **p = &l->owners[o->key & (OWNER_BUCKETS - 1)];
	while (*p != o)
		p = &(*p)->next;
	*p = o->next;
	free(o);
}

/* The oldest record of @p o whose call has ended, the lock held; nullptr if
 * the calls of all its records run. */
static struct ledger_task *
ledger_owner_ended (struct ledger_owner const *o)
{
	struct list *pos;
	list_foreach(pos, &o->records) {
		struct ledger_task *t = container_of(pos, struct ledger_task, mine);
		if (atomic_load_explicit(&t->status, memory_order_relaxed) != LEDGER_WORKING)
			return t;
	}
	return nullptr;
}

/* Take @p t, an ended record, out of the table, off the ended list and off
 * its owner's list, the lock held, and free the owner if it has no records
 * left.  The ledger's reference to @p t is then the caller's to drop. */
static void
ledger_drop (struct ledger      *l,
             struct ledger_task *t)
{
	struct ledger_task **p = &l->buckets[t->hash & (BUCKETS - 1)];
	while (*p != t)
		p = &(*p)->next;
	*p = t->next;
	list_del(&t->hook);
	list_del(&t->mine);
	struct ledger_owner *o = t->owner;
	t->owner = nullptr;
	--o->n;
	ledger_owner_gc(l, o);
	l->bytes -= t->len;
	--l->n;
	ledger_sub(&l->metrics.kept, 1);
	ledger_sub(&l->metrics.bytes, t->len);
	ledger_add(&l->metrics.gone, 1);
}

/* The owner with the most records whose calls have ended, the lock held;
 * nullptr if the calls of all records run. */
static struct ledger_owner *
ledger_owner_most (struct ledger const *l)
{
	struct ledger_owner *most = nullptr;
	uint32_t max = 0;
	for (size_t i = 0; i < OWNER_BUCKETS; ++i)
		for (struct ledger_owner *o = l->owners[i]; o; o = o->next)
			if (o->n - o->running > max) {
				max = o->n - o->running;
				most = o;
			}
	return most;
}

/*
 * Let go of the ended records whose time is up, and of the oldest ended
 * while the results pass LEDGER_BYTES; never of @p keep.  With @p room, if
 * the ledger is full after that, let go of the oldest ended record of the
 * owner with the most ended records, so that owners with many records do
 * not remove the records of owners with few.  The lock held: returns those
 * let go of, chained by next, for their references to be dropped once it
 * is not.
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
		if (t == keep || (t->due > now && l->bytes <= LEDGER_BYTES))
			break;
		ledger_drop(l, t);
		t->next = gone;
		gone = t;
	}
	struct ledger_owner *o = room && l->n == LEDGER_MAX ? ledger_owner_most(l) : nullptr;
	if (o) {
		/* The owner has an ended record, as o->n - o->running counts them. */
		struct ledger_task *t = ledger_owner_ended(o);
		ledger_drop(l, t);
		t->next = gone;
		gone = t;
	}
	return gone;
}

/* Put @p t, a new record, into the ledger for the owner @p key, the lock
 * held.  If the owner has LEDGER_MINE records or more, removes its oldest
 * ended record from the ledger and chains it to @p *gone.  Returns 0,
 * EBUSY, ENOSPC or ENOMEM, as ledger_open() describes. */
static int
ledger_insert (struct ledger       *l,
               struct ledger_task  *t,
               uint64_t             key,
               struct ledger_task **gone)
{
	if (l->n == LEDGER_MAX)
		return ENOSPC;
	struct ledger_owner *o = ledger_owner_get(l, key);
	if (!o)
		return ENOMEM;
	if (t->kind == LEDGER_JOB && o->running >= LEDGER_RUNNING)
		return EBUSY;
	struct ledger_task *old = o->n >= LEDGER_MINE ? ledger_owner_ended(o) : nullptr;
	if (old) {
		ledger_drop(l, old);
		old->next = *gone;
		*gone = old;
	}
	struct ledger_task **bucket = &l->buckets[t->hash & (BUCKETS - 1)];
	t->next = *bucket;
	*bucket = t;
	++l->n;
	t->owner = o;
	list_append(&o->records, &t->mine);
	++o->n;
	++o->running;
	return 0;
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
	if (!e)
		return nullptr;
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
	memset(l->owners, 0, sizeof l->owners);
	struct ledger_metrics *m = &l->metrics;
	atomic_init(&m->opened, 0);
	atomic_init(&m->jobs, 0);
	for (size_t i = 0; i < LEDGER_STATUSES; ++i)
		atomic_init(&m->ended[i], 0);
	atomic_init(&m->gone, 0);
	atomic_init(&m->full, 0);
	atomic_init(&m->refused, 0);
	atomic_init(&m->kept, 0);
	atomic_init(&m->bytes, 0);
	return l;
}

void
ledger_destroy (struct ledger **p_ledger)
{
	struct ledger *l = p_ledger ? *p_ledger : nullptr;
	if (!l)
		return;
	*p_ledger = nullptr;
	for (size_t i = 0; i < BUCKETS; ++i)
		for (struct ledger_task *t = l->buckets[i], *next; t; t = next) {
			next = t->next;
			t->owner = nullptr;
			ledger_task_put(t);
		}
	for (size_t i = 0; i < OWNER_BUCKETS; ++i)
		for (struct ledger_owner *o = l->owners[i], *next; o; o = next) {
			next = o->next;
			free(o);
		}
	pthread_mutex_destroy(&l->lock);
	free(l);
}

struct ledger_metrics const *
ledger_metrics (struct ledger const *ledger)
{
	return &ledger->metrics;
}

struct ledger_task *
ledger_open (struct ledger    *ledger,
             uint64_t          owner,
             enum ledger_kind  kind,
             uint16_t          tool,
             struct loop      *loop,
             struct handle     who,
             void            (*fn) (struct loop *loop,
                                    struct msg  *msg),
             int              *e)
{
	static char const hex[] = "0123456789abcdef";
	if (!e)
		return nullptr;
	if (!ledger || !loop || !fn || (kind != LEDGER_TASK && kind != LEDGER_JOB)) {
		*e = EINVAL;
		return nullptr;
	}
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
	list_init(&t->mine);
	t->owner = nullptr;
	t->loop = loop;
	t->result = nullptr;
	t->len = t->size = 0;
	t->created = ledger_now();
	t->ended = t->due = 0;
	t->who = who;
	atomic_init(&t->status, LEDGER_WORKING);
	atomic_init(&t->refs, 2);
	atomic_init(&t->asked, 0);
	atomic_init(&t->told, 0);
	t->tool = tool;
	t->kind = kind;
	for (size_t i = 0; i < BITS; ++i) {
		t->id[2 * i] = hex[bits[i] >> 4];
		t->id[2 * i + 1] = hex[bits[i] & 15];
	}
	t->hash = util_hash(0, t->id, LEDGER_ID);

	struct ledger_metrics *m = &ledger->metrics;
	pthread_mutex_lock(&ledger->lock);
	struct ledger_task *gone = ledger_sweep(ledger, nullptr, true);
	*e = ledger_insert(ledger, t, owner, &gone);
	pthread_mutex_unlock(&ledger->lock);
	ledger_put_all(gone);
	if (*e) {
		if (*e != ENOMEM)
			ledger_add(*e == EBUSY ? &m->refused : &m->full, 1);
		free(t);
		return nullptr;
	}
	ledger_add(kind == LEDGER_JOB ? &m->jobs : &m->opened, 1);
	ledger_add(&m->kept, 1);
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
	if (!ledger || !task || status == LEDGER_WORKING || status >= LEDGER_STATUSES ||
	    atomic_load_explicit(&task->status, memory_order_relaxed) != LEDGER_WORKING)
		return;
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
	--task->owner->running;
	ledger->bytes += task->len;
	struct ledger_task *gone = ledger_sweep(ledger, task, false);
	pthread_mutex_unlock(&ledger->lock);
	ledger_add(&ledger->metrics.ended[status], 1);
	ledger_add(&ledger->metrics.bytes, task->len);
	ledger_put_all(gone);
}

struct ledger_task *
ledger_find (struct ledger *ledger,
             uint64_t       owner,
             char const    *id,
             size_t         len)
{
	if (!ledger || !id || len != LEDGER_ID)
		return nullptr;
	uint64_t hash = util_hash(0, id, len);
	pthread_mutex_lock(&ledger->lock);
	struct ledger_task *gone = ledger_sweep(ledger, nullptr, false);
	struct ledger_task *t = ledger->buckets[hash & (BUCKETS - 1)];
	while (t && (t->hash != hash || memcmp(t->id, id, len) || t->owner->key != owner))
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
