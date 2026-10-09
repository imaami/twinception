/**
 * @file fold.c
 * @brief Uses of dstr that an optimizing compiler must fold into
 *        constants (trial 07)
 *
 * Every object here is a local initialized from a short string
 * literal, so its contents are known at compile time. The trial checks
 * that the object file references no symbol at all except fold_sink(),
 * an outside function some of them call while their object is alive.
 */
#include <stddef.h>
#include <stdint.h>

#include "dstr.h"

/** A function the compiler knows nothing about */
extern void
fold_sink (void);

size_t
fold_len (void);

size_t
fold_len (void)
{
	dstr_auto s = dstr("hello");
	return dstr_len(&s);
}

size_t
fold_full (void);

size_t
fold_full (void)
{
	/* A literal that exactly fills the default object */
#if UINTPTR_MAX > 0xFFFFFFFFU
	dstr_auto s = dstr("abcdefghijklmnopqrstuvw");
#else
	dstr_auto s = dstr("abcdefghijk");
#endif
	return dstr_len(&s) + dstr_space_left(&s);
}

size_t
fold_cat (void);

size_t
fold_cat (void)
{
	dstr_auto s = dstr("abc");
	if (!dstr_cat(&s, "def"))
		return 0;
	return dstr_len(&s);
}

int
fold_eq (void);

int
fold_eq (void)
{
	dstr_auto s = dstr("hello");
	char const *const p = dstr_get(&s);
	return p[0] == 'h' && p[4] == 'o' && !p[5];
}

size_t
fold_cleanup (void);

size_t
fold_cleanup (void)
{
	struct dstr s = dstr("hi");
	size_t const n = dstr_space_left(&s);
	dstr_cleanup(&s);
	return n + dstr_len(&s);
}

size_t
fold_pointer (void);

size_t
fold_pointer (void)
{
	char const *const p = "hello";
	dstr_auto s = dstr(p);
	return dstr_len(&s);
}

size_t
fold_call (void);

size_t
fold_call (void)
{
	/* The object stays alive across a call the compiler can't see */
	dstr_auto s = dstr("hello");
	fold_sink();
	return dstr_len(&s);
}

size_t
fold_call_cat (void);

size_t
fold_call_cat (void)
{
	dstr_auto s = dstr("abc");
	fold_sink();
	if (!dstr_cat(&s, "def"))
		return 0;
	fold_sink();
	return dstr_len(&s);
}

size_t
fold_reserve (void);

size_t
fold_reserve (void)
{
	/* A reserve that fits in place, around calls the compiler can't
	 * see */
	dstr_auto s = dstr("abc");
	fold_sink();
	if (!dstr_reserve(&s, 5))
		return 0;
	fold_sink();
	return dstr_space_left(&s);
}

size_t
fold_cat_n (void);

size_t
fold_cat_n (void)
{
	/* Three bytes of a literal, one of them a null character */
	dstr_auto s = dstr("abc");
	fold_sink();
	if (!dstr_cat_n(&s, "d\0fgh", 3))
		return 0;
	fold_sink();
	return dstr_len(&s) + (size_t)dstr_get(&s)[5];
}

size_t
fold_erase (void);

size_t
fold_erase (void)
{
	dstr_auto s = dstr("hello");
	fold_sink();
	if (!dstr_erase(&s, 1, 3))
		return 0;
	fold_sink();
	char const *const p = dstr_get(&s);
	return dstr_len(&s) + (p[0] == 'h' && p[1] == 'o' && !p[2]);
}

size_t
fold_clear (void);

size_t
fold_clear (void)
{
	dstr_auto s = dstr("hello");
	fold_sink();
	if (!dstr_clear(&s))
		return 0;
	fold_sink();
	return dstr_len(&s) + dstr_space_left(&s);
}

size_t
fold_move (void);

size_t
fold_move (void)
{
	/* Both objects stay alive across the calls */
	dstr_auto s = dstr("hello");
	fold_sink();
	dstr_auto t = dstr_move(&s);
	fold_sink();
	return dstr_len(&s) + dstr_len(&t);
}
