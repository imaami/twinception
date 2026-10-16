/** @file Shared metadata for completion unit and terminal tests. */
#pragma once

#include <json-c/json.h>

#include "chat-completion.h"

inline void chat_completion_fixture(chat_completion::catalog &catalog)
{
	json_object *tools = json_tokener_parse(R"json([
		{"name":"git","description":"Inspect a repository","inputSchema":{
			"type":"object","additionalProperties":false,"properties":{
				"command":{"type":"string","enum":["log","status"]},
				"repo":{"type":"string","enum":["project","other project"]},
				"recursive":{"type":"boolean"},"count":{"type":"integer"},
				"message":{"type":"string"},"participant":{"type":"string"},
				"notebook":{"type":"string"},
				"options":{"type":"array"},"config":{"type":"object"}}}},
		{"name":"lookup","inputSchema":{"type":"object","properties":{
			"query":{"type":"string"},"mode":{"const":"exact"},
			"depth":{"$ref":"#/$defs/level"},"code":{"enum":["001","002"]},
			"mixed":{"enum":["true","42",42]},
			"author":{"const":null},
			"structured":{"enum":[{"depth":2},[1,2]]}},
			"$defs":{"level":{"type":"integer","enum":[1,2]}}}},
		{"name":"required","inputSchema":{"type":"object","required":["text"],
			"properties":{"text":{"type":["string","null"]}}}},
		{"name":"help","inputSchema":{"type":"object","properties":{}}},
		{"name":"bad\u001b[31m","inputSchema":{"type":"object"}}
	])json");
	for (size_t i = 0; i < json_object_array_length(tools); ++i)
		catalog.add_tool(json_object_array_get_idx(tools, i));
	json_object_put(tools);
	tools = nullptr;
	catalog.set_participants({"alice", "bob", "äiti", "alice", "bad\x1b[31m",
	                          "bad\xc2\x9b", "bad\xc0\xaf"});
	catalog.set_notebooks({"alice/anonymous/client", "bob/0123456789abcdef/upstream"});
}
