#include "cli_style.h"

static_assert(sizeof(struct cli_style_entry) == 16);

static constexpr struct cli_style_entry cli_style_none[CLI_STYLE_COUNT] = {};

#define CLI_STYLE(sequence) {sequence, sizeof(sequence) - 1}
static constexpr struct cli_style_entry cli_style_table[CLI_STYLE_COUNT] = {
	[CLI_STYLE_RESET]                = CLI_STYLE("\033[0m"),
	[CLI_STYLE_INFO]                 = CLI_STYLE("\033[0;38;5;110m"),
	[CLI_STYLE_PROMPT]               = CLI_STYLE("\033[0;38;5;229m"),
	[CLI_STYLE_REASONING]            = CLI_STYLE("\033[0;38;5;146m"),
	[CLI_STYLE_USER_INPUT]           = CLI_STYLE("\033[0;38;5;231m"),
	[CLI_STYLE_ERROR]                = CLI_STYLE("\033[0;38;5;203m"),
	[CLI_STYLE_TEXT]                 = CLI_STYLE("\033[0;38;5;252m"),
	[CLI_STYLE_FRAME]                = CLI_STYLE("\033[0;38;5;75m"),
	[CLI_STYLE_TITLE]                = CLI_STYLE("\033[0;38;5;229m"),
	[CLI_STYLE_TITLE_INACTIVE]       = CLI_STYLE("\033[0;38;5;153m"),
	[CLI_STYLE_STATUS]               = CLI_STYLE("\033[0;38;5;110m"),
	[CLI_STYLE_TOOL_ACTOR]           = CLI_STYLE("\033[0;38;5;215m"),
	[CLI_STYLE_TOOL_STATUS]          = CLI_STYLE("\033[0;38;5;180m"),
	[CLI_STYLE_PROGRESS]             = CLI_STYLE("\033[0;38;5;109m"),
	[CLI_STYLE_COMPLETION_COMMAND]   = CLI_STYLE("\033[0;38;5;81m"),
	[CLI_STYLE_COMPLETION_TOOL]      = CLI_STYLE("\033[0;38;5;114m"),
	[CLI_STYLE_COMPLETION_PARAMETER] = CLI_STYLE("\033[0;38;5;222m"),
	[CLI_STYLE_COMPLETION_VALUE]     = CLI_STYLE("\033[0;38;5;153m"),
	[CLI_STYLE_COMPLETION_NAME]      = CLI_STYLE("\033[0;38;5;215m"),
	[CLI_STYLE_COMPLETION_FILE]      = CLI_STYLE("\033[0;38;5;150m"),
};
#undef CLI_STYLE

struct cstr_view
cli_style_sequence (enum cli_style style)
{
	if (style >= CLI_STYLE_COUNT)
		return (struct cstr_view){};
	return (struct cstr_view){cli_style_table[style].bytes, cli_style_table[style].length};
}

struct cli_style_entry const *
cli_style_palette (bool color)
{
	return color ? cli_style_table : cli_style_none;
}
