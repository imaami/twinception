/** @file HTTP/1.1 transport for request streams (src/stream.h), which carry
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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "buf.h"
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

static constexpr uint64_t NS = 1'000'000'000;

/* An event bit epoll never reports, for the timer finding a deadline passed. */
static constexpr uint32_t DEADLINE = 1u << 31;

/* And one that steps keep: TLS holds input that epoll no longer reports, the
 * rest of a record a read had no room for, or records a read that waited to
 * send left. */
static constexpr uint32_t HELD = 1u << 30;

/* The head starts the input buffer until the request is done, so header
 * values are kept as offsets into it, which survive the buffer moving; 0 is
 * absent, since the method is at 0.  Heads are at most 65535 bytes. */
struct field {
	uint16_t off;
	uint16_t len;
};

struct req {
	size_t       body;                /* Content-Length */
	uint32_t     head;                /* length of the request line and headers */
	struct field target;
	struct field host;
	struct field origin;
	struct field auth;
	struct field acrh;                /* Access-Control-Request-Headers */
	struct field app[STREAM_FIELDS];  /* those the application asked for */
	bool         length;              /* Content-Length seen */
	bool         chunked;             /* Transfer-Encoding: chunked */
	bool         http10;
	bool         close;
	bool         expect;              /* Expect: 100-continue */
	bool         pna;                 /* Access-Control-Request-Private-Network */
};

/* The connections from one client address: home's, which counts them, and
 * keeps the entry for reuse once none is left; the client loops count the
 * bytes of bodies they hold for it. */
struct client {
	struct list     spare;    /* on home's shelf, once unused */
	struct client  *next;     /* in its hash chain */
	uint64_t        addr[2];
	size_t          conns;
	_Atomic size_t  held;     /* body bytes counted in hub->bodies; 0 once
	                             unused */
};

/* Where a chunked body's decoding stands. */
enum chunk { CHUNK_SIZE, CHUNK_DATA, CHUNK_CRLF, CHUNK_TRAILER };

struct conn {
	struct msg         msg;       /* home's hand-over, then the give-back */
	struct list        hook;      /* on srv->idle, oldest first, or srv->busy;
	                                 or on home's shelf */
	struct loop_ref    ref;
	struct loop_step   step;
	struct loop_timer  timer;     /* for the deadline that applies */
	struct stream      stream;
	struct buf         in;
	struct buf         out;
	struct httpd      *srv;
	struct httpd_lane *lane;      /* home sets it: the loop's */
	struct client     *client;    /* home sets it */
	struct ssl_ctx_st *ctx;       /* home sets it: what a TLS listener's
	                                 client shakes hands by, a reference
	                                 until the handshake is done */
	struct ssl_st     *ssl;       /* TLS's end, once the client begins */
	uint64_t           since;     /* when the request, the handshake, or the
	                                 idle spell began */
	uint64_t           stall;     /* when the output last stalled; 0: it flows */
	size_t             drained;   /* bytes sent since then */
	size_t             earlier;   /* of the output pending, the bytes of
	                                 responses before the one under way */
	size_t             held;      /* body bytes counted in the budgets */
	size_t             got;       /* body bytes handed on */
	size_t             chunk;     /* bytes left of the chunk under way */
	struct req         req;
	uint32_t          *groups;    /* home sets them: a Unix socket's user's
	                                 groups, sorted, in storage kept with
	                                 the connection's */
	uint32_t           n_groups;
	uint32_t           uid;       /* and the user */
	uint32_t           room;      /* groups the groups' storage holds */
	int                fd;        /* home sets it */
	uint32_t           events;    /* reported since the last step, and DEADLINE */
	uint32_t           scan;      /* where to resume looking for the head's end */
	uint32_t           trailer;   /* trailer bytes so far */
	uint16_t           trailers;  /* trailer fields so far */
	uint8_t            chunking;  /* enum chunk */
	bool               parsed;
	bool               body_in;   /* the body is all in */
	bool               open;      /* the application has the stream */
	bool               waits;     /* and waits for something outside the loop */
	bool               replied;   /* the response head is queued */
	bool               done;      /* the response is complete */
	bool               chunks;    /* the response body goes in chunks */
	bool               cont;      /* 100 Continue sent */
	bool               closing;
	bool               eof;
	bool               doomed;    /* to close at its next step */
	bool               dead;
	bool               linger;    /* half-closed, discarding input */
	bool               writable;  /* to tell the application once output drains */
	bool               local;     /* home sets it: the client is on loopback,
	                                 or a Unix socket */
	bool               uds;       /* home sets it: over a Unix socket */
	bool               idle;      /* on srv->idle */
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
 * any loop starts, its atomic members initialized then. */
struct httpd_hub {
	_Atomic size_t     bodies;  /* body bytes held for all clients */
	struct loop       *home;    /* home's door sets it, before any client
	                               loop starts: connections go back there */
	size_t             n;
	struct httpd_lane  lane[];  /* the client loops' */
};

/* A client loop's server.  Each connection list runs from the least
 * recently active one. */
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
struct listener {
	struct loop_ref ref;
	char const     *path;   /* a Unix socket's file, which goes as the
	                           socket closes; nullptr: TCP */
	int             fd;
	uint16_t        ready;  /* bool: clients may wait */
	uint16_t        tls;    /* bool: clients shake hands (home->ctx) */
};

/* Home's door: the listeners, and the clients' addresses, connections and
 * descriptors.  Each connection is a client loop's from the moment it is
 * placed, until it is given back. */
struct httpd_home {
	struct shelf            spare;      /* connections kept for reuse */
	struct shelf            gone;       /* and addresses' entries */
	struct loop_step        accept;
	struct loop_timer       tick;       /* housekeeping */
	struct loop            *loop;
	struct httpd_hub       *hub;
	struct client         **clients;    /* CLIENT_SLOTS hash chains */
	_Atomic uint64_t const *calls;      /* jobs in flight */
	struct listener        *listeners;
	struct ssl_ctx_st      *ctx;        /* a reference: what TLS listeners'
	                                       clients shake hands by; nullptr:
	                                       there is none */
	size_t                  n_listeners;
	size_t                  turn;       /* the listener to accept from next */
	uint64_t                rng;        /* xorshift64 */
	size_t                  conns;      /* placed, and not given back */
	size_t                  fd_budget;  /* for conns + calls, from
	                                       RLIMIT_NOFILE */
	int                     spare_fd;   /* given up to accept a client */
	uint32_t                cap;        /* connections per client address;
	                                       0: none */
	uint32_t                refused;    /* turned away for descriptors since
	                                       the tick */
	uint32_t                capped;     /* turned away by the cap since then */
	uint16_t                open;       /* bool: accepting */
	uint16_t                anyone;     /* bool: clients that show no token
	                                       may call tools from beyond
	                                       loopback */
};

/* Home's door, for the connections given back. */
static thread_local struct httpd_home *door;

/* A run of response bytes. */
struct frag {
	char const *ptr;
	size_t      len;
};

#define FRAG(s) ((struct frag){ s, sizeof s - 1 })

/* Decimal @p n written backwards from @p end; returns its first digit. */
static char *
decimal (char   *end,
         size_t  n)
{
	do *--end = (char)('0' + n % 10);
	while (n /= 10);
	return end;
}

/* Hexadecimal @p n written backwards from @p end; returns its first digit. */
static char *
hex (char   *end,
     size_t  n)
{
	do *--end = "0123456789abcdef"[n % 16];
	while (n /= 16);
	return end;
}

/* The value of hexadecimal digit @p c, or 16 if it is none. */
static unsigned
hexdigit (unsigned char c)
{
	return (unsigned)(c - '0') < 10 ? (unsigned)(c - '0')
	     : (unsigned)((c | 0x20) - 'a') < 6 ? (unsigned)((c | 0x20) - 'a') + 10
	     : 16;
}

/* @p s seconds after @p since; never, if @p s is 0. */
static uint64_t
after (uint64_t since,
       uint32_t s)
{
	return s ? since + s * NS : UINT64_MAX;
}

static struct frag
reason (unsigned status)
{
	switch (status) {
	case 200: return FRAG("OK");
	case 202: return FRAG("Accepted");
	case 204: return FRAG("No Content");
	case 400: return FRAG("Bad Request");
	case 401: return FRAG("Unauthorized");
	case 403: return FRAG("Forbidden");
	case 404: return FRAG("Not Found");
	case 405: return FRAG("Method Not Allowed");
	case 408: return FRAG("Request Timeout");
	case 413: return FRAG("Content Too Large");
	case 415: return FRAG("Unsupported Media Type");
	case 417: return FRAG("Expectation Failed");
	case 431: return FRAG("Request Header Fields Too Large");
	case 500: return FRAG("Internal Server Error");
	case 501: return FRAG("Not Implemented");
	case 503: return FRAG("Service Unavailable");
	case 505: return FRAG("HTTP Version Not Supported");
	default:  return FRAG("Unknown");
	}
}

static bool
tchar (unsigned char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
	       (c >= 'A' && c <= 'Z') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

static bool
has_token (char const *list,
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
 * machine by loopback, whatever the port. */
static bool
loopback_host (char const *host)
{
#define P(s) { s, sizeof s - 1 }
	static struct {
		char const *str;
		size_t      len;
	} const names[] = { P("localhost"), P("127.0.0.1"), P("[::1]") };
#undef P
	for (size_t i = 0; i < ARRAY_SIZE(names); ++i) {
		if (strncasecmp(host, names[i].str, names[i].len))
			continue;
		char const *port = host + names[i].len;
		if (!*port)
			return true;
		size_t digits = *port == ':' ? strspn(++port, "0123456789") : 0;
		return digits && digits <= 5 && !port[digits];
	}
	return false;
}

/* Browsers on this machine. */
static bool
loopback_origin (char const *origin)
{
	static char const http[] = "http://";
	static char const https[] = "https://";
	size_t scheme = !strncmp(origin, http, sizeof http - 1) ? sizeof http - 1
	              : !strncmp(origin, https, sizeof https - 1) ? sizeof https - 1
	              : 0;
	return scheme && loopback_host(origin + scheme);
}

static bool
origin_allowed (struct httpd const *srv,
                char const         *origin)
{
	if (loopback_origin(origin))
		return true;
	for (char const *const *o = srv->origins; o && *o; ++o)
		if (!strcmp(*o, "*") || !strcmp(*o, origin))
			return true;
	return false;
}


/* Length of the head (request line + headers + empty line), 0 if partial. */
static size_t
head_length (char const *data,
             size_t      len,
             size_t     *scan)
{
	size_t i = *scan;
	char const *nl = nullptr;
	while (i < len && (nl = memchr(data + i, '\n', len - i))) {
		i = (size_t)(nl - data);
		size_t left = len - i;
		if (left >= 2 && data[i + 1] == '\n')
			return i + 2;
		if (left >= 3 && data[i + 1] == '\r' && data[i + 2] == '\n')
			return i + 3;
		if (left < 2 || (left < 3 && data[i + 1] == '\r'))
			break;
		++i;
		nl = nullptr;
	}
	*scan = nl ? i : len;
	return 0;
}

/* Cut the next line at its LF, dropping a preceding CR. */
static char *
next_line (char **cursor)
{
	char *line = *cursor;
	char *nl = strchr(line, '\n');
	*nl = '\0';
	if (nl > line && nl[-1] == '\r')
		nl[-1] = '\0';
	*cursor = nl + 1;
	return line;
}

/* Record header @p name, whose @p value is at offset @p off of the head and
 * @p len bytes long, if it is one that matters here or to @p app. */
static unsigned
header (struct req              *req,
        struct stream_app const *app,
        char const              *name,
        char const              *value,
        uint16_t                 off,
        uint16_t                 len,
        size_t                   max_body)
{
	struct field *f = !strcasecmp(name, "host") ? &req->host
	                : !strcasecmp(name, "origin") ? &req->origin
	                : !strcasecmp(name, "authorization") ? &req->auth
	                : !strcasecmp(name, "access-control-request-headers") ? &req->acrh
	                : nullptr;
	bool list = false;
	for (size_t i = 0; !f && i < app->n_fields; ++i)
		if (!strcasecmp(name, app->fields[i])) {
			f = &req->app[i];
			list = app->lists >> i & 1;
		}
	if (f) {
		/* A list's first line stands for it. */
		if (f->off && !list)
			return 400;
		if (!f->off)
			*f = (struct field){ off, len };
		return 0;
	}

	if (!strcasecmp(name, "content-length")) {
		size_t n = strspn(value, "0123456789");
		if (!n || value[n] || req->length)
			return 400;
		/* Nineteen digits fit, so the check sees the true value. */
		if (n > 19 || (req->body = strtoull(value, nullptr, 10)) > max_body)
			return 413;
		req->length = true;
	} else if (!strcasecmp(name, "transfer-encoding")) {
		/* Only chunked, the one coding HTTP/1.1 requires (RFC 9112). */
		if (req->chunked)
			return 400;
		if (strcasecmp(value, "chunked"))
			return 501;
		req->chunked = true;
	} else if (!strcasecmp(name, "connection")) {
		if (has_token(value, "close", sizeof "close" - 1))
			req->close = true;
		else if (req->http10 && has_token(value, "keep-alive",
		                                  sizeof "keep-alive" - 1))
			req->close = false;
	} else if (!strcasecmp(name, "expect")) {
		if (strcasecmp(value, "100-continue"))
			return 417;
		req->expect = !req->http10;
	} else if (!strcasecmp(name, "access-control-request-private-network")) {
		req->pna = !strcasecmp(value, "true");
	}
	return 0;
}

/* Parse a complete head in place, leaving the method at its start as a
 * string; nonzero is an HTTP error. */
static unsigned
parse_head (char                      *data,
            size_t                     len,
            struct req                *req,
            struct stream_app const   *app,
            struct httpd_limits const *lim)
{
	*req = (struct req){ .head = (uint32_t)len };
	if (memchr(data, '\0', len))
		return 400;

	char *cursor = data;
	char *line = next_line(&cursor);
	for (char const *p = line; *p; ++p)
		if ((unsigned char)*p < ' ' || *p == 0x7f)
			return 400;
	char *target = strchr(line, ' ');
	char *version = target ? strchr(target + 1, ' ') : nullptr;
	if (!version || target == line || version == target + 1 ||
	    strchr(version + 1, ' '))
		return 400;
	*target = '\0';
	*version++ = '\0';
	req->target = (struct field){ (uint16_t)(target + 1 - data),
	                              (uint16_t)(version - target - 2) };
	for (char const *p = line; *p; ++p)
		if (!tchar((unsigned char)*p))
			return 400;
	req->http10 = !strcmp(version, "HTTP/1.0");
	if (!req->http10 && strcmp(version, "HTTP/1.1"))
		return strncmp(version, "HTTP/", 5) ? 400 : 505;
	req->close = req->http10;

	for (uint32_t fields = 0; *(line = next_line(&cursor)); ) {
		if (++fields > lim->fields)
			return 431;
		char *colon = strchr(line, ':');
		if (!colon || colon == line)
			return 400;
		*colon = '\0';
		for (char const *p = line; *p; ++p)
			if (!tchar((unsigned char)*p))
				return 400;

		char *value = colon + 1 + strspn(colon + 1, " \t");
		char *end = value + strlen(value);
		while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';
		for (char const *p = value; *p; ++p)
			if (((unsigned char)*p < ' ' && *p != '\t') || *p == 0x7f)
				return 400;

		unsigned status = header(req, app, line, value, (uint16_t)(value - data),
		                         (uint16_t)(end - value), lim->body);
		if (status)
			return status;
	}

	/* Two framings would let a proxy and us see different requests, and
	 * an HTTP/1.0 sender of chunks frames them faultily (RFC 9112 6.1). */
	return req->chunked && (req->length || req->http10) ? 400 : 0;
}

/* The value of header @p f, empty with a null pointer if absent. */
static struct frag
field (struct conn const *conn,
       struct field       f)
{
	char const *head = (char const *)buf_peek(&conn->in).ptr;
	return f.off ? (struct frag){ head + f.off, f.len } : (struct frag){};
}

/* The address a client is known by: IPv4 (as IPv4-mapped IPv6), or for other
 * IPv6 the /64 prefix, which is what one host is usually given. */
static void
client_key (struct sockaddr_storage const *ss,
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
 * appended. */
static struct client **
client_slot (struct httpd_home *home,
             uint64_t const     key[2])
{
	uint64_t h = key[0] ^ key[1] * 0x9e3779b97f4a7c15;
	h = (h ^ h >> 30) * 0xbf58476d1ce4e5b9;
	h = (h ^ h >> 27) * 0x94d049bb133111eb;
	struct client **p = &home->clients[(h ^ h >> 31) & (CLIENT_SLOTS - 1)];
	while (*p && ((*p)->addr[0] != key[0] || (*p)->addr[1] != key[1]))
		p = &(*p)->next;
	return p;
}

/* Count a connection from client @p key, in an entry kept for reuse if
 * there is one; nullptr if out of memory. */
static struct client *
client_get (struct httpd_home *home,
            uint64_t const     key[2])
{
	struct client **p = client_slot(home, key);
	struct client *client = *p;
	if (!client) {
		struct list *spare = shelf_take(&home->gone);
		if (spare) {
			client = container_of(spare, struct client, spare);
		} else if ((client = malloc(sizeof *client))) {
			atomic_init(&client->held, 0);
		} else {
			return nullptr;
		}
		client->next = nullptr;
		client->addr[0] = key[0];
		client->addr[1] = key[1];
		client->conns = 0;
		*p = client;
	}
	++client->conns;
	return client;
}

/* A connection of @p client's has been given back; after the last, as the
 * loop that closed it touches the entry no more, home keeps it for reuse. */
static void
client_put (struct httpd_home *home,
            struct client     *client)
{
	if (--client->conns)
		return;
	*client_slot(home, client->addr) = client->next;
	shelf_put(&home->gone, &client->spare);
}

static bool
loopback (struct sockaddr_storage const *ss)
{
	if (ss->ss_family == AF_INET) {
		struct sockaddr_in const *in = (void const *)ss;
		return ntohl(in->sin_addr.s_addr) >> 24 == 127;
	}
	if (ss->ss_family == AF_INET6) {
		struct sockaddr_in6 const *in6 = (void const *)ss;
		struct in6_addr const *a = &in6->sin6_addr;
		return IN6_IS_ADDR_LOOPBACK(a) ||
		       (IN6_IS_ADDR_V4MAPPED(a) && a->s6_addr[12] == 127);
	}
	return false;
}

/* The connection has closed, and its events are dispatched: give it back to
 * home, which counts it no more. */
static void
conn_release (struct loop_ref *ref)
{
	struct conn *conn = container_of(ref, struct conn, ref);
	loop_post(conn->srv->hub->home, &conn->msg);
}

/* Give back the body bytes the connection holds in the budgets. */
static void
conn_unhold (struct conn *conn)
{
	if (!conn->held)
		return;
	atomic_fetch_sub_explicit(&conn->srv->hub->bodies, conn->held,
	                          memory_order_relaxed);
	atomic_fetch_sub_explicit(&conn->client->held, conn->held, memory_order_relaxed);
	conn->held = 0;
}

/* Take @p n bytes from @p budget, of which @p max may be taken; false if they
 * do not fit.  Every client loop takes from the same. */
static bool
budget_take (_Atomic size_t *budget,
             size_t          n,
             size_t          max)
{
	for (size_t used = atomic_load_explicit(budget, memory_order_relaxed);
	     n <= max - used; )
		if (atomic_compare_exchange_weak_explicit(budget, &used, used + n,
		                                          memory_order_relaxed,
		                                          memory_order_relaxed))
			return true;
	return false;
}

/* Count @p n more body bytes in the budgets; false if they do not fit. */
static bool
conn_hold (struct conn *conn,
           size_t       n)
{
	struct httpd *srv = conn->srv;
	if (!n)
		return true;
	if (!budget_take(&srv->hub->bodies, n, srv->lim.bodies))
		return false;
	if (!budget_take(&conn->client->held, n, srv->lim.client_bodies)) {
		atomic_fetch_sub_explicit(&srv->hub->bodies, n, memory_order_relaxed);
		return false;
	}
	conn->held += n;
	return true;
}

/* Take the stream back from the application, if it has it. */
static void
conn_cancel (struct conn *conn)
{
	if (!conn->open)
		return;
	conn->open = conn->waits = conn->writable = false;
	conn->srv->app->on_cancel(&conn->stream);
	conn->stream.user = nullptr;
}

/* Tell home how many connections idle, and when the least recently active
 * of them began to, as either changed. */
static void
srv_idle (struct httpd *srv)
{
	uint64_t since = list_is_empty(&srv->idle) ? UINT64_MAX
	               : container_of(srv->idle.next, struct conn, hook)->since;
	if (since != srv->idle_since) {
		srv->idle_since = since;
		atomic_store_explicit(&srv->lane->idle, since, memory_order_relaxed);
	}
	atomic_store_explicit(&srv->lane->idles, (uint32_t)srv->idles,
	                      memory_order_relaxed);
}

static void
conn_close (struct conn *conn)
{
	if (conn->dead)
		return;
	conn->dead = true;

	/* The last connection of a server that drains. */
	struct httpd *srv = conn->srv;
	loop_work(srv->loop, LOOP_CLOSES, 1);
	if (!--srv->conns && srv->draining)
		loop_exit(srv->loop);
	atomic_store_explicit(&srv->lane->metrics.open, srv->conns, memory_order_relaxed);
	conn_cancel(conn);
	conn_unhold(conn);
	list_del(&conn->hook);
	srv->idles -= conn->idle;
	srv_idle(srv);
	loop_unready(&conn->step);
	loop_timer_stop(srv->loop, &conn->timer);
	loop_release(srv->loop, &conn->ref, conn_release);
	/* A handshake begun and not done has failed. */
	if (conn->ctx && conn->ssl)
		hist_add(&srv->lane->metrics.unshaken, 1);
	tls_ctx_put(conn->ctx);
	tls_destroy(&conn->ssl);
	close(conn->fd);
	buf_fini(&conn->out);
	buf_fini(&conn->in);
}

/* File the connection at the young end of its list. */
static void
conn_park (struct conn *conn,
           bool         idle)
{
	struct httpd *srv = conn->srv;
	list_del(&conn->hook);
	list_append(idle ? &srv->idle : &srv->busy, &conn->hook);
	srv->idles += idle - conn->idle;
	conn->idle = idle;
	srv_idle(srv);
}

static size_t
frag_len (struct frag const *f,
          size_t             n)
{
	size_t len = 0;
	for (size_t i = 0; i < n; ++i)
		len += f[i].len;
	return len;
}

/* Whether the client's bytes wait, in the input or in TLS: a head begun. */
static bool
conn_input (struct conn const *conn)
{
	return buf_peek(&conn->in).len || (conn->ssl && tls_pending(conn->ssl));
}

/* Whether TLS waits for room to send what a read left it, such as the
 * answer to a key update: it reads nothing more meanwhile. */
static bool
conn_blocked (struct conn const *conn)
{
	return conn->ssl && tls_blocked(conn->ssl);
}

/* As recv(), or tls_recv() with TLS: the bytes, or a negative error. */
static ssize_t
conn_recv (struct conn *conn,
           void        *buf,
           size_t       len)
{
	if (conn->ssl)
		return tls_recv(conn->ssl, buf, len);
	ssize_t n = recv(conn->fd, buf, len, 0);
	return n < 0 ? -errno : n;
}

/* As send(), or tls_send() with TLS: the bytes, or a negative error. */
static ssize_t
conn_send (struct conn *conn,
           void const  *buf,
           size_t       len)
{
	if (conn->ssl)
		return tls_send(conn->ssl, buf, len);
	ssize_t n = send(conn->fd, buf, len, MSG_NOSIGNAL);
	return n < 0 ? -errno : n;
}

/* Queue the connection's step: a bulk one while it shakes hands. */
static void
conn_ready (struct conn *conn)
{
	if (conn->ctx)
		loop_ready_bulk(conn->srv->loop, &conn->step);
	else
		loop_ready(conn->srv->loop, &conn->step);
}

/* Close the connection at its next step: output must not close it at once,
 * as an application may be writing it. */
static void
conn_doom (struct conn *conn)
{
	conn->doomed = true;
	conn_ready(conn);
}

/* Queue @p n fragments; false, dooming the connection, if out of memory. */
static bool
conn_put (struct conn       *conn,
          struct frag const *f,
          size_t             n)
{
	if (conn->dead || conn->doomed)
		return false;
	size_t total = frag_len(f, n);
	if (!total)
		return true;
	struct span s;
	if (buf_reserve(&conn->out, total, SIZE_MAX, &s)) {
		conn_doom(conn);
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		if (f[i].len)
			memcpy(s.ptr, f[i].ptr, f[i].len);
		s.ptr += f[i].len;
	}
	buf_commit(&conn->out, total);
	return true;
}

/* Queue a response head: the status line, our headers, up to 8 @p extra
 * header lines and the framing of a body of @p length bytes, or of a body of
 * STREAM_OPEN, which to HTTP/1.1 clients goes in chunks and to others ends
 * with the connection.  A response that begins before its request's body is
 * all in, or to a request that asked so, or while the server drains, ends the
 * connection. */
static bool
conn_head (struct conn       *conn,
           unsigned           status,
           struct frag const *extra,
           size_t             n_extra,
           size_t             length)
{
	struct req const *req = &conn->req;
	bool open = length == STREAM_OPEN;
	conn->chunks = open && !req->http10;
	conn->closing |= req->close || conn->srv->draining || !conn->body_in ||
	                 (open && !conn->chunks);
	conn->replied = true;
	conn->earlier = buf_peek(&conn->out).len;

	char code[8];
	code[7] = ' ';
	char const *c = decimal(code + 7, status);
	char digits[20];
	char const *len = decimal(digits + sizeof digits, open ? 0 : length);

	struct frag f[12 + 8];
	size_t n = 0;
	f[n++] = FRAG("HTTP/1.1 ");
	f[n++] = (struct frag){ c, (size_t)(code + 8 - c) };
	f[n++] = reason(status);
	f[n++] = FRAG("\r\n");
	if (conn->closing)
		f[n++] = FRAG("Connection: close\r\n");
	if (conn->chunks) {
		f[n++] = FRAG("Transfer-Encoding: chunked\r\n");
	} else if (!open && status != 204) {
		f[n++] = FRAG("Content-Length: ");
		f[n++] = (struct frag){ len, (size_t)(digits + sizeof digits - len) };
		f[n++] = FRAG("\r\n");
	}
	if (req->origin.off) {
		f[n++] = FRAG("Access-Control-Allow-Origin: ");
		f[n++] = field(conn, req->origin);
		f[n++] = FRAG("\r\nAccess-Control-Allow-Credentials: true\r\n"
		              "Access-Control-Expose-Headers: Mcp-Session-Id, "
		              "WWW-Authenticate\r\n"
		              "Vary: Origin\r\n");
	}
	for (size_t i = 0; i < n_extra; ++i)
		f[n++] = extra[i];
	f[n++] = FRAG("\r\n");
	return conn_put(conn, f, n);
}

/* Queue a complete response of our own: @p body, which is nullptr for none
 * (and no Content-Type), after @p extra header lines; at most 8 of each. */
static void
conn_respond (struct conn       *conn,
              unsigned           status,
              struct frag const *extra,
              size_t             n_extra,
              struct frag const *body,
              size_t             n_body)
{
	struct frag head[9];
	size_t n = 0;
	if (body)
		head[n++] = FRAG("Content-Type: application/json\r\n");
	for (size_t i = 0; i < n_extra; ++i)
		head[n++] = extra[i];
	if (conn_head(conn, status, head, n, frag_len(body, n_body)))
		conn_put(conn, body, n_body);
	conn->done = true;
}

/* A JSON-RPC error saying @p message, or else the reason phrase. */
static void
conn_error (struct conn       *conn,
            unsigned           status,
            struct frag const *message,
            struct frag const *extra,
            size_t             n_extra)
{
	struct frag const body[] = {
		FRAG("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\""),
		message ? *message : reason(status),
		FRAG("\"}}"),
	};
	conn_respond(conn, status, extra, n_extra, body, ARRAY_SIZE(body));
}

/* Fail the request under way with @p status, taking the stream back from the
 * application; with a response begun already, the connection just closes. */
static void
conn_fail (struct conn *conn,
           unsigned     status)
{
	conn_cancel(conn);
	conn->closing = true;
	if (conn->replied)
		conn_doom(conn);
	else
		conn_error(conn, status, nullptr, nullptr, 0);
}

static void
conn_preflight (struct conn *conn)
{
	struct req const *req = &conn->req;
	if (!req->origin.off) {
		conn_respond(conn, 204, &FRAG("Allow: POST, OPTIONS\r\n"), 1, nullptr, 0);
		return;
	}

	struct frag const extra[] = {
		FRAG("Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n"
		     "Access-Control-Allow-Headers: "),
		req->acrh.off ? field(conn, req->acrh) : FRAG("Content-Type, Authorization"),
		FRAG("\r\nAccess-Control-Max-Age: 600\r\n"),
		req->pna ? FRAG("Access-Control-Allow-Private-Network: true\r\n")
		         : FRAG(""),
		FRAG("Vary: Access-Control-Request-Headers\r\n"),
	};
	conn_respond(conn, 204, extra, ARRAY_SIZE(extra), nullptr, 0);
}

static int
stream_head (struct stream *s,
             uint16_t       status,
             struct span    fields,
             size_t         length)
{
	struct conn *conn = container_of(s, struct conn, stream);
	struct frag const extra = { (char const *)fields.ptr, fields.len };
	if (!conn->open || conn->replied || !conn_head(conn, status, &extra, 1, length))
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
stream_data (struct stream *s,
             struct span    bytes)
{
	struct conn *conn = container_of(s, struct conn, stream);
	struct httpd *srv = conn->srv;
	if (!conn->open || !conn->replied)
		return EPIPE;
	if (!bytes.len)
		return 0;
	if ((conn->earlier || conn->writable) &&
	    buf_peek(&conn->out).len + bytes.len > srv->lim.out_max) {
		conn_doom(conn);
		return EPIPE;
	}

	char digits[20];
	char const *size = hex(digits + sizeof digits - 2, bytes.len);
	digits[sizeof digits - 2] = '\r';
	digits[sizeof digits - 1] = '\n';
	struct frag const f[] = {
		{ size, (size_t)(digits + sizeof digits - size) },
		{ (char const *)bytes.ptr, bytes.len },
		FRAG("\r\n"),
	};
	bool ok = conn->chunks ? conn_put(conn, f, ARRAY_SIZE(f))
	                       : conn_put(conn, &f[1], 1);
	if (!ok)
		return EPIPE;
	loop_work(srv->loop, LOOP_SEND, bytes.len);
	loop_ready(srv->loop, &conn->step);
	if (buf_peek(&conn->out).len < srv->lim.out_pause)
		return 0;
	conn->writable = true;
	return EAGAIN;
}

static void
stream_end (struct stream *s,
            int            error)
{
	struct conn *conn = container_of(s, struct conn, stream);
	if (!conn->open)
		return;
	conn->open = conn->waits = conn->writable = false;
	conn->stream.user = nullptr;
	if (!conn->replied) {
		conn_error(conn, 500, nullptr, nullptr, 0);
	} else if (error) {
		/* The client sees the body cut short as the connection closes. */
		conn->closing = true;
		conn_doom(conn);
	} else if (conn->chunks) {
		conn_put(conn, &FRAG("0\r\n\r\n"), 1);
	}
	conn->done = true;
	loop_ready(conn->srv->loop, &conn->step);
}

static void
stream_waiting (struct stream *s)
{
	struct conn *conn = container_of(s, struct conn, stream);
	conn->waits = conn->open;
	loop_ready(conn->srv->loop, &conn->step);
}

static struct span
stream_field (struct stream const *s,
              size_t               i)
{
	struct conn const *conn = container_of(s, struct conn const, stream);
	struct frag f = field(conn, conn->req.app[i]);
	return (struct span){ (unsigned char *)(uintptr_t)f.ptr, f.len };
}

static struct stream_ops const stream_ops = {
	.head = stream_head,
	.data = stream_data,
	.end = stream_end,
	.waiting = stream_waiting,
	.field = stream_field,
};

/* Who the client of a freshly parsed head is, by its bearer token, or by
 * how it connects if it shows none, and what it may do, into its stream;
 * false if it may not connect (src/policy.h).  Credentials of another
 * scheme are none.  A client on loopback that names another host may be a
 * page whose name was rebound to 127.0.0.1, whose GET carries no Origin: it
 * connects as a remote one. */
static bool
conn_grant (struct conn *conn)
{
	static char const scheme[] = "Bearer";
	size_t n = sizeof scheme - 1;
	struct frag auth = field(conn, conn->req.auth);
	bool bearer = auth.ptr && auth.len >= n && !strncasecmp(auth.ptr, scheme, n) &&
	              (auth.len == n || auth.ptr[n] == ' ');
	size_t skip = bearer && auth.len > n ? n + 1 : n;
	char const *host = field(conn, conn->req.host).ptr;
	struct policy_peer const peer = {
		.groups = conn->groups,
		.n_groups = conn->n_groups,
		.uid = conn->uid
	};
	struct policy_grant g = {};
	bool ok = policy_grant(conn->srv->policy, bearer ? auth.ptr + skip : nullptr,
	                       bearer ? auth.len - skip : 0,
	                       conn->uds ? POLICY_UNIX
	                       : conn->local && (!host || loopback_host(host))
	                       ? POLICY_LOOPBACK : POLICY_REMOTE,
	                       conn->uds ? &peer : nullptr, &g);
	conn->stream.principal = g.principal;
	conn->stream.tools = g.tools;
	conn->stream.trusted = g.metrics;
	return ok;
}

/* Check a freshly parsed head against the Origin defence, the policy and the
 * body budgets, and count its body in those; nonzero is an HTTP error.  So a
 * client that may not send a body holds none of the budgets.  A chunked body
 * is counted as it comes. */
static unsigned
conn_admit (struct conn *conn)
{
	struct httpd *srv = conn->srv;
	struct req const *req = &conn->req;
	char const *head = (char const *)buf_peek(&conn->in).ptr;
	char const *origin = field(conn, req->origin).ptr;
	if (origin && !origin_allowed(srv, origin))
		return 403; /* DNS rebinding defence */
	if (strcmp(head, "OPTIONS") && !conn_grant(conn))
		return 401;
	return conn_hold(conn, req->body) ? 0 : 503;
}

/*
 * Decode chunks (RFC 9112 7.1) from the @p len bytes at @p raw that follow
 * the head, moving their data down to @p raw.  Sets @p *used to the bytes
 * taken and @p *out to the data bytes now at @p raw, and conn->body_in once
 * the last chunk and the trailer section are in; returns 0 or an HTTP error.
 * Lines end in CRLF only, as a lenient reading of framing is what request
 * smuggling exploits.  Chunk extensions and trailer fields are ignored.
 */
static unsigned
conn_dechunk (struct conn   *conn,
              unsigned char *raw,
              size_t         len,
              size_t        *used,
              size_t        *out)
{
	struct httpd_limits const *lim = &conn->srv->lim;
	size_t rd = 0;
	size_t wr = 0;
	unsigned status = 0;
	while (!status && rd < len && !conn->body_in) {
		if (conn->chunking == CHUNK_DATA) {
			size_t n = len - rd < conn->chunk ? len - rd : conn->chunk;
			memmove(raw + wr, raw + rd, n);
			wr += n;
			rd += n;
			if (!(conn->chunk -= n))
				conn->chunking = CHUNK_CRLF;
			continue;
		}
		if (conn->chunking == CHUNK_CRLF) {
			if (len - rd < 2)
				break;
			if (raw[rd] != '\r' || raw[rd + 1] != '\n')
				status = 400;
			rd += 2;
			conn->chunking = CHUNK_SIZE;
			continue;
		}

		size_t max = conn->chunking == CHUNK_SIZE ? CHUNK_LINE : lim->head;
		unsigned char *line = raw + rd;
		unsigned char *nl = memchr(line, '\n', len - rd < max ? len - rd : max);
		if (!nl) {
			status = len - rd >= max ? conn->chunking == CHUNK_SIZE ? 400 : 431 : 0;
			break;
		}
		size_t n = (size_t)(nl - line);
		rd += n + 1;
		if (!n || line[n - 1] != '\r') {
			status = 400;
		} else if (conn->chunking == CHUNK_TRAILER) {
			/* A trailer section counts against the head's limits, and
			 * each of its lines must be a field: a request that follows
			 * a missing empty line is no trailer. */
			size_t name = 0;
			while (name < n - 1 && tchar(line[name]))
				++name;
			conn->body_in = n == 1;
			if (!conn->body_in && (!name || line[name] != ':'))
				status = 400;
			else if ((conn->trailer += (uint32_t)n + 1) > lim->head ||
			         (!conn->body_in && ++conn->trailers > lim->fields))
				status = 431;
		} else {
			size_t size = 0;
			size_t i = 0;
			bool bad = false;
			for (unsigned d; i < n - 1 && (d = hexdigit(line[i])) < 16; ++i) {
				bad |= size > SIZE_MAX >> 4;
				size = size << 4 | d;
			}
			/* An extension follows the size after optional white space;
			 * only the line's end may hold a control character. */
			size_t ext = i + strspn((char const *)line + i, " \t");
			bad |= !i || (ext < n - 1 && line[ext] != ';');
			for (size_t k = ext; !bad && k < n - 1; ++k)
				bad = (line[k] < ' ' && line[k] != '\t') || line[k] == 0x7f;
			if (bad)
				status = 400;
			else if (size > lim->body - conn->got - wr)
				status = 413;
			conn->chunk = size;
			conn->chunking = size ? CHUNK_DATA : CHUNK_TRAILER;
		}
	}
	*used = rd;
	*out = wr;
	return status;
}

/* Take in what has come of the body, within the step's share: hand it to the
 * application, if it has the stream, and cut it out of the input.  Returns 0
 * or an HTTP error, and sets @p *more if the share ran out first. */
static unsigned
conn_body (struct conn *conn,
           bool        *more)
{
	struct loop *loop = conn->srv->loop;
	struct req const *req = &conn->req;
	struct span in = buf_peek(&conn->in);
	unsigned char *raw = in.ptr + req->head;
	size_t avail = in.len - req->head;
	size_t room = loop_room(loop, LOOP_BODY);
	size_t len = avail < room ? avail : room;
	size_t used;
	size_t out;
	if (req->chunked) {
		unsigned status = conn_dechunk(conn, raw, len, &used, &out);
		if (status)
			return status;
		if (!conn_hold(conn, out))
			return 503;
	} else {
		used = out = len < req->body - conn->got ? len : req->body - conn->got;
		conn->body_in = conn->got + out == req->body;
	}
	loop_work(loop, LOOP_BODY, used);
	conn->got += out;
	*more = len < avail && !conn->body_in;
	if (conn->open && (out || conn->body_in))
		conn->srv->app->on_body(&conn->stream, (struct span){ raw, out },
		                        conn->body_in);
	buf_cut(&conn->in, req->head, used);
	return 0;
}

/* Begin a request: CORS preflights are ours, the rest the application's. */
static void
conn_begin (struct conn *conn)
{
	struct httpd *srv = conn->srv;
	struct req const *req = &conn->req;
	char const *method = (char const *)buf_peek(&conn->in).ptr;
	conn->body_in = !req->chunked && !req->body;
	if (!strcmp(method, "OPTIONS")) {
		conn_preflight(conn);
		return;
	}

	/* The application's bodies would trail a HEAD response; end the stream. */
	conn->closing |= !strcmp(method, "HEAD");
	conn->open = true;
	conn->stream.user = nullptr;
	struct stream_app const *app = srv->app;
	struct frag target = field(conn, req->target);
	app->on_head(app->ctx, &conn->stream,
	             (struct span){ (unsigned char *)(uintptr_t)method, strlen(method) },
	             (struct span){ (unsigned char *)(uintptr_t)target.ptr, target.len },
	             req->chunked ? STREAM_OPEN : req->body);
	if (conn->open && conn->body_in)
		app->on_body(&conn->stream, (struct span){}, true);
}

/* What the connection's step does next. */
enum next {
	NEXT_MORE,   /* go on */
	NEXT_INPUT,  /* wait for input */
	NEXT_YIELD,  /* wait for the next step */
	NEXT_WAIT,   /* wait for the application */
	NEXT_END,    /* no more requests */
};

/* Advance the request under way, or begin the next unless the step has no
 * share left for that or has run to @p end. */
static enum next
conn_next (struct conn *conn,
           uint64_t     end)
{
	struct httpd *srv = conn->srv;
	struct req *req = &conn->req;
	if (conn->done && conn->body_in) {
		buf_consume(&conn->in, req->head);
		conn_unhold(conn);
		*req = (struct req){};
		conn->since = srv->loop->now;
		conn->got = conn->chunk = conn->scan = conn->trailer = conn->trailers = 0;
		conn->chunking = CHUNK_SIZE;
		conn->parsed = conn->body_in = conn->replied = conn->done = false;
		conn->chunks = conn->cont = false;
	}
	if (conn->done || ((conn->closing || srv->draining) && !conn->parsed))
		return NEXT_END;

	struct loop *loop = srv->loop;
	if (!conn->parsed && (!loop_room(loop, LOOP_REQS) ||
	                      (loop->work[LOOP_REQS] && loop_clock() >= end)))
		return NEXT_YIELD;
	if (!conn->parsed) {
		/* A head that does not end within lim.head bytes is too large. */
		struct span in = buf_peek(&conn->in);
		size_t scan = conn->scan;
		size_t head = head_length((char const *)in.ptr, in.len < srv->lim.head
		                                                ? in.len : srv->lim.head,
		                          &scan);
		conn->scan = (uint32_t)scan;
		unsigned status = head ? parse_head((char *)in.ptr, head, req, srv->app,
		                                    &srv->lim)
		                : in.len >= srv->lim.head ? 431 : 0;
		if (!head && !status)
			return NEXT_INPUT;
		loop_work(loop, LOOP_REQS, 1);
		if (!status)
			status = conn_admit(conn);
		if (status) {
			/* Echo no Origin unless it passed the check: only a 401
			 * follows that. */
			if (status != 401)
				*req = (struct req){};
			conn->closing = true;
			conn_error(conn, status, nullptr, status == 401
			           ? &FRAG("WWW-Authenticate: Bearer\r\n")
			           : &FRAG("Retry-After: 1\r\n"),
			           status == 401 || status == 503);
			return NEXT_END;
		}
		conn->parsed = true;
		conn->since = loop->now;
		conn_begin(conn);
		return NEXT_MORE;
	}

	if (!conn->body_in) {
		bool more = false;
		unsigned status = conn_body(conn, &more);
		if (status) {
			conn_fail(conn, status);
			return NEXT_END;
		}
		if (conn->body_in || conn->done)
			return NEXT_MORE;
		if (more)
			return NEXT_YIELD;
		if (req->expect && !conn->cont) {
			static char const cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
			conn_put(conn, &FRAG(cont), 1);
			conn->cont = true;
		}
		return NEXT_INPUT;
	}
	return conn->done ? NEXT_MORE : NEXT_WAIT;
}

#undef FRAG

/* Send what is queued, within the step's share; false if the connection
 * closed.  The write-stall clock starts when a send would block and restarts
 * only once WRITE_PROGRESS bytes have gone out, so a trickle cannot hold it;
 * once the output has drained, the request's own clock restarts, as it was
 * held up meanwhile.  TLS sends a record a call, whole, of TLS_RECORD bytes
 * or the output's last, so that a call after one that waited is never
 * shorter, and none sends records unseen; with the output sent, what a read
 * left it to send, such as the answer to a key update, whose wait for room
 * stalls as the output's does. */
static bool
conn_flush (struct conn *conn)
{
	struct loop *loop = conn->srv->loop;
	while (!conn->dead) {
		struct span out = buf_peek(&conn->out);
		if (!out.len) {
			buf_fini(&conn->out);
			enum tls_shake shake = conn_blocked(conn) ? tls_shake(conn->ssl)
			                                          : TLS_SHAKEN;
			if (shake == TLS_FAILED) {
				conn_close(conn);
				return false;
			}
			if (shake == TLS_WRITE) {
				if (!conn->stall)
					conn->stall = loop->now;
				return true;
			}
			if (conn->stall)
				conn->since = loop->now;
			conn->stall = 0;
			return true;
		}
		size_t len = conn->ssl && out.len > TLS_RECORD ? TLS_RECORD : out.len;
		size_t room = loop_room(loop, LOOP_SEND);
		if (!room || (conn->ssl && room < len))
			return true;
		ssize_t n = conn_send(conn, out.ptr, conn->ssl || len < room ? len : room);
		if (n >= 0) {
			buf_consume(&conn->out, (size_t)n);
			conn->drained += (size_t)n;
			conn->earlier -= conn->earlier < (size_t)n ? conn->earlier : (size_t)n;
			loop_work(loop, LOOP_SEND, (uint64_t)n);
		} else if (n == -EAGAIN || n == -EWOULDBLOCK) {
			if (!conn->stall || conn->drained >= WRITE_PROGRESS) {
				conn->stall = loop->now;
				conn->drained = 0;
			}
			return true;
		} else if (n != -EINTR) {
			conn_close(conn);
		}
	}
	return false;
}

/*
 * When the deadline that applies runs out, UINT64_MAX if none does: lingering,
 * the idle deadline until a TLS client begins its handshake, then the head's
 * until it is done, the write stall's while output is stuck, none while the
 * application works, the idle or the head deadline until a head is parsed,
 * then the body's minimum rate.
 */
static uint64_t
conn_deadline (struct conn const *conn)
{
	struct httpd_limits const *lim = &conn->srv->lim;
	if (conn->linger)
		return conn->since + LINGER_S * NS;
	if (conn->ctx)
		return after(conn->since, conn->ssl ? lim->head_time : lim->idle_time);
	if (conn->stall)
		return after(conn->stall, lim->write_time);
	if (conn->body_in && conn->parsed)
		return UINT64_MAX;
	if (!conn->parsed)
		return after(conn->since, conn_input(conn) ? lim->head_time
		                                           : lim->idle_time);

	/* From its start, once the grace is up, the body must have come at
	 * body_rate: the bytes so far pay for so much time. */
	size_t got = conn->got;
	size_t secs = lim->body_rate ? got / lim->body_rate : 0;
	if (!lim->body_rate || secs > UINT32_MAX)
		return UINT64_MAX;
	uint64_t paid = secs * NS + got % lim->body_rate * NS / lim->body_rate;
	uint64_t grace = lim->body_grace * NS;
	return conn->since + (paid > grace ? paid : grace);
}

/* Bring the timer forward to the deadline, if that is sooner. */
static void
conn_arm (struct conn *conn)
{
	uint64_t due = conn_deadline(conn);
	if (due < conn->timer.due)
		(void)loop_timer_set(conn->srv->loop, &conn->timer, due);
}

/* A deadline ran out: a slow head or body is answered 408 and the connection
 * ends once that is sent; otherwise it closes at once. */
static void
conn_expire (struct conn *conn)
{
	if (conn->linger || conn->stall || !buf_peek(&conn->in).len)
		conn_close(conn);
	else
		conn_fail(conn, 408);
}

/* Discard what a lingering client still sends, a read a step; close once it
 * has closed too. */
static void
conn_drain (struct conn *conn)
{
	ssize_t n = recv(conn->fd, nullptr, LOOP_STEP_RECV, MSG_TRUNC | MSG_DONTWAIT);
	if (!n || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
		conn_close(conn);
	else if (n > 0)
		loop_work(conn->srv->loop, LOOP_RECV, (uint64_t)n);
}

/* The last response is sent: linger, unless the client has closed already,
 * when what it sent is discarded and the connection closed.  A stream the
 * application still has is cancelled.  TLS says it ends, and is gone: what
 * the client sends from then on is only discarded. */
static void
conn_end (struct conn *conn)
{
	struct httpd *srv = conn->srv;
	conn_cancel(conn);
	tls_close(&conn->ssl);
	if (conn->eof || shutdown(conn->fd, SHUT_WR) ||
	    loop_modify(srv->loop, &conn->ref, EPOLLIN)) {
		/* Input left unread would turn the close into a reset. */
		(void)recv(conn->fd, nullptr, srv->in_max, MSG_TRUNC | MSG_DONTWAIT);
		conn_close(conn);
		return;
	}
	conn->linger = true;
	conn->since = srv->loop->now;
	conn_unhold(conn);
	buf_fini(&conn->in);
	loop_unready(&conn->step);
	conn_park(conn, false);
	conn_arm(conn);
}

/*
 * One step: requests, as far as the step's share and STEP_NS allow, then one
 * send of their responses.  Then wait for input once no complete request is
 * left, for the application while it works on one, for output space once
 * lim.out_pause bytes are pending, and for the next turn otherwise.
 */
static void
conn_pump (struct conn *conn)
{
	struct httpd *srv = conn->srv;
	struct loop *loop = srv->loop;
	enum next next = NEXT_MORE;
	uint64_t end = loop_clock() + STEP_NS;
	while (next == NEXT_MORE && !conn->dead && !conn->doomed &&
	       buf_peek(&conn->out).len < srv->lim.out_pause)
		next = conn_next(conn, end);
	if (conn->doomed) {
		conn_close(conn);
		return;
	}
	if (!conn_flush(conn))
		return;

	size_t pending = buf_peek(&conn->out).len;
	bool again = false;
	if (conn->writable && pending < srv->lim.out_pause / 2) {
		conn->writable = false;
		if (conn->open)
			srv->app->on_writable(&conn->stream);
		if (conn->doomed) {
			conn_close(conn);
			return;
		}
		pending = buf_peek(&conn->out).len;
		/* A response ended there leaves its request to the next step. */
		again = conn->done;
	}
	bool starved = next == NEXT_INPUT;
	bool blocked = conn_blocked(conn);
	if (!pending && !blocked && (next == NEXT_END || (starved && conn->eof))) {
		conn_end(conn);
		return;
	}

	/* Input that ended stays readable: watching it then would spin, as it
	 * would while TLS waits to send.  Input TLS holds, which epoll does not
	 * report, is read in the next step. */
	bool paused = pending >= srv->lim.out_pause || conn->closing;
	bool reads = starved && !paused && !blocked && !conn->eof && !conn->waits;
	bool held = reads && conn->events & HELD;
	uint32_t events = pending || blocked ? EPOLLOUT : 0;
	if (conn->waits)
		events |= EPOLLRDHUP;
	else if (reads)
		events |= EPOLLIN;
	if (loop_modify(loop, &conn->ref, events)) {
		conn_close(conn);
		return;
	}
	if (held)
		conn->events |= EPOLLIN;
	bool waiting = !again && !held &&
	               (next == NEXT_INPUT || next == NEXT_WAIT || paused || blocked);
	if (waiting)
		loop_unready(&conn->step);
	else
		loop_ready(loop, &conn->step);

	/* An idle connection keeps no storage, and under critical memory
	 * pressure one that waits keeps no more than its bytes need. */
	bool idle = starved && !pending && !blocked && !conn_input(conn);
	if (idle)
		buf_fini(&conn->in);
	else if (waiting && atomic_load_explicit(&buf_pressure, memory_order_relaxed) >=
	                    BUF_CRITICAL)
		loop_work(loop, LOOP_COPY, buf_trim(&conn->in) + buf_trim(&conn->out));

	conn_park(conn, idle);
	conn_arm(conn);
}

/* One recv, or one TLS record, straight into the input's free tail.  A
 * head's clock starts with its first byte, which may be in TLS still. */
static void
conn_read (struct conn *conn)
{
	/* Input is read only while no complete request is buffered, and a
	 * body is cut out as it comes, so the limit is never reached and an
	 * empty span means out of memory. */
	struct loop *loop = conn->srv->loop;
	bool first = !conn->parsed && !conn_input(conn);
	struct span s;
	if (buf_reserve(&conn->in, READ_MIN, conn->srv->in_max, &s)) {
		conn_close(conn);
		return;
	}
	size_t room = loop_room(loop, LOOP_RECV);
	size_t len = s.len < room ? s.len : room;
	ssize_t n = conn_recv(conn, s.ptr, len);
	conn->events &= ~HELD;
	if (n > 0) {
		buf_commit(&conn->in, (size_t)n);
		loop_work(loop, LOOP_RECV, (uint64_t)n);
	} else if (!n) {
		conn->eof = true;
	} else if (n != -EAGAIN && n != -EWOULDBLOCK && n != -EINTR) {
		conn_close(conn);
		return;
	}
	if (first && conn_input(conn))
		conn->since = loop->now;
	/* After a read that gave bytes, or that waits to send, what TLS holds
	 * is read again; a part of a record waits for the rest. */
	if (conn->ssl && (n > 0 || conn_blocked(conn)) && tls_pending(conn->ssl))
		conn->events |= HELD;
}

/* A flight of the handshake, which a TLS listener's client begins with its
 * first bytes, from then on within the head's deadline; once it is done,
 * the connection serves as any other.  Each flight's time is its CPU's,
 * mostly.  A step woken by the client's FIN alone counts none. */
static void
conn_shake (struct conn *conn)
{
	struct httpd *srv = conn->srv;
	struct loop *loop = srv->loop;
	struct metrics_conns *m = &srv->lane->metrics;
	if (!conn->ssl) {
		/* A client that closes without a word, such as a probe of the
		 * port, began no handshake. */
		char c;
		ssize_t peek = recv(conn->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
		if (peek < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			return;
		if (peek <= 0 || !(conn->ssl = tls_create(conn->ctx, conn->fd))) {
			conn_close(conn);
			return;
		}
		conn->since = loop->now;
		conn_park(conn, false);
	}
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
		conn_pump(conn);
		return;
	}
	if (shake == TLS_FAILED ||
	    loop_modify(loop, &conn->ref, shake == TLS_WRITE ? EPOLLOUT : EPOLLIN)) {
		conn_close(conn);
		return;
	}
	conn_arm(conn);
}

static void
conn_step (struct loop      *loop,
           struct loop_step *step)
{
	struct conn *conn = container_of(step, struct conn, step);
	uint32_t events = conn->events;
	conn->events &= HELD;

	/* A client that goes away while the application waits cancels it. */
	if (conn->doomed || (events & (EPOLLERR | EPOLLHUP)) ||
	    (conn->waits && (events & EPOLLRDHUP))) {
		conn_close(conn);
		return;
	}
	/* A handshake ends as a head would, but with no answer. */
	if (conn->ctx) {
		if (conn->srv->draining ||
		    ((events & DEADLINE) && conn_deadline(conn) <= loop->now))
			conn_close(conn);
		else if (events & (EPOLLIN | EPOLLOUT))
			conn_shake(conn);
		return;
	}
	if ((events & DEADLINE) && conn_deadline(conn) <= loop->now) {
		conn_expire(conn);
	} else if (conn->linger) {
		conn_drain(conn);
		return;
	} else if (events & EPOLLIN) {
		conn_read(conn);
	}
	if (!conn->dead)
		conn_pump(conn);
}

static void
conn_event (struct loop     *loop,
            struct loop_ref *ref,
            uint32_t         events)
{
	(void)loop;
	struct conn *conn = ref->owner;
	conn->events |= events;
	conn_ready(conn);
}

/* Fired early, re-arm for the deadline as it now stands; else let the step
 * act on it. */
static void
conn_timer (struct loop       *loop,
            struct loop_timer *timer)
{
	struct conn *conn = container_of(timer, struct conn, timer);
	uint64_t due = conn_deadline(conn);
	if (due > loop->now) {
		(void)loop_timer_set(loop, timer, due);
		return;
	}
	conn->events |= DEADLINE;
	conn_ready(conn);
}

/* Watch the listeners for clients, or for none if @p events is 0. */
static void
home_watch (struct httpd_home *home,
            uint32_t           events)
{
	for (size_t i = 0; i < home->n_listeners; ++i)
		(void)loop_modify(home->loop, &home->listeners[i].ref, events);
}

/* Free the storage of a connection, which may be a spare. */
static void
conn_free (struct conn *conn)
{
	free(conn->groups);
	free(conn);
}

/* Home has a connection back: it counts it no more, and keeps its storage
 * for the next client.  A descriptor came free, so the spare may be had again.
 * Once home's loop has stopped, its door is gone, and it is freed. */
static void
conn_back (struct loop *loop,
           struct msg  *msg)
{
	struct conn *conn = container_of(msg, struct conn, msg);
	struct httpd_home *home = loop->exit ? nullptr : door;
	if (!home) {
		conn_free(conn);
		return;
	}
	client_put(home, conn->client);
	--conn->lane->conns;
	--home->conns;
	shelf_put(&home->spare, &conn->hook);
	if (home->spare_fd < 0 && home->open &&
	    (home->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC)) >= 0)
		home_watch(home, EPOLLIN);
}

/* Set up a connection home placed on the loop, or close it at once while the
 * server drains, giving it back to home. */
static void
conn_setup (struct httpd *srv,
            struct conn  *conn)
{
	struct loop *loop = srv->loop;
	struct httpd_lane *lane = conn->lane;
	struct client *client = conn->client;
	uint32_t *groups = conn->groups;
	uint32_t n_groups = conn->n_groups;
	uint32_t uid = conn->uid;
	uint32_t room = conn->room;
	struct ssl_ctx_st *ctx = conn->ctx;
	int fd = conn->fd;
	bool local = conn->local;
	bool uds = conn->uds;
	*conn = (struct conn){
		.msg.fn = conn_back,
		.step.fn = conn_step,
		.timer.fn = conn_timer,
		.stream = {
			.ops = &stream_ops,
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
	    !loop_timer_set(loop, &conn->timer, after(conn->since, srv->lim.idle_time)) &&
	    !loop_watch(loop, &conn->ref, fd, EPOLLIN, conn_event, conn)) {
		list_init(&conn->hook);
		atomic_store_explicit(&lane->metrics.open, ++srv->conns,
		                      memory_order_relaxed);
		conn_park(conn, true);
		return;
	}
	loop_timer_stop(loop, &conn->timer);
	tls_ctx_put(ctx);
	close(fd);
	loop_post(srv->hub->home, &conn->msg);
}

/* The client loop takes a connection home placed on it, for a step to set
 * up; once the loop has stopped, its server and home's door may be gone, and
 * it is only closed and freed. */
static void
conn_take (struct loop *loop,
           struct msg  *msg)
{
	struct conn *conn = container_of(msg, struct conn, msg);
	if (loop->exit) {
		tls_ctx_put(conn->ctx);
		close(conn->fd);
		conn_free(conn);
		return;
	}
	struct httpd *srv = conn->lane->srv;
	list_append(&srv->taken, &conn->hook);
	loop_ready(loop, &srv->take);
}

/* Set up the connections home handed over, as many a step as its share of
 * accepts allows. */
static void
srv_take (struct loop      *loop,
          struct loop_step *step)
{
	struct httpd *srv = container_of(step, struct httpd, take);
	for (; !list_is_empty(&srv->taken); loop_work(loop, LOOP_ACCEPTS, 1)) {
		if (!loop_room(loop, LOOP_ACCEPTS)) {
			loop_ready(loop, step);
			return;
		}
		struct conn *conn = container_of(srv->taken.next, struct conn, hook);
		list_del(&conn->hook);
		conn_setup(srv, conn);
	}
}

/* On a client loop: take the policy home stored last, and let go of the
 * one before; once the loop has stopped, only let go of it. */
static void
srv_policy (struct loop *loop,
            struct msg  *msg)
{
	struct httpd_lane *lane = container_of(msg, struct httpd_lane, handover);
	struct policy *policy = atomic_exchange(&lane->policy, nullptr);
	struct httpd *srv = loop->exit ? nullptr : lane->srv;
	if (srv && policy) {
		struct policy *old = srv->policy;
		srv->policy = policy;
		policy = old;
	}
	policy_put(policy);
}

/* Home asks the loop to close idle connections, as descriptors run short. */
static void
srv_evicts (struct loop *loop,
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
 * a step as its share allows; once none is idle, none is left to close. */
static void
srv_evict (struct loop      *loop,
           struct loop_step *step)
{
	struct httpd *srv = container_of(step, struct httpd, evict);
	for (; srv->evicting && !list_is_empty(&srv->idle); --srv->evicting) {
		if (!loop_room(loop, LOOP_CLOSES)) {
			loop_ready_bulk(loop, step);
			return;
		}
		conn_close(container_of(srv->idle.next, struct conn, hook));
	}
	srv->evicting = 0;
}

/* Draining: close the idle connections, and from the deadline on every one
 * left, as many a step as its share allows. */
static void
srv_drain (struct loop      *loop,
           struct loop_step *step)
{
	struct httpd *srv = container_of(step, struct httpd, drain);
	struct list *rest = loop->now >= srv->drain_by ? &srv->busy : &srv->idle;
	while (loop_room(loop, LOOP_CLOSES)) {
		struct list *list = list_is_empty(&srv->idle) ? rest : &srv->idle;
		if (list_is_empty(list))
			return;
		conn_close(container_of(list->next, struct conn, hook));
	}
	loop_ready_bulk(loop, step);
}

static void
srv_deadline (struct loop       *loop,
              struct loop_timer *timer)
{
	loop_ready_bulk(loop, &container_of(timer, struct httpd, deadline)->drain);
}

/* Whether clients hold all the descriptors they may: one for each connection,
 * and one more for each job in flight, for its upstream connection. */
static bool
home_full (struct httpd_home const *home)
{
	return home->conns + hist_get(home->calls) >= home->fd_budget;
}

/* Leave FD_RESERVE descriptors and the two each client loop holds, its
 * epoll's and its inbox's, or half of fewer, to all but clients. */
static void
home_budget (struct httpd_home *home)
{
	struct rlimit nofile = { .rlim_cur = RLIM_INFINITY };
	(void)getrlimit(RLIMIT_NOFILE, &nofile);
	rlim_t fds = nofile.rlim_cur;
	rlim_t reserve = FD_RESERVE + 2 * home->hub->n;
	home->fd_budget = (size_t)(fds < 2 * reserve ? fds / 2 : fds - reserve);
}

/*
 * Have the least recently active idle connection closed, by its loop; false
 * if no loop has one idle to spare.  Each loop says how many idle and when
 * the least recently active of them began to, and home looks at each, only
 * when descriptors run short.  A loop asked to close some, which it has yet
 * to take on, stands for when its next idle one began to, were their
 * beginnings spread evenly since, so that a burst of new clients or calls is
 * spread over the loops, and a loop is asked for no more than it has idle.
 */
static bool
home_evict (struct httpd_home *home)
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
	if (oldest && !atomic_fetch_add(&oldest->evicts, 1))
		loop_post(oldest->loop, &oldest->evict);
	return oldest;
}

/* A client loop's weight: the connections home placed on it and has not had
 * back, and a quarter of its utilisation in per mille, which home works out
 * from the time its turns took since the last look, SAMPLE_NS ago at least.
 * A connection placed since counts once more, as its work is yet to show in
 * the utilisation: so connections that come and go spread over the loops
 * too, rather than go to the lightest until the next look. */
static uint64_t
home_weight (struct httpd_home *home,
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
 * of two as light the one with fewer connections. */
static struct httpd_lane *
home_lane (struct httpd_home *home)
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
	uint64_t wx = home_weight(home, x);
	uint64_t wy = home_weight(home, y);
	return wy < wx || (wy == wx && y->conns < x->conns) ? y : x;
}

static int
by_id (void const *a,
       void const *b)
{
	uint32_t const *x = a;
	uint32_t const *y = b;
	return (*x > *y) - (*x < *y);
}

/* Read the groups of the user of Unix socket @p fd, whose primary group is
 * @p gid, into @p conn, sorted, growing the storage it keeps for them if
 * need be; false if memory runs out. */
static bool
uds_groups (struct conn *conn,
            int          fd,
            gid_t        gid)
{
	static_assert(_Generic((gid_t)0, uint32_t: true, default: false));
	/* Room for the supplementary groups, as many as the kernel last said,
	 * and the primary one, which those may lack. */
	size_t n = 0;
	for (int e = ERANGE; e == ERANGE; ) {
		if (conn->room < n + 1) {
			uint32_t *groups = realloc(conn->groups, (n + 1) * sizeof *groups);
			if (!groups)
				return false;
			conn->groups = groups;
			conn->room = (uint32_t)n + 1;
		}
		socklen_t size = (socklen_t)((conn->room - 1) * sizeof *conn->groups);
		e = getsockopt(fd, SOL_SOCKET, SO_PEERGROUPS, conn->groups, &size) ? errno : 0;
		n = e && e != ERANGE ? 0 : size / sizeof *conn->groups;
	}
	conn->groups[n] = gid;
	conn->n_groups = (uint32_t)n + 1;
	qsort(conn->groups, n + 1, sizeof *conn->groups, by_id);
	return true;
}

/* Place the client on @p fd, from @p addr, on loopback if @p local, on a
 * client loop, in the storage of a connection given back if one is kept,
 * as the user @p cred says if over a Unix socket, to shake hands by @p ctx
 * if not nullptr; if out of memory, close @p fd. */
static void
home_place (struct httpd_home  *home,
            int                 fd,
            uint64_t const      addr[2],
            bool                local,
            struct ucred const *cred,
            struct ssl_ctx_st  *ctx)
{
	struct list *spare = shelf_take(&home->spare);
	struct conn *conn = spare ? container_of(spare, struct conn, hook)
	                          : malloc(sizeof *conn);
	if (conn && !spare) {
		conn->groups = nullptr;
		conn->room = 0;
	}
	struct client *client = conn && (!cred || uds_groups(conn, fd, cred->gid))
	                      ? client_get(home, addr) : nullptr;
	if (!client) {
		if (conn)
			shelf_put(&home->spare, &conn->hook);
		close(fd);
		return;
	}
	if (!cred)
		conn->n_groups = 0;
	conn->uid = cred ? cred->uid : 0;
	conn->uds = cred;
	struct httpd_lane *lane = home_lane(home);
	++lane->conns;
	++lane->placed;
	++home->conns;
	conn->msg.fn = conn_take;
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
home_take (struct httpd_home             *home,
           struct listener const         *l,
           int                            fd,
           struct sockaddr_storage const *ss,
           bool                           shed)
{
	/* A Unix socket's client counts as its user, against the cap. */
	uint64_t addr[2] = { UINT64_MAX };
	bool uds = ss->ss_family == AF_UNIX;
	struct ucred cred;
	socklen_t len = sizeof cred;
	if (!uds) {
		client_key(ss, addr);
	} else if (!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len)) {
		addr[1] = cred.uid;
	} else {
		close(fd);
		++home->refused;
		return;
	}
	struct client const *client = *client_slot(home, addr);
	if (client && home->cap && client->conns >= home->cap) {
		static char const busy[] = "HTTP/1.1 503 Service Unavailable\r\n"
		                           "Retry-After: 1\r\nConnection: close\r\n"
		                           "Content-Length: 0\r\n\r\n";
		if (!l->tls)
			(void)send(fd, busy, sizeof busy - 1, MSG_DONTWAIT | MSG_NOSIGNAL);
		close(fd);
		++home->capped;
	} else if ((shed || home_full(home)) && !home_evict(home)) {
		close(fd);
		++home->refused;
	} else {
		home_place(home, fd, addr, uds || loopback(ss), uds ? &cred : nullptr,
		           l->tls ? home->ctx : nullptr);
	}
}

/*
 * Out of descriptors, which accept() reports whether or not a client waits:
 * accept in the spare descriptor's place.  Returns a non-negative number if a
 * client was accepted from @p l, or else -1 with accept()'s errno, EAGAIN if
 * none waited.
 * The spare comes back once a descriptor comes free.
 */
static int
home_shed (struct httpd_home *home,
           struct listener   *l)
{
	close(home->spare_fd);
	struct sockaddr_storage ss;
	socklen_t len = sizeof ss;
	int fd = accept4(l->fd, (struct sockaddr *)&ss, &len,
	                 SOCK_NONBLOCK | SOCK_CLOEXEC);
	int e = errno;
	if (fd >= 0)
		home_take(home, l, fd, &ss, true);
	home->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	errno = e;
	return fd;
}

/* Whether accept() failing with @p e concerns only the connection it took,
 * so the next may be accepted: Linux passes on a pending connection's network
 * error, and firewall rules may forbid one. */
static bool
accept_next (int e)
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
 * given back, or the tick. */
static void
home_accept (struct loop      *loop,
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
		struct listener *l = &home->listeners[(home->turn + k) % n];
		home->turn = (home->turn + k + 1) % n;
		if (home->spare_fd < 0 &&
		    (home->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC)) < 0) {
			home_watch(home, 0);
			return;
		}
		loop_work(loop, LOOP_ACCEPTS, 1);
		struct sockaddr_storage ss;
		socklen_t len = sizeof ss;
		int fd = accept4(l->fd, (struct sockaddr *)&ss, &len,
		                 SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0 && (errno == EMFILE || errno == ENFILE))
			fd = home_shed(home, l);
		else if (fd >= 0)
			home_take(home, l, fd, &ss, false);
		if (fd >= 0 || accept_next(errno))
			continue;
		l->ready = false;
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			/* Out of memory or descriptors: retry on the next tick. */
			LOG(LOG_ERROR, "accept: %s", strerror(errno));
			home_watch(home, 0);
			return;
		}
	}
	loop_ready(loop, step);
}

static void
home_listen_event (struct loop     *loop,
                   struct loop_ref *ref,
                   uint32_t         events)
{
	(void)events;
	struct httpd_home *home = ref->owner;
	container_of(ref, struct listener, ref)->ready = true;
	loop_ready(loop, &home->accept);
}

static void
home_tick (struct loop       *loop,
           struct loop_timer *timer)
{
	struct httpd_home *home = container_of(timer, struct httpd_home, tick);
	home_budget(home);
	if (home->refused)
		LOG(LOG_WARN, "out of descriptors: turned %" PRIu32 " clients away",
		    home->refused);
	if (home->capped)
		LOG(LOG_WARN, "connection cap per client: turned %" PRIu32
		    " clients away", home->capped);
	home->refused = home->capped = 0;
	if (home->spare_fd < 0)
		home->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (home->open)
		home_watch(home, EPOLLIN);
	(void)loop_timer_set(loop, timer, loop->now + TICK_S * NS);
}

/* Listen on the Unix socket at @p path into @p l: one whose file is left
 * there, which no server listens on, goes first. */
static int
uds_listen (struct listener *l,
            char const      *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	size_t len = strlen(path);
	if (!len || len >= sizeof sa.sun_path) {
		LOG(LOG_ERROR, "invalid listen address: unix:%s", path);
		return EINVAL;
	}
	memcpy(sa.sun_path, path, len);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	int e = fd < 0 ? errno : 0;
	for (int tries = 0; !e && bind(fd, (struct sockaddr *)&sa, sizeof sa); ++tries) {
		e = errno;
		struct stat st;
		int probe = -1;
		if (e != EADDRINUSE || tries || lstat(path, &st) || !S_ISSOCK(st.st_mode) ||
		    (probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
			break;
		bool stale = connect(probe, (struct sockaddr *)&sa, sizeof sa) &&
		             errno == ECONNREFUSED;
		close(probe);
		if (!stale || unlink(path))
			break;
		e = 0;
	}
	if (!e && listen(fd, SOMAXCONN))
		e = errno;
	if (e) {
		LOG(LOG_ERROR, "cannot listen on unix:%s: %s", path, strerror(e));
		if (fd >= 0)
			close(fd);
		return e;
	}
	l->fd = fd;
	l->path = path;
	return 0;
}

/* Listen on @p addr, [host:]port, unix:PATH or tls:[host:]port, into
 * @p l. */
static int
home_listen (struct listener *l,
             char const      *addr)
{
	static char const uds[] = "unix:";
	char const *at = httpd_tls(addr);
	l->tls = at != nullptr;
	if (!at)
		at = addr;
	if (!strncmp(at, uds, sizeof uds - 1)) {
		if (!l->tls)
			return uds_listen(l, at + sizeof uds - 1);
		LOG(LOG_ERROR, "invalid listen address: %s", addr);
		return EINVAL;
	}
	char host[256] = "127.0.0.1";
	char const *port = at;
	char const *colon = strrchr(at, ':');
	if (*at == '[' || colon) {
		char const *start = at + (*at == '[');
		char const *end = *at == '[' ? strchr(at, ']') : colon;
		if (!end || (*at == '[' && end[1] != ':') ||
		    (size_t)(end - start) >= sizeof host) {
			LOG(LOG_ERROR, "invalid listen address: %s", addr);
			return EINVAL;
		}
		memcpy(host, start, (size_t)(end - start));
		host[end - start] = '\0';
		port = end + 1 + (*end == ']');
	}

	struct addrinfo const hints = {
		.ai_flags = AI_PASSIVE | AI_NUMERICSERV,
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	struct addrinfo *list;
	int g = getaddrinfo(*host ? host : nullptr, port, &hints, &list);
	if (g) {
		LOG(LOG_ERROR, "cannot listen on %s: %s", addr, gai_strerror(g));
		return EINVAL;
	}

	int e = EADDRNOTAVAIL;
	for (struct addrinfo *ai = list; ai && e; ai = ai->ai_next) {
		int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK |
		                SOCK_CLOEXEC, ai->ai_protocol);
		int one = 1;
		if (fd >= 0 &&
		    !setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) &&
		    !bind(fd, ai->ai_addr, ai->ai_addrlen) && !listen(fd, SOMAXCONN)) {
			l->fd = fd;
			e = 0;
		} else {
			e = errno;
			if (fd >= 0)
				close(fd);
		}
	}
	freeaddrinfo(list);
	if (e)
		LOG(LOG_ERROR, "cannot listen on %s: %s", addr, strerror(e));
	return e;
}

int
httpd_hub_new (struct httpd_hub **p_hub,
               size_t             loops)
{
	struct httpd_hub *hub = calloc(1, sizeof *hub + loops * sizeof *hub->lane);
	if (!hub)
		return ENOMEM;
	atomic_init(&hub->bodies, 0);
	hub->n = loops;
	for (size_t i = 0; i < loops; ++i) {
		struct httpd_lane *lane = &hub->lane[i];
		lane->evict.fn = srv_evicts;
		lane->handover.fn = srv_policy;
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
	*p_hub = hub;
	return 0;
}

void
httpd_hub_free (struct httpd_hub **p_hub)
{
	free(*p_hub);
	*p_hub = nullptr;
}

struct metrics_conns const *
httpd_hub_metrics (struct httpd_hub const *hub,
                   size_t                  i)
{
	return &hub->lane[i].metrics;
}

int
httpd_home_init (struct httpd_home      **p_home,
                 struct loop             *loop,
                 struct httpd_hub        *hub,
                 struct httpd_cfg const  *cfg,
                 _Atomic uint64_t const  *calls)
{
	struct httpd_home *home = malloc(sizeof *home);
	struct client **clients = calloc(CLIENT_SLOTS, sizeof *clients);
	if (!home || !clients) {
		free(clients);
		free(home);
		LOG(LOG_ERROR, "cannot start the server: %s", strerror(ENOMEM));
		return ENOMEM;
	}
	*home = (struct httpd_home){
		.accept.fn = home_accept,
		.tick.fn = home_tick,
		.loop = loop,
		.hub = hub,
		.clients = clients,
		.calls = calls,
		.rng = loop_clock() | 1,
		.spare_fd = -1,
		.cap = cfg->limits.clients
	};
	if (cfg->tls)
		home->ctx = tls_ctx_get(cfg->tls);
	struct policy_grant g;
	home->anyone = policy_grant(cfg->policy, nullptr, 0, POLICY_REMOTE, nullptr,
	                            &g) && g.tools;
	list_init(&home->spare.list);
	list_init(&home->gone.list);
	home_budget(home);
	hub->home = loop;
	door = *p_home = home;

	/* The listeners are watched for no events until the door opens. */
	size_t n = 0;
	while (cfg->listen[n])
		++n;
	int e = (home->listeners = calloc(n, sizeof *home->listeners)) ? 0 : ENOMEM;
	for (size_t i = 0; !e && i < n; ++i) {
		struct listener *l = &home->listeners[i];
		l->ref.fd = l->fd = -1;
		++home->n_listeners;
		e = home_listen(l, cfg->listen[i]);
		if (!e && l->tls && !home->ctx) {
			LOG(LOG_ERROR, "cannot listen on %s: no TLS certificate",
			    cfg->listen[i]);
			e = EINVAL;
		}
		if (!e)
			e = loop_watch(loop, &l->ref, l->fd, 0, home_listen_event, home);
	}
	if (!e && (home->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC)) < 0)
		e = errno;
	if (!e)
		e = loop_timer_set(loop, &home->tick, loop->now + TICK_S * NS);
	if (e) {
		if (e != EINVAL && e != EADDRINUSE)
			LOG(LOG_ERROR, "cannot start the server: %s", strerror(e));
		httpd_home_fini(p_home);
	}
	return e;
}

void
httpd_home_fini (struct httpd_home **p_home)
{
	struct httpd_home *home = *p_home;
	if (!home)
		return;
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
		for (struct client *c = home->clients[i], *next; c; c = next) {
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
	home_watch(home, EPOLLIN);
	home->open = true;

	/* The endpoints say the server is ready. */
	for (size_t i = 0; i < home->n_listeners; ++i) {
		struct listener const *l = &home->listeners[i];
		struct sockaddr_storage ss;
		socklen_t sslen = sizeof ss;
		char addr[INET6_ADDRSTRLEN];
		char port[8];
		if (l->path) {
			LOG(LOG_INFO, "MCP endpoint http://localhost/mcp on unix:%s", l->path);
		} else if (!getsockname(l->fd, (struct sockaddr *)&ss, &sslen) &&
		           !getnameinfo((struct sockaddr *)&ss, sslen, addr, sizeof addr,
		                        port, sizeof port, NI_NUMERICHOST | NI_NUMERICSERV)) {
			bool v6 = ss.ss_family == AF_INET6;
			LOG(LOG_INFO, "MCP endpoint http%s://%s%s%s:%s/mcp", l->tls ? "s" : "",
			    v6 ? "[" : "", addr, v6 ? "]" : "", port);
			if (home->anyone && !loopback(&ss))
				LOG(LOG_WARN, "warning: listening beyond loopback, where clients "
				    "that show no token may call tools; anyone who can connect "
				    "can spend your API credits");
		}
	}
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
		struct listener *l = &home->listeners[i];
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
	if (home_full(home))
		(void)home_evict(home);
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
	for (size_t n = shelf_unused(&home->spare, level); n && done < max; --n, ++done)
		conn_free(container_of(shelf_drop(&home->spare), struct conn, hook));
	for (size_t n = shelf_unused(&home->gone, level); n && done < max; --n, ++done)
		free(container_of(shelf_drop(&home->gone), struct client, spare));
	return done;
}

int
httpd_init (struct httpd            **p_httpd,
            struct loop              *loop,
            struct httpd_hub         *hub,
            size_t                    i,
            struct stream_app const  *app,
            struct httpd_cfg const   *cfg)
{
	if (app->n_fields > STREAM_FIELDS)
		return EINVAL;

	struct httpd *srv = malloc(sizeof *srv);
	if (!srv) {
		LOG(LOG_ERROR, "cannot start the server: %s", strerror(ENOMEM));
		return ENOMEM;
	}

	/* By default one client address may hold a quarter of the bodies. */
	struct httpd_limits lim = cfg->limits;
	if (!lim.client_bodies)
		lim.client_bodies = lim.bodies / 4 > lim.body ? lim.bodies / 4 : lim.body;
	*srv = (struct httpd){
		.take.fn = srv_take,
		.drain.fn = srv_drain,
		.evict.fn = srv_evict,
		.deadline.fn = srv_deadline,
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
	*p_httpd = srv;
	return 0;
}

void
httpd_fini (struct httpd **p_httpd)
{
	if (!p_httpd || !*p_httpd)
		return;
	struct httpd *srv = *p_httpd;
	*p_httpd = nullptr;

	/* Those handed over but not set up close at once, as when draining. */
	srv->draining = true;
	while (!list_is_empty(&srv->taken)) {
		struct conn *conn = container_of(srv->taken.next, struct conn, hook);
		list_del(&conn->hook);
		conn_setup(srv, conn);
	}
	struct list *const lists[] = { &srv->idle, &srv->busy };
	for (size_t i = 0; i < ARRAY_SIZE(lists); ++i)
		while (!list_is_empty(lists[i]))
			conn_close(container_of(lists[i]->next, struct conn, hook));
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
