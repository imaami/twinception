/** @file Shared catalog for the C unit and C++ terminal tests. */
#ifndef ASKAI_TESTS_AUTOCOMPLETE_FIXTURE_H_
#define ASKAI_TESTS_AUTOCOMPLETE_FIXTURE_H_

#include "autocomplete.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int autocomplete_fixture (struct autocomplete *autocomplete);

#ifdef __cplusplus
}
#endif

#endif /* ASKAI_TESTS_AUTOCOMPLETE_FIXTURE_H_ */
