/** @file OpenAI-compatible upstream mock for the askai load harness.
 *
 * Serves POST /v1/chat/completions with a configurable number of slots, a
 * per-request delay, a streaming token rate and an answer size, in JSON or in
 * SSE (when the request body carries stream:true): paced, a token an event,
 * or in a burst, up to BURST tokens an event.  GET /props gives the slots as
 * llama-server does, total_slots, when they are limited.  It counts requests,
 * the concurrency peak, completed replies and cancellations (clients that
 * hang up before their reply is sent, whether queued for a slot or holding
 * one, a request that comes with its hang-up included), of them those of
 * streams under way, reporting them on exit and at GET /stats; requests =
 * served + cancelled + active + queued at all times.  All I/O is non-blocking
 * on one epoll loop, so a disconnect is noticed at once.  With -c it closes
 * each connection after its reply, saying so, so that a client keeps none
 * idle.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
	MAX_IN = 8 << 20, /**< request bytes buffered per connection */
	BURST  = 256,     /**< tokens an event at most, unpaced */
};

enum conn_state {
	ST_RECV,    /**< reading a request head and body */
	ST_QUEUED,  /**< request parsed, waiting for a free slot */
	ST_DELAY,   /**< admitted, waiting out the per-request delay */
	ST_STREAM,  /**< emitting SSE token frames at the token rate */
	ST_FLUSH,   /**< draining the final bytes of a reply */
};

struct conn;

/** A list of connections kept in due-time order. */
struct fifo {
	struct conn *head;
	struct conn *tail;
};

struct conn {
	struct conn   *prev;       /**< neighbours on @ref on */
	struct conn   *next;
	struct fifo   *on;         /**< the slot queue or timer list we are on */
	unsigned char *in;
	unsigned char *out;
	uint64_t       due_ns;     /**< when the next timed action is due */
	size_t         in_len;
	size_t         in_cap;
	size_t         out_len;
	size_t         out_cap;
	size_t         out_sent;
	uint32_t       tokens_left;
	int            fd;
	uint8_t        state;
	bool           stream;     /**< this request asked for SSE */
	bool           keep;       /**< keep the connection after the reply */
	bool           chat;       /**< holds a chat-completion slot */
	bool           tail_sent;  /**< the SSE terminator has been queued */
};

static struct {
	struct fifo  waiting;      /**< requests queued for a slot */
	struct fifo  delays;       /**< admitted requests waiting out the delay */
	struct fifo  tokens;       /**< streams waiting for their next token */
	uint64_t     requests;
	uint64_t     served;
	uint64_t     cancelled;
	uint64_t     streams;      /**< of those, streams under way */
	uint32_t     active;
	uint32_t     queued;
	uint32_t     peak;
	uint32_t     slots;        /**< 0: unlimited */
	uint32_t     answer_tok;
	uint32_t     delay_ms;
	uint32_t     tok_per_s;    /**< 0: burst the whole answer at once */
	int          epfd;
	int          listen_fd;
	volatile sig_atomic_t stop;
	uint64_t     closing;      /**< bool, widened over the padding: -c */
} mock;

static uint64_t
now_ns (void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1'000'000'000u + (uint64_t)ts.tv_nsec;
}

static void
on_signal (int sig)
{
	(void)sig;
	mock.stop = 1;
}

/** Insert @p c after the last entry not due later, so @p f stays sorted. */
static void
fifo_insert (struct fifo *f,
             struct conn *c)
{
	struct conn *prev = f->tail;
	while (prev && prev->due_ns > c->due_ns)
		prev = prev->prev;
	c->prev = prev;
	c->next = prev ? prev->next : f->head;
	*(c->next ? &c->next->prev : &f->tail) = c;
	*(prev ? &prev->next : &f->head) = c;
	c->on = f;
}

static void
fifo_del (struct conn *c)
{
	struct fifo *f = c->on;
	*(c->prev ? &c->prev->next : &f->head) = c->next;
	*(c->next ? &c->next->prev : &f->tail) = c->prev;
	c->prev = c->next = nullptr;
	c->on = nullptr;
}

static void
want (struct conn *c,
      uint32_t     events)
{
	struct epoll_event ev = { .events = events, .data.ptr = c };
	epoll_ctl(mock.epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

/** Append @p len bytes to a connection's output, growing as needed. */
static bool
out_add (struct conn *c,
         char const  *data,
         size_t       len)
{
	if (c->out_len + len > c->out_cap) {
		size_t cap = c->out_cap ? c->out_cap : 512;
		while (cap < c->out_len + len)
			cap *= 2;
		unsigned char *p = realloc(c->out, cap);
		if (!p)
			return false;
		c->out = p;
		c->out_cap = cap;
	}
	memcpy(c->out + c->out_len, data, len);
	c->out_len += len;
	return true;
}

static bool
out_printf (struct conn *c,
            char const  *fmt,
            ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return n >= 0 && (size_t)n < sizeof buf && out_add(c, buf, (size_t)n);
}

/** Queue a chunked-transfer frame carrying one SSE event. */
static bool
sse_chunk (struct conn *c,
           char const  *event,
           size_t       len)
{
	char hdr[32];
	int n = snprintf(hdr, sizeof hdr, "%zx\r\n", len);
	return n > 0 && out_add(c, hdr, (size_t)n) && out_add(c, event, len) &&
	       out_add(c, "\r\n", 2);
}

/** Take a slot and start the per-request delay. */
static void
admit (struct conn *c)
{
	if (++mock.active > mock.peak)
		mock.peak = mock.active;
	c->chat = true;
	c->state = ST_DELAY;
	c->due_ns = now_ns() + (uint64_t)mock.delay_ms * 1'000'000u;
	fifo_insert(&mock.delays, c);
}

/** Give up a held slot and admit the oldest waiting connection, if any. */
static void
slot_release (void)
{
	mock.active--;
	struct conn *c = mock.waiting.head;
	if (!c)
		return;
	fifo_del(c);
	mock.queued--;
	admit(c);
}

static void
conn_free (struct conn *c)
{
	/* A request still holding or waiting for a slot was cancelled. */
	mock.cancelled += c->chat || c->state == ST_QUEUED;
	mock.streams += c->chat && c->state == ST_STREAM;
	if (c->on)
		fifo_del(c);
	if (c->state == ST_QUEUED)
		mock.queued--;
	if (c->chat)
		slot_release();
	epoll_ctl(mock.epfd, EPOLL_CTL_DEL, c->fd, nullptr);
	close(c->fd);
	free(c->in);
	free(c->out);
	free(c);
}

/** 1: everything sent; 0: partial (EPOLLOUT armed); -1: error (@p c freed). */
static int
drain (struct conn *c)
{
	while (c->out_sent < c->out_len) {
		ssize_t n = send(c->fd, c->out + c->out_sent, c->out_len - c->out_sent,
		                 MSG_NOSIGNAL);
		if (n > 0) {
			c->out_sent += (size_t)n;
		} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			want(c, EPOLLOUT | EPOLLRDHUP);
			return 0;
		} else if (!(n < 0 && errno == EINTR)) {
			conn_free(c);
			return -1;
		}
	}
	c->out_len = c->out_sent = 0;
	return 1;
}

/** Retire a finished reply; false if that closed (freed) the connection. */
static bool
request_done (struct conn *c)
{
	if (c->chat) {
		mock.served++;
		c->chat = false;
		slot_release();
	}
	if (!c->keep) {
		conn_free(c);
		return false;
	}
	c->state = ST_RECV;
	want(c, EPOLLIN | EPOLLRDHUP);
	return true;
}

/** Queue the next SSE unit; false means the stream is already complete. */
static bool
stream_next (struct conn *c)
{
	if (c->tokens_left) {
		/* The tokens of the JSON answer: "A", then " A" each. */
		static char const head[] = "data: {\"choices\":[{\"index\":0,"
		                           "\"delta\":{\"content\":\"";
		static char const tail[] = "\"}}]}\n\n";
		char ev[sizeof head + 2 * BURST + sizeof tail];
		uint32_t n = mock.tok_per_s ? 1 : c->tokens_left < BURST ? c->tokens_left
		                                                         : BURST;
		bool first = c->tokens_left == mock.answer_tok;
		size_t len = sizeof head - 1;
		memcpy(ev, head, len);
		for (uint32_t i = 0; i < n; ++i) {
			if (i || !first)
				ev[len++] = ' ';
			ev[len++] = 'A';
		}
		memcpy(ev + len, tail, sizeof tail - 1);
		if (!sse_chunk(c, ev, len + sizeof tail - 1))
			c->keep = false;
		c->tokens_left -= n;
		c->due_ns = mock.tok_per_s ? now_ns() + 1'000'000'000u / mock.tok_per_s : 0;
		return true;
	}
	if (!c->tail_sent) {
		char const done[] = "data: {\"choices\":[{\"index\":0,\"delta\":{},"
		                    "\"finish_reason\":\"stop\"}]}\n\n";
		char const bye[] = "data: [DONE]\n\n";
		if (!(sse_chunk(c, done, sizeof done - 1) &&
		      sse_chunk(c, bye, sizeof bye - 1) && out_add(c, "0\r\n\r\n", 5)))
			c->keep = false;
		c->tail_sent = true;
		return true;
	}
	return false;
}

/** Drive output: drain, pace SSE tokens, and retire a finished reply.
 *  False if @p c was freed. */
static bool
flush (struct conn *c)
{
	for (;;) {
		int d = drain(c);
		if (d <= 0)
			return !d;
		if (c->state != ST_STREAM)
			return request_done(c);
		if (c->tokens_left && c->due_ns > now_ns()) {
			want(c, EPOLLRDHUP);
			fifo_insert(&mock.tokens, c);
			return true;
		}
		if (!stream_next(c))
			return request_done(c);
	}
}

/** Build a complete reply for an admitted request and start sending it. */
static void
reply_begin (struct conn *c)
{
	c->tokens_left = mock.answer_tok;
	c->tail_sent = false;
	if (c->stream) {
		c->state = ST_STREAM;
		c->due_ns = 0;
		if (!out_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream"
		                "\r\nCache-Control: no-cache\r\n%sTransfer-Encoding: "
		                "chunked\r\n\r\n", c->keep ? "" : "Connection: close\r\n"))
			conn_free(c);
		else
			flush(c);
		return;
	}

	c->state = ST_FLUSH;
	char const pre[] = "{\"id\":\"mock\",\"object\":\"chat.completion\","
	                   "\"choices\":[{\"index\":0,\"message\":{\"role\":"
	                   "\"assistant\",\"content\":\"";
	char const tail[] = "\"},\"finish_reason\":\"stop\"}]}";
	size_t content = c->tokens_left ? c->tokens_left * 2 - 1 : 0;
	size_t len = sizeof pre - 1 + content + sizeof tail - 1;
	bool ok = out_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
	                     "%sContent-Length: %zu\r\n\r\n",
	                     c->keep ? "" : "Connection: close\r\n", len) &&
	          out_add(c, pre, sizeof pre - 1);
	for (uint32_t i = 0; ok && i < c->tokens_left; ++i)
		ok = out_add(c, i ? " A" : "A", i ? 2 : 1);
	if (ok && out_add(c, tail, sizeof tail - 1))
		flush(c);
	else
		conn_free(c);
}

/** Reply with a fixed body; false if @p c was freed. */
static bool
serve_fixed (struct conn *c,
             char const  *status,
             char const  *body,
             size_t       len)
{
	c->state = ST_FLUSH;
	if (out_printf(c, "HTTP/1.1 %s\r\nContent-Type: application/json\r\n"
	               "Content-Length: %zu\r\n\r\n", status, len) &&
	    out_add(c, body, len))
		return flush(c);
	conn_free(c);
	return false;
}

/** Dispatch one fully-received request (head + body) at the front of @p in.
 *  False if @p c was freed. */
static bool
handle_request (struct conn *c,
                size_t       head,
                size_t       clen)
{
	bool post = c->in_len >= 5 && !memcmp(c->in, "POST ", 5);
	bool get = !post && c->in_len >= 4 && !memcmp(c->in, "GET ", 4);
	bool stats = get && memmem(c->in, head, "/stats", 6);
	bool props = get && mock.slots && memmem(c->in, head, "/props", 6);
	unsigned char *body = c->in + head;
	c->keep = !mock.closing && !memmem(c->in, head, "Connection: close", 17) &&
	          !memmem(c->in, head, "connection: close", 17);
	c->stream = post && (memmem(body, clen, "\"stream\":true", 13) ||
	                     memmem(body, clen, "\"stream\": true", 14));

	size_t used = head + clen;
	memmove(c->in, c->in + used, c->in_len - used);
	c->in_len -= used;

	if (stats) {
		char buf[256];
		int n = snprintf(buf, sizeof buf, "{\"requests\":%llu,\"served\":%llu,"
		                 "\"cancelled\":%llu,\"active\":%u,\"queued\":%u,"
		                 "\"peak\":%u,\"slots\":%u,\"streams\":%llu}",
		                 (unsigned long long)mock.requests,
		                 (unsigned long long)mock.served,
		                 (unsigned long long)mock.cancelled, mock.active,
		                 mock.queued, mock.peak, mock.slots,
		                 (unsigned long long)mock.streams);
		return serve_fixed(c, "200 OK", buf, (size_t)n);
	}
	if (props) {
		char buf[64];
		int n = snprintf(buf, sizeof buf, "{\"total_slots\":%u}", mock.slots);
		return serve_fixed(c, "200 OK", buf, (size_t)n);
	}
	if (!post)
		return serve_fixed(c, "404 Not Found", "", 0);

	mock.requests++;
	if (!mock.slots || mock.active < mock.slots) {
		admit(c);
		return true;
	}
	mock.queued++;
	c->state = ST_QUEUED;
	c->due_ns = UINT64_MAX;
	fifo_insert(&mock.waiting, c);
	want(c, EPOLLRDHUP); /* still watch for a hang-up while queued */
	return true;
}

/** The Content-Length of a request head, or SIZE_MAX if it is not acceptable. */
static size_t
content_length (unsigned char const *in,
                size_t               head)
{
	unsigned char const *cl = memmem(in, head, "Content-Length:", 15);
	if (!cl)
		cl = memmem(in, head, "content-length:", 15);
	if (!cl)
		return 0;
	char const *v = (char const *)cl + 15;
	v += strspn(v, " \t");
	if (*v < '0' || *v > '9')
		return SIZE_MAX;
	errno = 0;
	unsigned long long n = strtoull(v, nullptr, 10); /* the head ends in CRLF */
	return errno || n > MAX_IN ? SIZE_MAX : (size_t)n;
}

static void
on_readable (struct conn *c)
{
	bool eof = false;
	for (;;) {
		if (c->in_len == c->in_cap) {
			size_t cap = c->in_cap ? c->in_cap * 2 : 4096;
			unsigned char *p = cap <= MAX_IN ? realloc(c->in, cap) : nullptr;
			if (!p) {
				conn_free(c);
				return;
			}
			c->in = p;
			c->in_cap = cap;
		}
		ssize_t n = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
		if (n > 0) {
			c->in_len += (size_t)n;
		} else if (!n) {
			eof = true; /* a clean close cancels a request in flight */
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			break;
		} else if (errno != EINTR) {
			conn_free(c);
			return;
		}
	}

	/* A request that came whole with the hang-up counts, as cancelled. */
	while (c->state == ST_RECV && c->in_len) {
		unsigned char *end = memmem(c->in, c->in_len, "\r\n\r\n", 4);
		if (!end)
			break;
		size_t head = (size_t)(end - c->in) + 4;
		size_t clen = content_length(c->in, head);
		if (clen == SIZE_MAX) {
			conn_free(c);
			return;
		}
		if (c->in_len - head < clen)
			break;
		if (!handle_request(c, head, clen))
			return;
	}
	if (eof)
		conn_free(c);
}

static void
on_event (struct conn *c,
          uint32_t     events)
{
	/* A request may come in the same event as its hang-up: read it first. */
	if (events & EPOLLIN && c->state == ST_RECV)
		on_readable(c);
	else if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
		conn_free(c);
	else if (events & EPOLLIN)
		on_readable(c);
	else if (events & EPOLLOUT && (c->state == ST_FLUSH || c->state == ST_STREAM))
		flush(c);
}

static void
accept_conns (void)
{
	for (;;) {
		int fd = accept4(mock.listen_fd, nullptr, nullptr,
		                 SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0)
			return;
		struct conn *c = calloc(1, sizeof *c);
		if (!c) {
			close(fd);
			return;
		}
		c->fd = fd;
		c->state = ST_RECV;
		/* A stream's head, events and tail go in sends of their own,
		 * which Nagle would hold back for the client's delayed ACKs. */
		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
		struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP, .data.ptr = c };
		if (epoll_ctl(mock.epfd, EPOLL_CTL_ADD, fd, &ev))
			conn_free(c);
	}
}

/** Fire every delay and token time that has arrived; return the next one. */
static uint64_t
run_timers (void)
{
	uint64_t t = now_ns();
	for (struct conn *c; (c = mock.delays.head) && c->due_ns <= t; ) {
		fifo_del(c);
		reply_begin(c);
	}
	for (struct conn *c; (c = mock.tokens.head) && c->due_ns <= t; ) {
		fifo_del(c);
		flush(c);
	}
	/* Firing may have armed new timers, so look at the heads only now. */
	uint64_t next = UINT64_MAX;
	if (mock.delays.head)
		next = mock.delays.head->due_ns;
	if (mock.tokens.head && mock.tokens.head->due_ns < next)
		next = mock.tokens.head->due_ns;
	return next;
}

static int
listen_on (char const *host,
           char const *port)
{
	struct addrinfo const hints = {
		.ai_flags = AI_PASSIVE | AI_NUMERICSERV,
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *list;
	if (getaddrinfo(host, port, &hints, &list))
		return -1;
	int fd = -1;
	for (struct addrinfo *ai = list; ai && fd < 0; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
		            ai->ai_protocol);
		int one = 1;
		if (fd >= 0 &&
		    (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) ||
		     bind(fd, ai->ai_addr, ai->ai_addrlen) || listen(fd, 4096))) {
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(list);
	return fd;
}

int
main (int   argc,
      char *argv[])
{
	char const *addr = "127.0.0.1:0";
	mock.answer_tok = 1;
	for (int o; (o = getopt(argc, argv, "l:s:d:r:z:ch")) >= 0; ) {
		switch (o) {
		case 'l': addr = optarg; break;
		case 'c': mock.closing = true; break;
		case 's': mock.slots = (uint32_t)strtoul(optarg, nullptr, 10); break;
		case 'd': mock.delay_ms = (uint32_t)strtoul(optarg, nullptr, 10); break;
		case 'r': mock.tok_per_s = (uint32_t)strtoul(optarg, nullptr, 10); break;
		case 'z': mock.answer_tok = (uint32_t)strtoul(optarg, nullptr, 10); break;
		default:
			fprintf(stderr, "usage: %s [-l host:port] [-s slots] [-d delay_ms] "
			        "[-r tokens_per_s] [-z answer_tokens] [-c]\n", argv[0]);
			return 2;
		}
	}

	char host[256] = "127.0.0.1";
	char const *port = addr;
	char const *colon = strrchr(addr, ':');
	if (colon) {
		size_t n = (size_t)(colon - addr);
		if (n >= sizeof host)
			return 2;
		memcpy(host, addr, n);
		host[n] = '\0';
		port = colon + 1;
	}

	/* Pace tokens and delays to the microsecond, not the default 50 us slack. */
	prctl(PR_SET_TIMERSLACK, 1UL);
	mock.listen_fd = listen_on(host, port);
	mock.epfd = epoll_create1(EPOLL_CLOEXEC);
	if (mock.listen_fd < 0 || mock.epfd < 0) {
		perror("mock: listen");
		return 1;
	}

	struct sockaddr_storage ss;
	socklen_t len = sizeof ss;
	char pbuf[8] = "?";
	if (!getsockname(mock.listen_fd, (struct sockaddr *)&ss, &len))
		getnameinfo((struct sockaddr *)&ss, len, nullptr, 0, pbuf, sizeof pbuf,
		            NI_NUMERICSERV);
	fprintf(stderr, "mock: http://127.0.0.1:%s/v1/chat/completions "
	        "slots=%u delay=%ums rate=%u/s answer=%utok\n", pbuf, mock.slots,
	        mock.delay_ms, mock.tok_per_s, mock.answer_tok);

	/* Signals are only let in while waiting, so a stop is never missed. */
	sigset_t stops, waitmask;
	sigemptyset(&stops);
	sigaddset(&stops, SIGINT);
	sigaddset(&stops, SIGTERM);
	sigprocmask(SIG_BLOCK, &stops, &waitmask);
	sigdelset(&waitmask, SIGINT);
	sigdelset(&waitmask, SIGTERM);
	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
	signal(SIGPIPE, SIG_IGN);

	struct epoll_event lev = { .events = EPOLLIN, .data.ptr = nullptr };
	epoll_ctl(mock.epfd, EPOLL_CTL_ADD, mock.listen_fd, &lev);

	while (!mock.stop) {
		uint64_t next = run_timers();
		struct timespec ts;
		struct timespec *to = nullptr;
		if (next != UINT64_MAX) {
			uint64_t t = now_ns();
			uint64_t dt = next > t ? next - t : 0;
			ts = (struct timespec){ .tv_sec = (time_t)(dt / 1'000'000'000u),
			                        .tv_nsec = (long)(dt % 1'000'000'000u) };
			to = &ts;
		}
		struct epoll_event evs[256];
		int n = epoll_pwait2(mock.epfd, evs, 256, to, &waitmask);
		for (int i = 0; i < n; ++i) {
			if (!evs[i].data.ptr)
				accept_conns();
			else
				on_event(evs[i].data.ptr, evs[i].events);
		}
	}

	fprintf(stderr, "mock: requests=%llu served=%llu cancelled=%llu active=%u "
	        "queued=%u peak=%u streams=%llu\n", (unsigned long long)mock.requests,
	        (unsigned long long)mock.served, (unsigned long long)mock.cancelled,
	        mock.active, mock.queued, mock.peak, (unsigned long long)mock.streams);
	return 0;
}
