/** @file build.h
 *
 * A build of a repository: the translation units of the compile commands at
 * a commit, each with the graph that a parse of clang gave for it
 * (src/graph.h). The code tool answers callers from the graphs, without
 * clang.
 */
#ifndef ASKAI_SRC_BUILD_H_
#define ASKAI_SRC_BUILD_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "compat.h"
#include "cstr_view.h"
#include "graph.h"
#include "util.h"

struct cxi_out;

/** What a build contains for a translation unit. */
enum build_state : uint32_t {
	BUILD_NONE,     /**< nothing: no parse of the unit has ended */
	BUILD_AGAIN,    /**< nothing: a parse of the unit took too long, and the
	                     unit is parsed once more */
	BUILD_PARSING,  /**< nothing yet: a parse of the unit runs */
	BUILD_GRAPH,    /**< the graph of the unit. The unit does not change
	                     after this. */
	BUILD_FAILED,   /**< nothing: clang could not parse the unit. The unit
	                     does not change after this. */
};

/** Returns true if a unit in the state @p state waits for a parse. */
static force_inline bool
build_waits (uint32_t state)
{
	return state == BUILD_NONE || state == BUILD_AGAIN;
}

/** A translation unit of a build. */
struct build_unit {
	struct graph g;        /**< empty unless state is BUILD_GRAPH */
	uint32_t     path;     /**< the offset in the text of the build of the
	                            path of the file of the unit, without the
	                            path of the tree */
	uint32_t     words;    /**< the offset in the text of the clang command
	                            line of the unit */
	uint32_t     n_words;  /**< the bytes of the command line, whose words
	                            are each followed by a NUL */
	uint32_t     state;    /**< enum build_state */
};

/** The translation units of a build, in the order of the compile commands.
 *  text contains the paths of the files, each followed by a NUL, and the
 *  command lines. A zeroed build is empty.
 */
struct build {
	UTIL_ARRAY(struct build_unit);
	struct buf text;
};

/** Returns an empty build. */
static force_inline struct build
build (void)
{
	return (struct build){ .text = buf() };
}

/** Frees the graphs, the units and the text of @p b. */
extern void
build_fini (struct build *b);

/**
 * Appends to @p b the translation unit of the file @p path, which clang
 * parses with the command line @p words, each word followed by a NUL. The
 * unit is BUILD_NONE. Returns 0, ENOMEM, or EOVERFLOW if the text of the
 * build would reach 4 GiB.
 */
extern int
build_add (struct build     *b,
           struct cstr_view  path,
           struct cstr_view  words);

/** Returns the path of the file of @p unit, a unit of @p b. */
static force_inline char const *
build_path (struct build const      *b,
            struct build_unit const *unit)
{
	return (char const *)buf_peek(&b->text).ptr + unit->path;
}

/** Returns the clang command line of @p unit, a unit of @p b. */
static force_inline struct cstr_view
build_words (struct build const      *b,
             struct build_unit const *unit)
{
	return cstr_view_n((char const *)buf_peek(&b->text).ptr + unit->words,
	                   unit->n_words);
}

/**
 * Ends the parse of the unit @p unit of @p b with @p bytes, the output of
 * the parse. If the bytes are a graph (graph()), the unit takes the storage
 * of @p bytes and is BUILD_GRAPH. Otherwise, and for a null @p bytes, the
 * unit is BUILD_FAILED, and @p bytes is left as it is.
 */
extern void
build_parsed (struct build *b,
              size_t        unit,
              struct buf   *bytes);

/** The functions that callers looks for in the graphs of a build: each
 *  function and each function-like macro named name. With a mask of
 *  GRAPH_FN_STATIC and GRAPH_FN_MACRO, only the one that one translation
 *  unit declares: flags tells if it is a macro, or a static function. A
 *  macro or a static function is then the one whose first declaration is
 *  in file, a path as a graph names it. For another function, if file is
 *  not empty, a function that another file defines is no callee.
 */
struct build_callee {
	struct cstr_view name;
	struct cstr_view file;
	uint32_t         mask;
	uint32_t         flags;  /**< enum graph_fn_flag */
};

/**
 * Appends to @p out the callers of @p callee in the graphs of @p b, as a
 * list. Each function that @p callee names is a callee, with the file and
 * the lines of its definition, after the word macro for a macro, or after
 * the word declared those of its first declaration. Under a callee, a row
 * for each caller: the file and the lines of the calls, the calling
 * function and its lines, and notes in brackets. The caller of a use of a
 * macro can also be a macro, or the file scope. A row that several
 * translation units give, as for a function
 * that a header defines, is appended once. Then a line for each of: the
 * files of the units that are BUILD_FAILED, the files of the graphs with
 * GRAPH_ERRORS, and the number of the units without a parse that has
 * ended.
 */
extern void
build_callers (struct build const        *b,
               struct build_callee const *callee,
               struct cxi_out            *out);

#endif /* ASKAI_SRC_BUILD_H_ */
