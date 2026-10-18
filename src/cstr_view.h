/** @file
 * Read-only, byte-counted views of strings.
 */
#ifndef TWINCEPTION_SRC_CSTR_VIEW_H_
#define TWINCEPTION_SRC_CSTR_VIEW_H_

#include <stddef.h>

struct dstr;

/**
 * A view owns no storage. Its bytes must remain readable and unchanged for
 * as long as it is used. data may be null only when len is zero. A view need
 * not end in NUL and may contain embedded NULs; len counts bytes, not code
 * points. These operations neither validate nor interpret an encoding.
 */
struct cstr_view {
	char const *data;
	size_t      len;
};

/** A string literal's bytes, excluding its final NUL. Rejects pointers. */
#define CSTR_VIEW(s) ((struct cstr_view){ "" s, sizeof (s) - 1 })

/** Measure a NUL-terminated string once. A null pointer gives an empty view. */
extern struct cstr_view
cstr_view (char const *text);

/** View a dstr's full contents, including embedded NULs, until it changes. */
extern struct cstr_view
cstr_view_dstr (struct dstr const *text);

/** Compare all bytes, including embedded NULs. Empty views compare equal. */
extern bool
cstr_view_equal (struct cstr_view left,
                 struct cstr_view right);

/** Lexicographic byte order: negative, zero or positive, like strcmp(). */
extern int
cstr_view_compare (struct cstr_view left,
                   struct cstr_view right);

/** Whether text begins with prefix. The empty prefix matches every view. */
extern bool
cstr_view_starts (struct cstr_view text,
                  struct cstr_view prefix);

/**
 * Take up to length bytes starting at offset, clamping both to the view.
 * SIZE_MAX as length takes the remaining bytes. The result references the
 * original storage; slicing does not allocate or seek character boundaries.
 */
extern struct cstr_view
cstr_view_slice (struct cstr_view text,
                 size_t           offset,
                 size_t           length);

#endif /* TWINCEPTION_SRC_CSTR_VIEW_H_ */
