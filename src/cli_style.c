#include "cli_style.h"

struct cli_style_entry {
	char    bytes[15];
	uint8_t length;
};

static_assert(sizeof(struct cli_style_entry) == 16);

#define CLI_STYLE(sequence) {sequence, sizeof(sequence) - 1}
static constexpr struct cli_style_entry cli_style_table[CLI_STYLE_COUNT] = {
	[CLI_STYLE_RESET]      = CLI_STYLE("\033[0m"),
	[CLI_STYLE_INFO]       = CLI_STYLE("\033[0;35m"),
	[CLI_STYLE_PROMPT]     = CLI_STYLE("\033[0;33m"),
	[CLI_STYLE_REASONING]  = CLI_STYLE("\033[0;90m"),
	[CLI_STYLE_USER_INPUT] = CLI_STYLE("\033[0;1;32m"),
	[CLI_STYLE_ERROR]      = CLI_STYLE("\033[0;1;31m"),
};
#undef CLI_STYLE

struct cstr_view
cli_style_sequence (enum cli_style style)
{
	if (style >= CLI_STYLE_COUNT)
		return (struct cstr_view){};
	return (struct cstr_view){cli_style_table[style].bytes, cli_style_table[style].length};
}
