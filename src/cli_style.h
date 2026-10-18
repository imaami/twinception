/** @file Terminal styles identified by index, with one shared SGR palette. */
#ifndef TWINCEPTION_SRC_CLI_STYLE_H_
#define TWINCEPTION_SRC_CLI_STYLE_H_

#include <stdint.h>

#include "cstr_view.h"

enum cli_style : uint32_t {
	CLI_STYLE_RESET,
	CLI_STYLE_INFO,
	CLI_STYLE_PROMPT,
	CLI_STYLE_REASONING,
	CLI_STYLE_USER_INPUT,
	CLI_STYLE_ERROR,
	CLI_STYLE_COUNT,
};

/** Each sequence resets previous attributes. Invalid IDs return an empty view. */
extern struct cstr_view
cli_style_sequence (enum cli_style style);

#endif /* TWINCEPTION_SRC_CLI_STYLE_H_ */
