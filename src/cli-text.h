/** @file Counted byte views shared by the CLI's transport and terminal. */
#ifndef TWINCEPTION_SRC_CLI_TEXT_H_
#define TWINCEPTION_SRC_CLI_TEXT_H_

#include <stddef.h>
#include <string.h>

#include "dstr.h"

struct cli_text {
	char const *data;
	size_t      len;
};

/** Literal view, excluding its terminating NUL. */
#define CLI_TEXT(s) ((struct cli_text){ (s), sizeof(s) - 1 })

static inline struct cli_text
cli_text_cstr(char const *text)
{
	return (struct cli_text){ text, text ? strlen(text) : 0 };
}

static inline struct cli_text
cli_text_dstr(struct dstr const *text)
{
	return (struct cli_text){ dstr_get(text), dstr_len(text) };
}

static inline bool
cli_text_equal(struct cli_text a, struct cli_text b)
{
	return a.len == b.len && (!a.len || !memcmp(a.data, b.data, a.len));
}

/** Clamp both bounds; an empty view need not have a nonnull pointer. */
static inline struct cli_text
cli_text_slice(struct cli_text text, size_t offset, size_t length)
{
	if (offset > text.len)
		offset = text.len;
	text.len -= offset;
	if (text.data)
		text.data += offset;
	if (text.len > length)
		text.len = length;
	return text;
}

static inline bool
cli_text_starts(struct cli_text text, struct cli_text prefix)
{
	return cli_text_equal(cli_text_slice(text, 0, prefix.len), prefix);
}

#endif /* TWINCEPTION_SRC_CLI_TEXT_H_ */
