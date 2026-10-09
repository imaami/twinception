/** @file Minimal HTTP/1.1 server carrying MCP's Streamable HTTP transport.
 *
 * Every path is the MCP endpoint.  Request bodies need a Content-Length and
 * responses are complete JSON documents.  A connection serves one request at
 * a time, so a pipelined request waits for the tool call ahead of it, and
 * closing the connection during a call cancels the call.
 */
#define _GNU_SOURCE /* accept4() */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "dstr.h"
#include "httpd.h"
#include "list.h"
#include "loop.h"
#include "mcp.h"
#include "util.h"

enum {
	MAX_CONNS = 64,
	MAX_HEAD  = 16 << 10,
	MAX_BODY  = 4 << 20,
	IDLE_S    = 60,
	TICK_S    = 5,
};

struct req {
	struct mcp_meta mcp;
	char const     *method;
	char const     *origin;
	char const     *auth;
	char const     *type;
	char const     *acrh;    /* Access-Control-Request-Headers */
	size_t          head;    /* length of the request line and headers */
	size_t          body;    /* Content-Length */
	bool            length;  /* Content-Length seen */
	bool            close;
	bool            expect;  /* Expect: 100-continue */
	bool            pna;     /* Access-Control-Request-Private-Network */
};

struct conn {
	struct list      hook;
	struct loop_ref  ref;
	struct dstr      in;
	struct dstr      out;
	struct dstr      head;   /* parsed copy of the current request head */
	struct req       req;
	struct httpd    *srv;
	struct mcp_call *call;
	size_t           sent;
	size_t           scan;   /* where to resume looking for the head's end */
	time_t           active;
	int              fd;
	bool             parsed;
	bool             done;   /* the current request has its response */
	bool             cont;   /* 100 Continue sent */
	bool             closing;
	bool             eof;
	bool             dispatching;
	bool             dead;
};

struct httpd {
	struct list        conns;
	struct loop_ref    listen_ref;
	struct loop_ref    tick_ref;
	struct loop       *loop;
	struct mcp        *mcp;
	char const *const *origins;
	char const        *token;
	size_t             token_len;
	size_t             nconns;
	int                listen_fd;
	int                tick_fd;
	bool               any_origin;
	bool               paused;
};

static void conn_close (struct conn *conn);

static time_t
now (void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

static char const *
reason (unsigned status)
{
	switch (status) {
	case 200: return "OK";
	case 202: return "Accepted";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 411: return "Length Required";
	case 413: return "Content Too Large";
	case 415: return "Unsupported Media Type";
	case 417: return "Expectation Failed";
	case 431: return "Request Header Fields Too Large";
	case 500: return "Internal Server Error";
	case 505: return "HTTP Version Not Supported";
	default:  return "Unknown";
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

static bool
is_json (char const *type)
{
	static char const json[] = "application/json";
	size_t n = sizeof json - 1;
	return type && !strncasecmp(type, json, n) &&
	       (!type[n] || type[n] == ';' || type[n] == ' ' || type[n] == '\t');
}

/* Browsers on this machine, whatever the port. */
static bool
loopback_origin (char const *origin)
{
#define P(s) { s, sizeof s - 1 }
	static struct {
		char const *str;
		size_t      len;
	} const prefixes[] = {
		P("http://localhost"), P("https://localhost"),
		P("http://127.0.0.1"), P("https://127.0.0.1"),
		P("http://[::1]"),     P("https://[::1]"),
	};
#undef P
	for (size_t i = 0; i < ARRAY_SIZE(prefixes); ++i) {
		if (strncmp(origin, prefixes[i].str, prefixes[i].len))
			continue;
		char const *port = origin + prefixes[i].len;
		if (!*port)
			return true;
		size_t digits = *port == ':' ? strspn(++port, "0123456789") : 0;
		return digits && digits <= 5 && !port[digits];
	}
	return false;
}

static bool
origin_allowed (struct httpd const *srv,
                char const         *origin)
{
	if (srv->any_origin || loopback_origin(origin))
		return true;
	for (char const *const *o = srv->origins; o && *o; ++o)
		if (!strcmp(*o, origin))
			return true;
	return false;
}

static bool
authorized (struct httpd const *srv,
            char const         *auth)
{
	static char const scheme[] = "Bearer ";
	if (!auth || strncasecmp(auth, scheme, sizeof scheme - 1))
		return false;

	/* Compare in constant time; only the length can leak. */
	char const *token = auth + sizeof scheme - 1;
	size_t len = strlen(token);
	unsigned char diff = len != srv->token_len;
	for (size_t i = 0; i < srv->token_len; ++i)
		diff |= (unsigned char)srv->token[i] ^
		        (unsigned char)(i < len ? token[i] : 0);
	return !diff;
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

static unsigned
header (struct req *req,
        char const *name,
        char const *value,
        bool        http10)
{
#define FIELD(str, member)                             \
	if (!strcasecmp(name, str)) {                  \
		if (req->member)                       \
			return 400;                    \
		req->member = value;                   \
		return 0;                              \
	}
	FIELD("origin", origin)
	FIELD("authorization", auth)
	FIELD("content-type", type)
	FIELD("access-control-request-headers", acrh)
	FIELD("mcp-protocol-version", mcp.version)
	FIELD("mcp-method", mcp.method)
	FIELD("mcp-name", mcp.name)
	FIELD("mcp-session-id", mcp.session)
#undef FIELD

	if (!strcasecmp(name, "content-length")) {
		size_t n = strspn(value, "0123456789");
		if (!n || value[n] || req->length)
			return 400;
		if (n > 9 || (req->body = strtoul(value, nullptr, 10)) > MAX_BODY)
			return 413;
		req->length = true;
	} else if (!strcasecmp(name, "transfer-encoding")) {
		return 411;
	} else if (!strcasecmp(name, "connection")) {
		if (has_token(value, "close", sizeof "close" - 1))
			req->close = true;
		else if (http10 && has_token(value, "keep-alive",
		                             sizeof "keep-alive" - 1))
			req->close = false;
	} else if (!strcasecmp(name, "expect")) {
		if (strcasecmp(value, "100-continue"))
			return 417;
		req->expect = !http10;
	} else if (!strcasecmp(name, "access-control-request-private-network")) {
		req->pna = !strcasecmp(value, "true");
	}
	return 0;
}

/* Parse a complete, NUL-terminated head in place; nonzero is an HTTP error. */
static unsigned
parse_head (char       *data,
            size_t      len,
            struct req *req)
{
	*req = (struct req){ .head = len };
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
	for (char const *p = line; *p; ++p)
		if (!tchar((unsigned char)*p))
			return 400;
	bool http10 = !strcmp(version, "HTTP/1.0");
	if (!http10 && strcmp(version, "HTTP/1.1"))
		return strncmp(version, "HTTP/", 5) ? 400 : 505;
	req->method = line;
	req->close = http10;

	while (*(line = next_line(&cursor))) {
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

		unsigned status = header(req, line, value, http10);
		if (status)
			return status;
	}
	return 0;
}

static void
srv_resume (struct httpd *srv)
{
	if (srv->paused && srv->nconns < MAX_CONNS &&
	    !loop_modify(srv->loop, &srv->listen_ref, EPOLLIN))
		srv->paused = false;
}

static void
conn_release (struct loop_ref *ref)
{
	free(container_of(ref, struct conn, ref));
}

static void
conn_close (struct conn *conn)
{
	if (conn->dead)
		return;
	conn->dead = true;

	struct httpd *srv = conn->srv;
	mcp_cancel(srv->mcp, &conn->call);
	list_del(&conn->hook);
	--srv->nconns;
	loop_release(srv->loop, &conn->ref, conn_release);
	close(conn->fd);
	dstr_cleanup(&conn->head);
	dstr_cleanup(&conn->out);
	dstr_cleanup(&conn->in);
	srv_resume(srv);
}

static void
conn_respond (struct conn *conn,
              unsigned     status,
              char const  *body,
              size_t       len,
              char const  *extra)
{
	if (conn->dead)
		return;

	struct req const *req = &conn->req;
	struct dstr *out = &conn->out;
	conn->closing |= req->close;
	conn->done = conn->parsed;

	bool ok = dstr_printf(out, "HTTP/1.1 %u %s\r\n%s%s", status, reason(status),
	                      body ? "Content-Type: application/json\r\n" : "",
	                      conn->closing ? "Connection: close\r\n" : "");
	if (ok && status != 204)
		ok = dstr_printf(out, "Content-Length: %zu\r\n", len);
	if (ok && req->origin)
		ok = dstr_printf(out, "Access-Control-Allow-Origin: %s\r\n"
		                      "Access-Control-Allow-Credentials: true\r\n"
		                      "Access-Control-Expose-Headers: Mcp-Session-Id, "
		                      "WWW-Authenticate\r\n"
		                      "Vary: Origin\r\n", req->origin);
	if (ok)
		ok = dstr_printf(out, "%s\r\n", extra ? extra : "");
	if (ok)
		ok = dstr_cat_n(out, body, len);
	if (!ok)
		conn_close(conn);
}

static void
conn_error (struct conn *conn,
            unsigned     status,
            char const  *extra)
{
	char body[128];
	int n = snprintf(body, sizeof body, "{\"jsonrpc\":\"2.0\",\"error\":"
	                 "{\"code\":-32600,\"message\":\"%s\"}}", reason(status));
	conn_respond(conn, status, body, (size_t)n, extra);
}

/* Let the event handler flush, so replies never re-enter dispatching. */
static void
conn_rearm (struct conn *conn)
{
	uint32_t events = conn->eof ? 0 : EPOLLRDHUP;
	if (conn->sent < dstr_len(&conn->out))
		events |= EPOLLOUT;
	else if (!conn->call && !conn->eof)
		events |= EPOLLIN;
	if (loop_modify(conn->srv->loop, &conn->ref, events))
		conn_close(conn);
}

static void
conn_reply (void       *user,
            unsigned    status,
            char const *body,
            size_t      len,
            char const *session)
{
	struct conn *conn = user;
	char extra[80];
	if (session)
		snprintf(extra, sizeof extra, "Mcp-Session-Id: %s\r\n", session);
	conn_respond(conn, status, body, len, session ? extra : nullptr);
	if (!conn->dispatching && !conn->dead)
		conn_rearm(conn);
}

static void
conn_preflight (struct conn *conn)
{
	struct req const *req = &conn->req;
	if (!req->origin) {
		conn_respond(conn, 204, nullptr, 0, "Allow: POST, OPTIONS\r\n");
		return;
	}

	dstr_auto extra = DSTR_INIT;
	if (!dstr_printf(&extra,
	                 "Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n"
	                 "Access-Control-Allow-Headers: %s\r\n"
	                 "Access-Control-Max-Age: 600\r\n%s"
	                 "Vary: Access-Control-Request-Headers\r\n",
	                 req->acrh ? req->acrh : "Content-Type, Authorization",
	                 req->pna ? "Access-Control-Allow-Private-Network: true\r\n"
	                          : ""))
		conn_close(conn);
	else
		conn_respond(conn, 204, nullptr, 0, dstr_get(&extra));
}

static void
conn_dispatch (struct conn *conn)
{
	struct httpd *srv = conn->srv;
	struct req *req = &conn->req;

	/* Our error bodies would trail a HEAD response; end the stream. */
	conn->closing |= !strcmp(req->method, "HEAD");
	if (req->origin && !origin_allowed(srv, req->origin)) {
		/* DNS rebinding defence: refuse, and grant no CORS access. */
		req->origin = nullptr;
		conn->closing = true;
		conn_error(conn, 403, nullptr);
	} else if (!strcmp(req->method, "OPTIONS")) {
		conn_preflight(conn);
	} else if (srv->token && !authorized(srv, req->auth)) {
		conn_error(conn, 401, "WWW-Authenticate: Bearer\r\n");
	} else if (strcmp(req->method, "POST")) {
		/* No standalone SSE stream (GET) and no sessions to DELETE. */
		conn_error(conn, 405, "Allow: POST, OPTIONS\r\n");
	} else if (!is_json(req->type)) {
		conn_error(conn, 415, nullptr);
	} else {
		conn->dispatching = true;
		mcp_post(srv->mcp, &req->mcp, dstr_get(&conn->in) + req->head, req->body,
		         conn_reply, conn, &conn->call);
		conn->dispatching = false;
	}
}

/* Advance to the next request; true if that produced output to flush. */
static bool
conn_next (struct conn *conn)
{
	struct req *req = &conn->req;
	if (conn->done) {
		dstr_erase(&conn->in, 0, req->head + req->body);
		*req = (struct req){0};
		conn->parsed = conn->done = conn->cont = false;
		conn->scan = 0;
	}

	size_t in_len = dstr_len(&conn->in);
	if (!conn->parsed) {
		char const *in = dstr_get(&conn->in);
		size_t head = head_length(in, in_len, &conn->scan);
		unsigned status = head > MAX_HEAD || (!head && in_len > MAX_HEAD)
			? 431 : 0;
		if (!head && !status)
			return false;
		if (!status) {
			dstr_clear(&conn->head);
			if (!dstr_cat_n(&conn->head, in, head)) {
				conn_close(conn);
				return false;
			}
			status = parse_head((char *)dstr_raw(&conn->head), head, req);
		}
		if (status) {
			*req = (struct req){0};
			conn->closing = true;
			conn_error(conn, status, nullptr);
			return true;
		}
		conn->parsed = true;
	}

	if (in_len - req->head < req->body) {
		if (!req->expect || conn->cont)
			return false;
		conn->cont = true;
		static char const cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
		if (!dstr_cat_n(&conn->out, cont, sizeof cont - 1))
			conn_close(conn);
		return true;
	}

	conn_dispatch(conn);
	return true;
}

/* Write pending output; true once everything is out. */
static bool
conn_flush (struct conn *conn)
{
	char const *out = dstr_get(&conn->out);
	size_t len = dstr_len(&conn->out);
	while (!conn->dead && conn->sent < len) {
		ssize_t n = send(conn->fd, out + conn->sent, len - conn->sent,
		                 MSG_NOSIGNAL);
		if (n >= 0) {
			conn->sent += (size_t)n;
			conn->active = now();
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return false;
		} else if (errno != EINTR) {
			conn_close(conn);
		}
	}
	if (conn->dead)
		return false;
	dstr_clear(&conn->out);
	conn->sent = 0;
	if (conn->closing) {
		conn_close(conn);
		return false;
	}
	return true;
}

static void
conn_pump (struct conn *conn)
{
	while (conn_flush(conn) && !conn->call && conn_next(conn)) {}
	if (conn->dead)
		return;
	if (conn->eof && !conn->call && conn->sent == dstr_len(&conn->out))
		conn_close(conn);
	else
		conn_rearm(conn);
}

static void
conn_read (struct conn *conn)
{
	while (!conn->eof) {
		/* More than one maximal request buffered is abuse. */
		if (dstr_len(&conn->in) > MAX_HEAD + MAX_BODY) {
			conn_close(conn);
			return;
		}
		char chunk[16 << 10];
		ssize_t n = recv(conn->fd, chunk, sizeof chunk, 0);
		if (n > 0) {
			if (!dstr_cat_n(&conn->in, chunk, (size_t)n)) {
				conn_close(conn);
				return;
			}
			conn->active = now();
		} else if (!n) {
			conn->eof = true;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return;
		} else if (errno != EINTR) {
			conn_close(conn);
			return;
		}
	}
}

static int
conn_event (struct loop     *loop,
            struct loop_ref *ref,
            uint32_t         events)
{
	(void)loop;
	struct conn *conn = ref->owner;

	/* A client that goes away during a tool call cancels it. */
	if ((events & EPOLLERR) ||
	    (conn->call && (events & (EPOLLRDHUP | EPOLLHUP)))) {
		conn_close(conn);
		return 0;
	}
	if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))
		conn_read(conn);
	if (!conn->dead)
		conn_pump(conn);
	return 0;
}

static int
srv_accept (struct loop     *loop,
            struct loop_ref *ref,
            uint32_t         events)
{
	(void)events;
	struct httpd *srv = ref->owner;
	while (srv->nconns < MAX_CONNS) {
		int fd = accept4(srv->listen_fd, nullptr, nullptr,
		                 SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			/* Out of descriptors or memory: retry on a later tick. */
			fprintf(stderr, "askai: accept: %s\n", strerror(errno));
			break;
		}

		struct conn *conn = malloc(sizeof *conn);
		if (conn)
			*conn = (struct conn){
				.in = DSTR_INIT,
				.out = DSTR_INIT,
				.head = DSTR_INIT,
				.srv = srv,
				.fd = fd,
				.active = now()
			};
		if (!conn || loop_watch(loop, &conn->ref, fd, EPOLLIN | EPOLLRDHUP,
		                        conn_event, conn)) {
			free(conn);
			close(fd);
			break;
		}
		list_append(&srv->conns, &conn->hook);
		++srv->nconns;
	}
	if (!loop_modify(loop, &srv->listen_ref, 0))
		srv->paused = true;
	return 0;
}

static int
srv_tick (struct loop     *loop,
          struct loop_ref *ref,
          uint32_t         events)
{
	(void)loop;
	(void)events;
	struct httpd *srv = ref->owner;
	uint64_t ticks;
	while (read(srv->tick_fd, &ticks, sizeof ticks) < 0 && errno == EINTR) {}

	time_t t = now();
	struct list *node;
	struct list *tmp;
	list_foreach_safe (node, tmp, &srv->conns) {
		struct conn *conn = container_of(node, struct conn, hook);
		if (!conn->call && t - conn->active > IDLE_S)
			conn_close(conn);
	}
	srv_resume(srv);
	return 0;
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

static int
srv_listen (struct httpd *srv,
            char const   *addr)
{
	char host[256] = "127.0.0.1";
	char const *port = addr;
	char const *colon = strrchr(addr, ':');
	if (*addr == '[' || colon) {
		char const *start = addr + (*addr == '[');
		char const *end = *addr == '[' ? strchr(addr, ']') : colon;
		if (!end || (*addr == '[' && end[1] != ':') ||
		    (size_t)(end - start) >= sizeof host) {
			fprintf(stderr, "askai: invalid listen address: %s\n", addr);
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
		fprintf(stderr, "askai: cannot listen on %s: %s\n", addr, gai_strerror(g));
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
			srv->listen_fd = fd;
			e = 0;
		} else {
			e = errno;
			if (fd >= 0)
				close(fd);
		}
	}
	freeaddrinfo(list);
	if (e)
		fprintf(stderr, "askai: cannot listen on %s: %s\n", addr, strerror(e));
	return e;
}

int
httpd_init (struct httpd           **p_httpd,
            struct loop             *loop,
            struct mcp              *mcp,
            struct httpd_cfg const  *cfg)
{
	struct httpd *srv = calloc(1, sizeof *srv);
	if (!srv)
		return ENOMEM;
	*srv = (struct httpd){
		.loop = loop,
		.mcp = mcp,
		.origins = cfg->origins,
		.token = cfg->token,
		.token_len = cfg->token ? strlen(cfg->token) : 0,
		.listen_fd = -1,
		.tick_fd = -1
	};
	srv->listen_ref.fd = -1;
	srv->tick_ref.fd = -1;
	list_init(&srv->conns);
	for (char const *const *o = cfg->origins; o && *o; ++o)
		srv->any_origin |= !strcmp(*o, "*");
	*p_httpd = srv;

	int e = srv_listen(srv, cfg->listen);
	if (!e) {
		srv->tick_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
		struct itimerspec const its = {
			.it_interval.tv_sec = TICK_S,
			.it_value.tv_sec = TICK_S
		};
		if (srv->tick_fd < 0 || timerfd_settime(srv->tick_fd, 0, &its, nullptr))
			e = errno;
	}
	if (!e)
		e = loop_watch(loop, &srv->tick_ref, srv->tick_fd, EPOLLIN, srv_tick, srv);
	if (!e)
		e = loop_watch(loop, &srv->listen_ref, srv->listen_fd, EPOLLIN,
		               srv_accept, srv);
	if (e) {
		if (srv->listen_fd >= 0)
			fprintf(stderr, "askai: cannot start the server: %s\n", strerror(e));
		httpd_fini(p_httpd);
		return e;
	}

	struct sockaddr_storage ss;
	socklen_t sslen = sizeof ss;
	char addr[INET6_ADDRSTRLEN];
	char port[8];
	if (!getsockname(srv->listen_fd, (struct sockaddr *)&ss, &sslen) &&
	    !getnameinfo((struct sockaddr *)&ss, sslen, addr, sizeof addr,
	                 port, sizeof port, NI_NUMERICHOST | NI_NUMERICSERV)) {
		bool v6 = ss.ss_family == AF_INET6;
		fprintf(stderr, "askai: MCP endpoint http://%s%s%s:%s/mcp\n",
		        v6 ? "[" : "", addr, v6 ? "]" : "", port);
		if (!srv->token && !loopback(&ss))
			fputs("askai: warning: listening beyond loopback without "
			      "ASKAI_TOKEN; anyone who can connect can spend your "
			      "API credits\n", stderr);
	}
	return 0;
}

void
httpd_fini (struct httpd **p_httpd)
{
	if (!p_httpd || !*p_httpd)
		return;
	struct httpd *srv = *p_httpd;
	*p_httpd = nullptr;

	struct list *node;
	struct list *tmp;
	list_foreach_safe (node, tmp, &srv->conns)
		conn_close(container_of(node, struct conn, hook));
	loop_unwatch(srv->loop, &srv->listen_ref);
	loop_unwatch(srv->loop, &srv->tick_ref);
	if (srv->listen_fd >= 0)
		close(srv->listen_fd);
	if (srv->tick_fd >= 0)
		close(srv->tick_fd);
	free(srv);
}
