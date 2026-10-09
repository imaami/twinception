/* A user translation unit's use of every dstr.h function and
 * macro, so that the inline bodies get compiled with the user's flags.
 * Included after the headers by the trial's generated sources. */

static struct dstr use_static = DSTR_INIT;

size_t
use_param (char name[16]);

/* An array parameter is a pointer: dstr() must not trip the
 * compilers' warnings about sizeof on array parameters */
size_t
use_param (char name[16])
{
	struct dstr s = dstr(name);
	size_t const n = dstr_len(&s);
	dstr_cleanup(&s);
	return n;
}

size_t
use_dstr (char const *text);

size_t
use_dstr (char const *text)
{
	struct dstr s = dstr(text);
	dstr_auto t = dstr(NULL);
	struct dstr_info const info = dstr_info(&s);
	size_t n = dstr_len(&s) + dstr_space_left(&s)
	         + dstr_get_block_count(&s) + info.length;

	if (info.state == DSTR_IS_SSO && DSTR_BLK_SIZE > 1U)
		++n;
	if (dstr_reserve(&s, 10) && dstr_cat(&s, "x"))
		n += (size_t)(dstr_get_write_ptr(&s) - dstr_get(&s));
	if (dstr_raw(&s) && dstr_cat(&t, dstr_get(&s)))
		++n;
	if (dstr_cat(&use_static, "y"))
		++n;
	if (dstr_cat_n(&s, text, n % 4U) &&
	    dstr_printf(&s, "%s%d%zu%c", text, -1, n, 'z'))
		n += dstr_len(&s);
	if (dstr_erase(&s, 1, n) && dstr_clear(&s))
		++n;

	struct dstr m = dstr_move(&s);
	n += dstr_len(&m);
	dstr_cleanup(&m);
	dstr_cleanup(&s);
	dstr_cleanup(&use_static);

	/* Every kind of dstr() argument: literals that fit in place
	 * or not (the long one has 128 characters, more than any object
	 * size allows in place), a parenthesized literal, one with an
	 * embedded null, arrays, a pointer into an array, and a pointer to
	 * a long literal (gcc -O0 used to warn about that one) */
	char small[8] = "array";
	char large[200] = "large array";
	char const *const known = "abcdefghijklmnopqrstuvwxyz0123";
	struct dstr v[] = {
		dstr("literal"),
		dstr(""),
		dstr("a string literal longer than the largest in-place"
		     " string, which holds 127 characters at the very most,"
		     " so it is always allocated"),
		dstr(("parenthesized")),
		dstr("embedded\0null"),
		dstr(small),
		dstr(large),
		dstr(small + 1),
		dstr(known),
	};
	for (size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
		n += dstr_len(&v[i]);
		dstr_cleanup(&v[i]);
	}
	return n + use_param(large);
}
