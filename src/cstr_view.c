/** @file cstr_view.c
 *
 * Conversions between string views and dstr objects.
 */
#include <errno.h>
#include <stdbit.h>

#include "cstr_view.h"
#include "dstr.h"

struct cstr_view
cstr_view_dstr (struct dstr const *text)
{
	return (struct cstr_view){ dstr_get(text), dstr_len(text) };
}

int
cstr_view_cat (struct cstr_view  text,
               struct dstr      *dest)
{
	return dstr_cat_n(dest, text.data, text.len) ? 0 : ENOMEM;
}

int
cstr_view_store (struct cstr_view  text,
                 struct dstr      *dest)
{
	dstr_clear(dest);
	return cstr_view_cat(text, dest);
}

int
cstr_view_join (struct cstr_view const *names,
                size_t                  n,
                uint64_t                mask,
                struct cstr_view        sep,
                struct cstr_view        last,
                struct dstr            *dest)
{
	if (n < 64)
		mask &= (UINT64_C(1) << n) - 1;
	int e = 0;
	while (!e && mask) {
		size_t const i = stdc_trailing_zeros(mask);
		mask &= mask - 1;
		e = cstr_view_cat(names[i], dest);
		if (!e && mask)
			e = cstr_view_cat(mask & (mask - 1) ? sep : last, dest);
	}
	return e;
}
