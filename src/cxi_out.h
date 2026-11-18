/** @file cxi_out.h
 *
 * A text of the code tool (src/code.h): an answer or rows of one. A text
 * from clang or from the commit can contain any bytes, so the functions
 * that append one escape control bytes and bytes that are not UTF-8.
 */
#ifndef ASKAI_SRC_CXI_OUT_H_
#define ASKAI_SRC_CXI_OUT_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "compat.h"
#include "cstr_view.h"
#include "dstr.h"

/** The size of a text of cxi_out_fmt(). */
enum { CXI_OUT_FMT = 256 };

struct cxi_out {
	struct dstr      text;
	struct dstr      norm;  /**< the path of the last cxi_out_rel() of
	                             askai-code in normal form */
	struct cstr_view tree;  /**< the tree of the commit with a trailing
	                             slash, the prefix that cxi_out_text() and
	                             cxi_out_rel() remove from a path */
	int64_t          e;     /**< the error of an append that failed */
};

/** Returns an empty output. The functions that append a path to the output
 *  remove the prefix @p tree from the path.
 */
static force_inline struct cxi_out
cxi_out (struct cstr_view tree)
{
	return (struct cxi_out){
		.text = DSTR_INIT,
		.norm = DSTR_INIT,
		.tree = tree
	};
}

/** Frees the text of @p out. */
extern void
cxi_out_fini (struct cxi_out *out);

/** Appends the @p n bytes at @p p. */
extern void
cxi_out_put (struct cxi_out *out,
             char const     *p,
             size_t          n);

/** Appends the string literal @p s. */
#define cxi_out_lit(out, s) cxi_out_put((out), "" s, sizeof (s) - 1)

static force_inline void
cxi_out_view (struct cxi_out   *out,
              struct cstr_view  s)
{
	cxi_out_put(out, s.data, s.len);
}

/** Appends @p n in decimal. */
extern void
cxi_out_num (struct cxi_out *out,
             uint64_t        n);

/** Appends the @p n bytes that snprintf() wrote to the @p size bytes at
 *  @p buf. Sets the error of @p out if snprintf() failed or the text did
 *  not fit.
 */
static force_inline void
cxi_out_fmt_ (struct cxi_out *out,
              char const     *buf,
              int             size,
              int             n)
{
	if (n < 0 || n >= size)
		out->e = EOVERFLOW;
	else
		cxi_out_put(out, buf, (size_t)n);
}

/** Appends the text that snprintf() makes of a format and arguments: for a
 *  text that contains a number. The text must fit in CXI_OUT_FMT bytes with
 *  the NUL: an argument of %s is a short text of the code tool.
 */
#define cxi_out_fmt(out, ...) do {              \
        char cxi_fmt_[CXI_OUT_FMT];             \
        cxi_out_fmt_((out), cxi_fmt_,           \
                     (int)sizeof cxi_fmt_,      \
                     snprintf(cxi_fmt_,         \
                              sizeof cxi_fmt_,  \
                              __VA_ARGS__));    \
} while (0)

/** Appends the byte @p c as \xNN. */
extern void
cxi_out_hex (struct cxi_out *out,
             unsigned char   c);

/** Returns the length of the UTF-8 character at @p p, or 0 for a control
 *  character or bytes that are not UTF-8.
 */
extern size_t
cxi_out_char (char const *p,
              size_t      n);

/** Appends @p s from clang, the commit or the request without the path of
 *  the tree and the column of a place, as in (unnamed at src/x.h:7:2), with
 *  control bytes and bytes that are not UTF-8 as \n, \t or \xNN.
 */
extern void
cxi_out_text (struct cxi_out *out,
              char const     *s);

/** Appends the path or request word @p p, quoted as git quotes a path that
 *  contains '"', '\\', a control character or bytes that are not UTF-8.
 */
extern void
cxi_out_path (struct cxi_out   *out,
              struct cstr_view  p);

/** Appends the lines @p first to @p last of a declaration as FIRST-LAST, or
 *  as FIRST if the declaration is one line.
 */
extern void
cxi_out_range (struct cxi_out *out,
               uint32_t        first,
               uint32_t        last);

#endif /* ASKAI_SRC_CXI_OUT_H_ */
