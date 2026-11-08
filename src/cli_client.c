#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "cli_client.h"
#include "cli_network.h"
#include "compat.h"
#include "jb.h"
#include "notes.h"
#include "oid.h"
#include "sse.h"
#include "util.h"

int
cli_error_set (struct cli_error *error,
               int               e,
               char const       *format, ...)
{
	va_list args;
	va_start(args, format);
	error->len = (uint32_t)util_vformat(error->data, sizeof error->data, format, args);
	va_end(args);
	return e;
}

struct cstr_view cli_error_text(struct cli_error const *error, int e)
{
	return error->len ? util_text_view(error) : cstr_view(strerror(e));
}

struct json_object *cli_client_string(struct cstr_view text)
{
	return text.len <= CLI_LIMIT ? jb_string(text.data ? text : CSTR_VIEW("")) : nullptr;
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
		return cli_error_set(error, EFBIG, "Conversation exceeds the %d MiB client limit; use /clear",
		                     CLI_LIMIT >> 20);
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
}

struct cli_client_response {
	struct buf body;
	struct sse stream;
	struct dstr session;
	struct cli_client *client;
	struct cli_error *error;
	struct json_object *result;
	cli_client_notify_fn *notify;
	void *context;
	int64_t id;
	int failure;
	uint32_t sse;  /* bool, widened over the padding: the body is an event stream */
};

static void cli_client_response_fini(struct cli_client_response *response)
{
	buf_fini(&response->body);
	sse_fini(&response->stream);
	dstr_cleanup(&response->session);
	cli_client_json_cleanup(&response->result);
}

/* Handle the JSON-RPC message in the @p len bytes at @p data, the body or
 * one event of the response @p user.
 */
static int cli_client_response_event(char const *data, size_t len, void *user)
{
	struct cli_client_response *response = user;
	cli_json_auto event = jb_parse(data, len, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8, 0);
	if (!json_object_is_type(event, json_type_object))
		return cli_error_set(response->error, EPROTO, "Malformed JSON response from AskAI");
	struct json_object *id = jb_get(event, "id");
	if (id) {
		if (!response->id || jb_int(id, 0) != response->id || response->result)
			return cli_error_set(response->error, EPROTO, "Unexpected JSON-RPC response id");
		response->result = json_object_get(event);
		return 0;
	}
	if (jb_is(jb_get(event, "method"), "notifications/tools/list_changed"))
		response->client->flags |= CLI_CLIENT_CATALOG_DIRTY;
	return response->notify ? response->notify(response->context, event, response->id, response->error) : 0;
}

/* Start @p response to the request @p id of @p client, 0 for a
 * notification.
 */
static void
cli_client_response_init (struct cli_client_response *response,
                          struct cli_client          *client,
                          cli_client_notify_fn       *notify,
                          void                       *context,
                          int64_t                     id,
                          struct cli_error           *error)
{
	*response = (struct cli_client_response){
		.stream = sse(cli_client_response_event, response),
		.session = DSTR_INIT, .client = client, .error = error,
		.notify = notify, .context = context, .id = id,
	};
}

/* The partial line and event of a stream are at most CLI_LIMIT bytes. */
static int cli_client_response_receive(struct cstr_view bytes, void *context)
{
	struct cli_client_response *response = context;
	struct sse *stream = &response->stream;
	if (response->failure)
		return response->failure;
	if (!response->sse)
		response->failure = buf_put(&response->body, bytes.data, bytes.len, CLI_LIMIT);
	else if (bytes.len > CLI_LIMIT - dstr_len(&stream->input) - dstr_len(&stream->data))
		response->failure = ENOBUFS;
	else
		response->failure = sse_feed(stream, bytes.data, bytes.len);
	if (response->failure == ENOBUFS)
		response->failure = cli_error_set(response->error, EFBIG, "AskAI response exceeds the %d MiB client limit",
		                                  CLI_LIMIT >> 20);
	return response->failure;
}

static int cli_client_response_header(struct cstr_view line, void *context)
{
	struct cli_client_response *response = context;
	if (cstr_view_starts(line, CSTR_VIEW("HTTP/"))) {
		response->sse = false;
		dstr_clear(&response->session);
	}
	struct cstr_view value = line, key;
	if (!cstr_view_cut(&value, ':', &key))
		return 0;
	value = cstr_view_trim(value);
	if (cstr_view_equal_case(key, CSTR_VIEW("Content-Type"))) {
		(void)cstr_view_cut(&value, ';', &value);
		response->sse = cstr_view_equal_case(cstr_view_trim_end(value),
		                                     CSTR_VIEW("text/event-stream"));
	} else if (cstr_view_equal_case(key, CSTR_VIEW("Mcp-Session-Id"))) {
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
	if (response->sse && (dstr_len(&response->stream.input) || dstr_len(&response->stream.data)))
		return cli_error_set(response->error, EPROTO, "AskAI closed an incomplete SSE event");
	return 0;
}

/* The request of @p wire to AskAI for @p response. */
static struct cli_network_post
cli_client_network_post (struct cli_client          *client,
                         struct cstr_view            wire,
                         struct cli_client_response *response,
                         long                        timeout,
                         uint64_t                    flags)
{
	return (struct cli_network_post){
		.socket = cstr_view_dstr(&client->socket), .session = cstr_view_dstr(&client->session),
		.wire = wire, .header = cli_client_response_header, .body = cli_client_response_receive,
		.context = response, .timeout = timeout, .flags = flags,
	};
}

static int cli_client_post(struct cli_client *client, struct cstr_view wire,
                           struct cli_client_response *response, uint64_t flags, long timeout)
{
	struct cli_network_post const post = cli_client_network_post(client, wire, response, timeout, flags);
	struct cli_network_result result;
	int e = cli_network_perform(client->network, &post, &result);
	return cli_client_response_status(response, e, &result);
}

static struct json_object *cli_client_packet(char const *method, struct json_object *params, int64_t id)
{
	struct json_object *message = jb_object("jsonrpc", json_object_new_string("2.0"),
	                                        "method", json_object_new_string(method),
	                                        "params", params, nullptr);
	return id ? jb_set_or_free(message, "id", json_object_new_int64(id)) : message;
}

static int cli_client_notification(struct cli_client *client, char const *method,
                                   struct json_object *params, uint64_t flags,
                                   long timeout, struct cli_error *error)
{
	cli_json_auto message = cli_client_packet(method, params, 0);
	struct cli_client_response response;
	cli_client_response_init(&response, client, nullptr, nullptr, 0, error);
	struct cstr_view wire;
	int e = cli_client_wire(message, JB_WIRE, &wire, error);
	if (!e)
		e = cli_client_post(client, wire, &response, flags, timeout);
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
	int e = response->sse ? 0 : cli_client_response_event((char const *)response->body.data,
	                                                       response->body.tail, response);
	if (e)
		return e;
	if (!response->result)
		return cli_error_set(response->error, EPROTO, "AskAI closed the stream without a result");
	struct json_object *failure = jb_get(response->result, "error");
	struct json_object *value = jb_get(response->result, "result");
	if (failure) {
		struct cstr_view text = jb_view(jb_get(failure, "message"));
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

/* The timeout of a request in seconds: none for a tool call, 30 for
 * another call, and 5 for a notification, which is sent without an id.
 */
static long
cli_client_timeout (char const *method,
                    int64_t     id)
{
	return !id ? 5 : strcmp(method, CLI_TOOLS_CALL) ? 30 : 0;
}

/* Store the id of the next request in *id. Returns 0, or EOVERFLOW after
 * it releases params, which the request would have consumed.
 */
static int
cli_client_next_id (struct cli_client  *client,
                    struct json_object *params,
                    int64_t            *id)
{
	if (client->next_id == INT64_MAX) {
		json_object_put(params);
		return EOVERFLOW;
	}
	*id = ++client->next_id;
	return 0;
}

static int cli_client_call_start (struct cli_client *client, char const *method,
                                  struct json_object *params, cli_client_notify_fn *notify,
                                  cli_client_done_fn *done, void *context, int64_t id,
                                  uint64_t *request, struct cli_error *error);

static void
cli_client_report_cancel (struct cli_client      *client,
                          int                     e,
                          struct cli_error const *error)
{
	if (e && e != ESHUTDOWN && client->report)
		(void)client->report(client, CSTR_VIEW("Cancellation could not be confirmed"),
		                     cli_error_text(error, e));
}

/* Tell AskAI that the call id was interrupted. With wait, wait up to 5 s for
 * the reply; otherwise cli_client_call_done receives it.
 */
static void
cli_client_cancel (struct cli_client *client,
                   int64_t            id,
                   bool               wait)
{
	static char const method[] = "notifications/cancelled";
	struct json_object *params = jb_object("requestId", json_object_new_int64(id),
	                                       "reason", json_object_new_string("Interrupted by the terminal user"), nullptr);
	struct cli_error error = {};
	uint64_t request;
	int e = wait ? cli_client_notification(client, method, params, 0, 5, &error)
	             : cli_client_call_start(client, method, params, nullptr, nullptr, nullptr, 0, &request, &error);
	cli_client_report_cancel(client, e, &error);
}

int cli_client_rpc(struct cli_client *client, char const *method,
                    struct json_object *params, cli_client_notify_fn *notify,
                    void *context, struct json_object **result, struct cli_error *error)
{
	*result = nullptr;
	int64_t id;
	int e = cli_client_next_id(client, params, &id);
	if (e)
		return e;
	cli_json_auto message = cli_client_packet(method, params, id);
	struct cli_client_response response;
	cli_client_response_init(&response, client, notify, context, id, error);
	struct cstr_view wire;
	e = cli_client_wire(message, JB_WIRE, &wire, error);
	long timeout = cli_client_timeout(method, id);
	if (!e)
		e = cli_client_post(client, wire, &response, CLI_NETWORK_INTERRUPTIBLE, timeout);
	/* Only a tool call runs without a timeout, and AskAI is told when
	 * one is interrupted. */
	if (e == EINTR && !timeout)
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
	if (!call) {
		*e = ENOMEM;
		return nullptr;
	}
	*call = (struct cli_client_call){.done = done};
	cli_client_response_init(&call->response, client, notify, context, id, &call->error);
	*e = 0;
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
		cli_client_report_cancel(client, e, &call->error);
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
	struct cli_network_post const post = cli_client_network_post(client, wire, &call->response,
	                                                             cli_client_timeout(method, id),
	                                                             id ? CLI_NETWORK_INTERRUPTIBLE : 0);
	e = cli_network_submit(client->network, &post, cli_client_call_done, request);
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
	if (!done) {
		json_object_put(params);
		return EINVAL;
	}
	int64_t id;
	int e = cli_client_next_id(client, params, &id);
	return e ? e : cli_client_call_start(client, method, params, notify, done, context, id, request, error);
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
	if (!e && (!jb_is(jb_get(initialized, "protocolVersion"), CLI_VERSION) || !dstr_len(&client->session)))
		e = cli_error_set(error, EPROTO, "AskAI did not negotiate the supported MCP session");
	if (!e)
		e = cli_client_notification(client, "notifications/initialized", json_object_new_object(),
		                            CLI_NETWORK_INTERRUPTIBLE, 30, error);
	return e ? e : cli_client_refresh(client, error);
}

/* The arrays that cli_client_read_notebooks() appends to. */
struct cli_client_notebooks {
	struct json_object *names;      /* the participant names */
	struct json_object *notebooks;  /* the notebook selectors */
};

/* Parse notebook selectors into the arrays of @p context, a struct
 * cli_client_notebooks.
 */
static int
cli_client_read_notebooks (struct cstr_view  text,
                           void             *context)
{
	struct cli_client_notebooks const *lists = context;
	/* A truncated last line is never an authoritative notebook selector. */
	struct cstr_view line;
	while (cstr_view_cut(&text, '\n', &line)) {
		struct cstr_view self = CSTR_VIEW("Self (writable): ");
		struct cstr_view peer = CSTR_VIEW(" (read-only)");
		if (cstr_view_starts(line, self))
			line = cstr_view_slice(line, self.len, SIZE_MAX);
		if (cstr_view_ends(line, peer))
			line.len -= peer.len;
		struct cstr_view rest = line, name;
		if (!notes_selector_valid(line) || !cstr_view_cut(&rest, '/', &name))
			continue;
		int e = jb_push(lists->names, cli_client_string(name));
		if (!e)
			e = jb_push(lists->notebooks, cli_client_string(line));
		if (e)
			return e;
	}
	return 0;
}

/* The note names that cli_client_read_files() parses, as views into the
 * text that it parses.
 */
struct cli_client_files {
	UTIL_ARRAY(struct cstr_view);
};

/* Returns the note name in line, a line of a listing of the notes tool,
 * which is "NAME  (N bytes, OID)". Returns an empty view if the line is not
 * in this form.
 */
static struct cstr_view
cli_client_file_name (struct cstr_view line)
{
	if (!cstr_view_ends(line, CSTR_VIEW(")")))
		return (struct cstr_view){};
	size_t open = line.len - 1;
	while (open && line.data[open] != '(')
		--open;
	if (open < 3 || !cstr_view_equal(cstr_view_slice(line, open - 2, 2), CSTR_VIEW("  ")))
		return (struct cstr_view){};
	struct cstr_view count = cstr_view_slice(line, open + 1, line.len - open - 2);
	struct cstr_view name = cstr_view_slice(line, 0, open - 2);
	uint64_t n;
	size_t digits = util_digits(count.data, count.len, UINT64_MAX, &n);
	struct cstr_view suffix = cstr_view_slice(count, digits, SIZE_MAX);
	if (!digits || !cstr_view_starts(suffix, CSTR_VIEW(" bytes, ")))
		return (struct cstr_view){};
	return oid_valid(cstr_view_slice(suffix, sizeof " bytes, " - 1, SIZE_MAX)) ? name : (struct cstr_view){};
}

static int
cli_client_read_files (struct cstr_view  text,
                       void             *context)
{
	struct cli_client_files *found = context;
	struct cstr_view line;
	while (cstr_view_cut(&text, '\n', &line)) {
		line = cli_client_file_name(line);
		if (!line.len)
			continue;
		if (found->n == 65536)
			return E2BIG;
		if (util_grow(found->items, found->cap, found->n + 1))
			return ENOMEM;
		found->items[found->n++] = line;
	}
	return 0;
}

/* Call the notes tool with params and pass the text of each text block of
 * its result to parse. The result is stored in *listing. The caller frees
 * it after the last use of the views that parse stored. If the result is
 * an error, nothing is parsed, and the call fails with "The notes tool
 * rejected <what> discovery", unless lenient.
 */
static int
cli_client_notes (struct cli_client   *client,
                  struct json_object  *params,
                  char const          *what,
                  bool                 lenient,
                  int                (*parse) (struct cstr_view text, void *context),
                  void                *context,
                  struct json_object **listing,
                  struct cli_error    *error)
{
	int e = cli_client_rpc(client, CLI_TOOLS_CALL, params, nullptr, nullptr, listing, error);
	if (e)
		return e;
	if (json_object_get_boolean(jb_get(*listing, "isError")))
		return lenient ? 0 : cli_error_set(error, EPROTO, "The notes tool rejected %s discovery", what);
	struct json_object *content = jb_get(*listing, "content");
	if (!json_object_is_type(content, json_type_array))
		return cli_error_set(error, EPROTO, "Invalid %s discovery result", what);
	for (size_t i = 0, n = json_object_array_length(content); !e && i < n; ++i) {
		struct json_object *block = jb_at(content, i);
		if (!jb_is(jb_get(block, "type"), "text"))
			continue;
		struct cstr_view text = jb_view(jb_get(block, "text"));
		e = text.data ? parse(text, context) : EPROTO;
	}
	return e;
}

/* Discover the note files of the notebooks of the catalog. */
static int
cli_client_discover_files (struct cli_client   *client,
                           struct autocomplete *catalog,
                           struct cstr_view     tool,
                           struct cli_error    *error)
{
	size_t count = autocomplete_list_count(catalog, AUTOCOMPLETE_NOTEBOOKS);
	if (count > 128)
		return cli_error_set(error, E2BIG, "More than 128 notebooks; filename discovery is bounded to 128");
	struct cli_client_files files = {};
	int e = 0;
	for (size_t i = 0; !e && i < count; ++i) {
		struct cstr_view notebook = autocomplete_list_at(catalog, AUTOCOMPLETE_NOTEBOOKS, i);
		struct json_object *params = jb_object("name", cli_client_string(tool),
		                                      "arguments", jb_object("action", jb_string(notes_action_names[NOTES_ACTION_LIST]),
		                                                             "notebook", cli_client_string(notebook), nullptr), nullptr);
		files.n = 0;
		/* The names in files are views into the listing. */
		cli_json_auto listing = nullptr;
		e = cli_client_notes(client, params, "note filename", true, cli_client_read_files, &files,
		                     &listing, error);
		if (!e)
			e = autocomplete_set_note_files(catalog, tool, notebook, files.items, files.n);
	}
	free(files.items);
	return e;
}

int
cli_client_refresh_notes (struct cli_client *client,
                          struct cli_error  *error)
{
	client->flags &= ~CLI_CLIENT_NOTES_DIRTY;
	if (!dstr_len(&client->notes_tool))
		return 0;
	autocomplete_clear_note_files(client->catalog);
	return cli_client_discover_files(client, client->catalog, cstr_view_dstr(&client->notes_tool), error);
}

static int
cli_client_discover_books (struct cli_client  *client,
                           struct cstr_view    tool,
                           struct json_object *names,
                           struct json_object *notebooks,
                           struct cli_error   *error)
{
	struct json_object *params = jb_object("name", cli_client_string(tool),
	                                       "arguments", jb_object("action", jb_string(notes_action_names[NOTES_ACTION_NOTEBOOKS]), nullptr), nullptr);
	struct cli_client_notebooks lists = {names, notebooks};
	cli_json_auto listing = nullptr;
	return cli_client_notes(client, params, "notebook", false, cli_client_read_notebooks, &lists,
	                        &listing, error);
}

static bool
cli_client_has_action (struct json_object *actions,
                       enum notes_action   action)
{
	for (size_t i = 0, count = jb_len(actions); i < count; ++i)
		if (cstr_view_equal(jb_view(jb_at(actions, i)), notes_action_names[action]))
			return true;
	return false;
}

/* Set list of the catalog to the strings of names, a JSON array. */
static int
cli_client_catalog_names (struct autocomplete    *catalog,
                          enum autocomplete_list  list,
                          struct json_object     *names)
{
	size_t count = json_object_array_length(names);
	struct cstr_view *views = calloc(count ? count : 1, sizeof *views);
	if (!views)
		return ENOMEM;
	for (size_t i = 0; i < count; ++i)
		views[i] = jb_view(jb_at(names, i));
	int e = autocomplete_set_names(catalog, list, views, count);
	free(views);
	return e;
}

/* Report the failure e of an optional discovery and go on, unless it was
 * interrupted or ran out of memory.
 */
static int
cli_client_report_optional (struct cli_client *client,
                            struct cstr_view   title,
                            struct cli_error  *error,
                            int                e)
{
	if (!e || e == EINTR || e == ENOMEM)
		return e;
	if (client->report)
		(void)client->report(client, title, cli_error_text(error, e));
	util_text_clear(error);
	return 0;
}

/* What cli_client_refresh() collects from the tool list. */
struct cli_client_scan {
	struct dstr          notebook_tool;  /* the tool that lists the notebooks */
	struct dstr          session;        /* the session of notebook_tool */
	struct dstr          participant;    /* the participant of notebook_tool */
	struct dstr          chosen;         /* the ask tool */
	struct autocomplete *catalog;
	struct json_object  *names;          /* the participant names */
	struct json_object  *notebooks;      /* the notebook selectors */
	uint64_t             files;          /* bool: notebook_tool lists note files */
};

static void
cli_client_scan_fini (struct cli_client_scan *scan)
{
	dstr_cleanup(&scan->notebook_tool);
	dstr_cleanup(&scan->session);
	dstr_cleanup(&scan->participant);
	dstr_cleanup(&scan->chosen);
	autocomplete_destroy(&scan->catalog);
	cli_client_json_cleanup(&scan->names);
	cli_client_json_cleanup(&scan->notebooks);
}

/* Store name in scan->notebook_tool as the tool that lists the notebooks.
 * meta is the askai _meta of the tool definition. Add its participant to
 * scan->names and copy it to scan->participant, and copy its session to
 * scan->session. Set scan->files if the input properties props contain
 * file and notebook, and the actions contain list.
 */
static int
cli_client_scan_notes (struct cli_client_scan *scan,
                       struct cstr_view        name,
                       struct json_object     *meta,
                       struct json_object     *props,
                       struct json_object     *actions)
{
	int e = cstr_view_store(name, &scan->notebook_tool);
	struct json_object *self = jb_get(meta, "participant");
	if (!e && json_object_is_type(self, json_type_string)) {
		e = jb_push(scan->names, json_object_get(self));
		if (!e)
			e = cstr_view_store(jb_view(self), &scan->participant);
	}
	struct json_object *identity = jb_get(meta, "session");
	if (!e && json_object_is_type(identity, json_type_string))
		e = cstr_view_store(jb_view(identity), &scan->session);
	scan->files = jb_get(props, "file") && jb_get(props, "notebook") && cli_client_has_action(actions, NOTES_ACTION_LIST);
	return e;
}

/* Add entry, a tool of the tool list, to scan. */
static int
cli_client_scan_tool (struct cli_client      *client,
                      struct cli_client_scan *scan,
                      struct json_object     *entry,
                      struct cli_error       *error)
{
	int e = autocomplete_add_tool(scan->catalog, entry);
	if (e)
		return e;
	struct cstr_view name = jb_view(jb_get(entry, "name"));
	if (!name.data)
		return EPROTO;
	struct json_object *props = jb_get(jb_get(entry, "inputSchema"), "properties");
	struct json_object *meta = jb_get(jb_get(entry, "_meta"), "askai");
	struct json_object *actions = jb_get(jb_get(props, "action"), "enum");
	bool source = jb_true(jb_get(meta, "notebooks"));
	if (!meta && cstr_view_equal(name, CSTR_VIEW("notes")) && jb_get(props, "notebook"))
		source |= cli_client_has_action(actions, NOTES_ACTION_NOTEBOOKS);
	if (source && !dstr_len(&scan->notebook_tool))
		e = cli_client_scan_notes(scan, name, meta, props, actions);
	bool candidate = dstr_len(&client->tool) ? cstr_view_equal(name, cstr_view_dstr(&client->tool))
	                                       : jb_is(jb_get(entry, "title"), "Ask AI");
	if (e || !candidate)
		return e;
	if (!jb_get(props, "prompt") || !jb_get(props, "messages") || !jb_get(props, "background"))
		return cli_error_set(error, EPROTO, "The selected AskAI tool needs the chat extension; rebuild the server too");
	if (dstr_len(&scan->chosen))
		return cli_error_set(error, EPROTO, "Multiple chat tools; select one with --tool");
	return cstr_view_store(name, &scan->chosen);
}

/* Add the tools of the tool list, at most 32 pages of them, to scan. */
static int
cli_client_scan_tools (struct cli_client      *client,
                       struct cli_client_scan *scan,
                       struct cli_error       *error)
{
	dstr_auto cursor = dstr(nullptr);
	for (unsigned page = 0; page < 32; ++page) {
		struct json_object *params = json_object_new_object();
		if (dstr_len(&cursor) && jb_set(params, "cursor", cli_client_string(cstr_view_dstr(&cursor)))) {
			json_object_put(params);
			return ENOMEM;
		}
		cli_json_auto result = nullptr;
		int e = cli_client_rpc(client, "tools/list", params, nullptr, nullptr, &result, error);
		if (e)
			return e;
		struct json_object *tools = jb_get(result, "tools");
		if (!json_object_is_type(tools, json_type_array))
			return cli_error_set(error, EPROTO, "AskAI returned an invalid tool list");
		for (size_t i = 0, count = json_object_array_length(tools); !e && i < count; ++i)
			e = cli_client_scan_tool(client, scan, jb_at(tools, i), error);
		struct json_object *next = jb_get(result, "nextCursor");
		if (e || !next)
			return e;
		struct cstr_view after = jb_view(next);
		if (!after.len || cstr_view_equal(after, cstr_view_dstr(&cursor)))
			return cli_error_set(error, EPROTO, "Invalid tool-list cursor");
		e = cstr_view_store(after, &cursor);
		if (e)
			return e;
	}
	return cli_error_set(error, E2BIG, "Too many tool-list pages");
}

int
cli_client_refresh (struct cli_client *client,
                    struct cli_error  *error)
{
	client->flags &= ~(CLI_CLIENT_CATALOG_DIRTY | CLI_CLIENT_NOTES_DIRTY);
	int e = 0;
	__attribute__((cleanup(cli_client_scan_fini)))
	struct cli_client_scan scan = {
		.notebook_tool = DSTR_INIT, .session = DSTR_INIT, .participant = DSTR_INIT, .chosen = DSTR_INIT,
		.catalog = autocomplete_create(&e),
		.names = json_object_new_array(), .notebooks = json_object_new_array(),
	};
	if (!scan.catalog || !scan.names || !scan.notebooks)
		return ENOMEM;
	e = cli_client_scan_tools(client, &scan, error);
	if (!e && !dstr_len(&scan.chosen))
		e = cli_error_set(error, EPROTO, "No accessible AskAI chat tool was found");
	if (!e && dstr_len(&scan.notebook_tool))
		e = cli_client_report_optional(client, CSTR_VIEW("Participant discovery failed"), error,
		                               cli_client_discover_books(client, cstr_view_dstr(&scan.notebook_tool),
		                                                         scan.names, scan.notebooks, error));
	if (!e && scan.files) {
		e = jb_push(scan.notebooks, json_object_new_string("own"));
		if (!e)
			e = jb_push(scan.notebooks, json_object_new_string("other"));
	}
	if (!e)
		e = cli_client_catalog_names(scan.catalog, AUTOCOMPLETE_PARTICIPANTS, scan.names);
	if (!e)
		e = cli_client_catalog_names(scan.catalog, AUTOCOMPLETE_NOTEBOOKS, scan.notebooks);
	if (!e) {
		struct cstr_view current = cstr_view_dstr(&scan.participant);
		e = autocomplete_set_names(scan.catalog, AUTOCOMPLETE_CHAT_PARTICIPANTS, &current, current.len ? 1 : 0);
	}
	if (!e && scan.files)
		e = cli_client_report_optional(client, CSTR_VIEW("Note filename discovery failed"), error,
		                               cli_client_discover_files(client, scan.catalog,
		                                                         cstr_view_dstr(&scan.notebook_tool), error));
	if (e)
		return e;
	if (client->catalog && (!cstr_view_equal(cstr_view_dstr(&client->session_key), cstr_view_dstr(&scan.session)) ||
	                        !cstr_view_equal(cstr_view_dstr(&client->participant), cstr_view_dstr(&scan.participant)) ||
	                        !cstr_view_equal(cstr_view_dstr(&client->tool), cstr_view_dstr(&scan.chosen)))) {
		client->flags |= CLI_CLIENT_IDENTITY_CHANGED;
		return cli_error_set(error, ESTALE, "AskAI changed its session, participant or ask tool; restart the client to select its history");
	}
	autocomplete_destroy(&client->catalog);
	client->catalog = scan.catalog;
	scan.catalog = nullptr;
	dstr_cleanup(&client->tool);
	client->tool = dstr_move(&scan.chosen);
	dstr_cleanup(&client->notes_tool);
	if (scan.files)
		client->notes_tool = dstr_move(&scan.notebook_tool);
	dstr_cleanup(&client->session_key);
	client->session_key = dstr_move(&scan.session);
	dstr_cleanup(&client->participant);
	client->participant = dstr_move(&scan.participant);
	return 0;
}
