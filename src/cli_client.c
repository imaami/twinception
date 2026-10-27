#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "buf.h"
#include "cli_client.h"
#include "cli_network.h"
#include "compat.h"
#include "jb.h"
#include "util.h"

int
cli_error_set (struct cli_error *error,
               int               e,
               char const       *format, ...)
{
	va_list args;
	va_start(args, format);
	int n = vsnprintf(error->message, sizeof error->message, format, args);
	va_end(args);
	if (n < 0)
		error->message[0] = '\0';
	return e;
}

struct cstr_view cli_error_text(struct cli_error const *error, int e)
{
	return cstr_view(error->message[0] ? error->message : strerror(e));
}

struct cstr_view cli_client_text(struct json_object *value)
{
	struct cstr_view text = {};
	text.data = jb_str(value, &text.len);
	return text;
}

struct json_object *cli_client_string(struct cstr_view text)
{
	return text.len <= CLI_LIMIT ? json_object_new_string_len(text.data ? text.data : "", (int)text.len) : nullptr;
}

int
cli_client_wire (struct json_object *value,
                 int                 flags,
                 struct cstr_view   *wire,
                 struct cli_error   *error)
{
	*wire = (struct cstr_view){};
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
                             struct cli_network *network, int *e)
{
	struct cli_client client = {.socket = DSTR_INIT, .session = DSTR_INIT, .tool = DSTR_INIT,
	                            .session_key = DSTR_INIT, .participant = DSTR_INIT,
	                            .notes_tool = DSTR_INIT, .network = network};
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

static struct cli_client_response
cli_client_response (struct cli_client    *client,
                     cli_client_notify_fn *notify,
                     void                 *context,
                     int64_t               id,
                     struct cli_error     *error)
{
	return (struct cli_client_response){
		.session = DSTR_INIT, .client = client, .error = error,
		.notify = notify, .context = context, .id = id,
	};
}

static void cli_client_response_fini(struct cli_client_response *response)
{
	buf_fini(&response->body);
	buf_fini(&response->line);
	buf_fini(&response->data);
	dstr_cleanup(&response->session);
	cli_client_json_cleanup(&response->result);
}

static int cli_client_response_event(struct cli_client_response *response, struct cstr_view text)
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

static int cli_client_response_append(struct buf *buffer, struct cstr_view bytes)
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
		e = cli_client_response_event(response, (struct cstr_view){(char const *)data->data, data->tail - 1});
		data->tail = 0;
	} else if (line->tail >= 5 && !memcmp(line->data, "data:", 5)) {
		size_t offset = line->tail > 5 && line->data[5] == ' ' ? 6 : 5;
		e = cli_client_response_append(data, (struct cstr_view){(char const *)line->data + offset, line->tail - offset});
		if (!e)
			e = cli_client_response_append(data, CSTR_VIEW("\n"));
	}
	line->tail = 0;
	return e;
}

static int cli_client_response_receive(char const *ptr, size_t bytes, void *context)
{
	struct cli_client_response *response = context;
	if (!response->sse) {
		response->failure = cli_client_response_append(&response->body, (struct cstr_view){ptr, bytes});
	} else {
		size_t offset = 0;
		while (offset < bytes && !response->failure) {
			char const *newline = memchr(ptr + offset, '\n', bytes - offset);
			size_t length = newline ? (size_t)(newline - ptr) - offset : bytes - offset;
			response->failure = cli_client_response_append(&response->line, (struct cstr_view){ptr + offset, length});
			offset += length;
			if (newline && !response->failure) {
				++offset;
				response->failure = cli_client_response_line(response);
			}
		}
	}
	if (response->failure == EFBIG)
		cli_error_set(response->error, EFBIG, "AskAI response exceeds the 16 MiB client limit");
	return response->failure;
}

/* Whether the ASCII text a equals the literal b, ignoring case. */
static bool cli_client_equal_case(struct cstr_view a, struct cstr_view b)
{
	return a.len == b.len && !strncasecmp(a.data, b.data, a.len);
}

static int cli_client_response_header(char const *ptr, size_t bytes, void *context)
{
	struct cli_client_response *response = context;
	struct cstr_view line = {ptr, bytes};
	if (cstr_view_starts(line, CSTR_VIEW("HTTP/"))) {
		response->sse = false;
		dstr_clear(&response->session);
	}
	char const *colon = memchr(ptr, ':', bytes);
	if (!colon)
		return 0;
	struct cstr_view key = {ptr, (size_t)(colon - ptr)};
	struct cstr_view value = cstr_view_slice(line, key.len + 1, SIZE_MAX);
	while (value.len && (value.data[value.len - 1] == '\r' || value.data[value.len - 1] == '\n' ||
	                     value.data[value.len - 1] == ' ' || value.data[value.len - 1] == '\t'))
		--value.len;
	while (value.len && (*value.data == ' ' || *value.data == '\t')) {
		++value.data;
		--value.len;
	}
	if (cli_client_equal_case(key, CSTR_VIEW("Content-Type"))) {
		char const *semi = memchr(value.data, ';', value.len);
		if (semi)
			value.len = (size_t)(semi - value.data);
		response->sse = cli_client_equal_case(value, CSTR_VIEW("text/event-stream"));
	} else if (cli_client_equal_case(key, CSTR_VIEW("Mcp-Session-Id"))) {
		bool valid = value.len <= 1024;
		for (size_t i = 0; valid && i < value.len; ++i)
			valid = (unsigned char)value.data[i] >= 0x21 && (unsigned char)value.data[i] <= 0x7e;
		response->failure = valid ? cstr_view_store(value, &response->session)
		                          : cli_error_set(response->error, EPROTO, "Invalid MCP session header");
	}
	return response->failure;
}

static int
cli_client_response_status (struct cli_client_response      *response,
                            int                              e,
                            struct cli_network_result const *result)
{
	if (response->failure)
		return response->failure;
	if (e)
		return result->message[0] ? cli_error_set(response->error, e, "%s", result->message) : e;
	if (result->status < 200 || result->status >= 300) {
		size_t length = response->body.tail < 1024 ? response->body.tail : 1024;
		return cli_error_set(response->error, EPROTO, "AskAI HTTP %ld%s%.*s", result->status,
		                      length ? ": " : "", (int)length,
		                      length ? (char const *)response->body.data : "");
	}
	if (response->sse && (response->line.tail || response->data.tail))
		return cli_error_set(response->error, EPROTO, "AskAI closed an incomplete SSE event");
	return 0;
}

static int cli_client_post(struct cli_client *client, struct cstr_view wire,
                           struct cli_client_response *response, bool interruptible, long timeout)
{
	struct cli_network_result result;
	int e = cli_network_perform(client->network, cstr_view_dstr(&client->socket),
	                            cstr_view_dstr(&client->session), wire, timeout, interruptible,
	                            cli_client_response_header, cli_client_response_receive, response, &result);
	return cli_client_response_status(response, e, &result);
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
	struct cli_client_response response = cli_client_response(client, nullptr, nullptr, 0, error);
	struct cstr_view wire;
	int e = cli_client_wire(message, JB_WIRE, &wire, error);
	if (!e)
		e = cli_client_post(client, wire, &response, interruptible, timeout);
	cli_client_response_fini(&response);
	return e;
}

static int
cli_client_response_result (struct cli_client_response *response,
                            struct json_object        **result)
{
	if (dstr_len(&response->session)) {
		dstr_cleanup(&response->client->session);
		response->client->session = dstr_move(&response->session);
	}
	int e = response->sse ? 0 : cli_client_response_event(response,
	               (struct cstr_view){(char const *)response->body.data, response->body.tail});
	if (e)
		return e;
	if (!response->result)
		return cli_error_set(response->error, EPROTO, "AskAI closed the stream without a result");
	struct json_object *failure = jb_get(response->result, "error");
	struct json_object *value = jb_get(response->result, "result");
	if (failure) {
		struct cstr_view text = cli_client_text(jb_get(failure, "message"));
		return cli_error_set(response->error, EPROTO, "AskAI: %.*s", (int)text.len,
		                      text.data ? text.data : "Invalid error response");
	}
	if (!json_object_is_type(value, json_type_object))
		return cli_error_set(response->error, EPROTO, "AskAI returned no result object");
	*result = json_object_get(value);
	return 0;
}

struct cli_client_call {
	struct cli_client_response response;
	struct cli_error           error;
	cli_client_done_fn         *done;
};

static int cli_client_call_start (struct cli_client *client, char const *method,
                                  struct json_object *params, cli_client_notify_fn *notify,
                                  cli_client_done_fn *done, void *context, int64_t id,
                                  uint64_t *request, struct cli_error *error);

static void
cli_client_unconfirmed (struct cli_client      *client,
                        int                     e,
                        struct cli_error const *error)
{
	if (e && e != ESHUTDOWN && client->report)
		(void)client->report(client, CSTR_VIEW("Cancellation could not be confirmed"),
		                     cli_error_text(error, e));
}

/* Tell AskAI that the call id was interrupted. With wait, wait up to 5 s for
 * the reply; otherwise cli_client_call_done receives it. */
static void
cli_client_cancel (struct cli_client *client,
                   int64_t            id,
                   bool               wait)
{
	struct json_object *params = jb_object("requestId", json_object_new_int64(id),
	                                       "reason", json_object_new_string("Interrupted by the terminal user"), nullptr);
	struct cli_error error = {};
	uint64_t request;
	int e = wait ? cli_client_notification(client, "notifications/cancelled", params, false, 5, &error)
	             : cli_client_call_start(client, "notifications/cancelled", params,
	                                     nullptr, nullptr, nullptr, 0, &request, &error);
	cli_client_unconfirmed(client, e, &error);
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
	struct cli_client_response response = cli_client_response(client, notify, context, id, error);
	struct cstr_view wire;
	int e = cli_client_wire(message, JB_WIRE, &wire, error);
	bool call = !strcmp(method, "tools/call");
	if (!e)
		e = cli_client_post(client, wire, &response, true, call ? 0 : 30);
	if (e == EINTR && call)
		cli_client_cancel(client, id, true);
	if (!e)
		e = cli_client_response_result(&response, result);
	cli_client_response_fini(&response);
	return e;
}

static struct cli_client_call *
cli_client_call_create (struct cli_client    *client,
                        cli_client_notify_fn *notify,
                        cli_client_done_fn   *done,
                        void                 *context,
                        int64_t               id,
                        int                  *e)
{
	struct cli_client_call *call = malloc(sizeof *call);
	*e = call ? 0 : ENOMEM;
	if (call)
		*call = (struct cli_client_call){
			.response = cli_client_response(client, notify, context, id, &call->error),
			.done = done,
		};
	return call;
}

static void
cli_client_call_destroy (struct cli_client_call **ptr)
{
	struct cli_client_call *call = *ptr;
	if (!call)
		return;
	cli_client_response_fini(&call->response);
	free(call);
	*ptr = nullptr;
}

static void
cli_client_call_done (void                            *context,
                      int                              e,
                      struct cli_network_result const *status)
{
	struct cli_client_response *response = context;
	struct cli_client_call *call = container_of(response, struct cli_client_call, response);
	struct cli_client *client = response->client;
	e = cli_client_response_status(response, e, status);
	if (e == EINTR && response->id)
		cli_client_cancel(client, response->id, false);
	cli_json_auto result = nullptr;
	if (!e && response->id)
		e = cli_client_response_result(response, &result);
	if (call->done)
		call->done(response->context, e, result, &call->error);
	else
		cli_client_unconfirmed(client, e, &call->error);
	cli_client_call_destroy(&call);
}

static int
cli_client_call_start (struct cli_client    *client,
                       char const           *method,
                       struct json_object   *params,
                       cli_client_notify_fn *notify,
                       cli_client_done_fn   *done,
                       void                 *context,
                       int64_t               id,
                       uint64_t             *request,
                       struct cli_error     *error)
{
	cli_json_auto message = cli_client_packet(method, params, id);
	struct cstr_view wire;
	int e = cli_client_wire(message, JB_WIRE, &wire, error);
	if (e)
		return e;
	struct cli_client_call *call = cli_client_call_create(client, notify, done, context, id, &e);
	if (!call)
		return e;
	long timeout = !id ? 5 : !strcmp(method, "tools/call") ? 0 : 30;
	e = cli_network_submit(client->network, cstr_view_dstr(&client->socket),
	                        cstr_view_dstr(&client->session), wire, timeout, id != 0,
	                        cli_client_response_header, cli_client_response_receive,
	                        &call->response, cli_client_call_done, request);
	if (e) {
		cli_client_call_destroy(&call);
		if (e == EAGAIN || e == ENOBUFS)
			cli_error_set(error, e, "Too many active requests; retry when a tool completes");
	}
	return e;
}

int
cli_client_submit (struct cli_client    *client,
                   char const           *method,
                   struct json_object   *params,
                   cli_client_notify_fn *notify,
                   cli_client_done_fn   *done,
                   void                 *context,
                   uint64_t             *request,
                   struct cli_error     *error)
{
	*request = 0;
	if (client->next_id == INT64_MAX || !done) {
		json_object_put(params);
		return done ? EOVERFLOW : EINVAL;
	}
	return cli_client_call_start(client, method, params, notify, done, context,
	                              ++client->next_id, request, error);
}

void
cli_client_cancel_request (struct cli_client *client,
                           uint64_t           request)
{
	cli_network_cancel_call(client->network, request, false);
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

static bool cli_client_digits(struct cstr_view text)
{
	for (size_t i = 0; i < text.len; ++i)
		if (text.data[i] < '0' || text.data[i] > '9')
			return false;
	return true;
}

/* Parse notebook selectors into lists[0], the participant names, and
 * lists[1], the notebooks. */
static int
cli_client_read_notebooks (struct cstr_view  text,
                           void             *lists)
{
	struct json_object *names = ((struct json_object **)lists)[0];
	struct json_object *notebooks = ((struct json_object **)lists)[1];
	/* A truncated last line is never an authoritative notebook selector. */
	while (text.len) {
		char const *newline = memchr(text.data, '\n', text.len);
		if (!newline)
			break;
		struct cstr_view line = {text.data, (size_t)(newline - text.data)};
		text = cstr_view_slice(text, line.len + 1, SIZE_MAX);
		struct cstr_view self = CSTR_VIEW("Self (writable): ");
		struct cstr_view peer = CSTR_VIEW(" (read-only)");
		if (cstr_view_starts(line, self))
			line = cstr_view_slice(line, self.len, SIZE_MAX);
		if (line.len >= peer.len &&
		    cstr_view_equal(cstr_view_slice(line, line.len - peer.len, SIZE_MAX), peer))
			line.len -= peer.len;
		char const *first = memchr(line.data, '/', line.len);
		if (!first)
			continue;
		struct cstr_view name = {line.data, (size_t)(first - line.data)};
		struct cstr_view rest = cstr_view_slice(line, name.len + 1, SIZE_MAX);
		char const *last = memchr(rest.data, '/', rest.len);
		if (!last)
			continue;
		struct cstr_view principal = {rest.data, (size_t)(last - rest.data)};
		struct cstr_view side = cstr_view_slice(rest, principal.len + 1, SIZE_MAX);
		if ((!cstr_view_equal(side, CSTR_VIEW("client")) && !cstr_view_equal(side, CSTR_VIEW("upstream"))) ||
		    !util_word(name.data, name.len, UTIL_WORD_MAX))
			continue;
		if (!cstr_view_equal(principal, CSTR_VIEW("anonymous")) &&
		    (principal.len != 16 || !util_is_hex(principal.data, principal.len)))
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
	struct cstr_view *data;
	size_t count, capacity;
};

static void cli_client_names_fini(struct cli_client_names *names)
{
	free(names->data);
	names->data = nullptr;
	names->count = names->capacity = 0;
}

static int cli_client_names_add(struct cli_client_names *names, struct cstr_view text)
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
	names->data[names->count++] = text;
	return 0;
}

static int
cli_client_read_files (struct cstr_view  text,
                       void             *files)
{
	while (text.len) {
		char const *newline = memchr(text.data, '\n', text.len);
		if (!newline)
			break;
		struct cstr_view line = {text.data, (size_t)(newline - text.data)};
		text = cstr_view_slice(text, line.len + 1, SIZE_MAX);
		if (!line.len || line.data[line.len - 1] != ')')
			continue;
		size_t tail = line.len - 1;
		while (tail && line.data[tail] != '(')
			--tail;
		if (tail < 3 || line.data[tail - 1] != ' ' || line.data[tail - 2] != ' ')
			continue;
		tail -= 2;
		struct cstr_view count = cstr_view_slice(line, tail + 3, line.len - tail - 4);
		struct cstr_view suffix = {};
		if (!cstr_view_equal(count, CSTR_VIEW("binary"))) {
			char const *space = memchr(count.data, ' ', count.len);
			if (!space || space == count.data)
				continue;
			size_t digits = (size_t)(space - count.data);
			suffix = cstr_view_slice(count, digits, SIZE_MAX);
			if (!cli_client_digits(cstr_view_slice(count, 0, digits)))
				continue;
		}
		line.len = tail;
		if (cstr_view_starts(suffix, CSTR_VIEW(" bytes, "))) {
			struct cstr_view blob = cstr_view_slice(suffix, sizeof " bytes, " - 1, SIZE_MAX);
			if (!util_is_oid(blob.data, blob.len))
				continue;
		} else {
			if (suffix.len && !cstr_view_equal(suffix, CSTR_VIEW(" line")) &&
			    !cstr_view_equal(suffix, CSTR_VIEW(" lines")))
				continue;
			char const *dot = memchr(line.data, '.', line.len);
			if (!dot)
				continue;
			size_t index = (size_t)(dot - line.data);
			if (!index || index + 1 >= line.len || line.data[index + 1] != ' ' ||
			    !cli_client_digits(cstr_view_slice(line, 0, index)))
				continue;
			line = cstr_view_slice(line, index + 2, SIZE_MAX);
			size_t const date_size = sizeof "2026-10-14 12:34 UTC  " - 1;
			if (line.len <= date_size || !cstr_view_equal(cstr_view_slice(line, 17, 5), CSTR_VIEW("UTC  ")))
				continue;
			line = cstr_view_slice(line, date_size, SIZE_MAX);
		}
		int e = cli_client_names_add(files, line);
		if (e)
			return e;
	}
	return 0;
}

static int
cli_client_name_compare (void const *a,
                         void const *b)
{
	struct cstr_view x = cli_client_text(*(struct json_object *const *)a);
	struct cstr_view y = cli_client_text(*(struct json_object *const *)b);
	return cstr_view_compare(x, y);
}

static void
cli_client_unique (struct json_object *names)
{
	json_object_array_sort(names, cli_client_name_compare);
	for (size_t i = json_object_array_length(names); i > 1; --i)
		if (cstr_view_equal(cli_client_text(jb_at(names, i - 1)), cli_client_text(jb_at(names, i - 2))))
			(void)json_object_array_del_idx(names, i - 1, 1);
}

/* Call the notes tool with params and pass the text of each text block of
 * its result to parse. A result that is an error parses nothing: it fails
 * with "The notes tool rejected <what> discovery", unless lenient. */
static int
cli_client_notes (struct cli_client  *client,
                  struct json_object *params,
                  char const         *what,
                  bool                lenient,
                  int               (*parse) (struct cstr_view text, void *context),
                  void               *context,
                  struct cli_error   *error)
{
	cli_json_auto listing = nullptr;
	int e = cli_client_rpc(client, "tools/call", params, nullptr, nullptr, &listing, error);
	if (e)
		return e;
	if (json_object_get_boolean(jb_get(listing, "isError")))
		return lenient ? 0 : cli_error_set(error, EPROTO, "The notes tool rejected %s discovery", what);
	struct json_object *content = jb_get(listing, "content");
	if (!json_object_is_type(content, json_type_array))
		return cli_error_set(error, EPROTO, "Invalid %s discovery result", what);
	for (size_t i = 0, n = json_object_array_length(content); !e && i < n; ++i) {
		struct json_object *block = jb_at(content, i);
		if (!cli_client_is(jb_get(block, "type"), "text"))
			continue;
		struct cstr_view text = cli_client_text(jb_get(block, "text"));
		e = text.data ? parse(text, context) : EPROTO;
	}
	return e;
}

/* The notebooks are sorted and unique. */
static int
cli_client_discover_files (struct cli_client   *client,
                           struct autocomplete *catalog,
                           struct cstr_view     tool,
                           struct json_object  *notebooks,
                           struct cli_error    *error)
{
	size_t count = json_object_array_length(notebooks);
	if (count > 128)
		return cli_error_set(error, E2BIG, "More than 128 notebooks; filename discovery is bounded to 128");
	struct cli_client_names files = {};
	int e = 0;
	for (size_t i = 0; !e && i < count; ++i) {
		struct cstr_view notebook = cli_client_text(jb_at(notebooks, i));
		struct json_object *params = jb_object("name", cli_client_string(tool),
		                                      "arguments", jb_object("action", json_object_new_string("list"),
		                                                             "notebook", cli_client_string(notebook), nullptr), nullptr);
		files.count = 0;
		e = cli_client_notes(client, params, "note filename", true, cli_client_read_files, &files, error);
		if (!e)
			e = autocomplete_set_note_files(catalog, tool,
			                                notebook, files.data, files.count);
	}
	cli_client_names_fini(&files);
	return e;
}

int
cli_client_refresh_notes (struct cli_client *client,
                          struct cli_error  *error)
{
	client->notes_dirty = false;
	if (!dstr_len(&client->notes_tool))
		return 0;
	autocomplete_clear_note_files(client->catalog);
	return cli_client_discover_files(client, client->catalog, cstr_view_dstr(&client->notes_tool), client->notebooks, error);
}

static int
cli_client_discover_books (struct cli_client  *client,
                           struct cstr_view    tool,
                           struct json_object *names,
                           struct json_object *notebooks,
                           struct cli_error   *error)
{
	struct json_object *params = jb_object("name", cli_client_string(tool),
	                                       "arguments", jb_object("action", json_object_new_string("notebooks"), nullptr), nullptr);
	struct json_object *lists[] = {names, notebooks};
	return cli_client_notes(client, params, "notebook", false, cli_client_read_notebooks, lists, error);
}

static bool
cli_client_has_action (struct json_object *actions,
                       char const         *name)
{
	if (!json_object_is_type(actions, json_type_array))
		return false;
	for (size_t i = 0, count = json_object_array_length(actions); i < count; ++i)
		if (cli_client_is(jb_at(actions, i), name))
			return true;
	return false;
}

static int
cli_client_catalog_names (struct autocomplete    *catalog,
                          enum autocomplete_list  list,
                          struct json_object     *names)
{
	cli_client_unique(names);
	struct cli_client_names views = {};
	int e = 0;
	for (size_t i = 0, count = json_object_array_length(names); !e && i < count; ++i)
		e = cli_client_names_add(&views, cli_client_text(jb_at(names, i)));
	if (!e)
		e = autocomplete_set_names(catalog, list, views.data, views.count);
	cli_client_names_fini(&views);
	return e;
}

/* Report the failure e of an optional discovery and go on, unless it was
 * interrupted or ran out of memory. */
static int
cli_client_soft (struct cli_client *client,
                 struct cstr_view   title,
                 struct cli_error  *error,
                 int                e)
{
	if (!e || e == EINTR || e == ENOMEM)
		return e;
	if (client->report)
		(void)client->report(client, title, cli_error_text(error, e));
	error->message[0] = '\0';
	return 0;
}

int
cli_client_refresh (struct cli_client *client,
                    struct cli_error  *error)
{
	client->catalog_dirty = false;
	client->notes_dirty = false;
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
		if (dstr_len(&cursor) && jb_set(params, "cursor", cli_client_string(cstr_view_dstr(&cursor)))) {
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
			struct cstr_view name = cli_client_text(jb_get(entry, "name"));
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
			if (!meta && cstr_view_equal(name, CSTR_VIEW("notes")) && jb_get(props, "notebook"))
				source |= cli_client_has_action(actions, "notebooks");
			if (source && !dstr_len(&notebook_tool)) {
				e = cstr_view_store(name, &notebook_tool);
				struct json_object *self = jb_get(meta, "participant");
				if (!e && json_object_is_type(self, json_type_string)) {
					e = jb_push(names, json_object_get(self));
					if (!e)
						e = cstr_view_store(cli_client_text(self), &participant);
				}
				struct json_object *identity = jb_get(meta, "session");
				if (!e && json_object_is_type(identity, json_type_string))
					e = cstr_view_store(cli_client_text(identity), &session);
				files = jb_get(props, "file") && jb_get(props, "notebook") && cli_client_has_action(actions, "list");
			}
			bool candidate = dstr_len(&client->tool) ? cstr_view_equal(name, cstr_view_dstr(&client->tool))
			                                       : cli_client_is(jb_get(entry, "title"), "Ask AI");
			if (e || !candidate)
				continue;
			if (!jb_get(props, "prompt") || !jb_get(props, "messages") || !jb_get(props, "background"))
				e = cli_error_set(error, EPROTO, "The selected AskAI tool needs the chat extension; rebuild the server too");
			else if (dstr_len(&chosen))
				e = cli_error_set(error, EPROTO, "Multiple chat tools; select one with --tool");
			else
				e = cstr_view_store(name, &chosen);
		}
		if (e)
			break;
		struct json_object *next = jb_get(result, "nextCursor");
		if (!next) {
			complete = true;
			break;
		}
		struct cstr_view after = cli_client_text(next);
		if (!after.len || cstr_view_equal(after, cstr_view_dstr(&cursor)))
			e = cli_error_set(error, EPROTO, "Invalid tool-list cursor");
		else
			e = cstr_view_store(after, &cursor);
	}
	if (!e && !complete)
		e = cli_error_set(error, E2BIG, "Too many tool-list pages");
	if (!e && !dstr_len(&chosen))
		e = cli_error_set(error, EPROTO, "No accessible AskAI chat tool was found");
	if (e)
		goto done;
	if (dstr_len(&notebook_tool))
		e = cli_client_soft(client, CSTR_VIEW("Participant discovery failed"), error,
		                    cli_client_discover_books(client, cstr_view_dstr(&notebook_tool),
		                                              names, notebooks, error));
	if (!e && files) {
		e = jb_push(notebooks, json_object_new_string("own"));
		if (!e)
			e = jb_push(notebooks, json_object_new_string("other"));
	}
	if (!e)
		e = cli_client_catalog_names(catalog, AUTOCOMPLETE_PARTICIPANTS, names);
	if (!e)
		e = cli_client_catalog_names(catalog, AUTOCOMPLETE_NOTEBOOKS, notebooks);
	if (!e) {
		struct cstr_view current = cstr_view_dstr(&participant);
		e = autocomplete_set_names(catalog, AUTOCOMPLETE_CHAT_PARTICIPANTS, &current, current.len ? 1 : 0);
	}
	if (!e && files)
		e = cli_client_soft(client, CSTR_VIEW("Note filename discovery failed"), error,
		                    cli_client_discover_files(client, catalog, cstr_view_dstr(&notebook_tool),
		                                              notebooks, error));
	if (e)
		goto done;
	if (client->catalog && (!cstr_view_equal(cstr_view_dstr(&client->session_key), cstr_view_dstr(&session)) ||
	                        !cstr_view_equal(cstr_view_dstr(&client->participant), cstr_view_dstr(&participant)) ||
	                        !cstr_view_equal(cstr_view_dstr(&client->tool), cstr_view_dstr(&chosen)))) {
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
	dstr_cleanup(&client->session_key);
	client->session_key = dstr_move(&session);
	dstr_cleanup(&client->participant);
	client->participant = dstr_move(&participant);
done:
	autocomplete_destroy(&catalog);
	return e;
}
