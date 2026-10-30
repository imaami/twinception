/** @file cli_tool.c
 *
 * Manual tool request ownership and presentation.
 */
#include <errno.h>
#include <stdlib.h>

#include "cli_client.h"
#include "cli_stream.h"
#include "cli_tool.h"
#include "cli_ui.h"
#include "compat.h"
#include "jb.h"
#include "list.h"

enum { CLI_TOOL_WRITES_NOTES = 1u << 0 };

struct cli_tool {
	struct cli_stream   stream;
	struct list         hook;
	struct cli_client  *client;
	struct cli_console *console;
	struct json_object *name;
	struct json_object *params;
	cli_tool_done_fn   *done;
	void               *context;
	uint64_t            request;
	uint64_t            flags;
};

static struct cstr_view
cli_tool_name (struct cli_tool const *tool)
{
	return cli_client_text(tool->name);
}

struct cli_tool *
cli_tool_create (struct cli_client  *client,
                 struct cli_console *console,
                 struct cstr_view    command,
                 struct cli_error   *error,
                 int                *e)
{
	struct autocomplete_tool const *entry = nullptr;
	cli_json_auto arguments = nullptr;
	struct autocomplete_error diagnostic = {error->message, sizeof error->message, 0};
	*e = autocomplete_parse_call(client->catalog, command, &entry, &arguments, &diagnostic);
	if (*e)
		return nullptr;
	uint64_t flags = 0;
	if (cstr_view_equal(entry->name, cstr_view_dstr(&client->notes_tool))) {
		struct json_object *action = jb_get(arguments, "action");
		if (cli_client_is(action, "add") || cli_client_is(action, "copy") || cli_client_is(action, "replace"))
			flags |= CLI_TOOL_WRITES_NOTES;
	}
	struct cstr_view token = CSTR_VIEW("tool");
	cli_json_auto params = jb_object("name", cli_client_string(entry->name),
	                                 "arguments", json_object_get(arguments),
	                                 "_meta", jb_object("progressToken",
	                                                    cli_client_string(token), nullptr), nullptr);
	struct cli_tool *tool = params ? malloc(sizeof *tool) : nullptr;
	*e = tool ? 0 : ENOMEM;
	if (!tool)
		return nullptr;
	*tool = (struct cli_tool){.client = client, .console = console,
	                          .name = json_object_get(jb_get(params, "name")),
	                          .params = json_object_get(params), .flags = flags};
	list_init(&tool->hook);
	/* The stream views the name that the tool owns. */
	tool->stream = cli_stream(console, cli_tool_name(tool), token, 0);
	return tool;
}

void
cli_tool_destroy (struct cli_tool **pp)
{
	if (!pp || !*pp)
		return;
	struct cli_tool *tool = *pp;
	*pp = nullptr;
	list_del(&tool->hook);
	cli_stream_fini(&tool->stream);
	json_object_put(tool->name);
	tool->name = nullptr;
	json_object_put(tool->params);
	tool->params = nullptr;
	free(tool);
}

static int
cli_tool_begin (struct cli_tool *tool)
{
	tool->client->notes_dirty |= (tool->flags & CLI_TOOL_WRITES_NOTES) != 0;
	struct cstr_view name = cli_tool_name(tool);
	dstr_auto action = dstr("call ");
	if (!dstr_cat_n(&action, name.data, name.len))
		return ENOMEM;
	return cli_stream_action(&tool->stream, CSTR_VIEW("you"), cstr_view_dstr(&action));
}

static int
cli_tool_content (struct cli_tool    *tool,
                  struct json_object *result,
                  struct cli_error   *error)
{
	struct json_object *content = jb_get(result, "content");
	if (json_object_is_type(content, json_type_array)) {
		for (size_t i = 0, count = json_object_array_length(content); i < count; ++i) {
			struct json_object *block = jb_at(content, i);
			struct cstr_view kind = cli_client_text(jb_get(block, "type"));
			if (!kind.data)
				return cli_error_set(error, EPROTO, "Malformed tool content");
			int e;
			if (cstr_view_equal(kind, CSTR_VIEW("text"))) {
				struct cstr_view text = cli_client_text(jb_get(block, "text"));
				if (!text.data)
					return cli_error_set(error, EPROTO, "Malformed tool content");
				e = cli_ui_message(tool->console, text);
			} else {
				dstr_auto label = dstr("[");
				if (!dstr_cat_n(&label, kind.data, kind.len) || !dstr_cat(&label, " content]"))
					return ENOMEM;
				e = cli_ui_message(tool->console, cstr_view_dstr(&label));
			}
			if (e)
				return e;
		}
		return 0;
	}
	struct json_object *structured = jb_get(result, "structuredContent");
	if (!structured)
		return cli_error_set(error, EPROTO, "AskAI returned no tool content");
	struct cstr_view wire;
	int e = cli_client_wire(structured, JSON_C_TO_STRING_PRETTY, &wire, error);
	return e ? e : cli_ui_message(tool->console, wire);
}

static int
cli_tool_result (struct cli_tool    *tool,
                 struct json_object *result,
                 struct cli_error   *error)
{
	int e = cli_tool_content(tool, result, error);
	struct cstr_view name = cli_tool_name(tool);
	if (!e && json_object_get_boolean(jb_get(result, "isError")))
		e = cli_error_set(error, EIO, "Tool %.*s failed", (int)name.len, name.data);
	return e ? e : cli_stream_action(&tool->stream, name, CSTR_VIEW("completed"));
}

int
cli_tool_finish (struct cli_tool        *tool,
                 int                     e,
                 struct json_object     *result,
                 struct cli_error const *error)
{
	struct cli_error detail = *error;
	if (!e)
		e = cli_tool_result(tool, result, &detail);
	return e ? cli_ui_error(tool->console, cli_tool_name(tool), cli_error_text(&detail, e)) : 0;
}

int
cli_tool_run (struct cli_tool  *tool,
              struct cli_error *error)
{
	int e = cli_tool_begin(tool);
	cli_json_auto result = nullptr;
	if (!e)
		e = cli_client_rpc(tool->client, "tools/call", json_object_get(tool->params),
		                   cli_stream_notify, &tool->stream, &result, error);
	return e ? e : cli_tool_result(tool, result, error);
}

static void
cli_tool_done (void                   *context,
               int                     e,
               struct json_object     *result,
               struct cli_error const *error)
{
	struct cli_tool *tool = container_of(context, struct cli_tool, stream);
	tool->request = 0;
	tool->client->notes_dirty |= (tool->flags & CLI_TOOL_WRITES_NOTES) != 0;
	list_del(&tool->hook);
	tool->done(tool, tool->context, e, result, error);
	cli_tool_destroy(&tool);
}

int
cli_tool_submit (struct cli_tool  *tool,
                 struct list      *active,
                 cli_tool_done_fn *done,
                 void             *context,
                 struct cli_error *error)
{
	if (!done || !active)
		return EINVAL;
	int e = cli_tool_begin(tool);
	if (!e)
		e = cli_client_submit(tool->client, "tools/call", json_object_get(tool->params),
		                      cli_stream_notify, cli_tool_done, &tool->stream, &tool->request, error);
	if (!e) {
		cli_client_json_cleanup(&tool->params);
		tool->done = done;
		tool->context = context;
		list_append(active, &tool->hook);
	}
	return e;
}

void
cli_tool_cancel_last (struct list *active)
{
	if (!list_is_empty(active)) {
		struct cli_tool *tool = container_of(active->prev, struct cli_tool, hook);
		cli_client_cancel_request(tool->client, tool->request);
	}
}
