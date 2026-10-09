/**
 * @file suites_main.c
 * @brief main() for programs made of one suite per object size
 *
 * Expects suites.inc on the include path: one SUITE(name) line per
 * suite, each `name` a function compiled from a per-size translation
 * unit with the signature below.
 */
#include "hell.h"

#define SUITE(name) extern void name (struct hell_totals *t);
#include "suites.inc"
#undef SUITE

int
main (void)
{
	struct hell_totals t = { 0, 0 };
#define SUITE(name) name(&t);
#include "suites.inc"
#undef SUITE
	hell_collect(&t);
	return hell_exit_totals(&t);
}
