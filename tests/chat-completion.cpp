/** @file Exercise completion and invocation parsing without a terminal. */

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

#include "chat-completion-fixture.h"

static void chat_completion_has(const chat_completion::catalog &catalog,
                                std::string_view input, std::string_view expected,
                                size_t cursor = std::string_view::npos)
{
	if (cursor == std::string_view::npos)
		cursor = input.size();
	auto matches = catalog.complete(input, cursor);
	for (const auto &[text, point] : matches) {
		assert(point <= text.size());
		std::string_view trimmed(text);
		while (!trimmed.empty() && trimmed.back() == ' ')
			trimmed.remove_suffix(1);
		if (trimmed == expected)
			return;
	}
	std::fprintf(stderr, "completion missing: [%.*s] -> [%.*s]\n",
	             static_cast<int>(input.size()), input.data(),
	             static_cast<int>(expected.size()), expected.data());
	for (const auto &[text, point] : matches)
		std::fprintf(stderr, "  [%s], point %zu\n", text.c_str(), point);
	std::abort();
}

static json_object *chat_completion_parse(const chat_completion::catalog &catalog,
                                          std::string_view input, const char *expected_name)
{
	std::string name;
	json_object *arguments = nullptr;
	catalog.parse_call(input, name, arguments);
	assert(name == expected_name);
	assert(arguments && json_object_is_type(arguments, json_type_object));
	return arguments;
}

static void chat_completion_bad(const chat_completion::catalog &catalog, std::string_view input)
{
	std::string name;
	json_object *arguments = nullptr;
	try {
		catalog.parse_call(input, name, arguments);
	} catch (const std::invalid_argument &) {
		assert(!arguments);
		return;
	}
	json_object_put(arguments);
	arguments = nullptr;
	std::fprintf(stderr, "invalid tool call accepted: [%.*s]\n",
	             static_cast<int>(input.size()), input.data());
	std::abort();
}

int main()
{
	chat_completion::catalog catalog;
	chat_completion_fixture(catalog);
	chat_completion_has(catalog, "/to", "/tool");
	chat_completion_has(catalog, "/tool gi", "/tool git");
	chat_completion_has(catalog, "/tool git com", "/tool git command=");
	chat_completion_has(catalog, "/tool git command=lo", "/tool git command=log");
	chat_completion_has(catalog, "/tool git recursive=t", "/tool git recursive=true");
	chat_completion_has(catalog, "/tool git participant=al", "/tool git participant=alice");
	chat_completion_has(catalog, "/tool git notebook=ali", "/tool git notebook=alice/anonymous/client");
	chat_completion_has(catalog, "/tool lookup mode=ex", "/tool lookup mode=exact");
	chat_completion_has(catalog, "/tool lookup depth=1", "/tool lookup depth=1");
	chat_completion_has(catalog, "/tool lookup mixed=t", "/tool lookup mixed='true'");
	chat_completion_has(catalog, "/tool lookup mixed=4", "/tool lookup mixed='42'");
	chat_completion_has(catalog, "/tool lookup mixed=4", "/tool lookup mixed=42");
	chat_completion_has(catalog, "/tool lookup author=", "/tool lookup author=null");
	assert(catalog.complete("/tool lookup author=al", 22).empty());
	chat_completion_has(catalog, "/tool lookup structured=", "/tool lookup structured={\"depth\":2}");
	chat_completion_has(catalog, "/tool lookup structured=", "/tool lookup structured=[1,2]");
	chat_completion_has(catalog, "/tool git repo=other", "/tool git repo='other project'");
	chat_completion_has(catalog, "/tool git repo=\"other", "/tool git repo=\"other project\"");
	chat_completion_has(catalog, "@al", "@alice");
	chat_completion_has(catalog, "hello @al", "hello @alice");
	chat_completion_has(catalog, "hello @al, next", "hello @alice, next", 9);
	chat_completion_has(catalog, "al: hi", "alice: hi", 2);
	chat_completion_has(catalog, "ä", "äiti:");
	chat_completion_has(catalog, "/tool git command=stale count=2",
	                    "/tool git command=status count=2", 20);
	assert(catalog.complete("/tool absent ", 13).empty());
	assert(catalog.complete("/tool git recipient=", 20).empty());
	assert(catalog.complete("/tool git command=log com", 25).empty());
	assert(catalog.complete("x", 2).empty());
	assert(catalog.participants().size() == 3);
	for (const auto &[text, point] : catalog.complete("/tool ", 6)) {
		(void)point;
		assert(text.find('\x1b') == std::string::npos);
	}
	for (const auto &[text, point] : catalog.complete("@", 1)) {
		(void)point;
		assert(text.find('\x1b') == std::string::npos);
	}

	json_object *arguments = chat_completion_parse(catalog,
		"/tool git command=log recursive=true count=42 message='hello world' "
		"options='[\"--oneline\"]' config='{\"depth\":2}'", "git");
	json_object *value = nullptr;
	assert(json_object_object_get_ex(arguments, "recursive", &value));
	assert(json_object_is_type(value, json_type_boolean) && json_object_get_boolean(value));
	assert(json_object_object_get_ex(arguments, "count", &value));
	assert(json_object_is_type(value, json_type_int) && json_object_get_int(value) == 42);
	assert(json_object_object_get_ex(arguments, "message", &value));
	assert(std::string_view(json_object_get_string(value)) == "hello world");
	assert(json_object_object_get_ex(arguments, "options", &value));
	assert(json_object_is_type(value, json_type_array) && json_object_array_length(value) == 1);
	assert(json_object_object_get_ex(arguments, "config", &value));
	assert(json_object_is_type(value, json_type_object));
	json_object_put(arguments);
	arguments = nullptr;

	arguments = chat_completion_parse(catalog, "/tool git message=true", "git");
	assert(json_object_object_get_ex(arguments, "message", &value));
	assert(json_object_is_type(value, json_type_string));
	assert(std::string_view(json_object_get_string(value)) == "true");
	json_object_put(arguments);
	arguments = nullptr;
	arguments = chat_completion_parse(catalog,
		"/tool git {\"message\":\"hello \\\"world\\\"\",\"options\":[1,null]}", "git");
	assert(json_object_object_get_ex(arguments, "message", &value));
	assert(std::string_view(json_object_get_string(value)) == "hello \"world\"");
	json_object_put(arguments);
	arguments = nullptr;
	arguments = chat_completion_parse(catalog, "/tool git repo=other\\ project", "git");
	assert(json_object_object_get_ex(arguments, "repo", &value));
	assert(std::string_view(json_object_get_string(value)) == "other project");
	json_object_put(arguments);
	arguments = nullptr;
	arguments = chat_completion_parse(catalog, "/tool lookup code=001 depth=2", "lookup");
	assert(json_object_object_get_ex(arguments, "code", &value));
	assert(json_object_is_type(value, json_type_string));
	assert(std::string_view(json_object_get_string(value)) == "001");
	assert(json_object_object_get_ex(arguments, "depth", &value));
	assert(json_object_get_int(value) == 2);
	json_object_put(arguments);
	arguments = nullptr;
	arguments = chat_completion_parse(catalog, "/tool required {\"text\":null}", "required");
	assert(json_object_object_get_ex(arguments, "text", &value) && !value);
	json_object_put(arguments);
	arguments = nullptr;
	arguments = chat_completion_parse(catalog, "/tool required text=null", "required");
	assert(json_object_object_get_ex(arguments, "text", &value) && !value);
	json_object_put(arguments);
	arguments = nullptr;
	arguments = chat_completion_parse(catalog, "/tool required text='null'", "required");
	assert(json_object_object_get_ex(arguments, "text", &value));
	assert(json_object_is_type(value, json_type_string));
	assert(std::string_view(json_object_get_string(value)) == "null");
	json_object_put(arguments);
	arguments = nullptr;
	// Raw JSON is the escape hatch for schemas that shorthand cannot interpret;
	// the server owns validation of these arguments.
	arguments = chat_completion_parse(catalog, "/tool git {\"extension\":true}", "git");
	assert(json_object_object_get_ex(arguments, "extension", &value));
	assert(json_object_get_boolean(value));
	json_object_put(arguments);
	arguments = nullptr;
	for (const auto &[text, point] : catalog.complete("/tool lookup mixed=", 19)) {
		(void)point;
		arguments = chat_completion_parse(catalog, text, "lookup");
		assert(json_object_object_get_ex(arguments, "mixed", &value));
		if (text.find('\'') != std::string::npos)
			assert(json_object_is_type(value, json_type_string));
		else
			assert(json_object_is_type(value, json_type_int) && json_object_get_int(value) == 42);
		json_object_put(arguments);
		arguments = nullptr;
	}
	for (const auto &[text, point] : catalog.complete("/tool lookup structured=", 24)) {
		(void)point;
		arguments = chat_completion_parse(catalog, text, "lookup");
		assert(json_object_object_get_ex(arguments, "structured", &value));
		if (text.find('{') != std::string::npos)
			assert(json_object_is_type(value, json_type_object));
		else
			assert(json_object_is_type(value, json_type_array));
		json_object_put(arguments);
		arguments = nullptr;
	}
	for (const char *input : {
		"/tool absent", "/tool git recursive=maybe", "/tool git count=1.5",
		"/tool git message='unfinished", "/tool git command=log command=status",
		"/tool git unknown=1", "/tool git options=no", "/tool git config=[]",
		"/tool git {\"message\":\"hello\"} trailing", "/tool git message", "/tool required"})
		chat_completion_bad(catalog, input);

	chat_completion::catalog other;
	other.set_participants({"carol"});
	assert(other.complete("/tool gi", 8).empty());
	assert(other.complete("@al", 3).empty());
	chat_completion_has(other, "@ca", "@carol");
	catalog.clear_tools();
	assert(catalog.complete("/tool gi", 8).empty());
	chat_completion_has(catalog, "@al", "@alice");
	std::puts("chat completion: tools, schema values, names, cursor edits, typed calls and isolation: PASS");
}
