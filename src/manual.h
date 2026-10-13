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
 *
 * The man tool is the manual's kind of tool (src/tool.h), its part of a
 * client loop the fetches there.  A man call takes its page at once if the
 * manual has it, else once man has, which the call waits for, named
 * (src/mcp.h).  It replies with the page, or with why there is none.  A
 * call that goes leaves the page to the manual.
 */
#ifndef TWINCEPTION_SRC_MANUAL_H_
#define TWINCEPTION_SRC_MANUAL_H_

#include <stdatomic.h>
#include <stdint.h>

struct manual;
struct plugin;

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
 * for a name holds @p recheck seconds.  nullptr, with the error in @p *e,
 * if it cannot be made.
 */
extern struct manual *
manual_create (uint32_t  recheck,
               int      *e);

/** Free the manual, once every loop has ended its fetches. */
extern void
manual_destroy (struct manual **p_manual);

extern struct manual_metrics const *
manual_metrics (struct manual const *manual);

/** The man tool, a plugin built in (src/plugin.h) whose instance is a
 *  manual. */
extern struct plugin const manual_plugin;

#endif /* TWINCEPTION_SRC_MANUAL_H_ */
