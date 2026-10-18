/** @file Counted views preserve NULs and clamp slices without overflow. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "cstr_view.h"
#include "dstr.h"

int
main (void)
{
	struct cstr_view empty = cstr_view(nullptr);
	assert(!empty.data && !empty.len);
	assert(cstr_view_equal(empty, CSTR_VIEW("")));
	assert(!cstr_view_compare(empty, CSTR_VIEW("")));
	assert(cstr_view_starts(empty, empty));
	assert(!cstr_view_starts(empty, CSTR_VIEW("x")));
	struct cstr_view slice = cstr_view_slice(empty, SIZE_MAX, SIZE_MAX);
	assert(!slice.data && !slice.len);

	char const counted[] = { 'a', '\0', 'b' };
	struct cstr_view text = { counted, sizeof counted };
	assert(cstr_view_equal(text, CSTR_VIEW("a\0b")));
	assert(!cstr_view_equal(text, CSTR_VIEW("a")));
	assert(!cstr_view_equal(text, CSTR_VIEW("a\0c")));
	assert(cstr_view_compare(text, CSTR_VIEW("a")) > 0);
	assert(cstr_view_compare(CSTR_VIEW("a"), text) < 0);
	assert(cstr_view_compare(text, CSTR_VIEW("a\0c")) < 0);
	assert(cstr_view_compare(CSTR_VIEW("\xff"), CSTR_VIEW("\x7f")) > 0);
	assert(cstr_view_starts(text, CSTR_VIEW("a\0")));
	assert(!cstr_view_starts(text, CSTR_VIEW("a\0b\0")));
	assert(cstr_view_starts(text, empty));
	assert(cstr_view_equal(cstr_view("a\0b"), CSTR_VIEW("a")));

	slice = cstr_view_slice(text, 1, SIZE_MAX);
	assert(slice.data == counted + 1 && slice.len == 2);
	assert(cstr_view_equal(slice, CSTR_VIEW("\0b")));
	assert(cstr_view_equal(cstr_view_slice(text, 1, 1), CSTR_VIEW("\0")));
	slice = cstr_view_slice(text, SIZE_MAX, SIZE_MAX);
	assert(slice.data == counted + sizeof counted && !slice.len);
	slice = cstr_view_slice(text, 1, 0);
	assert(slice.data == counted + 1 && !slice.len);

	struct dstr owned = DSTR_INIT;
	assert(dstr_cat_n(&owned, counted, sizeof counted));
	struct cstr_view borrowed = cstr_view_dstr(&owned);
	assert(borrowed.data == dstr_get(&owned));
	assert(cstr_view_equal(borrowed, text));
	dstr_cleanup(&owned);
	puts("cstr views: empty values, counted NULs, byte order and bounded slices: PASS");
}
