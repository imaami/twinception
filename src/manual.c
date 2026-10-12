/** @file The manual (manual.h). */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "buf.h"
#include "child.h"
#include "jb.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "manual.h"
#include "util.h"

enum {
	FOUND_MAX = 4 << 10,  /* bytes of man -w's answer: a path, a line break */
	ALIASES   = 8,        /* names a file is kept by, at most */
	BUCKETS   = 1 << 12,  /* the table's, to begin with */
};

static constexpr uint64_t NS = 1'000'000'000;

/* What ends a page cut short. */
static char const cut[] = "\n\n[The page goes on past the 2 MiB shown.]\n";
static_assert(MANUAL_MAX == 2 << 20, "the note says 2 MiB");

static char const busy[] = "Too many pages are being rendered; try again in a "
                           "few seconds.";

struct manual_fetch;

/*
 * An entry of the manual's table: a page's file, keyed by its path, or an
 * alias, a name and section a page is asked by, keyed by "NAME\0SECTION",
 * which no path holds.  Files stay until the manual is destroyed; an alias
 * goes as man fails for it.  The manual's lock guards all but the key.
 */
struct manual_entry {
	struct manual_entry *next;                   /* in its bucket */
	uint64_t             hash;
	union {
		struct {                                     /* a file's */
			struct manual_page  *page;     /* the manual's reference */
			uint32_t             aliases;  /* that name the file */
		};
		struct {                                     /* an alias's */
			struct manual_entry *file;     /* what man found for it */
			struct manual_fetch *fetch;    /* finding it, while under way */
			uint64_t             found;    /* loop_clock() as man found it */
		};
	};
	uint32_t             len;                    /* of the key, less its '\0' */
	char                 key[];
};

/* A fetch of an alias's page, on the loop of the ask that began it: man
 * finds the file, then renders it, unless the manual has the page as the
 * file is. */
struct manual_fetch {
	struct list          hook;     /* on its loop's fetches */
	struct loop_step     step;     /* copying the page in */
	struct stat          st;       /* the file's, as man began to render it */
	struct manual_loop  *ml;
	struct manual_entry *alias;
	struct manual_entry *file;     /* once man has found it */
	struct manual_wait  *waiters;  /* under the manual's lock */
	struct child        *child;
	struct manual_page  *page;     /* being copied in */
	size_t               off;      /* of the output, copied */
};

struct manual {
	pthread_mutex_t       lock;     /* the table's */
	struct manual_metrics metrics;
	struct manual_entry **buckets;
	char                 *env[6];   /* man's environment */
	size_t                mask;     /* the buckets, less 1 */
	size_t                entries;
	uint64_t              seed;     /* of the keys' hash */
	uint64_t              recheck;  /* ns a finding holds */
};

/* Add @p n to @p p, which any thread may write. */
static void
manual_count (_Atomic uint64_t *p,
              uint64_t          n)
{
	atomic_fetch_add_explicit(p, n, memory_order_relaxed);
}

static struct manual_page *
manual_page_get (struct manual_page *page)
{
	atomic_fetch_add_explicit(&page->refs, 1, memory_order_relaxed);
	return page;
}

void
manual_page_put (struct manual_page *page)
{
	if (page && atomic_fetch_sub_explicit(&page->refs, 1, memory_order_acq_rel) == 1)
		free(page);
}

void
manual_wait_destroy (struct manual_wait **p_w)
{
	struct manual_wait *w = *p_w;
	if (!w)
		return;
	*p_w = nullptr;
	manual_page_put(w->page);
	free(w);
}

/* Whether @p page was rendered from its file as @p st finds it. */
static bool
manual_page_same (struct manual_page const *page,
                  struct stat const        *st)
{
	return page->dev == st->st_dev && page->ino == st->st_ino &&
	       page->size == st->st_size && page->ctime.tv_sec == st->st_ctim.tv_sec &&
	       page->ctime.tv_nsec == st->st_ctim.tv_nsec;
}

/* The entry keyed by @p key, of @p len bytes and @p hash; nullptr if none. */
static struct manual_entry *
manual_lookup (struct manual const *m,
               char const          *key,
               size_t               len,
               uint64_t             hash)
{
	struct manual_entry *e = m->buckets[hash & m->mask];
	while (e && (e->hash != hash || e->len != len || memcmp(e->key, key, len)))
		e = e->next;
	return e;
}

/* Double the buckets, if memory allows: longer chains are all it costs if
 * not. */
static void
manual_grow (struct manual *m)
{
	size_t n = 2 * (m->mask + 1);
	struct manual_entry **buckets = calloc(n, sizeof *buckets);
	if (!buckets)
		return;
	for (size_t i = 0; i <= m->mask; ++i) {
		for (struct manual_entry *e = m->buckets[i]; e; ) {
			struct manual_entry *next = e->next;
			e->next = buckets[e->hash & (n - 1)];
			buckets[e->hash & (n - 1)] = e;
			e = next;
		}
	}
	free(m->buckets);
	m->buckets = buckets;
	m->mask = n - 1;
}

/* A new entry keyed by @p key, in the table, the buckets doubling once the
 * entries outnumber them; nullptr if out of memory. */
static struct manual_entry *
manual_add (struct manual *m,
            char const    *key,
            size_t         len,
            uint64_t       hash)
{
	struct manual_entry *e = malloc(sizeof *e + len + 1);
	if (!e)
		return nullptr;
	*e = (struct manual_entry){ .hash = hash, .len = (uint32_t)len };
	memcpy(e->key, key, len);
	e->key[len] = '\0';
	if (++m->entries > m->mask + 1)
		manual_grow(m);
	struct manual_entry **bucket = &m->buckets[hash & m->mask];
	e->next = *bucket;
	*bucket = e;
	return e;
}

/* Take alias @p a out of the table, and free it. */
static void
manual_drop (struct manual       *m,
             struct manual_entry *a)
{
	struct manual_entry **p = &m->buckets[a->hash & m->mask];
	while (*p != a)
		p = &(*p)->next;
	*p = a->next;
	--m->entries;
	if (a->file)
		--a->file->aliases;
	free(a);
}

/* Free @p f, its waiters posted, and the page it holds, if any. */
static void
manual_fetch_free (struct manual_fetch *f)
{
	manual_page_put(f->page);
	child_destroy(&f->child);
	loop_unready(&f->step);
	list_del(&f->hook);
	free(f);
}

/* The fetch failed, as @p why says: the alias goes, and each waiter is
 * posted why. */
static void
manual_fetch_fail (struct manual_fetch *f,
                   char const          *why)
{
	struct manual *m = f->ml->manual;
	pthread_mutex_lock(&m->lock);
	f->alias->fetch = nullptr;
	manual_drop(m, f->alias);
	struct manual_wait *w = f->waiters;
	pthread_mutex_unlock(&m->lock);
	for (struct manual_wait *next; w; w = next) {
		next = w->next;
		util_format(w->why, sizeof w->why, "%s", why);
		loop_post(w->loop, &w->msg);
	}
	manual_fetch_free(f);
}

/*
 * The fetch is over with @p page, whose reference it takes: one it
 * rendered, which is f->page, the manual keeps by its file.  The alias
 * names the file, if the file has room for one more name, and else goes.
 * Each waiter is posted the page.
 */
static void
manual_fetch_done (struct manual_fetch *f,
                   struct manual_page  *page)
{
	struct manual_loop *ml = f->ml;
	struct manual *m = ml->manual;
	struct manual_entry *a = f->alias;
	struct manual_entry *file = f->file;
	struct manual_page *old = nullptr;
	bool rendered = page == f->page;
	pthread_mutex_lock(&m->lock);
	if (rendered) {
		old = file->page;
		file->page = manual_page_get(page);
		manual_count(&m->metrics.pages, !old);
		manual_count(&m->metrics.bytes, page->len - (old ? old->len : 0));
	}
	if (a->file != file) {
		if (a->file)
			--a->file->aliases;
		a->file = file->aliases < ALIASES ? file : nullptr;
		file->aliases += a->file == file;
	}
	a->found = ml->loop->now;
	a->fetch = nullptr;
	if (!a->file)
		manual_drop(m, a);
	struct manual_wait *w = f->waiters;
	for (struct manual_wait *x = w; x; x = x->next)
		x->page = manual_page_get(page);
	pthread_mutex_unlock(&m->lock);

	if (rendered)
		LOG(LOG_VERBOSE, old ? "man page %s rendered anew: %zu bytes"
		                     : "man page %s rendered: %zu bytes", file->key, page->len);
	manual_page_put(old);
	for (struct manual_wait *next; w; w = next) {
		next = w->next;
		loop_post(w->loop, &w->msg);
	}
	/* The fetch's page is this one, or none. */
	if (!rendered)
		manual_page_put(page);
	manual_fetch_free(f);
}

/* Run man with @p argv, its output up to @p max bytes, then @p done; else
 * fail the fetch with why not.  Counts the run in @p runs. */
static void
manual_fetch_run (struct manual_fetch *f,
                  char const          *argv[],
                  size_t               max,
                  void               (*done) (struct child *c),
                  _Atomic uint64_t    *runs)
{
	struct manual_loop *ml = f->ml;
	struct loop *loop = ml->loop;
	struct child_cmd const cmd = {
		.file = "man",
		.argv = argv,
		.envp = ml->manual->env,
		.max = max,
		.deadline = loop->now + MANUAL_SECONDS * NS
	};
	int e = 0;
	f->child = child_create(loop, &ml->children, &cmd, done, f, &e);
	if (f->child) {
		manual_count(runs, 1);
		return;
	}
	if (e == EBUSY) {
		manual_fetch_fail(f, busy);
		return;
	}
	char why[MANUAL_WHY];
	util_format(why, sizeof why, "Cannot run man: %s.", strerror(e));
	LOG(LOG_WARN, "%s", why);
	manual_fetch_fail(f, why);
}

/* Whether man, @p c, ended well, or its output was cut short; else @p why
 * says what went wrong. */
static bool
manual_fetch_ended (struct manual_fetch const *f,
                    struct child const        *c,
                    char                      *why)
{
	if (c->cut)
		return true;
	char const *name = f->alias->key;
	char const *section = name + strlen(name) + 1;
	if (c->late) {
		util_format(why, MANUAL_WHY, "man took longer than %d s.", MANUAL_SECONDS);
		return false;
	}
	switch (c->code) {
	case CLD_EXITED:
		if (!c->status)
			return true;
		if (c->status != 16)
			util_format(why, MANUAL_WHY, "man failed, with exit status %d.", c->status);
		else if (*section)
			util_format(why, MANUAL_WHY, "No manual entry for %s in section %s.", name,
			            section);
		else
			util_format(why, MANUAL_WHY, "No manual entry for %s.", name);
		return false;
	case 0:
		util_format(why, MANUAL_WHY, "man ended, and why is unknown.");
		return false;
	default:
		util_format(why, MANUAL_WHY, "man was killed by signal %d.", c->status);
		return false;
	}
}

/* Copy as much of the page in as the step has the share for, made UTF-8
 * text, the rest in bulk steps; then the fetch is over. */
static void
manual_fetch_copy (struct loop      *loop,
                   struct loop_step *step)
{
	struct manual_fetch *f = container_of(step, struct manual_fetch, step);
	struct span out = buf_peek(&f->child->out);
	size_t left = out.len - f->off;
	size_t copy = loop_room(loop, LOOP_COPY);
	size_t mend = loop_room(loop, LOOP_ESCAPE);
	size_t n = left < copy ? left : copy;
	n = n < mend ? n : mend;
	if (n) {
		char *p = (char *)out.ptr + f->off;
		n = jb_mend(p, n, n == left);
		memcpy(f->page->text + f->off, p, n);
		loop_work(loop, LOOP_COPY, n);
		loop_work(loop, LOOP_ESCAPE, n);
		f->off += n;
	}
	if (f->off < out.len)
		loop_ready_bulk(loop, step);
	else
		manual_fetch_done(f, f->page);
}

/* Man has rendered the page, or failed to: copy it in. */
static void
manual_fetch_rendered (struct child *c)
{
	struct manual_fetch *f = c->owner;
	char why[MANUAL_WHY];
	if (!manual_fetch_ended(f, c, why)) {
		manual_fetch_fail(f, why);
		return;
	}
	size_t len = buf_peek(&c->out).len;
	size_t note = c->cut ? sizeof cut - 1 : 0;
	struct manual_page *page = malloc(sizeof *page + len + note);
	if (!page) {
		manual_fetch_fail(f, "Out of memory.");
		return;
	}
	page->ctime = f->st.st_ctim;
	page->dev = f->st.st_dev;
	page->ino = f->st.st_ino;
	page->size = f->st.st_size;
	page->len = len + note;
	atomic_init(&page->refs, 1);
	memcpy(page->text + len, cut, note);
	f->page = page;
	manual_fetch_copy(f->ml->loop, &f->step);
}

/* Man has found the alias's file, or failed to: give the page the manual
 * has, if the file is as it was, else render it. */
static void
manual_fetch_found (struct child *c)
{
	struct manual_fetch *f = c->owner;
	struct manual *m = f->ml->manual;
	char why[MANUAL_WHY];
	if (!manual_fetch_ended(f, c, why)) {
		manual_fetch_fail(f, why);
		return;
	}
	struct span out = buf_peek(&c->out);
	unsigned char const *end = out.len ? memchr(out.ptr, '\n', out.len) : nullptr;
	size_t len = end ? (size_t)(end - out.ptr) : 0;
	if (!len || *out.ptr != '/' || memchr(out.ptr, '\0', len)) {
		manual_fetch_fail(f, "man named no file for the page.");
		return;
	}
	uint64_t hash = util_hash(m->seed, out.ptr, len);
	pthread_mutex_lock(&m->lock);
	struct manual_entry *file = manual_lookup(m, (char const *)out.ptr, len, hash);
	if (!file)
		file = manual_add(m, (char const *)out.ptr, len, hash);
	struct manual_page *page = file && file->page ? manual_page_get(file->page)
	                                              : nullptr;
	pthread_mutex_unlock(&m->lock);
	child_destroy(&f->child);
	f->file = file;
	if (!file) {
		manual_fetch_fail(f, "Out of memory.");
		return;
	}
	if (stat(file->key, &f->st)) {
		util_format(why, sizeof why, "Cannot read the page's file: %s.", strerror(errno));
		manual_page_put(page);
		manual_fetch_fail(f, why);
		return;
	}
	if (page && manual_page_same(page, &f->st)) {
		manual_fetch_done(f, page);
		return;
	}
	manual_page_put(page);
	char const *argv[] = { "man", "-l", "--", file->key, nullptr };
	manual_fetch_run(f, argv, MANUAL_MAX, manual_fetch_rendered, &m->metrics.renders);
}

/* Have @p w, if not nullptr, wait for the page of the alias @p key: join
 * the fetch under way, or begin one; without a waiter, begin one only if
 * none is under way.  Returns 0 or ENOMEM. */
static int
manual_loop_ask (struct manual_loop *ml,
                 char const         *key,
                 size_t              len,
                 uint64_t            hash,
                 struct manual_wait *w)
{
	struct manual *m = ml->manual;
	pthread_mutex_lock(&m->lock);
	struct manual_entry *a = manual_lookup(m, key, len, hash);
	if (a && a->fetch) {
		if (w) {
			w->next = a->fetch->waiters;
			a->fetch->waiters = w;
		}
		pthread_mutex_unlock(&m->lock);
		return 0;
	}
	struct manual_fetch *f = malloc(sizeof *f);
	if (f && !a)
		a = manual_add(m, key, len, hash);
	if (!f || !a) {
		pthread_mutex_unlock(&m->lock);
		free(f);
		return ENOMEM;
	}
	*f = (struct manual_fetch){
		.step.fn = manual_fetch_copy,
		.ml = ml,
		.alias = a,
		.waiters = w
	};
	a->fetch = f;
	pthread_mutex_unlock(&m->lock);
	list_append(&ml->fetches, &f->hook);

	/* Man finds the file. */
	char const *name = a->key;
	char const *section = name + strlen(name) + 1;
	char const *argv[7] = { "man", "-w" };
	size_t i = 2;
	if (*section) {
		argv[i++] = "-s";
		argv[i++] = section;
	}
	argv[i++] = "--";
	argv[i] = name;
	manual_fetch_run(f, argv, FOUND_MAX, manual_fetch_found, &m->metrics.finds);
	return 0;
}
int
manual_find (struct manual_loop  *ml,
             char const          *name,
             size_t               name_len,
             char const          *section,
             size_t               section_len,
             struct handle        who,
             void               (*fn) (struct loop *loop,
                                       struct msg  *msg),
             struct manual_page **p_page)
{
	struct manual *m = ml->manual;
	*p_page = nullptr;
	if (name_len > MANUAL_NAME || section_len > MANUAL_SECTION)
		return EINVAL;
	char key[MANUAL_NAME + 1 + MANUAL_SECTION];
	size_t len = name_len + 1 + section_len;
	memcpy(key, name, name_len);
	key[name_len] = '\0';
	if (section_len)
		memcpy(key + name_len + 1, section, section_len);
	uint64_t hash = util_hash(m->seed, key, len);

	pthread_mutex_lock(&m->lock);
	struct manual_entry *a = manual_lookup(m, key, len, hash);
	struct manual_entry *file = a ? a->file : nullptr;
	struct manual_page *page = file && file->page ? manual_page_get(file->page)
	                                              : nullptr;
	bool recheck = page && !a->fetch && a->found + m->recheck < ml->loop->now;
	pthread_mutex_unlock(&m->lock);
	struct stat st;
	if (page && !stat(file->key, &st) && manual_page_same(page, &st)) {
		manual_count(&m->metrics.hits, 1);
		*p_page = page;
		/* Man may find another file for it by now: the page goes out
		 * meanwhile. */
		if (recheck)
			(void)manual_loop_ask(ml, key, len, hash, nullptr);
		return 0;
	}
	manual_page_put(page);

	struct manual_wait *w = malloc(sizeof *w);
	if (!w)
		return ENOMEM;
	*w = (struct manual_wait){ .msg.fn = fn, .loop = ml->loop, .who = who };
	int e = manual_loop_ask(ml, key, len, hash, w);
	if (e)
		free(w);
	else
		manual_count(&m->metrics.waits, 1);
	return e;
}

void
manual_loop_fini (struct manual_loop *ml)
{
	while (!list_is_empty(&ml->fetches))
		manual_fetch_fail(container_of(ml->fetches.next, struct manual_fetch, hook),
		                  "Server is shutting down.");
	child_destroy_all(&ml->children);
}

/* The entry "@p name=value" of askai's environment, where @p name is @p len
 * bytes and ends in '='; nullptr if it has none. */
static char *
manual_env (char const *name,
            size_t      len)
{
	extern char **environ;
	for (char **e = environ; e && *e; ++e)
		if (!strncmp(*e, name, len))
			return *e;
	return nullptr;
}

struct manual *
manual_create (uint32_t  recheck,
               int      *e)
{
	struct manual *m = malloc(sizeof *m);
	struct manual_entry **buckets = calloc(BUCKETS, sizeof *buckets);
	*e = !m || !buckets ? ENOMEM : pthread_mutex_init(&m->lock, nullptr);
	if (*e) {
		free(buckets);
		free(m);
		return nullptr;
	}
	/* man's environment is askai's PATH and MANPATH, and UTF-8 text, 80
	 * columns wide, neither hyphenated nor justified, for a model to read. */
	static char const path[] = "PATH=";
	static char const manpath[] = "MANPATH=";
	char *env[ARRAY_SIZE(m->env)] = {
		manual_env(path, sizeof path - 1),
		"LC_ALL=C.UTF-8",
		"MANWIDTH=80",
		"MANROFFOPT=-rHY=0 -dAD=l",
		manual_env(manpath, sizeof manpath - 1)
	};
	if (!*env)
		*env = "PATH=/usr/bin:/bin";
	memcpy(m->env, env, sizeof env);
	m->buckets = buckets;
	m->mask = BUCKETS - 1;
	m->entries = 0;
	/* Without random bytes, the clock seeds the hash. */
	m->seed = loop_clock();
	(void)getrandom(&m->seed, sizeof m->seed, GRND_NONBLOCK);
	m->recheck = recheck * NS;
	struct manual_metrics *mm = &m->metrics;
	atomic_init(&mm->pages, 0);
	atomic_init(&mm->bytes, 0);
	atomic_init(&mm->hits, 0);
	atomic_init(&mm->waits, 0);
	atomic_init(&mm->finds, 0);
	atomic_init(&mm->renders, 0);
	return m;
}

void
manual_destroy (struct manual **p_manual)
{
	struct manual *m = *p_manual;
	if (!m)
		return;
	*p_manual = nullptr;
	for (size_t i = 0; i <= m->mask; ++i) {
		for (struct manual_entry *e = m->buckets[i]; e; ) {
			struct manual_entry *next = e->next;
			/* A file's key, a path, holds no '\0'. */
			if (!memchr(e->key, '\0', e->len))
				manual_page_put(e->page);
			free(e);
			e = next;
		}
	}
	free(m->buckets);
	pthread_mutex_destroy(&m->lock);
	free(m);
}

struct manual_metrics const *
manual_metrics (struct manual const *manual)
{
	return &manual->metrics;
}
