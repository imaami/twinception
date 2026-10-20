/** @file Manual tool calls, including their asynchronous request lifetime. */
#ifndef TWINCEPTION_SRC_CLI_TOOL_H_
#define TWINCEPTION_SRC_CLI_TOOL_H_

#include "cstr_view.h"

struct cli_client;
struct cli_console;
struct cli_error;
struct cli_tool;
struct json_object;
struct list;

/** Parse a command and own its request parameters. The client and console
 * are borrowed and must outlive the call. */
extern struct cli_tool *
cli_tool_create (struct cli_client  *client,
                 struct cli_console *console,
                 struct cstr_view    command,
                 struct cli_error   *error,
                 int                *e);

/** Destroy a call that has not transferred ownership to submit. */
extern void
cli_tool_destroy (struct cli_tool **tool);

/** Execute synchronously and display the result. */
extern int
cli_tool_run (struct cli_tool  *tool,
              struct cli_error *error);

/** The call has left the active list. Arguments are borrowed until return;
 * the module destroys the call after this callback. */
typedef void cli_tool_done_fn (struct cli_tool        *tool,
                               void                   *context,
                               int                     e,
                               struct json_object     *result,
                               struct cli_error const *error);

/** Success transfers ownership to the module, links the call into active,
 * and schedules done exactly once. Failure leaves ownership with the caller.
 * The list, callback context, client and console must outlive completion. */
extern int
cli_tool_submit (struct cli_tool  *tool,
                 struct list      *active,
                 cli_tool_done_fn *done,
                 void             *context,
                 struct cli_error *error);

/** Display an asynchronous result or error when the owner permits output. */
extern int
cli_tool_finish (struct cli_tool        *tool,
                 int                     e,
                 struct json_object     *result,
                 struct cli_error const *error);

/** Cancel the newest call in the module-managed active list, if any. */
extern void
cli_tool_cancel_last (struct list *active);

#endif /* TWINCEPTION_SRC_CLI_TOOL_H_ */
