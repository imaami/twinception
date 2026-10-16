/** @file Unix-socket MCP chat client; terminal interaction adapted from llama-cli.
 * Provider selection, inference, tools and notes belong to the AskAI server. */
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <getopt.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <json-c/json.h>

#include "autocomplete.h"
#include "autocomplete-file.h"
#include "cli-console.hpp"
#include "cli-ui.hpp"

namespace {

constexpr size_t cli_limit = 16 * 1024 * 1024;
constexpr char cli_version[] = "2025-11-25";
volatile sig_atomic_t cli_signal = 0;
volatile sig_atomic_t cli_signal_write = -1;

void cli_on_signal(int signum)
{
	int saved = errno;
	if (signum != SIGWINCH)
		cli_signal = signum;
	if (cli_signal_write >= 0) {
		unsigned char byte = static_cast<unsigned char>(signum);
		(void)write(cli_signal_write, &byte, 1);
	}
	errno = saved;
}

struct cli_signals {
	int pipe[2] = {-1, -1};
	struct sigaction old_int{}, old_term{}, old_pipe{}, old_winch{};
	unsigned installed = 0;

	cli_signals()
	{
		if (pipe2(pipe, O_CLOEXEC | O_NONBLOCK))
			throw std::runtime_error(std::strerror(errno));
		struct sigaction action{};
		action.sa_handler = cli_on_signal;
		sigemptyset(&action.sa_mask);
		cli_signal_write = pipe[1];
		try {
			if (sigaction(SIGINT, &action, &old_int))
				throw std::runtime_error("Cannot install SIGINT handler");
			installed = 1;
			if (sigaction(SIGTERM, &action, &old_term))
				throw std::runtime_error("Cannot install SIGTERM handler");
			installed = 2;
			if (sigaction(SIGWINCH, &action, &old_winch))
				throw std::runtime_error("Cannot install SIGWINCH handler");
			installed = 3;
			action.sa_handler = SIG_IGN;
			if (sigaction(SIGPIPE, &action, &old_pipe))
				throw std::runtime_error("Cannot ignore SIGPIPE");
			installed = 4;
		} catch (...) {
			close();
			throw;
		}
	}

	void close()
	{
		cli_signal_write = -1;
		if (installed >= 1)
			(void)sigaction(SIGINT, &old_int, nullptr);
		if (installed >= 2)
			(void)sigaction(SIGTERM, &old_term, nullptr);
		if (installed >= 3)
			(void)sigaction(SIGWINCH, &old_winch, nullptr);
		if (installed >= 4)
			(void)sigaction(SIGPIPE, &old_pipe, nullptr);
		for (int &fd : pipe) {
			if (fd >= 0)
				::close(fd);
			fd = -1;
		}
	}
	~cli_signals() { close(); }

	void drain()
	{
		char bytes[64];
		while (read(pipe[0], bytes, sizeof bytes) > 0) {}
	}
};

struct cli_json_delete {
	void operator()(json_object *object) const { json_object_put(object); }
};
using cli_json = std::unique_ptr<json_object, cli_json_delete>;

cli_json cli_owned(json_object *object)
{
	if (!object)
		throw std::bad_alloc();
	return cli_json(object);
}

json_object *cli_get(json_object *object, const char *key)
{
	json_object *value = nullptr;
	return json_object_object_get_ex(object, key, &value) ? value : nullptr;
}

std::string_view cli_text(json_object *object)
{
	if (!json_object_is_type(object, json_type_string))
		throw std::runtime_error("Expected a JSON string from AskAI");
	return {json_object_get_string(object),
	        static_cast<size_t>(json_object_get_string_len(object))};
}

cli_json cli_string(std::string_view text)
{
	if (text.size() > cli_limit)
		throw std::runtime_error("Text exceeds the 16 MiB client limit");
	return cli_owned(json_object_new_string_len(text.data(), static_cast<int>(text.size())));
}

void cli_set(json_object *object, const char *key, cli_json value)
{
	if (json_object_object_add(object, key, value.get()))
		throw std::bad_alloc();
	(void)value.release();
}

void cli_add(json_object *array, cli_json value)
{
	if (json_object_array_add(array, value.get()))
		throw std::bad_alloc();
	(void)value.release();
}

struct cli_parser {
	std::unique_ptr<json_tokener, decltype(&json_tokener_free)>
		parser{json_tokener_new(), json_tokener_free};

	cli_parser()
	{
		if (!parser)
			throw std::bad_alloc();
		json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
	}

	cli_json parse(std::string_view text)
	{
		if (text.size() > cli_limit)
			throw std::runtime_error("AskAI JSON exceeds the 16 MiB client limit");
		json_tokener_reset(parser.get());
		cli_json value(json_tokener_parse_ex(parser.get(), text.data(), static_cast<int>(text.size())));
		if (json_tokener_get_error(parser.get()) != json_tokener_success ||
		    json_tokener_get_parse_end(parser.get()) != text.size() ||
		    !json_object_is_type(value.get(), json_type_object))
			throw std::runtime_error("Malformed JSON response from AskAI");
		return value;
	}
};

std::string_view cli_wire(json_object *object, int flags = JSON_C_TO_STRING_PLAIN)
{
	size_t size = 0;
	const char *wire = json_object_to_json_string_length(object, flags, &size);
	if (!wire)
		throw std::bad_alloc();
	if (size > cli_limit)
		throw std::runtime_error("Conversation exceeds the 16 MiB client limit; use /clear");
	return {wire, size};
}

std::string_view cli_view(autocomplete_text text)
{
	return {text.data, text.len};
}

autocomplete_text cli_span(std::string_view text)
{
	return {text.data(), text.size()};
}

void cli_check(int error)
{
	if (error == ENOMEM)
		throw std::bad_alloc();
	if (error)
		throw std::runtime_error(std::strerror(error));
}

struct cli_autocomplete_delete {
	void operator()(struct autocomplete *object) const { autocomplete_destroy(&object); }
};
using cli_autocomplete = std::unique_ptr<struct autocomplete, cli_autocomplete_delete>;

cli_autocomplete cli_autocomplete_create()
{
	int error = 0;
	cli_autocomplete object(autocomplete_create(&error));
	cli_check(error);
	return object;
}

struct cli_files_delete {
	void operator()(struct autocomplete_file *object) const { autocomplete_file_destroy(&object); }
};
using cli_files = std::unique_ptr<struct autocomplete_file, cli_files_delete>;

bool cli_equal(std::string_view a, std::string_view b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
		[](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

std::string_view cli_trim(std::string_view text)
{
	while (!text.empty() && (text.back() == '\r' || text.back() == '\n' ||
	                         text.back() == ' ' || text.back() == '\t'))
		text.remove_suffix(1);
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
		text.remove_prefix(1);
	return text;
}

struct cli_interrupted : std::exception {};

/** One HTTP response: parse SSE incrementally without retaining old events. */
struct cli_response {
	cli_parser parser;
	std::function<void(json_object *)> event;
	std::string body, line, data, session, error;
	bool sse = false;
	long status = 0;

	void dispatch()
	{
		if (data.empty())
			return;
		data.pop_back();
		auto value = parser.parse(data);
		event(value.get());
		data.clear();
	}

	void consume_line()
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty())
			dispatch();
		else if (line.compare(0, 5, "data:") == 0) {
			size_t off = line.size() > 5 && line[5] == ' ' ? 6 : 5;
			if (data.size() + line.size() - off + 1 > cli_limit)
				throw std::runtime_error("AskAI SSE event exceeds the client limit");
			data.append(line, off, std::string::npos);
			data += '\n';
		}
		line.clear();
	}

	static size_t receive(char *ptr, size_t size, size_t count, void *opaque) noexcept
	{
		auto &response = *static_cast<cli_response *>(opaque);
		size_t bytes = size * count;
		try {
			if (!response.sse) {
				if (bytes > cli_limit - response.body.size())
					throw std::runtime_error("AskAI response exceeds the client limit");
				response.body.append(ptr, bytes);
			} else {
				for (size_t i = 0; i < bytes; ++i) {
					if (ptr[i] == '\n')
						response.consume_line();
					else {
						if (response.line.size() == cli_limit)
							throw std::runtime_error("AskAI SSE line exceeds the client limit");
						response.line += ptr[i];
					}
				}
			}
			return bytes;
		} catch (const std::exception &e) {
			response.error = e.what();
			return 0;
		}
	}

	static size_t header(char *ptr, size_t size, size_t count, void *opaque) noexcept
	{
		auto &response = *static_cast<cli_response *>(opaque);
		size_t bytes = size * count;
		try {
			std::string_view line(ptr, bytes);
			if (line.substr(0, 5) == "HTTP/") {
				response.sse = false;
				response.session.clear();
			}
			size_t colon = line.find(':');
			if (colon != std::string_view::npos) {
				auto key = line.substr(0, colon);
				auto value = cli_trim(line.substr(colon + 1));
				if (cli_equal(key, "Content-Type"))
					response.sse = cli_equal(value.substr(0, value.find(';')), "text/event-stream");
				else if (cli_equal(key, "Mcp-Session-Id")) {
					if (value.size() > 1024 || std::any_of(value.begin(), value.end(),
						[](unsigned char c) { return c < 0x21 || c > 0x7e; }))
						throw std::runtime_error("Invalid MCP session header");
					response.session = value;
				}
			}
			return bytes;
		} catch (const std::exception &e) {
			response.error = e.what();
			return 0;
		}
	}
};

struct cli_curl {
	cli_curl()
	{
		if (curl_global_init(CURL_GLOBAL_DEFAULT))
			throw std::runtime_error("Cannot initialize libcurl");
	}
	~cli_curl() { curl_global_cleanup(); }
};

/** Every connection explicitly selects a Unix socket; no proxy or TCP fallback. */
struct cli_client {
	cli_signals &signals;
	std::string socket, session, tool;
	int64_t next_id = 0;
	cli_autocomplete catalog;
	bool catalog_dirty = false;

	void post(std::string_view wire, cli_response &response, bool interruptible = true,
	          long timeout = 0)
	{
		std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), curl_easy_cleanup);
		std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
		if (!easy || !multi)
			throw std::bad_alloc();
		curl_slist *raw_headers = nullptr;
		auto append = [&](const std::string &line) {
			curl_slist *next = curl_slist_append(raw_headers, line.c_str());
			if (!next) {
				curl_slist_free_all(raw_headers);
				raw_headers = nullptr;
				throw std::bad_alloc();
			}
			raw_headers = next;
		};
		append("Content-Type: application/json");
		append("Accept: application/json, text/event-stream");
		append(std::string("MCP-Protocol-Version: ") + cli_version);
		if (!session.empty())
			append("Mcp-Session-Id: " + session);
		std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
		auto option = [&](CURLoption key, auto value) {
			CURLcode e = curl_easy_setopt(easy.get(), key, value);
			if (e != CURLE_OK)
				throw std::runtime_error(curl_easy_strerror(e));
		};
		option(CURLOPT_URL, "http://localhost/mcp");
		option(CURLOPT_UNIX_SOCKET_PATH, socket.c_str());
		option(CURLOPT_PROXY, "");
		option(CURLOPT_NOPROXY, "*");
		option(CURLOPT_FOLLOWLOCATION, 0L);
		option(CURLOPT_NOSIGNAL, 1L);
		option(CURLOPT_CONNECTTIMEOUT, 10L);
		option(CURLOPT_TIMEOUT, timeout);
		option(CURLOPT_POST, 1L);
		option(CURLOPT_POSTFIELDS, wire.data());
		option(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(wire.size()));
		option(CURLOPT_HTTPHEADER, headers.get());
		option(CURLOPT_WRITEFUNCTION, cli_response::receive);
		option(CURLOPT_WRITEDATA, &response);
		option(CURLOPT_HEADERFUNCTION, cli_response::header);
		option(CURLOPT_HEADERDATA, &response);
		CURLMcode e = curl_multi_add_handle(multi.get(), easy.get());
		if (e != CURLM_OK)
			throw std::runtime_error(curl_multi_strerror(e));
		try {
			int active = 0;
			do {
				if (interruptible && cli_signal)
					throw cli_interrupted();
				e = curl_multi_perform(multi.get(), &active);
				if (e != CURLM_OK)
					throw std::runtime_error(curl_multi_strerror(e));
				if (active) {
					curl_waitfd wake{signals.pipe[0], CURL_WAIT_POLLIN, 0};
					e = curl_multi_poll(multi.get(), &wake, 1, 1000, nullptr);
					if (e != CURLM_OK)
						throw std::runtime_error(curl_multi_strerror(e));
					if (wake.revents)
						signals.drain();
				}
			} while (active);
			int left = 0;
			CURLMsg *done = curl_multi_info_read(multi.get(), &left);
			if (!response.error.empty())
				throw std::runtime_error(response.error);
			if (!done || done->msg != CURLMSG_DONE || done->data.result != CURLE_OK)
				throw std::runtime_error(done ? curl_easy_strerror(done->data.result)
				                              : "HTTP transfer did not complete");
			if (curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &response.status) != CURLE_OK)
				throw std::runtime_error("Cannot read HTTP status");
			if (response.status < 200 || response.status >= 300)
				throw std::runtime_error("AskAI HTTP " + std::to_string(response.status) +
				                         (response.body.empty() ? "" : ": " + response.body.substr(0, 1024)));
			if (response.sse && (!response.line.empty() || !response.data.empty()))
				throw std::runtime_error("AskAI closed an incomplete SSE event");
		} catch (...) {
			(void)curl_multi_remove_handle(multi.get(), easy.get());
			throw;
		}
		(void)curl_multi_remove_handle(multi.get(), easy.get());
	}

	cli_json packet(const char *method, cli_json params, int64_t id)
	{
		auto message = cli_owned(json_object_new_object());
		cli_set(message.get(), "jsonrpc", cli_string("2.0"));
		cli_set(message.get(), "method", cli_string(method));
		if (id)
			cli_set(message.get(), "id", cli_owned(json_object_new_int64(id)));
		cli_set(message.get(), "params", std::move(params));
		return message;
	}

	void cancel(int64_t id) noexcept
	{
		try {
			auto params = cli_owned(json_object_new_object());
			cli_set(params.get(), "requestId", cli_owned(json_object_new_int64(id)));
			cli_set(params.get(), "reason", cli_string("Interrupted by the terminal user"));
			auto message = packet("notifications/cancelled", std::move(params), 0);
			cli_response response;
			post(cli_wire(message.get()), response, false, 5);
		} catch (const std::exception &e) {
			cli_ui::show_error("Cancellation could not be confirmed", e.what());
		}
	}

	cli_json rpc(const char *method, cli_json params,
	              const std::function<void(json_object *, int64_t)> &notify = {})
	{
		int64_t id = ++next_id;
		auto message = packet(method, std::move(params), id);
		cli_json result;
		cli_response response;
		response.event = [&](json_object *event) {
			json_object *event_id = cli_get(event, "id");
			if (event_id) {
				if (!json_object_is_type(event_id, json_type_int) ||
				    json_object_get_int64(event_id) != id || result)
					throw std::runtime_error("Unexpected JSON-RPC response id");
				result.reset(json_object_get(event));
			} else {
				json_object *method = cli_get(event, "method");
				if (method && cli_text(method) == "notifications/tools/list_changed")
					catalog_dirty = true;
				if (notify)
					notify(event, id);
			}
		};
		try {
			post(cli_wire(message.get()), response, true, !std::strcmp(method, "tools/call") ? 0 : 30);
		} catch (const cli_interrupted &) {
			if (!std::strcmp(method, "tools/call"))
				cancel(id);
			throw;
		}
		if (!response.session.empty())
			session = response.session;
		if (!response.sse) {
			auto value = response.parser.parse(response.body);
			response.event(value.get());
		}
		if (!result)
			throw std::runtime_error("AskAI closed the stream without a result");
		if (json_object *error = cli_get(result.get(), "error"))
			throw std::runtime_error("AskAI: " + std::string(cli_text(cli_get(error, "message"))));
		json_object *value = cli_get(result.get(), "result");
		if (!json_object_is_type(value, json_type_object))
			throw std::runtime_error("AskAI returned no result object");
		return cli_owned(json_object_get(value));
	}

	void connect()
	{
		auto params = cli_owned(json_object_new_object());
		cli_set(params.get(), "protocolVersion", cli_string(cli_version));
		cli_set(params.get(), "capabilities", cli_owned(json_object_new_object()));
		auto info = cli_owned(json_object_new_object());
		cli_set(info.get(), "name", cli_string("askai-cli"));
		cli_set(info.get(), "version", cli_string("1"));
		cli_set(params.get(), "clientInfo", std::move(info));
		auto initialized = rpc("initialize", std::move(params));
		if (cli_text(cli_get(initialized.get(), "protocolVersion")) != cli_version || session.empty())
			throw std::runtime_error("AskAI did not negotiate the supported MCP session");
		auto ready = packet("notifications/initialized", cli_owned(json_object_new_object()), 0);
		cli_response response;
		post(cli_wire(ready.get()), response, true, 30);
		refresh();
	}

	void refresh()
	{
		catalog_dirty = false;
		auto discovered = cli_autocomplete_create();
		std::vector<std::string> participants, notebooks;
		std::string notebook_tool;
		std::string cursor, chosen;
		for (unsigned page = 0; page < 32; ++page) {
			auto params = cli_owned(json_object_new_object());
			if (!cursor.empty())
				cli_set(params.get(), "cursor", cli_string(cursor));
			auto result = rpc("tools/list", std::move(params));
			json_object *tools = cli_get(result.get(), "tools");
			if (!json_object_is_type(tools, json_type_array))
				throw std::runtime_error("AskAI returned an invalid tool list");
			size_t count = json_object_array_length(tools);
			for (size_t i = 0; i < count; ++i) {
				json_object *entry = json_object_array_get_idx(tools, i);
				cli_check(autocomplete_add_tool(discovered.get(), entry));
				std::string_view name = cli_text(cli_get(entry, "name"));
				json_object *props = cli_get(cli_get(entry, "inputSchema"), "properties");
				json_object *meta = cli_get(cli_get(entry, "_meta"), "askai");
				json_object *books = cli_get(meta, "notebooks");
				bool note_source = json_object_is_type(books, json_type_boolean) &&
				                   json_object_get_boolean(books);
				// Older AskAI servers have the same read-only discovery action.
				json_object *actions = cli_get(cli_get(props, "action"), "enum");
				if (!meta && name == "notes" && cli_get(props, "notebook") &&
				    json_object_is_type(actions, json_type_array)) {
					for (size_t a = 0; a < json_object_array_length(actions); ++a) {
						json_object *action = json_object_array_get_idx(actions, a);
						if (json_object_is_type(action, json_type_string) && cli_text(action) == "notebooks")
							note_source = true;
					}
				}
				if (note_source && notebook_tool.empty()) {
					notebook_tool = name;
					json_object *self = cli_get(meta, "participant");
					if (json_object_is_type(self, json_type_string))
						participants.emplace_back(cli_text(self));
				}
				json_object *title = cli_get(entry, "title");
				bool candidate = !tool.empty() ? name == tool
				               : title && cli_text(title) == "Ask AI";
				if (!candidate)
					continue;
				if (!cli_get(props, "prompt") || !cli_get(props, "messages") ||
				    !cli_get(props, "background"))
					throw std::runtime_error("The selected AskAI tool needs the chat extension; rebuild the server too");
				if (!chosen.empty())
					throw std::runtime_error("Multiple chat tools; select one with --tool");
				chosen = name;
			}
			json_object *next = cli_get(result.get(), "nextCursor");
			if (!next) {
				if (chosen.empty())
					throw std::runtime_error("No accessible AskAI chat tool was found");
				if (!notebook_tool.empty()) {
					try {
						auto args = cli_owned(json_object_new_object());
						cli_set(args.get(), "action", cli_string("notebooks"));
						auto call = cli_owned(json_object_new_object());
						cli_set(call.get(), "name", cli_string(notebook_tool));
						cli_set(call.get(), "arguments", std::move(args));
						auto listing = rpc("tools/call", std::move(call));
						if (json_object_get_boolean(cli_get(listing.get(), "isError")))
							throw std::runtime_error("The notes tool rejected notebook discovery");
						json_object *content = cli_get(listing.get(), "content");
						if (!json_object_is_type(content, json_type_array))
							throw std::runtime_error("Invalid notebook discovery result");
						for (size_t b = 0; b < json_object_array_length(content); ++b) {
							json_object *block = json_object_array_get_idx(content, b);
							json_object *type = cli_get(block, "type");
							if (json_object_is_type(type, json_type_string) && cli_text(type) == "text")
								read_notebooks(cli_text(cli_get(block, "text")), participants, notebooks);
						}
					} catch (const cli_interrupted &) {
						throw;
					} catch (const std::exception &e) {
						cli_ui::show_error("Participant discovery failed", e.what());
					}
				}
				std::vector<autocomplete_text> names;
				names.reserve(std::max(participants.size(), notebooks.size()));
				for (const auto &name : participants)
					names.push_back(cli_span(name));
				cli_check(autocomplete_set_participants(discovered.get(), names.data(), names.size()));
				names.clear();
				for (const auto &name : notebooks)
					names.push_back(cli_span(name));
				cli_check(autocomplete_set_notebooks(discovered.get(), names.data(), names.size()));
				catalog = std::move(discovered);
				tool = std::move(chosen);
				return;
			}
			std::string_view after = cli_text(next);
			if (after.empty() || after == cursor)
				throw std::runtime_error("Invalid tool-list cursor");
			cursor = after;
		}
		throw std::runtime_error("Too many tool-list pages");
	}

	static void read_notebooks(std::string_view text, std::vector<std::string> &names,
	                           std::vector<std::string> &notebooks)
	{
		// A truncated last line is never an authoritative notebook selector.
		for (size_t newline; (newline = text.find('\n')) != std::string_view::npos;) {
			std::string_view line = text.substr(0, newline);
			text.remove_prefix(newline + 1);
			size_t first = line.find('/'), last = line.rfind('/');
			if (first == std::string_view::npos || first == last || !first || first > 64)
				continue;
			std::string_view name = line.substr(0, first);
			std::string_view principal = line.substr(first + 1, last - first - 1);
			std::string_view side = line.substr(last + 1);
			if ((side != "client" && side != "upstream") ||
			    std::any_of(name.begin(), name.end(), [](unsigned char c) {
				    return !(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') &&
				           !(c >= '0' && c <= '9') && c != '_' && c != '-';
			    }))
				continue;
			if (principal != "anonymous" &&
			    (principal.size() != 16 || std::any_of(principal.begin(), principal.end(), [](unsigned char c) {
				    return !(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f');
			    })))
				continue;
			names.emplace_back(name);
			notebooks.emplace_back(line);
		}
	}
};

struct cli_options {
	std::string socket, tool, prompt, system;
	bool has_prompt = false, has_system = false, single = false, simple = false;
	bool multiline = false, reasoning = true;
	std::string color = "auto";
};

std::string cli_read_file(const std::string &path)
{
	std::string expanded = path;
	if (path == "~" || path.compare(0, 2, "~/") == 0) {
		if (const char *home = std::getenv("HOME"))
			expanded = home + path.substr(1);
	}
	std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(expanded.c_str(), "rb"), std::fclose);
	if (!file)
		throw std::runtime_error(path + ": " + std::strerror(errno));
	std::string result;
	char chunk[8192];
	while (size_t count = std::fread(chunk, 1, sizeof chunk, file.get())) {
		if (count > cli_limit - result.size())
			throw std::runtime_error("Prompt file exceeds the 16 MiB client limit");
		result.append(chunk, count);
	}
	if (std::ferror(file.get()))
		throw std::runtime_error(path + ": read failed");
	if (result.find('\0') != std::string::npos)
		throw std::runtime_error("Prompt files must contain text without NUL bytes");
	return result;
}

void cli_help()
{
	std::puts("Usage: askai-cli --socket PATH [options]\n"
	          "Chat with AskAI's upstream LLM over its Unix socket.\n\n"
	          "  --socket PATH            AskAI socket (or ASKAI_SOCKET)\n"
	          "  --tool NAME              Select the configured ask tool\n"
	          "  -p, --prompt TEXT        Send an initial prompt\n"
	          "  -f, --file FILE          Read the initial prompt from a text file\n"
	          "  --system TEXT            Override server system instructions\n"
	          "  --single-turn            Exit after one answer\n"
	          "  --simple-io              Basic line input; automatic for pipes\n"
	          "  --multiline-input        Enter adds a line; finish with a trailing /\n"
	          "  --color auto|on|off       Terminal colors (default auto)\n"
	          "  --no-reasoning            Hide exposed reasoning\n"
	          "  -h, --help               Show this help\n\n"
	          "Commands: /help, /clear, /regen, /read FILE, /exit\n"
	          "          /tools [NAME], /tool NAME key=VALUE ..., /names, /refresh\n"
	          "Tab completes exposed tools, parameters, values and participant names.\n"
	          "Readline: PgUp/PgDn search history by prefix; Ctrl-R searches history.\n"
	          "End a line with \\ to continue it. Ctrl-C cancels the current answer;\n"
	          "while entering a prompt it exits. Ctrl-D exits. Tools run in AskAI.");
}

cli_options cli_args(int argc, char **argv)
{
	cli_options out;
	if (const char *env = std::getenv("ASKAI_SOCKET"))
		out.socket = env;
	static const option options[] = {
		{"socket", required_argument, nullptr, 's'},
		{"tool", required_argument, nullptr, 't'},
		{"prompt", required_argument, nullptr, 'p'},
		{"file", required_argument, nullptr, 'f'},
		{"system", required_argument, nullptr, 'S'},
		{"single-turn", no_argument, nullptr, '1'},
		{"simple-io", no_argument, nullptr, 'i'},
		{"multiline-input", no_argument, nullptr, 'm'},
		{"color", required_argument, nullptr, 'c'},
		{"no-reasoning", no_argument, nullptr, 'r'},
		{"help", no_argument, nullptr, 'h'},
		{"version", no_argument, nullptr, 'v'},
		{nullptr, 0, nullptr, 0}
	};
	for (int c; (c = getopt_long(argc, argv, "hp:f:", options, nullptr)) != -1;) {
		switch (c) {
		case 's': out.socket = optarg; break;
		case 't': out.tool = optarg; break;
		case 'p': case 'f':
			if (out.has_prompt)
				throw std::runtime_error("Use only one --prompt or --file");
			out.prompt = c == 'p' ? optarg : cli_read_file(optarg);
			out.has_prompt = true;
			break;
		case 'S': out.system = optarg; out.has_system = true; break;
		case '1': out.single = true; break;
		case 'i': out.simple = true; break;
		case 'm': out.multiline = true; break;
		case 'c': out.color = optarg; break;
		case 'r': out.reasoning = false; break;
		case 'h': cli_help(); std::exit(0);
		case 'v': std::puts("askai-cli 1"); std::exit(0);
		default: throw std::runtime_error("Invalid option; see --help");
		}
	}
	if (optind != argc)
		throw std::runtime_error("Unexpected positional argument; see --help");
	if (out.socket.compare(0, 5, "unix:") == 0)
		out.socket.erase(0, 5);
	if (out.socket.empty())
		throw std::runtime_error("Specify --socket PATH or ASKAI_SOCKET");
	if (out.color != "auto" && out.color != "on" && out.color != "off")
		throw std::runtime_error("--color must be auto, on or off");
	return out;
}

using cli_history = std::vector<std::pair<std::string, std::string>>;

void cli_progress(cli_ui::assistant_turn &display, json_object *event,
                   std::string_view actor, std::string_view token)
{
	json_object *method = cli_get(event, "method");
	if (!json_object_is_type(method, json_type_string) || cli_text(method) != "notifications/progress")
		return;
	json_object *params = cli_get(event, "params");
	json_object *progress = cli_get(params, "progressToken");
	json_object *message = cli_get(params, "message");
	if (json_object_is_type(progress, json_type_string) && cli_text(progress) == token &&
	    json_object_is_type(message, json_type_string))
		display.action(actor, cli_text(message));
}

std::string cli_ask(cli_client &client, const cli_options &options,
                     const cli_history &history, size_t turns, const std::string &prompt)
{
	if (turns > 512)
		throw std::runtime_error("Conversation has 512 turns; use /clear");
	size_t bytes = prompt.size();
	for (size_t i = 0; i < turns; ++i) {
		if (bytes > cli_limit || history[i].first.size() > cli_limit - bytes)
			throw std::runtime_error("Conversation exceeds the client limit; use /clear");
		bytes += history[i].first.size();
		if (history[i].second.size() > cli_limit - bytes)
			throw std::runtime_error("Conversation exceeds the client limit; use /clear");
		bytes += history[i].second.size();
	}
	cli_ui::assistant_turn display;
	std::string streamed;
	int64_t stream_turn = 0;
	auto args = cli_owned(json_object_new_object());
	cli_set(args.get(), "prompt", cli_string(prompt));
	cli_set(args.get(), "background", cli_owned(json_object_new_boolean(false)));
	if (options.has_system)
		cli_set(args.get(), "system", cli_string(options.system));
	auto messages = cli_owned(json_object_new_array());
	auto add_message = [&](const char *role, const std::string &text) {
		auto message = cli_owned(json_object_new_object());
		cli_set(message.get(), "role", cli_string(role));
		cli_set(message.get(), "content", cli_string(text));
		cli_add(messages.get(), std::move(message));
	};
	for (size_t i = 0; i < turns; ++i) {
		add_message("user", history[i].first);
		add_message("assistant", history[i].second);
	}
	cli_set(args.get(), "messages", std::move(messages));
	auto params = cli_owned(json_object_new_object());
	cli_set(params.get(), "name", cli_string(client.tool));
	cli_set(params.get(), "arguments", std::move(args));
	auto meta = cli_owned(json_object_new_object());
	auto extension = cli_owned(json_object_new_object());
	cli_set(extension.get(), "stream", cli_owned(json_object_new_boolean(true)));
	cli_set(meta.get(), "askai", std::move(extension));
	cli_set(meta.get(), "progressToken", cli_string("cli"));
	cli_set(params.get(), "_meta", std::move(meta));
	auto result = client.rpc("tools/call", std::move(params), [&](json_object *event, int64_t id) {
		cli_progress(display, event, client.tool, "cli");
		json_object *method = cli_get(event, "method");
		if (!method || cli_text(method) != "notifications/askai/delta")
			return;
		json_object *p = cli_get(event, "params");
		json_object *request = cli_get(p, "requestId");
		if (!json_object_is_type(request, json_type_int) || json_object_get_int64(request) != id)
			throw std::runtime_error("Unexpected streaming request id");
		std::string_view kind = cli_text(cli_get(p, "kind"));
		std::string_view text = cli_text(cli_get(p, "text"));
		json_object *turn = cli_get(p, "turn");
		int64_t current = turn ? json_object_get_int64(turn) : 0;
		if (current != stream_turn) {
			if (!streamed.empty())
				display.push(false, "\n");
			streamed.clear();
			stream_turn = current;
		}
		if (kind == "text") {
			if (text.size() > cli_limit - streamed.size())
				throw std::runtime_error("Assistant turn exceeds the client limit");
			streamed += text;
			display.push(false, text);
		} else if (kind == "reasoning") {
			if (options.reasoning)
				display.push(true, text);
		} else
			throw std::runtime_error("Unknown AskAI delta kind");
		if (std::ferror(stdout))
			throw std::runtime_error("Cannot write the assistant output");
	});
	json_object *content = cli_get(result.get(), "content");
	if (!json_object_is_type(content, json_type_array) || !json_object_array_length(content))
		throw std::runtime_error("AskAI returned no assistant text");
	json_object *first = json_object_array_get_idx(content, 0);
	if (cli_text(cli_get(first, "type")) != "text")
		throw std::runtime_error("AskAI returned unsupported assistant content");
	std::string answer(cli_text(cli_get(first, "text")));
	if (json_object_get_boolean(cli_get(result.get(), "isError")))
		throw std::runtime_error(answer);
	if (answer.compare(0, streamed.size(), streamed) == 0)
		display.push(false, std::string_view(answer).substr(streamed.size()));
	else if (!streamed.empty() && answer.size() >= streamed.size() &&
	         answer.compare(answer.size() - streamed.size(), streamed.size(), streamed) == 0) {
		/* Agenda wakeups may include already displayed earlier turns. */
	} else {
		if (!streamed.empty())
			display.push(false, "\n");
		display.push(false, answer);
	}
	display.finish();
	if (std::ferror(stdout))
		throw std::runtime_error("Cannot write the assistant output");
	return answer;
}

void cli_tools(const cli_client &client, std::string_view name)
{
	if (!name.empty()) {
		const autocomplete_tool *entry = autocomplete_find_tool(client.catalog.get(), cli_span(name));
		if (!entry)
			throw std::runtime_error("No accessible tool named " + std::string(name));
		cli_ui::show_info(cli_view(entry->name));
		if (entry->description.len)
			cli_ui::show_message(cli_view(entry->description));
		cli_ui::show_message(cli_wire(entry->schema, JSON_C_TO_STRING_PRETTY));
		return;
	}
	std::string line;
	line.reserve(256);
	for (size_t i = 0, count = autocomplete_tool_count(client.catalog.get()); i < count; ++i) {
		const autocomplete_tool *entry = autocomplete_tool_at(client.catalog.get(), i);
		std::string_view summary = cli_view(entry->description);
		summary = summary.substr(0, std::min(summary.find('\n'), size_t(160)));
		line.assign(entry->name.data, entry->name.len);
		if (!summary.empty()) {
			line += "  ";
			line += summary;
		}
		cli_ui::show_message(line);
	}
}

void cli_tool_call(cli_client &client, std::string_view command)
{
	const autocomplete_tool *tool = nullptr;
	json_object *raw_arguments = nullptr;
	char message[512];
	autocomplete_error diagnostic{message, sizeof message, 0};
	int error = autocomplete_parse_call(client.catalog.get(), cli_span(command), &tool, &raw_arguments, &diagnostic);
	if (error) {
		if (diagnostic.len)
			throw std::runtime_error(std::string(message, diagnostic.len));
		cli_check(error);
	}
	std::string_view name = cli_view(tool->name);
	cli_json arguments(raw_arguments);
	auto params = cli_owned(json_object_new_object());
	cli_set(params.get(), "name", cli_string(name));
	cli_set(params.get(), "arguments", std::move(arguments));
	auto meta = cli_owned(json_object_new_object());
	cli_set(meta.get(), "progressToken", cli_string("tool"));
	cli_set(params.get(), "_meta", std::move(meta));
	cli_ui::assistant_turn display;
	display.action("you", "call " + std::string(name));
	auto result = client.rpc("tools/call", std::move(params), [&](json_object *event, int64_t) {
		cli_progress(display, event, name, "tool");
	});
	json_object *content = cli_get(result.get(), "content");
	if (json_object_is_type(content, json_type_array)) {
		for (size_t i = 0; i < json_object_array_length(content); ++i) {
			json_object *block = json_object_array_get_idx(content, i);
			json_object *type = cli_get(block, "type");
			if (!json_object_is_type(type, json_type_string))
				throw std::runtime_error("Malformed tool content");
			std::string_view kind = cli_text(type);
			if (kind == "text")
				cli_ui::show_message(cli_text(cli_get(block, "text")));
			else
				cli_ui::show_message("[" + std::string(kind) + " content]");
		}
	} else if (json_object *structured = cli_get(result.get(), "structuredContent")) {
		cli_ui::show_message(cli_wire(structured, JSON_C_TO_STRING_PRETTY));
	} else {
		throw std::runtime_error("AskAI returned no tool content");
	}
	if (json_object_get_boolean(cli_get(result.get(), "isError")))
		throw std::runtime_error("Tool " + std::string(name) + " failed");
	display.action(name, "completed");
	if (std::ferror(stdout))
		throw std::runtime_error("Cannot write the tool output");
}

int cli_run(const cli_options &options)
{
	cli_curl curl;
	cli_signals signals;
	bool tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
	bool color = options.color == "on" || (options.color == "auto" && tty && !std::getenv("NO_COLOR"));
	int error = 0;
	cli_console::terminal terminal(options.simple || !tty, color, &cli_signal, error,
	                                signals.pipe[0]);
	if (error)
		throw std::runtime_error(std::strerror(error));
	cli_client client{signals, options.socket, {}, options.tool, 0, {}, false};
	client.connect();
	cli_files files(autocomplete_file_create(&error));
	cli_check(error);
	cli_console::set_completion_callback([&client, &files](std::string_view line, size_t cursor) {
		cli_console::completion_matches matches;
		cli_check(autocomplete_complete(client.catalog.get(), cli_span(line), cursor, &matches.data, &matches.size));
		if (!matches.size) {
			constexpr autocomplete_text commands[] = {{"/read ", sizeof "/read " - 1}};
			cli_check(autocomplete_file_complete(files.get(), cli_span(line), cursor, commands,
			                                     sizeof commands / sizeof commands[0], &matches.data, &matches.size));
		}
		return matches;
	});
	if (tty)
		cli_ui::show_info("Connected to " + client.tool + ". /help lists commands; Ctrl-C cancels an answer.");
	cli_history history;
	bool initial = options.has_prompt;
	for (;;) {
		if (client.catalog_dirty) {
			try { client.refresh(); }
			catch (const cli_interrupted &) { throw; }
			catch (const std::exception &e) { cli_ui::show_error("Cannot refresh completion metadata", e.what()); }
		}
		bool interpret = !initial;
		std::string prompt;
		if (initial) {
			prompt = options.prompt;
			initial = false;
		} else {
			bool eof = false;
			int e = cli_console::read_input(prompt, options.multiline, eof, tty ? "\n> " : "");
			if (e == EINTR)
				return 128 + cli_signal;
			if (e)
				throw std::runtime_error(std::strerror(e));
			if (eof && prompt.empty())
				return 0;
		}
		try {
			bool regenerate = false;
			if (interpret) {
				std::string_view command = cli_trim(prompt);
				size_t split = command.find_first_of(" \t\r\n");
				std::string_view verb = command.substr(0, split);
				std::string_view rest = split == std::string_view::npos ? std::string_view{} : cli_trim(command.substr(split));
				if (command == "/exit" || command == "/quit")
					return 0;
				if (command == "/help") {
					cli_ui::show_message("/clear starts a new conversation; /regen retries the last turn;\n"
					                      "/read FILE sends a text file; /exit leaves the chat.\n"
					                      "/tools [NAME] lists accessible tools or shows a tool's schema.\n"
					                      "/tool NAME key=VALUE ... calls a tool directly; {JSON} is also accepted.\n"
					                      "/names lists known session participants; /refresh updates discovery.\n"
					                      "Tab completes tools, parameters, enum values, @names and leading nicknames.\n"
					                      "PgUp/PgDn search history by prefix; Ctrl-R searches history.\n"
					                      "End a line with \\ to continue it, or / to submit multiline input.\n"
					                      "Ctrl-C cancels an answer or tool call. Direct tool results stay out of chat history.");
					continue;
				}
				if (verb == "/tools") {
					cli_tools(client, rest);
					continue;
				}
				if (command == "/names") {
					std::string names = "Known session participants:";
					size_t count = autocomplete_participant_count(client.catalog.get());
					for (size_t i = 0; i < count; ++i) {
						names += ' ';
						names += cli_view(autocomplete_participant_at(client.catalog.get(), i));
					}
					cli_ui::show_message(!count
					                     ? "No participants discovered. /refresh reloads the shared notebooks."
					                     : names);
					continue;
				}
				if (command == "/refresh") {
					client.refresh();
					cli_ui::show_info("Tools and session participants refreshed.");
					continue;
				}
				if (verb == "/tool") {
					cli_tool_call(client, command);
					if (options.single)
						return 0;
					continue;
				}
				if (command == "/clear") {
					history.clear();
					if (tty)
						cli_ui::show_info("Conversation cleared.");
					continue;
				}
				if (command == "/regen") {
					if (history.empty())
						throw std::runtime_error("There is no previous answer to regenerate");
					prompt = history.back().first;
					regenerate = true;
				} else if (verb == "/read") {
					if (rest.empty())
						throw std::runtime_error("Usage: /read FILE");
					prompt = cli_read_file(std::string(rest));
				}
			}
			if (cli_trim(prompt).empty()) {
				if (options.single)
					throw std::runtime_error("The prompt is empty");
				continue;
			}
			size_t turns = history.size() - static_cast<size_t>(regenerate);
			std::string answer = cli_ask(client, options, history, turns, prompt);
			if (regenerate)
				history.pop_back();
			history.emplace_back(std::move(prompt), std::move(answer));
		} catch (const cli_interrupted &) {
			if (options.single || cli_signal == SIGTERM)
				return 128 + cli_signal;
			cli_signal = 0;
			signals.drain();
			cli_ui::show_info("Cancelled.");
		} catch (const std::exception &e) {
			cli_ui::show_error(e.what());
			if (options.single || !tty)
				return 1;
		}
		if (options.single)
			return 0;
	}
}

} // namespace

int main(int argc, char **argv)
{
	try {
		std::setlocale(LC_ALL, "");
		return cli_run(cli_args(argc, argv));
	} catch (const cli_interrupted &) {
		return 128 + cli_signal;
	} catch (const std::exception &e) {
		cli_console::write_error("askai-cli: ");
		cli_console::write_error(e.what());
		cli_console::write_error("\n");
		return 1;
	}
}
