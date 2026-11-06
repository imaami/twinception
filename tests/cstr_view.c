/** @file Counted views preserve NULs and clamp slices without overflow. */
#include <assert.h>
#include <errno.h>
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

	assert(cstr_view_ends(text, CSTR_VIEW("\0b")));
	assert(cstr_view_ends(text, empty) && cstr_view_ends(empty, empty));
	assert(!cstr_view_ends(text, CSTR_VIEW("a\0b\0")));
	assert(!cstr_view_ends(text, CSTR_VIEW("\0c")));
	assert(cstr_view_equal_case(CSTR_VIEW("Content-Type"), CSTR_VIEW("content-TYPE")));
	assert(cstr_view_equal_case(empty, CSTR_VIEW("")));
	assert(cstr_view_equal_case(text, CSTR_VIEW("A\0B")));
	assert(!cstr_view_equal_case(text, CSTR_VIEW("A\0C")));
	assert(!cstr_view_equal_case(CSTR_VIEW("a"), CSTR_VIEW("ab")));
	assert(!cstr_view_equal_case(CSTR_VIEW("@[`{"), CSTR_VIEW("`{@[")));
	assert(!cstr_view_equal_case(CSTR_VIEW("\xc4"), CSTR_VIEW("\xe4")));
	assert(cstr_view_starts_case(CSTR_VIEW("DATA:,x"), CSTR_VIEW("data:")));
	assert(cstr_view_starts_case(empty, empty));
	assert(!cstr_view_starts_case(CSTR_VIEW("dat"), CSTR_VIEW("data:")));
	assert(!cstr_view_starts_case(text, CSTR_VIEW("a\0c")));

	slice = cstr_view_slice(text, 1, SIZE_MAX);
	assert(slice.data == counted + 1 && slice.len == 2);
	assert(cstr_view_equal(slice, CSTR_VIEW("\0b")));
	assert(cstr_view_equal(cstr_view_slice(text, 1, 1), CSTR_VIEW("\0")));
	slice = cstr_view_slice(text, SIZE_MAX, SIZE_MAX);
	assert(slice.data == counted + sizeof counted && !slice.len);
	slice = cstr_view_slice(text, 1, 0);
	assert(slice.data == counted + 1 && !slice.len);

	assert(cstr_view_equal(cstr_view_trim(CSTR_VIEW(" \t a b \r\n")), CSTR_VIEW("a b")));
	assert(cstr_view_equal(cstr_view_trim(CSTR_VIEW("\na\t")), CSTR_VIEW("\na")));
	assert(cstr_view_equal(cstr_view_trim(CSTR_VIEW("a\0")), CSTR_VIEW("a\0")));
	assert(!cstr_view_trim(CSTR_VIEW(" \r\n\t")).len);
	assert(!cstr_view_trim(empty).len);
	assert(cstr_view_equal(cstr_view_trim_end(CSTR_VIEW(" a \t\r\n")), CSTR_VIEW(" a")));
	assert(cstr_view_equal(cstr_view_trim_end(CSTR_VIEW("a\0")), CSTR_VIEW("a\0")));
	assert(!cstr_view_trim_end(empty).len);

	static struct cstr_view const names[] = {
		CSTR_VIEW_INIT("one"), CSTR_VIEW_INIT("two"), CSTR_VIEW_INIT("t\0o"),
	};
	assert(cstr_view_index(names, 3, CSTR_VIEW("two")) == 1);
	assert(cstr_view_index(names, 3, CSTR_VIEW("t\0o")) == 2);
	assert(cstr_view_index(names, 3, CSTR_VIEW("t")) == 3);
	assert(cstr_view_index(names, 3, empty) == 3);
	assert(cstr_view_index(names, 0, CSTR_VIEW("one")) == 0);

	struct dstr owned = DSTR_INIT;
	assert(dstr_cat_n(&owned, counted, sizeof counted));
	struct cstr_view borrowed = cstr_view_dstr(&owned);
	assert(borrowed.data == dstr_get(&owned));
	assert(cstr_view_equal(borrowed, text));
	assert(!cstr_view_store(CSTR_VIEW("replaced"), &owned));
	assert(cstr_view_equal(cstr_view_dstr(&owned), CSTR_VIEW("replaced")));
	assert(!cstr_view_store(empty, &owned) && !dstr_len(&owned));
	assert(!cstr_view_store(text, &owned));
	assert(cstr_view_equal(cstr_view_dstr(&owned), text));
	assert(!cstr_view_cat(empty, &owned) && !cstr_view_cat(CSTR_VIEW("!"), &owned));
	assert(cstr_view_equal(cstr_view_dstr(&owned), CSTR_VIEW("a\0b!")));
	assert(cstr_view_cat(text, nullptr) == ENOMEM);
	dstr_cleanup(&owned);
	puts("cstr views: empty values, counted NULs, byte order, cases, bounded slices, trims, "
	     "lookups, stores and appends: PASS");
}
