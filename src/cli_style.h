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
	CLI_STYLE_TEXT,
	CLI_STYLE_FRAME,
	CLI_STYLE_TITLE,
	CLI_STYLE_TITLE_INACTIVE,
	CLI_STYLE_STATUS,
	CLI_STYLE_TOOL_ACTOR,
	CLI_STYLE_TOOL_STATUS,
	CLI_STYLE_PROGRESS,
	CLI_STYLE_COMPLETION_COMMAND,
	CLI_STYLE_COMPLETION_TOOL,
	CLI_STYLE_COMPLETION_PARAMETER,
	CLI_STYLE_COMPLETION_VALUE,
	CLI_STYLE_COMPLETION_NAME,
	CLI_STYLE_COMPLETION_FILE,
	CLI_STYLE_COUNT,
};

/** The SGR sequence of a style, NUL-terminated. */
struct cli_style_entry {
	char    bytes[15];
	uint8_t length;
};

/** Each sequence resets previous attributes. Invalid IDs return an empty view. */
extern struct cstr_view
cli_style_sequence (enum cli_style style);

/** The sequences of all styles, indexed by style, or with !color one whose
 * sequences are all empty. */
extern struct cli_style_entry const *
cli_style_palette (bool color);

#endif /* TWINCEPTION_SRC_CLI_STYLE_H_ */
