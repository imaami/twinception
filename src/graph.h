/** @file graph.h
 *
 * The graph of a translation unit: the functions that the translation unit
 * declares, and the calls in the code of the functions that it defines.
 * askai-code fills a draft from one parse of clang (src/cxi.c) and writes
 * the graph as bytes. A reader gets the graph from the bytes without clang.
 *
 * The bytes are a head and the parts. A part is an array of records whose
 * members are all uint32_t, in the byte order of the machine. The text is
 * the last part. A record names another record by the index of that record
 * in its part, and a text by the offset of the first byte of that text in
 * the text part. Each text ends with a NUL. graph() checks each index and
 * offset of the bytes, so that a reader follows them without a check.
 */
#ifndef ASKAI_SRC_GRAPH_H_
#define ASKAI_SRC_GRAPH_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "compat.h"
#include "cstr_view.h"
#include "util.h"

enum : uint32_t {
	GRAPH_NONE = UINT32_MAX,  /**< in place of an index or an offset:
	                               no record, no text */
};

/** How the program evaluates an expression of a function. The state of an
 *  expression is the largest of the states of its place in each expression
 *  that contains it.
 */
enum graph_state : uint32_t {
	GRAPH_EVALUATED,    /**< evaluated when the program reaches the code */
	GRAPH_UNPROVEN,     /**< the tool cannot tell if the program evaluates
	                         it */
	GRAPH_UNEVALUATED,  /**< never evaluated, such as an operand of
	                         sizeof */
};

enum graph_call_flag : uint32_t {
	GRAPH_CALL_ELEMENT = 1U << 0,  /**< the callee is an element of an
	                                    array */
	GRAPH_CALL_BUILTIN = 1U << 1,  /**< the callee is a builtin function
	                                    that no file declares */
	GRAPH_CALL_CLEANUP = 1U << 2,  /**< the call is that of the attribute
	                                    cleanup of a variable */
};

enum graph_fn_flag : uint32_t {
	GRAPH_FN_STATIC = 1U << 0,  /**< the function has internal linkage */
};

/** What a graph says of the parse of the translation unit. */
enum graph_flag : uint32_t {
	GRAPH_ERRORS = 1U << 0,  /**< clang reported an error. Calls can be
	                              missing in the graph. */
};

/** The lines of a declaration in a file. */
struct graph_place {
	uint32_t file;   /**< text: the path of the file as an answer prints
	                      it, without the path of the tree */
	uint32_t first;
	uint32_t last;
};

/** A function that the translation unit declares. */
struct graph_fn {
	uint32_t           name;      /**< text */
	uint32_t           flags;     /**< enum graph_fn_flag */
	struct graph_place declared;  /**< the first declaration */
	struct graph_place defined;   /**< the definition. defined.file is
	                                   GRAPH_NONE if the translation unit
	                                   defines no such function. */
	uint32_t           calls;     /**< the index of the first call in the
	                                   code of the definition */
	uint32_t           n_calls;   /**< the number of these calls. The
	                                   calls of a function are in the
	                                   graph only if a file of the tree
	                                   defines the function. */
};

/** A call in the code of a function: a call expression, or the call of the
 *  function of the attribute cleanup of a variable, at the line of the
 *  variable. The callees are the n indices from to on in the callee part,
 *  each one of a function. Several callees are the functions of which an
 *  expression selects one. n is 0 for a call through anything else, such
 *  as a function pointer, and for a call that is GRAPH_UNEVALUATED.
 */
struct graph_call {
	uint32_t file;   /**< text: the path of the file of the call */
	uint32_t line;
	uint32_t to;
	uint32_t n;
	uint32_t macro;  /**< text: the name of the macro that the call is
	                      written through, or GRAPH_NONE. Two calls
	                      through one definition of a macro name the
	                      same offset. */
	uint32_t state;  /**< enum graph_state */
	uint32_t flags;  /**< enum graph_call_flag */
};

/** A graph that a reader got from bytes. The parts are in bytes, which the
 *  graph owns. A zeroed graph is empty.
 */
struct graph {
	struct graph_fn const   *fns;
	struct graph_call const *calls;
	uint32_t const          *to;
	char const              *text;
	struct buf               bytes;
	uint64_t                 flags;  /**< enum graph_flag, widened over the
	                                      padding */
	uint32_t                 n_fns;
	uint32_t                 n_calls;
	uint32_t                 n_to;
	uint32_t                 n_text;
};

/**
 * Returns the graph of the readable bytes of @p bytes, which
 * graph_draft_write() wrote, and takes the storage of @p bytes: @p bytes is
 * then empty. If the bytes are not a graph, or one of another layout,
 * writes EINVAL to @p *e, returns an empty graph and leaves @p bytes as it
 * is. Writes 0 to @p *e otherwise.
 */
extern struct graph
graph (struct buf *bytes,
       int        *e);

/** Frees the bytes of @p g. */
extern void
graph_fini (struct graph *g);

/** Returns the text at the offset @p at of @p g. */
static force_inline char const *
graph_text (struct graph const *g,
            uint32_t            at)
{
	return g->text + at;
}

/** The parts of a graph while a parse fills them. A writer appends the
 *  records of the functions, the calls and the callees to the arrays
 *  (util_push()), and a text with graph_draft_text(). A zeroed draft is
 *  empty.
 */
struct graph_draft {
	UTIL_ARRAY(struct graph_fn)   fns;
	UTIL_ARRAY(struct graph_call) calls;
	UTIL_ARRAY(uint32_t)          to;
	UTIL_ARRAY(char)              text;
	uint64_t                      flags;  /**< enum graph_flag, widened
	                                           over the padding */
};

/** Returns an empty draft. */
static force_inline struct graph_draft
graph_draft (void)
{
	return (struct graph_draft){};
}

/** Frees the parts of @p draft. */
extern void
graph_draft_fini (struct graph_draft *draft);

/**
 * Appends @p s and a NUL to the text of @p draft, and writes the offset of
 * the text to @p *at. Returns 0, ENOMEM, or EOVERFLOW if the text part
 * would reach GRAPH_NONE bytes.
 */
extern int
graph_draft_text (struct graph_draft *draft,
                  struct cstr_view    s,
                  uint32_t           *at);

/**
 * Writes @p draft to @p fd as the bytes of a graph. Returns 0, the error
 * of write(), or EOVERFLOW if a part has GRAPH_NONE records or more.
 */
extern int
graph_draft_write (struct graph_draft const *draft,
                   int                       fd);

#endif /* ASKAI_SRC_GRAPH_H_ */
