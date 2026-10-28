/** @file CLI command spellings, syntax, completion and execution policy. */
#ifndef TWINCEPTION_SRC_CLI_COMMAND_H_
#define TWINCEPTION_SRC_CLI_COMMAND_H_

#include <stdint.h>

#include "cstr_view.h"

/** Keep commands in C-locale order: lookup narrows a sorted prefix range. */
#define CLI_COMMANDS(X) \
        X(CLEAR, clear, NONE, INTERRUPT, "", "starts a new conversation;") \
        X(EXIT, exit, NONE, LOCAL, "", "leaves the chat.") \
        X(HELP, help, NONE, LOCAL, "", "lists commands and editing keys.") \
        X(HISTORY, history, NONE, LOCAL, "", "displays the completed conversation.") \
        X(NAMES, names, NONE, LOCAL, "", "lists known session participants.") \
        X(QUIT, quit, NONE, LOCAL, "", "leaves the chat.") \
        X(READ, read, FILE, WAIT, " FILE", "sends a text file.") \
        X(REFRESH, refresh, NONE, WAIT, "", "updates tool and participant discovery.") \
        X(REGEN, regen, NONE, INTERRUPT, "", "retries the last turn.") \
        X(TOOL, tool, TOOL_CALL, CONCURRENT, " NAME key=VALUE ... | {JSON}", "calls a tool directly; results stay out of chat history.") \
        X(TOOLS, tools, TOOL, LOCAL, " [NAME]", "lists accessible tools or shows a tool's schema.")

enum cli_command_id {
#define CLI_COMMAND_ID(id, word, args, policy, syntax, description) CLI_COMMAND_##id,
	CLI_COMMANDS(CLI_COMMAND_ID)
#undef CLI_COMMAND_ID
	CLI_COMMAND_COUNT,
};

enum cli_command_args {
	CLI_COMMAND_ARGS_NONE,
	CLI_COMMAND_ARGS_FILE,
	CLI_COMMAND_ARGS_TOOL,
	CLI_COMMAND_ARGS_TOOL_CALL,
};

enum cli_command_policy : uint64_t {
	CLI_COMMAND_LOCAL,
	CLI_COMMAND_WAIT,
	CLI_COMMAND_INTERRUPT,
	CLI_COMMAND_CONCURRENT,
};

struct cli_command {
	struct cstr_view        name;
	struct cstr_view        completion;
	struct cstr_view        usage;
	struct cstr_view        description;
	enum cli_command_policy policy;
	enum cli_command_id     id;
	enum cli_command_args   args;
};

struct cli_command_matches {
	struct cli_command const *data;
	size_t                    count;
};

/** Returns a process-lifetime descriptor, or nullptr for an invalid id. */
extern struct cli_command const *
cli_command_at (enum cli_command_id id);

/** Exact counted token lookup. Arguments and surrounding whitespace are
 * the caller's responsibility. Embedded NUL bytes never match. */
extern struct cli_command const *
cli_command_find (struct cstr_view token);

/** Returns a borrowed contiguous range in lexical order. An empty prefix
 * selects every command. No allocation and no rescanning shared prefixes. */
extern struct cli_command_matches
cli_command_prefix (struct cstr_view prefix);

#endif /* TWINCEPTION_SRC_CLI_COMMAND_H_ */
