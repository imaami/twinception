/** @file httpd.c
 *
 * HTTP/1.1 transport for request streams (src/stream.h), which carry
 * MCP's Streamable HTTP.
 *
 * Every path is the endpoint.  A connection serves one request at a time.
 * The application gets each as a stream, and its body as it arrives, from a
 * Content-Length or decoded from chunks; it answers with a Content-Length or,
 * for a body of unknown length such as an event stream, in chunks.  Closing
 * the connection while the application owes the response cancels the stream,
 * and so does shutting down only its sending side once the application waits
 * for something outside the loop.  CORS preflights, the Origin defence and the
 * policy are answered here, before the application sees a request.
 *
 * Requests are parsed in place in the connection's input buffer.  The head
 * stays at its start until the request is done, while the body is handed on
 * and cut out behind it.  Events and deadlines only queue a connection's step,
 * which reads once, does at most the step's share of requests, body bytes and
 * sends (loop.h), and sends once.  A connection with more requests buffered
 * queues its step again rather than reading, so a deep pipeline cannot hold
 * up other clients.  The output's storage goes back to the loop's stash
 * (src/buf.h) once sent, and the input's once the connection is idle, so an
 * idle connection holds none; under critical memory pressure, one that waits
 * keeps no more than its bytes need.
 *
 * Each connection has one timer, for the deadline that applies to it: idle
 * keep-alive, the head, the body's minimum rate, a write stall, or lingering.
 * Activity never moves the timer later; one that fires early re-arms for the
 * deadline as it then stands.  Bytes trickling in or out never refresh one.
 *
 * A connection ends by half-closing once its last response is sent, then
 * lingers, discarding what the client still sends, until the client closes
 * too or LINGER_S pass.  So a client that sends more than was read, such as a
 * body too large, still reads the response, and a reset, which on loopback
 * makes the resetting side free the other's queues, is rare.  A response that
 * ends before its request's body is all in ends the connection this way.
 *
 * A request's head says who its client is, by its bearer token, or by how it
 * connects if it shows none, and what it may do (src/policy.h); one that may
 * not connect is refused then, before its body.  Bodies are budgeted, for
 * all clients together and for each address, across every client loop: the
 * budgets are atomic counts, which a loop takes from with a compare-and-swap
 * and gives back to.
 *
 * Home's door accepts the clients, up to a step's share a turn, and places
 * each on the lighter of two client loops chosen at random, the weight being
 * the connections home placed on the loop and has not had back, those it
 * placed since it last worked out the loop's utilisation once more, and a
 * quarter of that utilisation in per mille, from the time its turns took.
 * Home hands the connection over in its own storage, which the loop sets up
 * in a step and gives back once the connection has closed; home keeps the
 * storage for the next (design section 5).  So home counts the connections
 * of each loop and each client address exactly, from placement to give-back,
 * those on their way included.
 *
 * Draining, home's door accepts no more clients, and a client loop closes
 * its idle connections, and any home still places, a share a step; the
 * others close once their response is sent, a response begun from then on
 * saying so, and all that are left at the drain's deadline close then, a
 * share a step too.  The loop exits once no client is left.
 *
 * Connections are limited by a cap per client address and by descriptors.
 * Clients may hold all but FD_RESERVE descriptors and the client loops' own,
 * each job in flight counting one more, as for its upstream connection.  A
 * new client or job beyond that, or a new client once descriptors run out
 * all the same, makes room: the client loop whose least recently active idle
 * connection is the least recently active of all closes it, as home asks.
 * With none idle, the client is turned away, after a spare descriptor has
 * let it be accepted, and home accepts no more until it has the spare again.
 */
#define _GNU_SOURCE /* accept4(), struct ucred */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "buf.h"
#include "cstr_view.h"
#include "hist.h"
#include "httpd.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "metrics.h"
#include "policy.h"
#include "shelf.h"
#include "stream.h"
#include "tls.h"
#include "util.h"

enum {
	READ_MIN       = 4 << 10,   /* free space a read asks for */
	CHUNK_LINE     = 1 << 10,   /* longest chunk-size line, extensions included */
	STEP_NS        = 100'000,   /* requests and accepts stop starting after
	                               this, within the step's units of work */
	SAMPLE_NS      = 1'000'000'000,  /* the least time over which home works
	                                    out a client loop's utilisation: a
	                                    request's millisecond is no load */
	FD_RESERVE     = 64,        /* descriptors kept from clients */
	TICK_S         = 5,         /* home's door's housekeeping */
	LINGER_S       = 2,         /* to wait for a client to close too */
	WRITE_PROGRESS = 16 << 10,  /* bytes sent that restart the write-stall clock */
	CLIENT_SLOTS   = 1 << 16,   /* client hash chains: never resized, as that
	                               would rehash every client in one step */
};

/* An event bit epoll never reports, for the timer finding a deadline passed. */
static constexpr uint32_t DEADLINE = 1u << 31;

/* And one that steps keep: TLS holds input that epoll no longer reports, the
 * rest of a record a read had no room for, or records a read that waited to
 * send left.
 */
static constexpr uint32_t HELD = 1u << 30;

/* The head starts the input buffer until the request is done, so header
 * values are kept as offsets into it, which survive the buffer moving; 0 is
 * absent, since the method is at 0.  Heads are at most 65535 bytes.
 */
struct httpd_field {
	uint16_t off;
	uint16_t len;
};

struct httpd_req {
	size_t             body;                /* Content-Length */
	uint32_t           head;                /* length of the request line and headers */
	struct httpd_field target;
	struct httpd_field host;
	struct httpd_field origin;
	struct httpd_field auth;
	struct httpd_field acrh;                /* Access-Control-Request-Headers */
	struct httpd_field app[STREAM_FIELDS];  /* those the application asked for */
	bool               length;              /* Content-Length seen */
	bool               chunked;             /* Transfer-Encoding: chunked */
	bool               http10;
	bool               close;
	bool               expect;              /* Expect: 100-continue */
	bool               pna;                 /* Access-Control-Request-Private-Network */
	bool               options;             /* the method is OPTIONS */
	bool               head_only;           /* the method is HEAD */
};

/* The connections from one client address: home's, which counts them, and
 * keeps the entry for reuse once none is left; the client loops count the
 * bytes of bodies they hold for it.
 */
struct httpd_client {
	struct list          spare;    /* on home's shelf, once unused */
	struct httpd_client *next;     /* in its hash chain */
	uint64_t             addr[2];
	size_t               conns;
	_Atomic size_t       held;     /* body bytes counted in hub->bodies; 0 once
	                                  unused */
};

/* Where a chunked body's decoding stands. */
enum httpd_chunk { CHUNK_SIZE, CHUNK_DATA, CHUNK_CRLF, CHUNK_TRAILER };

struct httpd_conn {
	struct msg           msg;       /* home's hand-over, then the give-back */
	struct list          hook;      /* on srv->idle, oldest first, or srv->busy;
	                                   or on home's shelf */
	struct loop_ref      ref;
	struct loop_step     step;
	struct loop_timer    timer;     /* for the deadline that applies */
	struct stream        stream;
	struct buf           in;
	struct buf           out;
	struct httpd        *srv;
	struct httpd_lane   *lane;      /* home sets it: the loop's */
	struct httpd_client *client;    /* home sets it */
	struct ssl_ctx_st   *ctx;       /* home sets it: what a TLS listener's
	                                   client shakes hands by, a reference
	                                   until the handshake is done */
	struct ssl_st       *ssl;       /* TLS's end, once the client begins */
	uint64_t             since;     /* when the request, the handshake, or the
	                                   idle spell began */
	uint64_t             stall;     /* when the output last stalled; 0: it flows */
	size_t               drained;   /* bytes sent since then */
	size_t               earlier;   /* of the output pending, the bytes of
	                                   responses before the one under way */
	size_t               held;      /* body bytes counted in the budgets */
	size_t               got;       /* body bytes handed on */
	size_t               chunk;     /* bytes left of the chunk under way */
	struct httpd_req     req;
	uint32_t            *groups;    /* home sets them: a Unix socket's user's
	                                   groups, sorted, in storage kept with
	                                   the connection's */
	uint32_t             n_groups;
	uint32_t             uid;       /* and the user */
	uint32_t             room;      /* groups the groups' storage holds */
	uint32_t             events;    /* reported since the last step, and DEADLINE */
	uint32_t             scan;      /* where to resume looking for the head's end */
	uint32_t             trailer;   /* trailer bytes so far */
	int                  fd;        /* home sets it; -1 once closed */
	uint16_t             trailers;  /* trailer fields so far */
	uint16_t             chunking;  /* enum httpd_chunk */
	bool                 parsed;
	bool                 body_in;   /* the body is all in */
	bool                 open;      /* the application has the stream */
	bool                 waits;     /* and waits for something outside the loop */
	bool                 replied;   /* the response head is queued */
	bool                 done;      /* the response is complete */
	bool                 chunks;    /* the response body goes in chunks */
	bool                 cont;      /* 100 Continue sent */
	bool                 closing;
	bool                 eof;
	bool                 doomed;    /* to close at its next step */
	bool                 linger;    /* half-closed, discarding input */
	bool                 writable;  /* to tell the application once output drains */
	bool                 local;     /* home sets it: the client is on loopback,
	                                   or a Unix socket */
	bool                 uds;       /* home sets it: over a Unix socket */
	bool                 idle;      /* on srv->idle */
};

/* A client loop, as its server and home's door share it. */
struct httpd_lane {
	struct msg               evict;     /* home's, to the loop: close idle
	                                       connections */
	struct msg               handover;  /* home's, to the loop: take the
	                                       policy */
	struct metrics_conns     metrics;   /* the loop's */
	struct loop             *loop;      /* the loop's server sets it, before
	                                       the door opens */
	struct httpd            *srv;       /* the loop's own */
	_Atomic(struct policy *) policy;    /* home stores, the loop takes: the
	                                       policy to serve by next */
	_Atomic uint64_t         idle;      /* the loop's: when its least recently
	                                       active idle connection began to
	                                       idle; UINT64_MAX: none is idle */
	uint64_t                 busy;      /* home's: the time the loop's turns
	                                       had taken when it sampled them */
	uint64_t                 sampled;   /* home's: when */
	uint64_t                 util;      /* home's: the loop's utilisation, per
	                                       mille, as last sampled */
	size_t                   conns;     /* home's: those it placed on the
	                                       loop, and has not had back */
	size_t                   placed;    /* home's: those it placed since it
	                                       sampled the loop's turns */
	_Atomic uint32_t         idles;     /* the loop's: its idle connections */
	_Atomic uint32_t         evicts;    /* home adds, the loop takes: idle
	                                       connections to close */
};

/* What home's door and the client loops' servers share (R4): made before
 * any loop starts, its atomic members initialized then.
 */
struct httpd_hub {
	_Atomic size_t     bodies;  /* body bytes held for all clients */
	struct loop       *home;    /* home's door sets it, before any client
	                               loop starts: connections go back there */
	size_t             n;
	struct httpd_lane  lane[];  /* the client loops' */
};

/* A client loop's server.  Each connection list runs from the least
 * recently active one.
 */
struct httpd {
	struct list              idle;        /* keep-alives between requests */
	struct list              busy;        /* the others */
	struct list              taken;       /* handed over, to set up */
	struct loop_step         take;        /* sets them up */
	struct loop_step         drain;       /* closes connections, draining */
	struct loop_step         evict;       /* closes idle connections, as home
	                                         asks */
	struct loop_timer        deadline;    /* the drain's: every connection
	                                         closes */
	struct httpd_limits      lim;
	struct loop             *loop;
	struct httpd_hub        *hub;
	struct httpd_lane       *lane;
	struct stream_app const *app;
	char const *const       *origins;
	struct policy           *policy;      /* a reference */
	uint64_t                 drain_by;    /* the drain's deadline */
	uint64_t                 idle_since;  /* as lane->idle last said */
	size_t                   in_max;      /* input a connection may buffer */
	size_t                   conns;       /* client connections */
	size_t                   idles;       /* of those, idle ones */
	size_t                   evicting;    /* idle connections to close */
	uint64_t                 draining;    /* bool, widened over the padding */
};

/* A socket home's door listens on. */
struct httpd_listener {
	struct loop_ref ref;
	char const     *path;   /* a Unix socket's file, which goes as the
	                           socket closes; nullptr: TCP */
	int             fd;
	uint16_t        ready;  /* bool: clients may wait */
	uint16_t        tls;    /* bool: clients shake hands (home->ctx) */
};

/* Home's door: the listeners, and the clients' addresses, connections and
 * descriptors.  Each connection is a client loop's from the moment it is
 * placed, until it is given back.
 */
struct httpd_home {
	struct shelf            spare;      /* connections kept for reuse */
	struct shelf            gone;       /* and addresses' entries */
	struct loop_step        accept;
	struct loop_timer       tick;       /* housekeeping */
	struct loop            *loop;
	struct httpd_hub       *hub;
	struct httpd_client   **clients;    /* CLIENT_SLOTS hash chains */
	_Atomic uint64_t const *calls;      /* jobs in flight */
	struct httpd_listener  *listeners;
	struct ssl_ctx_st      *ctx;        /* a reference: what TLS listeners'
	                                       clients shake hands by; nullptr:
	                                       there is none */
	size_t                  n_listeners;
	size_t                  turn;       /* the listener to accept from next */
	uint64_t                rng;        /* xorshift64 */
	uint64_t                seed;       /* of the hash of the clients, which
	                                       no client knows */
	size_t                  conns;      /* placed, and not given back */
	size_t                  fd_budget;  /* for conns + calls, from
	                                       RLIMIT_NOFILE */
	uint32_t                cap;        /* connections per client address;
	                                       0: none */
	uint32_t                refused;    /* turned away for descriptors since
	                                       the tick */
	uint32_t                capped;     /* turned away by the cap since then */
	uint32_t                open;       /* bool: accepting */
	uint32_t                anyone;     /* bool: clients that show no token
	                                       may call tools from beyond
	                                       loopback */
	int                     spare_fd;   /* given up to accept a client;
	                                       -1 while given up */
};

/* Home's door, for the connections given back. */
static thread_local struct httpd_home *door;

/* Decimal @p n written backwards from @p end; returns its first digit. */
static char *
httpd_decimal (char   *end,
               size_t  n)
{
	do *--end = (char)('0' + n % 10);
	while (n /= 10);
	return end;
}

/* Hexadecimal @p n written backwards from @p end; returns its first digit. */
static char *
httpd_hex (char   *end,
           size_t  n)
{
	do *--end = "0123456789abcdef"[n % 16];
	while (n /= 16);
	return end;
}

/* @p s seconds after @p since; never, if @p s is 0. */
static uint64_t
httpd_after (uint64_t since,
             uint32_t s)
{
	return s ? since + s * NS : UINT64_MAX;
}

static struct cstr_view
httpd_reason (unsigned status)
{
	switch (status) {
	case 200: return CSTR_VIEW("OK");
	case 202: return CSTR_VIEW("Accepted");
	case 204: return CSTR_VIEW("No Content");
	case 400: return CSTR_VIEW("Bad Request");
	case 401: return CSTR_VIEW("Unauthorized");
	case 403: return CSTR_VIEW("Forbidden");
	case 404: return CSTR_VIEW("Not Found");
	case 405: return CSTR_VIEW("Method Not Allowed");
	case 408: return CSTR_VIEW("Request Timeout");
	case 413: return CSTR_VIEW("Content Too Large");
	case 415: return CSTR_VIEW("Unsupported Media Type");
	case 417: return CSTR_VIEW("Expectation Failed");
	case 431: return CSTR_VIEW("Request Header Fields Too Large");
	case 500: return CSTR_VIEW("Internal Server Error");
	case 501: return CSTR_VIEW("Not Implemented");
	case 503: return CSTR_VIEW("Service Unavailable");
	case 505: return CSTR_VIEW("HTTP Version Not Supported");
	default:  return CSTR_VIEW("Unknown");
	}
}

static bool
httpd_tchar (unsigned char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
	       (c >= 'A' && c <= 'Z') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

/* Whether @p s is all token characters. */
static bool
httpd_token (char const *s)
{
	while (httpd_tchar((unsigned char)*s))
		++s;
	return !*s;
}

/* Whether @p c is a control character other than a tab. */
static bool
httpd_ctl (unsigned char c)
{
	return (c < ' ' && c != '\t') || c == 0x7f;
}

static bool
httpd_has_token (char const *list,
                 char const *token,
                 size_t      len)
{
	for (char const *p = list; *p; ) {
		p += strspn(p, " \t,");
		size_t n = strcspn(p, ",");
		size_t end = n;
		while (end && (p[end - 1] == ' ' || p[end - 1] == '\t'))
			--end;
		if (end == len && !strncasecmp(p, token, len))
			return true;
		p += n;
	}
	return false;
}

/* Whether @p host, a Host field or an origin's host and port, names this
 * machine by loopback, whatever the port.
 */
static bool
httpd_loopback_host (char const *host)
{
	static struct cstr_view const names[] = {
		CSTR_VIEW_INIT("localhost"), CSTR_VIEW_INIT("127.0.0.1"), CSTR_VIEW_INIT("[::1]")
	};
	for (size_t i = 0; i < ARRAY_SIZE(names); ++i) {
		if (strncasecmp(host, names[i].data, names[i].len))
			continue;
		char const *port = host + names[i].len;
		if (!*port)
			return true;
		size_t digits = *port == ':' ? strspn(port + 1, "0123456789") : 0;
		return digits && digits <= 5 && !port[1 + digits];
	}
	return false;
}

/* The header fields read here, but for those the application asks for. */
enum httpd_header {
	HEADER_HOST,
	HEADER_ORIGIN,
	HEADER_AUTHORIZATION,
	HEADER_ACRH,
	HEADER_CONTENT_LENGTH,
	HEADER_TRANSFER_ENCODING,
	HEADER_CONNECTION,
	HEADER_EXPECT,
	HEADER_PNA,
	HEADERS
};

static struct cstr_view const httpd_headers[HEADERS] = {
	[HEADER_HOST]              = CSTR_VIEW_INIT("host"),
	[HEADER_ORIGIN]            = CSTR_VIEW_INIT("origin"),
	[HEADER_AUTHORIZATION]     = CSTR_VIEW_INIT("authorization"),
	[HEADER_ACRH]              = CSTR_VIEW_INIT("access-control-request-headers"),
	[HEADER_CONTENT_LENGTH]    = CSTR_VIEW_INIT("content-length"),
	[HEADER_TRANSFER_ENCODING] = CSTR_VIEW_INIT("transfer-encoding"),
	[HEADER_CONNECTION]        = CSTR_VIEW_INIT("connection"),
	[HEADER_EXPECT]            = CSTR_VIEW_INIT("expect"),
	[HEADER_PNA]               = CSTR_VIEW_INIT("access-control-request-private-network"),
};

/* Which of httpd_headers @p name, of @p len bytes, is; HEADERS if none. */
static enum httpd_header
httpd_header (char const *name,
              size_t      len)
{
	size_t i = 0;
	while (i < HEADERS && (len != httpd_headers[i].len ||
	                       strncasecmp(name, httpd_headers[i].data, len)))
		++i;
	return (enum httpd_header)i;
}

/* Browsers on this machine. */
static bool
httpd_loopback_origin (char const *origin)
{
	static char const http[] = "http://";
	static char const https[] = "https://";
	size_t scheme = !strncmp(origin, http, sizeof http - 1) ? sizeof http - 1
	              : !strncmp(origin, https, sizeof https - 1) ? sizeof https - 1
	              : 0;
	return scheme && httpd_loopback_host(origin + scheme);
}

static bool
httpd_origin_allowed (struct httpd const *srv,
                      char const         *origin)
{
	if (httpd_loopback_origin(origin))
		return true;
	for (char const *const *o = srv->origins; o && *o; ++o)
		if (!strcmp(*o, "*") || !strcmp(*o, origin))
			return true;
	return false;
}

/* The length of the head at the input's start, which @p in holds: the
 * request line, the header lines and the empty line; 0 while it is partial.
 * Only its first lim.head bytes are looked at, and those once.
 */
static size_t
httpd_conn_head_length (struct httpd_conn *conn,
                        struct span        in)
{
	char const *data = (char const *)in.ptr;
	size_t len = in.len < conn->srv->lim.head ? in.len : conn->srv->lim.head;
	size_t i = conn->scan;
	for (char const *nl; i < len && (nl = memchr(data + i, '\n', len - i)); ) {
		i = (size_t)(nl - data) + 1;
		size_t cr = i < len && data[i] == '\r';
		if (i + cr == len) {
			/* The empty line may yet follow this one's end. */
			conn->scan = (uint32_t)(i - 1);
			return 0;
		}
		if (data[i + cr] == '\n')
			return i + cr + 1;
	}
	conn->scan = (uint32_t)len;
	return 0;
}

/* Cut the next line at its LF, dropping a preceding CR. */
static char *
httpd_next_line (char **cursor)
{
	char *line = *cursor;
	char *nl = strchr(line, '\n');
	*nl = '\0';
	if (nl > line && nl[-1] == '\r')
		nl[-1] = '\0';
	*cursor = nl + 1;
	return line;
}

/* Keep @p f in @p kept, unless a field came there before: a list's first
 * line stands for it, and any other may come once.
 */
static unsigned
httpd_field_keep (struct httpd_field *kept,
                  struct httpd_field  f,
                  bool                list)
{
	if (!kept->off) {
		*kept = f;
		return 0;
	}
	return list ? 0 : 400;
}

/* Keep field @p f, named @p name, if it is one that @p app asks for. */
static unsigned
httpd_req_app (struct httpd_req        *req,
               struct stream_app const *app,
               char const              *name,
               struct httpd_field       f)
{
	for (size_t i = 0; i < app->n_fields; ++i)
		if (!strcasecmp(name, app->fields[i]))
			return httpd_field_keep(&req->app[i], f, app->lists >> i & 1);
	return 0;
}

static unsigned
httpd_req_length (struct httpd_req *req,
                  char const       *value,
                  size_t            max_body)
{
	size_t n = strspn(value, "0123456789");
	if (!n || value[n] || req->length)
		return 400;
	/* Nineteen digits fit, so the check sees the true value. */
	if (n > 19 || (req->body = strtoull(value, nullptr, 10)) > max_body)
		return 413;
	req->length = true;
	return 0;
}

/* Record header field @p name, of @p name_len bytes, whose value is
 * @p value, @p f in the head, if it is one that matters here or to @p app;
 * nonzero is an HTTP error.  None of those read here is ever the
 * application's.
 */
static unsigned
httpd_req_header (struct httpd_req        *req,
                  struct stream_app const *app,
                  char const              *name,
                  size_t                   name_len,
                  char const              *value,
                  struct httpd_field       f,
                  size_t                   max_body)
{
	switch (httpd_header(name, name_len)) {
	case HEADER_HOST:
		return httpd_field_keep(&req->host, f, false);
	case HEADER_ORIGIN:
		return httpd_field_keep(&req->origin, f, false);
	case HEADER_AUTHORIZATION:
		return httpd_field_keep(&req->auth, f, false);
	case HEADER_ACRH:
		return httpd_field_keep(&req->acrh, f, false);
	case HEADER_CONTENT_LENGTH:
		return httpd_req_length(req, value, max_body);
	case HEADER_TRANSFER_ENCODING:
		/* Only chunked, the one coding HTTP/1.1 requires (RFC 9112). */
		if (req->chunked)
			return 400;
		if (strcasecmp(value, "chunked"))
			return 501;
		req->chunked = true;
		return 0;
	case HEADER_CONNECTION:
		if (httpd_has_token(value, "close", sizeof "close" - 1))
			req->close = true;
		else if (req->http10 && httpd_has_token(value, "keep-alive",
		                                        sizeof "keep-alive" - 1))
			req->close = false;
		return 0;
	case HEADER_EXPECT:
		if (strcasecmp(value, "100-continue"))
			return 417;
		req->expect = !req->http10;
		return 0;
	case HEADER_PNA:
		req->pna = !strcasecmp(value, "true");
		return 0;
	default:
		return httpd_req_app(req, app, name, f);
	}
}

/* A header line of the head at @p data: a token, a colon, and a value that
 * holds no control character but tabs, with white space around it.
 */
static unsigned
httpd_req_field (struct httpd_req        *req,
                 struct stream_app const *app,
                 char                    *line,
                 char const              *data,
                 size_t                   max_body)
{
	char *colon = strchr(line, ':');
	if (!colon || colon == line)
		return 400;
	*colon = '\0';
	if (!httpd_token(line))
		return 400;

	char *value = colon + 1 + strspn(colon + 1, " \t");
	char *end = value;
	for (; *end; ++end)
		if (httpd_ctl((unsigned char)*end))
			return 400;
	while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
	struct httpd_field const f = { (uint16_t)(value - data), (uint16_t)(end - value) };
	return httpd_req_header(req, app, line, (size_t)(colon - line), value, f,
	                        max_body);
}

/* The request line, which starts the head: the method stays there, as a
 * string.
 */
static unsigned
httpd_req_line (struct httpd_req *req,
                char             *line)
{
	for (char const *p = line; *p; ++p)
		if ((unsigned char)*p < ' ' || *p == 0x7f)
			return 400;
	char *target = strchr(line, ' ');
	if (!target || target == line)
		return 400;
	char *version = strchr(target + 1, ' ');
	if (!version || version == target + 1 || strchr(version + 1, ' '))
		return 400;
	req->target = (struct httpd_field){ (uint16_t)(target + 1 - line),
	                                    (uint16_t)(version - target - 1) };
	*target = '\0';
	*version++ = '\0';
	if (!httpd_token(line))
		return 400;
	req->http10 = !strcmp(version, "HTTP/1.0");
	if (!req->http10 && strcmp(version, "HTTP/1.1"))
		return strncmp(version, "HTTP/", sizeof "HTTP/" - 1) ? 400 : 505;
	req->close = req->http10;
	req->options = !strcmp(line, "OPTIONS");
	req->head_only = !strcmp(line, "HEAD");
	return 0;
}

/* Parse a complete head in place, leaving the method at its start as a
 * string; nonzero is an HTTP error.
 */
static unsigned
httpd_req_parse (struct httpd_req          *req,
                 char                      *data,
                 size_t                     len,
                 struct stream_app const   *app,
                 struct httpd_limits const *lim)
{
	*req = (struct httpd_req){ .head = (uint32_t)len };
	if (memchr(data, '\0', len))
		return 400;

	char *cursor = data;
	unsigned status = httpd_req_line(req, httpd_next_line(&cursor));
	if (status)
		return status;
	uint32_t fields = 0;
	for (char *line; *(line = httpd_next_line(&cursor)); ) {
		if (++fields > lim->fields)
			return 431;
		status = httpd_req_field(req, app, line, data, lim->body);
		if (status)
			return status;
	}

	/* Two framings would let a proxy and us see different requests, and
	 * an HTTP/1.0 sender of chunks frames them faultily (RFC 9112 6.1). */
	return req->chunked && (req->length || req->http10) ? 400 : 0;
}

/* The value of header @p f, empty with a null pointer if absent. */
static struct cstr_view
httpd_conn_field (struct httpd_conn const *conn,
                  struct httpd_field       f)
{
	char const *head = (char const *)buf_peek(&conn->in).ptr;
	return f.off ? (struct cstr_view){ head + f.off, f.len } : (struct cstr_view){};
}

/* The address a client is known by: IPv4 (as IPv4-mapped IPv6), or for other
 * IPv6 the /64 prefix, which is what one host is usually given.
 */
static void
httpd_addr_key (struct sockaddr_storage const *ss,
                uint64_t                       key[2])
{
	unsigned char a[16] = {};
	if (ss->ss_family == AF_INET) {
		struct sockaddr_in const *in = (void const *)ss;
		a[10] = a[11] = 0xff;
		memcpy(a + 12, &in->sin_addr, 4);
	} else if (ss->ss_family == AF_INET6) {
		struct sockaddr_in6 const *in6 = (void const *)ss;
		memcpy(a, &in6->sin6_addr, IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr) ? 16 : 8);
	}
	memcpy(key, a, sizeof a);
}

/* The link that holds client @p key in home's table, or where it would be
 * appended.
 */
static struct httpd_client **
httpd_home_client_slot (struct httpd_home *home,
                        uint64_t const     key[2])
{
	uint64_t h = util_hash(home->seed, key, 2 * sizeof *key);
	struct httpd_client **p = &home->clients[h & (CLIENT_SLOTS - 1)];
	while (*p && ((*p)->addr[0] != key[0] || (*p)->addr[1] != key[1]))
		p = &(*p)->next;
	return p;
}

/* Count a connection from client @p key, in an entry kept for reuse if
 * there is one; nullptr if out of memory.
 */
static struct httpd_client *
httpd_home_client_get (struct httpd_home *home,
                       uint64_t const     key[2])
{
	struct httpd_client **p = httpd_home_client_slot(home, key);
	struct httpd_client *client = *p;
	if (client) {
		++client->conns;
		return client;
	}
	struct list *spare = shelf_take(&home->gone);
	if (spare) {
		client = container_of(spare, struct httpd_client, spare);
	} else {
		client = malloc(sizeof *client);
		if (!client)
			return nullptr;
		atomic_init(&client->held, 0);
	}
	client->next = nullptr;
	client->addr[0] = key[0];
	client->addr[1] = key[1];
	client->conns = 1;
	*p = client;
	return client;
}

/* A connection of @p client's has been given back; after the last, as the
 * loop that closed it touches the entry no more, home keeps it for reuse.
 */
static void
httpd_home_client_put (struct httpd_home   *home,
                       struct httpd_client *client)
{
	if (--client->conns)
		return;
	*httpd_home_client_slot(home, client->addr) = client->next;
	shelf_put(&home->gone, &client->spare);
}

/* Whether @p ss, an IP address, is on loopback. */
static bool
httpd_addr_loopback (struct sockaddr_storage const *ss)
{
	struct sockaddr_in const *in = (void const *)ss;
	struct sockaddr_in6 const *in6 = (void const *)ss;
	struct in6_addr const *a = &in6->sin6_addr;
	switch (ss->ss_family) {
	case AF_INET:
		return ntohl(in->sin_addr.s_addr) >> 24 == 127;
	case AF_INET6:
		return IN6_IS_ADDR_LOOPBACK(a) ||
		       (IN6_IS_ADDR_V4MAPPED(a) && a->s6_addr[12] == 127);
	default:
		return false;
	}
}

/* The connection has closed, and its events are dispatched: give it back to
 * home, which counts it no more.
 */
static void
httpd_conn_release (struct loop_ref *ref)
{
	struct httpd_conn *conn = container_of(ref, struct httpd_conn, ref);
	loop_post(conn->srv->hub->home, &conn->msg);
}

/* Give back the body bytes the connection holds in the budgets. */
static void
httpd_conn_unhold (struct httpd_conn *conn)
{
	if (!conn->held)
		return;
	atomic_fetch_sub_explicit(&conn->srv->hub->bodies, conn->held,
	                          memory_order_relaxed);
	atomic_fetch_sub_explicit(&conn->client->held, conn->held, memory_order_relaxed);
	conn->held = 0;
}

/* Take @p n bytes from @p budget, of which @p max may be taken: 0, or
 * ENOBUFS if they do not fit.  Every client loop takes from the same.
 */
static int
httpd_budget_take (_Atomic size_t *budget,
                   size_t          n,
                   size_t          max)
{
	for (size_t used = atomic_load_explicit(budget, memory_order_relaxed);
	     n <= max - used; )
		if (atomic_compare_exchange_weak_explicit(budget, &used, used + n,
		                                          memory_order_relaxed,
		                                          memory_order_relaxed))
			return 0;
	return ENOBUFS;
}

/* Count @p n more body bytes in the budgets: 0, or ENOBUFS if they do not
 * fit.
 */
static int
httpd_conn_hold (struct httpd_conn *conn,
                 size_t             n)
{
	struct httpd *srv = conn->srv;
	if (!n)
		return 0;
	int e = httpd_budget_take(&srv->hub->bodies, n, srv->lim.bodies);
	if (e)
		return e;
	e = httpd_budget_take(&conn->client->held, n, srv->lim.client_bodies);
	if (e) {
		atomic_fetch_sub_explicit(&srv->hub->bodies, n, memory_order_relaxed);
		return e;
	}
	conn->held += n;
	return 0;
}

/* Take the stream back from the application, if it has it. */
static void
httpd_conn_cancel (struct httpd_conn *conn)
{
	if (!conn->open)
		return;
	conn->open = conn->waits = conn->writable = false;
	conn->srv->app->on_cancel(&conn->stream);
	conn->stream.user = nullptr;
}

/* Tell home how many connections idle, and when the least recently active
 * of them began to, as either changed.
 */
static void
httpd_show_idle (struct httpd *srv)
{
	uint64_t since = list_is_empty(&srv->idle) ? UINT64_MAX
	               : container_of(srv->idle.next, struct httpd_conn, hook)->since;
	if (since != srv->idle_since) {
		srv->idle_since = since;
		atomic_store_explicit(&srv->lane->idle, since, memory_order_relaxed);
	}
	atomic_store_explicit(&srv->lane->idles, (uint32_t)srv->idles,
	                      memory_order_relaxed);
}

static void
httpd_conn_close (struct httpd_conn *conn)
{
	int fd = conn->fd;
	if (fd < 0)
		return;
	conn->fd = -1;

	/* The last connection of a server that drains. */
	struct httpd *srv = conn->srv;
	loop_work(srv->loop, LOOP_CLOSES, 1);
	if (!--srv->conns && srv->draining)
		loop_exit(srv->loop);
	atomic_store_explicit(&srv->lane->metrics.open, srv->conns, memory_order_relaxed);
	httpd_conn_cancel(conn);
	httpd_conn_unhold(conn);
	list_del(&conn->hook);
	srv->idles -= conn->idle;
	httpd_show_idle(srv);
	loop_unready(&conn->step);
	loop_timer_stop(srv->loop, &conn->timer);
	loop_release(srv->loop, &conn->ref, httpd_conn_release);
	/* A handshake begun and not done has failed. */
	if (conn->ctx && conn->ssl)
		hist_add(&srv->lane->metrics.unshaken, 1);
	tls_ctx_put(conn->ctx);
	conn->ctx = nullptr;
	tls_destroy(&conn->ssl);
	close(fd);
	buf_fini(&conn->out);
	buf_fini(&conn->in);
}

/* File the connection at the young end of its list. */
static void
httpd_conn_park (struct httpd_conn *conn,
                 bool               idle)
{
	struct httpd *srv = conn->srv;
	list_del(&conn->hook);
	list_append(idle ? &srv->idle : &srv->busy, &conn->hook);
	srv->idles += idle - conn->idle;
	conn->idle = idle;
	httpd_show_idle(srv);
}

static size_t
httpd_frag_len (struct cstr_view const *f,
                size_t                  n)
{
	size_t len = 0;
	for (size_t i = 0; i < n; ++i)
		len += f[i].len;
	return len;
}

/* Whether the client's bytes wait, in the input or in TLS: a head begun. */
static bool
httpd_conn_input (struct httpd_conn const *conn)
{
	return buf_peek(&conn->in).len || (conn->ssl && tls_pending(conn->ssl));
}

/* Whether TLS waits for room to send what a read left it, such as the
 * answer to a key update: it reads nothing more meanwhile.
 */
static bool
httpd_conn_blocked (struct httpd_conn const *conn)
{
	return conn->ssl && tls_blocked(conn->ssl);
}

/* As recv(), or tls_recv() with TLS: the bytes, or a negative error. */
static ssize_t
httpd_conn_recv (struct httpd_conn *conn,
                 void              *buf,
                 size_t             len)
{
	if (conn->ssl)
		return tls_recv(conn->ssl, buf, len);
	ssize_t n = recv(conn->fd, buf, len, 0);
	return n < 0 ? -errno : n;
}

/* As send(), or tls_send() with TLS: the bytes, or a negative error. */
static ssize_t
httpd_conn_send (struct httpd_conn *conn,
                 void const        *buf,
                 size_t             len)
{
	if (conn->ssl)
		return tls_send(conn->ssl, buf, len);
	ssize_t n = send(conn->fd, buf, len, MSG_NOSIGNAL);
	return n < 0 ? -errno : n;
}

/* Queue the connection's step: a bulk one while it shakes hands. */
static void
httpd_conn_ready (struct httpd_conn *conn)
{
	if (conn->ctx)
		loop_ready_bulk(conn->srv->loop, &conn->step);
	else
		loop_ready(conn->srv->loop, &conn->step);
}

/* Close the connection at its next step: output must not close it at once,
 * as an application may be writing it.
 */
static void
httpd_conn_doom (struct httpd_conn *conn)
{
	conn->doomed = true;
	httpd_conn_ready(conn);
}

/* Queue @p n fragments: 0, or EPIPE once the connection is doomed, or the
 * error that dooms it, out of memory.
 */
static int
httpd_conn_put (struct httpd_conn      *conn,
                struct cstr_view const *f,
                size_t                  n)
{
	if (conn->doomed)
		return EPIPE;
	size_t total = httpd_frag_len(f, n);
	if (!total)
		return 0;
	struct span s;
	int e = buf_reserve(&conn->out, total, SIZE_MAX, &s);
	if (e) {
		httpd_conn_doom(conn);
		return e;
	}
	for (size_t i = 0; i < n; ++i) {
		if (f[i].len)
			memcpy(s.ptr, f[i].data, f[i].len);
		s.ptr += f[i].len;
	}
	buf_commit(&conn->out, total);
	return 0;
}

/* Queue a response head: the status line, our headers, up to 9 @p extra
 * header lines and the framing of a body of @p length bytes, or of a body of
 * STREAM_OPEN, which to HTTP/1.1 clients goes in chunks and to others ends
 * with the connection.  A response that begins before its request's body is
 * all in, or to a request that asked so, or while the server drains, ends the
 * connection.  Returns as httpd_conn_put() does.
 */
static int
httpd_conn_head (struct httpd_conn      *conn,
                 unsigned                status,
                 struct cstr_view const *extra,
                 size_t                  n_extra,
                 size_t                  length)
{
	struct httpd_req const *req = &conn->req;
	char digits[20];
	struct cstr_view framing[3] = {};
	if (length != STREAM_OPEN) {
		/* A 204 has no body to give the length of. */
		if (status != 204) {
			char const *len = httpd_decimal(digits + sizeof digits, length);
			framing[0] = CSTR_VIEW("Content-Length: ");
			framing[1] = (struct cstr_view){ len, (size_t)(digits + sizeof digits - len) };
			framing[2] = CSTR_VIEW("\r\n");
		}
	} else if (req->http10) {
		conn->closing = true;
	} else {
		conn->chunks = true;
		framing[0] = CSTR_VIEW("Transfer-Encoding: chunked\r\n");
	}
	conn->closing |= req->close || conn->srv->draining || !conn->body_in;
	conn->replied = true;
	conn->earlier = buf_peek(&conn->out).len;

	char code[8];
	code[7] = ' ';
	char const *c = httpd_decimal(code + 7, status);
	struct cstr_view const origin = httpd_conn_field(conn, req->origin);
	struct cstr_view f[12 + 9];
	size_t n = 0;
	f[n++] = CSTR_VIEW("HTTP/1.1 ");
	f[n++] = (struct cstr_view){ c, (size_t)(code + 8 - c) };
	f[n++] = httpd_reason(status);
	f[n++] = CSTR_VIEW("\r\n");
	if (conn->closing)
		f[n++] = CSTR_VIEW("Connection: close\r\n");
	for (size_t i = 0; i < ARRAY_SIZE(framing); ++i)
		f[n++] = framing[i];
	if (origin.data) {
		f[n++] = CSTR_VIEW("Access-Control-Allow-Origin: ");
		f[n++] = origin;
		f[n++] = CSTR_VIEW("\r\nAccess-Control-Allow-Credentials: true\r\n"
		                   "Access-Control-Expose-Headers: Mcp-Session-Id, "
		                   "WWW-Authenticate\r\n"
		                   "Vary: Origin\r\n");
	}
	for (size_t i = 0; i < n_extra; ++i)
		f[n++] = extra[i];
	f[n++] = CSTR_VIEW("\r\n");
	return httpd_conn_put(conn, f, n);
}

/* Queue a complete response of our own: @p body, which is nullptr for none
 * (and no Content-Type), after @p extra header lines; at most 8 of each.  A
 * failure dooms the connection.
 */
static void
httpd_conn_respond (struct httpd_conn      *conn,
                    unsigned                status,
                    struct cstr_view const *extra,
                    size_t                  n_extra,
                    struct cstr_view const *body,
                    size_t                  n_body)
{
	struct cstr_view head[9];
	size_t n = 0;
	if (body)
		head[n++] = CSTR_VIEW("Content-Type: application/json\r\n");
	for (size_t i = 0; i < n_extra; ++i)
		head[n++] = extra[i];
	if (!httpd_conn_head(conn, status, head, n, httpd_frag_len(body, n_body)))
		(void)httpd_conn_put(conn, body, n_body);
	conn->done = true;
}

/* A JSON-RPC error saying the reason phrase. */
static void
httpd_conn_error (struct httpd_conn      *conn,
                  unsigned                status,
                  struct cstr_view const *extra,
                  size_t                  n_extra)
{
	struct cstr_view const body[] = {
		CSTR_VIEW("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\""),
		httpd_reason(status),
		CSTR_VIEW("\"}}"),
	};
	httpd_conn_respond(conn, status, extra, n_extra, body, ARRAY_SIZE(body));
}

/* Fail the request under way with @p status, taking the stream back from the
 * application; with a response begun already, the connection just closes.
 */
static void
httpd_conn_fail (struct httpd_conn *conn,
                 unsigned           status)
{
	httpd_conn_cancel(conn);
	conn->closing = true;
	if (conn->replied)
		httpd_conn_doom(conn);
	else
		httpd_conn_error(conn, status, nullptr, 0);
}

static void
httpd_conn_preflight (struct httpd_conn *conn)
{
	struct httpd_req const *req = &conn->req;
	if (!req->origin.off) {
		httpd_conn_respond(conn, 204, &CSTR_VIEW("Allow: POST, OPTIONS\r\n"), 1, nullptr, 0);
		return;
	}

	struct cstr_view const acrh = httpd_conn_field(conn, req->acrh);
	struct cstr_view const extra[] = {
		CSTR_VIEW("Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n"
		          "Access-Control-Allow-Headers: "),
		acrh.data ? acrh : CSTR_VIEW("Content-Type, Authorization"),
		CSTR_VIEW("\r\nAccess-Control-Max-Age: 600\r\n"),
		req->pna ? CSTR_VIEW("Access-Control-Allow-Private-Network: true\r\n")
		         : CSTR_VIEW(""),
		CSTR_VIEW("Vary: Access-Control-Request-Headers\r\n"),
	};
	httpd_conn_respond(conn, 204, extra, ARRAY_SIZE(extra), nullptr, 0);
}

static int
httpd_stream_head (struct stream *s,
                   uint16_t       status,
                   struct span    fields,
                   size_t         length)
{
	struct httpd_conn *conn = container_of(s, struct httpd_conn, stream);
	struct cstr_view const extra = { (char const *)fields.ptr, fields.len };
	if (!conn->open || conn->replied || httpd_conn_head(conn, status, &extra, 1, length))
		return EPIPE;
	loop_ready(conn->srv->loop, &conn->step);
	return 0;
}

/*
 * Bytes count as sent work where they are queued, as that copies them.  They
 * may take the output pending past lim.out_max, at the cost of the stream and
 * the connection, only while the response has it all to itself and has not
 * been asked to wait: the write deadline then guards a client that does not
 * read, while one that lets earlier responses pile up is cut off.
 */
static int
httpd_stream_data (struct stream *s,
                   struct span    bytes)
{
	struct httpd_conn *conn = container_of(s, struct httpd_conn, stream);
	struct httpd *srv = conn->srv;
	if (!conn->open || !conn->replied)
		return EPIPE;
	if (!bytes.len)
		return 0;
	if ((conn->earlier || conn->writable) &&
	    buf_peek(&conn->out).len + bytes.len > srv->lim.out_max) {
		httpd_conn_doom(conn);
		return EPIPE;
	}

	/* In chunks, each is its size in hexadecimal, a CRLF, its bytes and
	 * another. */
	char digits[20];
	struct cstr_view f[] = { {}, { (char const *)bytes.ptr, bytes.len }, {} };
	if (conn->chunks) {
		char const *size = httpd_hex(digits + sizeof digits - 2, bytes.len);
		digits[sizeof digits - 2] = '\r';
		digits[sizeof digits - 1] = '\n';
		f[0] = (struct cstr_view){ size, (size_t)(digits + sizeof digits - size) };
		f[2] = CSTR_VIEW("\r\n");
	}
	if (httpd_conn_put(conn, f, ARRAY_SIZE(f)))
		return EPIPE;
	loop_work(srv->loop, LOOP_SEND, bytes.len);
	loop_ready(srv->loop, &conn->step);
	if (buf_peek(&conn->out).len < srv->lim.out_pause)
		return 0;
	conn->writable = true;
	return EAGAIN;
}

static void
httpd_stream_end (struct stream *s,
                  int            error)
{
	struct httpd_conn *conn = container_of(s, struct httpd_conn, stream);
	if (!conn->open)
		return;
	conn->open = conn->waits = conn->writable = false;
	conn->stream.user = nullptr;
	if (!conn->replied) {
		httpd_conn_error(conn, 500, nullptr, 0);
	} else if (error) {
		/* The client sees the body cut short as the connection closes. */
		conn->closing = true;
		httpd_conn_doom(conn);
	} else if (conn->chunks) {
		(void)httpd_conn_put(conn, &CSTR_VIEW("0\r\n\r\n"), 1);
	}
	conn->done = true;
	loop_ready(conn->srv->loop, &conn->step);
}

static void
httpd_stream_waiting (struct stream *s)
{
	struct httpd_conn *conn = container_of(s, struct httpd_conn, stream);
	conn->waits = conn->open;
	loop_ready(conn->srv->loop, &conn->step);
}

static struct span
httpd_stream_field (struct stream const *s,
                    size_t               i)
{
	struct httpd_conn const *conn = container_of(s, struct httpd_conn const, stream);
	struct cstr_view f = httpd_conn_field(conn, conn->req.app[i]);
	return (struct span){ (unsigned char *)(uintptr_t)f.data, f.len };
}

static struct stream_ops const httpd_stream_ops = {
	.head = httpd_stream_head,
	.data = httpd_stream_data,
	.end = httpd_stream_end,
	.waiting = httpd_stream_waiting,
	.field = httpd_stream_field,
};

/* The token of Authorization field @p auth, if it shows the bearer scheme;
 * else none, a null pointer.  Credentials of another scheme are none.
 */
static struct cstr_view
httpd_bearer (struct cstr_view auth)
{
	static char const scheme[] = "Bearer";
	size_t n = sizeof scheme - 1;
	if (auth.len < n || strncasecmp(auth.data, scheme, n))
		return (struct cstr_view){};
	if (auth.len == n)
		return (struct cstr_view){ auth.data + n, 0 };
	if (auth.data[n] != ' ')
		return (struct cstr_view){};
	return (struct cstr_view){ auth.data + n + 1, auth.len - n - 1 };
}

/* Who the client of a freshly parsed head is, by its bearer token, or by
 * how it connects if it shows none, and what it may do, into its stream;
 * whether it may connect (src/policy.h).  A client on loopback that names
 * another host may be a page whose name was rebound to 127.0.0.1, whose GET
 * carries no Origin: it connects as a remote one.
 */
static bool
httpd_conn_grant (struct httpd_conn *conn)
{
	struct cstr_view const token = httpd_bearer(httpd_conn_field(conn, conn->req.auth));
	char const *host = httpd_conn_field(conn, conn->req.host).data;
	enum policy_kind const kind = conn->uds ? POLICY_UNIX
	                            : conn->local && (!host || httpd_loopback_host(host))
	                            ? POLICY_LOOPBACK : POLICY_REMOTE;
	struct policy_peer const peer = {
		.groups = conn->groups,
		.n_groups = conn->n_groups,
		.uid = conn->uid
	};
	struct policy_grant g = {};
	bool ok = policy_grant(conn->srv->policy, token.data, token.len, kind, &peer, &g);
	conn->stream.principal = g.principal;
	conn->stream.tools = g.tools;
	conn->stream.upstream = g.upstream;
	conn->stream.who = g.who;
	conn->stream.trusted = g.metrics;
	return ok;
}

/* Parse the head of @p len bytes at @p data, the input's start, and check it
 * against the Origin defence, the policy and the body budgets, and count its
 * body in those; nonzero is an HTTP error.  So a client that may not send a
 * body holds none of the budgets.  A chunked body is counted as it comes.
 */
static unsigned
httpd_conn_admit (struct httpd_conn *conn,
                  char              *data,
                  size_t             len)
{
	struct httpd *srv = conn->srv;
	struct httpd_req *req = &conn->req;
	unsigned status = httpd_req_parse(req, data, len, srv->app, &srv->lim);
	if (status)
		return status;
	char const *origin = httpd_conn_field(conn, req->origin).data;
	if (origin && !httpd_origin_allowed(srv, origin))
		return 403; /* DNS rebinding defence */
	if (!req->options && !httpd_conn_grant(conn))
		return 401;
	return httpd_conn_hold(conn, req->body) ? 503 : 0;
}

/* A chunked body as it is decoded in place: the @p len bytes at @p raw are
 * read from @p rd on, and their data moved down to @p wr.
 */
struct httpd_dechunk {
	unsigned char *raw;
	size_t         len;
	size_t         rd;
	size_t         wr;
};

/* Take the line @p d reads next, which must end in CRLF within @p max bytes,
 * into @p *line, less its CRLF: 0, with @p *line empty and a null pointer
 * while the line is not all in, or an HTTP error, @p too_long if no LF comes
 * within @p max bytes, 400 if no CR precedes it, again with @p *line empty.
 */
static unsigned
httpd_dechunk_line (struct httpd_dechunk *d,
                    size_t                max,
                    unsigned              too_long,
                    struct span          *line)
{
	unsigned char *start = d->raw + d->rd;
	size_t avail = d->len - d->rd;
	unsigned char *nl = memchr(start, '\n', avail < max ? avail : max);
	*line = (struct span){};
	if (!nl)
		return avail < max ? 0 : too_long;
	d->rd += (size_t)(nl - start) + 1;
	if (nl == start || nl[-1] != '\r')
		return 400;
	*line = (struct span){ start, (size_t)(nl - start) - 1 };
	return 0;
}

/* A chunk-size line: the size, in hexadecimal, and any extensions, which
 * follow it after optional white space; only the line's end may hold a
 * control character.
 */
static unsigned
httpd_conn_chunk_size (struct httpd_conn    *conn,
                       struct httpd_dechunk *d)
{
	struct span s;
	unsigned status = httpd_dechunk_line(d, CHUNK_LINE, 400, &s);
	if (!s.ptr)
		return status;
	unsigned char const *line = s.ptr;
	size_t n = s.len;

	size_t size = 0;
	size_t i = 0;
	bool over = false;
	for (unsigned v; i < n && (v = util_hex_digit(line[i])) < 16; ++i) {
		over |= size > SIZE_MAX >> 4;
		size = size << 4 | v;
	}
	size_t ext = i + strspn((char const *)line + i, " \t");
	if (!i || over || (ext < n && line[ext] != ';'))
		return 400;
	for (size_t k = ext; k < n; ++k)
		if (httpd_ctl(line[k]))
			return 400;
	if (size > conn->srv->lim.body - conn->got - d->wr)
		return 413;
	conn->chunk = size;
	conn->chunking = size ? CHUNK_DATA : CHUNK_TRAILER;
	return 0;
}

/* As much of a chunk's data as is in. */
static void
httpd_conn_chunk_data (struct httpd_conn    *conn,
                       struct httpd_dechunk *d)
{
	size_t n = d->len - d->rd < conn->chunk ? d->len - d->rd : conn->chunk;
	memmove(d->raw + d->wr, d->raw + d->rd, n);
	d->wr += n;
	d->rd += n;
	conn->chunk -= n;
	if (!conn->chunk)
		conn->chunking = CHUNK_CRLF;
}

/* The CRLF that ends a chunk's data. */
static unsigned
httpd_conn_chunk_end (struct httpd_conn    *conn,
                      struct httpd_dechunk *d)
{
	unsigned char const *crlf = d->raw + d->rd;
	if (d->len - d->rd < 2)
		return 0;
	d->rd += 2;
	conn->chunking = CHUNK_SIZE;
	return crlf[0] == '\r' && crlf[1] == '\n' ? 0 : 400;
}

/* A line of the trailer section, which counts against the head's limits;
 * each but the empty line that ends it must be a field: a request that
 * follows a missing empty line is no trailer.
 */
static unsigned
httpd_conn_trailer (struct httpd_conn    *conn,
                    struct httpd_dechunk *d)
{
	struct httpd_limits const *lim = &conn->srv->lim;
	struct span line;
	unsigned status = httpd_dechunk_line(d, lim->head, 431, &line);
	if (!line.ptr)
		return status;

	size_t name = 0;
	while (name < line.len && httpd_tchar(line.ptr[name]))
		++name;
	conn->trailer += (uint32_t)line.len + 2;
	if (!line.len)
		conn->body_in = true;
	else if (!name || line.ptr[name] != ':')
		return 400;
	else
		++conn->trailers;
	return conn->trailer > lim->head || conn->trailers > lim->fields ? 431 : 0;
}

/*
 * Decode chunks (RFC 9112 7.1) from the @p len bytes at @p raw that follow
 * the head, moving their data down to @p raw, and count the data in the
 * budgets.  Sets @p *used to the bytes taken and @p *out to the data bytes
 * now at @p raw, and conn->body_in once the last chunk and the trailer
 * section are in; returns 0 or an HTTP error.  A chunk's data is taken as
 * it comes, and a line or the CRLF after the data once it is all in.  Lines
 * end in CRLF only, as a lenient reading of framing is what request
 * smuggling exploits.  Chunk extensions and trailer fields are ignored.
 */
static unsigned
httpd_conn_dechunk (struct httpd_conn *conn,
                    unsigned char     *raw,
                    size_t             len,
                    size_t            *used,
                    size_t            *out)
{
	struct httpd_dechunk d = { raw, len, 0, 0 };
	for (size_t was = SIZE_MAX; d.rd != was && d.rd < len && !conn->body_in; ) {
		was = d.rd;
		unsigned status;
		switch (conn->chunking) {
		case CHUNK_SIZE:
			status = httpd_conn_chunk_size(conn, &d);
			break;
		case CHUNK_DATA:
			httpd_conn_chunk_data(conn, &d);
			continue;
		case CHUNK_CRLF:
			status = httpd_conn_chunk_end(conn, &d);
			break;
		default:
			status = httpd_conn_trailer(conn, &d);
		}
		if (status)
			return status;
	}
	*used = d.rd;
	*out = d.wr;
	return httpd_conn_hold(conn, d.wr) ? 503 : 0;
}

/* What the connection's step does next. */
enum httpd_next {
	NEXT_MORE,   /* go on */
	NEXT_INPUT,  /* wait for input */
	NEXT_YIELD,  /* wait for the next step */
	NEXT_WAIT,   /* wait for the application */
	NEXT_END,    /* no more requests */
};

/* Take in what has come of the body, within the step's share: hand it to the
 * application, if it has the stream, and cut it out of the input.  Then wait
 * for more, for 100 Continue first if the client asked so, or for the next
 * step if the share ran out first.
 */
static enum httpd_next
httpd_conn_body (struct httpd_conn *conn)
{
	struct loop *loop = conn->srv->loop;
	struct httpd_req const *req = &conn->req;
	struct span in = buf_peek(&conn->in);
	unsigned char *raw = in.ptr + req->head;
	size_t avail = in.len - req->head;
	size_t room = loop_room(loop, LOOP_BODY);
	size_t len = avail < room ? avail : room;
	size_t used;
	size_t out;
	if (req->chunked) {
		unsigned status = httpd_conn_dechunk(conn, raw, len, &used, &out);
		if (status) {
			httpd_conn_fail(conn, status);
			return NEXT_END;
		}
	} else {
		size_t left = req->body - conn->got;
		used = out = len < left ? len : left;
		conn->body_in = out == left;
	}
	loop_work(loop, LOOP_BODY, used);
	conn->got += out;
	if (conn->open && (out || conn->body_in))
		conn->srv->app->on_body(&conn->stream, (struct span){ raw, out },
		                        conn->body_in);
	buf_cut(&conn->in, req->head, used);
	if (conn->body_in || conn->done)
		return NEXT_MORE;
	if (len < avail)
		return NEXT_YIELD;
	if (req->expect && !conn->cont) {
		(void)httpd_conn_put(conn, &CSTR_VIEW("HTTP/1.1 100 Continue\r\n\r\n"), 1);
		conn->cont = true;
	}
	return NEXT_INPUT;
}

/* Begin a request: CORS preflights are ours, the rest the application's. */
static void
httpd_conn_begin (struct httpd_conn *conn)
{
	struct httpd_req const *req = &conn->req;
	size_t length = req->chunked ? STREAM_OPEN : req->body;
	conn->body_in = !length;
	if (req->options) {
		httpd_conn_preflight(conn);
		return;
	}

	/* The application's bodies would trail a HEAD response; end the stream. */
	conn->closing |= req->head_only;
	conn->open = true;
	conn->stream.user = nullptr;
	struct stream_app const *app = conn->srv->app;
	unsigned char *head = buf_peek(&conn->in).ptr;
	app->on_head(app->ctx, &conn->stream,
	             (struct span){ head, (size_t)req->target.off - 1 },
	             (struct span){ head + req->target.off, req->target.len }, length);
	/* The text is in the policy, which a reload can free (stream.h). */
	conn->stream.who = (struct tool_text){};
	if (conn->open && conn->body_in)
		app->on_body(&conn->stream, (struct span){}, true);
}

/* The request is done, and its body all in: make way for the next. */
static void
httpd_conn_reset (struct httpd_conn *conn)
{
	buf_consume(&conn->in, conn->req.head);
	httpd_conn_unhold(conn);
	conn->req = (struct httpd_req){};
	conn->since = conn->srv->loop->now;
	conn->got = conn->chunk = conn->scan = conn->trailer = conn->trailers = 0;
	conn->chunking = CHUNK_SIZE;
	conn->parsed = conn->body_in = conn->replied = conn->done = false;
	conn->chunks = conn->cont = false;
}

/* Answer a head that came to nothing with @p status, and end the connection.
 * Only a 401 echoes the Origin, which passed the check by then; the rest
 * echo none, a 503 of the body budgets, which follows the check too,
 * included.
 */
static enum httpd_next
httpd_conn_refuse (struct httpd_conn *conn,
                   unsigned           status)
{
	loop_work(conn->srv->loop, LOOP_REQS, 1);
	conn->closing = true;
	switch (status) {
	case 401:
		httpd_conn_error(conn, status, &CSTR_VIEW("WWW-Authenticate: Bearer\r\n"), 1);
		break;
	case 503:
		conn->req = (struct httpd_req){};
		httpd_conn_error(conn, status, &CSTR_VIEW("Retry-After: 1\r\n"), 1);
		break;
	default:
		conn->req = (struct httpd_req){};
		httpd_conn_error(conn, status, nullptr, 0);
	}
	return NEXT_END;
}

/* Begin the next request, once its head is in, unless the connection ends,
 * or the step has no share left for that or has run to @p end.
 */
static enum httpd_next
httpd_conn_parse (struct httpd_conn *conn,
                  uint64_t           end)
{
	struct httpd *srv = conn->srv;
	struct loop *loop = srv->loop;
	if (conn->closing || srv->draining)
		return NEXT_END;
	if (!loop_room(loop, LOOP_REQS) ||
	    (loop->work[LOOP_REQS] && loop_clock() >= end))
		return NEXT_YIELD;

	/* A head that does not end within lim.head bytes is too large. */
	struct span in = buf_peek(&conn->in);
	size_t head = httpd_conn_head_length(conn, in);
	if (!head)
		return in.len < srv->lim.head ? NEXT_INPUT : httpd_conn_refuse(conn, 431);
	unsigned status = httpd_conn_admit(conn, (char *)in.ptr, head);
	if (status)
		return httpd_conn_refuse(conn, status);
	loop_work(loop, LOOP_REQS, 1);
	conn->parsed = true;
	conn->since = loop->now;
	httpd_conn_begin(conn);
	return NEXT_MORE;
}

/* Advance the request under way, or begin the next unless the step has no
 * share left for that or has run to @p end.
 */
static enum httpd_next
httpd_conn_next (struct httpd_conn *conn,
                 uint64_t           end)
{
	if (conn->done) {
		if (!conn->body_in)
			return NEXT_END;
		httpd_conn_reset(conn);
	}
	if (!conn->parsed)
		return httpd_conn_parse(conn, end);
	return conn->body_in ? NEXT_WAIT : httpd_conn_body(conn);
}

/* The output is all sent: its storage goes back, and TLS sends what a read
 * left it to send, such as the answer to a key update, whose wait for room
 * stalls as the output's does.  With nothing left, the request's own clock
 * restarts, as it was held up meanwhile.  Returns 0, or the error that closed
 * the connection.
 */
static int
httpd_conn_sent (struct httpd_conn *conn)
{
	uint64_t now = conn->srv->loop->now;
	buf_fini(&conn->out);
	enum tls_shake shake = httpd_conn_blocked(conn) ? tls_shake(conn->ssl)
	                                                : TLS_SHAKEN;
	if (shake == TLS_FAILED) {
		httpd_conn_close(conn);
		return EPROTO;
	}
	if (shake == TLS_WRITE) {
		if (!conn->stall)
			conn->stall = now;
		return 0;
	}
	if (conn->stall)
		conn->since = now;
	conn->stall = 0;
	return 0;
}

/* A send would block: the write-stall clock starts, and restarts only once
 * WRITE_PROGRESS bytes have gone out, so a trickle cannot hold it.
 */
static void
httpd_conn_stall (struct httpd_conn *conn)
{
	if (conn->stall && conn->drained < WRITE_PROGRESS)
		return;
	conn->stall = conn->srv->loop->now;
	conn->drained = 0;
}

/* Send what is queued, within the step's share: 0, or the error that closed
 * the connection.  TLS sends a record a call, whole, of TLS_RECORD bytes or
 * the output's last, so that a call after one that waited is never shorter,
 * and none sends records unseen.
 */
static int
httpd_conn_flush (struct httpd_conn *conn)
{
	struct loop *loop = conn->srv->loop;
	for (;;) {
		struct span out = buf_peek(&conn->out);
		if (!out.len)
			return httpd_conn_sent(conn);
		size_t room = loop_room(loop, LOOP_SEND);
		size_t max = conn->ssl ? TLS_RECORD : room;
		size_t len = out.len < max ? out.len : max;
		if (!room || len > room)
			return 0;
		ssize_t n = httpd_conn_send(conn, out.ptr, len);
		if (n >= 0) {
			buf_consume(&conn->out, (size_t)n);
			conn->drained += (size_t)n;
			conn->earlier -= conn->earlier < (size_t)n ? conn->earlier : (size_t)n;
			loop_work(loop, LOOP_SEND, (uint64_t)n);
			continue;
		}
		if (n == -EINTR)
			continue;
		if (n == -EAGAIN || n == -EWOULDBLOCK) {
			httpd_conn_stall(conn);
			return 0;
		}
		httpd_conn_close(conn);
		return (int)-n;
	}
}

/*
 * When the deadline that applies runs out, UINT64_MAX if none does: lingering,
 * the idle deadline until a TLS client begins its handshake, then the head's
 * until it is done, the write stall's while output is stuck, the idle or the
 * head deadline until a head is parsed, none once the body is in and the
 * application works, else the body's minimum rate.
 */
static uint64_t
httpd_conn_deadline (struct httpd_conn const *conn)
{
	struct httpd_limits const *lim = &conn->srv->lim;
	if (conn->linger)
		return conn->since + LINGER_S * NS;
	if (conn->ctx)
		return httpd_after(conn->since, conn->ssl ? lim->head_time : lim->idle_time);
	if (conn->stall)
		return httpd_after(conn->stall, lim->write_time);
	if (!conn->parsed)
		return httpd_after(conn->since, httpd_conn_input(conn) ? lim->head_time
		                                                       : lim->idle_time);
	if (conn->body_in || !lim->body_rate)
		return UINT64_MAX;

	/* From its start, once the grace is up, the body must have come at
	 * body_rate: the bytes so far pay for so much time. */
	size_t secs = conn->got / lim->body_rate;
	if (secs > UINT32_MAX)
		return UINT64_MAX;
	uint64_t paid = secs * NS + conn->got % lim->body_rate * NS / lim->body_rate;
	uint64_t grace = lim->body_grace * NS;
	return conn->since + (paid > grace ? paid : grace);
}

/* Bring the timer forward to the deadline, if that is sooner. */
static void
httpd_conn_arm (struct httpd_conn *conn)
{
	uint64_t due = httpd_conn_deadline(conn);
	if (due < conn->timer.due)
		(void)loop_timer_set(conn->srv->loop, &conn->timer, due);
}

/* A deadline ran out: a slow head or body is answered 408 and the connection
 * ends once that is sent; otherwise it closes at once.  Returns 0, or
 * ETIMEDOUT once it is closed.
 */
static int
httpd_conn_expire (struct httpd_conn *conn)
{
	if (!conn->stall && buf_peek(&conn->in).len) {
		httpd_conn_fail(conn, 408);
		return 0;
	}
	httpd_conn_close(conn);
	return ETIMEDOUT;
}

/* Discard what a lingering client still sends, a read a step; close once it
 * has closed too.  Returns 0, or once the connection is closed EPIPE, or
 * recv()'s error.
 */
static int
httpd_conn_discard (struct httpd_conn *conn)
{
	ssize_t n = recv(conn->fd, nullptr, LOOP_STEP_RECV, MSG_TRUNC | MSG_DONTWAIT);
	if (n > 0) {
		loop_work(conn->srv->loop, LOOP_RECV, (uint64_t)n);
		return 0;
	}
	int e = n ? errno : EPIPE;
	if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR)
		return 0;
	httpd_conn_close(conn);
	return e;
}

/* The last response is sent: linger, unless the client has closed already,
 * when what it sent is discarded and the connection closed.  A stream the
 * application still has is cancelled.  TLS says it ends, and is gone: what
 * the client sends from then on is only discarded.
 */
static void
httpd_conn_end (struct httpd_conn *conn)
{
	struct httpd *srv = conn->srv;
	httpd_conn_cancel(conn);
	tls_close(conn->ssl);
	tls_destroy(&conn->ssl);
	if (conn->eof || shutdown(conn->fd, SHUT_WR) ||
	    loop_modify(srv->loop, &conn->ref, EPOLLIN)) {
		/* Input left unread would turn the close into a reset. */
		(void)recv(conn->fd, nullptr, srv->in_max, MSG_TRUNC | MSG_DONTWAIT);
		httpd_conn_close(conn);
		return;
	}
	conn->linger = true;
	conn->since = srv->loop->now;
	httpd_conn_unhold(conn);
	buf_fini(&conn->in);
	loop_unready(&conn->step);
	httpd_conn_park(conn, false);
	httpd_conn_arm(conn);
}

/*
 * After a step that ended at @p next: wait for input once no complete request
 * is left, for the application while it works on one, for output space once
 * lim.out_pause bytes are @p pending or TLS is @p blocked, and for the next
 * turn otherwise, as for a request the step left (@p again).  Input that
 * ended stays readable: watching it then would spin, as it would while TLS
 * waits to send.  Input TLS holds, which epoll does not report, is read in
 * the next step.
 */
static void
httpd_conn_wait (struct httpd_conn *conn,
                 enum httpd_next    next,
                 size_t             pending,
                 bool               blocked,
                 bool               again)
{
	struct httpd *srv = conn->srv;
	bool starved = next == NEXT_INPUT;
	bool paused = pending >= srv->lim.out_pause || conn->closing;
	bool reads = starved && !paused && !blocked && !conn->eof && !conn->waits;
	bool held = reads && conn->events & HELD;
	uint32_t events = pending || blocked ? EPOLLOUT : 0;
	if (conn->waits)
		events |= EPOLLRDHUP;
	else if (reads)
		events |= EPOLLIN;
	if (loop_modify(srv->loop, &conn->ref, events)) {
		httpd_conn_close(conn);
		return;
	}
	if (held)
		conn->events |= EPOLLIN;
	bool waiting = !again && !held &&
	               (starved || next == NEXT_WAIT || paused || blocked);
	if (waiting)
		loop_unready(&conn->step);
	else
		loop_ready(srv->loop, &conn->step);

	/* An idle connection keeps no storage, and under critical memory
	 * pressure one that waits keeps no more than its bytes need. */
	bool idle = starved && !pending && !blocked && !httpd_conn_input(conn);
	if (idle)
		buf_fini(&conn->in);
	else if (waiting && atomic_load_explicit(&buf_pressure, memory_order_relaxed) >=
	                    BUF_CRITICAL)
		loop_work(srv->loop, LOOP_COPY, buf_trim(&conn->in) + buf_trim(&conn->out));

	httpd_conn_park(conn, idle);
	httpd_conn_arm(conn);
}

/* One step: requests, as far as the step's share and STEP_NS allow, then one
 * send of their responses; once the last is sent, the connection ends.
 */
static void
httpd_conn_pump (struct httpd_conn *conn)
{
	struct httpd *srv = conn->srv;
	enum httpd_next next = NEXT_MORE;
	uint64_t end = loop_clock() + STEP_NS;
	while (next == NEXT_MORE && !conn->doomed &&
	       buf_peek(&conn->out).len < srv->lim.out_pause)
		next = httpd_conn_next(conn, end);
	if (conn->doomed) {
		httpd_conn_close(conn);
		return;
	}
	if (httpd_conn_flush(conn))
		return;

	size_t pending = buf_peek(&conn->out).len;
	bool again = false;
	/* Call on_writable() when the pending output is at most half of
	 * out_pause.  With an out_pause of 1, half of it is 0, so the call
	 * happens only when no output is pending.  A comparison with < would
	 * never be true for that value. */
	if (conn->writable && pending <= srv->lim.out_pause / 2) {
		conn->writable = false;
		if (conn->open)
			srv->app->on_writable(&conn->stream);
		if (conn->doomed) {
			httpd_conn_close(conn);
			return;
		}
		pending = buf_peek(&conn->out).len;
		/* A response ended there leaves its request to the next step. */
		again = conn->done;
	}
	bool blocked = httpd_conn_blocked(conn);
	if (!pending && !blocked &&
	    (next == NEXT_END || (next == NEXT_INPUT && conn->eof))) {
		httpd_conn_end(conn);
		return;
	}
	httpd_conn_wait(conn, next, pending, blocked, again);
}

/* One recv, or one TLS record, straight into the input's free tail.  A
 * head's clock starts with its first byte, which may be in TLS still.
 * Returns 0, or the error that closed the connection.
 */
static int
httpd_conn_read (struct httpd_conn *conn)
{
	/* Input is read only while no complete request is buffered, and a
	 * body is cut out as it comes, so the limit is never reached and an
	 * error means out of memory. */
	struct loop *loop = conn->srv->loop;
	bool first = !conn->parsed && !httpd_conn_input(conn);
	struct span s;
	int e = buf_reserve(&conn->in, READ_MIN, conn->srv->in_max, &s);
	if (e) {
		httpd_conn_close(conn);
		return e;
	}
	size_t room = loop_room(loop, LOOP_RECV);
	ssize_t n = httpd_conn_recv(conn, s.ptr, s.len < room ? s.len : room);
	conn->events &= ~HELD;
	if (n > 0) {
		buf_commit(&conn->in, (size_t)n);
		loop_work(loop, LOOP_RECV, (uint64_t)n);
	} else if (!n) {
		conn->eof = true;
	} else if (n != -EAGAIN && n != -EWOULDBLOCK && n != -EINTR) {
		httpd_conn_close(conn);
		return (int)-n;
	}
	if (first && httpd_conn_input(conn))
		conn->since = loop->now;
	/* After a read that gave bytes, or that waits to send, what TLS holds
	 * is read again; a part of a record waits for the rest. */
	if (conn->ssl && (n > 0 || httpd_conn_blocked(conn)) && tls_pending(conn->ssl))
		conn->events |= HELD;
	return 0;
}

/* A TLS listener's client begins the handshake with its first bytes: its end
 * of TLS is made then.  Returns 0, or EAGAIN while it has sent none, or EPIPE
 * once the connection is closed: a client that closes without a word, such
 * as a probe of the port, began no handshake.
 */
static int
httpd_conn_hello (struct httpd_conn *conn)
{
	char c;
	ssize_t peek = recv(conn->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
	if (peek < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return EAGAIN;
	conn->ssl = peek > 0 ? tls_create(conn->ctx, conn->fd) : nullptr;
	if (!conn->ssl) {
		httpd_conn_close(conn);
		return EPIPE;
	}
	conn->since = conn->srv->loop->now;
	httpd_conn_park(conn, false);
	return 0;
}

/* A flight of the handshake, from the client's first bytes on within the
 * head's deadline; once it is done, the connection serves as any other.
 * Each flight's time is its CPU's, mostly.  A step woken by the client's FIN
 * alone counts none.
 */
static void
httpd_conn_shake (struct httpd_conn *conn)
{
	struct httpd *srv = conn->srv;
	struct loop *loop = srv->loop;
	struct metrics_conns *m = &srv->lane->metrics;
	if (!conn->ssl && httpd_conn_hello(conn))
		return;
	uint64_t start = loop_clock();
	enum tls_shake shake = tls_shake(conn->ssl);
	hist_record(&m->flights, loop_clock() - start);
	loop_work(loop, LOOP_FLIGHTS, 1);
	if (shake == TLS_SHAKEN) {
		hist_add(&m->shaken, 1);
		tls_ctx_put(conn->ctx);
		conn->ctx = nullptr;
		conn->since = loop->now;
		if (tls_pending(conn->ssl))
			conn->events |= HELD;
		httpd_conn_pump(conn);
		return;
	}
	if (shake == TLS_FAILED ||
	    loop_modify(loop, &conn->ref, shake == TLS_WRITE ? EPOLLOUT : EPOLLIN)) {
		httpd_conn_close(conn);
		return;
	}
	httpd_conn_arm(conn);
}

static void
httpd_conn_step (struct loop      *loop,
                 struct loop_step *step)
{
	struct httpd_conn *conn = container_of(step, struct httpd_conn, step);
	uint32_t events = conn->events;
	conn->events &= HELD;

	/* A client that goes away while the application waits cancels it. */
	if (conn->doomed || (events & (EPOLLERR | EPOLLHUP)) ||
	    (conn->waits && (events & EPOLLRDHUP))) {
		httpd_conn_close(conn);
		return;
	}
	bool due = (events & DEADLINE) && httpd_conn_deadline(conn) <= loop->now;
	/* A handshake ends as a head would, but with no answer. */
	if (conn->ctx) {
		if (conn->srv->draining || due)
			httpd_conn_close(conn);
		else if (events & (EPOLLIN | EPOLLOUT))
			httpd_conn_shake(conn);
		return;
	}
	/* A lingering connection only discards what comes, until it closes. */
	if (conn->linger) {
		if (due)
			httpd_conn_close(conn);
		else
			(void)httpd_conn_discard(conn);
		return;
	}
	int e = due ? httpd_conn_expire(conn)
	      : events & EPOLLIN ? httpd_conn_read(conn)
	      : 0;
	if (!e)
		httpd_conn_pump(conn);
}

static void
httpd_conn_event (struct loop     *loop,
                  struct loop_ref *ref,
                  uint32_t         events)
{
	(void)loop;
	struct httpd_conn *conn = ref->owner;
	conn->events |= events;
	httpd_conn_ready(conn);
}

/* Fired early, re-arm for the deadline as it now stands; else let the step
 * act on it.
 */
static void
httpd_conn_timer (struct loop       *loop,
                  struct loop_timer *timer)
{
	struct httpd_conn *conn = container_of(timer, struct httpd_conn, timer);
	uint64_t due = httpd_conn_deadline(conn);
	if (due > loop->now) {
		(void)loop_timer_set(loop, timer, due);
		return;
	}
	conn->events |= DEADLINE;
	httpd_conn_ready(conn);
}

/* Watch the listeners for clients, or for none if @p events is 0. */
static void
httpd_home_watch (struct httpd_home *home,
                  uint32_t           events)
{
	for (size_t i = 0; i < home->n_listeners; ++i)
		(void)loop_modify(home->loop, &home->listeners[i].ref, events);
}

/* Take the spare descriptor, given up to accept a client once descriptors
 * run out: 0, or open()'s error.
 */
static int
httpd_home_spare (struct httpd_home *home)
{
	home->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	return home->spare_fd < 0 ? errno : 0;
}

/* Free the storage of a connection, which may be a spare. */
static void
httpd_conn_destroy (struct httpd_conn **p_conn)
{
	if (!p_conn || !*p_conn)
		return;
	struct httpd_conn *conn = *p_conn;
	*p_conn = nullptr;
	free(conn->groups);
	free(conn);
}

/* Let go of the client of a connection home placed and no loop set up: its
 * descriptor, and what it was to shake hands by.
 */
static void
httpd_conn_drop (struct httpd_conn *conn)
{
	tls_ctx_put(conn->ctx);
	conn->ctx = nullptr;
	close(conn->fd);
	conn->fd = -1;
}

/* Home has a connection back: it counts it no more, and keeps its storage
 * for the next client.  A descriptor came free, so the spare may be had again.
 * Once home's loop has stopped, its door is gone, and it is freed.
 */
static void
httpd_conn_back (struct loop *loop,
                 struct msg  *msg)
{
	struct httpd_conn *conn = container_of(msg, struct httpd_conn, msg);
	struct httpd_home *home = door;
	if (loop->exit || !home) {
		httpd_conn_destroy(&conn);
		return;
	}
	httpd_home_client_put(home, conn->client);
	--conn->lane->conns;
	--home->conns;
	shelf_put(&home->spare, &conn->hook);
	if (home->spare_fd < 0 && home->open && !httpd_home_spare(home))
		httpd_home_watch(home, EPOLLIN);
}

/* Set up a connection home placed on the loop, or close it at once while the
 * server drains, giving it back to home.
 */
static void
httpd_conn_setup (struct httpd_conn *conn,
                  struct httpd      *srv)
{
	struct loop *loop = srv->loop;
	struct httpd_lane *lane = conn->lane;
	struct httpd_client *client = conn->client;
	uint32_t *groups = conn->groups;
	uint32_t n_groups = conn->n_groups;
	uint32_t uid = conn->uid;
	uint32_t room = conn->room;
	struct ssl_ctx_st *ctx = conn->ctx;
	int fd = conn->fd;
	bool local = conn->local;
	bool uds = conn->uds;
	*conn = (struct httpd_conn){
		.msg.fn = httpd_conn_back,
		.ref.fd = -1,
		.step.fn = httpd_conn_step,
		.timer.fn = httpd_conn_timer,
		.stream = {
			.ops = &httpd_stream_ops,
			.peer = { client->addr[0], client->addr[1] }
		},
		.srv = srv,
		.lane = lane,
		.client = client,
		.ctx = ctx,
		.since = loop->now,
		.groups = groups,
		.n_groups = n_groups,
		.uid = uid,
		.room = room,
		.fd = fd,
		.local = local,
		.uds = uds
	};
	hist_add(&lane->metrics.taken, 1);
	/* Replies go out whole: Nagle would hold back one that follows another
	 * until the client, which sends nothing, acknowledges. */
	int one = 1;
	if (!srv->draining &&
	    (uds || !setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one)) &&
	    !loop_timer_set(loop, &conn->timer, httpd_after(conn->since, srv->lim.idle_time)) &&
	    !loop_watch(loop, &conn->ref, fd, EPOLLIN, httpd_conn_event, conn)) {
		list_init(&conn->hook);
		atomic_store_explicit(&lane->metrics.open, ++srv->conns,
		                      memory_order_relaxed);
		httpd_conn_park(conn, true);
		return;
	}
	loop_timer_stop(loop, &conn->timer);
	httpd_conn_drop(conn);
	loop_post(srv->hub->home, &conn->msg);
}

/* The client loop takes a connection home placed on it, for a step to set
 * up; once the loop has stopped, its server and home's door may be gone, and
 * it is only closed and freed.
 */
static void
httpd_conn_take (struct loop *loop,
                 struct msg  *msg)
{
	struct httpd_conn *conn = container_of(msg, struct httpd_conn, msg);
	if (loop->exit) {
		httpd_conn_drop(conn);
		httpd_conn_destroy(&conn);
		return;
	}
	struct httpd *srv = conn->lane->srv;
	list_append(&srv->taken, &conn->hook);
	loop_ready(loop, &srv->take);
}

/* Set up the connections home handed over, as many a step as its share of
 * accepts allows.
 */
static void
httpd_take_step (struct loop      *loop,
                 struct loop_step *step)
{
	struct httpd *srv = container_of(step, struct httpd, take);
	for (; !list_is_empty(&srv->taken); loop_work(loop, LOOP_ACCEPTS, 1)) {
		if (!loop_room(loop, LOOP_ACCEPTS)) {
			loop_ready(loop, step);
			return;
		}
		struct httpd_conn *conn = container_of(srv->taken.next, struct httpd_conn,
		                                       hook);
		list_del(&conn->hook);
		httpd_conn_setup(conn, srv);
	}
}

/* On a client loop: take the policy home stored last, and let go of the
 * one before; once the loop has stopped, only let go of it.
 */
static void
httpd_lane_policy (struct loop *loop,
                   struct msg  *msg)
{
	struct httpd_lane *lane = container_of(msg, struct httpd_lane, handover);
	struct policy *policy = atomic_exchange(&lane->policy, nullptr);
	if (policy && !loop->exit) {
		struct policy *old = lane->srv->policy;
		lane->srv->policy = policy;
		policy = old;
		lane->srv->app->on_policy(lane->srv->app->ctx);
	}
	policy_put(policy);
}

/* Home asks the loop to close idle connections, as descriptors run short. */
static void
httpd_lane_evict (struct loop *loop,
                  struct msg  *msg)
{
	struct httpd_lane *lane = container_of(msg, struct httpd_lane, evict);
	uint32_t n = atomic_exchange(&lane->evicts, 0);
	if (loop->exit)
		return;
	lane->srv->evicting += n;
	loop_ready_bulk(loop, &lane->srv->evict);
}

/* Close the least recently active idle connections home asked for, as many
 * a step as its share allows; once none is idle, none is left to close.
 */
static void
httpd_evict_step (struct loop      *loop,
                  struct loop_step *step)
{
	struct httpd *srv = container_of(step, struct httpd, evict);
	for (; srv->evicting && !list_is_empty(&srv->idle); --srv->evicting) {
		if (!loop_room(loop, LOOP_CLOSES)) {
			loop_ready_bulk(loop, step);
			return;
		}
		httpd_conn_close(container_of(srv->idle.next, struct httpd_conn, hook));
	}
	srv->evicting = 0;
}

/* Draining: close the idle connections, and from the deadline on every one
 * left, as many a step as its share allows.
 */
static void
httpd_drain_step (struct loop      *loop,
                  struct loop_step *step)
{
	struct httpd *srv = container_of(step, struct httpd, drain);
	struct list *rest = loop->now >= srv->drain_by ? &srv->busy : &srv->idle;
	while (loop_room(loop, LOOP_CLOSES)) {
		struct list *list = list_is_empty(&srv->idle) ? rest : &srv->idle;
		if (list_is_empty(list))
			return;
		httpd_conn_close(container_of(list->next, struct httpd_conn, hook));
	}
	loop_ready_bulk(loop, step);
}

static void
httpd_deadline_event (struct loop       *loop,
                      struct loop_timer *timer)
{
	loop_ready_bulk(loop, &container_of(timer, struct httpd, deadline)->drain);
}

/* Whether clients hold all the descriptors they may: one for each connection,
 * and one more for each job in flight, for its upstream connection.
 */
static bool
httpd_home_full (struct httpd_home const *home)
{
	return home->conns + hist_get(home->calls) >= home->fd_budget;
}

/* Leave FD_RESERVE descriptors and the two each client loop holds, its
 * epoll's and its inbox's, or half of fewer, to all but clients.
 */
static void
httpd_home_budget (struct httpd_home *home)
{
	struct rlimit nofile = { .rlim_cur = RLIM_INFINITY };
	(void)getrlimit(RLIMIT_NOFILE, &nofile);
	rlim_t fds = nofile.rlim_cur;
	rlim_t reserve = FD_RESERVE + 2 * home->hub->n;
	home->fd_budget = (size_t)(fds < 2 * reserve ? fds / 2 : fds - reserve);
}

/*
 * Have the least recently active idle connection closed, by its loop: 0, or
 * EAGAIN if no loop has one idle to spare.  Each loop says how many idle and
 * when the least recently active of them began to, and home looks at each,
 * only when descriptors run short.  A loop asked to close some, which it has
 * yet to take on, stands for when its next idle one began to, were their
 * beginnings spread evenly since, so that a burst of new clients or calls is
 * spread over the loops, and a loop is asked for no more than it has idle.
 */
static int
httpd_home_evict (struct httpd_home *home)
{
	struct httpd_hub *hub = home->hub;
	struct httpd_lane *oldest = nullptr;
	uint64_t now = home->loop->now;
	uint64_t least = UINT64_MAX;
	for (size_t i = 0; i < hub->n; ++i) {
		struct httpd_lane *lane = &hub->lane[i];
		uint32_t idles = atomic_load_explicit(&lane->idles, memory_order_relaxed);
		uint32_t asked = atomic_load_explicit(&lane->evicts, memory_order_relaxed);
		if (idles <= asked)
			continue;
		uint64_t since = atomic_load_explicit(&lane->idle, memory_order_relaxed);
		uint64_t next = since < now ? since + (now - since) / idles * asked : since;
		if (next < least) {
			least = next;
			oldest = lane;
		}
	}
	if (!oldest)
		return EAGAIN;
	if (!atomic_fetch_add(&oldest->evicts, 1))
		loop_post(oldest->loop, &oldest->evict);
	return 0;
}

/* A client loop's weight: the connections home placed on it and has not had
 * back, and a quarter of its utilisation in per mille, which home works out
 * from the time its turns took since the last look, SAMPLE_NS ago at least.
 * A connection placed since counts once more, as its work is yet to show in
 * the utilisation: so connections that come and go spread over the loops
 * too, rather than go to the lightest until the next look.
 */
static uint64_t
httpd_home_weight (struct httpd_home *home,
                   struct httpd_lane *lane)
{
	uint64_t now = home->loop->now;
	if (now - lane->sampled >= SAMPLE_NS) {
		uint64_t busy = hist_get(&lane->loop->metrics->turn.sum);
		uint64_t util = (busy - lane->busy) * 1000 / (now - lane->sampled);
		lane->util = util < 1000 ? util : 1000;
		lane->busy = busy;
		lane->sampled = now;
		lane->placed = 0;
	}
	return lane->conns + lane->placed + lane->util / 4;
}

/* The lighter of two client loops chosen at random (design section 3), or
 * of two as light the one with fewer connections.
 */
static struct httpd_lane *
httpd_home_lane (struct httpd_home *home)
{
	struct httpd_hub *hub = home->hub;
	if (hub->n == 1)
		return hub->lane;
	uint64_t r = home->rng;
	r ^= r << 13;
	r ^= r >> 7;
	r ^= r << 17;
	home->rng = r;
	size_t a = (size_t)(r % hub->n);
	size_t b = (size_t)((r >> 32) % (hub->n - 1));
	struct httpd_lane *x = &hub->lane[a];
	struct httpd_lane *y = &hub->lane[b + (b >= a)];
	uint64_t wx = httpd_home_weight(home, x);
	uint64_t wy = httpd_home_weight(home, y);
	return wy < wx || (wy == wx && y->conns < x->conns) ? y : x;
}

/* Make room for @p n groups in the storage kept with the connection: 0, or
 * ENOMEM.
 */
static int
httpd_conn_room (struct httpd_conn *conn,
                 size_t             n)
{
	if (conn->room >= n)
		return 0;
	uint32_t *groups = realloc(conn->groups, n * sizeof *groups);
	if (!groups)
		return ENOMEM;
	conn->groups = groups;
	conn->room = (uint32_t)n;
	return 0;
}

/* Read the groups of the user of Unix socket @p fd, whose primary group is
 * @p gid, into @p conn, sorted, growing the storage it keeps for them if
 * need be: 0, or ENOMEM.
 */
static int
httpd_conn_groups (struct httpd_conn *conn,
                   int                fd,
                   gid_t              gid)
{
	static_assert(_Generic((gid_t)0, uint32_t: true, default: false));
	/* Room for the supplementary groups, as many as the kernel last said,
	 * and the primary one, which those may lack. */
	size_t n = 0;
	int e = ERANGE;
	while (e == ERANGE) {
		if (httpd_conn_room(conn, n + 1))
			return ENOMEM;
		socklen_t size = (socklen_t)((conn->room - 1) * sizeof *conn->groups);
		e = getsockopt(fd, SOL_SOCKET, SO_PEERGROUPS, conn->groups, &size) ? errno : 0;
		n = size / sizeof *conn->groups;
	}
	/* Those the kernel cannot tell are none. */
	if (e)
		n = 0;
	conn->groups[n] = gid;
	conn->n_groups = (uint32_t)n + 1;
	policy_sort_ids(conn->groups, n + 1);
	return 0;
}

/* The user of the client on @p fd, as @p cred says if over a Unix socket,
 * with the user's groups; else none: 0, or ENOMEM.
 */
static int
httpd_conn_user (struct httpd_conn  *conn,
                 int                 fd,
                 struct ucred const *cred)
{
	conn->uds = cred;
	if (!cred) {
		conn->n_groups = 0;
		conn->uid = 0;
		return 0;
	}
	conn->uid = cred->uid;
	return httpd_conn_groups(conn, fd, cred->gid);
}

/* Storage for a connection: one given back, if home keeps any; nullptr if
 * out of memory.
 */
static struct httpd_conn *
httpd_home_conn (struct httpd_home *home)
{
	struct list *spare = shelf_take(&home->spare);
	if (spare)
		return container_of(spare, struct httpd_conn, hook);
	struct httpd_conn *conn = malloc(sizeof *conn);
	if (conn)
		*conn = (struct httpd_conn){ .fd = -1 };
	return conn;
}

/* Place the client on @p fd, from @p addr, on loopback if @p local, on a
 * client loop, in the storage of a connection given back if one is kept,
 * as the user @p cred says if over a Unix socket, to shake hands by @p ctx
 * if not nullptr; if out of memory, close @p fd.
 */
static void
httpd_home_place (struct httpd_home  *home,
                  int                 fd,
                  uint64_t const      addr[2],
                  bool                local,
                  struct ucred const *cred,
                  struct ssl_ctx_st  *ctx)
{
	struct httpd_conn *conn = httpd_home_conn(home);
	if (!conn) {
		close(fd);
		return;
	}
	struct httpd_client *client = httpd_conn_user(conn, fd, cred) ? nullptr
	                            : httpd_home_client_get(home, addr);
	if (!client) {
		shelf_put(&home->spare, &conn->hook);
		close(fd);
		return;
	}
	struct httpd_lane *lane = httpd_home_lane(home);
	++lane->conns;
	++lane->placed;
	++home->conns;
	conn->msg.fn = httpd_conn_take;
	conn->lane = lane;
	conn->client = client;
	conn->ctx = ctx ? tls_ctx_get(ctx) : nullptr;
	conn->fd = fd;
	conn->local = local;
	loop_post(lane->loop, &conn->msg);
}

/*
 * Place the client on @p fd, from listener @p l, unless its address is at
 * the cap, which is said in plain HTTP but to TLS's clients.  A client
 * beyond the descriptor budget, or one accepted in the spare descriptor's
 * place (@p shed), has the least recently active idle connection closed to
 * make room, or is turned away if none is idle.
 */
static void
httpd_home_take (struct httpd_home             *home,
                 struct httpd_listener const   *l,
                 int                            fd,
                 struct sockaddr_storage const *ss,
                 bool                           shed)
{
	/* A Unix socket's client is on this machine, and counts as its user,
	 * against the cap. */
	uint64_t addr[2] = { UINT64_MAX };
	struct ucred cred;
	struct ucred const *user = nullptr;
	bool local = true;
	socklen_t len = sizeof cred;
	if (ss->ss_family != AF_UNIX) {
		httpd_addr_key(ss, addr);
		local = httpd_addr_loopback(ss);
	} else if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len)) {
		close(fd);
		++home->refused;
		return;
	} else {
		addr[1] = cred.uid;
		user = &cred;
	}
	struct httpd_client const *client = *httpd_home_client_slot(home, addr);
	if (client && home->cap && client->conns >= home->cap) {
		static char const busy[] = "HTTP/1.1 503 Service Unavailable\r\n"
		                           "Retry-After: 1\r\nConnection: close\r\n"
		                           "Content-Length: 0\r\n\r\n";
		if (!l->tls)
			(void)send(fd, busy, sizeof busy - 1, MSG_DONTWAIT | MSG_NOSIGNAL);
		close(fd);
		++home->capped;
		return;
	}
	if ((shed || httpd_home_full(home)) && httpd_home_evict(home)) {
		close(fd);
		++home->refused;
		return;
	}
	httpd_home_place(home, fd, addr, local, user, l->tls ? home->ctx : nullptr);
}

/* Accept a client from @p l, and take it as httpd_home_take() does: 0, or
 * accept()'s error.
 */
static int
httpd_home_accept (struct httpd_home           *home,
                   struct httpd_listener const *l,
                   bool                         shed)
{
	struct sockaddr_storage ss;
	socklen_t len = sizeof ss;
	int fd = accept4(l->fd, (struct sockaddr *)&ss, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0)
		return errno;
	httpd_home_take(home, l, fd, &ss, shed);
	return 0;
}

/*
 * Out of descriptors, which accept() reports whether or not a client waits:
 * accept in the spare descriptor's place.  Returns 0 if a client was
 * accepted from @p l, or else accept()'s error, EAGAIN if none waited.
 * The spare comes back once a descriptor comes free.
 */
static int
httpd_home_shed (struct httpd_home           *home,
                 struct httpd_listener const *l)
{
	close(home->spare_fd);
	home->spare_fd = -1;
	int e = httpd_home_accept(home, l, true);
	(void)httpd_home_spare(home);
	return e;
}

/* Whether accept() failing with @p e concerns only the connection it took,
 * so the next may be accepted: Linux passes on a pending connection's network
 * error, and firewall rules may forbid one.
 */
static bool
httpd_accept_next (int e)
{
	switch (e) {
	case EINTR: case ECONNABORTED: case EPERM: case EPROTO: case ENOPROTOOPT:
	case ENETDOWN: case ENETUNREACH: case EHOSTDOWN: case EHOSTUNREACH:
	case ENONET: case EOPNOTSUPP:
		return true;
	default:
		return false;
	}
}

/* Accept clients as far as the step's share goes, fewer once STEP_NS have
 * passed: turning one away closes it, which costs the more the more it has
 * sent.  The listeners that clients wait on take turns, a client each.
 * Only with the spare descriptor at hand, to accept a client with when
 * descriptors run out; without it, listening pauses until a connection is
 * given back, or the tick.
 */
static void
httpd_home_accept_step (struct loop      *loop,
                        struct loop_step *step)
{
	struct httpd_home *home = container_of(step, struct httpd_home, accept);
	uint64_t end = loop_clock() + STEP_NS;
	size_t n = home->n_listeners;
	for (unsigned i = 0; loop_room(loop, LOOP_ACCEPTS) && (!i || loop_clock() < end);
	     ++i) {
		size_t k = 0;
		while (k < n && !home->listeners[(home->turn + k) % n].ready)
			++k;
		if (k == n)
			return;
		size_t at = (home->turn + k) % n;
		struct httpd_listener *l = &home->listeners[at];
		home->turn = (at + 1) % n;
		if (home->spare_fd < 0 && httpd_home_spare(home)) {
			httpd_home_watch(home, 0);
			return;
		}
		loop_work(loop, LOOP_ACCEPTS, 1);
		int e = httpd_home_accept(home, l, false);
		if (e == EMFILE || e == ENFILE)
			e = httpd_home_shed(home, l);
		if (!e || httpd_accept_next(e))
			continue;
		l->ready = false;
		if (e == EAGAIN || e == EWOULDBLOCK)
			continue;
		/* Out of memory or descriptors: retry on the next tick. */
		LOG(LOG_ERROR, "accept: %s", strerror(e));
		httpd_home_watch(home, 0);
		return;
	}
	loop_ready(loop, step);
}

static void
httpd_listener_event (struct loop     *loop,
                      struct loop_ref *ref,
                      uint32_t         events)
{
	(void)events;
	struct httpd_home *home = ref->owner;
	container_of(ref, struct httpd_listener, ref)->ready = true;
	loop_ready(loop, &home->accept);
}

static void
httpd_home_tick (struct loop       *loop,
                 struct loop_timer *timer)
{
	struct httpd_home *home = container_of(timer, struct httpd_home, tick);
	httpd_home_budget(home);
	if (home->refused)
		LOG(LOG_WARN, "out of descriptors: turned %" PRIu32 " clients away",
		    home->refused);
	if (home->capped)
		LOG(LOG_WARN, "connection cap per client: turned %" PRIu32
		    " clients away", home->capped);
	home->refused = home->capped = 0;
	if (home->spare_fd < 0)
		(void)httpd_home_spare(home);
	if (home->open)
		httpd_home_watch(home, EPOLLIN);
	(void)loop_timer_set(loop, timer, loop->now + TICK_S * NS);
}

/* Address @p sa was in use as the listener's Unix socket was bound: if its
 * file is a socket left there, which no server listens on, remove it and
 * bind again.  Returns 0, or EADDRINUSE if the file is no such socket, or
 * the error that kept the probe, the removal or the bind from working.
 */
static int
httpd_listener_rebind (struct httpd_listener    *l,
                       struct sockaddr_un const *sa)
{
	struct stat st;
	if (lstat(sa->sun_path, &st) || !S_ISSOCK(st.st_mode))
		return EADDRINUSE;
	int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (probe < 0)
		return errno;
	bool stale = connect(probe, (struct sockaddr const *)sa, sizeof *sa) &&
	             errno == ECONNREFUSED;
	close(probe);
	if (!stale)
		return EADDRINUSE;
	if (unlink(sa->sun_path))
		return errno;
	return bind(l->fd, (struct sockaddr const *)sa, sizeof *sa) ? errno : 0;
}

/* Bind the listener's Unix socket to @p sa and listen: a file left there,
 * which no server listens on, goes first.  Returns 0, or the error.
 */
static int
httpd_listener_bind_uds (struct httpd_listener    *l,
                         struct sockaddr_un const *sa)
{
	if (bind(l->fd, (struct sockaddr const *)sa, sizeof *sa)) {
		int e = errno == EADDRINUSE ? httpd_listener_rebind(l, sa) : errno;
		if (e)
			return e;
	}
	return listen(l->fd, SOMAXCONN) ? errno : 0;
}

/* Make @p l listen on the Unix socket at @p path, which is @p len bytes
 * long.  httpd_listen_addr() has checked that the path fits in sun_path.  When
 * httpd_home_drain() closes the listener, it removes the socket file.
 */
static int
httpd_listener_uds (struct httpd_listener *l,
                    char const            *path,
                    size_t                 len)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	memcpy(sa.sun_path, path, len);
	l->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	int e = l->fd < 0 ? errno : httpd_listener_bind_uds(l, &sa);
	if (e) {
		LOG(LOG_ERROR, "cannot listen on unix:%s: %s", path, strerror(e));
		return e;
	}
	l->path = path;
	return 0;
}

/* Listen on @p ai into @p l: 0, or the error. */
static int
httpd_listener_bind (struct httpd_listener *l,
                     struct addrinfo const *ai)
{
	int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
	                ai->ai_protocol);
	if (fd < 0)
		return errno;
	int one = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) ||
	    bind(fd, ai->ai_addr, ai->ai_addrlen) || listen(fd, SOMAXCONN)) {
		int e = errno;
		close(fd);
		return e;
	}
	l->fd = fd;
	return 0;
}

/* Copy the host of @p len bytes at @p host, the part of a listen address
 * before its last colon, to a->host, without brackets.  A host in brackets,
 * and a host with a colon, must be an IPv6 address, which may have a zone
 * after a '%'.  Returns 0 or EINVAL.
 */
static int
httpd_listen_host (struct httpd_listen_addr *a,
                   char const               *host,
                   size_t                    len)
{
	bool bracket = len && *host == '[';
	if (bracket && (len < 2 || host[len - 1] != ']'))
		return EINVAL;
	host += bracket;
	len -= 2 * bracket;
	if (len >= sizeof a->host)
		return EINVAL;
	memcpy(a->host, host, len);
	a->host[len] = '\0';
	if (!bracket && !memchr(a->host, ':', len))
		return 0;
	/* inet_pton() does not parse a zone, so the check ends at the '%'.  A
	 * zone must not be empty.  getaddrinfo() reads the zone later. */
	char ip[INET6_ADDRSTRLEN];
	size_t n = strcspn(a->host, "%");
	if (n >= sizeof ip || (a->host[n] && !a->host[n + 1]))
		return EINVAL;
	memcpy(ip, a->host, n);
	ip[n] = '\0';
	return inet_pton(AF_INET6, ip, &(struct in6_addr){}) == 1 ? 0 : EINVAL;
}

struct httpd_listen_addr
httpd_listen_addr (char const *text,
                   int        *e)
{
	static char const tls[] = "tls:";
	static char const uds[] = "unix:";
	struct httpd_listen_addr a = {
		.tls = !strncmp(text, tls, sizeof tls - 1),
		.host = "127.0.0.1"
	};
	char const *at = text + a.tls * (sizeof tls - 1);
	if (!strncmp(at, uds, sizeof uds - 1)) {
		a.path = at + sizeof uds - 1;
		a.path_len = strlen(a.path);
		*e = a.tls ? EPROTONOSUPPORT
		   : !a.path_len ? EINVAL
		   : a.path_len >= sizeof (struct sockaddr_un){}.sun_path ? ENAMETOOLONG
		   : 0;
		return a;
	}

	/* The port is the text after the last colon, or all of the text if it
	 * has no colon.  The host is the text before the last colon. */
	char const *colon = strrchr(at, ':');
	a.port = at;
	if (colon) {
		a.port = colon + 1;
		*e = httpd_listen_host(&a, at, (size_t)(colon - at));
		if (*e)
			return a;
	}
	/* The loop stops once the value exceeds 65535, so the value cannot
	 * overflow. */
	size_t digits = strspn(a.port, "0123456789");
	uint32_t value = 0;
	for (size_t i = 0; i < digits && value <= 65535; ++i)
		value = value * 10 + (uint32_t)(a.port[i] - '0');
	*e = !digits || a.port[digits] ? EINVAL : value > 65535 ? ERANGE : 0;
	return a;
}

/* Make @p l listen on the listen address @p addr.  Returns 0 or an error
 * number.
 */
static int
httpd_listener_open (struct httpd_listener *l,
                     char const            *addr)
{
	int e;
	struct httpd_listen_addr const a = httpd_listen_addr(addr, &e);
	if (e) {
		LOG(LOG_ERROR, "invalid listen address: %s: %s", addr, strerror(e));
		return e;
	}
	l->tls = a.tls;
	if (a.path)
		return httpd_listener_uds(l, a.path, a.path_len);

	struct addrinfo const hints = {
		.ai_flags = AI_PASSIVE | AI_NUMERICSERV,
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	struct addrinfo *list;
	int g = getaddrinfo(*a.host ? a.host : nullptr, a.port, &hints, &list);
	if (g) {
		LOG(LOG_ERROR, "cannot listen on %s: %s", addr, gai_strerror(g));
		return EINVAL;
	}
	e = EADDRNOTAVAIL;
	for (struct addrinfo const *ai = list; ai && e; ai = ai->ai_next)
		e = httpd_listener_bind(l, ai);
	freeaddrinfo(list);
	if (e)
		LOG(LOG_ERROR, "cannot listen on %s: %s", addr, strerror(e));
	return e;
}

/* Listen on @p addr with the next listener, watched for no events until the
 * door opens.
 */
static int
httpd_home_listen (struct httpd_home *home,
                   char const        *addr)
{
	struct httpd_listener *l = &home->listeners[home->n_listeners++];
	l->ref.fd = l->fd = -1;
	int e = httpd_listener_open(l, addr);
	if (e)
		return e;
	if (l->tls && !home->ctx) {
		LOG(LOG_ERROR, "cannot listen on %s: no TLS certificate", addr);
		return EINVAL;
	}
	return loop_watch(home->loop, &l->ref, l->fd, 0, httpd_listener_event, home);
}

/* Listen on each of @p listen, take the spare descriptor, and start the
 * tick.
 */
static int
httpd_home_start (struct httpd_home *home,
                  char const *const *listen)
{
	size_t n = 0;
	while (listen[n])
		++n;
	home->listeners = calloc(n, sizeof *home->listeners);
	if (!home->listeners)
		return ENOMEM;
	for (size_t i = 0; i < n; ++i) {
		int e = httpd_home_listen(home, listen[i]);
		if (e)
			return e;
	}
	int e = httpd_home_spare(home);
	if (e)
		return e;
	return loop_timer_set(home->loop, &home->tick, home->loop->now + TICK_S * NS);
}

/* Say where clients find the endpoint of @p l; warn if clients that show no
 * token may call tools from beyond loopback (@p anyone) there.
 */
static void
httpd_listener_announce (struct httpd_listener const *l,
                         bool                         anyone)
{
	if (l->path) {
		LOG(LOG_INFO, "MCP endpoint http://localhost/mcp on unix:%s", l->path);
		return;
	}
	struct sockaddr_storage ss;
	socklen_t len = sizeof ss;
	char addr[INET6_ADDRSTRLEN];
	char port[8];
	if (getsockname(l->fd, (struct sockaddr *)&ss, &len) ||
	    getnameinfo((struct sockaddr *)&ss, len, addr, sizeof addr, port, sizeof port,
	                NI_NUMERICHOST | NI_NUMERICSERV))
		return;
	LOG(LOG_INFO, ss.ss_family == AF_INET6 ? "MCP endpoint http%s://[%s]:%s/mcp"
	                                       : "MCP endpoint http%s://%s:%s/mcp",
	    l->tls ? "s" : "", addr, port);
	if (anyone && !httpd_addr_loopback(&ss))
		LOG(LOG_WARN, "warning: listening beyond loopback, and clients without "
		    "a token may call tools");
}

struct httpd_hub *
httpd_hub_create (size_t  loops,
                  int    *e)
{
	struct httpd_hub *hub = calloc(1, sizeof *hub + loops * sizeof *hub->lane);
	if (!hub) {
		*e = ENOMEM;
		return nullptr;
	}
	atomic_init(&hub->bodies, 0);
	hub->n = loops;
	for (size_t i = 0; i < loops; ++i) {
		struct httpd_lane *lane = &hub->lane[i];
		lane->evict.fn = httpd_lane_evict;
		lane->handover.fn = httpd_lane_policy;
		atomic_init(&lane->policy, nullptr);
		atomic_init(&lane->metrics.open, 0);
		atomic_init(&lane->metrics.taken, 0);
		atomic_init(&lane->metrics.shaken, 0);
		atomic_init(&lane->metrics.unshaken, 0);
		hist_init(&lane->metrics.flights);
		atomic_init(&lane->idle, UINT64_MAX);
		atomic_init(&lane->idles, 0);
		atomic_init(&lane->evicts, 0);
	}
	return hub;
}

void
httpd_hub_destroy (struct httpd_hub **p_hub)
{
	if (!p_hub)
		return;
	free(*p_hub);
	*p_hub = nullptr;
}

struct metrics_conns const *
httpd_hub_metrics (struct httpd_hub const *hub,
                   size_t                  i)
{
	return &hub->lane[i].metrics;
}

struct httpd_home *
httpd_home_create (struct loop            *loop,
                   struct httpd_hub       *hub,
                   struct httpd_cfg const *cfg,
                   _Atomic uint64_t const *calls,
                   int                    *e)
{
	struct httpd_home *home = malloc(sizeof *home);
	struct httpd_client **clients = calloc(CLIENT_SLOTS, sizeof *clients);
	if (!home || !clients) {
		free(clients);
		free(home);
		LOG(LOG_ERROR, "cannot start the server: %s", strerror(ENOMEM));
		*e = ENOMEM;
		return nullptr;
	}
	*home = (struct httpd_home){
		.accept.fn = httpd_home_accept_step,
		.tick.fn = httpd_home_tick,
		.loop = loop,
		.hub = hub,
		.clients = clients,
		.calls = calls,
		.ctx = cfg->tls ? tls_ctx_get(cfg->tls) : nullptr,
		.rng = loop_clock() | 1,
		.seed = loop_clock(),
		.cap = cfg->limits.clients,
		.spare_fd = -1
	};
	struct policy_grant g;
	home->anyone = policy_grant(cfg->policy, nullptr, 0, POLICY_REMOTE, nullptr,
	                            &g) && g.tools;
	/* Without random bytes, the clock seeds the hash. */
	(void)getrandom(&home->seed, sizeof home->seed, GRND_NONBLOCK);
	list_init(&home->spare.list);
	list_init(&home->gone.list);
	httpd_home_budget(home);
	hub->home = loop;
	door = home;

	*e = httpd_home_start(home, cfg->listen);
	if (!*e)
		return home;
	if (*e != EINVAL && *e != EADDRINUSE)
		LOG(LOG_ERROR, "cannot start the server: %s", strerror(*e));
	httpd_home_destroy(&home);
	return nullptr;
}

void
httpd_home_destroy (struct httpd_home **p_home)
{
	if (!p_home || !*p_home)
		return;
	struct httpd_home *home = *p_home;
	*p_home = nullptr;
	httpd_home_drain(home);
	loop_timer_stop(home->loop, &home->tick);
	if (home->spare_fd >= 0)
		close(home->spare_fd);
	while (httpd_home_relieve(home, BUF_CRITICAL, SIZE_MAX))
		;
	/* Connections on their way to a loop, or back, count their client
	 * addresses still; the loops are done with them. */
	for (size_t i = 0; i < CLIENT_SLOTS; ++i)
		for (struct httpd_client *c = home->clients[i], *next; c; c = next) {
			next = c->next;
			free(c);
		}
	free(home->clients);
	free(home->listeners);
	tls_ctx_put(home->ctx);
	door = nullptr;
	free(home);
}

void
httpd_home_open (struct httpd_home *home)
{
	/* Drained already, the door stays shut. */
	if (!home->n_listeners || home->listeners->fd < 0)
		return;
	httpd_home_watch(home, EPOLLIN);
	home->open = true;

	/* The endpoints say the server is ready. */
	for (size_t i = 0; i < home->n_listeners; ++i)
		httpd_listener_announce(&home->listeners[i], home->anyone);
}

int
httpd_home_policy (struct httpd_home *home,
                   struct policy     *policy)
{
	/* Once the door is open, every client loop runs. */
	if (!home->open)
		return EAGAIN;
	/* A policy the loop has yet to take is replaced, its message still on
	 * its way; else the message goes again. */
	struct httpd_hub *hub = home->hub;
	for (size_t i = 0; i < hub->n; ++i) {
		struct httpd_lane *lane = &hub->lane[i];
		struct policy *old = atomic_exchange(&lane->policy, policy_get(policy));
		if (old)
			policy_put(old);
		else
			loop_post(lane->loop, &lane->handover);
	}
	return 0;
}

void
httpd_home_tls (struct httpd_home *home,
                struct ssl_ctx_st *ctx)
{
	if (!home->ctx)
		return;
	tls_ctx_put(home->ctx);
	home->ctx = tls_ctx_get(ctx);
}

void
httpd_home_drain (struct httpd_home *home)
{
	home->open = false;
	loop_unready(&home->accept);
	for (size_t i = 0; i < home->n_listeners; ++i) {
		struct httpd_listener *l = &home->listeners[i];
		if (l->fd < 0)
			continue;
		loop_unwatch(home->loop, &l->ref);
		close(l->fd);
		l->fd = -1;
		if (l->path)
			(void)unlink(l->path);
	}
}

void
httpd_home_crowd (struct httpd_home *home)
{
	if (httpd_home_full(home))
		(void)httpd_home_evict(home);
}

void
httpd_home_age (struct httpd_home *home)
{
	shelf_age(&home->spare);
	shelf_age(&home->gone);
}

size_t
httpd_home_relieve (struct httpd_home *home,
                    enum buf_pressure  level,
                    size_t             max)
{
	/* What home keeps holds no buffer, but a few groups, so each is a unit. */
	size_t done = 0;
	for (size_t n = shelf_unused(&home->spare, level); n && done < max; --n, ++done) {
		struct httpd_conn *conn = container_of(shelf_drop(&home->spare),
		                                       struct httpd_conn, hook);
		httpd_conn_destroy(&conn);
	}
	for (size_t n = shelf_unused(&home->gone, level); n && done < max; --n, ++done)
		free(container_of(shelf_drop(&home->gone), struct httpd_client, spare));
	return done;
}

struct httpd *
httpd_create (struct loop             *loop,
              struct httpd_hub        *hub,
              size_t                   i,
              struct stream_app const *app,
              struct httpd_cfg const  *cfg,
              int                     *e)
{
	if (app->n_fields > STREAM_FIELDS) {
		*e = EINVAL;
		return nullptr;
	}

	struct httpd *srv = malloc(sizeof *srv);
	if (!srv) {
		LOG(LOG_ERROR, "cannot start the server: %s", strerror(ENOMEM));
		*e = ENOMEM;
		return nullptr;
	}

	/* By default one client address may hold a quarter of the bodies. */
	struct httpd_limits lim = cfg->limits;
	if (!lim.client_bodies)
		lim.client_bodies = lim.bodies / 4 > lim.body ? lim.bodies / 4 : lim.body;
	*srv = (struct httpd){
		.take.fn = httpd_take_step,
		.drain.fn = httpd_drain_step,
		.evict.fn = httpd_evict_step,
		.deadline.fn = httpd_deadline_event,
		.lim = lim,
		.loop = loop,
		.hub = hub,
		.lane = &hub->lane[i],
		.app = app,
		.origins = cfg->origins,
		.policy = policy_get(cfg->policy),
		.idle_since = UINT64_MAX,
		.in_max = lim.head + lim.body + READ_MIN
	};
	list_init(&srv->idle);
	list_init(&srv->busy);
	list_init(&srv->taken);
	srv->lane->loop = loop;
	srv->lane->srv = srv;
	return srv;
}

void
httpd_destroy (struct httpd **p_httpd)
{
	if (!p_httpd || !*p_httpd)
		return;
	struct httpd *srv = *p_httpd;
	*p_httpd = nullptr;

	/* Those handed over but not set up close at once, as when draining. */
	srv->draining = true;
	while (!list_is_empty(&srv->taken)) {
		struct httpd_conn *conn = container_of(srv->taken.next, struct httpd_conn,
		                                       hook);
		list_del(&conn->hook);
		httpd_conn_setup(conn, srv);
	}
	struct list *const lists[] = { &srv->idle, &srv->busy };
	for (size_t i = 0; i < ARRAY_SIZE(lists); ++i)
		while (!list_is_empty(lists[i]))
			httpd_conn_close(container_of(lists[i]->next, struct httpd_conn, hook));
	/* Home, which outlives the loop's server, has them back now. */
	loop_reap(srv->loop);
	loop_unready(&srv->take);
	loop_unready(&srv->drain);
	loop_unready(&srv->evict);
	loop_timer_stop(srv->loop, &srv->deadline);
	policy_put(srv->policy);
	free(srv);
}

void
httpd_drain (struct httpd *srv,
             uint64_t      deadline)
{
	srv->draining = true;
	srv->drain_by = deadline;
	if (!srv->conns) {
		loop_exit(srv->loop);
		return;
	}
	/* A request under way goes on, its body included; the connections see
	 * the drain for themselves. */
	(void)loop_timer_set(srv->loop, &srv->deadline, deadline);
	loop_ready_bulk(srv->loop, &srv->drain);
}
