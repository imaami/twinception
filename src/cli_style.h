/** @file cli_style.h
 *
 * Terminal styles identified by index, with one shared SGR palette.
 */
#ifndef ASKAI_SRC_CLI_STYLE_H_
#define ASKAI_SRC_CLI_STYLE_H_

#include <stdint.h>

/** The styles: X(ID, sequence), the constant CLI_STYLE_ID of enum cli_style
 *  and the SGR sequence of the style in the palette with color.
 */
#define CLI_STYLE_LIST(X)                           \
        X(RESET,                "\033[0m")          \
        X(INFO,                 "\033[0;38;5;110m") \
        X(PROMPT,               "\033[0;38;5;229m") \
        X(REASONING,            "\033[0;38;5;146m") \
        X(USER_INPUT,           "\033[0;38;5;231m") \
        X(ERROR,                "\033[0;38;5;203m") \
        X(TEXT,                 "\033[0;38;5;252m") \
        X(FRAME,                "\033[0;38;5;75m")  \
        X(TITLE,                "\033[0;38;5;229m") \
        X(TITLE_INACTIVE,       "\033[0;38;5;153m") \
        X(STATUS,               "\033[0;38;5;110m") \
        X(TOOL_ACTOR,           "\033[0;38;5;215m") \
        X(TOOL_STATUS,          "\033[0;38;5;180m") \
        X(PROGRESS,             "\033[0;38;5;109m") \
        X(COMPLETION_COMMAND,   "\033[0;38;5;81m")  \
        X(COMPLETION_TOOL,      "\033[0;38;5;114m") \
        X(COMPLETION_PARAMETER, "\033[0;38;5;222m") \
        X(COMPLETION_VALUE,     "\033[0;38;5;153m") \
        X(COMPLETION_NAME,      "\033[0;38;5;215m") \
        X(COMPLETION_FILE,      "\033[0;38;5;150m")

enum cli_style : uint32_t {
#define CLI_STYLE_ENUM(id, ...) CLI_STYLE_##id,
	CLI_STYLE_LIST(CLI_STYLE_ENUM)
#undef CLI_STYLE_ENUM
	CLI_STYLE_COUNT,
};

/** The SGR sequence of a style, NUL-terminated. */
struct cli_style_entry {
	char    bytes[15];
	uint8_t length;
};

/** The sequences of all styles, indexed by style, or with !color one whose
 * sequences are all empty.
 */
extern struct cli_style_entry const *
cli_style_palette (bool color);

#endif /* ASKAI_SRC_CLI_STYLE_H_ */
