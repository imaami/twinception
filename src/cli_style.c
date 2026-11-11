#include "cli_style.h"

static_assert(sizeof(struct cli_style_entry) == 16);

static struct cli_style_entry const cli_style_none[CLI_STYLE_COUNT] = {};

/* A sequence and its terminating NUL fit the array bytes of an entry. */
#define CLI_STYLE_FITS(id, sequence) \
        static_assert(sizeof(sequence) <= sizeof cli_style_none->bytes);
CLI_STYLE_LIST(CLI_STYLE_FITS)
#undef CLI_STYLE_FITS

static struct cli_style_entry const cli_style_table[CLI_STYLE_COUNT] = {
#define CLI_STYLE_ENTRY(id, sequence) [CLI_STYLE_##id] = { sequence, sizeof(sequence) - 1 },
	CLI_STYLE_LIST(CLI_STYLE_ENTRY)
#undef CLI_STYLE_ENTRY
};

struct cli_style_entry const *
cli_style_palette (bool color)
{
	return color ? cli_style_table : cli_style_none;
}
