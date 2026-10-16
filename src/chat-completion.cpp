/** @file Completion uses only the active connection's discovery snapshots. */
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <json-c/json.h>

#include "chat-completion.h"

namespace chat_completion {
namespace {

using json_ptr = std::unique_ptr<json_object, decltype(&json_object_put)>;
using matches = std::vector<std::pair<std::string, size_t>>;

json_object *get(json_object *object, const char *key)
{
	json_object *value = nullptr;
	return object && json_object_object_get_ex(object, key, &value) ? value : nullptr;
}

std::string text(json_object *value)
{
	return json_object_is_type(value, json_type_string)
	     ? std::string(json_object_get_string(value), json_object_get_string_len(value)) : "";
}

bool starts(std::string_view value, std::string_view prefix)
{
	return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool name_byte(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_' || c == '-' || c >= 0x80;
}

bool safe(std::string_view value, bool identifier = false)
{
	if (value.empty())
		return false;
	for (size_t i = 0; i < value.size(); ++i) {
		unsigned char c = value[i];
		if (c < 0x20 || c == 0x7f || (identifier && (space(c) || c == '=' || c == '\'' || c == '"' || c == '\\')))
			return false;
		if (c < 0x80)
			continue;
		unsigned count = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
		if (!count || count >= value.size() - i)
			return false;
		unsigned code = c & ((1u << (6 - count)) - 1);
		for (unsigned n = 0; n < count; ++n) {
			unsigned char next = value[++i];
			if ((next & 0xc0) != 0x80)
				return false;
			code = (code << 6) | (next & 0x3f);
		}
		if (code < 0xa0 || (count == 2 && code < 0x800) ||
		    (count == 3 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
			return false;
	}
	return true;
}

void normalize(std::vector<std::string> &values)
{
	values.erase(std::remove_if(values.begin(), values.end(), [](const auto &value) {
		return !safe(value, true);
	}), values.end());
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
}

struct token {
	size_t begin, end;
};

std::vector<token> tokenize(std::string_view line, bool strict)
{
	std::vector<token> result;
	for (size_t i = 0; i < line.size();) {
		if (space(line[i])) {
			++i;
			continue;
		}
		size_t begin = i;
		char quote = 0;
		bool escape = false;
		std::string nesting;
		for (; i < line.size(); ++i) {
			char c = line[i];
			if (escape) {
				escape = false;
				continue;
			}
			if (c == '\\' && quote != '\'') {
				escape = true;
				continue;
			}
			if (quote) {
				if (c == quote)
					quote = 0;
				continue;
			}
			if (c == '\'' || c == '"')
				quote = c;
			else if (c == '[' || c == '{')
				nesting.push_back(c);
			else if (c == ']' || c == '}') {
				if (!nesting.empty() && nesting.back() == (c == ']' ? '[' : '{'))
					nesting.pop_back();
				else if (strict)
					throw std::invalid_argument("Unmatched bracket in tool arguments");
			} else if (space(c) && nesting.empty())
				break;
		}
		if (strict && (quote || escape || !nesting.empty()))
			throw std::invalid_argument("Unterminated quote, escape or JSON value in tool arguments");
		result.push_back({begin, i});
	}
	return result;
}

std::string unquote(std::string_view value)
{
	std::string result;
	char quote = 0;
	bool escape = false;
	for (char c : value) {
		if (escape) {
			result.push_back(c);
			escape = false;
		} else if (c == '\\' && quote != '\'')
			escape = true;
		else if (quote) {
			if (c == quote)
				quote = 0;
			else
				result.push_back(c);
		} else if (c == '\'' || c == '"')
			quote = c;
		else
			result.push_back(c);
	}
	return result;
}

std::string quote(std::string_view value, char preferred = 0)
{
	if (!preferred && !value.empty() && std::none_of(value.begin(), value.end(), [](unsigned char c) {
		return space(c) || c == '\\' || c == '\'' || c == '"' || c == '[' || c == ']' || c == '{' || c == '}';
	}))
		return std::string(value);
	char mark = preferred == '"' ? '"' : '\'';
	std::string result(1, mark);
	for (char c : value) {
		if (mark == '\'' && c == '\'')
			result += "'\\''";
		else {
			if (mark == '"' && (c == '"' || c == '\\'))
				result.push_back('\\');
			result.push_back(c);
		}
	}
	result.push_back(mark);
	return result;
}

json_ptr parse_json(std::string_view value)
{
	if (value.size() >= INT_MAX)
		throw std::invalid_argument("Tool argument is too long");
	std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new(), json_tokener_free);
	if (!parser)
		throw std::bad_alloc();
	json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
	std::string terminated(value);
	json_ptr result(json_tokener_parse_ex(parser.get(), terminated.c_str(), static_cast<int>(terminated.size() + 1)), json_object_put);
	size_t end = json_tokener_get_parse_end(parser.get());
	while (end < value.size() && space(value[end]))
		++end;
	if (json_tokener_get_error(parser.get()) != json_tokener_success || end < value.size())
		throw std::invalid_argument("Invalid JSON tool argument");
	return result;
}

json_object *resolve(json_object *root, json_object *schema)
{
	for (unsigned depth = 0; schema && depth < 16; ++depth) {
		std::string ref = text(get(schema, "$ref"));
		if (!starts(ref, "#/"))
			return schema;
		json_object *next = root;
		for (size_t begin = 2; next && begin <= ref.size();) {
			size_t end = ref.find('/', begin);
			std::string key(ref.substr(begin, end == std::string::npos ? end : end - begin));
			std::string decoded;
			for (size_t i = 0; i < key.size(); ++i) {
				if (key[i] == '~' && i + 1 < key.size() && (key[i + 1] == '0' || key[i + 1] == '1'))
					decoded.push_back(key[++i] == '0' ? '~' : '/');
				else
					decoded.push_back(key[i]);
			}
			next = get(next, decoded.c_str());
			if (end == std::string::npos)
				break;
			begin = end + 1;
		}
		if (!next || next == schema)
			return schema;
		schema = next;
	}
	return schema;
}

json_object *properties(const tool &entry)
{
	return get(resolve(entry.schema.get(), entry.schema.get()), "properties");
}

json_object *property(const tool &entry, const std::string &key)
{
	return resolve(entry.schema.get(), get(properties(entry), key.c_str()));
}

bool has_type(json_object *schema, std::string_view type)
{
	json_object *value = get(schema, "type");
	if (json_object_is_type(value, json_type_string))
		return text(value) == type;
	if (json_object_is_type(value, json_type_array))
		for (size_t i = 0, count = json_object_array_length(value); i < count; ++i)
			if (text(json_object_array_get_idx(value, i)) == type)
				return true;
	return false;
}

bool accepted_type(json_object *schema, json_object *value)
{
	if (!get(schema, "type"))
		return true;
	switch (json_object_get_type(value)) {
	case json_type_null: return has_type(schema, "null");
	case json_type_boolean: return has_type(schema, "boolean");
	case json_type_double: {
		double number = json_object_get_double(value);
		return has_type(schema, "number") ||
		       (has_type(schema, "integer") && std::isfinite(number) && std::floor(number) == number);
	}
	case json_type_int: return has_type(schema, "integer") || has_type(schema, "number");
	case json_type_object: return has_type(schema, "object");
	case json_type_array: return has_type(schema, "array");
	case json_type_string: return has_type(schema, "string");
	}
	return false;
}

bool string_schema(json_object *schema)
{
	if (get(schema, "type"))
		return has_type(schema, "string");
	if (json_object_is_type(get(schema, "const"), json_type_string))
		return true;
	json_object *values = get(schema, "enum");
	if (!json_object_is_type(values, json_type_array) || !json_object_array_length(values))
		return false;
	for (size_t i = 0, count = json_object_array_length(values); i < count; ++i)
		if (!json_object_is_type(json_object_array_get_idx(values, i), json_type_string))
			return false;
	return true;
}

void validate(const tool &entry, json_object *arguments)
{
	json_object *schema = resolve(entry.schema.get(), entry.schema.get());
	json_object *required = get(schema, "required");
	if (json_object_is_type(required, json_type_array))
		for (size_t i = 0, count = json_object_array_length(required); i < count; ++i) {
			std::string key = text(json_object_array_get_idx(required, i));
			json_object *unused = nullptr;
			if (!json_object_object_get_ex(arguments, key.c_str(), &unused))
				throw std::invalid_argument("Missing required tool argument: " + key);
		}
	json_object *additional = get(schema, "additionalProperties");
	json_object_object_foreach(arguments, key, value) {
		json_object *prop = property(entry, key);
		if (!prop && json_object_is_type(additional, json_type_boolean) && !json_object_get_boolean(additional))
			throw std::invalid_argument(std::string("Unknown tool argument: ") + key);
		if (prop && !accepted_type(prop, value))
			throw std::invalid_argument(std::string("Wrong type for tool argument: ") + key);
	}
}

void replacement(matches &result, std::string_view line, size_t begin, size_t end,
                 std::string value, bool separate = false)
{
	if (result.size() >= 256)
		return;
	if (separate && (end == line.size() || !space(line[end])))
		value.push_back(' ');
	std::string completed(line.substr(0, begin));
	completed += value;
	size_t cursor = completed.size();
	completed.append(line.substr(end));
	result.emplace_back(std::move(completed), cursor);
}

struct candidate {
	std::string value;
	bool string, quoted;
};

void value_candidates(std::vector<candidate> &values, json_object *schema)
{
	if (!schema)
		return;
	auto append = [&](json_object *value) {
		bool string = json_object_is_type(value, json_type_string);
		std::string candidate = string
		                      ? text(value) : json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
		if (candidate.empty() || safe(candidate))
			values.push_back({std::move(candidate), string, string &&
			                  (json_object_is_type(get(schema, "type"), json_type_array) || !string_schema(schema))});
	};
	json_object *constant = nullptr;
	if (json_object_object_get_ex(schema, "const", &constant)) {
		append(constant);
		return;
	}
	json_object *enumeration = get(schema, "enum");
	if (json_object_is_type(enumeration, json_type_array)) {
		for (size_t i = 0, count = json_object_array_length(enumeration); i < count; ++i)
			append(json_object_array_get_idx(enumeration, i));
		return;
	}
	if (has_type(schema, "boolean")) {
		values.push_back({"false", false, false});
		values.push_back({"true", false, false});
	}
	if (has_type(schema, "null"))
		values.push_back({"null", false, false});
}

}

void catalog::clear_tools()
{
	entries.clear();
	storage = 0;
}

void catalog::add_tool(json_object *entry)
{
	std::string name = text(get(entry, "name"));
	if (!safe(name, true))
		return;
	tool value{name, text(get(entry, "title")), text(get(entry, "description")),
	           std::shared_ptr<json_object>(json_object_get(get(entry, "inputSchema")), json_object_put)};
	size_t schema_size = 0;
	if (value.schema && !json_object_to_json_string_length(value.schema.get(), JSON_C_TO_STRING_PLAIN, &schema_size))
		throw std::bad_alloc();
	value.storage = value.name.size() + value.title.size() + value.description.size() + schema_size;
	auto found = std::lower_bound(entries.begin(), entries.end(), name, [](const auto &item, const auto &key) {
		return item.name < key;
	});
	bool replace = found != entries.end() && found->name == name;
	size_t previous = replace ? found->storage : 0;
	constexpr size_t limit = 16 * 1024 * 1024;
	if ((!replace && entries.size() >= 4096) || value.storage > limit || storage - previous > limit - value.storage)
		throw std::invalid_argument("Tool catalog exceeds the client discovery limit");
	size_t updated = storage - previous + value.storage;
	if (replace)
		*found = std::move(value);
	else
		entries.insert(found, std::move(value));
	storage = updated;
}

void catalog::set_participants(std::vector<std::string> values)
{
	normalize(values);
	names = std::move(values);
}

void catalog::set_notebooks(std::vector<std::string> values)
{
	normalize(values);
	notebooks = std::move(values);
}

const tool *catalog::find_tool(std::string_view name) const
{
	auto found = std::lower_bound(entries.begin(), entries.end(), name, [](const auto &item, auto key) {
		return item.name < key;
	});
	return found != entries.end() && found->name == name ? &*found : nullptr;
}

matches catalog::complete(std::string_view line, size_t cursor) const
{
	matches result;
	if (cursor > line.size())
		return result;
	auto tokens = tokenize(line, false);
	token current{cursor, cursor};
	size_t index = 0;
	for (size_t i = 0; i < tokens.size(); ++i) {
		index = i;
		if (cursor < tokens[i].begin)
			break;
		if (cursor >= tokens[i].begin && cursor <= tokens[i].end) {
			current = tokens[i];
			index = i;
			break;
		}
		index = i + 1;
	}
	std::string_view prefix = line.substr(current.begin, cursor - current.begin);
	if (!line.empty() && line.front() == '/') {
		if (!index) {
			for (const char *command : {"/clear", "/exit", "/help", "/names", "/read", "/refresh", "/regen", "/tool", "/tools"})
				if (starts(command, prefix))
					replacement(result, line, current.begin, current.end, command,
					            std::string_view(command) == "/tool" || std::string_view(command) == "/read");
			return result;
		}
		if (tokens.empty())
			return result;
		std::string_view command = line.substr(tokens[0].begin, tokens[0].end - tokens[0].begin);
		if (command != "/tool" && command != "/tools")
			return result;
		if (index == 1 || tokens.size() == 1) {
			for (const auto &entry : entries)
				if (starts(entry.name, prefix))
					replacement(result, line, current.begin, current.end, entry.name, command == "/tool");
			return result;
		}
		if (command == "/tools" || tokens.size() < 2)
			return result;
		const tool *entry = find_tool(line.substr(tokens[1].begin, tokens[1].end - tokens[1].begin));
		if (!entry || (!prefix.empty() && prefix.front() == '{'))
			return result;
		size_t equals = prefix.find('=');
		if (equals == std::string_view::npos) {
			std::set<std::string> used;
			for (size_t i = 2; i < tokens.size(); ++i) {
				if (i == index)
					continue;
				auto value = line.substr(tokens[i].begin, tokens[i].end - tokens[i].begin);
				size_t split = value.find('=');
				if (split != std::string_view::npos)
					used.emplace(value.substr(0, split));
			}
			json_object *props = properties(*entry);
			if (!json_object_is_type(props, json_type_object))
				return result;
			size_t existing = line.substr(current.begin, current.end - current.begin).find('=');
			json_object_object_foreach(props, key, unused) {
				(void)unused;
				if (safe(key, true) && starts(key, prefix) && !used.count(key))
					replacement(result, line, current.begin,
					            existing == std::string_view::npos ? current.end : current.begin + existing,
					            std::string(key) + (existing == std::string_view::npos ? "=" : ""));
			}
		} else {
			std::string key(prefix.substr(0, equals));
			json_object *schema = property(*entry, key);
			std::vector<candidate> values;
			value_candidates(values, schema);
			json_object *constant = nullptr;
			bool names_allowed = schema && (!get(schema, "type") || has_type(schema, "string")) &&
			                     !get(schema, "enum") && !json_object_object_get_ex(schema, "const", &constant);
			if (names_allowed && (key == "participant" || key == "recipient" || key == "author"))
				for (const auto &name : names)
					values.push_back({name, true, !string_schema(schema)});
			if (names_allowed && key == "notebook")
				for (const auto &name : notebooks)
					values.push_back({name, true, !string_schema(schema)});
			std::string_view input = prefix.substr(equals + 1);
			std::string decoded = unquote(input);
			char mark = !input.empty() && (input.front() == '\'' || input.front() == '"') ? input.front() : 0;
			for (const auto &[value, string, quoted] : values)
				if (starts(value, decoded))
					replacement(result, line, current.begin + equals + 1, current.end,
					            string ? quote(value, mark ? mark : quoted ? '\'' : 0) : value, true);
		}
	} else {
		current = {cursor, cursor};
		while (current.begin && name_byte(line[current.begin - 1]))
			--current.begin;
		if (current.begin && line[current.begin - 1] == '@')
			--current.begin;
		while (current.end < line.size() && name_byte(line[current.end]))
			++current.end;
		prefix = line.substr(current.begin, cursor - current.begin);
		if (prefix.empty())
			return result;
		bool mention = prefix.front() == '@';
		std::string_view partial = mention ? prefix.substr(1) : prefix;
		bool separate = current.end == line.size() || space(line[current.end]);
		for (const auto &name : names)
			if (starts(name, partial)) {
				std::string value = (mention ? "@" : "") + name;
				if (!mention && !current.begin && separate)
					value += ':';
				replacement(result, line, current.begin, current.end, std::move(value), separate);
			}
	}
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

void catalog::parse_call(std::string_view line, std::string &name, json_object *&arguments) const
{
	arguments = nullptr;
	auto tokens = tokenize(line, true);
	if (tokens.size() < 2 || line.substr(tokens[0].begin, tokens[0].end - tokens[0].begin) != "/tool")
		throw std::invalid_argument("Usage: /tool NAME key=VALUE ... or /tool NAME {JSON}");
	name = line.substr(tokens[1].begin, tokens[1].end - tokens[1].begin);
	const tool *entry = find_tool(name);
	if (!entry)
		throw std::invalid_argument("Unknown or inaccessible tool: " + name);
	json_ptr result(nullptr, json_object_put);
	if (tokens.size() > 2 && line[tokens[2].begin] == '{') {
		result = parse_json(line.substr(tokens[2].begin));
		if (!json_object_is_type(result.get(), json_type_object))
			throw std::invalid_argument("Tool arguments must be a JSON object");
		arguments = result.release();
		return;
	} else {
		result.reset(json_object_new_object());
		if (!result)
			throw std::bad_alloc();
		for (size_t i = 2; i < tokens.size(); ++i) {
			auto value = line.substr(tokens[i].begin, tokens[i].end - tokens[i].begin);
			size_t equals = value.find('=');
			std::string key(value.substr(0, equals));
			if (equals == std::string_view::npos || !safe(key, true))
				throw std::invalid_argument("Expected a tool argument as name=value");
			json_object *existing = nullptr;
			if (json_object_object_get_ex(result.get(), key.c_str(), &existing))
				throw std::invalid_argument("Duplicate tool argument: " + key);
			std::string_view raw = value.substr(equals + 1);
			std::string decoded = !raw.empty() && (raw.front() == '{' || raw.front() == '[')
			                    ? std::string(raw) : unquote(raw);
			json_object *schema = property(*entry, key);
			json_ptr converted(nullptr, json_object_put);
			bool string = string_schema(schema);
			bool quoted = !raw.empty() && (raw.front() == '\'' || raw.front() == '"');
			bool use_string = (quoted && !get(schema, "type")) ||
			                  (string && (quoted || !json_object_is_type(get(schema, "type"), json_type_array)));
			if (!use_string) {
				try {
					converted = parse_json(decoded);
					use_string = string && !accepted_type(schema, converted.get());
				} catch (const std::invalid_argument &) {
					if (!string && get(schema, "type"))
						throw std::invalid_argument("Invalid value for tool argument: " + key);
					use_string = true;
				}
			}
			if (use_string) {
				if (decoded.size() > INT_MAX)
					throw std::invalid_argument("Tool string argument is too long");
				converted.reset(json_object_new_string_len(decoded.data(), static_cast<int>(decoded.size())));
				if (!converted)
					throw std::bad_alloc();
			}
			if (json_object_object_add(result.get(), key.c_str(), converted.get()))
				throw std::bad_alloc();
			(void)converted.release();
		}
	}
	validate(*entry, result.get());
	arguments = result.release();
}

}
