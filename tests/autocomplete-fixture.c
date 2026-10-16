/** @file Shared metadata for autocomplete unit and terminal tests. */

#include <errno.h>
#include <json-c/json.h>

#include "autocomplete-fixture.h"

int autocomplete_fixture (struct autocomplete *autocomplete)
{
	static char const definitions[] =
	"["
	"\t\t{\"name\":\"git\",\"description\":\"Inspect a repository\",\"inputSchema\":{"
	"\t\t\t\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
	"\t\t\t\t\"command\":{\"type\":\"string\",\"enum\":[\"log\",\"status\"]},"
	"\t\t\t\t\"repo\":{\"type\":\"string\",\"enum\":[\"project\",\"other project\"]},"
	"\t\t\t\t\"recursive\":{\"type\":\"boolean\"},\"count\":{\"type\":\"integer\"},"
	"\t\t\t\t\"message\":{\"type\":\"string\"},\"participant\":{\"type\":\"string\"},"
	"\t\t\t\t\"notebook\":{\"type\":\"string\"},"
	"\t\t\t\t\"options\":{\"type\":\"array\"},\"config\":{\"type\":\"object\"}}}},"
	"\t\t{\"name\":\"lookup\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
	"\t\t\t\"query\":{\"type\":\"string\"},\"mode\":{\"const\":\"exact\"},"
	"\t\t\t\"depth\":{\"$ref\":\"#/$defs/level\"},\"code\":{\"enum\":[\"001\",\"002\"]},"
	"\t\t\t\"mixed\":{\"enum\":[\"true\",\"42\",42]},"
	"\t\t\t\"author\":{\"const\":null},"
	"\t\t\t\"structured\":{\"enum\":[{\"depth\":2},[1,2]]}},"
	"\t\t\t\"$defs\":{\"level\":{\"type\":\"integer\",\"enum\":[1,2]}}}},"
	"\t\t{\"name\":\"required\",\"inputSchema\":{\"type\":\"object\",\"required\":[\"text\"],"
	"\t\t\t\"properties\":{\"text\":{\"type\":[\"string\",\"null\"]}}}},"
	"\t\t{\"name\":\"help\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
	"\t\t{\"name\":\"bad\\u001b[31m\",\"inputSchema\":{\"type\":\"object\"}}"
	"\t]";
	struct json_object *tools = json_tokener_parse(definitions);
	if (!tools)
		return ENOMEM;
	int error = 0;
	size_t count = json_object_array_length(tools);
	for (size_t i = 0; i < count && !error; ++i)
		error = autocomplete_add_tool(autocomplete, json_object_array_get_idx(tools, i));
	json_object_put(tools);
	tools = nullptr;
	if (error)
		return error;

#define NAME(text) {text, sizeof(text) - 1}
	static struct autocomplete_text const participants[] = {
		NAME("alice"), NAME("bob"), NAME("äiti"), NAME("alice"),
		NAME("bad\x1b[31m"), NAME("bad\xc2\x9b"), NAME("bad\xc0\xaf"),
		NAME("bad\0name")
	};
	static struct autocomplete_text const notebooks[] = {
		NAME("alice/anonymous/client"), NAME("bob/0123456789abcdef/upstream")
	};
#undef NAME
	error = autocomplete_set_participants(autocomplete, participants,
	                                      sizeof participants / sizeof *participants);
	if (error)
		return error;
	return autocomplete_set_notebooks(autocomplete, notebooks, sizeof notebooks / sizeof *notebooks);
}
