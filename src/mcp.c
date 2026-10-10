/** @file MCP's Streamable HTTP transport and JSON-RPC semantics, over request
 *  streams (src/stream.h).
 *
 * Two protocol eras are served side by side.  Legacy clients (revisions up to
 * 2025-11-25, which includes llama.cpp) open with an initialize handshake;
 * modern clients (2026-07-28) are stateless and declare their version in each
 * request's _meta, mirrored into HTTP headers that must match the body.
 *
 * A request's body is copied and pre-scanned for its JSON's nesting, values,
 * object members and number lengths as it arrives, then parsed within the
 * step's share of work (loop.h), a slice a bulk step once it exceeds that, by
 * tokeners the loop reuses.  A large message is freed the same way, by its own
 * request after the reply has gone out; the request's stream ends only then,
 * so its client's next request waits for that, and memory for messages is
 * bounded by the requests under way.  An upstream answer is parsed and freed
 * in slices too.  Replies are built from fragments, the unchanging ones
 * serialized once in the shared, immutable mcp_static, so no json-c value is
 * shared between replies or loops, and go out a piece a step, each piece once
 * the transport has room for it.  A finished request is kept for reuse, its
 * buffers' storage given back to the loop's stash (src/buf.h) and that of its
 * answer and pre-scan kept, until memory pressure has it freed; the one that
 * ended last is reused first, so steady requests reuse the same few.
 *
 * A tool call answers on an event stream (SSE) when the client accepts one
 * and its era has them (all but 2024-11-05): id-less data: events, and a ':'
 * comment every keepalive seconds while the upstream works.  Closing the
 * stream, which in HTTP/1.1 means closing the connection or its sending side,
 * cancels the call in every era; legacy clients may also cancel it with
 * notifications/cancelled.
 *
 * GET /metrics answers a trusted client (src/stream.h) with the metrics,
 * rendered a share a step (src/metrics.h).  Draining, tool calls that wait
 * for the upstream, and those yet to start, end with an error, which on an
 * event stream is its last event.
 */
#include <errno.h>
#include <json-c/printbuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <time.h>

#include "ai.h"
#include "buf.h"
#include "dstr.h"
#include "http.h"
#include "jb.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "mcp.h"
#include "metrics.h"
#include "stream.h"
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

enum {
	MAX_RESPONSE = 16 << 20,  /* upstream response bytes */
	MAX_ID       = 1 << 10,   /* string request id bytes: every reply repeats it */
	MAX_ECHO     = 256,       /* bytes of a client's string an error repeats */
	MAX_SCALAR   = 1 << 10,   /* bytes of a JSON number or literal, which json-c
	                             converts whole: no valid one needs more */
	CACHE_TTL_MS = 300'000,   /* how long modern clients may cache lists */
};

static constexpr uint64_t NS = 1'000'000'000;

/** Newest first; the first MODERN revisions carry their version in _meta. */
static char const *const versions[] = {
	"2026-07-28",
	"2025-11-25",
	"2025-06-18",
	"2025-03-26",
	"2024-11-05",
};
enum { MODERN = 1 };

/** The revision before Streamable HTTP, whose clients take no event stream. */
static char const pre_streams[] = "2024-11-05";

static char const meta_version[] = "io.modelcontextprotocol/protocolVersion";
static char const meta_caps[] = "io.modelcontextprotocol/clientCapabilities";
static char const meta_server[] = "io.modelcontextprotocol/serverInfo";

/** The request header fields read here, in field() order. */
enum { F_TYPE, F_ACCEPT, F_VERSION, F_METHOD, F_NAME, F_SESSION, F_N };

static char const *const field_names[F_N] = {
	[F_TYPE]    = "content-type",
	[F_ACCEPT]  = "accept",
	[F_VERSION] = "mcp-protocol-version",
	[F_METHOD]  = "mcp-method",
	[F_NAME]    = "mcp-name",
	[F_SESSION] = "mcp-session-id",
};

struct mcp_static {
	struct ai_endpoint const *ep;
	char const               *tool;
	struct dstr               list[2];      /**< tools/list results: legacy, modern */
	struct dstr               discover;     /**< the server/discover result */
	struct dstr               init_tail;    /**< an initialize result after the
	                                             version */
	struct dstr               modern_tail;  /**< closes a modern result */
	struct dstr               versions;     /**< the supported versions, an array */
	struct mcp_limits         lim;
};

/** Where a request stands. */
enum phase {
	PHASE_BODY,      /* taking the body in */
	PHASE_PARSE,     /* parsing it, a slice a step */
	PHASE_DISPATCH,  /* handing the message on, in a step of its own */
	PHASE_BUILD,     /* writing the upstream request, a slice a step */
	PHASE_CALL,      /* waiting for the upstream */
	PHASE_ANSWER,    /* parsing its answer, a slice a step */
	PHASE_TEXT,      /* taking the tool's output from that, in a step of its own */
	PHASE_REPLY,     /* escaping the output into the reply, a slice a step */
	PHASE_SEND,      /* sending the reply, a piece a step */
	PHASE_FREE,      /* freeing the message, a slice a step, then ending */
	PHASE_METRICS,   /* rendering the metrics, a share a step */
};

/* Where a client's string goes in the upstream request. */
struct splice {
	struct json_object *str;
	size_t              at;  /* offset into the draft's text */
};

/** A request, from its stream's head to its end or cancellation. */
struct mcp_req {
	struct list          hook;       /* on mcp->calls while a tool call runs,
	                                    then on mcp->freeing */
	struct loop_step     step;       /* the phase's work, and keep-alives */
	struct loop_timer    timer;      /* the next keep-alive */
	struct buf           body;       /* the request body */
	struct buf           upload;     /* the upstream request */
	struct buf           response;   /* the upstream body as it arrives */
	struct buf           reply;      /* our response body, until sent */
	struct jb_parser     parser;     /* of the body, then of the answer */
	struct jb_reaper     reaper;     /* what is freed a slice a step */
	struct timespec      start;
	struct splice        splice[2];  /* prompt and system, in draft order */
	struct dstr          answer;     /* the tool's output; its storage stays
	                                    with the request for reuse */
	struct stream       *s;          /* nullptr once it is gone */
	struct mcp          *mcp;
	struct json_object  *msg;        /* parsed, until dispatched; then the
	                                    upstream's answer, until read */
	struct json_object  *id;         /* the request's id, if any */
	struct json_object  *draft;      /* the upstream request without the
	                                    client's strings, until written */
	char const          *text;       /* the draft serialized */
	size_t               len;
	size_t               off;        /* bytes of the next splice written, of
	                                    the answer escaped, or metrics done */
	size_t               values;     /* of msg */
	uint64_t             head;       /* loop_clock() at the request's head */
	struct http_request *request;
	struct jb_scan       scan;       /* the body's pre-scan */
	long                 status;     /* the upstream's HTTP status */
	int                  error;      /* to end the stream with */
	uint16_t             splices;
	uint16_t             next;       /* the splice to write next */
	uint8_t              phase;      /* enum phase */
	uint8_t              method;     /* enum metrics_method */
	bool                 modern;
	bool                 sse_ok;     /* the client takes an event stream */
	bool                 sse;        /* the response is one */
	bool                 failed;     /* the answer reports an error */
	bool                 overflow;   /* the upstream response was too large */
	bool                 ping;       /* a keep-alive is due */
	bool                 nomem;      /* building the reply ran out of memory */
	bool                 blocked;    /* waiting for on_writable() */
};

/* What the server counts, for any thread to read: allocated apart, so that
 * its atomic members are initialized where they stay (hist.h). */
struct mcp_counts {
	struct metrics_kept kept;
	struct hist         methods[METHOD_N];  /* by enum metrics_method */
};

struct mcp {
	struct list              calls;     /* requests with a tool call under way */
	struct list              freeing;   /* requests freeing their message */
	struct list              spare;     /* finished requests, kept for reuse,
	                                       the last ended last */
	struct loop_step         drain;     /* ends the calls, draining */
	struct stream_app        app;
	struct jb_pool           pool;      /* for request bodies */
	struct jb_pool           answers;   /* for upstream answers */
	struct metrics_src       metrics;   /* counts' */
	struct mcp_counts       *counts;
	struct loop             *loop;
	struct http             *http;
	struct mcp_static const *st;
	size_t                   spares;    /* on spare */
	size_t                   low;       /* fewest on spare in this window */
	size_t                   idle;      /* fewest on spare in the window before */
	uint64_t                 draining;  /* bool, widened over the padding */
};

/* A run of reply bytes. */
struct frag {
	char const *ptr;
	size_t      len;
};

#define FRAG(s) ((struct frag){ s, sizeof s - 1 })
#define TEXT(d) ((struct frag){ dstr_get(d), dstr_len(d) })

static bool
span_is (struct span  s,
         char const  *str)
{
	size_t len = strlen(str);
	return s.len == len && !memcmp(s.ptr, str, len);
}

/* Whether field @p f equals @p str, both being absent alike. */
static bool
same (struct span  f,
      char const  *str)
{
	return !str ? !f.ptr : f.ptr && span_is(f, str);
}

/* Whether fields @p a and @p b are equal, both being absent alike. */
static bool
same_field (struct span a,
            struct span b)
{
	return !a.ptr ? !b.ptr : b.ptr && a.len == b.len && !memcmp(a.ptr, b.ptr, a.len);
}

/* Index into versions[], ARRAY_SIZE(versions) if @p v is unknown or absent. */
static size_t
version_index (char const *v,
               size_t      len)
{
	size_t i = 0;
	while (v && i < ARRAY_SIZE(versions) &&
	       (strlen(versions[i]) != len || memcmp(versions[i], v, len)))
		++i;
	return v ? i : ARRAY_SIZE(versions);
}

static bool
is_legacy (char const *v,
           size_t      len)
{
	size_t i = version_index(v, len);
	return i >= MODERN && i < ARRAY_SIZE(versions);
}

static bool
is_json (struct span type)
{
	static char const json[] = "application/json";
	size_t n = sizeof json - 1;
	return type.len >= n && !strncasecmp((char const *)type.ptr, json, n) &&
	       (type.len == n || type.ptr[n] == ';' || type.ptr[n] == ' ' ||
	        type.ptr[n] == '\t');
}

/* Whether a request's target is the metrics. */
static bool
is_metrics (struct span target)
{
	static char const path[] = "/metrics";
	size_t n = sizeof path - 1;
	return target.len >= n && !memcmp(target.ptr, path, n) &&
	       (target.len == n || target.ptr[n] == '?');
}

/* The enum metrics_method of @p method. */
static uint8_t
method_index (char const *method)
{
	static char const *const names[METHOD_N] = {
		[METHOD_INITIALIZE] = "initialize",
		[METHOD_PING]       = "ping",
		[METHOD_TOOLS_LIST] = "tools/list",
		[METHOD_TOOLS_CALL] = "tools/call",
		[METHOD_DISCOVER]   = "server/discover",
	};
	uint8_t i = METHOD_N;
	while (--i && (!names[i] || strcmp(method, names[i])))
		;
	return i;
}

/* Whether an Accept field lists text/event-stream. */
static bool
accepts_sse (struct span accept)
{
	static char const sse[] = "text/event-stream";
	size_t n = sizeof sse - 1;
	char const *p = (char const *)accept.ptr;
	char const *end = p + accept.len;
	while (p < end) {
		while (p < end && (*p == ' ' || *p == '\t' || *p == ','))
			++p;
		char const *comma = memchr(p, ',', (size_t)(end - p));
		char const *stop = comma ? comma : end;
		char const *semi = memchr(p, ';', (size_t)(stop - p));
		char const *type = semi ? semi : stop;
		while (type > p && (type[-1] == ' ' || type[-1] == '\t'))
			--type;
		if ((size_t)(type - p) == n && !strncasecmp(p, sse, n))
			return true;
		p = stop;
	}
	return false;
}

static double
elapsed (struct timespec const *start)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - start->tv_sec) +
	       (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

/* The first @p max bytes of @p len at most, never ending within a character. */
static size_t
clip (char const *s,
      size_t      len,
      size_t      max)
{
	if (len <= max)
		return len;
	while (max && ((unsigned char)s[max] & 0xc0) == 0x80)
		--max;
	return max;
}

/* Free the message, or the answer, now if the step has the share for it,
 * else leave it to the request's steps, a few values each: freeing one takes
 * as long as parsing it. */
static void
req_dispose (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	struct json_object *msg = req->msg;
	req->msg = nullptr;
	if (!msg)
		return;
	if (req->values <= loop_room(loop, LOOP_VALUES)) {
		json_object_put(msg);
		loop_work(loop, LOOP_VALUES, req->values);
	} else {
		jb_reap(&req->reaper, msg);
		loop_ready_bulk(loop, &req->step);
	}
}

/* The bytes a finished request keeps for reuse. */
static uint64_t
req_kept (struct mcp_req const *req)
{
	return sizeof *req + dstr_get_block_count(&req->answer) * DSTR_BLK_SIZE +
	       req->scan.cap * sizeof *req->scan.members +
	       req->reaper.cap * sizeof *req->reaper.stack;
}

/* End the stream, if the request still has it, and keep the request for
 * reuse. */
static void
req_end (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	hist_record(&mcp->counts->methods[req->method], loop_clock() - req->head);
	if (req->s)
		req->s->ops->end(req->s, req->error);
	if (req->hook.next)
		list_del(&req->hook);
	loop_unready(&req->step);
	list_append(&mcp->spare, &req->hook);
	++mcp->spares;
	hist_add(&mcp->counts->kept.spares, 1);
	hist_add(&mcp->counts->kept.bytes[0], req_kept(req));
	atomic_store_explicit(&mcp->counts->kept.bytes[1],
	                      mcp->pool.bytes + mcp->answers.bytes, memory_order_relaxed);
}

/* A request to begin, the spare one that ended last if there is one, whose
 * storage is the likeliest to fit; nullptr if out of memory.  Its answer,
 * pre-scan and reaper are empty, their storage kept. */
static struct mcp_req *
req_new (struct mcp *mcp)
{
	if (list_is_empty(&mcp->spare)) {
		struct mcp_req *req = malloc(sizeof *req);
		if (req)
			*req = (struct mcp_req){ .answer = DSTR_INIT };
		return req;
	}
	struct mcp_req *req = container_of(mcp->spare.prev, struct mcp_req, hook);
	list_del(&req->hook);
	if (--mcp->spares < mcp->low)
		mcp->low = mcp->spares;
	hist_add(&mcp->counts->kept.spares, -1ULL);
	hist_add(&mcp->counts->kept.bytes[0], -req_kept(req));
	return req;
}

/* Free the spare request that ended first, and the values it was left to
 * free. */
static void
req_free (struct mcp *mcp)
{
	struct mcp_req *req = container_of(mcp->spare.next, struct mcp_req, hook);
	list_del(&req->hook);
	--mcp->spares;
	mcp->low -= mcp->low > 0;
	mcp->idle -= mcp->idle > 0;
	hist_add(&mcp->counts->kept.spares, -1ULL);
	hist_add(&mcp->counts->kept.bytes[0], -req_kept(req));
	jb_reaper_fini(&req->reaper);
	jb_scan_end(&req->scan);
	dstr_cleanup(&req->answer);
	free(req);
}

/* The reply is queued, or the stream gone: let go of everything at once but
 * the values still to free, and end once those are freed. */
static void
req_finish (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	if (req->hook.next) {
		list_del(&req->hook);
		req->hook = (struct list){};
	}
	http_cancel(mcp->http, &req->request);
	loop_timer_stop(mcp->loop, &req->timer);
	jb_parse_end(&req->parser, req->phase == PHASE_ANSWER ? &mcp->answers
	                                                      : &mcp->pool,
	             &req->reaper);
	req_dispose(req);
	json_object_put(req->draft);
	json_object_put(req->id);
	req->draft = req->id = nullptr;
	jb_scan_reset(&req->scan);
	buf_free(&req->body);
	buf_free(&req->upload);
	buf_free(&req->response);
	buf_free(&req->reply);
	dstr_clear(&req->answer);
	if (!req->reaper.n) {
		req_end(req);
		return;
	}
	req->phase = PHASE_FREE;
	list_append(&mcp->freeing, &req->hook);
	loop_ready_bulk(mcp->loop, &req->step);
}

/* Answer a request MCP takes no part in with an HTTP error, the stream
 * having no request state. */
static void
http_error (struct stream *s,
            uint16_t       status,
            char const    *message,
            char const    *fields)
{
	char head[160];
	char body[160];
	int h = snprintf(head, sizeof head, "Content-Type: application/json\r\n%s",
	                 fields);
	int b = snprintf(body, sizeof body, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":"
	                 "-32600,\"message\":\"%s\"}}", message);
	if (!s->ops->head(s, status, (struct span){ (unsigned char *)head, (size_t)h },
	                  (size_t)b))
		(void)s->ops->data(s, (struct span){ (unsigned char *)body, (size_t)b });
	s->ops->end(s, 0);
}

/* The same for a request with state, which goes. */
static void
req_http_error (struct mcp_req *req,
                uint16_t        status,
                char const     *message)
{
	http_error(req->s, status, message, "");
	req->s = nullptr;
	req_finish(req);
}

/* Queue @p bytes of the response; false if the stream can carry no more.
 * Once the transport asks to wait, the request waits for on_writable(). */
static bool
req_write (struct mcp_req *req,
           struct span     bytes)
{
	int e = req->s->ops->data(req->s, bytes);
	req->blocked |= e == EAGAIN;
	return e != EPIPE;
}

/* Send as much of the reply as the step has the share for, the rest a piece
 * a bulk step, each once the transport has room for it; then finish. */
static void
req_send (struct mcp_req *req)
{
	if (req->blocked)
		return;
	struct span reply = buf_peek(&req->reply);
	size_t room = loop_room(req->mcp->loop, LOOP_SEND);
	size_t n = reply.len < room ? reply.len : room;
	if (n && !req_write(req, (struct span){ reply.ptr, n })) {
		req->error = EPIPE;
		req_finish(req);
		return;
	}
	buf_consume(&req->reply, n);
	if (n == reply.len)
		req_finish(req);
	else if (!req->blocked)
		loop_ready_bulk(req->mcp->loop, &req->step);
}

/* Begin the reply body: on an event stream, an event. */
static void
reply_open (struct mcp_req *req)
{
	buf_consume(&req->reply, buf_peek(&req->reply).len);
	req->nomem = false;
	if (req->sse) {
		struct span s = buf_reserve(&req->reply, 6, SIZE_MAX);
		req->nomem = !s.len;
		if (s.len) {
			memcpy(s.ptr, "data: ", 6);
			buf_commit(&req->reply, 6);
		}
	}
}

static void
reply_put (struct mcp_req    *req,
           struct frag const *f,
           size_t             n)
{
	for (size_t i = 0; i < n && !req->nomem; ++i) {
		struct span s = f[i].len ? buf_reserve(&req->reply, f[i].len, SIZE_MAX)
		                         : (struct span){};
		req->nomem = f[i].len && !s.len;
		if (s.len) {
			memcpy(s.ptr, f[i].ptr, f[i].len);
			buf_commit(&req->reply, f[i].len);
		}
	}
}

/* Escape up to @p len bytes of @p src into the reply. */
static void
reply_escape (struct mcp_req *req,
              char const     *src,
              size_t          len)
{
	struct span s = req->nomem || !len ? (struct span){}
	              : buf_reserve(&req->reply, 6 * len, SIZE_MAX);
	req->nomem |= len && !s.len;
	if (s.len)
		buf_commit(&req->reply, jb_escape((char *)s.ptr, src, len));
}

/* The reply's start: '{"jsonrpc":"2.0","id":ID,"result":', or "error". */
static void
reply_envelope (struct mcp_req *req,
                bool            error)
{
	size_t len = 0;
	char const *id = req->id ? json_object_to_json_string_length(req->id, JB_WIRE,
	                                                             &len)
	                         : nullptr;
	struct frag const f[] = {
		FRAG("{\"jsonrpc\":\"2.0\","),
		id ? FRAG("\"id\":") : FRAG(""),
		{ id, len },
		id ? FRAG(",") : FRAG(""),
		error ? FRAG("\"error\":") : FRAG("\"result\":"),
	};
	req->nomem |= req->id && !id;
	reply_put(req, f, ARRAY_SIZE(f));
}

/* Send the reply, as JSON with @p status or as the stream's last event, then
 * finish the request. */
static void
reply_close (struct mcp_req *req,
             uint16_t        status,
             char const     *session)
{
	struct stream *s = req->s;
	if (req->sse)
		reply_put(req, &FRAG("\n\n"), 1);
	req->error = req->nomem ? ENOMEM : 0;
	if (!req->error && !req->sse) {
		char fields[128];
		int n = snprintf(fields, sizeof fields, "Content-Type: application/json\r\n"
		                 "%s%s%s", session ? "Mcp-Session-Id: " : "",
		                 session ? session : "", session ? "\r\n" : "");
		req->error = s->ops->head(s, status, (struct span){ (unsigned char *)fields,
		                                                    (size_t)n },
		                          buf_peek(&req->reply).len);
	}
	if (req->error) {
		req_finish(req);
		return;
	}
	req->phase = PHASE_SEND;
	req_send(req);
}

/* A JSON-RPC error; @p message must be JSON string content already. */
static void
reply_error (struct mcp_req    *req,
             uint16_t           status,
             int                code,
             char const        *message,
             struct frag const *data,
             size_t             n_data)
{
	char num[16];
	int n = snprintf(num, sizeof num, "%d", code);
	reply_open(req);
	reply_envelope(req, true);
	struct frag const f[] = {
		FRAG("{\"code\":"),
		{ num, (size_t)n },
		FRAG(",\"message\":\""),
		{ message, strlen(message) },
		FRAG("\""),
	};
	reply_put(req, f, ARRAY_SIZE(f));
	if (n_data) {
		reply_put(req, &FRAG(",\"data\":"), 1);
		reply_put(req, data, n_data);
	}
	reply_put(req, &FRAG("}}"), 1);
	reply_close(req, status, nullptr);
}

/* A result made of @p n fragments. */
static void
reply_result (struct mcp_req    *req,
              struct frag const *result,
              size_t             n,
              char const        *session)
{
	reply_open(req);
	reply_envelope(req, false);
	reply_put(req, result, n);
	reply_put(req, &FRAG("}"), 1);
	reply_close(req, 200, session);
}

/* Begin a tool result: its text follows, escaped. */
static void
tool_open (struct mcp_req *req)
{
	reply_open(req);
	reply_envelope(req, false);
	reply_put(req, &FRAG("{\"content\":[{\"type\":\"text\",\"text\":\""), 1);
}

static void
tool_close (struct mcp_req *req)
{
	struct mcp_static const *st = req->mcp->st;
	struct frag const f[] = {
		FRAG("\"}]"),
		req->failed ? FRAG(",\"isError\":true") : FRAG(""),
		req->modern ? TEXT(&st->modern_tail) : FRAG("}"),
		FRAG("}"),
	};
	reply_put(req, f, ARRAY_SIZE(f));
	reply_close(req, 200, nullptr);
}

/* A tool result of @p text, short and ours. */
static void
tool_result (struct mcp_req *req,
             char const     *text,
             bool            failed)
{
	req->failed = failed;
	tool_open(req);
	reply_escape(req, text, strlen(text));
	tool_close(req);
}

/* Escape as much of the answer into the reply as the step has the share
 * for; the rest goes on in bulk steps. */
static void
req_reply (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	size_t len = dstr_len(&req->answer) - req->off;
	size_t room = loop_room(loop, LOOP_ESCAPE);
	size_t n = len < room ? len : room;
	reply_escape(req, dstr_get(&req->answer) + req->off, n);
	loop_work(loop, LOOP_ESCAPE, n);
	req->off += n;
	if (n < len && !req->nomem)
		loop_ready_bulk(loop, &req->step);
	else
		tool_close(req);
}

static size_t
call_write (char const *data,
            size_t      len,
            void       *user)
{
	struct mcp_req *req = user;
	if (!len)
		return 0;
	struct span s = buf_reserve(&req->response, len, MAX_RESPONSE);
	if (!s.len) {
		req->overflow = errno == ENOBUFS;
		return 0;
	}
	memcpy(s.ptr, data, len);
	buf_commit(&req->response, len);
	return len;
}

/* Reply with the tool's output, in req->answer unless building that ran out
 * of memory (@p ok false); it goes out a slice at a time. */
static void
call_reply (struct mcp_req *req,
            bool            ok,
            bool            failed)
{
	char const *str = dstr_get(&req->answer);
	size_t len = dstr_len(&req->answer);
	if (ok && failed)
		LOG(LOG_WARN, "tool call failed after %.1f s: %.*s", elapsed(&req->start),
		    (int)clip(str, len, 300), str);
	else if (ok)
		LOG(LOG_VERBOSE, "tool call answered after %.1f s (%zu bytes)",
		    elapsed(&req->start), len);
	if (!ok) {
		reply_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return;
	}

	loop_timer_stop(req->mcp->loop, &req->timer);
	req->failed = failed;
	req->phase = PHASE_REPLY;
	req->off = 0;
	tool_open(req);
	req_reply(req);
}

/* The upstream has answered: parse that a slice a step, or say why it did
 * not. */
static void
call_done (CURLcode    result,
           long        status,
           char const *curl_error,
           void       *user)
{
	struct mcp_req *req = user;
	req->request = nullptr;
	list_del(&req->hook);
	req->hook = (struct list){};
	req->status = status;
	if (result == CURLE_OK) {
		req->phase = PHASE_ANSWER;
		loop_ready_bulk(req->mcp->loop, &req->step);
		return;
	}

	struct dstr *text = &req->answer;
	bool ok = req->overflow
	        ? dstr_printf(text, "Upstream response exceeds %d MiB.", MAX_RESPONSE >> 20)
	        : result == CURLE_OPERATION_TIMEDOUT
	        ? dstr_printf(text, "Upstream request timed out after %.0f s.",
	                      elapsed(&req->start))
	        : dstr_printf(text, "Upstream request failed: %s%s%s",
	                      curl_easy_strerror(result),
	                      curl_error && *curl_error ? ": " : "",
	                      curl_error ? curl_error : "");
	buf_free(&req->response);
	call_reply(req, ok, true);
}

/* Serialize a client's string as "", noting where its text goes:
 * call_build() escapes that into the upstream request a slice at a time. */
static int
splice_mark (struct json_object *str,
             struct printbuf    *pb,
             int                 level,
             int                 flags)
{
	(void)level;
	(void)flags;
	struct mcp_req *req = json_object_get_userdata(str);
	if (req->splices == ARRAY_SIZE(req->splice) ||
	    printbuf_strappend(pb, "\"\"") < 0)
		return -1;
	req->splice[req->splices++] = (struct splice){ str, (size_t)pb->bpos - 1 };
	return 0;
}

/* Write the draft from where splice @p k - 1 goes to where splice @p k goes,
 * or to its end; false if out of memory. */
static bool
call_copy (struct mcp_req *req,
           size_t          k)
{
	size_t from = k ? req->splice[k - 1].at : 0;
	size_t len = (k < req->splices ? req->splice[k].at : req->len) - from;
	struct span s = len ? buf_reserve(&req->upload, len, SIZE_MAX) : (struct span){};
	if (len && !s.len)
		return false;
	if (len)
		memcpy(s.ptr, req->text + from, len);
	buf_commit(&req->upload, len);
	return true;
}

/* Escape as much of a client's string into the upstream request as the step
 * has the share for, with the draft that follows it; once all is written,
 * start it. */
static void
call_build (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct loop *loop = mcp->loop;
	bool ok = true;
	if (req->next < req->splices) {
		struct json_object *str = req->splice[req->next].str;
		size_t len = (size_t)json_object_get_string_len(str) - req->off;
		size_t room = loop_room(loop, LOOP_ESCAPE);
		size_t n = len < room ? len : room;
		struct span s = n ? buf_reserve(&req->upload, 6 * n, SIZE_MAX)
		                  : (struct span){};
		ok = !n || s.len;
		if (n && ok) {
			buf_commit(&req->upload, jb_escape((char *)s.ptr,
			                                   json_object_get_string(str) +
			                                   req->off, n));
			loop_work(loop, LOOP_ESCAPE, n);
			req->off += n;
		}
		if (ok && n == len) {
			req->off = 0;
			ok = call_copy(req, ++req->next);
		}
	}
	if (ok && req->next < req->splices) {
		loop_ready_bulk(loop, &req->step);
		return;
	}

	struct ai_endpoint const *ep = mcp->st->ep;
	struct span body = buf_peek(&req->upload);
	json_object_put(req->draft);
	req->draft = nullptr;
	req->phase = PHASE_CALL;
	if (!ok || http_post_json(mcp->http, ep->url, (char const *)body.ptr, body.len,
	                          false, ep->headers, ep->timeout_ms, call_write,
	                          call_done, req, &req->request))
		reply_error(req, 500, INTERNAL_ERROR, "Cannot start the upstream request",
		            nullptr, 0);
}

/* Open the reply as an event stream, which keep-alives keep open while the
 * upstream works. */
static void
req_stream (struct mcp_req *req)
{
	static char const fields[] = "Content-Type: text/event-stream\r\n"
	                             "Cache-Control: no-cache\r\n"
	                             "X-Accel-Buffering: no\r\n";
	struct mcp *mcp = req->mcp;
	struct stream *s = req->s;
	req->sse = !s->ops->head(s, 200, (struct span){ (unsigned char *)fields,
	                                                sizeof fields - 1 }, STREAM_OPEN);
	uint32_t keepalive = mcp->st->lim.keepalive;
	if (req->sse && keepalive)
		(void)loop_timer_set(mcp->loop, &req->timer,
		                     mcp->loop->now + keepalive * NS);
}

/* Start a tool call; false if the request is finished instead. */
static bool
tools_call (struct mcp_req     *req,
            struct json_object *params)
{
	struct mcp *mcp = req->mcp;
	struct mcp_static const *st = mcp->st;
	if (mcp->draining) {
		reply_error(req, 503, INTERNAL_ERROR, "Server is shutting down", nullptr, 0);
		return false;
	}
	char const *name = jb_text(jb_get(params, "name"));
	if (!name || strcmp(name, st->tool)) {
		char msg[32 + 6 * 100];
		size_t len = name ? (size_t)json_object_get_string_len(jb_get(params, "name"))
		                  : 0;
		size_t n = name ? jb_escape(msg + 14, name, clip(name, len, 100))
		                : (size_t)sprintf(msg + 14, "(none)");
		memcpy(msg, "Unknown tool: ", 14);
		msg[14 + n] = '\0';
		reply_error(req, 200, INVALID_PARAMS, msg, nullptr, 0);
		return false;
	}

	/* Argument problems go back to the model so it can correct itself. */
	struct json_object *args = jb_get(params, "arguments");
	struct json_object *prompt = jb_get(args, "prompt");
	struct json_object *system = jb_get(args, "system");
	char const *problem =
		args && !json_object_is_type(args, json_type_object)
		? "Tool arguments must be an object."
		: !jb_text(prompt) || !*jb_text(prompt)
		? "The prompt argument must be a non-empty string."
		: system && !jb_text(system) ? "The system argument must be a string."
		: nullptr;
	if (problem) {
		tool_result(req, problem, true);
		return false;
	}

	clock_gettime(CLOCK_MONOTONIC, &req->start);
	list_append(&mcp->calls, &req->hook);

	/* The client's strings, which may be long, are left out of the draft
	 * request, and call_build() escapes them in; a default system prompt
	 * is ours, and goes in with the draft. */
	char const *sys = system ? jb_text(system) : st->ep->system;
	struct json_object *instructions = !sys || !*sys ? nullptr
	                                 : system ? json_object_get(system)
	                                 : json_object_new_string(sys);
	json_object_set_serializer(prompt, splice_mark, req, nullptr);
	if (system)
		json_object_set_serializer(system, splice_mark, req, nullptr);
	if (!sys || !*sys || instructions)
		req->draft = ai_request(st->ep, instructions, prompt);
	json_object_put(instructions);
	req->text = req->draft ? json_object_to_json_string_length(req->draft, JB_WIRE,
	                                                           &req->len)
	                       : nullptr;
	if (!req->text || !call_copy(req, 0)) {
		reply_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return false;
	}
	req->s->ops->waiting(req->s);
	if (req->sse_ok)
		req_stream(req);
	req->phase = PHASE_BUILD;
	loop_ready_bulk(mcp->loop, &req->step);
	return true;
}

static void
initialize (struct mcp_req     *req,
            struct json_object *params)
{
	struct json_object *v = jb_get(params, "protocolVersion");
	char const *version = jb_text(v);
	if (!is_legacy(version, version ? (size_t)json_object_get_string_len(v) : 0))
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

	struct frag const f[] = {
		FRAG("{\"protocolVersion\":\""),
		{ version, strlen(version) },
		TEXT(&req->mcp->st->init_tail),
	};
	reply_result(req, f, ARRAY_SIZE(f), *session ? session : nullptr);
}

/* A legacy client gave up on a call: end its stream quietly.  A call
 * under way has its stream, and so its session's field. */
static void
cancel (struct mcp         *mcp,
        struct span         session,
        struct json_object *params)
{
	struct json_object *id = jb_get(params, "requestId");
	if (!id)
		return;

	struct list *node;
	struct list *tmp;
	list_foreach_safe (node, tmp, &mcp->calls) {
		struct mcp_req *req = container_of(node, struct mcp_req, hook);
		if (!same_field(session, req->s->ops->field(req->s, F_SESSION)) ||
		    !json_object_equal(req->id, id))
			continue;
		if (!req->sse)
			(void)req->s->ops->head(req->s, 202, (struct span){}, 0);
		LOG(LOG_VERBOSE, "tool call cancelled after %.1f s", elapsed(&req->start));
		req_finish(req);
	}
}

/* Validate the per-request metadata of a modern request. */
static bool
modern_valid (struct mcp_req     *req,
              char const         *method,
              struct json_object *params)
{
	struct stream *s = req->s;
	struct json_object *m = jb_get(params, "_meta");
	struct json_object *v = jb_get(m, meta_version);
	char const *version = jb_text(v);
	if (!version || !json_object_is_type(jb_get(m, meta_caps), json_type_object)) {
		reply_error(req, 400, INVALID_PARAMS, "Request _meta must carry "
		            "protocolVersion and clientCapabilities", nullptr, 0);
		return false;
	}
	if (!same(s->ops->field(s, F_VERSION), version)) {
		reply_error(req, 400, HEADER_MISMATCH, "MCP-Protocol-Version header "
		            "does not match the request _meta", nullptr, 0);
		return false;
	}
	size_t len = (size_t)json_object_get_string_len(v);
	if (version_index(version, len) >= MODERN) {
		char echo[6 * MAX_ECHO];
		size_t n = jb_escape(echo, version, clip(version, len, MAX_ECHO));
		struct frag const data[] = {
			FRAG("{\"supported\":"),
			TEXT(&req->mcp->st->versions),
			FRAG(",\"requested\":\""),
			{ echo, n },
			FRAG("\"}"),
		};
		reply_error(req, 400, UNSUPPORTED_VERSION, "Unsupported protocol version",
		            data, ARRAY_SIZE(data));
		return false;
	}
	if (!same(s->ops->field(s, F_METHOD), method)) {
		reply_error(req, 400, HEADER_MISMATCH,
		            "Mcp-Method header does not match the request method", nullptr, 0);
		return false;
	}
	char const *name = jb_text(jb_get(params, "name"));
	if (!strcmp(method, "tools/call") && name &&
	    !same(s->ops->field(s, F_NAME), name)) {
		reply_error(req, 400, HEADER_MISMATCH,
		            "Mcp-Name header does not match the tool name", nullptr, 0);
		return false;
	}
	return true;
}

/* Answer the message, or start the tool call it asks for; false if the
 * request is finished. */
static bool
dispatch (struct mcp_req *req)
{
	struct json_object *msg = req->msg;
	struct mcp *mcp = req->mcp;
	struct mcp_static const *st = mcp->st;
	struct stream *s = req->s;
	char const *method = jb_text(jb_get(msg, "method"));
	struct json_object *params = jb_get(msg, "params");
	struct json_object *id;
	bool has_id = json_object_object_get_ex(msg, "id", &id);

	if (!jb_is(jb_get(msg, "jsonrpc"), "2.0") ||
	    (!method && !(has_id && (jb_get(msg, "result") || jb_get(msg, "error"))))) {
		reply_error(req, 400, INVALID_REQUEST, "Invalid Request", nullptr, 0);
		return false;
	}
	req->method = !method || !has_id ? METHOD_NOTIFY : method_index(method);
	if (!method || !has_id) {
		/* Responses (we send no requests) and notifications. */
		if (method && !strcmp(method, "notifications/cancelled"))
			cancel(mcp, s->ops->field(s, F_SESSION), params);
		(void)s->ops->head(s, 202, (struct span){}, 0);
		req_finish(req);
		return false;
	}
	if (json_object_is_type(id, json_type_string)
	    ? json_object_get_string_len(id) > MAX_ID
	    : !json_object_is_type(id, json_type_int) &&
	      !json_object_is_type(id, json_type_double)) {
		reply_error(req, 400, INVALID_REQUEST, "Invalid request id", nullptr, 0);
		return false;
	}
	req->id = json_object_get(id);
	if (params && !json_object_is_type(params, json_type_object)) {
		reply_error(req, 400, INVALID_PARAMS, "Params must be an object", nullptr, 0);
		return false;
	}

	struct json_object *v = jb_get(jb_get(params, "_meta"), meta_version);
	char const *version = jb_text(v);
	struct span header = s->ops->field(s, F_VERSION);
	req->modern = (version &&
	               !is_legacy(version, (size_t)json_object_get_string_len(v))) ||
	              (header.ptr && !is_legacy((char const *)header.ptr, header.len));
	if (req->modern && !modern_valid(req, method, params))
		return false;
	/* Without the header a legacy client speaks 2025-03-26, which has
	 * event streams; 2024-11-05 has none. */
	req->sse_ok = accepts_sse(s->ops->field(s, F_ACCEPT)) &&
	              (req->modern || !same(header, pre_streams));

	if (!strcmp(method, "tools/call"))
		return tools_call(req, params);
	if (!strcmp(method, "tools/list"))
		reply_result(req, &TEXT(&st->list[req->modern]), 1, nullptr);
	else if (req->modern && !strcmp(method, "server/discover"))
		reply_result(req, &TEXT(&st->discover), 1, nullptr);
	else if (!req->modern && !strcmp(method, "initialize"))
		initialize(req, params);
	else if (!req->modern && !strcmp(method, "ping"))
		reply_result(req, &FRAG("{}"), 1, nullptr);
	else
		/* Legacy clients take a 404 for an expired session, so keep 200. */
		reply_error(req, req->modern ? 404 : 200, METHOD_NOT_FOUND,
		            "Method not found", nullptr, 0);
	return false;
}

#undef TEXT
#undef FRAG

/* Hand the parsed message on; a tool call frees what it does not keep of it
 * while it runs. */
static void
req_dispatch (struct mcp_req *req)
{
	if (!req->msg)
		reply_error(req, 400, PARSE_ERROR, "Parse error", nullptr, 0);
	else if (dispatch(req))
		req_dispose(req);
}

/* Parse as much of @p json as the step has the share for, with a tokener from
 * @p pool; true once the parse is over, with its value in req->msg. */
static bool
req_slice (struct mcp_req *req,
           struct jb_pool *pool,
           struct span     json)
{
	struct loop *loop = req->mcp->loop;
	struct jb_parser *parser = &req->parser;
	uint64_t bytes = loop_room(loop, LOOP_PARSE);
	uint64_t values = loop_room(loop, LOOP_VALUES);
	if (bytes < 4 || !values)
		return false;
	size_t off = parser->off;
	uint32_t seen = parser->cut.values;
	size_t copied = parser->cut.copied;
	bool over = jb_parse_some(parser, pool, (char const *)json.ptr, json.len, bytes,
	                          (uint32_t)values);
	loop_work(loop, LOOP_PARSE, parser->off - off);
	loop_work(loop, LOOP_VALUES, parser->cut.values - seen);
	loop_work(loop, LOOP_COPY, parser->cut.copied - copied);
	if (over) {
		req->values = parser->cut.values;
		req->msg = jb_parse_take(parser);
		jb_parse_end(parser, pool, &req->reaper);
	}
	return over;
}

/* Parse as much of the body as the step has the share for, the rest a slice
 * a bulk step; the message is handed on at once if one step parsed it all,
 * else in a step of its own. */
static void
req_parse (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	bool whole = !req->parser.off;
	if (!req_slice(req, &req->mcp->pool, buf_peek(&req->body))) {
		loop_ready_bulk(loop, &req->step);
		return;
	}
	buf_free(&req->body);
	req->phase = PHASE_DISPATCH;
	if (whole)
		req_dispatch(req);
	else
		loop_ready_bulk(loop, &req->step);
}

/* Parse as much of the upstream's answer as the step has the share for, and
 * check it is UTF-8 text; once it is parsed, or found not to be JSON, read the
 * tool's output from it in a step of its own, as that copies the output. */
static void
req_answer (struct mcp_req *req)
{
	struct span json = buf_peek(&req->response);
	char const *text = json.len ? (char const *)json.ptr : "";
	size_t off = req->parser.off;
	bool over = req_slice(req, &req->mcp->answers, (struct span){
		(unsigned char *)(uintptr_t)text, json.len });
	size_t end = !over ? req->parser.off : req->msg ? json.len : off;
	if (jb_utf8(text + off, end - off) < end - off) {
		jb_parse_end(&req->parser, &req->mcp->answers, &req->reaper);
		req_dispose(req);
		over = true;
	}
	if (over)
		req->phase = PHASE_TEXT;
	loop_ready_bulk(req->mcp->loop, &req->step);
}

/* Read the tool's output from the answer, then let go of that. */
static void
req_text (struct mcp_req *req)
{
	struct span body = buf_peek(&req->response);
	bool failed = true;
	bool ok = !ai_answer(req->mcp->st->ep->protocol, req->status, req->msg,
	                     body.len ? (char const *)body.ptr : "", body.len,
	                     &req->answer, &failed);
	loop_work(req->mcp->loop, LOOP_COPY, dstr_len(&req->answer));
	req_dispose(req);
	buf_free(&req->response);
	call_reply(req, ok, failed);
}

/* Render a share of the metrics into the reply; once all are in, send it. */
static void
req_metrics (struct mcp_req *req)
{
	static char const fields[] = "Content-Type: text/plain; version=0.0.4; "
	                             "charset=utf-8\r\n";
	struct stream *s = req->s;
	int e = metrics_render(&req->mcp->metrics, &req->off, &req->reply);
	if (e == EAGAIN) {
		loop_ready_bulk(req->mcp->loop, &req->step);
		return;
	}
	req->error = e ? e : s->ops->head(s, 200, (struct span){
		(unsigned char *)(uintptr_t)fields, sizeof fields - 1
	}, buf_peek(&req->reply).len);
	if (req->error) {
		req_finish(req);
		return;
	}
	req->phase = PHASE_SEND;
	req_send(req);
}

/* A keep-alive, if one is due before the reply, a slice of what is left to
 * free, then the phase's work. */
static void
req_step (struct loop      *loop,
          struct loop_step *step)
{
	struct mcp_req *req = container_of(step, struct mcp_req, step);
	if (req->ping && req->phase < PHASE_REPLY) {
		/* Output the client has yet to read keeps the stream open too. */
		req->ping = false;
		if (!req->blocked)
			(void)req_write(req, (struct span){ (unsigned char *)":\n\n", 3 });
		(void)loop_timer_set(loop, &req->timer,
		                     loop->now + req->mcp->st->lim.keepalive * NS);
	}
	if (req->reaper.n) {
		loop_work(loop, LOOP_VALUES, jb_reap_some(&req->reaper,
		                                          loop_room(loop, LOOP_VALUES)));
		if (req->reaper.n) {
			loop_ready_bulk(loop, step);
		} else if (req->phase == PHASE_FREE) {
			req_end(req);
			return;
		}
	}
	switch (req->phase) {
	case PHASE_PARSE:
		req_parse(req);
		break;
	case PHASE_DISPATCH:
		req_dispatch(req);
		break;
	case PHASE_BUILD:
		call_build(req);
		break;
	case PHASE_ANSWER:
		req_answer(req);
		break;
	case PHASE_TEXT:
		req_text(req);
		break;
	case PHASE_REPLY:
		req_reply(req);
		break;
	case PHASE_SEND:
		req_send(req);
		break;
	case PHASE_METRICS:
		req_metrics(req);
		break;
	default:
		break;
	}
}

static void
req_timer (struct loop       *loop,
           struct loop_timer *timer)
{
	struct mcp_req *req = container_of(timer, struct mcp_req, timer);
	req->ping = true;
	loop_ready(loop, &req->step);
}

static void
mcp_on_head (void          *ctx,
             struct stream *s,
             struct span    method,
             struct span    target,
             size_t         length)
{
	struct mcp *mcp = ctx;
	bool metrics = span_is(method, "GET") && is_metrics(target);
	if (!metrics && !span_is(method, "POST")) {
		/* No standalone event stream (GET) and no sessions to DELETE. */
		http_error(s, 405, "Method Not Allowed", "Allow: POST, OPTIONS\r\n");
		return;
	}
	if (metrics && !s->trusted) {
		http_error(s, 403, "Forbidden", "");
		return;
	}
	if (!metrics && !is_json(s->ops->field(s, F_TYPE))) {
		http_error(s, 415, "Unsupported Media Type", "");
		return;
	}
	struct mcp_req *req = req_new(mcp);
	if (!req) {
		http_error(s, 500, "Out of memory", "");
		return;
	}
	*req = (struct mcp_req){
		.step.fn = req_step,
		.timer.fn = req_timer,
		.reaper = req->reaper,
		.scan = req->scan,
		.answer = req->answer,
		.s = s,
		.mcp = mcp,
		.head = loop_clock(),
		.phase = metrics ? PHASE_METRICS : PHASE_BODY,
		.method = metrics ? METHOD_METRICS : METHOD_OTHER
	};
	s->user = req;
	/* A body of known length is taken in without growing. */
	if (!metrics && length && length != STREAM_OPEN)
		(void)buf_reserve(&req->body, length, length);
}

static void
mcp_on_body (struct stream *s,
             struct span    bytes,
             bool           last)
{
	struct mcp_req *req = s->user;
	if (req->phase == PHASE_METRICS) {
		/* A body to a GET says nothing. */
		if (last)
			loop_ready_bulk(req->mcp->loop, &req->step);
		return;
	}
	struct mcp_limits const *lim = &req->mcp->st->lim;
	size_t n = jb_scan(&req->scan, (char const *)bytes.ptr, bytes.len, lim->depth,
	                   lim->values, lim->members, MAX_SCALAR);
	if (n < bytes.len) {
		char c = (char)bytes.ptr[n];
		req_http_error(req, 400,
		               req->scan.state == JB_SCAN_SCALAR && req->scan.run == MAX_SCALAR
		               ? "Request JSON has too long a number"
		               : c == ':' ? "Request JSON object has too many members"
		               : (c == '{' || c == '[') && req->scan.depth == lim->depth
		               ? "Request JSON is nested too deeply"
		               : req->scan.values == lim->values
		               ? "Request JSON has too many values"
		               : "Out of memory");
		return;
	}
	struct span room = bytes.len ? buf_reserve(&req->body, bytes.len, SIZE_MAX)
	                             : (struct span){};
	if (bytes.len && !room.len) {
		req_http_error(req, 500, "Out of memory");
		return;
	}
	if (bytes.len) {
		memcpy(room.ptr, bytes.ptr, bytes.len);
		buf_commit(&req->body, bytes.len);
	}
	if (last) {
		req->phase = PHASE_PARSE;
		req_parse(req);
	}
}

/* The output has drained: send on. */
static void
mcp_on_writable (struct stream *s)
{
	struct mcp_req *req = s->user;
	req->blocked = false;
	loop_ready_bulk(req->mcp->loop, &req->step);
}

static void
mcp_on_cancel (struct stream *s)
{
	struct mcp_req *req = s->user;
	if (req->request)
		LOG(LOG_VERBOSE, "tool call cancelled after %.1f s", elapsed(&req->start));
	req->s = nullptr;
	req_finish(req);
}

struct stream_app const *
mcp_app (struct mcp *mcp)
{
	return &mcp->app;
}

/* Draining: end the tool calls under way with an error, as many a step as
 * its share of requests allows. */
static void
drain_calls (struct loop      *loop,
             struct loop_step *step)
{
	struct mcp *mcp = container_of(step, struct mcp, drain);
	for (; !list_is_empty(&mcp->calls); loop_work(loop, LOOP_REQS, 1)) {
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		struct mcp_req *req = container_of(mcp->calls.next, struct mcp_req, hook);
		list_del(&req->hook);
		req->hook = (struct list){};
		http_cancel(mcp->http, &req->request);
		loop_timer_stop(loop, &req->timer);
		reply_error(req, 503, INTERNAL_ERROR, "Server is shutting down", nullptr, 0);
	}
}

int
mcp_init (struct mcp               **p_mcp,
          struct loop               *loop,
          struct http               *http,
          struct mcp_static const   *st,
          struct metrics_src const  *metrics)
{
	struct mcp *mcp = malloc(sizeof *mcp);
	struct mcp_counts *counts = malloc(sizeof *counts);
	if (!mcp || !counts) {
		free(counts);
		free(mcp);
		return ENOMEM;
	}
	atomic_init(&counts->kept.spares, 0);
	atomic_init(&counts->kept.bytes[0], 0);
	atomic_init(&counts->kept.bytes[1], 0);
	for (size_t i = 0; i < METHOD_N; ++i)
		hist_init(&counts->methods[i]);
	*mcp = (struct mcp){
		.drain.fn = drain_calls,
		.app = {
			.fields = field_names,
			.n_fields = F_N,
			.lists = 1 << F_ACCEPT,
			.ctx = mcp,
			.on_head = mcp_on_head,
			.on_body = mcp_on_body,
			.on_writable = mcp_on_writable,
			.on_cancel = mcp_on_cancel
		},
		.pool = {
			.depth = st->lim.depth,
			.scalar = MAX_SCALAR,
			.flags = JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8
		},
		.answers = {
			.scalar = MAX_SCALAR,
			.flags = JSON_TOKENER_VALIDATE_UTF8
		},
		.metrics = *metrics,
		.loop = loop,
		.http = http,
		.st = st,
		.counts = counts
	};
	mcp->metrics.methods = counts->methods;
	mcp->metrics.kept = &counts->kept;
	list_init(&mcp->calls);
	list_init(&mcp->freeing);
	list_init(&mcp->spare);
	*p_mcp = mcp;
	return 0;
}

void
mcp_fini (struct mcp **p_mcp)
{
	if (!p_mcp || !*p_mcp)
		return;
	struct mcp *mcp = *p_mcp;
	*p_mcp = nullptr;

	/* The transport has cancelled every stream by now, so only requests
	 * still freeing their message are left. */
	while (!list_is_empty(&mcp->freeing))
		req_end(container_of(mcp->freeing.next, struct mcp_req, hook));
	while (mcp_relieve(mcp, BUF_CRITICAL, SIZE_MAX))
		;
	loop_unready(&mcp->drain);
	free(mcp->counts);
	free(mcp);
}

void
mcp_age (struct mcp *mcp)
{
	mcp->idle = mcp->low;
	mcp->low = mcp->spares;
}

size_t
mcp_relieve (struct mcp        *mcp,
             enum buf_pressure  level,
             size_t             max)
{
	/* A tokener keeps the buffer of the longest string it parsed, which
	 * may be mapped: at high pressure one longer than a slice goes, at
	 * critical any, each in a call of its own. */
	size_t keep = level >= BUF_CRITICAL ? 0 : LOOP_STEP_PARSE;
	size_t size = jb_pool_release(&mcp->pool, keep);
	if (size || (size = jb_pool_release(&mcp->answers, keep))) {
		atomic_store_explicit(&mcp->counts->kept.bytes[1], mcp->pool.bytes +
		                      mcp->answers.bytes, memory_order_relaxed);
		return buf_units(size);
	}
	size_t done = 0;
	for (size_t n = level >= BUF_CRITICAL ? mcp->spares
	              : mcp->low < mcp->idle ? mcp->low : mcp->idle; n; --n) {
		size_t units = buf_units(req_kept(container_of(mcp->spare.next,
		                                               struct mcp_req, hook)));
		if (done && done + units > max)
			break;
		req_free(mcp);
		done += units;
	}
	return done;
}

void
mcp_drain (struct mcp *mcp)
{
	mcp->draining = true;
	loop_ready_bulk(mcp->loop, &mcp->drain);
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

/* Serialize @p val into @p text, then free it; false if out of memory. */
static bool
serialize (struct dstr        *text,
           struct json_object *val)
{
	size_t len = 0;
	char const *json = val ? json_object_to_json_string_length(val, JB_WIRE, &len)
	                       : nullptr;
	bool ok = json && dstr_cat_n(text, json, len);
	json_object_put(val);
	return ok;
}

/* Make @p result a modern list result: cacheable, complete, with our _meta. */
static struct json_object *
modern_list (struct json_object *result,
             struct json_object *info)
{
	if (!jb_set(result, "ttlMs", json_object_new_int(CACHE_TTL_MS)) ||
	    !jb_set(result, "cacheScope", json_object_new_string("public")) ||
	    !jb_set(result, "resultType", json_object_new_string("complete")) ||
	    !jb_set(result, "_meta", jb_object(meta_server, json_object_get(info),
	                                       nullptr))) {
		json_object_put(result);
		return nullptr;
	}
	return result;
}

int
mcp_static_new (struct mcp_static        **p_static,
                struct ai_endpoint const  *ep,
                struct mcp_tool const     *tool,
                struct mcp_limits const   *limits)
{
	struct mcp_static *st = malloc(sizeof *st);
	if (!st)
		return ENOMEM;
	*st = (struct mcp_static){
		.ep = ep,
		.tool = tool->name,
		.list = { DSTR_INIT, DSTR_INIT },
		.discover = DSTR_INIT,
		.init_tail = DSTR_INIT,
		.modern_tail = DSTR_INIT,
		.versions = DSTR_INIT,
		.lim = *limits
	};
	*p_static = st;

	/* Built with json-c, which escapes as replies need, then kept as text. */
	struct json_object *info = jb_object("name", json_object_new_string("askai"),
	                                     "title", json_object_new_string("Ask AI"),
	                                     "version", json_object_new_string("0.1.0"),
	                                     nullptr);
	struct json_object *tools = jb_list(tool_definition(tool));
	struct json_object *list = json_object_new_array();
	bool ok = info && tools;
	for (size_t i = 0; ok && i < ARRAY_SIZE(versions); ++i)
		ok = jb_push(list, json_object_new_string(versions[i]));
	struct dstr info_text = DSTR_INIT;
	ok = ok && serialize(&info_text, json_object_get(info)) &&
	     serialize(&st->versions, json_object_get(list)) &&
	     serialize(&st->list[0], jb_object("tools", json_object_get(tools),
	                                       nullptr)) &&
	     serialize(&st->list[1], modern_list(jb_object("tools", json_object_get(tools),
	                                                   nullptr), info)) &&
	     serialize(&st->discover, modern_list(jb_object(
	             "supportedVersions", json_object_get(list),
	             "capabilities", jb_object("tools", jb_object(nullptr), nullptr),
	             nullptr), info)) &&
	     dstr_printf(&st->init_tail, "\",\"capabilities\":{\"tools\":{}},"
	                 "\"serverInfo\":%s}", dstr_get(&info_text)) &&
	     dstr_printf(&st->modern_tail, ",\"resultType\":\"complete\",\"_meta\":"
	                 "{\"%s\":%s}}", meta_server, dstr_get(&info_text));
	dstr_cleanup(&info_text);
	json_object_put(list);
	json_object_put(tools);
	json_object_put(info);
	if (!ok) {
		mcp_static_free(p_static);
		return ENOMEM;
	}
	return 0;
}

void
mcp_static_free (struct mcp_static **p_static)
{
	if (!p_static || !*p_static)
		return;
	struct mcp_static *st = *p_static;
	*p_static = nullptr;
	for (size_t i = 0; i < ARRAY_SIZE(st->list); ++i)
		dstr_cleanup(&st->list[i]);
	dstr_cleanup(&st->discover);
	dstr_cleanup(&st->init_tail);
	dstr_cleanup(&st->modern_tail);
	dstr_cleanup(&st->versions);
	free(st);
}
