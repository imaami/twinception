/** @file Shared catalog for the C unit and C++ terminal tests. */
#ifndef TWINCEPTION_TESTS_AUTOCOMPLETE_FIXTURE_H_
#define TWINCEPTION_TESTS_AUTOCOMPLETE_FIXTURE_H_

#include "autocomplete.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int autocomplete_fixture (struct autocomplete *autocomplete);

#ifdef __cplusplus
}
#endif

#endif /* TWINCEPTION_TESTS_AUTOCOMPLETE_FIXTURE_H_ */
