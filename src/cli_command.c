#include "cli_command.h"

#define CLI_COMMAND_ENTRY(id, word, args, policy, syntax, description) \
	{CSTR_VIEW("/" #word), CSTR_VIEW("/" #word " "), \
	 CSTR_VIEW("/" #word syntax), CSTR_VIEW(description), \
	 CLI_COMMAND_##policy, CLI_COMMAND_##id, CLI_COMMAND_ARGS_##args},
static struct cli_command const cli_command_table[] = {
	CLI_COMMANDS(CLI_COMMAND_ENTRY)
};
#undef CLI_COMMAND_ENTRY

struct cli_command const *
cli_command_at (enum cli_command_id id)
{
	return (unsigned)id < CLI_COMMAND_COUNT ? &cli_command_table[id] : nullptr;
}

static unsigned char
cli_command_byte (struct cli_command const *command,
                  size_t                    offset)
{
	return offset < command->name.len ? (unsigned char)command->name.data[offset] : 0;
}

struct cli_command_matches
cli_command_prefix (struct cstr_view prefix)
{
	size_t first = 0, end = CLI_COMMAND_COUNT;
	for (size_t offset = 0; offset < prefix.len && first < end; ++offset) {
		unsigned char byte = (unsigned char)prefix.data[offset];
		if (!byte)
			return (struct cli_command_matches){};
		/* Earlier bytes already matched. Only inspect the next byte while
		 * shrinking the same interval; its boundaries move at most COUNT
		 * times over the entire lookup. */
		while (first < end && cli_command_byte(&cli_command_table[first], offset) < byte)
			++first;
		while (first < end && cli_command_byte(&cli_command_table[end - 1], offset) > byte)
			--end;
	}
	return (struct cli_command_matches){cli_command_table + first, end - first};
}

struct cli_command const *
cli_command_find (struct cstr_view token)
{
	struct cli_command_matches matches = cli_command_prefix(token);
	return matches.count && matches.data->name.len == token.len ? matches.data : nullptr;
}
