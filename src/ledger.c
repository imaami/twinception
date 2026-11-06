/** @file ledger.c
 *
 * The ledger of tool calls that keep running after askai has answered the
 * requests that started them (src/ledger.h). One mutex protects the hash
 * table of the records by id, the hash table of the owners by key, and the
 * list of the records whose calls have ended.
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "clk.h"
#include "hist.h"
#include "ledger.h"
#include "list.h"
#include "loop.h"
#include "util.h"

enum {
	BUCKETS       = LEDGER_MAX,     /* the table's, a power of two */
	OWNER_BUCKETS = 256,            /* the owners' table's, likewise */
};

static_assert(!(BUCKETS & (BUCKETS - 1)), "a mask picks the bucket");
static_assert(!(OWNER_BUCKETS & (OWNER_BUCKETS - 1)), "a mask picks the bucket");
static_assert(LEDGER_MINE > 1, "ledger_insert() must not free the owner as it "
                               "removes a record of the owner");

/* The records, timers and waits of one owner. The ledger frees the owner
 * when it has none of them left.
 */
struct ledger_owner {
	struct list          records;   /* by their member mine, oldest first */
	struct list          waits;     /* by their member hook */
	struct ledger_owner *next;      /* in its bucket */
	uint64_t             key;
	uint64_t             n_waits;
	uint32_t             n;         /* records */
	uint32_t             running;   /* records whose calls run */
	struct ledger_timer  timers[LEDGER_TIMERS];  /* due 0: none */
};

struct ledger {
	pthread_mutex_t        lock;
	struct list            ended;     /* records whose calls ended, oldest
	                                     first */
	struct ledger_metrics  metrics;
	size_t                 bytes;     /* of the ended records' results */
	uint32_t               n;         /* records in the ledger */
	uint32_t               n_owners;
	uint32_t               n_anonymous;  /* owners with LEDGER_ANONYMOUS */
	uint32_t               n_timers;  /* the number of the last timer set */
	struct ledger_task    *buckets[BUCKETS];
	struct ledger_owner   *owners[OWNER_BUCKETS];
};

/* The link to the first owner of the bucket of @p key. */
static struct ledger_owner **
ledger_owner_bucket (struct ledger *l,
                     uint64_t       key)
{
	return &l->owners[key & (array_size(l->owners) - 1)];
}

/* The owner whose key is @p key, the lock held; nullptr if the ledger has
 * no such owner.
 */
static struct ledger_owner *
ledger_owner_find (struct ledger *l,
                   uint64_t       key)
{
	struct ledger_owner *o = *ledger_owner_bucket(l, key);
	while (o && o->key != key)
		o = o->next;
	return o;
}

/* Free @p o, with the lock held, if it contains no records, no waits and
 * no timers.
 */
static void
ledger_owner_gc (struct ledger       *l,
                 struct ledger_owner *o)
{
	if (o->n || o->n_waits || ledger_timer_any(o->timers))
		return;
	struct ledger_owner **p = ledger_owner_bucket(l, o->key);
	while (*p != o)
		p = &(*p)->next;
	*p = o->next;
	l->n_anonymous -= (o->key & LEDGER_ANONYMOUS) != 0;
	--l->n_owners;
	free(o);
}

/* Remove the timers of @p o that fired LEDGER_KEEP seconds or more before
 * @p now, the lock held.
 */
static void
ledger_owner_expire (struct ledger_owner *o,
                     uint64_t             now)
{
	for (size_t i = 0; i < array_size(o->timers); ++i) {
		uint64_t due = o->timers[i].due;
		if (due < now && now - due >= LEDGER_KEEP * NS)
			o->timers[i].due = 0;
	}
}

/* Remove the expired timers of every owner, and free the owners that then
 * have nothing, the lock held.
 */
static void
ledger_owners_expire (struct ledger *l)
{
	uint64_t now = loop_clock();
	for (size_t i = 0; i < array_size(l->owners); ++i)
		for (struct ledger_owner *o = l->owners[i], *next; o; o = next) {
			next = o->next;
			ledger_owner_expire(o, now);
			ledger_owner_gc(l, o);
		}
}

/* Whether the ledger has room for a new owner whose key is @p key, the lock
 * held: fewer than LEDGER_OWNERS owners, and for a key with
 * LEDGER_ANONYMOUS, fewer than LEDGER_OWNERS / 2 owners with it.
 */
static bool
ledger_owner_room (struct ledger const *l,
                   uint64_t             key)
{
	return l->n_owners < LEDGER_OWNERS &&
	       (!(key & LEDGER_ANONYMOUS) || l->n_anonymous < LEDGER_OWNERS / 2);
}

/* The owner whose key is @p key, the lock held, made if the ledger has no
 * such owner. If it cannot be made, returns nullptr with the error in
 * @p *e: ENOSPC while the ledger has no room for it, after the removal of
 * every owner's expired timers, or ENOMEM.
 */
static struct ledger_owner *
ledger_owner_get (struct ledger *l,
                  uint64_t       key,
                  int           *e)
{
	struct ledger_owner *o = ledger_owner_find(l, key);
	*e = 0;
	if (o)
		return o;
	if (!ledger_owner_room(l, key))
		ledger_owners_expire(l);
	if (!ledger_owner_room(l, key)) {
		*e = ENOSPC;
		return nullptr;
	}
	o = malloc(sizeof *o);
	if (!o) {
		*e = ENOMEM;
		return nullptr;
	}
	struct ledger_owner **bucket = ledger_owner_bucket(l, key);
	*o = (struct ledger_owner){ .next = *bucket, .key = key };
	list_init(&o->records);
	list_init(&o->waits);
	*bucket = o;
	++l->n_owners;
	l->n_anonymous += (key & LEDGER_ANONYMOUS) != 0;
	return o;
}

/* The timer of @p o whose number is @p id, the lock held; nullptr if none. */
static struct ledger_timer *
ledger_owner_timer (struct ledger_owner *o,
                    uint32_t             id)
{
	for (size_t i = 0; i < array_size(o->timers); ++i)
		if (o->timers[i].due && o->timers[i].id == id)
			return &o->timers[i];
	return nullptr;
}

/* Put each wait of @p o whose message is not posted on a chain by its
 * member hit, with a reference for the message, the lock held. Returns
 * the chain, for ledger_wake() once the lock is not held.
 */
static struct ledger_wait *
ledger_owner_wake (struct ledger_owner *o)
{
	struct ledger_wait *hits = nullptr;
	struct list *pos;
	list_foreach(pos, &o->waits) {
		struct ledger_wait *w = container_of(pos, struct ledger_wait, hook);
		/* Acquire, as ledger_wait_heard() releases: the loop has delivered
		 * the last post of the message, and the poster of it has read hit. */
		if (atomic_exchange_explicit(&w->posted, 1, memory_order_acquire))
			continue;
		atomic_fetch_add_explicit(&w->refs, 1, memory_order_relaxed);
		w->hit = hits;
		hits = w;
	}
	return hits;
}

/* Post the message of each wait on the chain at @p hits. */
static void
ledger_wake (struct ledger_wait *hits)
{
	/* The handler of a message can free its wait, and a later end can put
	 * the wait on another chain, so the function reads hit before it posts.
	 * A message that loop_post() refuses never runs, so the function drops
	 * the reference of that message itself. */
	while (hits) {
		struct ledger_wait *w = hits;
		hits = w->hit;
		if (loop_post(w->loop, &w->wake))
			ledger_wait_put(w);
	}
}

/* Put a copy of @p t into @p out, by number. */
static void
ledger_look_timer (struct ledger_look        *out,
                   struct ledger_timer const *t)
{
	size_t k = out->n_timers++;
	for (; k && out->timers[k - 1].id > t->id; --k)
		out->timers[k] = out->timers[k - 1];
	out->timers[k] = *t;
}

/* Put copies of the timers of @p o into @p out, and the time of the first
 * that has not fired by @p now, the lock held; with @p take, only those
 * that have fired, which @p o then has no more.
 */
static void
ledger_owner_timers (struct ledger_owner *o,
                     uint64_t             now,
                     bool                 take,
                     struct ledger_look  *out)
{
	enum { FIRED = 1, TAKE = 2 };
	for (size_t i = 0; i < array_size(o->timers); ++i) {
		struct ledger_timer *t = &o->timers[i];
		if (!t->due)
			continue;
		switch ((t->due <= now ? FIRED : 0) | (take ? TAKE : 0)) {
		case FIRED | TAKE:
			ledger_look_timer(out, t);
			t->due = 0;
			break;
		case 0:
			ledger_look_timer(out, t);
			[[fallthrough]];
		case TAKE:
			out->due = t->due < out->due ? t->due : out->due;
			break;
		default:
			ledger_look_timer(out, t);
		}
	}
}

/* Whether @p id is among the @p n ids at @p ids. */
static bool
ledger_listed (struct hexid const *ids,
               size_t              n,
               struct hexid const *id)
{
	for (size_t i = 0; i < n; ++i)
		if (hexid_equal(&ids[i], id))
			return true;
	return false;
}

/* Put references into @p out to the ended records of @p o, the oldest
 * first: those that no one has read, or, if @p n is not 0, those whose ids
 * are among the @p n at @p ids; mark them read. The lock held.
 */
static void
ledger_owner_tell (struct ledger_owner *o,
                   struct hexid const  *ids,
                   size_t               n,
                   struct ledger_look  *out)
{
	for (struct list *pos = o->records.next;
	     pos != &o->records && out->n_tasks < array_size(out->tasks); pos = pos->next) {
		struct ledger_task *t = container_of(pos, struct ledger_task, mine);
		if (atomic_load_explicit(&t->status, memory_order_relaxed) == LEDGER_WORKING ||
		    (n && !ledger_listed(ids, n, &t->id)))
			continue;
		if (atomic_exchange_explicit(&t->told, 1, memory_order_relaxed) && !n)
			continue;
		atomic_fetch_add_explicit(&t->refs, 1, memory_order_relaxed);
		out->tasks[out->n_tasks++] = t;
	}
}

/* Make @p out empty. */
static void
ledger_look_empty (struct ledger_look *out)
{
	out->due = UINT64_MAX;
	out->more = 0;
	out->n_tasks = 0;
	out->n_timers = 0;
}

/* The oldest record of @p o whose call has ended, the lock held; nullptr if
 * the calls of all its records run.
 */
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

/* The link to the record whose id is @p id and whose hash is @p hash, the
 * lock held, or the link at the end of its bucket if the ledger has none.
 */
static struct ledger_task **
ledger_slot (struct ledger      *l,
             uint64_t            hash,
             struct hexid const *id)
{
	struct ledger_task **p = &l->buckets[hash & (array_size(l->buckets) - 1)];
	while (*p && ((*p)->hash != hash || !hexid_equal(&(*p)->id, id)))
		p = &(*p)->next;
	return p;
}

/* The record of the owner @p owner whose id is @p id, the lock held;
 * nullptr if the ledger has none. Ids are unique across owners, as
 * ledger_restore() refuses one in the ledger.
 */
static struct ledger_task *
ledger_mine (struct ledger      *l,
             uint64_t            owner,
             struct hexid const *id)
{
	struct ledger_task *t = *ledger_slot(l, util_hash(0, id->hex, HEXID_DIGITS), id);
	return t && t->owner->key == owner ? t : nullptr;
}

/* Take @p t, an ended record, out of the table, off the ended list and off
 * its owner's list, the lock held, and free the owner if it has no records
 * left. Chains @p t at @p *gone, whose references the caller drops once
 * the lock is not held.
 */
static void
ledger_drop (struct ledger       *l,
             struct ledger_task  *t,
             struct ledger_task **gone)
{
	*ledger_slot(l, t->hash, &t->id) = t->next;
	t->next = *gone;
	*gone = t;
	list_del(&t->hook);
	list_del(&t->mine);
	struct ledger_owner *o = t->owner;
	t->owner = nullptr;
	--o->n;
	ledger_owner_gc(l, o);
	l->bytes -= t->size;
	--l->n;
	hist_count(&l->metrics.kept, -(uint64_t)1);
	hist_count(&l->metrics.bytes, -(uint64_t)t->size);
	hist_count(&l->metrics.gone, 1);
}

/* The owner with the most records whose calls have ended, the lock held;
 * nullptr if the calls of all records run.
 */
static struct ledger_owner *
ledger_owner_most (struct ledger const *l)
{
	struct ledger_owner *most = nullptr;
	uint32_t max = 0;
	for (size_t i = 0; i < array_size(l->owners); ++i)
		for (struct ledger_owner *o = l->owners[i]; o; o = o->next)
			if (o->n - o->running > max) {
				max = o->n - o->running;
				most = o;
			}
	return most;
}

/*
 * Let go of the ended records whose time is up, and of the oldest ended
 * while the results pass LEDGER_BYTES; never of @p keep. With @p room, if
 * the ledger is full after that, let go of the oldest ended record of the
 * owner with the most ended records, so that owners with many records do
 * not remove the records of owners with few. The lock held: returns those
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
		ledger_drop(l, t, &gone);
	}
	struct ledger_owner *o = room && l->n == LEDGER_MAX ? ledger_owner_most(l) : nullptr;
	/* The owner has an ended record, as o->n - o->running counts them. */
	if (o)
		ledger_drop(l, ledger_owner_ended(o), &gone);
	return gone;
}

/* Put @p t, a new record, into the ledger for the owner @p key, the lock
 * held. If the owner has LEDGER_MINE records or more, removes its oldest
 * ended record from the ledger and chains it to @p *gone. Returns 0,
 * EBUSY, ENOSPC or ENOMEM, as ledger_open() describes.
 */
static int
ledger_insert (struct ledger       *l,
               struct ledger_task  *t,
               uint64_t             key,
               struct ledger_task **gone)
{
	if (l->n == LEDGER_MAX)
		return ENOSPC;
	int e;
	struct ledger_owner *o = ledger_owner_get(l, key, &e);
	if (!o)
		return e;
	bool working = atomic_load_explicit(&t->status, memory_order_relaxed) == LEDGER_WORKING;
	if (working && t->kind == LEDGER_JOB && o->running >= LEDGER_RUNNING)
		return EBUSY;
	struct ledger_task *old = o->n >= LEDGER_MINE ? ledger_owner_ended(o) : nullptr;
	if (old)
		ledger_drop(l, old, gone);
	struct ledger_task **bucket = &l->buckets[t->hash & (array_size(l->buckets) - 1)];
	t->next = *bucket;
	*bucket = t;
	++l->n;
	t->owner = o;
	struct list *pos = o->records.prev;
	while (pos != &o->records &&
	       container_of(pos, struct ledger_task, mine)->created > t->created)
		pos = pos->prev;
	list_append(pos->next, &t->mine);
	++o->n;
	o->running += working;
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
	struct ledger *l = calloc(1, sizeof *l);
	*e = l ? pthread_mutex_init(&l->lock, nullptr) : ENOMEM;
	if (*e) {
		free(l);
		return nullptr;
	}
	list_init(&l->ended);
	struct ledger_metrics *m = &l->metrics;
	atomic_init(&m->opened, 0);
	atomic_init(&m->jobs, 0);
	for (size_t i = 0; i < array_size(m->ended); ++i)
		atomic_init(&m->ended[i], 0);
	atomic_init(&m->gone, 0);
	atomic_init(&m->full, 0);
	atomic_init(&m->refused, 0);
	atomic_init(&m->timers, 0);
	atomic_init(&m->kept, 0);
	atomic_init(&m->bytes, 0);
	atomic_init(&m->waits, 0);
	return l;
}

void
ledger_destroy (struct ledger **p_ledger)
{
	struct ledger *l = p_ledger ? *p_ledger : nullptr;
	if (!l)
		return;
	*p_ledger = nullptr;
	for (size_t i = 0; i < array_size(l->buckets); ++i)
		for (struct ledger_task *t = l->buckets[i], *next; t; t = next) {
			next = t->next;
			t->owner = nullptr;
			ledger_task_put(t);
		}
	for (size_t i = 0; i < array_size(l->owners); ++i)
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
	if (!e)
		return nullptr;
	struct hexid id;
	if ((*e = hexid_make(&id)))
		return nullptr;
	return ledger_restore(ledger, owner, kind, tool, &id, clk_realtime_ns(), 0,
	                      LEDGER_WORKING, (struct span){}, loop, who, fn, e);
}

struct ledger_task *
ledger_restore (struct ledger      *ledger,
                uint64_t            owner,
                enum ledger_kind    kind,
                uint16_t            tool,
                struct hexid const *id,
                uint64_t            created,
                uint64_t            ended,
                enum ledger_status  status,
                struct span         result,
                struct loop        *loop,
                struct handle       who,
                void              (*fn) (struct loop *loop,
                                         struct msg  *msg),
                int                *e)
{
	if (!e)
		return nullptr;
	*e = EINVAL;
	bool working = status == LEDGER_WORKING;
	if (!ledger || !id || !*id->hex ||
	    (kind != LEDGER_TASK && kind != LEDGER_JOB) ||
	    status < LEDGER_WORKING || status >= LEDGER_STATUSES ||
	    (result.len && !result.ptr) ||
	    (working && (!loop || !fn || result.len || ended)) ||
	    (status == LEDGER_CANCELLED && result.len))
		return nullptr;
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
	t->result = (struct dstr)DSTR_INIT;
	t->size = result.len;
	if (result.len && !dstr_cat_n(&t->result, (char const *)result.ptr, result.len)) {
		free(t);
		t = nullptr;
		*e = ENOMEM;
		return nullptr;
	}
	t->created = created;
	t->ended = working ? 0 : ended ? ended : clk_realtime_ns();
	t->due = working ? 0 : loop_clock() + LEDGER_KEEP * NS;
	t->who = who;
	atomic_init(&t->status, status);
	atomic_init(&t->refs, 2);
	atomic_init(&t->asked, 0);
	atomic_init(&t->told, 0);
	t->tool = tool;
	t->kind = kind;
	t->id = *id;
	t->hash = util_hash(0, t->id.hex, HEXID_DIGITS);

	struct ledger_metrics *m = &ledger->metrics;
	struct ledger_task *gone = nullptr;
	struct ledger_task *excess = nullptr;
	struct ledger_wait *hits = nullptr;
	pthread_mutex_lock(&ledger->lock);
	*e = EEXIST;
	if (!*ledger_slot(ledger, t->hash, &t->id)) {
		gone = ledger_sweep(ledger, nullptr, true);
		*e = ledger_insert(ledger, t, owner, &gone);
	}
	if (!*e) {
		hist_count(kind == LEDGER_JOB ? &m->jobs : &m->opened, 1);
		hist_count(&m->kept, 1);
		if (!working) {
			list_append(&ledger->ended, &t->hook);
			ledger->bytes += t->size;
			hist_count(&m->ended[status], 1);
			hist_count(&m->bytes, t->size);
			hits = ledger_owner_wake(t->owner);
			excess = ledger_sweep(ledger, t, false);
		}
	}
	pthread_mutex_unlock(&ledger->lock);
	ledger_wake(hits);
	ledger_put_all(gone);
	ledger_put_all(excess);
	if (*e) {
		if (*e == EBUSY || *e == ENOSPC)
			hist_count(*e == EBUSY ? &m->refused : &m->full, 1);
		dstr_cleanup(&t->result);
		free(t);
		t = nullptr;
		return nullptr;
	}
	return t;
}

int
ledger_task_reserve (struct ledger_task *task,
                     size_t              len)
{
	dstr_clear(&task->result);
	task->size = dstr_reserve(&task->result, len) ? len : 0;
	return task->size == len ? 0 : ENOMEM;
}

int
ledger_task_write (struct ledger_task *task,
                   struct span         bytes)
{
	if (bytes.len > task->size - dstr_len(&task->result))
		return EPIPE;
	/* ledger_task_reserve() made the room, so the append does not
	 * allocate. */
	(void)dstr_cat_n(&task->result, (char const *)bytes.ptr, bytes.len);
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
	/* Only a whole reply is kept, so task->size is the bytes of the kept
	 * reply. */
	if (dstr_len(&task->result) != task->size || status == LEDGER_CANCELLED) {
		dstr_cleanup(&task->result);
		task->size = 0;
	}
	task->ended = clk_realtime_ns();
	task->due = loop_clock() + LEDGER_KEEP * NS;
	pthread_mutex_lock(&ledger->lock);
	atomic_store_explicit(&task->status, status, memory_order_release);
	list_append(&ledger->ended, &task->hook);
	--task->owner->running;
	struct ledger_wait *hits = ledger_owner_wake(task->owner);
	ledger->bytes += task->size;
	struct ledger_task *gone = ledger_sweep(ledger, task, false);
	pthread_mutex_unlock(&ledger->lock);
	hist_count(&ledger->metrics.ended[status], 1);
	hist_count(&ledger->metrics.bytes, task->size);
	ledger_wake(hits);
	ledger_put_all(gone);
}

struct ledger_task *
ledger_find (struct ledger      *ledger,
             uint64_t            owner,
             struct hexid const *id)
{
	if (!ledger || !id)
		return nullptr;
	pthread_mutex_lock(&ledger->lock);
	struct ledger_task *gone = ledger_sweep(ledger, nullptr, false);
	struct ledger_task *t = ledger_mine(ledger, owner, id);
	if (t)
		atomic_fetch_add_explicit(&t->refs, 1, memory_order_relaxed);
	pthread_mutex_unlock(&ledger->lock);
	ledger_put_all(gone);
	return t;
}

int
ledger_remove (struct ledger      *ledger,
               uint64_t            owner,
               struct hexid const *id)
{
	if (!ledger || !id || !*id->hex)
		return EINVAL;
	struct ledger_task *gone = nullptr;
	pthread_mutex_lock(&ledger->lock);
	struct ledger_task *t = ledger_mine(ledger, owner, id);
	int e = !t ? ENOENT :
	        atomic_load_explicit(&t->status, memory_order_relaxed) == LEDGER_WORKING ? EBUSY : 0;
	if (!e)
		ledger_drop(ledger, t, &gone);
	pthread_mutex_unlock(&ledger->lock);
	ledger_put_all(gone);
	return e;
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
	dstr_cleanup(&task->result);
	free(task);
}

int
ledger_timer_add (struct ledger_timer timers[static LEDGER_TIMERS],
                  uint64_t            due,
                  uint32_t            id,
                  struct cstr_view    note)
{
	if (!due || note.len > LEDGER_NOTE || (note.len && !note.data))
		return EINVAL;
	size_t i = 0;
	while (i < LEDGER_TIMERS && timers[i].due)
		++i;
	if (i == LEDGER_TIMERS)
		return EBUSY;
	timers[i] = (struct ledger_timer){ .due = due, .id = id, .len = (uint32_t)note.len };
	if (note.len)
		memcpy(timers[i].note, note.data, note.len);
	return 0;
}

bool
ledger_timer_any (struct ledger_timer const timers[static LEDGER_TIMERS])
{
	for (size_t i = 0; i < LEDGER_TIMERS; ++i)
		if (timers[i].due)
			return true;
	return false;
}

int
ledger_timer_set (struct ledger    *ledger,
                  uint64_t          owner,
                  uint64_t          due,
                  struct cstr_view  note,
                  uint32_t         *id)
{
	if (!ledger || !due || (note.len && !note.data) || note.len > LEDGER_NOTE || !id)
		return EINVAL;
	uint64_t now = loop_clock();
	struct ledger_wait *hits = nullptr;
	int e;
	pthread_mutex_lock(&ledger->lock);
	struct ledger_owner *o = ledger_owner_get(ledger, owner, &e);
	if (o) {
		ledger_owner_expire(o, now);
		/* One counter numbers the timers of all owners, from 1 to
		 * UINT32_MAX and then from 1 again, so that one number never
		 * refers to a second timer of an owner that the ledger freed
		 * and created again. */
		uint32_t const next = ledger->n_timers % UINT32_MAX + 1;
		e = ledger_timer_add(o->timers, due, next, note);
		if (!e) {
			ledger->n_timers = next;
			*id = next;
			hits = ledger_owner_wake(o);
			hist_count(&ledger->metrics.timers, 1);
		}
	}
	pthread_mutex_unlock(&ledger->lock);
	ledger_wake(hits);
	return e;
}

int
ledger_timer_cancel (struct ledger *ledger,
                     uint64_t       owner,
                     uint32_t       id)
{
	if (!ledger)
		return EINVAL;
	uint64_t now = loop_clock();
	int e = ENOENT;
	pthread_mutex_lock(&ledger->lock);
	struct ledger_owner *o = ledger_owner_find(ledger, owner);
	if (o) {
		ledger_owner_expire(o, now);
		struct ledger_timer *t = ledger_owner_timer(o, id);
		e = !t ? ENOENT : t->due <= now ? EALREADY : 0;
		if (!e)
			t->due = 0;
		ledger_owner_gc(ledger, o);
	}
	pthread_mutex_unlock(&ledger->lock);
	return e;
}

struct ledger_wait *
ledger_wait_enter (struct ledger  *ledger,
                   uint64_t        owner,
                   struct loop    *loop,
                   struct handle   who,
                   void          (*fn) (struct loop *loop,
                                        struct msg  *msg),
                   int            *e)
{
	if (!e)
		return nullptr;
	if (!ledger || !loop || !fn) {
		*e = EINVAL;
		return nullptr;
	}
	struct ledger_wait *w = malloc(sizeof *w);
	if (!w) {
		*e = ENOMEM;
		return nullptr;
	}
	w->wake = (struct msg){ .fn = fn };
	list_init(&w->hook);
	w->hit = nullptr;
	w->owner = nullptr;
	w->loop = loop;
	w->who = who;
	atomic_init(&w->refs, 1);
	atomic_init(&w->posted, 0);
	pthread_mutex_lock(&ledger->lock);
	struct ledger_owner *o = ledger_owner_get(ledger, owner, e);
	if (o && o->n_waits >= LEDGER_WAITS)
		*e = EBUSY;
	if (!*e) {
		list_append(&o->waits, &w->hook);
		++o->n_waits;
		w->owner = o;
	}
	pthread_mutex_unlock(&ledger->lock);
	if (*e) {
		free(w);
		return nullptr;
	}
	hist_count(&ledger->metrics.waits, 1);
	return w;
}

void
ledger_wait_heard (struct ledger_wait *wait)
{
	if (wait)
		atomic_store_explicit(&wait->posted, 0, memory_order_release);
}

void
ledger_wait_leave (struct ledger       *ledger,
                   struct ledger_wait **p_wait)
{
	struct ledger_wait *w = p_wait ? *p_wait : nullptr;
	if (!ledger || !w)
		return;
	*p_wait = nullptr;
	pthread_mutex_lock(&ledger->lock);
	struct ledger_owner *o = w->owner;
	if (o) {
		list_del(&w->hook);
		--o->n_waits;
		w->owner = nullptr;
		ledger_owner_gc(ledger, o);
	}
	pthread_mutex_unlock(&ledger->lock);
	if (o)
		hist_count(&ledger->metrics.waits, -(uint64_t)1);
	ledger_wait_put(w);
}

void
ledger_wait_put (struct ledger_wait *wait)
{
	if (wait && atomic_fetch_sub_explicit(&wait->refs, 1, memory_order_acq_rel) == 1)
		free(wait);
}

int
ledger_list (struct ledger      *ledger,
             uint64_t            owner,
             struct ledger_look *out)
{
	if (!out)
		return EINVAL;
	ledger_look_empty(out);
	if (!ledger)
		return EINVAL;
	uint64_t now = loop_clock();
	pthread_mutex_lock(&ledger->lock);
	struct ledger_task *gone = ledger_sweep(ledger, nullptr, false);
	struct ledger_owner *o = ledger_owner_find(ledger, owner);
	if (o) {
		ledger_owner_expire(o, now);
		ledger_owner_timers(o, now, false, out);
		for (struct list *pos = o->records.prev;
		     pos != &o->records && out->n_tasks < array_size(out->tasks);
		     pos = pos->prev) {
			struct ledger_task *t = container_of(pos, struct ledger_task, mine);
			atomic_fetch_add_explicit(&t->refs, 1, memory_order_relaxed);
			out->tasks[out->n_tasks++] = t;
		}
		out->more = o->n - out->n_tasks;
		ledger_owner_gc(ledger, o);
	}
	pthread_mutex_unlock(&ledger->lock);
	ledger_put_all(gone);
	return 0;
}

int
ledger_tell (struct ledger      *ledger,
             struct ledger_wait *wait,
             struct hexid const *ids,
             size_t              n,
             struct ledger_look *out)
{
	if (!out)
		return EINVAL;
	ledger_look_empty(out);
	if (!ledger || !wait || (n && !ids))
		return EINVAL;
	uint64_t now = loop_clock();
	pthread_mutex_lock(&ledger->lock);
	struct ledger_owner *o = wait->owner;
	if (o) {
		ledger_owner_timers(o, now, true, out);
		ledger_owner_tell(o, ids, n, out);
	}
	pthread_mutex_unlock(&ledger->lock);
	return o ? 0 : EINVAL;
}

void
ledger_look_put (struct ledger_look *look)
{
	if (!look)
		return;
	for (uint32_t i = 0; i < look->n_tasks; ++i)
		ledger_task_put(look->tasks[i]);
	look->n_tasks = 0;
}
