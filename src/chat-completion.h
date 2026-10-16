/** @file Connection-owned, offline completion of MCP tools and session names. */
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct json_object;

namespace chat_completion {

struct tool {
	std::string name, title, description;
	std::shared_ptr<json_object> schema;
	size_t storage = 0;
};

/** Discovery replaces these snapshots between prompts; Tab never performs I/O
 * other than local path completion. Schemas retain their own JSON references. */
class catalog {
public:
	void clear_tools();
	void add_tool(json_object *entry);
	void set_participants(std::vector<std::string> names);
	void set_notebooks(std::vector<std::string> names);
	const std::vector<tool> &tools() const { return entries; }
	const std::vector<std::string> &participants() const { return names; }
	const tool *find_tool(std::string_view name) const;
	std::vector<std::pair<std::string, size_t>> complete(std::string_view line, size_t cursor) const;
	/** Parses /tool NAME key=VALUE ... or /tool NAME {JSON}. On success the
	 * caller owns arguments; on failure it is null and invalid_argument is thrown.
	 * This coerces property types, but the server remains the schema validator. */
	void parse_call(std::string_view line, std::string &name, json_object *&arguments) const;
private:
	std::vector<tool> entries;
	std::vector<std::string> names, notebooks;
	size_t storage = 0;
};

}
