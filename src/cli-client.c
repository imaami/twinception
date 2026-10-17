#include <curl/curl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "cli-client.h"
#include "cli-ui.h"
#include "jb.h"

int cli_error_set(struct cli_error *error, int e, char const *format, ...)
{
	va_list args;
	va_start(args, format);
	int n = vsnprintf(error->message, sizeof error->message, format, args);
	va_end(args);
	if (n < 0)
		error->message[0] = '\0';
	return e;
}

struct cli_text cli_error_text(struct cli_error const *error, int e)
{
	return cli_text_cstr(error->message[0] ? error->message : strerror(e));
}

struct cli_text cli_client_text(struct json_object *value)
{
	struct cli_text text = {};
	text.data = jb_str(value, &text.len);
	return text;
}

struct json_object *cli_client_string(struct cli_text text)
{
	return text.len <= CLI_LIMIT ? json_object_new_string_len(text.data ? text.data : "", (int)text.len) : nullptr;
}

int cli_client_wire(struct json_object *value, int flags,
                     struct cli_text *wire, struct cli_error *error)
{
	*wire = (struct cli_text){};
	if (!value)
		return ENOMEM;
	wire->data = json_object_to_json_string_length(value, flags, &wire->len);
	if (!wire->data)
		return ENOMEM;
	if (wire->len > CLI_LIMIT)
		return cli_error_set(error, EFBIG, "Conversation exceeds the 16 MiB client limit; use /clear");
	return 0;
}

struct cli_client cli_client(char const *socket, char const *tool,
                             volatile sig_atomic_t *interrupted, int wake_fd, int *e)
{
	struct cli_client client = {.socket = DSTR_INIT, .session = DSTR_INIT, .tool = DSTR_INIT,
	                            .session_key = DSTR_INIT, .participant = DSTR_INIT,
	                            .notes_tool = DSTR_INIT, .interrupted = interrupted, .wake_fd = wake_fd};
	*e = dstr_cat(&client.socket, socket) && dstr_cat(&client.tool, tool) ? 0 : ENOMEM;
	return client;
}

void cli_client_fini(struct cli_client *client)
{
	dstr_cleanup(&client->socket);
	dstr_cleanup(&client->session);
	dstr_cleanup(&client->tool);
	dstr_cleanup(&client->session_key);
	dstr_cleanup(&client->participant);
	dstr_cleanup(&client->notes_tool);
	autocomplete_destroy(&client->catalog);
	cli_client_json_cleanup(&client->notebooks);
}

struct cli_client_response {
	struct buf body, line, data;
	struct dstr session;
	struct cli_client *client;
	struct cli_error *error;
	struct json_object *result;
	cli_client_notify_fn *notify;
	void *context;
	int64_t id;
	int failure;
	bool sse;
};

static void cli_client_response_fini(struct cli_client_response *response)
{
	buf_fini(&response->body);
	buf_fini(&response->line);
	buf_fini(&response->data);
	dstr_cleanup(&response->session);
	cli_client_json_cleanup(&response->result);
}

static int cli_client_response_event(struct cli_client_response *response, struct cli_text text)
{
	cli_json_auto event = jb_parse(text.data, text.len,
	                               JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8, 0);
	if (!json_object_is_type(event, json_type_object))
		return cli_error_set(response->error, EPROTO, "Malformed JSON response from AskAI");
	struct json_object *id = jb_get(event, "id");
	if (id) {
		if (!response->id || !json_object_is_type(id, json_type_int) ||
		    json_object_get_int64(id) != response->id || response->result)
			return cli_error_set(response->error, EPROTO, "Unexpected JSON-RPC response id");
		response->result = json_object_get(event);
		return 0;
	}
	if (cli_client_is(jb_get(event, "method"), "notifications/tools/list_changed"))
		response->client->catalog_dirty = true;
	return response->notify ? response->notify(response->context, event, response->id, response->error) : 0;
}

static int cli_client_response_append(struct buf *buffer, struct cli_text bytes)
{
	if (!bytes.len)
		return 0;
	struct span room;
	int e = buf_reserve(buffer, bytes.len, CLI_LIMIT, &room);
	if (!e) {
		memcpy(room.ptr, bytes.data, bytes.len);
		buf_commit(buffer, bytes.len);
	}
	return e == ENOBUFS ? EFBIG : e;
}

static int cli_client_response_line(struct cli_client_response *response)
{
	struct buf *line = &response->line, *data = &response->data;
	if (line->tail && line->data[line->tail - 1] == '\r')
		--line->tail;
	int e = 0;
	if (!line->tail && data->tail) {
		e = cli_client_response_event(response, (struct cli_text){(char const *)data->data, data->tail - 1});
		data->tail = 0;
	} else if (line->tail >= 5 && !memcmp(line->data, "data:", 5)) {
		size_t offset = line->tail > 5 && line->data[5] == ' ' ? 6 : 5;
		e = cli_client_response_append(data, (struct cli_text){(char const *)line->data + offset, line->tail - offset});
		if (!e)
			e = cli_client_response_append(data, CLI_TEXT("\n"));
	}
	line->tail = 0;
	return e;
}

static size_t cli_client_response_receive(char *ptr, size_t size, size_t count, void *context)
{
	struct cli_client_response *response = context;
	if (size && count > SIZE_MAX / size) {
		response->failure = EOVERFLOW;
		return 0;
	}
	size_t bytes = size * count;
	if (!response->sse) {
		response->failure = cli_client_response_append(&response->body, (struct cli_text){ptr, bytes});
	} else {
		size_t offset = 0;
		while (offset < bytes && !response->failure) {
			char const *newline = memchr(ptr + offset, '\n', bytes - offset);
			size_t length = newline ? (size_t)(newline - ptr) - offset : bytes - offset;
			response->failure = cli_client_response_append(&response->line, (struct cli_text){ptr + offset, length});
			offset += length;
			if (newline && !response->failure) {
				++offset;
				response->failure = cli_client_response_line(response);
			}
		}
	}
	if (response->failure == EFBIG)
		cli_error_set(response->error, EFBIG, "AskAI response exceeds the 16 MiB client limit");
	return response->failure ? 0 : bytes;
}

static bool cli_client_equal_case(struct cli_text a, struct cli_text b)
{
	if (a.len != b.len)
		return false;
	for (size_t i = 0; i < a.len; ++i) {
		unsigned char x = (unsigned char)a.data[i], y = (unsigned char)b.data[i];
		if (x >= 'A' && x <= 'Z')
			x += 'a' - 'A';
		if (y >= 'A' && y <= 'Z')
			y += 'a' - 'A';
		if (x != y)
			return false;
	}
	return true;
}

static size_t cli_client_response_header(char *ptr, size_t size, size_t count, void *context)
{
	struct cli_client_response *response = context;
	if (size && count > SIZE_MAX / size) {
		response->failure = EOVERFLOW;
		return 0;
	}
	size_t bytes = size * count;
	struct cli_text line = {ptr, bytes};
	if (cli_text_starts(line, CLI_TEXT("HTTP/"))) {
		response->sse = false;
		dstr_clear(&response->session);
	}
	char const *colon = memchr(ptr, ':', bytes);
	if (!colon)
		return bytes;
	struct cli_text key = {ptr, (size_t)(colon - ptr)};
	struct cli_text value = cli_text_slice(line, key.len + 1, SIZE_MAX);
	while (value.len && (value.data[value.len - 1] == '\r' || value.data[value.len - 1] == '\n' ||
	                     value.data[value.len - 1] == ' ' || value.data[value.len - 1] == '\t'))
		--value.len;
	while (value.len && (*value.data == ' ' || *value.data == '\t')) {
		++value.data;
		--value.len;
	}
	if (cli_client_equal_case(key, CLI_TEXT("Content-Type"))) {
		char const *semi = memchr(value.data, ';', value.len);
		if (semi)
			value.len = (size_t)(semi - value.data);
		response->sse = cli_client_equal_case(value, CLI_TEXT("text/event-stream"));
	} else if (cli_client_equal_case(key, CLI_TEXT("Mcp-Session-Id"))) {
		bool valid = value.len <= 1024;
		for (size_t i = 0; valid && i < value.len; ++i)
			valid = (unsigned char)value.data[i] >= 0x21 && (unsigned char)value.data[i] <= 0x7e;
		if (!valid)
			response->failure = cli_error_set(response->error, EPROTO, "Invalid MCP session header");
		else {
			dstr_clear(&response->session);
			if (!dstr_cat_n(&response->session, value.data, value.len))
				response->failure = ENOMEM;
		}
	}
	return response->failure ? 0 : bytes;
}

static int cli_client_header(struct curl_slist **headers, char const *line)
{
	struct curl_slist *next = curl_slist_append(*headers, line);
	if (!next)
		return ENOMEM;
	*headers = next;
	return 0;
}

static int cli_client_post(struct cli_client *client, struct cli_text wire,
                           struct cli_client_response *response, bool interruptible, long timeout)
{
	CURL *easy = curl_easy_init();
	CURLM *multi = curl_multi_init();
	struct curl_slist *headers = nullptr;
	dstr_auto session_header = dstr(nullptr);
	int e = easy && multi ? 0 : ENOMEM;
	bool attached = false;
	if (e)
		goto done;
	e = cli_client_header(&headers, "Content-Type: application/json");
	if (!e)
		e = cli_client_header(&headers, "Accept: application/json, text/event-stream");
	if (!e)
		e = cli_client_header(&headers, "MCP-Protocol-Version: " CLI_VERSION);
	if (!e && dstr_len(&client->session)) {
		if (!dstr_cat(&session_header, "Mcp-Session-Id: ") || !dstr_cat(&session_header, dstr_get(&client->session)))
			e = ENOMEM;
		else
			e = cli_client_header(&headers, dstr_get(&session_header));
	}
	if (e)
		goto done;
#define CLI_OPTION(key, value) do { \
	CURLcode code = curl_easy_setopt(easy, (key), (value)); \
	if (code != CURLE_OK) { \
		e = cli_error_set(response->error, EIO, "%s", curl_easy_strerror(code)); \
		goto done; \
	} \
} while (0)
	CLI_OPTION(CURLOPT_URL, "http://localhost/mcp");
	CLI_OPTION(CURLOPT_UNIX_SOCKET_PATH, dstr_get(&client->socket));
	CLI_OPTION(CURLOPT_PROXY, "");
	CLI_OPTION(CURLOPT_NOPROXY, "*");
	CLI_OPTION(CURLOPT_FOLLOWLOCATION, 0L);
	CLI_OPTION(CURLOPT_NOSIGNAL, 1L);
	CLI_OPTION(CURLOPT_CONNECTTIMEOUT, 10L);
	CLI_OPTION(CURLOPT_TIMEOUT, timeout);
	CLI_OPTION(CURLOPT_POST, 1L);
	CLI_OPTION(CURLOPT_POSTFIELDS, wire.data);
	CLI_OPTION(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)wire.len);
	CLI_OPTION(CURLOPT_HTTPHEADER, headers);
	CLI_OPTION(CURLOPT_WRITEFUNCTION, cli_client_response_receive);
	CLI_OPTION(CURLOPT_WRITEDATA, response);
	CLI_OPTION(CURLOPT_HEADERFUNCTION, cli_client_response_header);
	CLI_OPTION(CURLOPT_HEADERDATA, response);
#undef CLI_OPTION
	CURLMcode code = curl_multi_add_handle(multi, easy);
	if (code != CURLM_OK)
		goto multi_error;
	attached = true;
	int active;
	do {
		if (interruptible && client->interrupted && *client->interrupted) {
			e = EINTR;
			goto done;
		}
		code = curl_multi_perform(multi, &active);
		if (code != CURLM_OK)
			goto multi_error;
		if (active) {
			struct curl_waitfd wake = {client->wake_fd, CURL_WAIT_POLLIN, 0};
			code = curl_multi_poll(multi, client->wake_fd >= 0 ? &wake : nullptr,
			                       client->wake_fd >= 0 ? 1 : 0, 1000, nullptr);
			if (code != CURLM_OK)
				goto multi_error;
			if (wake.revents) {
				char bytes[64];
				while (read(client->wake_fd, bytes, sizeof bytes) > 0) {}
			}
		}
	} while (active);
	e = response->failure;
	if (e)
		goto done;
	int left;
	CURLMsg *completed = curl_multi_info_read(multi, &left);
	if (!completed || completed->msg != CURLMSG_DONE || completed->data.result != CURLE_OK) {
		e = cli_error_set(response->error, EIO, "%s", completed && completed->msg == CURLMSG_DONE
		                  ? curl_easy_strerror(completed->data.result) : "HTTP transfer did not complete");
		goto done;
	}
	long status;
	if (curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) {
		e = cli_error_set(response->error, EIO, "Cannot read HTTP status");
		goto done;
	}
	if (status < 200 || status >= 300) {
		size_t length = response->body.tail < 1024 ? response->body.tail : 1024;
		e = cli_error_set(response->error, EPROTO, "AskAI HTTP %ld%s%.*s", status, length ? ": " : "",
		                  (int)length, length ? (char const *)response->body.data : "");
	} else if (response->sse && (response->line.tail || response->data.tail)) {
		e = cli_error_set(response->error, EPROTO, "AskAI closed an incomplete SSE event");
	}
	goto done;
multi_error:
	e = cli_error_set(response->error, EIO, "%s", curl_multi_strerror(code));
done:
	if (attached)
		(void)curl_multi_remove_handle(multi, easy);
	if (easy)
		curl_easy_cleanup(easy);
	if (multi)
		(void)curl_multi_cleanup(multi);
	curl_slist_free_all(headers);
	return e;
}

static struct json_object *cli_client_packet(char const *method, struct json_object *params, int64_t id)
{
	struct json_object *message = jb_object("jsonrpc", json_object_new_string("2.0"),
	                                        "method", json_object_new_string(method),
	                                        "params", params, nullptr);
	if (id && jb_set(message, "id", json_object_new_int64(id))) {
		json_object_put(message);
		message = nullptr;
	}
	return message;
}

static int cli_client_notification(struct cli_client *client, char const *method,
                                   struct json_object *params, bool interruptible,
                                   long timeout, struct cli_error *error)
{
	cli_json_auto message = cli_client_packet(method, params, 0);
	struct cli_text wire;
	int e = cli_client_wire(message, JB_WIRE, &wire, error);
	struct cli_client_response response = {.session = DSTR_INIT, .client = client, .error = error};
	if (!e)
		e = cli_client_post(client, wire, &response, interruptible, timeout);
	cli_client_response_fini(&response);
	return e;
}

static void cli_client_cancel(struct cli_client *client, int64_t id)
{
	struct cli_error error = {};
	struct json_object *params = jb_object("requestId", json_object_new_int64(id),
	                                       "reason", json_object_new_string("Interrupted by the terminal user"), nullptr);
	int e = cli_client_notification(client, "notifications/cancelled", params, false, 5, &error);
	if (e)
		(void)cli_ui_error(CLI_TEXT("Cancellation could not be confirmed"), cli_error_text(&error, e));
}

int cli_client_rpc(struct cli_client *client, char const *method,
                    struct json_object *params, cli_client_notify_fn *notify,
                    void *context, struct json_object **result, struct cli_error *error)
{
	*result = nullptr;
	if (client->next_id == INT64_MAX) {
		json_object_put(params);
		return EOVERFLOW;
	}
	int64_t id = ++client->next_id;
	cli_json_auto message = cli_client_packet(method, params, id);
	struct cli_client_response response = {.session = DSTR_INIT, .client = client, .error = error,
	                                       .notify = notify, .context = context, .id = id};
	struct cli_text wire;
	int e = cli_client_wire(message, JB_WIRE, &wire, error);
	bool call = !strcmp(method, "tools/call");
	if (!e)
		e = cli_client_post(client, wire, &response, true, call ? 0 : 30);
	if (e == EINTR && call)
		cli_client_cancel(client, id);
	if (!e && dstr_len(&response.session)) {
		dstr_cleanup(&client->session);
		client->session = dstr_move(&response.session);
	}
	if (!e && !response.sse)
		e = cli_client_response_event(&response, (struct cli_text){(char const *)response.body.data, response.body.tail});
	if (!e && !response.result)
		e = cli_error_set(error, EPROTO, "AskAI closed the stream without a result");
	if (!e) {
		struct json_object *failure = jb_get(response.result, "error");
		struct json_object *value = jb_get(response.result, "result");
		if (failure) {
			struct cli_text text = cli_client_text(jb_get(failure, "message"));
			e = cli_error_set(error, EPROTO, "AskAI: %.*s", (int)text.len, text.data ? text.data : "Invalid error response");
		} else if (!json_object_is_type(value, json_type_object)) {
			e = cli_error_set(error, EPROTO, "AskAI returned no result object");
		} else {
			*result = json_object_get(value);
		}
	}
	cli_client_response_fini(&response);
	return e;
}

int cli_client_connect(struct cli_client *client, struct cli_error *error)
{
	struct json_object *params = jb_object("protocolVersion", json_object_new_string(CLI_VERSION),
	                                       "capabilities", json_object_new_object(),
	                                       "clientInfo", jb_object("name", json_object_new_string("askai-cli"),
	                                                               "version", json_object_new_string("1"), nullptr), nullptr);
	cli_json_auto initialized = nullptr;
	int e = cli_client_rpc(client, "initialize", params, nullptr, nullptr, &initialized, error);
	if (!e && (!cli_client_is(jb_get(initialized, "protocolVersion"), CLI_VERSION) || !dstr_len(&client->session)))
		e = cli_error_set(error, EPROTO, "AskAI did not negotiate the supported MCP session");
	if (!e)
		e = cli_client_notification(client, "notifications/initialized", json_object_new_object(), true, 30, error);
	return e ? e : cli_client_refresh(client, error);
}

static int cli_client_set_text(struct dstr *dest, struct cli_text text)
{
	dstr_clear(dest);
	return dstr_cat_n(dest, text.data, text.len) ? 0 : ENOMEM;
}

static bool cli_client_charset(struct cli_text text, char const *allowed)
{
	for (size_t i = 0; i < text.len; ++i)
		if (!text.data[i] || !strchr(allowed, (unsigned char)text.data[i]))
			return false;
	return true;
}

static int cli_client_read_notebooks(struct cli_text text, struct json_object *names,
                                      struct json_object *notebooks)
{
	/* A truncated last line is never an authoritative notebook selector. */
	while (text.len) {
		char const *newline = memchr(text.data, '\n', text.len);
		if (!newline)
			break;
		struct cli_text line = {text.data, (size_t)(newline - text.data)};
		text = cli_text_slice(text, line.len + 1, SIZE_MAX);
		char const *first = memchr(line.data, '/', line.len);
		if (!first || first == line.data || first - line.data > 64)
			continue;
		struct cli_text name = {line.data, (size_t)(first - line.data)};
		struct cli_text rest = cli_text_slice(line, name.len + 1, SIZE_MAX);
		char const *last = memchr(rest.data, '/', rest.len);
		if (!last)
			continue;
		struct cli_text principal = {rest.data, (size_t)(last - rest.data)};
		struct cli_text side = cli_text_slice(rest, principal.len + 1, SIZE_MAX);
		if ((!cli_text_equal(side, CLI_TEXT("client")) && !cli_text_equal(side, CLI_TEXT("upstream"))) ||
		    !cli_client_charset(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"))
			continue;
		if (!cli_text_equal(principal, CLI_TEXT("anonymous")) &&
		    (principal.len != 16 || !cli_client_charset(principal, "0123456789abcdef")))
			continue;
		int e = jb_push(names, cli_client_string(name));
		if (!e)
			e = jb_push(notebooks, cli_client_string(line));
		if (e)
			return e;
	}
	return 0;
}

struct cli_client_names {
	struct autocomplete_text *data;
	size_t count, capacity;
};

static void cli_client_names_fini(struct cli_client_names *names)
{
	free(names->data);
	names->data = nullptr;
	names->count = names->capacity = 0;
}

static int cli_client_names_add(struct cli_client_names *names, struct cli_text text)
{
	if (names->count == 65536)
		return E2BIG;
	if (names->count == names->capacity) {
		size_t capacity = names->capacity ? names->capacity * 2 : 256;
		void *data = realloc(names->data, capacity * sizeof *names->data);
		if (!data)
			return ENOMEM;
		names->data = data;
		names->capacity = capacity;
	}
	names->data[names->count++] = (struct autocomplete_text){text.data, text.len};
	return 0;
}

static int cli_client_read_files(struct cli_text text, struct cli_client_names *files)
{
	while (text.len) {
		char const *newline = memchr(text.data, '\n', text.len);
		if (!newline)
			break;
		struct cli_text line = {text.data, (size_t)(newline - text.data)};
		text = cli_text_slice(text, line.len + 1, SIZE_MAX);
		char const *dot = memchr(line.data, '.', line.len);
		if (!dot)
			continue;
		size_t index = (size_t)(dot - line.data);
		if (!index || index + 1 >= line.len || line.data[index + 1] != ' ' ||
		    !cli_client_charset(cli_text_slice(line, 0, index), "0123456789"))
			continue;
		line = cli_text_slice(line, index + 2, SIZE_MAX);
		size_t const date_size = sizeof "2026-10-14 12:34 UTC  " - 1;
		if (line.len <= date_size || !cli_text_equal(cli_text_slice(line, 17, 5), CLI_TEXT("UTC  ")))
			continue;
		line = cli_text_slice(line, date_size, SIZE_MAX);
		if (!line.len || line.data[line.len - 1] != ')')
			continue;
		size_t tail = line.len;
		while (tail && !cli_text_starts(cli_text_slice(line, tail, SIZE_MAX), CLI_TEXT("  (")))
			--tail;
		if (!tail || line.len - tail < 4)
			continue;
		struct cli_text count = cli_text_slice(line, tail + 3, line.len - tail - 4);
		if (!cli_text_equal(count, CLI_TEXT("binary"))) {
			char const *space = memchr(count.data, ' ', count.len);
			if (!space || space == count.data)
				continue;
			size_t digits = (size_t)(space - count.data);
			struct cli_text suffix = cli_text_slice(count, digits, SIZE_MAX);
			if (!cli_client_charset(cli_text_slice(count, 0, digits), "0123456789") ||
			    (!cli_text_equal(suffix, CLI_TEXT(" line")) && !cli_text_equal(suffix, CLI_TEXT(" lines"))))
				continue;
		}
		int e = cli_client_names_add(files, cli_text_slice(line, 0, tail));
		if (e)
			return e;
	}
	return 0;
}

static int cli_client_name_compare(void const *a, void const *b)
{
	struct cli_text x = cli_client_text(*(struct json_object *const *)a);
	struct cli_text y = cli_client_text(*(struct json_object *const *)b);
	size_t length = x.len < y.len ? x.len : y.len;
	int order = length ? memcmp(x.data, y.data, length) : 0;
	return order ? order : (x.len > y.len) - (x.len < y.len);
}

static void cli_client_unique(struct json_object *names)
{
	json_object_array_sort(names, cli_client_name_compare);
	for (size_t i = json_object_array_length(names); i > 1; --i)
		if (cli_text_equal(cli_client_text(jb_at(names, i - 1)), cli_client_text(jb_at(names, i - 2))))
			(void)json_object_array_del_idx(names, i - 1, 1);
}

static int cli_client_discover_files(struct cli_client *client, struct autocomplete *catalog,
                                     struct cli_text tool, struct json_object *notebooks,
                                     struct cli_error *error)
{
	cli_client_unique(notebooks);
	size_t count = json_object_array_length(notebooks);
	if (count > 128)
		return cli_error_set(error, E2BIG, "More than 128 notebooks; filename discovery is bounded to 128");
	struct cli_client_names files = {};
	int e = 0;
	for (size_t i = 0; !e && i < count; ++i) {
		struct cli_text notebook = cli_client_text(jb_at(notebooks, i));
		struct json_object *params = jb_object("name", cli_client_string(tool),
		                                      "arguments", jb_object("action", json_object_new_string("list"),
		                                                             "notebook", cli_client_string(notebook), nullptr), nullptr);
		cli_json_auto listing = nullptr;
		e = cli_client_rpc(client, "tools/call", params, nullptr, nullptr, &listing, error);
		if (e)
			break;
		if (json_object_get_boolean(jb_get(listing, "isError")))
			continue;
		struct json_object *content = jb_get(listing, "content");
		if (!json_object_is_type(content, json_type_array)) {
			e = cli_error_set(error, EPROTO, "Invalid note filename discovery result");
			break;
		}
		files.count = 0;
		for (size_t j = 0, n = json_object_array_length(content); !e && j < n; ++j) {
			struct json_object *block = jb_at(content, j);
			if (cli_client_is(jb_get(block, "type"), "text")) {
				struct cli_text text = cli_client_text(jb_get(block, "text"));
				e = text.data ? cli_client_read_files(text, &files) : EPROTO;
			}
		}
		if (!e)
			e = autocomplete_set_note_files(catalog, (struct autocomplete_text){tool.data, tool.len},
			                                (struct autocomplete_text){notebook.data, notebook.len}, files.data, files.count);
	}
	cli_client_names_fini(&files);
	return e;
}

int cli_client_refresh_notes(struct cli_client *client, struct cli_error *error)
{
	client->notes_dirty = false;
	if (!dstr_len(&client->notes_tool))
		return 0;
	autocomplete_clear_note_files(client->catalog);
	return cli_client_discover_files(client, client->catalog, cli_text_dstr(&client->notes_tool), client->notebooks, error);
}

static int cli_client_discover_books(struct cli_client *client, struct cli_text tool,
                                     struct json_object *names, struct json_object *notebooks,
                                     struct cli_error *error)
{
	struct json_object *params = jb_object("name", cli_client_string(tool),
	                                      "arguments", jb_object("action", json_object_new_string("notebooks"), nullptr), nullptr);
	cli_json_auto listing = nullptr;
	int e = cli_client_rpc(client, "tools/call", params, nullptr, nullptr, &listing, error);
	if (e)
		return e;
	if (json_object_get_boolean(jb_get(listing, "isError")))
		return cli_error_set(error, EPROTO, "The notes tool rejected notebook discovery");
	struct json_object *content = jb_get(listing, "content");
	if (!json_object_is_type(content, json_type_array))
		return cli_error_set(error, EPROTO, "Invalid notebook discovery result");
	for (size_t i = 0, n = json_object_array_length(content); !e && i < n; ++i) {
		struct json_object *block = jb_at(content, i);
		if (cli_client_is(jb_get(block, "type"), "text")) {
			struct cli_text text = cli_client_text(jb_get(block, "text"));
			e = text.data ? cli_client_read_notebooks(text, names, notebooks) : EPROTO;
		}
	}
	return e;
}

static bool cli_client_has_action(struct json_object *actions, char const *name)
{
	if (!json_object_is_type(actions, json_type_array))
		return false;
	for (size_t i = 0, count = json_object_array_length(actions); i < count; ++i)
		if (cli_client_is(jb_at(actions, i), name))
			return true;
	return false;
}

static int cli_client_catalog_names(struct autocomplete *catalog, struct json_object *names, bool notebooks)
{
	cli_client_unique(names);
	struct cli_client_names views = {};
	int e = 0;
	for (size_t i = 0, count = json_object_array_length(names); !e && i < count; ++i)
		e = cli_client_names_add(&views, cli_client_text(jb_at(names, i)));
	if (!e)
		e = notebooks ? autocomplete_set_notebooks(catalog, views.data, views.count)
		              : autocomplete_set_participants(catalog, views.data, views.count);
	cli_client_names_fini(&views);
	return e;
}

int cli_client_refresh(struct cli_client *client, struct cli_error *error)
{
	client->catalog_dirty = false;
	int e = 0;
	struct autocomplete *catalog = autocomplete_create(&e);
	cli_json_auto names = json_object_new_array();
	cli_json_auto notebooks = json_object_new_array();
	dstr_auto notebook_tool = dstr(nullptr), session = dstr(nullptr), participant = dstr(nullptr);
	dstr_auto cursor = dstr(nullptr), chosen = dstr(nullptr);
	bool files = false, complete = false;
	if (!catalog || !names || !notebooks) {
		e = ENOMEM;
		goto done;
	}
	for (unsigned page = 0; !e && page < 32; ++page) {
		struct json_object *params = json_object_new_object();
		if (dstr_len(&cursor) && jb_set(params, "cursor", cli_client_string(cli_text_dstr(&cursor)))) {
			json_object_put(params);
			e = ENOMEM;
			break;
		}
		cli_json_auto result = nullptr;
		e = cli_client_rpc(client, "tools/list", params, nullptr, nullptr, &result, error);
		if (e)
			break;
		struct json_object *tools = jb_get(result, "tools");
		if (!json_object_is_type(tools, json_type_array)) {
			e = cli_error_set(error, EPROTO, "AskAI returned an invalid tool list");
			break;
		}
		for (size_t i = 0, count = json_object_array_length(tools); !e && i < count; ++i) {
			struct json_object *entry = jb_at(tools, i);
			e = autocomplete_add_tool(catalog, entry);
			struct cli_text name = cli_client_text(jb_get(entry, "name"));
			if (e || !name.data) {
				if (!e)
					e = EPROTO;
				break;
			}
			struct json_object *props = jb_get(jb_get(entry, "inputSchema"), "properties");
			struct json_object *meta = jb_get(jb_get(entry, "_meta"), "askai");
			struct json_object *books = jb_get(meta, "notebooks");
			struct json_object *actions = jb_get(jb_get(props, "action"), "enum");
			bool source = json_object_is_type(books, json_type_boolean) && json_object_get_boolean(books);
			if (!meta && cli_text_equal(name, CLI_TEXT("notes")) && jb_get(props, "notebook"))
				source |= cli_client_has_action(actions, "notebooks");
			if (source && !dstr_len(&notebook_tool)) {
				e = cli_client_set_text(&notebook_tool, name);
				struct json_object *self = jb_get(meta, "participant");
				if (!e && json_object_is_type(self, json_type_string)) {
					e = jb_push(names, json_object_get(self));
					if (!e)
						e = cli_client_set_text(&participant, cli_client_text(self));
				}
				struct json_object *identity = jb_get(meta, "session");
				if (!e && json_object_is_type(identity, json_type_string))
					e = cli_client_set_text(&session, cli_client_text(identity));
				files = jb_get(props, "file") && jb_get(props, "notebook") && cli_client_has_action(actions, "list");
			}
			bool candidate = dstr_len(&client->tool) ? cli_text_equal(name, cli_text_dstr(&client->tool))
			                                       : cli_client_is(jb_get(entry, "title"), "Ask AI");
			if (e || !candidate)
				continue;
			if (!jb_get(props, "prompt") || !jb_get(props, "messages") || !jb_get(props, "background"))
				e = cli_error_set(error, EPROTO, "The selected AskAI tool needs the chat extension; rebuild the server too");
			else if (dstr_len(&chosen))
				e = cli_error_set(error, EPROTO, "Multiple chat tools; select one with --tool");
			else
				e = cli_client_set_text(&chosen, name);
		}
		if (e)
			break;
		struct json_object *next = jb_get(result, "nextCursor");
		if (!next) {
			complete = true;
			break;
		}
		struct cli_text after = cli_client_text(next);
		if (!after.len || cli_text_equal(after, cli_text_dstr(&cursor)))
			e = cli_error_set(error, EPROTO, "Invalid tool-list cursor");
		else
			e = cli_client_set_text(&cursor, after);
	}
	if (!e && !complete)
		e = cli_error_set(error, E2BIG, "Too many tool-list pages");
	if (!e && !dstr_len(&chosen))
		e = cli_error_set(error, EPROTO, "No accessible AskAI chat tool was found");
	if (e)
		goto done;
	if (dstr_len(&notebook_tool)) {
		e = cli_client_discover_books(client, cli_text_dstr(&notebook_tool), names, notebooks, error);
		if (e == EINTR || e == ENOMEM)
			goto done;
		if (e) {
			(void)cli_ui_error(CLI_TEXT("Participant discovery failed"), cli_error_text(error, e));
			error->message[0] = '\0';
			e = 0;
		}
	}
	if (files) {
		e = jb_push(notebooks, json_object_new_string("own"));
		if (!e)
			e = jb_push(notebooks, json_object_new_string("other"));
	}
	if (!e)
		e = cli_client_catalog_names(catalog, names, false);
	if (!e)
		e = cli_client_catalog_names(catalog, notebooks, true);
	if (e)
		goto done;
	if (files) {
		e = cli_client_discover_files(client, catalog, cli_text_dstr(&notebook_tool), notebooks, error);
		if (e == EINTR || e == ENOMEM)
			goto done;
		if (e) {
			(void)cli_ui_error(CLI_TEXT("Note filename discovery failed"), cli_error_text(error, e));
			error->message[0] = '\0';
			e = 0;
		}
	}
	if (client->catalog && (!cli_text_equal(cli_text_dstr(&client->session_key), cli_text_dstr(&session)) ||
	                        !cli_text_equal(cli_text_dstr(&client->participant), cli_text_dstr(&participant)) ||
	                        !cli_text_equal(cli_text_dstr(&client->tool), cli_text_dstr(&chosen)))) {
		client->identity_changed = true;
		e = cli_error_set(error, ESTALE, "AskAI changed its session, participant or ask tool; restart the client to select its history");
		goto done;
	}
	autocomplete_destroy(&client->catalog);
	client->catalog = catalog;
	catalog = nullptr;
	dstr_cleanup(&client->tool);
	client->tool = dstr_move(&chosen);
	dstr_cleanup(&client->notes_tool);
	if (files)
		client->notes_tool = dstr_move(&notebook_tool);
	cli_client_json_cleanup(&client->notebooks);
	client->notebooks = notebooks;
	notebooks = nullptr;
	client->notes_dirty = false;
	dstr_cleanup(&client->session_key);
	client->session_key = dstr_move(&session);
	dstr_cleanup(&client->participant);
	client->participant = dstr_move(&participant);
done:
	autocomplete_destroy(&catalog);
	return e;
}
