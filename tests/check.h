/** @file The checks of a unit test: CHECK() counts and prints each check
 *  that fails, and check_report() prints the result of the test.
 */
#ifndef ASKAI_TESTS_CHECK_H_
#define ASKAI_TESTS_CHECK_H_

#include <stdio.h>

/** The number of checks that failed. */
static unsigned failures;

/** Counts @p cond in failures and prints it with its place if it is false. */
#define CHECK(cond) \
        ((cond) ? (void)0 \
                : (void)(++failures, fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                                             __FILE__, __LINE__, #cond)))

/** Prints the result of the test @p name. Returns the exit status of the
 *  test: 1 if a check failed, else 0.
 */
static inline int
check_report (char const *name)
{
	if (failures) {
		fprintf(stderr, "%s: %u checks failed\n", name, failures);
		return 1;
	}
	printf("%s: all checks passed\n", name);
	return 0;
}

#endif /* ASKAI_TESTS_CHECK_H_ */
