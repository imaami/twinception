/** @file MCP JSON-RPC semantics behind the Streamable HTTP transport.
 *
 * Two protocol eras are served side by side.  Legacy clients (revisions up to
 * 2025-11-25, which includes llama.cpp) open with an initialize handshake;
 * modern clients (2026-07-28) are stateless and declare their version in each
 * request's _meta, mirrored into HTTP headers that must match the body.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#include "ai.h"
#include "buf.h"
#include "http.h"
#include "jb.h"
#include "list.h"
#include "mcp.h"
#include "util.h"

enum {
	PARSE_ERROR         = -32700,
	INVALID_REQUEST     = -32600,
	METHOD_NOT_FOUND    = -32601,
	INVALID_PARAMS      = -32602,
	INTERNAL_ERROR      = -32603,
	HEADER_MISMATCH     = -32020,
	UNSUPPORTED_VERSION = -32022,
};

/** Upstream responses beyond this many bytes are refused. */
enum { MAX_RESPONSE = 16 << 20 };

/** How long modern clients may cache tools/list and server/discover. */
enum { CACHE_TTL_MS = 300'000 };

/** Newest first; the first MODERN revisions carry their version in _meta. */
static char const *const versions[] = {
	"2026-07-28",
	"2025-11-25",
	"2025-06-18",
	"2025-03-26",
	"2024-11-05",
};
enum { MODERN = 1 };

static char const meta_version[] = "io.modelcontextprotocol/protocolVersion";
static char const meta_caps[] = "io.modelcontextprotocol/clientCapabilities";
static char const meta_server[] = "io.modelcontextprotocol/serverInfo";

/** Where and how a reply is delivered. */
struct target {
	mcp_reply_cb       *reply;
	void               *user;
	struct json_object *id;
	bool                modern;
};

struct mcp_call {
	struct list          hook;
	struct buf           response;
	struct target        to;
	struct timespec      start;
	struct mcp          *mcp;
	struct http_request *request;
	struct mcp_call    **owner;
	char                *session;
	bool                 overflow;
};

struct mcp {
	struct list               calls;
	struct json_object       *info;
	struct json_object       *tools;
	struct json_object       *versions;
	struct http              *http;
	struct ai_endpoint const *ep;
	char const               *tool;
	bool                      verbose;
};

/* Index into versions[], ARRAY_SIZE(versions) if @p v is unknown or absent. */
static size_t
version_index (char const *v)
{
	size_t i = 0;
	while (v && i < ARRAY_SIZE(versions) && strcmp(versions[i], v))
		++i;
	return v ? i : ARRAY_SIZE(versions);
}

static bool
is_legacy (char const *v)
{
	size_t i = version_index(v);
	return i >= MODERN && i < ARRAY_SIZE(versions);
}

static bool
same (char const *a,
      char const *b)
{
	return a == b || (a && b && !strcmp(a, b));
}

static double
elapsed (struct timespec const *start)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - start->tv_sec) +
	       (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static void
send_json (struct target const *to,
           unsigned             status,
           struct json_object  *msg,
           char const          *session)
{
	static char const oom[] = "{\"jsonrpc\":\"2.0\",\"error\":"
	                          "{\"code\":-32603,\"message\":\"Out of memory\"}}";
	size_t len = 0;
	char const *json = msg
		? json_object_to_json_string_length(msg, JB_WIRE, &len) : nullptr;
	if (json)
		to->reply(to->user, status, json, len, session);
	else
		to->reply(to->user, 500, oom, sizeof oom - 1, nullptr);
	json_object_put(msg);
}

static struct json_object *
envelope (struct target const *to,
          char const          *key,
          struct json_object  *val)
{
	struct json_object *jsonrpc = json_object_new_string("2.0");
	return to->id ? jb_object("jsonrpc", jsonrpc, "id", json_object_get(to->id),
	                          key, val, nullptr)
	              : jb_object("jsonrpc", jsonrpc, key, val, nullptr);
}

static void
send_error (struct target const *to,
            unsigned             status,
            int                  code,
            char const          *message,
            struct json_object  *data)
{
	struct json_object *error = jb_object(
		"code", json_object_new_int(code),
		"message", json_object_new_string(message), nullptr);
	if (data && !jb_set(error, "data", data)) {
		json_object_put(error);
		error = nullptr;
	}
	send_json(to, status, envelope(to, "error", error), nullptr);
}

static void
send_result (struct mcp const    *mcp,
             struct target const *to,
             struct json_object  *result,
             char const          *session)
{
	if (to->modern &&
	    (!jb_set(result, "resultType", json_object_new_string("complete")) ||
	     !jb_set(result, "_meta", jb_object(meta_server,
	                                        json_object_get(mcp->info),
	                                        nullptr)))) {
		json_object_put(result);
		result = nullptr;
	}
	send_json(to, 200, envelope(to, "result", result), session);
}

/* Modern clients get caching hints on list and discovery results. */
static struct json_object *
cacheable (struct target const *to,
           struct json_object  *result)
{
	if (to->modern &&
	    (!jb_set(result, "ttlMs", json_object_new_int(CACHE_TTL_MS)) ||
	     !jb_set(result, "cacheScope", json_object_new_string("public")))) {
		json_object_put(result);
		return nullptr;
	}
	return result;
}

static struct json_object *
tool_result (char const *text,
             size_t      len,
             bool        failed)
{
	struct json_object *result = jb_object("content", jb_list(jb_object(
		"type", json_object_new_string("text"),
		"text", json_object_new_string_len(text ? text : "", (int)len),
		nullptr)), nullptr);
	if (failed && !jb_set(result, "isError", json_object_new_boolean(1))) {
		json_object_put(result);
		return nullptr;
	}
	return result;
}

static void
call_free (struct mcp_call *call)
{
	list_del(&call->hook);
	json_object_put(call->to.id);
	buf_fini(&call->response);
	free(call->session);
	free(call);
}

static size_t
call_write (char const *data,
            size_t      len,
            void       *user)
{
	struct mcp_call *call = user;
	if (len > MAX_RESPONSE - call->response.len) {
		call->overflow = true;
		return 0;
	}
	return buf_append(&call->response, data, len) ? 0 : len;
}

static void
call_done (CURLcode    result,
           long        status,
           char const *curl_error,
           void       *user)
{
	struct mcp_call *call = user;
	struct mcp *mcp = call->mcp;
	call->request = nullptr;

	struct buf text = {0};
	bool failed = true;
	int e;
	if (result == CURLE_OK)
		e = ai_response(mcp->ep->protocol, status,
		                call->response.data ? call->response.data : "",
		                call->response.len, &text, &failed);
	else if (call->overflow)
		e = buf_printf(&text, "Upstream response exceeds %d MiB.",
		               MAX_RESPONSE >> 20);
	else if (result == CURLE_OPERATION_TIMEDOUT)
		e = buf_printf(&text, "Upstream request timed out after %.0f s.",
		               elapsed(&call->start));
	else
		e = buf_printf(&text, "Upstream request failed: %s%s%s",
		               curl_easy_strerror(result),
		               curl_error && *curl_error ? ": " : "",
		               curl_error ? curl_error : "");

	if (!e && failed)
		fprintf(stderr, "askai: tool call failed after %.1f s: %.*s\n",
		        elapsed(&call->start), (int)(text.len < 300 ? text.len : 300),
		        text.data);
	else if (!e && mcp->verbose)
		fprintf(stderr, "askai: tool call answered after %.1f s (%zu bytes)\n",
		        elapsed(&call->start), text.len);

	*call->owner = nullptr;
	send_result(mcp, &call->to,
	            e ? nullptr : tool_result(text.data, text.len, failed), nullptr);
	buf_fini(&text);
	call_free(call);
}

static void
tools_call (struct mcp             *mcp,
            struct mcp_meta const  *meta,
            struct target const    *to,
            struct json_object     *params,
            struct mcp_call       **p_call)
{
	char const *name = jb_text(jb_get(params, "name"));
	if (!name || strcmp(name, mcp->tool)) {
		char msg[160];
		snprintf(msg, sizeof msg, "Unknown tool: %.100s", name ? name : "(none)");
		send_error(to, 200, INVALID_PARAMS, msg, nullptr);
		return;
	}

	/* Argument problems go back to the model so it can correct itself. */
	struct json_object *args = jb_get(params, "arguments");
	char const *prompt = jb_text(jb_get(args, "prompt"));
	struct json_object *system = jb_get(args, "system");
	char const *problem =
		args && !json_object_is_type(args, json_type_object)
		? "Tool arguments must be an object."
		: !prompt || !*prompt ? "The prompt argument must be a non-empty string."
		: system && !jb_text(system) ? "The system argument must be a string."
		: nullptr;
	if (problem) {
		send_result(mcp, to, tool_result(problem, strlen(problem), true), nullptr);
		return;
	}

	char const *sys = system ? jb_text(system) : mcp->ep->system;
	struct json_object *request = ai_request(mcp->ep, sys && *sys ? sys : nullptr,
	                                         prompt);
	char const *body = request
		? json_object_to_json_string_ext(request, JB_WIRE) : nullptr;
	struct mcp_call *call = body ? calloc(1, sizeof *call) : nullptr;
	if (!call) {
		json_object_put(request);
		send_error(to, 500, INTERNAL_ERROR, "Out of memory", nullptr);
		return;
	}

	*call = (struct mcp_call){
		.to = *to,
		.mcp = mcp,
		.owner = p_call,
		.session = meta->session ? strdup(meta->session) : nullptr
	};
	json_object_get(call->to.id);
	clock_gettime(CLOCK_MONOTONIC, &call->start);
	list_append(&mcp->calls, &call->hook);

	int e = meta->session && !call->session ? ENOMEM
		: http_post_json(mcp->http, mcp->ep->url, body, mcp->ep->headers,
		                 mcp->ep->timeout_ms, call_write, call_done, call,
		                 &call->request);
	json_object_put(request);
	if (e) {
		call_free(call);
		send_error(to, 500, INTERNAL_ERROR, "Cannot start the upstream request",
		           nullptr);
		return;
	}
	*p_call = call;
}

static void
initialize (struct mcp const    *mcp,
            struct target const *to,
            struct json_object  *params)
{
	char const *version = jb_text(jb_get(params, "protocolVersion"));
	if (!is_legacy(version))
		version = versions[MODERN];

	/* Never stored: the ID only scopes the request IDs a client cancels. */
	static char const hex[] = "0123456789abcdef";
	unsigned char rnd[16];
	char session[2 * sizeof rnd + 1] = "";
	if (getrandom(rnd, sizeof rnd, 0) == (ssize_t)sizeof rnd) {
		for (size_t i = 0; i < sizeof rnd; ++i) {
			session[2 * i] = hex[rnd[i] >> 4];
			session[2 * i + 1] = hex[rnd[i] & 15];
		}
	}

	send_result(mcp, to, jb_object(
		"protocolVersion", json_object_new_string(version),
		"capabilities", jb_object("tools", jb_object(nullptr), nullptr),
		"serverInfo", json_object_get(mcp->info),
		nullptr), *session ? session : nullptr);
}

static void
cancel (struct mcp            *mcp,
        struct mcp_meta const *meta,
        struct json_object    *params)
{
	struct json_object *id = jb_get(params, "requestId");
	if (!id)
		return;

	struct list *node;
	struct list *tmp;
	list_foreach_safe (node, tmp, &mcp->calls) {
		struct mcp_call *call = container_of(node, struct mcp_call, hook);
		if (!same(call->session, meta->session) ||
		    !json_object_equal(call->to.id, id))
			continue;

		/* The client gave up on the response; end its POST quietly. */
		http_cancel(mcp->http, &call->request);
		*call->owner = nullptr;
		call->to.reply(call->to.user, 202, nullptr, 0, nullptr);
		if (mcp->verbose)
			fprintf(stderr, "askai: tool call cancelled after %.1f s\n",
			        elapsed(&call->start));
		call_free(call);
	}
}

/* Validate the per-request metadata of a modern request. */
static bool
modern_valid (struct mcp const      *mcp,
              struct mcp_meta const *meta,
              struct target const   *to,
              char const            *method,
              struct json_object    *params)
{
	struct json_object *m = jb_get(params, "_meta");
	char const *version = jb_text(jb_get(m, meta_version));
	if (!version || !json_object_is_type(jb_get(m, meta_caps), json_type_object)) {
		send_error(to, 400, INVALID_PARAMS, "Request _meta must carry "
		           "protocolVersion and clientCapabilities", nullptr);
		return false;
	}
	if (!same(meta->version, version)) {
		send_error(to, 400, HEADER_MISMATCH, "MCP-Protocol-Version header "
		           "does not match the request _meta", nullptr);
		return false;
	}
	if (version_index(version) >= MODERN) {
		send_error(to, 400, UNSUPPORTED_VERSION, "Unsupported protocol version",
		           jb_object("supported", json_object_get(mcp->versions),
		                     "requested", json_object_new_string(version),
		                     nullptr));
		return false;
	}
	if (!same(meta->method, method)) {
		send_error(to, 400, HEADER_MISMATCH,
		           "Mcp-Method header does not match the request method", nullptr);
		return false;
	}
	char const *name = jb_text(jb_get(params, "name"));
	if (!strcmp(method, "tools/call") && name && !same(meta->name, name)) {
		send_error(to, 400, HEADER_MISMATCH,
		           "Mcp-Name header does not match the tool name", nullptr);
		return false;
	}
	return true;
}

static void
dispatch (struct mcp             *mcp,
          struct mcp_meta const  *meta,
          struct json_object     *msg,
          struct target          *to,
          struct mcp_call       **p_call)
{
	char const *method = jb_text(jb_get(msg, "method"));
	struct json_object *params = jb_get(msg, "params");
	struct json_object *id;
	bool has_id = json_object_object_get_ex(msg, "id", &id);

	if (!jb_is(jb_get(msg, "jsonrpc"), "2.0") ||
	    (!method && !(has_id && (jb_get(msg, "result") || jb_get(msg, "error"))))) {
		send_error(to, 400, INVALID_REQUEST, "Invalid Request", nullptr);
		return;
	}
	if (!method || !has_id) {
		/* Responses (we send no requests) and notifications. */
		if (method && !strcmp(method, "notifications/cancelled"))
			cancel(mcp, meta, params);
		to->reply(to->user, 202, nullptr, 0, nullptr);
		return;
	}
	if (!json_object_is_type(id, json_type_string) &&
	    !json_object_is_type(id, json_type_int) &&
	    !json_object_is_type(id, json_type_double)) {
		send_error(to, 400, INVALID_REQUEST, "Invalid request id", nullptr);
		return;
	}
	to->id = id;
	if (params && !json_object_is_type(params, json_type_object)) {
		send_error(to, 400, INVALID_PARAMS, "Params must be an object", nullptr);
		return;
	}

	char const *version = jb_text(jb_get(jb_get(params, "_meta"), meta_version));
	to->modern = (version && !is_legacy(version)) ||
	             (meta->version && !is_legacy(meta->version));
	if (to->modern && !modern_valid(mcp, meta, to, method, params))
		return;

	if (!strcmp(method, "tools/call"))
		tools_call(mcp, meta, to, params, p_call);
	else if (!strcmp(method, "tools/list"))
		send_result(mcp, to, cacheable(to, jb_object(
			"tools", json_object_get(mcp->tools), nullptr)), nullptr);
	else if (to->modern && !strcmp(method, "server/discover"))
		send_result(mcp, to, cacheable(to, jb_object(
			"supportedVersions", json_object_get(mcp->versions),
			"capabilities", jb_object("tools", jb_object(nullptr), nullptr),
			nullptr)), nullptr);
	else if (!to->modern && !strcmp(method, "initialize"))
		initialize(mcp, to, params);
	else if (!to->modern && !strcmp(method, "ping"))
		send_result(mcp, to, json_object_new_object(), nullptr);
	else
		/* Legacy clients take a 404 for an expired session, so keep 200. */
		send_error(to, to->modern ? 404 : 200, METHOD_NOT_FOUND,
		           "Method not found", nullptr);
}

void
mcp_post (struct mcp             *mcp,
          struct mcp_meta const  *meta,
          char const             *body,
          size_t                  len,
          mcp_reply_cb           *reply,
          void                   *user,
          struct mcp_call       **p_call)
{
	struct target to = {
		.reply = reply,
		.user = user
	};
	struct json_object *msg = jb_parse(body, len, JSON_TOKENER_STRICT |
	                                              JSON_TOKENER_VALIDATE_UTF8);
	if (!msg) {
		send_error(&to, 400, PARSE_ERROR, "Parse error", nullptr);
		return;
	}
	dispatch(mcp, meta, msg, &to, p_call);
	json_object_put(msg);
}

void
mcp_cancel (struct mcp       *mcp,
            struct mcp_call **p_call)
{
	struct mcp_call *call = *p_call;
	if (!call)
		return;
	*p_call = nullptr;
	http_cancel(mcp->http, &call->request);
	call_free(call);
}

static struct json_object *
tool_definition (struct mcp_tool const *tool)
{
	return jb_object(
		"name", json_object_new_string(tool->name),
		"title", json_object_new_string("Ask AI"),
		"description", json_object_new_string(tool->description),
		"inputSchema", jb_object(
			"type", json_object_new_string("object"),
			"properties", jb_object(
				"prompt", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"The complete, self-contained question or task. The "
						"other AI cannot see this conversation, so include "
						"all the context it needs."),
					nullptr),
				"system", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"Optional system instructions for the other AI, such "
						"as a role or an answer format."),
					nullptr),
				nullptr),
			"required", jb_list(json_object_new_string("prompt")),
			"additionalProperties", json_object_new_boolean(0),
			nullptr),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(1),
			"openWorldHint", json_object_new_boolean(1),
			nullptr),
		nullptr);
}

int
mcp_init (struct mcp               **p_mcp,
          struct http               *http,
          struct ai_endpoint const  *ep,
          struct mcp_tool const     *tool,
          bool                       verbose)
{
	struct mcp *mcp = calloc(1, sizeof *mcp);
	if (!mcp)
		return ENOMEM;

	*mcp = (struct mcp){
		.info = jb_object("name", json_object_new_string("askai"),
		                  "title", json_object_new_string("Ask AI"),
		                  "version", json_object_new_string("0.1.0"),
		                  nullptr),
		.tools = jb_list(tool_definition(tool)),
		.versions = json_object_new_array(),
		.http = http,
		.ep = ep,
		.tool = tool->name,
		.verbose = verbose
	};
	list_init(&mcp->calls);
	bool ok = mcp->info && mcp->tools;
	for (size_t i = 0; ok && i < ARRAY_SIZE(versions); ++i)
		ok = jb_push(mcp->versions, json_object_new_string(versions[i]));
	*p_mcp = mcp;
	if (!ok) {
		mcp_fini(p_mcp);
		return ENOMEM;
	}
	return 0;
}

void
mcp_fini (struct mcp **p_mcp)
{
	if (!p_mcp || !*p_mcp)
		return;
	struct mcp *mcp = *p_mcp;
	*p_mcp = nullptr;

	struct list *node;
	struct list *tmp;
	list_foreach_safe (node, tmp, &mcp->calls) {
		struct mcp_call *call = container_of(node, struct mcp_call, hook);
		mcp_cancel(mcp, call->owner);
	}
	json_object_put(mcp->versions);
	json_object_put(mcp->tools);
	json_object_put(mcp->info);
	free(mcp);
}
