/** @file cstr_view.c
 *
 * Conversions between string views and dstr objects.
 */
#include <errno.h>

#include "cstr_view.h"
#include "dstr.h"

struct cstr_view
cstr_view_dstr (struct dstr const *text)
{
	return (struct cstr_view){ dstr_get(text), dstr_len(text) };
}

int
cstr_view_store (struct cstr_view  text,
                 struct dstr      *dest)
{
	dstr_clear(dest);
	return dstr_cat_n(dest, text.data, text.len) ? 0 : ENOMEM;
}
