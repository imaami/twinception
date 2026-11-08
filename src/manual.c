/** @file manual.c
 *
 * Man pages tool.
 */
#define _GNU_SOURCE /* memmem() */
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
	MANUAL_HEADING = 128,      /**< and of a heading's */
	MANUAL_WHY     = 1020,     /**< bytes of the reason why there is no
	                                page, with a list of pages; a waiter
	                                has no padding */
	MANUAL_HEADS   = 2044,     /**< bytes of a page's headings in a
	                                refusal; a call has no padding */
	HEAD_INDENT    = 3,        /**< spaces before a heading at most: man
	                                indents a section's heading 0 and a
	                                subsection's 3 */
	FOUND_MAX      = 4 << 10,  /**< bytes of man -w's answer: paths, each on a
	                                line */
	SEARCH_MAX     = 16 << 10, /**< bytes of man -k's answer that count */
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
	uint32_t         len;    /**< the length of text, at most TOOL_TEXT_MAX
	                              plus the length of the cut note */
	_Atomic uint32_t refs;
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

/* The state of the scan of a page for the part under a heading, as bits. */
enum manual_scan : uint32_t {
	SCAN_OPEN = 1u << 0,  /* the lines that follow belong to the part */
	SCAN_TAGS = 1u << 1,  /* a line as deep continues the part, as -Wformat=n after -Wformat */
};

/* A man call: the page it gives, and for a call with a heading, the scan
 * of the page for the part under the heading (manual_step()).
 */
struct manual_call {
	struct manual_page *page;     /* the call's reference, once it has one */
	struct json_object *heading;  /* a reference to the heading argument,
	                                 or nullptr */
	struct loop_lines   lines;    /* the page split so far */
	size_t              from;     /* where the part begins */
	size_t              to;       /* where its last line that is not blank
	                                 ends, its line feed included */
	uint32_t            depth;    /* the indentation of its first line, or
	                                 UINT32_MAX while there is no part */
	uint32_t            scan;     /* enum manual_scan */
	UTIL_TEXT(MANUAL_HEADS) heads;  /* " A, B,": the headings */
};

/* What ends a page cut short. */
static struct cstr_view const manual_cut =
	CSTR_VIEW_INIT("\n\n[The page goes on past the 4 MiB shown.]\n");
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
		hist_count(&m->metrics.bytes, (uint64_t)page->len - (old ? old->len : 0));
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
		                     : "man page %s rendered: %zu bytes",
		    file->key, (size_t)page->len);
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
	size_t const note = c->flags & CHILD_CUT ? manual_cut.len : 0;
	char buf[MANUAL_WHY];
	struct cstr_view const why = note ? (struct cstr_view){}
	                             : child_why(c, "man", MANUAL_SECONDS * NS, buf, sizeof buf);
	if (why.data) {
		manual_fetch_fail(f, why);
		return;
	}
	size_t const len = buf_peek(&c->out).len;
	struct manual_page *page = malloc(sizeof *page + len + note);
	if (!page) {
		manual_fetch_fail(f, manual_oom);
		return;
	}
	page->ctime = f->st.st_ctim;
	page->dev = f->st.st_dev;
	page->ino = f->st.st_ino;
	page->size = f->st.st_size;
	page->len = (uint32_t)(len + note);
	atomic_init(&page->refs, 1);
	memcpy(page->text + len, manual_cut.data, note);
	f->page = page;
	manual_fetch_copy(f->ml->loop, &f->step);
}

/* Ends the list of @p *len bytes at @p data, in which each item ends in a
 * comma: the comma of the last item becomes a period. A list that fills
 * the @p size bytes may have lost items, and its end becomes "..." instead.
 */
static void
manual_list_end (uint32_t *len,
                 char     *data,
                 size_t    size)
{
	if (*len + 1 < size) {
		data[*len - 1] = '.';
		return;
	}
	*len = (uint32_t)util_clip(data, *len, *len - 3);
	memcpy(data + *len, "...", 4);
	*len += 3;
}

/* Fails the fetch of @p c, a man -k run, with the miss and the pages whose
 * names begin with the name, if any.
 */
static void
manual_fetch_searched (struct child *c)
{
	struct manual_fetch *f = c->owner;
	char const *key = f->alias->key;
	struct cstr_view const name = { key, strlen(key) };
	char const *section = key + name.len + 1;
	struct span const out = buf_peek(&c->out);
	struct cstr_view rest = { (char const *)out.ptr, out.len };
	loop_work(f->ml->loop, LOOP_PARSE, out.len);
	UTIL_TEXT(MANUAL_WHY) why = {};
	(void)util_text_printf(&why, "No manual entry for %s%s%s.", key,
	                       *section ? " in section " : "", section);
	struct cstr_view const miss = util_text_view(&why);
	(void)util_text_printf(&why, " Pages whose names begin with %s:", key);
	uint32_t const lead = why.len;
	/* A line is "NAME (SECTION) - what the page is about". */
	for (struct cstr_view line, page; cstr_view_cut(&rest, '\n', &line); ) {
		if (cstr_view_cut(&line, ' ', &page) && cstr_view_starts_case(page, name) &&
		    cstr_view_starts(line, CSTR_VIEW("(")) && cstr_view_cut(&line, ')', &line))
			(void)util_text_printf(&why, " %.*s%.*s),", (int)page.len, page.data,
			                       (int)line.len, line.data);
	}
	manual_list_end(&why.len, why.data, sizeof why.data);
	manual_fetch_fail(f, why.len > lead ? util_text_view(&why) : miss);
}

/* Returns the section of the page file at @p path, which man -w named:
 * the last part of the file name between dots that begins with a digit,
 * such as 3posix in /usr/share/man/man3/printf.3posix.gz. The view is
 * empty if no part does.
 */
static struct cstr_view
manual_path_section (struct cstr_view path)
{
	struct cstr_view section = {}, part;
	while (cstr_view_cut(&path, '/', &part))
		continue;
	while (cstr_view_cut(&path, '.', &part))
		if (path.len && *path.data >= '0' && *path.data <= '9')
			section = path;
	(void)cstr_view_cut(&section, '.', &section);
	return section;
}

/* Man has found the alias's file, or failed to: give the page the manual
 * has, if the file is as it was, else render it.
 */
static void
manual_fetch_found (struct child *c)
{
	struct manual_fetch *f = c->owner;
	struct manual *m = f->ml->manual;
	if (child_status(c) == 16) {
		/* Man found no page: man -k lists the pages whose names begin with
		 * the name, . and + escaped. */
		child_destroy(&f->child);
		char re[1 + 2 * MANUAL_NAME + 1] = "^";
		size_t n = 1;
		for (char const *p = f->alias->key; *p; ++p) {
			re[n] = '\\';
			n += *p == '.' || *p == '+';
			re[n++] = *p;
		}
		char const *argv[] = { "man", "-k", "--", re, nullptr };
		manual_fetch_run(f, argv, SEARCH_MAX, manual_fetch_searched,
		                 &m->metrics.searches);
		return;
	}
	char buf[MANUAL_WHY];
	struct cstr_view const why = c->flags & CHILD_CUT ? (struct cstr_view){}
	                             : child_why(c, "man", MANUAL_SECONDS * NS, buf, sizeof buf);
	if (why.data) {
		manual_fetch_fail(f, why);
		return;
	}
	struct span const out = buf_peek(&c->out);
	struct cstr_view const paths = { (char const *)out.ptr, out.len };
	struct cstr_view path = paths;
	if (!cstr_view_cut(&path, '\n', &path) || !cstr_view_starts(path, CSTR_VIEW("/")) ||
	    memchr(path.data, '\0', path.len)) {
		manual_fetch_fail(f, CSTR_VIEW("man named no file for the page."));
		return;
	}
	loop_work(f->ml->loop, LOOP_PARSE, paths.len);
	UTIL_TEXT(MANUAL_WHY) several = {};
	(void)util_text_printf(&several, "Pages named %s are in more than one section:",
	                       f->alias->key);
	uint32_t digits = 0;  /* bit d: a page in section d */
	for (struct cstr_view rest = paths, line; cstr_view_cut(&rest, '\n', &line); ) {
		struct cstr_view const s = manual_path_section(line);
		if (s.len == 1)
			digits |= 1u << (*s.data - '0');
		if (s.len)
			(void)util_text_printf(&several, " %s(%.*s),", f->alias->key, (int)s.len,
			                       s.data);
	}
	if (digits & (digits - 1)) {
		manual_list_end(&several.len, several.data, sizeof several.data);
		(void)util_text_printf(&several, " Give section.");
		manual_fetch_fail(f, util_text_view(&several));
		return;
	}
	uint64_t hash = util_hash(m->seed, path.data, path.len);
	pthread_mutex_lock(&m->lock);
	struct manual_entry *file = manual_lookup(m, path.data, path.len, hash);
	if (!file)
		file = manual_add(m, path.data, path.len, hash);
	struct manual_page *page = file && file->page && ref_tryget(&file->page->refs)
	                           ? file->page : nullptr;
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

/* Begins a fetch of the page of the alias @p key, unless a fetch of that
 * page is under way. @p key points to the @p len bytes NAME\0SECTION, and
 * NAME is the first @p name_len bytes of @p key. If @p w is not nullptr,
 * the function adds @p w to the waiters of the fetch. Returns 0 or ENOMEM.
 */
static int
manual_loop_ask (struct manual_loop *ml,
                 char const         *key,
                 size_t              len,
                 size_t              name_len,
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

	/* Man finds the file, and without a section every page of the name:
	 * -s SECTION takes the place of -a. */
	char const *section = a->key + name_len + 1;
	char const *argv[7] = { "man", "-w", "-a" };
	size_t i = 3;
	if (*section) {
		argv[2] = "-s";
		argv[i++] = section;
	}
	argv[i++] = "--";
	argv[i] = a->key;
	manual_fetch_run(f, argv, FOUND_MAX, manual_fetch_found, &m->metrics.finds);
	return 0;
}

/* Gives the call in @p slot @p page, whose reference the call takes until
 * manual_drop(): all of it, or for a call with a heading, the part that
 * manual_step() finds. Returns true if the call goes on in steps.
 */
static bool
manual_answer (struct mcp_slot    *slot,
               struct manual_page *page)
{
	struct manual_call *call = *mcp_slot_own(slot);
	call->page = page;
	if (call->heading)
		return !mcp_slot_ready(slot);
	mcp_slot_lend(slot, (struct cstr_view){ page->text, page->len });
	mcp_slot_finish(slot, 0);
	return false;
}

/*
 * Scans @p line of the page @p text for the part of @p call under
 * @p heading. The part begins at a line after the page's first whose text
 * after its indentation begins with the heading, followed by a space, a
 * comma, =, [ or the end of the line: of such lines, the least indented,
 * the first of those. It goes on with the deeper lines that follow, and if
 * its first line has no two spaces in a row, with the lines as deep while
 * no blank or deeper line has come. Lines of at most MANUAL_HEADING bytes
 * and HEAD_INDENT spaces of indentation, without two spaces in a row, are
 * headings, which go to call->heads. Returns true once no later line can
 * change the part: a part at indentation 0 has closed.
 */
static bool
manual_call_line (struct manual_call *call,
                  struct cstr_view    text,
                  struct cstr_view    heading,
                  struct cstr_view    line)
{
	size_t const at = (size_t)(line.data - text.data);
	if (!at)
		return false;
	size_t depth = 0;
	while (depth < line.len && line.data[depth] == ' ')
		++depth;
	struct cstr_view const rest = cstr_view_slice(line, depth, SIZE_MAX);
	if (!rest.len) {
		call->scan &= ~SCAN_TAGS;
		return false;
	}
	if (depth > call->depth)
		call->scan &= ~SCAN_TAGS;
	else if (depth < call->depth || !(call->scan & SCAN_TAGS))
		call->scan = 0;
	size_t const end = at + line.len + (at + line.len < text.len);
	if (call->scan & SCAN_OPEN) {
		call->to = end;
		return false;
	}
	if (depth < call->depth && cstr_view_starts(rest, heading) &&
	    (rest.len == heading.len || memchr(" =,[", rest.data[heading.len], 4))) {
		call->from = at;
		call->to = end;
		call->depth = (uint32_t)depth;
		call->scan = SCAN_OPEN | (memmem(rest.data, rest.len, "  ", 2) ? 0 : SCAN_TAGS);
		return false;
	}
	if (depth <= HEAD_INDENT && rest.len <= MANUAL_HEADING &&
	    !memmem(rest.data, rest.len, "  ", 2))
		(void)util_text_printf(&call->heads, " %.*s,", (int)rest.len, rest.data);
	return !call->depth;
}

/* Looks for the part of the call's page under its heading, as far as the
 * step's share goes. Then ends the call with the part, or if the page has
 * no such part, with a refusal that lists the page's headings.
 */
static void
manual_step (struct mcp_slot *slot)
{
	struct manual_call *call = *mcp_slot_own(slot);
	struct cstr_view const text = { call->page->text, call->page->len };
	struct cstr_view const heading = jb_view(call->heading);
	struct loop *loop = mcp_slot_loop(slot);
	bool done = false;
	for (struct cstr_view line; !done && loop_lines_next(&call->lines, loop, text, &line); )
		done = manual_call_line(call, text, heading, line);
	if (!done && call->lines.start < text.len) {
		(void)mcp_slot_ready(slot);
		return;
	}
	if (call->depth != UINT32_MAX) {
		/* A part that runs into the cut of the page ends with the note. */
		size_t const to = call->to + manual_cut.len >= text.len &&
		                  cstr_view_ends(text, manual_cut) ? text.len : call->to;
		mcp_slot_lend(slot, cstr_view_slice(text, call->from, to - call->from));
		mcp_slot_finish(slot, 0);
	} else if (call->heads.len) {
		manual_list_end(&call->heads.len, call->heads.data, sizeof call->heads.data);
		mcp_slot_tellf(slot, TOOL_FAILED, "No line of the page begins with %.*s "
		               "followed by a space, a comma, =, [ or the line's end. Its "
		               "headings:%s", (int)heading.len, heading.data, call->heads.data);
	} else {
		mcp_slot_tellf(slot, TOOL_FAILED, "No line of the page begins with %.*s "
		               "followed by a space, a comma, =, [ or the line's end.",
		               (int)heading.len, heading.data);
	}
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
		(void)manual_answer(slot, page);
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
	struct manual_page *page = file && file->page && ref_tryget(&file->page->refs)
	                           ? file->page : nullptr;
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
			(void)manual_loop_ask(ml, key, len, name.len, hash, nullptr);
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
	int e = manual_loop_ask(ml, key, len, name.len, hash, w);
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
	*ml = (struct manual_loop){ .fetches = list(&ml->fetches), .children = children,
	                            .manual = service, .loop = loop };
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
	char *env[array_size(m->env)] = {
		util_env_path(),
		"LC_ALL=C.UTF-8",
		"MANWIDTH=80",
		"MANROFFOPT=-rHY=0 -dAD=l",
		util_env("MANPATH=", sizeof "MANPATH=" - 1)
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
	atomic_init(&mm->searches, 0);
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

/* Starts a man call in @p slot for the page in the name argument of
 * @p args, in the section of the section argument if it is given. If the
 * manual stores the page, the function gives it to the call. Otherwise
 * the call waits for manual_back() under its handle.
 */
static void
manual_start (struct mcp_slot    *slot,
              struct json_object *args)
{
	struct json_object *section = jb_get(args, "section");
	struct json_object *heading = jb_get(args, "heading");
	struct cstr_view const head = jb_view(heading);
	struct cstr_view const page = jb_text(jb_get(args, "name"), 1, MANUAL_NAME,
	                                      UTIL_ALNUM "_.:+@-");
	struct cstr_view sect = jb_text(section, 0, MANUAL_SECTION, UTIL_ALNUM);
	/* A small model may give a section as a number. */
	int64_t number = jb_int(section, -1);
	char digit[2] = "";
	if (number >= 0 && number <= 9) {
		digit[0] = (char)('0' + number);
		sect = (struct cstr_view){ digit, 1 };
	}
	struct cstr_view const problem =
		!page.data || *page.data == '-'
		? CSTR_VIEW("The name argument must name a manual page, such as printf or "
		            "git-grep: 1 to 128 letters, digits and _.:+@- characters, the "
		            "first not -.")
		: section && !sect.data
		? CSTR_VIEW("The section argument must name a section, such as 1, 3 or 3p: "
		            "1 to 16 letters and digits.")
		: heading && !(head.len && head.len <= MANUAL_HEADING && *head.data != ' ' &&
		               !memchr(head.data, '\n', head.len))
		? CSTR_VIEW("The heading argument must be the first words of a line of the "
		            "page: 1 to 128 bytes without a line feed, the first not a space.")
		: (struct cstr_view){};
	static_assert(MANUAL_NAME == 128 && MANUAL_SECTION == 16 && MANUAL_HEADING == 128,
	              "the problems say");
	if (problem.data) {
		mcp_slot_tell(slot, problem, TOOL_FAILED);
		return;
	}
	if (mcp_slot_offset(slot, args))
		return;
	struct manual_call *call = malloc(sizeof *call);
	if (!call) {
		mcp_slot_fail(slot, ENOMEM);
		return;
	}
	*call = (struct manual_call){ .heading = json_object_get(heading), .depth = UINT32_MAX };
	*mcp_slot_own(slot) = call;
	struct handle who;
	struct manual_page *found = nullptr;
	int e = mcp_slot_name(slot, &who);
	e = e ? e : manual_find(mcp_slot_part(slot), page, sect, who, &found);
	if (e)
		mcp_slot_fail(slot, e);
	else if (!found || manual_answer(slot, found))
		mcp_slot_park(slot);
}

/* Let go of a man call's page and heading, free the call, and set the
 * call's own word to nullptr.
 */
static void
manual_drop (struct mcp_slot *slot)
{
	void **own = mcp_slot_own(slot);
	struct manual_call *call = *own;
	*own = nullptr;
	manual_page_put(call->page);
	call->page = nullptr;
	json_object_put(call->heading);
	call->heading = nullptr;
	free(call);
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
			"shows it, in plain text, to check the exact interface of a system "
			"call, library function, command, file format or convention."),
		"inputSchema", schema_object(
			jb_object(
				"name", schema_property("string",
					"The page's name, such as printf, pthread_create, epoll "
					"or git-grep."),
				"section", schema_property("string",
					"Optional section: 1 commands, 2 system calls, 3 library "
					"functions, 3p POSIX, 4 devices, 5 file formats, 7 "
					"overviews and conventions, 8 administration. Without "
					"it, the first page man finds, or for a name with pages in "
					"two or more single-digit sections, a list of those pages."),
				"heading", schema_property("string",
					"Optional: the first words of a line of the page, such as "
					"SYNOPSIS, RETURN VALUE, EINVAL or -Wall. The output is "
					"then that line and the more deeply indented lines after "
					"it."),
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
		.step = manual_step,
		.drop = manual_drop,
		.flags = TOOL_LENT | TOOL_PAGED | TOOL_DETACH
	}}
};
