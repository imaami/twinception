/** @file Shared metadata for autocomplete unit and terminal tests. */

#include <errno.h>
#include <json-c/json.h>

#include "autocomplete_fixture.h"

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

	static struct cstr_view const participants[] = {
		CSTR_VIEW("alice"), CSTR_VIEW("bob"), CSTR_VIEW("äiti"), CSTR_VIEW("alice"),
		CSTR_VIEW("bad\x1b[31m"), CSTR_VIEW("bad\xc2\x9b"), CSTR_VIEW("bad\xc0\xaf"),
		CSTR_VIEW("bad\0name")
	};
	static struct cstr_view const notebooks[] = {
		CSTR_VIEW("alice/anonymous/client"), CSTR_VIEW("bob/0123456789abcdef/upstream")
	};
	static struct cstr_view const own[] = {
		CSTR_VIEW("findings/locks.md"), CSTR_VIEW("design docs/first.md"), CSTR_VIEW("äiti.md"),
		CSTR_VIEW("o'brien.md"), CSTR_VIEW("findings/locks.md"), CSTR_VIEW("bad\x1b[31m"),
		CSTR_VIEW("bad\xc2\x9b"), CSTR_VIEW("bad\xc0\xaf"), CSTR_VIEW("bad\0name")
	};
	static struct cstr_view const other[] = {CSTR_VIEW("model/thoughts.md")};
	static struct cstr_view const peer[] = {CSTR_VIEW("reviews/reply.md")};
	struct cstr_view const tool = CSTR_VIEW("notes");
	struct cstr_view const empty = CSTR_VIEW("");
	struct cstr_view const opposite = CSTR_VIEW("other");
	error = autocomplete_set_participants(autocomplete, participants,
	                                      sizeof participants / sizeof *participants);
	if (!error)
		error = autocomplete_set_chat_participants(autocomplete, participants,
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
