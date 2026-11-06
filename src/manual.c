/** @file manual.c
 *
 * Man pages tool.
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "buf.h"
#include "child.h"
#include "cstr_view.h"
#include "hist.h"
#include "htab.h"
#include "jb.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "manual.h"
#include "mcp.h"
#include "plugin.h"
#include "ref.h"
#include "schema.h"
#include "util.h"

enum {
	MANUAL_SECONDS = 10,       /**< for man to find, or render, a page */
	MANUAL_NAME    = 128,      /**< bytes of a page's name, at most */
	MANUAL_SECTION = 16,       /**< and of a section's */
	MANUAL_WHY     = 196,      /**< bytes of what says why there is no page */
	FOUND_MAX      = 4 << 10,  /**< bytes of man -w's answer: a path, a line break */
	ALIASES        = 8,        /**< names a file is kept by, at most */
	BUCKETS        = 1 << 12,  /**< the table's, to begin with */
};

/** A page as man rendered it, UTF-8 text, a page cut short ending in a note
 *  that says so; no one changes it. The manual holds a reference while it
 *  keeps the page, and each reader one.
 */
struct manual_page {
	struct timespec  ctime;  /**< its file's, as man began */
	dev_t            dev;
	ino_t            ino;
	off_t            size;
	size_t           len;
	_Atomic uint64_t refs;   /**< widened over the padding */
	char             text[];
};

/** A call that waits for a page. When the page is ready or man has
 *  failed, the waiter is posted to its loop, and the handler msg.fn frees
 *  it.
 */
struct manual_wait {
	struct msg            msg;
	struct manual_wait   *next;
	struct loop          *loop;
	struct manual_page   *page;   /**< a reference to the page, which the
	                                   handler moves to the call, or nullptr
	                                   with the reason in why */
	uint64_t              since;  /**< loop_clock() when the call began to
	                                   wait */
	struct handle         who;    /**< the handle of the call on its loop */
	UTIL_TEXT(MANUAL_WHY) why;
};

/** What a client loop has of the manual, the man tool's part of it: the
 *  fetches it runs, and the list of the loop's children.
 */
struct manual_loop {
	struct list     fetches;
	struct list    *children;
	struct manual  *manual;
	struct loop    *loop;
};

/* What ends a page cut short. */
static char const cut[] = "\n\n[The page goes on past the 4 MiB shown.]\n";
static_assert(TOOL_TEXT_MAX == 4U << 20, "the note says 4 MiB");

/* The reason of a fetch that ran out of memory. */
static struct cstr_view const manual_oom = CSTR_VIEW_INIT("Out of memory.");

struct manual_fetch;

/*
 * An entry of the manual's table: a page's file, keyed by its path, or an
 * alias, a name and section a page is asked by, keyed by "NAME\0SECTION",
 * which no path holds. Files stay until the manual is destroyed; an alias
 * goes as man fails for it. The manual's lock guards all but the key.
 */
struct manual_entry {
	struct htab_node     node;                   /* in the table, by the
	                                                hash of the key */
	union {
		struct {                                     /* a file's */
			struct manual_page  *page;     /* the manual's reference */
			uint64_t             aliases;  /* that name the file,
			                                  widened over the padding */
		};
		struct {                                     /* an alias's */
			struct manual_entry *file;     /* what man found for it */
			struct manual_fetch *fetch;    /* finding it, while under way */
			uint64_t             found;    /* loop_clock() as man found it */
		};
	};
	size_t               len;                    /* of the key, less its '\0' */
	char                 key[];
};

/* A fetch of an alias's page, on the loop of the ask that began it: man
 * finds the file, then renders it, unless the manual has the page as the
 * file is.
 */
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
	struct htab           table;    /* the entries */
	char                 *env[6];   /* man's environment */
	uint64_t              seed;     /* of the keys' hash */
	uint64_t              recheck;  /* ns a finding holds */
};

static struct manual_page *
manual_page_get (struct manual_page *page)
{
	ref_get(&page->refs);
	return page;
}

/* Let go of a reference to @p page, if not nullptr. */
static void
manual_page_put (struct manual_page *page)
{
	if (page && ref_put(&page->refs, 1))
		free(page);
}

/* Free @p *p_w, a waiter's post, and the page it holds, if any, and set it
 * to nullptr.
 */
static void
manual_wait_destroy (struct manual_wait **p_w)
{
	if (!p_w || !*p_w)
		return;
	struct manual_wait *w = *p_w;
	*p_w = nullptr;
	manual_page_put(w->page);
	w->page = nullptr;
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

/* Returns the section of the alias @p a, whose key is "NAME\0SECTION". */
static char const *
manual_entry_section (struct manual_entry const *a)
{
	return a->key + strlen(a->key) + 1;
}

/* Whether the key of the entry of @p node is the view at @p key. */
static bool
manual_match (struct htab_node const *node,
              void const             *key)
{
	struct manual_entry const *e = container_of(node, struct manual_entry, node);
	return cstr_view_equal((struct cstr_view){ e->key, e->len },
	                       *(struct cstr_view const *)key);
}

/* The entry keyed by @p key, of @p len bytes and @p hash; nullptr if none. */
static struct manual_entry *
manual_lookup (struct manual const *m,
               char const          *key,
               size_t               len,
               uint64_t             hash)
{
	struct cstr_view const k = { key, len };
	struct htab_node *node = *htab_find(&m->table, hash, manual_match, &k);
	return node ? container_of(node, struct manual_entry, node) : nullptr;
}

/* A new entry keyed by @p key, in the table, the buckets doubling once the
 * entries outnumber them, if memory allows: longer chains are all it costs
 * if not. nullptr if out of memory.
 */
static struct manual_entry *
manual_add (struct manual *m,
            char const    *key,
            size_t         len,
            uint64_t       hash)
{
	struct manual_entry *e = malloc(sizeof *e + len + 1);
	if (!e)
		return nullptr;
	*e = (struct manual_entry){ .node.hash = hash, .len = len };
	memcpy(e->key, key, len);
	e->key[len] = '\0';
	if (m->table.n > m->table.mask)
		(void)htab_grow(&m->table);
	htab_link(&m->table, &e->node);
	return e;
}

/* Take alias @p a out of the table, and free it. */
static void
manual_del (struct manual       *m,
            struct manual_entry *a)
{
	htab_unlink(&m->table, &a->node);
	if (a->file)
		--a->file->aliases;
	free(a);
}

/* Free @p *p_f, its waiters posted, and the page it holds, if any, and set
 * it to nullptr.
 */
static void
manual_fetch_destroy (struct manual_fetch **p_f)
{
	struct manual_fetch *f = *p_f;
	*p_f = nullptr;
	manual_page_put(f->page);
	f->page = nullptr;
	child_destroy(&f->child);
	loop_unready(&f->step);
	list_del(&f->hook);
	free(f);
}

/* Ends the failed fetch @p f. The function removes the alias of the fetch
 * and posts the reason @p why to each waiter.
 */
static void
manual_fetch_fail (struct manual_fetch *f,
                   struct cstr_view     why)
{
	struct manual *m = f->ml->manual;
	pthread_mutex_lock(&m->lock);
	f->alias->fetch = nullptr;
	manual_del(m, f->alias);
	struct manual_wait *w = f->waiters;
	pthread_mutex_unlock(&m->lock);
	for (struct manual_wait *next; w; w = next) {
		next = w->next;
		(void)util_text_store(&w->why, why);
		loop_post(w->loop, &w->msg);
	}
	manual_fetch_destroy(&f);
}

/*
 * The fetch is over with @p page, whose reference it takes: one it
 * rendered, which is f->page, the manual keeps by its file. The alias
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
	bool const rendered = page == f->page;
	pthread_mutex_lock(&m->lock);
	if (rendered) {
		old = file->page;
		file->page = manual_page_get(page);
		hist_count(&m->metrics.pages, !old);
		hist_count(&m->metrics.bytes, page->len - (old ? old->len : 0));
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
		manual_del(m, a);
	struct manual_wait *w = f->waiters;
	for (struct manual_wait *x = w; x; x = x->next)
		x->page = manual_page_get(page);
	pthread_mutex_unlock(&m->lock);

	if (rendered)
		LOG(LOG_VERBOSE, old ? "man page %s rendered anew: %zu bytes"
		                     : "man page %s rendered: %zu bytes", file->key, page->len);
	for (struct manual_wait *next; w; w = next) {
		next = w->next;
		loop_post(w->loop, &w->msg);
	}
	/* For a page that the fetch rendered, the file's old page goes here,
	 * and manual_fetch_destroy() puts f->page. Else the page goes. */
	manual_page_put(rendered ? old : page);
	manual_fetch_destroy(&f);
}

/* Run man with @p argv, its output up to @p max bytes, then @p done; else
 * fail the fetch with why not. Counts the run in @p runs.
 */
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
	f->child = child_create(loop, ml->children, &cmd, done, f, &e);
	if (f->child) {
		hist_count(runs, 1);
		return;
	}
	char buf[MANUAL_WHY];
	manual_fetch_fail(f, child_spawn_why(e, "man", buf, sizeof buf));
}

/* Writes the refusal of the alias @p a, which is not in the manual, to
 * the @p size bytes at @p why. Returns a view of the text.
 */
static struct cstr_view
manual_entry_none (struct manual_entry const *a,
                   char                      *why,
                   size_t                     size)
{
	char const *section = manual_entry_section(a);
	return *section ? util_format_view(why, size, "No manual entry for %s in section %s.",
	                                   a->key, section)
	                : util_format_view(why, size, "No manual entry for %s.", a->key);
}

/* Returns a view with data nullptr if man, @p c, ended well or its output
 * was cut short. Otherwise returns the reason, which may be text in the
 * @p size bytes at @p why.
 */
static struct cstr_view
manual_fetch_ended (struct manual_fetch const *f,
                    struct child const        *c,
                    char                      *why,
                    size_t                     size)
{
	if (c->flags & CHILD_CUT)
		return (struct cstr_view){};
	if (child_status(c) == 16)
		return manual_entry_none(f->alias, why, size);
	return child_why(c, "man", MANUAL_SECONDS * NS, why, size);
}

/* Copy as much of the page in as the step has the share for, made UTF-8
 * text, the rest in bulk steps; then the fetch is over.
 */
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
	char buf[MANUAL_WHY];
	struct cstr_view const why = manual_fetch_ended(f, c, buf, sizeof buf);
	if (why.data) {
		manual_fetch_fail(f, why);
		return;
	}
	size_t len = buf_peek(&c->out).len;
	size_t note = c->flags & CHILD_CUT ? sizeof cut - 1 : 0;
	struct manual_page *page = malloc(sizeof *page + len + note);
	if (!page) {
		manual_fetch_fail(f, manual_oom);
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
 * has, if the file is as it was, else render it.
 */
static void
manual_fetch_found (struct child *c)
{
	struct manual_fetch *f = c->owner;
	struct manual *m = f->ml->manual;
	char buf[MANUAL_WHY];
	struct cstr_view const why = manual_fetch_ended(f, c, buf, sizeof buf);
	if (why.data) {
		manual_fetch_fail(f, why);
		return;
	}
	struct span const out = buf_peek(&c->out);
	struct cstr_view path = { (char const *)out.ptr, out.len };
	if (!cstr_view_cut(&path, '\n', &path) || !cstr_view_starts(path, CSTR_VIEW("/")) ||
	    memchr(path.data, '\0', path.len)) {
		manual_fetch_fail(f, CSTR_VIEW("man named no file for the page."));
		return;
	}
	uint64_t hash = util_hash(m->seed, path.data, path.len);
	pthread_mutex_lock(&m->lock);
	struct manual_entry *file = manual_lookup(m, path.data, path.len, hash);
	if (!file)
		file = manual_add(m, path.data, path.len, hash);
	struct manual_page *page = file && file->page ? manual_page_get(file->page)
	                                              : nullptr;
	pthread_mutex_unlock(&m->lock);
	child_destroy(&f->child);
	f->file = file;
	if (!file) {
		manual_fetch_fail(f, manual_oom);
		return;
	}
	if (stat(file->key, &f->st)) {
		struct cstr_view const text = util_format_view(buf, sizeof buf, "Cannot read the "
		                                               "page's file: %s.", strerror(errno));
		manual_page_put(page);
		manual_fetch_fail(f, text);
		return;
	}
	if (page && manual_page_same(page, &f->st)) {
		manual_fetch_done(f, page);
		return;
	}
	manual_page_put(page);
	char const *argv[] = { "man", "-l", "--", file->key, nullptr };
	manual_fetch_run(f, argv, TOOL_TEXT_MAX, manual_fetch_rendered, &m->metrics.renders);
}

/* Have @p w, if not nullptr, wait for the page of the alias @p key: join
 * the fetch under way, or begin one; without a waiter, begin one only if
 * none is under way. Returns 0 or ENOMEM.
 */
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
	char const *section = manual_entry_section(a);
	char const *argv[7] = { "man", "-w" };
	size_t i = 2;
	if (*section) {
		argv[i++] = "-s";
		argv[i++] = section;
	}
	argv[i++] = "--";
	argv[i] = a->key;
	manual_fetch_run(f, argv, FOUND_MAX, manual_fetch_found, &m->metrics.finds);
	return 0;
}

/* Ends the call in @p slot with the text of @p page. The call owns the
 * reference @p page until manual_drop() releases it.
 */
static void
manual_answer (struct mcp_slot    *slot,
               struct manual_page *page)
{
	*mcp_slot_own(slot) = page;
	mcp_slot_lend(slot, (struct cstr_view){ page->text, page->len });
	mcp_slot_finish(slot, 0);
}

/* Handles the end of the fetch that a man call waited for. The function
 * ends the call with the page, or with the reason why there is no page,
 * unless the call has ended. After the loop has stopped, MCP no longer
 * exists, and the function only frees the waiter.
 */
static void
manual_back (struct loop *loop,
             struct msg  *msg)
{
	struct manual_wait *w = container_of(msg, struct manual_wait, msg);
	struct mcp_slot *slot = mcp_slot_named(loop, w->who);
	if (!slot) {
		manual_wait_destroy(&w);
		return;
	}
	/* The reference moves from the waiter to the call. */
	struct manual_page *page = w->page;
	w->page = nullptr;
	if (page) {
		manual_answer(slot, page);
	} else {
		LOG(LOG_VERBOSE, "man call failed after %.1f s: %s",
		    (double)(loop_clock() - w->since) / 1e9, w->why.data);
		mcp_slot_tell(slot, util_text_view(&w->why), TOOL_FAILED);
	}
	manual_wait_destroy(&w);
}

/*
 * Looks up the page @p name in the section @p section, which is empty for
 * no section. If the manual stores the page and the file of the page has
 * not changed, the function stores a reference to the page in @p *p_page.
 * Otherwise it sets @p *p_page to nullptr, and the call with the handle
 * @p who waits for manual_back(). Returns 0, EINVAL if the name or the
 * section is too long, or ENOMEM. The caller checks that man cannot read
 * the name or the section as an option.
 */
static int
manual_find (struct manual_loop  *ml,
             struct cstr_view     name,
             struct cstr_view     section,
             struct handle        who,
             struct manual_page **p_page)
{
	struct manual *m = ml->manual;
	*p_page = nullptr;
	if (name.len > MANUAL_NAME || section.len > MANUAL_SECTION)
		return EINVAL;
	char key[MANUAL_NAME + 1 + MANUAL_SECTION];
	size_t len = name.len + 1 + section.len;
	memcpy(key, name.data, name.len);
	key[name.len] = '\0';
	if (section.len)
		memcpy(key + name.len + 1, section.data, section.len);
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
		hist_count(&m->metrics.hits, 1);
		*p_page = page;
		/* man may now find another file for the page. The call receives
		 * the stored page, and manual_loop_ask() starts a new fetch
		 * unless one is under way. */
		if (recheck)
			(void)manual_loop_ask(ml, key, len, hash, nullptr);
		return 0;
	}
	manual_page_put(page);

	struct manual_wait *w = malloc(sizeof *w);
	if (!w)
		return ENOMEM;
	*w = (struct manual_wait){
		.msg.fn = manual_back,
		.loop = ml->loop,
		.since = loop_clock(),
		.who = who
	};
	int e = manual_loop_ask(ml, key, len, hash, w);
	if (e)
		free(w);
	else
		hist_count(&m->metrics.waits, 1);
	return e;
}

/* Creates the man tool's part of @p loop for @p service, a struct manual:
 * the list of the fetches that the loop runs. Their children go on
 * @p children. Returns the part, or nullptr with ENOMEM in @p *e.
 */
void *
manual_loop_create (void        *service,
                    struct loop *loop,
                    struct list *children,
                    int         *e)
{
	struct manual_loop *ml = malloc(sizeof *ml);
	if (!ml) {
		*e = ENOMEM;
		return nullptr;
	}
	*ml = (struct manual_loop){ .children = children, .manual = service, .loop = loop };
	list_init(&ml->fetches);
	*e = 0;
	return ml;
}

/* End the fetches the loop runs, as it stops: their waiters learn that the
 * server is shutting down, and their children are killed. Then free
 * @p *p_part, the man tool's part of the loop, and set it to nullptr.
 */
void
manual_loop_destroy (void **p_part)
{
	struct manual_loop *ml = *p_part;
	if (!ml)
		return;
	*p_part = nullptr;
	while (!list_is_empty(&ml->fetches))
		manual_fetch_fail(container_of(ml->fetches.next, struct manual_fetch, hook),
		                  CSTR_VIEW("Server is shutting down."));
	free(ml);
}

struct manual *
manual_create (uint32_t  recheck,
               int      *e)
{
	struct htab table = {};
	struct manual *m = malloc(sizeof *m);
	*e = !m ? ENOMEM : htab_init(&table, BUCKETS);
	*e = *e ? *e : pthread_mutex_init(&m->lock, nullptr);
	if (*e) {
		htab_fini(&table);
		free(m);
		return nullptr;
	}
	/* man's environment is askai's PATH and MANPATH, and UTF-8 text, 80
	 * columns wide, neither hyphenated nor justified, for a model to read. */
	static char const manpath[] = "MANPATH=";
	char *env[array_size(m->env)] = {
		util_env_path(),
		"LC_ALL=C.UTF-8",
		"MANWIDTH=80",
		"MANROFFOPT=-rHY=0 -dAD=l",
		util_env(manpath, sizeof manpath - 1)
	};
	memcpy(m->env, env, sizeof env);
	m->table = table;
	m->seed = util_seed();
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
	if (!p_manual || !*p_manual)
		return;
	struct manual *m = *p_manual;
	*p_manual = nullptr;
	for (struct htab_node *node = htab_next(&m->table, nullptr), *next; node; node = next) {
		next = htab_next(&m->table, node);
		struct manual_entry *e = container_of(node, struct manual_entry, node);
		/* A file's key, a path, holds no '\0'. */
		if (!memchr(e->key, '\0', e->len)) {
			manual_page_put(e->page);
			e->page = nullptr;
		}
		free(e);
	}
	htab_fini(&m->table);
	pthread_mutex_destroy(&m->lock);
	free(m);
}

struct manual_metrics const *
manual_counts (struct manual const *manual)
{
	return &manual->metrics;
}

/* Returns true if @p s, which ends in a NUL, is 1 to @p max of the
 * characters of @p set, and its first character is not the '-' that man
 * would read as the start of an option.
 */
static bool
manual_word (struct cstr_view  s,
             size_t            max,
             char const       *set)
{
	return s.len && s.len <= max && *s.data != '-' && strspn(s.data, set) == s.len;
}

/* Starts a man call in @p slot for the page in the name argument of
 * @p args, in the section of the section argument if it is given. If the
 * manual stores the page, the function ends the call with it. Otherwise
 * the call waits for manual_back() under its handle.
 */
static void
manual_start (struct mcp_slot    *slot,
              struct json_object *args)
{
	struct json_object *section = jb_get(args, "section");
	struct cstr_view const page = jb_view(jb_get(args, "name"));
	struct cstr_view sect = jb_view(section);
	/* A small model may give a section as a number. */
	int64_t number = jb_int(section, -1);
	char digit[2] = "";
	if (number >= 0 && number <= 9) {
		digit[0] = (char)('0' + number);
		sect = (struct cstr_view){ digit, 1 };
	}
	struct cstr_view const problem =
		!manual_word(page, MANUAL_NAME, UTIL_ALNUM "_.:+@-")
		? CSTR_VIEW("The name argument must name a manual page: 1 to 128 letters, "
		            "digits and _.:+@- characters, the first not -.")
		: section && !(sect.data && (!sect.len || manual_word(sect, MANUAL_SECTION, UTIL_ALNUM)))
		? CSTR_VIEW("The section argument must name a section, such as 1, 3 or 3p: "
		            "1 to 16 letters and digits.")
		: (struct cstr_view){};
	static_assert(MANUAL_NAME == 128 && MANUAL_SECTION == 16, "the problems say");
	if (problem.data) {
		mcp_slot_tell(slot, problem, TOOL_FAILED);
		return;
	}
	if (mcp_slot_offset(slot, args))
		return;
	struct handle who;
	struct manual_page *found = nullptr;
	int e = mcp_slot_name(slot, &who);
	e = e ? e : manual_find(mcp_slot_part(slot), page, sect, who, &found);
	if (e)
		mcp_slot_fail(slot, e);
	else if (found)
		manual_answer(slot, found);
	else
		mcp_slot_park(slot);
}

/* Let go of a man call's page, and set the call's own word to nullptr. */
static void
manual_drop (struct mcp_slot *slot)
{
	void **own = mcp_slot_own(slot);
	manual_page_put(*own);
	*own = nullptr;
}

static struct json_object *
manual_definition (void const *service)
{
	(void)service;
	return jb_object(
		"name", json_object_new_string("man"),
		"title", json_object_new_string("Manual page"),
		"description", json_object_new_string(
			"Read one of the server's Unix manual pages, as `man [section] name` "
			"shows it, in plain text; a long page is cut, and offset reads on "
			"past the cut. Use it to check the exact interface of a system "
			"call, library function, command, file format or convention. Pages "
			"are kept once rendered, so reading one again is cheap."),
		"inputSchema", schema_object(
			jb_object(
				"name", schema_property("string",
					"The page's name, such as printf, pthread_create, epoll "
					"or bash."),
				"section", schema_property("string",
					"Optional section: 1 commands, 2 system calls, 3 library "
					"functions, 3p POSIX, 4 devices, 5 file formats, 7 "
					"overviews and conventions, 8 administration. Without "
					"it, the first page man finds in its usual order."),
				"offset", schema_count(0, TOOL_OFFSET_ABOUT),
				nullptr),
			&CSTR_VIEW("name"), 1),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(1),
			"openWorldHint", json_object_new_boolean(0),
			nullptr),
		nullptr);
}

struct plugin const manual_plugin = {
	.tools = {{
		.definition = manual_definition,
		.start = manual_start,
		.drop = manual_drop,
		.flags = TOOL_LENT | TOOL_PAGED | TOOL_DETACH
	}}
};
