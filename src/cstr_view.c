/** @file String view construction, comparison and slicing (cstr_view.h). */
#include <string.h>

#include "cstr_view.h"
#include "dstr.h"

struct cstr_view
cstr_view (char const *text)
{
	return (struct cstr_view){ text, text ? strlen(text) : 0 };
}

struct cstr_view
cstr_view_dstr (struct dstr const *text)
{
	return (struct cstr_view){ dstr_get(text), dstr_len(text) };
}

bool
cstr_view_equal (struct cstr_view left,
                 struct cstr_view right)
{
	return left.len == right.len &&
	       (!left.len || !memcmp(left.data, right.data, left.len));
}

int
cstr_view_compare (struct cstr_view left,
                   struct cstr_view right)
{
	size_t common = left.len < right.len ? left.len : right.len;
	int difference = common ? memcmp(left.data, right.data, common) : 0;
	return difference ? difference : (left.len > right.len) - (left.len < right.len);
}

bool
cstr_view_starts (struct cstr_view text,
                  struct cstr_view prefix)
{
	return text.len >= prefix.len &&
	       (!prefix.len || !memcmp(text.data, prefix.data, prefix.len));
}

struct cstr_view
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
