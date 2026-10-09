/**
 * @file constant.c
 * @brief A string literal initializes a dstr at compile time, even
 *        without optimization (trial 07)
 */
#include "dstr.h"

struct dstr
constant_short (void);

struct dstr
constant_short (void)
{
	return dstr("hello");
}

struct dstr
constant_empty (void);

struct dstr
constant_empty (void)
{
	return dstr("");
}
