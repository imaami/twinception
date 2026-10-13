/** @file A plugin for the tests (tests/askai.py).  It has one tool,
 *  test_echo.  The output of a call is the value of the call's string
 *  argument "text". */
#include <json-c/json.h>

#include "plugin.h"

PLUGIN_MANIFEST(.name = "test", .tools = { "test_echo" });

/* Returns the definition of test_echo as a new reference, or nullptr if
 * json-c cannot allocate memory. */
static struct json_object *
test_plugin_echo_definition (void const *inst)
{
	(void)inst;
	return json_tokener_parse(
		"{\"name\":\"test_echo\","
		"\"description\":\"Returns the text argument unchanged.\","
		"\"inputSchema\":{\"type\":\"object\","
		"\"properties\":{\"text\":{\"type\":\"string\"}},"
		"\"required\":[\"text\"],"
		"\"additionalProperties\":false}}");
}

/* Ends the call before it returns.  The output is the value of the string
 * argument "text".  The call fails if "text" is missing or is not a
 * string. */
static void
test_plugin_echo_start (struct mcp_slot    *slot,
                        struct json_object *args)
{
	struct json_object *text;
	if (!json_object_object_get_ex(args, "text", &text) ||
	    !json_object_is_type(text, json_type_string)) {
		mcp_slot_tell(slot, TOOL_TEXT("The text argument must be a string."),
		              TOOL_FAILED);
		return;
	}
	/* mcp_slot_write() copies the bytes, so nothing reads args after the
	 * call has ended. */
	struct tool_text const out = {
		json_object_get_string(text),
		(size_t)json_object_get_string_len(text)
	};
	size_t taken;
	int e = mcp_slot_write(slot, out, 0, &taken);
	if (e)
		mcp_slot_fail(slot, e);
	else
		mcp_slot_finish(slot, 0);
}

struct plugin const askai_plugin = {
	.tools = {{
		.definition = test_plugin_echo_definition,
		.start = test_plugin_echo_start,
		.flags = TOOL_LENT
	}}
};
