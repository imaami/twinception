/** @file
 * Read-only, byte-counted views of strings.
 */
#ifndef TWINCEPTION_SRC_CSTR_VIEW_H_
#define TWINCEPTION_SRC_CSTR_VIEW_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

/** The initializer of a view of a string literal's bytes, excluding its
 *  final NUL, which a table of static storage can take. Rejects pointers.
 */
#define CSTR_VIEW_INIT(s) { "" s, sizeof (s) - 1 }

/** A string literal's bytes, excluding its final NUL. Rejects pointers. */
#define CSTR_VIEW(s) ((struct cstr_view)CSTR_VIEW_INIT(s))

/** Measure a NUL-terminated string once. Returns an empty view for a null
 *  pointer.
 */
static inline struct cstr_view
cstr_view (char const *text)
{
	return (struct cstr_view){ text, text ? strlen(text) : 0 };
}

/** View a dstr's full contents, including embedded NULs, until it changes. */
extern struct cstr_view
cstr_view_dstr (struct dstr const *text);

/** Append the bytes of text, which must not point into dest, to dest.
 * Returns 0, or ENOMEM with dest unchanged.
 */
extern int
cstr_view_cat (struct cstr_view  text,
               struct dstr      *dest);

/** Replace the contents of dest with the bytes of text, which must not
 * point into dest. Returns 0, or ENOMEM with dest left empty.
 */
extern int
cstr_view_store (struct cstr_view  text,
                 struct dstr      *dest);

/**
 * Append to @p dest the views @p names[i] whose bit i is set in @p mask, of
 * the first @p n, with @p sep between two and @p last before the final one:
 * "a, b or c". Returns 0, or ENOMEM with a part of the list appended.
 */
extern int
cstr_view_join (struct cstr_view const *names,
                size_t                  n,
                uint64_t                mask,
                struct cstr_view        sep,
                struct cstr_view        last,
                struct dstr            *dest);

/** Compare all bytes, including embedded NULs. Empty views compare equal. */
static inline bool
cstr_view_equal (struct cstr_view left,
                 struct cstr_view right)
{
	return left.len == right.len &&
	       (!left.len || !memcmp(left.data, right.data, left.len));
}

/** Lexicographic byte order: negative, zero or positive, like strcmp(). */
static inline int
cstr_view_compare (struct cstr_view left,
                   struct cstr_view right)
{
	size_t common = left.len < right.len ? left.len : right.len;
	int difference = common ? memcmp(left.data, right.data, common) : 0;
	return difference ? difference : (left.len > right.len) - (left.len < right.len);
}

/** Whether text begins with prefix. The empty prefix matches every view. */
static inline bool
cstr_view_starts (struct cstr_view text,
                  struct cstr_view prefix)
{
	return text.len >= prefix.len &&
	       (!prefix.len || !memcmp(text.data, prefix.data, prefix.len));
}

/** Whether text ends with suffix. The empty suffix matches every view. */
static inline bool
cstr_view_ends (struct cstr_view text,
                struct cstr_view suffix)
{
	return text.len >= suffix.len &&
	       (!suffix.len || !memcmp(text.data + text.len - suffix.len, suffix.data, suffix.len));
}

/** Compare all bytes, as cstr_view_equal() does, but take an ASCII letter
 *  and its other case as equal.
 */
static inline bool
cstr_view_equal_case (struct cstr_view left,
                      struct cstr_view right)
{
	if (left.len != right.len)
		return false;
	for (size_t i = 0; i < left.len; ++i) {
		unsigned char a = (unsigned char)left.data[i] | 0x20;
		unsigned char b = (unsigned char)right.data[i] | 0x20;
		if (a != b || (left.data[i] != right.data[i] && (a < 'a' || a > 'z')))
			return false;
	}
	return true;
}

/** Whether text begins with prefix, in any case of its ASCII letters. */
static inline bool
cstr_view_starts_case (struct cstr_view text,
                       struct cstr_view prefix)
{
	return text.len >= prefix.len &&
	       cstr_view_equal_case((struct cstr_view){ text.data, prefix.len }, prefix);
}

/**
 * Take up to length bytes starting at offset, clamping both to the view.
 * SIZE_MAX as length takes the remaining bytes. The result references the
 * original storage; slicing does not allocate or seek character boundaries.
 */
static inline struct cstr_view
cstr_view_slice (struct cstr_view text,
                 size_t           offset,
                 size_t           length)
{
	if (offset > text.len)
		offset = text.len;
	text.len -= offset;
	if (text.data)
		text.data += offset;
	if (text.len > length)
		text.len = length;
	return text;
}

/**
 * Split @p *rest at its first byte @p c: store the bytes before it in
 * @p *head and leave the bytes after it in @p *rest. Returns false, with
 * neither changed, if @p *rest does not contain @p c. With @p head equal
 * to @p rest, @p *rest ends as the bytes before @p c.
 */
static inline bool
cstr_view_cut (struct cstr_view *rest,
               char              c,
               struct cstr_view *head)
{
	struct cstr_view const text = *rest;
	char const *at = text.len ? memchr(text.data, c, text.len) : nullptr;
	if (!at)
		return false;
	size_t const n = (size_t)(at - text.data);
	*rest = (struct cstr_view){ at + 1, text.len - n - 1 };
	*head = (struct cstr_view){ text.data, n };
	return true;
}

/** Returns the index of the first of the @p n views at @p names that equals
 *  @p key, or @p n if none does.
 */
static inline size_t
cstr_view_index (struct cstr_view const *names,
                 size_t                  n,
                 struct cstr_view        key)
{
	size_t i = 0;
	while (i < n && !cstr_view_equal(names[i], key))
		++i;
	return i;
}

/** Whether @p c is a space, a tab, a carriage return or a line feed. */
static inline bool
cstr_view_space (char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/** Drop spaces, tabs, carriage returns and line feeds from the end of
 *  @p text.
 */
static inline struct cstr_view
cstr_view_trim_end (struct cstr_view text)
{
	while (text.len && cstr_view_space(text.data[text.len - 1]))
		--text.len;
	return text;
}

/**
 * Drop spaces and tabs from the start of @p text, and spaces, tabs,
 * carriage returns and line feeds from its end, as from a line of input or
 * the value of an HTTP header.
 */
static inline struct cstr_view
cstr_view_trim (struct cstr_view text)
{
	text = cstr_view_trim_end(text);
	while (text.len && (*text.data == ' ' || *text.data == '\t')) {
		++text.data;
		--text.len;
	}
	return text;
}

/**
 * Whether an element of the comma-separated @p list, a field value of HTTP
 * (RFC 9110 5.6.1), equals @p token in any case. The white space around an
 * element and its ";" parameters do not count.
 */
static inline bool
cstr_view_list_has (struct cstr_view list,
                    struct cstr_view token)
{
	for (bool more = true; more; ) {
		struct cstr_view item = list;
		more = cstr_view_cut(&list, ',', &item);
		(void)cstr_view_cut(&item, ';', &item);
		if (cstr_view_equal_case(cstr_view_trim(item), token))
			return true;
	}
	return false;
}

#endif /* TWINCEPTION_SRC_CSTR_VIEW_H_ */
