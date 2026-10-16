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
	"\t\t\t\t\"file\":{\"type\":\"string\"},"
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
	"\t\t{\"name\":\"notes\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
	"\t\t\t\"action\":{\"type\":\"string\",\"enum\":[\"add\",\"list\",\"read\",\"copy\",\"attach\",\"notebooks\"]},"
	"\t\t\t\"file\":{\"type\":\"string\"},\"notebook\":{\"type\":\"string\"}}}},"
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
	static struct autocomplete_text const own[] = {
		NAME("findings/locks.md"), NAME("design docs/first.md"), NAME("äiti.md"),
		NAME("o'brien.md"), NAME("findings/locks.md"), NAME("bad\x1b[31m"),
		NAME("bad\xc2\x9b"), NAME("bad\xc0\xaf"), NAME("bad\0name")
	};
	static struct autocomplete_text const other[] = {NAME("model/thoughts.md")};
	static struct autocomplete_text const peer[] = {NAME("reviews/reply.md")};
	struct autocomplete_text const tool = NAME("notes");
	struct autocomplete_text const empty = NAME("");
	struct autocomplete_text const opposite = NAME("other");
#undef NAME
	error = autocomplete_set_participants(autocomplete, participants,
	                                      sizeof participants / sizeof *participants);
	if (error)
		return error;
	error = autocomplete_set_notebooks(autocomplete, notebooks, sizeof notebooks / sizeof *notebooks);
	if (!error)
		error = autocomplete_set_note_files(autocomplete, tool, empty, own, sizeof own / sizeof *own);
	if (!error)
		error = autocomplete_set_note_files(autocomplete, tool, opposite, other, sizeof other / sizeof *other);
	if (!error)
		error = autocomplete_set_note_files(autocomplete, tool, notebooks[1], peer, sizeof peer / sizeof *peer);
	return error;
}
