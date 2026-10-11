/** @file The manual: the pages man(1) renders for the man tool, each kept
 *  once rendered, for every client loop to give (design section 4).
 *
 * A page is asked for by its name and, optionally, its section; man finds
 * the file for those (man -w), and the manual keeps the page by that file,
 * so that every name man finds it by shares the one rendering (man -l).  A
 * page is given as long as a stat() of its file, on each ask, finds it
 * unchanged.  Once man's finding for a name is older than the recheck, the
 * next ask has man find it again in the background, the page given
 * meanwhile, so that a page installed since, which man would now find first,
 * takes over.  The asks for a page that man is under way with wait for that
 * one fetch, on whichever loop began it, and it goes on, its page kept, when
 * the calls that waited for it go.  The manual keeps what it renders for
 * good: all of a system's pages take some 100 to 200 MiB.
 */
#ifndef TWINCEPTION_SRC_MANUAL_H_
#define TWINCEPTION_SRC_MANUAL_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "list.h"
#include "loop.h"

enum {
	MANUAL_MAX     = 2 << 20,  /**< bytes of a page kept: past those, cut short */
	MANUAL_SECONDS = 10,       /**< for man to find, or render, a page */
	MANUAL_NAME    = 128,      /**< bytes of a page's name, at most */
	MANUAL_SECTION = 16,       /**< and of a section's */
	MANUAL_WHY     = 192,      /**< bytes of what says why there is no page */
};

struct manual;

/** A page as man rendered it, UTF-8 text, a page cut short ending in a note
 *  that says so; no one changes it.  The manual holds a reference while it
 *  keeps the page, and each reader one. */
struct manual_page {
	struct timespec  ctime;  /**< its file's, as man began */
	dev_t            dev;
	ino_t            ino;
	off_t            size;
	size_t           len;
	_Atomic uint32_t refs;
	char             text[];
};

/** A call that waits for a page: posted to its loop, msg.fn its handler,
 *  once the page is in or man has failed; the handler frees it. */
struct manual_wait {
	struct msg           msg;
	struct manual_wait  *next;
	struct loop         *loop;
	struct manual_page  *page;   /**< the page, a reference the handler
	                                  may take; else nullptr, and why */
	struct handle        who;    /**< names the call on its loop */
	char                 why[MANUAL_WHY];
};

/** What a client loop has of the manual: the fetches it runs, and their
 *  children. */
struct manual_loop {
	struct list     fetches;
	struct list     children;
	struct manual  *manual;
	struct loop    *loop;
};

/** What the manual has done: any thread writes, any reads (src/hist.h). */
struct manual_metrics {
	_Atomic uint64_t pages;    /**< kept */
	_Atomic uint64_t bytes;    /**< their text */
	_Atomic uint64_t hits;     /**< asks a kept page answered */
	_Atomic uint64_t waits;    /**< asks that waited for man */
	_Atomic uint64_t finds;    /**< runs of man that found a page's file */
	_Atomic uint64_t renders;  /**< runs of man that rendered a page */
};

/**
 * A manual, empty, for man to fill with askai's PATH and MANPATH, its pages
 * 80 columns of UTF-8 text, neither hyphenated nor justified; man's finding
 * for a name holds @p recheck seconds.  Returns 0 or ENOMEM.
 */
extern int
manual_new (struct manual **p_manual,
            uint32_t        recheck);

/** Free the manual, once every loop has ended its fetches. */
extern void
manual_free (struct manual **p_manual);

extern struct manual_metrics const *
manual_metrics (struct manual const *manual);

/** Begin @p ml, the manual's part of @p loop. */
static inline void
manual_loop_init (struct manual_loop *ml,
                  struct manual      *manual,
                  struct loop        *loop)
{
	list_init(&ml->fetches);
	list_init(&ml->children);
	ml->manual = manual;
	ml->loop = loop;
}

/** End the fetches the loop runs, as it stops: their waiters learn that the
 *  server is shutting down, and their children are killed. */
extern void
manual_loop_fini (struct manual_loop *ml);

/**
 * The page @p name, of @p name_len bytes, of the section @p section, of
 * @p section_len bytes, 0 for none: in @p *p_page, a reference, if the
 * manual has it as its file is.  Else @p *p_page is nullptr, and the call
 * @p who names on the loop waits for a post to @p fn, a struct manual_wait.
 * Returns 0; EINVAL if either is too long; or ENOMEM.  They are man's to
 * take as a name and a section, never as an option: the caller checks them.
 */
extern int
manual_find (struct manual_loop  *ml,
             char const          *name,
             size_t               name_len,
             char const          *section,
             size_t               section_len,
             struct handle        who,
             void               (*fn) (struct loop *loop,
                                       struct msg  *msg),
             struct manual_page **p_page);

/** Let go of a reference to @p page, if not nullptr. */
extern void
manual_put (struct manual_page *page);

/** Free @p w, a waiter's post, and the page it holds, if any. */
extern void
manual_wait_free (struct manual_wait *w);

#endif /* TWINCEPTION_SRC_MANUAL_H_ */
