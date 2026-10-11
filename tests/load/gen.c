/** @file Open-loop load generator for askai's MCP Streamable HTTP endpoint.
 *
 * The probe is an open-loop Poisson arrival process of cheap requests in both
 * protocol eras askai speaks (legacy ping, tools/list and initialize; the
 * 2026-07-28 server/discover and tools/list), plus a ping on a fresh connection
 * that sees accept starvation.  Each arrival's latency is measured from its
 * *scheduled* send time, never from when a busy generator got around to it, so
 * a server stall cannot be hidden by coordinated omission: an arrival that
 * cannot be sent on time still accrues latency against its schedule, and one
 * that is never answered is recorded as a timeout at its age.  Every arrival is
 * accounted for (sched = ok + err + timeout + lost).  A dispatch-timed control
 * histogram of the same probes shows what an omission-prone probe would report.
 *
 * The probe's keep-alive connections take the arrivals of the cheap kinds in
 * turn, each queueing its own, so a connection the server holds back builds
 * up latency rather than shedding its arrivals to the others.
 *
 * Background modes (idle keep-alive, slow loris, slow bodies, slow readers, a
 * repeated request pipeline, malformed, hostile and dense requests, connection
 * churn, tool-call jobs, tool calls that hang up on their event stream,
 * legacy tool calls that a notifications/cancelled on another connection
 * cancels, a flooder's many tool calls in one session and interactive
 * sessions' one at a time) run alongside to stress the server while the
 * probe measures the damage.  The
 * pipeline and the malformed, hostile and dense modes time each batch or body
 * they send; with --solo, they run one after another, alone and without the
 * probe, a few rounds each, for a baseline of those times (SOLO lines).
 *
 * Besides the latencies, it reports how evenly the damage fell (FAIR lines):
 * the spread of the probe connections' p90 and p99, and of the probe kinds',
 * against their medians, and the tool-call connections' completions.  SIGINT
 * ends the run early, as if its duration were up.
 *
 * All socket access goes through @ref transport, so an S7 TLS client can be
 * dropped in without touching the probe or the load modes.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "hist.h"

/** Byte transport for one connection; S7 swaps @ref tcp for a TLS variant. */
struct transport {
	char const *host;
	char const *port;
	int     (*dial)  (struct transport *t); /**< start a non-blocking connect */
	ssize_t (*recv)  (int fd, void *buf, size_t len);
	ssize_t (*send)  (int fd, void const *buf, size_t len);
	void    (*close) (int fd);
	uint32_t    src;   /**< IPv4 base the dials take source addresses after; 0: any */
	uint32_t    dials; /**< connects started, which pick the source address */
	uint32_t    per;   /**< dials per source address; 0: PER_SRC */
};

/** Wire requests: the probe kinds' and the tool-call jobs', in both eras. */
enum req_id {
	RQ_PING, RQ_LIST, RQ_INIT, RQ_DISCOVER, RQ_LIST26, RQ_CALL, RQ_CALL26, RQ_N
};

#define RPC(id, method, params) \
	"{\"jsonrpc\":\"2.0\",\"id\":" #id ",\"method\":\"" method "\"" params "}"
#define META \
	"\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"," \
	"\"io.modelcontextprotocol/clientCapabilities\":{}}"
#define ERA26(method) \
	"MCP-Protocol-Version: 2026-07-28\r\nMcp-Method: " method "\r\n"
#define REQ(hdrs, body) { hdrs, body, sizeof body - 1 }

/* Headers and bodies as tests/askai.py sends them. */
static struct {
	char const *hdrs;     /**< era headers; empty for the legacy era */
	char const *body;
	size_t      body_len;
} const req_tmpl[RQ_N] = {
	[RQ_PING]     = REQ("", RPC(1, "ping", "")),
	[RQ_LIST]     = REQ("", RPC(1, "tools/list", "")),
	[RQ_INIT]     = REQ("", RPC(1, "initialize", ",\"params\":{"
	                    "\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},"
	                    "\"clientInfo\":{\"name\":\"probe\",\"version\":\"1\"}}")),
	[RQ_DISCOVER] = REQ(ERA26("server/discover"),
	                    RPC(1, "server/discover", ",\"params\":{" META "}")),
	[RQ_LIST26]   = REQ(ERA26("tools/list"),
	                    RPC(1, "tools/list", ",\"params\":{" META "}")),
	[RQ_CALL]     = REQ("", RPC(7, "tools/call", ",\"params\":{\"name\":"
	                    "\"ask_ai\",\"arguments\":{\"prompt\":\"Q\"}}")),
	[RQ_CALL26]   = REQ(ERA26("tools/call") "Mcp-Name: ask_ai\r\n",
	                    RPC(7, "tools/call", ",\"params\":{\"name\":\"ask_ai\","
	                    "\"arguments\":{\"prompt\":\"Q\"}," META "}")),
};

#undef REQ
#undef ERA26
#undef META
#undef RPC

static char   req_buf[RQ_N][1024];
static size_t req_len[RQ_N];

enum probe_kind {
	PK_PING, PK_LIST, PK_INIT, PK_DISCOVER, PK_LIST26, PK_FRESH, PK_N
};

static char const *const kind_name[PK_N] = {
	"ping", "tools/list", "initialize", "server/discover", "tools/list@2026",
	"fresh-ping",
};

/* The request each kind sends; fresh-ping sends it on a new connection. */
static uint8_t const kind_req[PK_N] = {
	RQ_PING, RQ_LIST, RQ_INIT, RQ_DISCOVER, RQ_LIST26, RQ_PING,
};

/** What became of scheduled arrivals; sched = ok + err + timeout + lost. */
enum fate { FATE_SCHED, FATE_OK, FATE_ERR, FATE_TIMEOUT, FATE_LOST, FATE_N };

static char const *const fate_name[FATE_N] = {
	"sched", "ok", "err", "timeout", "lost",
};

/** Histograms beyond the per-kind ones. */
enum {
	H_ALL = PK_N, /**< every kind, timed from the schedule */
	H_CO,         /**< every kind, timed from the dispatch (the control) */
	H_N,
};

enum {
	REDIAL_NS        = 100'000'000,   /**< pause before redialling a failed connect */
	CHURN_SLOTS      = 256,           /**< churn connections in flight at most */
	CHURN_TIMEOUT_NS = 2'000'000'000, /**< give-up age of a churn connection */
	PER_SRC          = 128,           /**< dials per source address, under
	                                       askai's per-client cap of 256 */
};

static atomic_bool g_stop;

static void
on_interrupt (int sig)
{
	(void)sig;
	atomic_store(&g_stop, true);
}

static uint64_t
now_ns (void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1'000'000'000u + (uint64_t)ts.tv_nsec;
}

/* --- plain-TCP transport ------------------------------------------------- */

/* Connect to the server; with a source base, from a fresh loopback address
 * for every PER_SRC dials, so that load modes look like many clients. */
static int
tcp_dial (struct transport *t)
{
	struct addrinfo const hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_NUMERICSERV,
	};
	struct addrinfo *list;
	if (getaddrinfo(t->host, t->port, &hints, &list))
		return -1;
	int fd = -1;
	for (struct addrinfo *ai = list; ai && fd < 0; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
		            ai->ai_protocol);
		int one = 1;
		if (fd < 0)
			continue;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		if (t->src && ai->ai_family == AF_INET) {
			struct sockaddr_in const src = {
				.sin_family = AF_INET,
				.sin_addr.s_addr = htonl(t->src + 1 + t->dials++ /
				                         (t->per ? t->per : PER_SRC))
			};
			setsockopt(fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof one);
			(void)bind(fd, (struct sockaddr const *)&src, sizeof src);
		}
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) && errno != EINPROGRESS) {
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(list);
	return fd;
}

static ssize_t
tcp_recv (int    fd,
          void  *buf,
          size_t len)
{
	return recv(fd, buf, len, 0);
}

static ssize_t
tcp_send (int         fd,
          void const *buf,
          size_t      len)
{
	return send(fd, buf, len, MSG_NOSIGNAL);
}

static void
tcp_close (int fd)
{
	close(fd);
}

static struct transport tcp = {
	.dial = tcp_dial,
	.recv = tcp_recv,
	.send = tcp_send,
	.close = tcp_close,
};

/* --- open-loop Poisson probe --------------------------------------------- */

enum pconn_state { PC_DOWN, PC_CONNECTING, PC_IDLE, PC_BUSY };

struct sched_ent {
	uint64_t sched_ns;
	uint8_t  kind;
};

/** Pending arrivals, FIFO. */
struct ring {
	struct sched_ent *ent;
	size_t            cap;
	size_t            head;
	size_t            tail;
};

struct pconn {
	struct ring q;           /**< a keep-alive slot's pending arrivals */
	uint64_t    sched_ns;    /**< the in-flight probe's scheduled send time */
	uint64_t    sent_ns;     /**< when it was handed to this connection */
	uint64_t    due_ns;      /**< its give-up time, or a down slot's redial time */
	char const *req;
	size_t      need;
	size_t      sent;
	size_t      rx;
	size_t      head;        /**< bytes up to and including the header terminator */
	size_t      clen;        /**< response Content-Length */
	int         fd;
	uint8_t     state;
	uint8_t     kind;
	bool        fresh;       /**< a new connection per probe */
	char        buf[16 << 10];
};


struct probe {
	struct transport       *t;
	struct hist            *hist;       /**< [H_N] */
	struct hist            *conn_hist;  /**< [nka]: each keep-alive slot's */
	struct pconn           *pool;       /**< keep-alive slots, then fresh ones */
	struct ring             fresh;      /**< pending fresh-connection arrivals */
	uint64_t                n[PK_N][FATE_N];
	uint64_t                connfail;   /**< keep-alive connects that failed */
	uint64_t                rng;
	uint64_t                timeout_ns; /**< give-up age, from the schedule */
	int                     nka;        /**< keep-alive slots */
	int                     npool;
	int                     turn;       /**< the keep-alive slot next in turn */
	int                     cursor;     /**< where the free fresh-slot scan resumes */
	int                     epfd;
	uint8_t                 kind_first; /**< the kinds this probe rotates through */
	uint8_t                 kind_end;
};

static double
rng_unit (uint64_t *s)
{
	/* splitmix64 -> (0,1] */
	uint64_t z = (*s += 0x9E3779B97F4A7C15u);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9u;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBu;
	z ^= z >> 31;
	return ((double)(z >> 11) + 1.0) * (1.0 / 9007199254740992.0);
}

static bool
ring_push (struct ring *r,
           uint64_t     sched,
           uint8_t      kind)
{
	size_t used = r->tail - r->head;
	if (used == r->cap) {
		size_t cap = r->cap ? r->cap * 2 : 1024;
		struct sched_ent *e = malloc(cap * sizeof *e);
		if (!e)
			return false;
		for (size_t i = 0; i < used; ++i)
			e[i] = r->ent[(r->head + i) % r->cap];
		free(r->ent);
		r->ent = e;
		r->cap = cap;
		r->head = 0;
		r->tail = used;
	}
	r->ent[r->tail++ % r->cap] = (struct sched_ent){ sched, kind };
	return true;
}

/* Count an arrival's fate.  An answer or a timeout is also a latency sample,
 * timed from the schedule and, if it was sent, from the dispatch; one sent on
 * a keep-alive slot @p pc counts for that slot too. */
static void
probe_settle (struct probe       *p,
              struct pconn const *pc,
              uint8_t             kind,
              uint64_t            sched_ns,
              uint64_t            sent_ns,
              enum fate           fate)
{
	p->n[kind][fate]++;
	if (fate != FATE_OK && fate != FATE_TIMEOUT)
		return;
	uint64_t now = now_ns();
	hist_record(&p->hist[kind], now - sched_ns);
	hist_record(&p->hist[H_ALL], now - sched_ns);
	if (sent_ns)
		hist_record(&p->hist[H_CO], now - sent_ns);
	if (pc && !pc->fresh)
		hist_record(&p->conn_hist[pc - p->pool], now - sched_ns);
}

static void
pc_arm (struct probe *p,
        struct pconn *pc,
        uint32_t      events)
{
	struct epoll_event ev = { .events = events, .data.ptr = pc };
	epoll_ctl(p->epfd, EPOLL_CTL_MOD, pc->fd, &ev);
}

static void pc_connect (struct probe *p, struct pconn *pc);

/* Drop the connection, settling its in-flight probe as @p fate.  A keep-alive
 * slot redials at once, or after REDIAL_NS if it never got connected. */
static void
pc_drop (struct probe *p,
         struct pconn *pc,
         enum fate     fate)
{
	if (pc->state == PC_BUSY)
		probe_settle(p, pc, pc->kind, pc->sched_ns, pc->sent_ns, fate);
	if (pc->fd >= 0) {
		epoll_ctl(p->epfd, EPOLL_CTL_DEL, pc->fd, nullptr);
		p->t->close(pc->fd);
		pc->fd = -1;
	}
	bool failed = pc->state == PC_CONNECTING;
	pc->state = PC_DOWN;
	if (pc->fresh)
		return;
	if (!failed) {
		pc_connect(p, pc);
		return;
	}
	p->connfail++;
	pc->due_ns = now_ns() + REDIAL_NS;
}

static void
pc_connect (struct probe *p,
            struct pconn *pc)
{
	pc->fd = p->t->dial(p->t);
	pc->state = PC_CONNECTING;
	struct epoll_event ev = { .events = EPOLLOUT, .data.ptr = pc };
	if (pc->fd < 0 || epoll_ctl(p->epfd, EPOLL_CTL_ADD, pc->fd, &ev))
		pc_drop(p, pc, FATE_ERR);
}

static void
pc_send (struct probe *p,
         struct pconn *pc)
{
	while (pc->sent < pc->need) {
		ssize_t n = p->t->send(pc->fd, pc->req + pc->sent, pc->need - pc->sent);
		if (n > 0) {
			pc->sent += (size_t)n;
		} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			pc_arm(p, pc, EPOLLOUT | EPOLLRDHUP);
			return;
		} else if (!(n < 0 && errno == EINTR)) {
			pc_drop(p, pc, FATE_ERR);
			return;
		}
	}
	pc_arm(p, pc, EPOLLIN | EPOLLRDHUP);
}

/* Read the response; on completion settle the probe and free the slot. */
static void
pc_recv (struct probe *p,
         struct pconn *pc)
{
	for (;;) {
		if (pc->rx == sizeof pc->buf) {
			pc_drop(p, pc, FATE_ERR); /* oversized response for a cheap request */
			return;
		}
		ssize_t n = p->t->recv(pc->fd, pc->buf + pc->rx, sizeof pc->buf - pc->rx);
		if (n > 0) {
			pc->rx += (size_t)n;
		} else if (!n) {
			pc_drop(p, pc, FATE_ERR);
			return;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			break;
		} else if (errno != EINTR) {
			pc_drop(p, pc, FATE_ERR);
			return;
		}

		if (!pc->head) {
			char *end = memmem(pc->buf, pc->rx, "\r\n\r\n", 4);
			if (!end)
				continue;
			pc->head = (size_t)(end - pc->buf) + 4;
			char *cl = memmem(pc->buf, pc->head, "Content-Length:", 15);
			if (!cl)
				cl = memmem(pc->buf, pc->head, "content-length:", 15);
			pc->clen = cl ? strtoul(cl + 15, nullptr, 10) : 0;
		}
		if (pc->rx >= pc->head + pc->clen) {
			int status = pc->rx > 12 ? atoi(pc->buf + 9) : 0;
			probe_settle(p, pc, pc->kind, pc->sched_ns, pc->sent_ns,
			             status >= 200 && status < 300 ? FATE_OK : FATE_ERR);
			bool close_it = pc->fresh ||
			                memmem(pc->buf, pc->head, "Connection: close", 17) ||
			                memmem(pc->buf, pc->head, "connection: close", 17);
			pc->state = PC_IDLE;
			pc->rx = pc->head = pc->clen = 0;
			if (close_it)
				pc_drop(p, pc, FATE_ERR); /* nothing in flight to settle */
			else
				pc_arm(p, pc, EPOLLRDHUP);
			return;
		}
	}
}

/* A free fresh slot, round-robin from the last one used, or nullptr. */
static struct pconn *
probe_free (struct probe *p)
{
	int n = p->npool - p->nka;
	for (int k = 0; k < n; ++k) {
		int i = p->nka + (p->cursor + k) % n;
		if (p->pool[i].state == PC_DOWN) {
			p->cursor = i - p->nka + 1;
			return &p->pool[i];
		}
	}
	return nullptr;
}

/* Send arrival @p e on slot @p pc; latency stays timed from the schedule. */
static void
probe_send (struct probe     *p,
            struct pconn     *pc,
            struct sched_ent  e,
            uint64_t          now)
{
	pc->state = PC_BUSY;
	pc->kind = e.kind;
	pc->req = req_buf[kind_req[e.kind]];
	pc->need = req_len[kind_req[e.kind]];
	pc->sent = pc->rx = pc->head = pc->clen = 0;
	pc->sched_ns = e.sched_ns;
	pc->sent_ns = now;
	pc->due_ns = e.sched_ns + p->timeout_ns;
	if (!pc->fresh) {
		pc_send(p, pc);
		return;
	}
	/* The connect is part of a fresh probe: send once writable. */
	pc->fd = p->t->dial(p->t);
	struct epoll_event ev = { .events = EPOLLOUT | EPOLLRDHUP, .data.ptr = pc };
	if (pc->fd < 0 || epoll_ctl(p->epfd, EPOLL_CTL_ADD, pc->fd, &ev))
		pc_drop(p, pc, FATE_ERR);
}

/* Hand pending arrivals on: each idle keep-alive slot its next own, and fresh
 * arrivals to free fresh slots. */
static void
probe_dispatch (struct probe *p,
                uint64_t      now)
{
	for (int i = 0; i < p->nka; ++i) {
		struct pconn *pc = &p->pool[i];
		if (pc->state == PC_IDLE && pc->q.head != pc->q.tail)
			probe_send(p, pc, pc->q.ent[pc->q.head++ % pc->q.cap], now);
	}
	struct ring *r = &p->fresh;
	for (struct pconn *pc; r->head != r->tail && (pc = probe_free(p)); )
		probe_send(p, pc, r->ent[r->head++ % r->cap], now);
}

static void
probe_event (struct probe *p,
             struct pconn *pc,
             uint32_t      events)
{
	if (pc->state == PC_CONNECTING) {
		int err = 0;
		socklen_t len = sizeof err;
		if ((events & (EPOLLERR | EPOLLHUP)) ||
		    getsockopt(pc->fd, SOL_SOCKET, SO_ERROR, &err, &len) || err) {
			pc_drop(p, pc, FATE_ERR);
			return;
		}
		pc->state = PC_IDLE;
		pc_arm(p, pc, EPOLLRDHUP);
		return;
	}
	/* On an idle keep-alive, a hang-up just means redial. */
	if (events & (EPOLLERR | EPOLLHUP) || (pc->state == PC_IDLE && events & EPOLLRDHUP))
		pc_drop(p, pc, FATE_ERR);
	else if (pc->state != PC_BUSY)
		return;
	else if (events & EPOLLOUT && pc->sent < pc->need)
		pc_send(p, pc);
	else if (events & (EPOLLIN | EPOLLRDHUP))
		pc_recv(p, pc);
}

/* Time out the arrivals of @p r, queued for keep-alive slot @p pc or for
 * nullptr, a fresh one, that are older than the timeout; returns the nearest
 * due time of the rest, or @p next if sooner. */
static uint64_t
ring_expire (struct probe *p,
             struct pconn *pc,
             struct ring  *r,
             uint64_t      now,
             uint64_t      next)
{
	while (r->head != r->tail) {
		struct sched_ent e = r->ent[r->head % r->cap];
		uint64_t due = e.sched_ns + p->timeout_ns;
		if (due > now)
			return due < next ? due : next;
		r->head++;
		probe_settle(p, pc, e.kind, e.sched_ns, 0, FATE_TIMEOUT); /* never sent */
	}
	return next;
}

/* Time out arrivals and probes older than the timeout, redial down keep-alive
 * slots that are due, and return the nearest such due time. */
static uint64_t
probe_timers (struct probe *p,
              uint64_t      now)
{
	uint64_t next = ring_expire(p, nullptr, &p->fresh, now, UINT64_MAX);
	for (int i = 0; i < p->npool; ++i) {
		struct pconn *pc = &p->pool[i];
		next = ring_expire(p, pc, &pc->q, now, next);
		if (pc->state != PC_BUSY && (pc->state != PC_DOWN || pc->fresh))
			continue;
		if (pc->due_ns > now) {
			if (pc->due_ns < next)
				next = pc->due_ns;
		} else if (pc->state == PC_BUSY) {
			pc_drop(p, pc, FATE_TIMEOUT);
		} else {
			pc_connect(p, pc);
		}
	}
	return next;
}

static bool
probe_busy (struct probe const *p)
{
	bool busy = p->fresh.head != p->fresh.tail;
	for (int i = 0; !busy && i < p->npool; ++i)
		busy = p->pool[i].state == PC_BUSY || p->pool[i].q.head != p->pool[i].q.tail;
	return busy;
}

static bool
probe_init (struct probe           *p,
            struct transport       *t,
            int                     nka,
            int                     nfresh,
            uint64_t                timeout_ns)
{
	*p = (struct probe){
		.t = t,
		.hist = malloc(H_N * sizeof *p->hist),
		.conn_hist = malloc(((size_t)nka + 1) * sizeof *p->conn_hist),
		.pool = calloc((size_t)(nka + nfresh), sizeof *p->pool),
		.rng = 0x2545F4914F6CDD1Du ^ now_ns(),
		.timeout_ns = timeout_ns,
		.nka = nka,
		.npool = nka + nfresh,
		.epfd = epoll_create1(EPOLL_CLOEXEC),
		.kind_first = nka ? PK_PING : PK_FRESH,
		.kind_end = nfresh ? PK_N : PK_FRESH,
	};
	for (int i = 0; p->hist && i < H_N; ++i)
		hist_init(&p->hist[i]);
	for (int i = 0; p->conn_hist && i <= nka; ++i)
		hist_init(&p->conn_hist[i]);
	for (int i = 0; p->pool && i < p->npool; ++i) {
		p->pool[i].fd = -1;
		p->pool[i].fresh = i >= nka;
	}
	return p->hist && p->conn_hist && p->pool && p->epfd >= 0;
}

static void
probe_fini (struct probe *p)
{
	for (int i = 0; p->pool && i < p->npool; ++i) {
		if (p->pool[i].fd >= 0)
			p->t->close(p->pool[i].fd);
		free(p->pool[i].q.ent);
	}
	if (p->epfd >= 0)
		close(p->epfd);
	free(p->fresh.ent);
	free(p->pool);
	free(p->conn_hist);
	free(p->hist);
}

/* Establish the keep-alive pool and hold it, before any background load
 * competes for the server's connection slots.  Returns how many are ready. */
static int
probe_warmup (struct probe *p)
{
	for (int i = 0; i < p->nka; ++i)
		pc_connect(p, &p->pool[i]);
	uint64_t warm_end = now_ns() + 2'000'000'000u;
	struct epoll_event evs[256];
	int ready = 0;
	while (ready < p->nka && now_ns() < warm_end) {
		int n = epoll_wait(p->epfd, evs, 256, 100);
		for (int i = 0; i < n; ++i)
			probe_event(p, evs[i].data.ptr, evs[i].events);
		ready = 0;
		for (int i = 0; i < p->nka; ++i)
			ready += p->pool[i].state == PC_IDLE;
	}
	return ready;
}

/* Generate arrivals for @p duration_s seconds (0: until g_stop), wait for the
 * answers or timeouts, and count whatever is still pending as lost. */
static void
probe_run (struct probe *p,
           double        rate_hz,
           double        duration_s)
{
	struct epoll_event evs[256];
	uint64_t now = now_ns();
	uint64_t end = duration_s > 0 ? now + (uint64_t)(duration_s * 1e9) : UINT64_MAX;
	uint64_t next_send = now;
	uint64_t drain_end = UINT64_MAX;
	uint8_t kind = p->kind_first;

	for (;; now = now_ns()) {
		if (end > now && atomic_load_explicit(&g_stop, memory_order_relaxed))
			end = now;
		for (; next_send <= now && next_send < end;
		     next_send += (uint64_t)(-log(rng_unit(&p->rng)) / rate_hz * 1e9)) {
			p->n[kind][FATE_SCHED]++;
			struct ring *r = kind == PK_FRESH ? &p->fresh
			               : &p->pool[p->turn++ % p->nka].q;
			if (!ring_push(r, next_send, kind))
				p->n[kind][FATE_LOST]++;
			if (++kind == p->kind_end)
				kind = p->kind_first;
		}
		uint64_t nearest = probe_timers(p, now);
		probe_dispatch(p, now);

		if (next_send < end) {
			if (next_send < nearest)
				nearest = next_send;
		} else if (!probe_busy(p)) {
			break;
		} else if (drain_end == UINT64_MAX) {
			drain_end = now + p->timeout_ns + 1'000'000'000u;
		} else if (now >= drain_end) {
			break;
		}
		uint64_t dt = nearest > now ? nearest - now : 0;
		if (dt > 20'000'000u)
			dt = 20'000'000u;
		struct timespec ts = { .tv_sec = (time_t)(dt / 1'000'000'000u),
		                       .tv_nsec = (long)(dt % 1'000'000'000u) };
		int n = epoll_pwait2(p->epfd, evs, 256, &ts, nullptr);
		for (int i = 0; i < n; ++i)
			probe_event(p, evs[i].data.ptr, evs[i].events);
	}

	/* Timeouts run from the schedule, so nothing should be left by now. */
	for (int i = -1; i < p->npool; ++i) {
		struct ring *r = i < 0 ? &p->fresh : &p->pool[i].q;
		for (; r->head != r->tail; r->head++)
			p->n[r->ent[r->head % r->cap].kind][FATE_LOST]++;
		if (i >= 0 && p->pool[i].state == PC_BUSY) {
			p->n[p->pool[i].kind][FATE_LOST]++;
			p->pool[i].state = PC_IDLE;
		}
	}
}

/* --- background load modes ----------------------------------------------- */

enum {
	BG_MAX  = 14, /**< one thread per mode */
	BG_KEYS = 5, /**< counters per mode */
};

/** One background load mode, run by its own thread, from its own sources. */
struct bg {
	struct transport        tcp;
	struct transport       *t;
	char const             *name;
	char const             *key[BG_KEYS]; /**< what n[] counts; nullptr: unused */
	struct probe           *probe;        /**< churn's own probe */
	struct hist            *lat;          /**< bursts and the pipeline: the time
	                                           each body or batch took */
	uint64_t               *per;          /**< jobs: completions per connection */
	uint64_t                n[BG_KEYS];   /**< read only after the join */
	uint64_t                rounds;       /**< bodies or batches to send before
	                                           stopping; 0: until the end */
	double                  rate;
	int                     count;
	int                     gap_ms;
	int                     mode;
};

/* Finish a non-blocking connect; true once writable with no SO_ERROR. */
static bool
wait_writable (int fd,
               int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLOUT };
	if (poll(&pfd, 1, timeout_ms) != 1)
		return false;
	int err = 0;
	socklen_t len = sizeof err;
	return !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) && !err;
}

enum hold_mode { HOLD_IDLE, HOLD_LORIS, HOLD_SLOWBODY, HOLD_SLOWREAD };
enum { HOLD_OPENED, HOLD_SERVED, HOLD_CLOSED, HOLD_PIPED };

/* Hold @p count connections in one hold mode: idle keep-alives (one answered
 * ping, then silence), loris heads and slow bodies trickled one byte per gap,
 * or slow readers that pipeline requests and never read the responses. */
static void *
hold_thread (void *arg)
{
	struct bg *b = arg;
	struct pollfd *pfd = calloc((size_t)b->count, sizeof *pfd);
	size_t *piped = calloc((size_t)b->count, sizeof *piped);
	/* A slow reader's pipeline: enough tools/list answers to fill the
	 * server's send buffer and our receive buffer many times over. */
	size_t one = req_len[RQ_LIST];
	size_t batch_len = (64u << 10) / one * one;
	char *batch = b->mode == HOLD_SLOWREAD ? malloc(batch_len) : nullptr;
	if (!pfd || !piped || (b->mode == HOLD_SLOWREAD && !batch))
		goto out;
	for (size_t off = 0; batch && off < batch_len; off += one)
		memcpy(batch + off, req_buf[RQ_LIST], one);

	static char const loris[] = "POST /mcp HTTP/1.1\r\nHost: x\r\nX-Slow: ";
	static char const body[] = "POST /mcp HTTP/1.1\r\nHost: x\r\n"
	                           "Content-Type: application/json\r\n"
	                           "Content-Length: 1048576\r\n\r\n{";
	struct { char const *s; size_t len; } const hello[] = {
		[HOLD_IDLE]     = { req_buf[RQ_PING], req_len[RQ_PING] },
		[HOLD_LORIS]    = { loris, sizeof loris - 1 },
		[HOLD_SLOWBODY] = { body, sizeof body - 1 },
	};
	/* Stop dialling once the server stops taking connections: the rest
	 * would only time out, and the open ones need their poll loop. */
	int n = 0;
	for (int i = 0; i < b->count && !atomic_load(&g_stop); ++i) {
		int fd = b->t->dial(b->t);
		if (fd < 0 || !wait_writable(fd, 2000)) {
			if (fd >= 0)
				b->t->close(fd);
			break;
		}
		if (b->mode == HOLD_SLOWREAD)
			setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &(int){ 4096 }, sizeof (int));
		else
			b->t->send(fd, hello[b->mode].s, hello[b->mode].len);
		/* A slow reader only watches for the server hanging up. */
		pfd[n].fd = fd;
		pfd[n].events = b->mode == HOLD_SLOWREAD ? POLLRDHUP : POLLIN;
		++n;
	}
	b->n[HOLD_OPENED] = (uint64_t)n;

	bool trickle = b->mode == HOLD_LORIS || b->mode == HOLD_SLOWBODY;
	for (;;) {
		/* Top the slow readers' pipelines up as the server takes them in. */
		for (int i = 0; batch && i < n; ++i)
			while (pfd[i].fd >= 0 && piped[i] < batch_len) {
				ssize_t s = b->t->send(pfd[i].fd, batch + piped[i],
				                       batch_len - piped[i]);
				if (s <= 0)
					break;
				piped[i] += (size_t)s;
			}
		if (atomic_load(&g_stop) ||
		    poll(pfd, (nfds_t)n, trickle ? b->gap_ms : 200) < 0)
			break;
		for (int i = 0; i < n; ++i) {
			if (!(pfd[i].revents & (POLLIN | POLLRDHUP | POLLHUP | POLLERR)))
				continue;
			char junk[256];
			ssize_t got = pfd[i].revents & POLLIN
				? b->t->recv(pfd[i].fd, junk, sizeof junk) : 0;
			if (got > 0) {
				b->n[HOLD_SERVED] += got >= 12 &&
				                     !memcmp(junk, "HTTP/1.1 200", 12);
				continue;
			}
			if (got < 0 && (errno == EAGAIN || errno == EINTR))
				continue;
			b->n[HOLD_CLOSED]++;
			b->t->close(pfd[i].fd);
			pfd[i].fd = -1; /* poll skips it */
		}
		for (int i = 0; trickle && i < n; ++i)
			if (pfd[i].fd >= 0)
				b->t->send(pfd[i].fd, b->mode == HOLD_LORIS ? "a" : " ", 1);
	}
	for (int i = 0; i < n; ++i) {
		b->n[HOLD_PIPED] += piped[i] / one;
		if (pfd[i].fd >= 0)
			b->t->close(pfd[i].fd);
	}
out:
	free(batch);
	free(piped);
	free(pfd);
	return nullptr;
}

/*
 * Hostile JSON bodies.  Those of the first three kinds exceed askai's default
 * limits: deep nesting, many values, a huge string.  The others keep to them,
 * so askai parses them in full: 1024 values (empty objects, or 507 keys), a
 * 1 MiB string, and a 1 MiB tool call prompt full of escapes.
 */
enum { HOSTILE_KINDS = 7, HOSTILE_MAX = 4'200'000 };

static char *
hostile_body (int     kind,
              size_t *len)
{
	static char const ping[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\","
	                           "\"params\":{";
	static char const call[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
	                           "\"tools/call\",\"params\":{\"name\":\"ask_ai\","
	                           "\"arguments\":{\"prompt\":\"";
	static char const prose[] = "All work and no play makes \\\"Jack\\\" a dull "
	                            "boy.\\n";
	size_t const max = 1 << 20;
	char *s = malloc(HOSTILE_MAX);
	if (!s)
		return nullptr;
	size_t k = 0;
	switch (kind) {
	case 0:
		for (size_t i = 0; i < 100'000; ++i)
			s[k++] = '[';
		for (size_t i = 0; i < 100'000; ++i)
			s[k++] = ']';
		break;
	case 1:
		s[k++] = '[';
		for (size_t i = 0; i < 400'000; ++i)
			k += (size_t)sprintf(s + k, "%s0", i ? "," : "");
		s[k++] = ']';
		break;
	case 2:
		s[k++] = '"';
		memset(s + k, 'x', 4'000'000);
		k += 4'000'000;
		s[k++] = '"';
		break;
	case 3:
		/* 11 values come before the array's elements. */
		k = (size_t)sprintf(s, "%s\"a\":[", ping);
		for (size_t i = 0; i < 1013; ++i)
			k += (size_t)sprintf(s + k, "%s{}", i ? "," : "");
		k += (size_t)sprintf(s + k, "]}}");
		break;
	case 4:
		/* 9 values come before the members. */
		k = (size_t)sprintf(s, "%s", ping);
		for (size_t i = 0; i < 507; ++i)
			k += (size_t)sprintf(s + k, "%s\"k%zu\":0", i ? "," : "", i);
		k += (size_t)sprintf(s + k, "}}");
		break;
	case 5:
		k = (size_t)sprintf(s, "%s\"a\":\"", ping);
		memset(s + k, 'x', max - 3 - k);
		k = max - 3;
		k += (size_t)sprintf(s + k, "\"}}");
		break;
	default:
		k = (size_t)sprintf(s, "%s", call);
		while (k + sizeof prose - 1 <= max - 4) {
			memcpy(s + k, prose, sizeof prose - 1);
			k += sizeof prose - 1;
		}
		memset(s + k, '.', max - 4 - k);
		k = max - 4;
		k += (size_t)sprintf(s + k, "\"}}}");
	}
	*len = k;
	return s;
}

/*
 * Dense JSON bodies of 1 MiB, each a ping with many small values: 524 k
 * zeros, 349 k empty objects, or 237 k members in objects of 512.  askai
 * admits them only with --max-values raised, and must parse and free them a
 * few at a time.
 */
enum { DENSE_KINDS = 3, DENSE_MEMBERS = 512 };

static char *
dense_body (int     kind,
            size_t *len)
{
	static char const ping[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\","
	                           "\"params\":{\"a\":[";
	size_t const max = 1 << 20;
	char *s = malloc(max);
	if (!s)
		return nullptr;
	size_t k = (size_t)sprintf(s, "%s", ping);
	for (size_t i = 0; k + 24 < max; ++i) {
		size_t m = i % DENSE_MEMBERS;
		k += kind == 0 ? (size_t)sprintf(s + k, "%s0", i ? "," : "")
		   : kind == 1 ? (size_t)sprintf(s + k, "%s{}", i ? "," : "")
		   : (size_t)sprintf(s + k, "%s\"k%zu\":0", !i ? "{" : m ? "," : "},{", m);
	}
	k += (size_t)sprintf(s + k, "%s]}}", kind == 2 ? "}" : "");
	*len = k;
	return s;
}

enum burst_mode { BURST_MALFORMED, BURST_HOSTILE, BURST_DENSE };
enum { BURST_OPENED, BURST_ANSWERED, BURST_REFUSED, BURST_SENT, BURST_OK };
static_assert((int)DENSE_KINDS <= (int)HOSTILE_KINDS);

/* Send a JSON body of @p len bytes whole on @p fd, after its head, unless
 * the stop cuts it short; whether it went whole. */
static bool
burst_send (struct bg  *b,
            int         fd,
            char const *body,
            size_t      len)
{
	char head[160];
	int k = snprintf(head, sizeof head, "POST /mcp HTTP/1.1\r\nHost: x\r\n"
	                 "Content-Type: application/json\r\n"
	                 "Accept: application/json, text/event-stream\r\n"
	                 "Content-Length: %zu\r\n\r\n", len);
	b->t->send(fd, head, (size_t)k);
	size_t off = 0;
	while (off < len && !atomic_load(&g_stop)) {
		ssize_t s = b->t->send(fd, body + off, len - off);
		if (s > 0)
			off += (size_t)s;
		else if (s < 0 && errno != EINTR &&
		         !(errno == EAGAIN && wait_writable(fd, 1000)))
			break;
	}
	return off == len;
}

/* Back-to-back connections, each carrying one malformed request, or one
 * hostile or dense JSON body (rotating through the kinds).  A hostile tool
 * call hangs up once its event stream opens, which cancels it. */
static void *
burst_thread (void *arg)
{
	struct bg *b = arg;
	int kinds = b->mode == BURST_HOSTILE ? HOSTILE_KINDS
	          : b->mode == BURST_DENSE ? DENSE_KINDS : 0;
	char *host[HOSTILE_KINDS] = {};
	size_t hlen[HOSTILE_KINDS] = {};
	for (int i = 0; i < kinds; ++i)
		if (!(host[i] = b->mode == BURST_DENSE ? dense_body(i, &hlen[i])
		                                       : hostile_body(i, &hlen[i])))
			goto out;

	for (uint64_t i = 0; !atomic_load(&g_stop) && (!b->rounds || i < b->rounds); ++i) {
		int fd = b->t->dial(b->t);
		if (fd < 0 || !wait_writable(fd, 1000)) {
			if (fd >= 0)
				b->t->close(fd);
			b->n[BURST_REFUSED]++;
			poll(nullptr, 0, 1); /* avoid a hot spin when the server refuses */
			continue;
		}
		b->n[BURST_OPENED]++;
		uint64_t t0 = now_ns();
		static char const bad[] = "POST /mcp HTTP/1.1\r\nBad Header\r\n\r\n";
		size_t m = kinds ? i % (uint64_t)kinds : 0;
		bool sent = b->mode == BURST_MALFORMED
		          ? b->t->send(fd, bad, sizeof bad - 1) == (ssize_t)(sizeof bad - 1)
		          : burst_send(b, fd, host[m], hlen[m]);
		b->n[BURST_SENT] += sent;
		/* A reply's first bytes, which for a tool call open its event
		 * stream: hanging up then cancels the call.  A body over a limit
		 * may be answered before it is all sent; one the stop cut short
		 * counts neither way. */
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		char junk[512];
		ssize_t r = sent && poll(&pfd, 1, b->mode == BURST_DENSE ? 10'000 : 2000) == 1
		          ? b->t->recv(fd, junk, sizeof junk) : 0;
		if (r > 0) {
			b->n[BURST_ANSWERED]++;
			b->n[BURST_OK] += r > 12 && !memcmp(junk, "HTTP/1.1 200", 12);
			hist_record(b->lat, now_ns() - t0);
		}
		b->t->close(fd);
	}
out:
	for (int i = 0; i < HOSTILE_KINDS; ++i)
		free(host[i]);
	return nullptr;
}

/* Connection churn: open-loop fresh-connection pings at b->rate per second,
 * on a probe of its own, so it is paced and accounted like the probe. */
static void *
churn_thread (void *arg)
{
	struct bg *b = arg;
	probe_run(b->probe, b->rate, 0);
	return nullptr;
}

enum { PIPE_BATCHES, PIPE_REQS, PIPE_GOT, PIPE_DRAIN_US, PIPE_DRAIN_MAX_US };

/* Back-to-back batches of b->count MiB of pings on one connection, sent in
 * bulk so the server must buffer and re-scan a deep pipeline; each batch
 * follows once the previous one is answered, for as long as the run lasts. */
static void *
pipeline_thread (void *arg)
{
	struct bg *b = arg;
	static char const ok[] = "HTTP/1.1 200";
	size_t one = req_len[RQ_PING];
	size_t bn = (256u << 10) / one * one;
	uint64_t want = ((uint64_t)b->count << 20) / one;
	uint64_t total = want * one;
	char *batch = malloc(bn);
	char *buf = malloc((64 << 10) + sizeof ok);
	int fd = b->t->dial(b->t);
	if (!batch || !buf || fd < 0 || !wait_writable(fd, 2000))
		goto out;
	for (size_t off = 0; off < bn; off += one)
		memcpy(batch + off, req_buf[RQ_PING], one);

	uint64_t sent = 0;  /* bytes of this batch */
	uint64_t got = 0;   /* answers to this batch */
	size_t carry = 0;   /* a read's tail, for a status line split over two */
	uint64_t t0 = now_ns();
	struct pollfd pfd = { .fd = fd };
	while (!atomic_load(&g_stop) && (!b->rounds || b->n[PIPE_BATCHES] < b->rounds)) {
		if (got == want) {
			uint64_t ns = now_ns() - t0;
			b->n[PIPE_BATCHES]++;
			b->n[PIPE_DRAIN_US] += ns / 1000;
			if (ns / 1000 > b->n[PIPE_DRAIN_MAX_US])
				b->n[PIPE_DRAIN_MAX_US] = ns / 1000;
			hist_record(b->lat, ns);
			sent = got = 0;
			t0 = now_ns();
			continue;
		}
		pfd.events = POLLIN | (sent < total ? POLLOUT : 0);
		int ready = poll(&pfd, 1, 100); /* short, to notice g_stop */
		if (ready < 0 && errno != EINTR)
			break;
		if (ready <= 0)
			continue;
		if (pfd.revents & POLLOUT) {
			size_t off = (size_t)(sent % bn);
			size_t chunk = bn - off;
			if (chunk > total - sent)
				chunk = (size_t)(total - sent);
			ssize_t s = b->t->send(fd, batch + off, chunk);
			if (s > 0) {
				sent += (uint64_t)s;
				b->n[PIPE_REQS] += (sent / one) - (sent - (uint64_t)s) / one;
			} else if (s < 0 && errno != EAGAIN && errno != EINTR) {
				break;
			}
		}
		if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
			ssize_t r = b->t->recv(fd, buf + carry, 64 << 10);
			if (!r || (r < 0 && errno != EAGAIN && errno != EINTR))
				break;
			if (r < 0)
				continue;
			size_t len = carry + (size_t)r;
			for (char *q = buf; (q = memmem(q, len - (size_t)(q - buf), ok,
			                                sizeof ok - 1)); q += sizeof ok - 1) {
				++got;
				++b->n[PIPE_GOT];
			}
			carry = len < sizeof ok - 2 ? len : sizeof ok - 2;
			memmove(buf, buf + len - carry, carry);
		}
	}
out:
	if (fd >= 0)
		b->t->close(fd);
	free(buf);
	free(batch);
	return nullptr;
}

/*
 * Tool-call jobs: b->count concurrent connections, each looping tools/call,
 * alternately in the legacy and the 2026-07-28 era; askai answers them on
 * event streams, which a job reads to their last chunk.  Each connection
 * comes from an address of its own, so that each is a principal of its own
 * to askai's scheduler.  In hang-up mode (b->mode JOBS_HANGUP) a job instead
 * waits for its event stream to open, then hangs up about HANGUP_MS later,
 * from a half to one and a half times that, which askai must pass upstream
 * as a cancellation unless the answer came first: against an upstream that
 * takes about as long, hang-ups race answers.  In cancel mode (JOBS_CANCEL)
 * a job's legacy call carries a session of its connection's own, and as long
 * after its stream opens, a notifications/cancelled for it goes on a new
 * connection, which askai may well serve on another loop; the call's stream
 * then ends without an event, withdrawn, unless the answer came first.  In
 * flood mode (JOBS_FLOOD) every connection's legacy call is in one session,
 * one principal's, which askai refuses as busy past what it queues for one,
 * whereupon the connection calls again BUSY_MS later.  In interactive mode
 * (JOBS_INTERACTIVE) each connection is a session of its own, which calls
 * again once answered, and each call's time from its sending to its answer
 * counts in the mode's latencies.
 */
enum job_state { JOB_DEAD, JOB_CONNECTING, JOB_SENDING, JOB_READING, JOB_HOLDING };
enum { JOB_SUBMITTED, JOB_COMPLETED, JOB_FAILED, JOB_DROPPED, JOB_HUNGUP };
enum { JOBS, JOBS_HANGUP, JOBS_CANCEL, JOBS_FLOOD, JOBS_INTERACTIVE };
enum { HANGUP_MS = 20, BUSY_MS = 10, JOB_REQ = 512 };

struct job_conn {
	uint64_t done;     /**< calls completed on this connection */
	uint64_t due_ns;   /**< when a holding job hangs up, cancels, or calls
	                        again */
	uint64_t sent_ns;  /**< when its call went */
	size_t   sent;
	size_t   rx;
	size_t   skipped;  /**< reply bytes dropped from the middle of buf */
	size_t   call_len;
	size_t   cancel_len;
	int      fd;
	uint8_t  state;
	uint8_t  req;
	bool     cancelled;  /**< the call's cancellation has gone */
	char     call[JOB_REQ];    /**< cancel mode: the call, with a session */
	char     cancel[JOB_REQ];  /**< and its notifications/cancelled */
	char     buf[8 << 10];
};

/* Build @p c's call and cancellation, in a session of its own, @p i. */
static bool
job_session (struct job_conn *c,
             int              i)
{
	static char const call[] = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":"
	                           "\"tools/call\",\"params\":{\"name\":\"ask_ai\","
	                           "\"arguments\":{\"prompt\":\"Q\"}}}";
	static char const cancel[] = "{\"jsonrpc\":\"2.0\",\"method\":"
	                             "\"notifications/cancelled\",\"params\":"
	                             "{\"requestId\":7,\"reason\":\"load\"}}";
	static char const head[] = "POST /mcp HTTP/1.1\r\nHost: x\r\n"
	                           "Content-Type: application/json\r\n"
	                           "Accept: application/json, text/event-stream\r\n"
	                           "MCP-Protocol-Version: 2025-11-25\r\n"
	                           "Mcp-Session-Id: %032x\r\n%s"
	                           "Content-Length: %zu\r\n\r\n%s";
	int n = snprintf(c->call, sizeof c->call, head, (unsigned)i, "",
	                 sizeof call - 1, call);
	int m = snprintf(c->cancel, sizeof c->cancel, head, (unsigned)i,
	                 "Connection: close\r\n", sizeof cancel - 1, cancel);
	c->call_len = n > 0 ? (size_t)n : 0;
	c->cancel_len = m > 0 ? (size_t)m : 0;
	return n > 0 && (size_t)n < sizeof c->call && m > 0 && (size_t)m < sizeof c->cancel;
}

/* Send @p c's cancellation on a new connection, and wait for its 202; false
 * if it does not come within a second or two. */
static bool
job_cancel (struct bg       *b,
            struct job_conn *c)
{
	int fd = b->t->dial(b->t);
	bool ok = fd >= 0 && wait_writable(fd, 1000) &&
	          b->t->send(fd, c->cancel, c->cancel_len) == (ssize_t)c->cancel_len;
	char got[512];
	size_t len = 0;
	for (struct pollfd pfd = { .fd = fd, .events = POLLIN };
	     ok && len < sizeof got && poll(&pfd, 1, 1000) == 1; ) {
		ssize_t r = b->t->recv(fd, got + len, sizeof got - len);
		if (r <= 0)
			break;
		len += (size_t)r;
	}
	if (fd >= 0)
		b->t->close(fd);
	return ok && len >= 12 && !memcmp(got, "HTTP/1.1 202", 12);
}

static void
job_dial (struct bg       *b,
          int              ep,
          struct job_conn *c)
{
	c->fd = b->t->dial(b->t);
	c->state = JOB_CONNECTING;
	if (c->fd < 0) {
		c->state = JOB_DEAD;
		return;
	}
	struct epoll_event ev = { .events = EPOLLOUT, .data.ptr = c };
	if (epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ev)) {
		b->t->close(c->fd);
		c->fd = -1;
		c->state = JOB_DEAD;
	}
}

static void
job_arm (int              ep,
         struct job_conn *c,
         uint32_t         events)
{
	struct epoll_event ev = { .events = events, .data.ptr = c };
	epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
}

/* Close the connection; a job that askai cut off counts as dropped. */
static void
job_kill (struct bg       *b,
          int              ep,
          struct job_conn *c,
          bool             dropped)
{
	b->n[JOB_DROPPED] += dropped;
	if (c->fd >= 0) {
		epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, nullptr);
		b->t->close(c->fd);
	}
	c->fd = -1;
	c->state = JOB_DEAD;
}

/* The length of the head the buffer starts with, 0 if it is not all in. */
static size_t
job_head (struct job_conn const *c)
{
	char const *end = memmem(c->buf, c->rx, "\r\n\r\n", 4);
	return end ? (size_t)(end - c->buf) + 4 : 0;
}

/* Whether the buffer holds a whole response, with a Content-Length or in
 * chunks up to the last, and if so in @p *ok whether it carries a result.  Of
 * a long one it holds the start and the end: a result begins near the one,
 * and an error flag comes at the other. */
static bool
job_complete (struct job_conn const *c,
              size_t                 head,
              bool                  *ok)
{
	if (memmem(c->buf, head, "Transfer-Encoding: chunked", 26)) {
		if (c->rx < head + 5 || memcmp(c->buf + c->rx - 5, "0\r\n\r\n", 5) ||
		    (c->rx > head + 5 && c->buf[c->rx - 6] != '\n'))
			return false;
	} else {
		char const *cl = memmem(c->buf, head, "Content-Length:", 15);
		if (!cl || c->rx + c->skipped < head + strtoul(cl + 15, nullptr, 10))
			return false;
	}
	*ok = atoi(c->buf + 9) == 200 && memmem(c->buf + head, c->rx - head,
	                                        "\"result\"", 8) &&
	      !memmem(c->buf + head, c->rx - head, "\"isError\":true", 14);
	return true;
}

static void *
jobs_thread (void *arg)
{
	struct bg *b = arg;
	bool hangup = b->mode == JOBS_HANGUP || b->mode == JOBS_CANCEL;
	bool own = b->mode >= JOBS_CANCEL;
	uint64_t rng = 0x2545F4914F6CDD1Du ^ now_ns();
	int ep = epoll_create1(EPOLL_CLOEXEC);
	struct job_conn *jc = calloc((size_t)b->count, sizeof *jc);
	b->per = calloc((size_t)b->count, sizeof *b->per);
	if (ep < 0 || !jc || !b->per) {
		if (ep >= 0)
			close(ep);
		free(jc);
		return nullptr;
	}
	for (int i = 0; i < b->count; ++i) {
		jc[i].req = i & 1 ? RQ_CALL26 : RQ_CALL;
		if (own && !job_session(&jc[i], b->mode == JOBS_FLOOD ? 0xf100d
		                                : b->mode == JOBS_CANCEL ? i : 0x1000 + i))
			continue;
		job_dial(b, ep, &jc[i]);
	}

	while (!atomic_load(&g_stop)) {
		struct epoll_event evs[128];
		int n = epoll_pwait2(ep, evs, 128, &(struct timespec){
			.tv_nsec = b->mode ? 2'000'000 : 100'000'000 }, nullptr);
		for (int i = 0; i < n; ++i) {
			struct job_conn *c = evs[i].data.ptr;
			uint32_t e = evs[i].events;
			char const *req = own ? c->call : req_buf[c->req];
			size_t need = own ? c->call_len : req_len[c->req];
			if (c->state == JOB_HOLDING)
				continue;
			if (e & (EPOLLERR | EPOLLHUP) && c->state != JOB_READING) {
				job_kill(b, ep, c, true);
				continue;
			}
			if (c->state == JOB_CONNECTING) {
				int err = 0;
				socklen_t l = sizeof err;
				getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &l);
				if (err) {
					job_kill(b, ep, c, true);
					continue;
				}
				c->state = JOB_SENDING;
				c->sent = 0;
			}
			if (c->state == JOB_SENDING) {
				ssize_t s = b->t->send(c->fd, req + c->sent, need - c->sent);
				if (s < 0 && errno != EAGAIN && errno != EINTR) {
					job_kill(b, ep, c, true);
					continue;
				}
				if (s > 0)
					c->sent += (size_t)s;
				if (c->sent < need) {
					job_arm(ep, c, EPOLLOUT);
					continue;
				}
				b->n[JOB_SUBMITTED]++;
				c->state = JOB_READING;
				c->rx = c->skipped = 0;
				c->cancelled = false;
				c->sent_ns = now_ns();
				job_arm(ep, c, EPOLLIN);
			}
			if (c->state != JOB_READING || !(e & (EPOLLIN | EPOLLHUP | EPOLLERR)))
				continue;
			/* Of a long reply, keep the head, a little more and the end. */
			size_t head = job_head(c);
			size_t keep = head + (1 << 10);
			if (head && c->rx == sizeof c->buf && keep < c->rx / 2) {
				memmove(c->buf + keep, c->buf + c->rx - keep, keep);
				c->skipped += c->rx - 2 * keep;
				c->rx = 2 * keep;
			}
			size_t room = sizeof c->buf - c->rx;
			ssize_t r = room ? b->t->recv(c->fd, c->buf + c->rx, room) : -2;
			if (r == -1 && (errno == EAGAIN || errno == EINTR))
				continue;
			if (r <= 0) {
				job_kill(b, ep, c, true);
				continue;
			}
			c->rx += (size_t)r;
			head = job_head(c);
			bool ok;
			if (head && job_complete(c, head, &ok)) {
				/* A cancelled call's stream ends without an event; a
				 * flooder's call refused as busy calls again a little
				 * later. */
				bool quiet = c->cancelled && !ok && atoi(c->buf + 9) == 200 &&
				             !memmem(c->buf + head, c->rx - head, "data:", 5);
				bool busy = b->mode == JOBS_FLOOD && !ok &&
				            memmem(c->buf + head, c->rx - head, "try again", 9);
				b->n[ok ? JOB_COMPLETED : quiet || busy ? JOB_HUNGUP : JOB_FAILED]++;
				c->done += ok;
				if (ok && b->mode == JOBS_INTERACTIVE)
					hist_record(b->lat, now_ns() - c->sent_ns);
				c->state = busy ? JOB_HOLDING : JOB_SENDING;
				c->sent = 0;
				c->due_ns = now_ns() + BUSY_MS * 1'000'000u;
				job_arm(ep, c, busy ? 0 : EPOLLOUT);
			} else if (hangup && head && !c->cancelled &&
			           memmem(c->buf, head, "text/event-stream", 17)) {
				c->state = JOB_HOLDING;
				c->due_ns = now_ns() + (uint64_t)((0.5 + rng_unit(&rng)) *
				                                  HANGUP_MS * 1e6);
			}
		}
		/* Hang up on the jobs held long enough; re-establish any
		 * connections closed or refused. */
		uint64_t now = now_ns();
		for (int i = 0; i < b->count; ++i) {
			struct job_conn *c = &jc[i];
			if (c->state == JOB_HOLDING && c->due_ns <= now &&
			    b->mode == JOBS_FLOOD) {
				c->state = JOB_SENDING;
				job_arm(ep, c, EPOLLOUT);
			} else if (c->state == JOB_HOLDING && c->due_ns <= now &&
			    b->mode == JOBS_CANCEL) {
				/* Read the call's stream on, to its end. */
				b->n[JOB_DROPPED] += !job_cancel(b, c);
				c->cancelled = true;
				c->state = JOB_READING;
			} else if (c->state == JOB_HOLDING && c->due_ns <= now) {
				b->n[JOB_HUNGUP]++;
				job_kill(b, ep, c, false);
			}
			if (c->state == JOB_DEAD && (!own || c->call_len))
				job_dial(b, ep, c);
		}
	}
	for (int i = 0; i < b->count; ++i) {
		b->per[i] = jc[i].done;
		if (jc[i].fd >= 0)
			b->t->close(jc[i].fd);
	}
	close(ep);
	free(jc);
	return nullptr;
}

/* --- reporting ----------------------------------------------------------- */

/* The fate counts (if any), the samples above @p slow_ns and the percentiles
 * of a summary line. */
static void
print_tail (uint64_t const    *n,
            struct hist const *h,
            uint64_t           slow_ns)
{
	for (int f = 0; n && f < FATE_N; ++f)
		printf(" %s=%llu", fate_name[f], (unsigned long long)n[f]);
	printf(" slow=%llu p50_us=%.1f p90_us=%.1f p99_us=%.1f p99_9_us=%.1f "
	       "max_us=%.1f\n", (unsigned long long)hist_above(h, slow_ns),
	       hist_pct(h, 0.5) / 1e3, hist_pct(h, 0.9) / 1e3, hist_pct(h, 0.99) / 1e3,
	       hist_pct(h, 0.999) / 1e3, h->max / 1e3);
}

/* Sum a probe's fate counts over the kinds it rotates through. */
static void
probe_totals (struct probe const *p,
              uint64_t            tot[FATE_N])
{
	for (int f = 0; f < FATE_N; ++f) {
		tot[f] = 0;
		for (int k = p->kind_first; k < p->kind_end; ++k)
			tot[f] += p->n[k][f];
	}
}

static void
hist_print (char const        *name,
            struct hist const *h)
{
	printf("  %-15s n=%-7llu p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f max=%.1f us\n",
	       name, (unsigned long long)h->count, hist_pct(h, 0.5) / 1e3,
	       hist_pct(h, 0.9) / 1e3, hist_pct(h, 0.99) / 1e3,
	       hist_pct(h, 0.999) / 1e3, h->max / 1e3);
	if (!h->count)
		return;
	for (int oct = 0; oct <= HIST_OCTAVES; ++oct) {
		uint64_t lo = oct ? (uint64_t)HIST_SUB << (oct - 1) : 0;
		uint64_t hi = (uint64_t)HIST_SUB << oct;
		uint64_t c = 0;
		for (size_t i = hist_index(lo); i < hist_index(hi) && i < HIST_BUCKETS; ++i)
			c += h->bucket[i];
		if (!c)
			continue;
		int bar = (int)(50 * c / h->count);
		printf("    [%8.1f..%8.1f us] %7llu %.*s\n", lo / 1e3, hi / 1e3,
		       (unsigned long long)c, bar,
		       "##################################################");
	}
}

/* Print the upstream mock's counters as of now, labelled @p when.  With no
 * label, print them only if no call is active or queued; returns whether it
 * printed. */
static bool
print_mock_stats (char const *port,
                  char const *when)
{
	struct transport m = tcp;
	m.port = port;
	static char const get[] = "GET /stats HTTP/1.1\r\nHost: x\r\n"
	                          "Connection: close\r\n\r\n";
	char buf[1024];
	size_t len = 0;
	int fd = m.dial(&m);
	if (fd >= 0 && wait_writable(fd, 1000) &&
	    m.send(fd, get, sizeof get - 1) == (ssize_t)(sizeof get - 1))
		for (struct pollfd pfd = { .fd = fd, .events = POLLIN };
		     len < sizeof buf - 1 && poll(&pfd, 1, 1000) == 1; ) {
			ssize_t r = m.recv(fd, buf + len, sizeof buf - 1 - len);
			if (r <= 0)
				break;
			len += (size_t)r;
		}
	if (fd >= 0)
		m.close(fd);
	buf[len] = '\0';
	char const *body = strstr(buf, "\r\n\r\n");
	if (!when && (!body || !strstr(body, "\"active\":0,\"queued\":0,")))
		return false;
	printf("MOCK %s %s\n", when ? when : "end", body ? body + 4 : "unavailable");
	return true;
}

static int
cmp_double (void const *a,
            void const *b)
{
	double x = *(double const *)a;
	double y = *(double const *)b;
	return (x > y) - (x < y);
}

/* The median and the largest of @p n values, sorted in place, and their
 * ratio, as " NAME_median=… NAME_max=… NAME_ratio=…". */
static void
print_spread (char const *name,
              double     *v,
              size_t      n)
{
	qsort(v, n, sizeof *v, cmp_double);
	double median = n ? (n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2) : 0;
	double max = n ? v[n - 1] : 0;
	printf(" %s_median=%.1f %s_max=%.1f %s_ratio=%.2f", name, median, name, max,
	       name, median > 0 ? max / median : 0);
}

static bool
build_requests (void)
{
	for (size_t i = 0; i < RQ_N; ++i) {
		int n = snprintf(req_buf[i], sizeof req_buf[i],
		                 "POST /mcp HTTP/1.1\r\nHost: x\r\n"
		                 "Content-Type: application/json\r\n"
		                 "Accept: application/json, text/event-stream\r\n"
		                 "%sContent-Length: %zu\r\n\r\n%s", req_tmpl[i].hdrs,
		                 req_tmpl[i].body_len, req_tmpl[i].body);
		if (n < 0 || (size_t)n >= sizeof req_buf[i])
			return false;
		req_len[i] = (size_t)n;
	}
	return true;
}

static void
raise_nofile (void)
{
	struct rlimit rl;
	if (!getrlimit(RLIMIT_NOFILE, &rl)) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_NOFILE, &rl);
	}
}

int
main (int   argc,
      char *argv[])
{
	enum {
		OPT_IDLE = 0x100, OPT_LORIS, OPT_GAP, OPT_SLOWBODY, OPT_SLOWREAD,
		OPT_PIPELINE, OPT_MALFORMED, OPT_HOSTILE, OPT_DENSE, OPT_CHURN, OPT_JOBS,
		OPT_HANGUP, OPT_CANCEL, OPT_FRESH, OPT_STATS, OPT_SLOW, OPT_SOLO,
		OPT_FLOOD, OPT_INTERACTIVE,
	};
	static struct option const opts[] = {
		{ "host",      required_argument, nullptr, 'H' },
		{ "port",      required_argument, nullptr, 'p' },
		{ "rate",      required_argument, nullptr, 'r' },
		{ "duration",  required_argument, nullptr, 'd' },
		{ "timeout",   required_argument, nullptr, 'T' },
		{ "conns",     required_argument, nullptr, 'c' },
		{ "fresh",     required_argument, nullptr, OPT_FRESH },
		{ "idle",      required_argument, nullptr, OPT_IDLE },
		{ "loris",     required_argument, nullptr, OPT_LORIS },
		{ "slowbody",  required_argument, nullptr, OPT_SLOWBODY },
		{ "gap",       required_argument, nullptr, OPT_GAP },
		{ "slowread",  required_argument, nullptr, OPT_SLOWREAD },
		{ "pipeline",  required_argument, nullptr, OPT_PIPELINE },
		{ "malformed", no_argument,       nullptr, OPT_MALFORMED },
		{ "hostile",   no_argument,       nullptr, OPT_HOSTILE },
		{ "dense",     no_argument,       nullptr, OPT_DENSE },
		{ "churn",     required_argument, nullptr, OPT_CHURN },
		{ "jobs",      required_argument, nullptr, OPT_JOBS },
		{ "hangup",    required_argument, nullptr, OPT_HANGUP },
		{ "cancel",    required_argument, nullptr, OPT_CANCEL },
		{ "flood",     required_argument, nullptr, OPT_FLOOD },
		{ "interactive", required_argument, nullptr, OPT_INTERACTIVE },
		{ "stats",     required_argument, nullptr, OPT_STATS },
		{ "slow",      required_argument, nullptr, OPT_SLOW },
		{ "solo",      no_argument,       nullptr, OPT_SOLO },
		{ nullptr, 0, nullptr, 0 },
	};

	tcp.host = "127.0.0.1";
	tcp.port = "8090";
	char const *stats = nullptr;
	double rate = 1000.0;
	double duration = 10.0;
	double churn = 0;
	long timeout_ms = 5000;
	long slow_us = 1000;
	int conns = 32, fresh = 64;
	int idle = 0, loris = 0, gap = 1000, slowbody = 0, slowread = 0;
	int pipeline = 0, jobs = 0, hangup = 0, cancel = 0, flood = 0, interactive = 0;
	bool malformed = false, hostile = false, dense = false, solo = false;

	for (int o; (o = getopt_long(argc, argv, "H:p:r:d:T:c:", opts, nullptr)) >= 0; )
		switch (o) {
		case 'H': tcp.host = optarg; break;
		case 'p': tcp.port = optarg; break;
		case 'r': rate = atof(optarg); break;
		case 'd': duration = atof(optarg); break;
		case 'T': timeout_ms = atol(optarg); break;
		case 'c': conns = atoi(optarg); break;
		case OPT_FRESH: fresh = atoi(optarg); break;
		case OPT_IDLE: idle = atoi(optarg); break;
		case OPT_LORIS: loris = atoi(optarg); break;
		case OPT_SLOWBODY: slowbody = atoi(optarg); break;
		case OPT_GAP: gap = atoi(optarg); break;
		case OPT_SLOWREAD: slowread = atoi(optarg); break;
		case OPT_PIPELINE: pipeline = atoi(optarg); break;
		case OPT_MALFORMED: malformed = true; break;
		case OPT_HOSTILE: hostile = true; break;
		case OPT_DENSE: dense = true; break;
		case OPT_HANGUP: hangup = atoi(optarg); break;
		case OPT_CANCEL: cancel = atoi(optarg); break;
		case OPT_FLOOD: flood = atoi(optarg); break;
		case OPT_INTERACTIVE: interactive = atoi(optarg); break;
		case OPT_CHURN: churn = atof(optarg); break;
		case OPT_JOBS: jobs = atoi(optarg); break;
		case OPT_STATS: stats = optarg; break;
		case OPT_SLOW: slow_us = atol(optarg); break;
		case OPT_SOLO: solo = true; break;
		default: return 2;
		}
	if (rate <= 0 || duration <= 0 || timeout_ms <= 0 || slow_us < 0 || conns < 0 ||
	    fresh < 0 || conns + fresh < 1) {
		fputs("gen: need rate, duration, timeout > 0 and some probe slots\n", stderr);
		return 2;
	}

	/* Wake on schedule: the default 50 us slack would count as latency.
	 * Threads inherit it, so set it before any start. */
	prctl(PR_SET_TIMERSLACK, 1UL);
	raise_nofile();
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_interrupt);
	if (!build_requests()) {
		fputs("gen: a request is too long\n", stderr);
		return 1;
	}

	/* What each body or batch of the heavy modes takes. */
	static struct hist lat[BG_MAX];
	if (solo) {
		/* The heavy modes alone, one after another, a few rounds each. */
		struct {
			bool      on;
			void   *(*fn) (void *);
			struct bg b;
		} solos[] = {
			{ malformed, burst_thread, { .name = "malformed", .mode = BURST_MALFORMED,
			                             .rounds = 8 } },
			{ hostile, burst_thread, { .name = "hostile", .mode = BURST_HOSTILE,
			                           .rounds = 3 * HOSTILE_KINDS } },
			{ dense, burst_thread, { .name = "dense", .mode = BURST_DENSE,
			                         .rounds = 3 * DENSE_KINDS } },
			{ pipeline > 0, pipeline_thread, { .name = "pipeline", .count = pipeline,
			                                   .rounds = 3 } },
		};
		for (size_t i = 0; i < sizeof solos / sizeof *solos; ++i) {
			struct bg *b = &solos[i].b;
			if (!solos[i].on)
				continue;
			b->tcp = tcp;
			b->tcp.src = (uint32_t)(127 << 24 | (int)(i + 1) << 16);
			b->t = &b->tcp;
			b->lat = &lat[i];
			solos[i].fn(b);
			printf("SOLO name=%s n=%llu", b->name, (unsigned long long)b->lat->count);
			print_tail(nullptr, b->lat, (uint64_t)slow_us * 1000);
		}
		return 0;
	}

	/* The probe dials from any address (127.0.0.1); churn and each
	 * background mode from 127.N.0.1 onwards. */
	struct probe p, cp;
	struct transport ctcp = tcp;
	ctcp.src = (uint32_t)(127 << 24 | (BG_MAX + 1) << 16);
	if (!probe_init(&p, &tcp, conns, fresh, (uint64_t)timeout_ms * 1'000'000u) ||
	    !probe_init(&cp, &ctcp, 0, CHURN_SLOTS, CHURN_TIMEOUT_NS)) {
		fputs("gen: out of memory\n", stderr);
		return 1;
	}

	/* Hold the probe's own connections before background load claims slots. */
	int ready = probe_warmup(&p);

	/* Start background load; each enabled mode owns one thread. */
	struct bg bgs[BG_MAX] = {};
	pthread_t th[BG_MAX];
	int nth = 0;
#define START(cond, fn, ...) \
	do { \
		if (cond) { \
			bgs[nth] = (struct bg){ .tcp = tcp, .lat = &lat[nth], __VA_ARGS__ }; \
			bgs[nth].tcp.src = (uint32_t)(127 << 24 | (nth + 1) << 16); \
			bgs[nth].tcp.per = bgs[nth].count && fn == jobs_thread; \
			bgs[nth].t = &bgs[nth].tcp; \
			nth += !pthread_create(&th[nth], nullptr, fn, &bgs[nth]); \
		} \
	} while (0)
	START(idle > 0, hold_thread, .name = "idle", .mode = HOLD_IDLE, .count = idle,
	      .key = { "opened", "served", "closed" });
	START(loris > 0, hold_thread, .name = "loris", .mode = HOLD_LORIS,
	      .count = loris, .gap_ms = gap, .key = { "opened", nullptr, "closed" });
	START(slowbody > 0, hold_thread, .name = "slowbody", .mode = HOLD_SLOWBODY,
	      .count = slowbody, .gap_ms = gap, .key = { "opened", nullptr, "closed" });
	START(slowread > 0, hold_thread, .name = "slowread", .mode = HOLD_SLOWREAD,
	      .count = slowread, .key = { "opened", nullptr, "closed", "piped" });
	START(malformed, burst_thread, .name = "malformed", .mode = BURST_MALFORMED,
	      .key = { "opened", "answered", "refused", "sent", "ok" });
	START(hostile, burst_thread, .name = "hostile", .mode = BURST_HOSTILE,
	      .key = { "opened", "answered", "refused", "sent", "ok" });
	START(churn > 0, churn_thread, .name = "churn", .probe = &cp, .rate = churn);
	START(dense, burst_thread, .name = "dense", .mode = BURST_DENSE,
	      .key = { "opened", "answered", "refused", "sent", "ok" });
	START(jobs > 0, jobs_thread, .name = "jobs", .count = jobs,
	      .key = { "submitted", "completed", "failed", "dropped" });
	START(hangup > 0, jobs_thread, .name = "hangup", .count = hangup,
	      .mode = JOBS_HANGUP,
	      .key = { "submitted", "completed", "failed", "dropped", "hungup" });
	START(cancel > 0, jobs_thread, .name = "cancel", .count = cancel,
	      .mode = JOBS_CANCEL,
	      .key = { "submitted", "completed", "failed", "dropped", "withdrawn" });
	START(flood > 0, jobs_thread, .name = "flood", .count = flood,
	      .mode = JOBS_FLOOD,
	      .key = { "submitted", "completed", "failed", "dropped", "busy" });
	START(interactive > 0, jobs_thread, .name = "interactive", .count = interactive,
	      .mode = JOBS_INTERACTIVE,
	      .key = { "submitted", "completed", "failed", "dropped" });
	START(pipeline > 0, pipeline_thread, .name = "pipeline", .count = pipeline,
	      .key = { "batches", "reqs", "got", "drain_total_us", "drain_max_us" });
#undef START

	fprintf(stderr, "gen: %s:%s rate=%.0f/s duration=%.0fs conns=%d/%d fresh=%d "
	        "bg=%d\n", tcp.host, tcp.port, rate, duration, ready, conns, fresh, nth);

	/* Let background load ramp up before the measured window opens. */
	if (nth)
		poll(nullptr, 0, 500);
	probe_run(&p, rate, duration);
	if (stats)
		(void)print_mock_stats(stats, "window");
	atomic_store(&g_stop, true);
	for (int i = 0; i < nth; ++i)
		pthread_join(th[i], nullptr);
	/* What the upstream saw once askai has passed on the hang-ups: wait up
	 * to 3 s for the calls of clients gone to be cancelled. */
	for (int i = 0; stats && i < 30 && !print_mock_stats(stats, i < 29 ? nullptr
	                                                              : "end"); ++i)
		poll(nullptr, 0, 100);

	hist_print("all", &p.hist[H_ALL]);
	for (int k = p.kind_first; k < p.kind_end; ++k) {
		printf("SUMMARY kind=%s", kind_name[k]);
		print_tail(p.n[k], &p.hist[k], (uint64_t)slow_us * 1000);
	}
	uint64_t tot[FATE_N];
	probe_totals(&p, tot);
	printf("SUMMARY kind=all");
	print_tail(tot, &p.hist[H_ALL], (uint64_t)slow_us * 1000);
	printf("CONTROL timing=dispatch n=%llu", (unsigned long long)p.hist[H_CO].count);
	print_tail(nullptr, &p.hist[H_CO], (uint64_t)slow_us * 1000);
	printf("PROBE conns=%d ready=%d fresh=%d timeout_ms=%ld slow_us=%ld "
	       "connfail=%llu\n", conns, ready, fresh, timeout_ms, slow_us,
	       (unsigned long long)p.connfail);
	/* Every arrival must have met a fate, and none may have been lost. */
	bool whole = !tot[FATE_LOST] &&
	             tot[FATE_SCHED] == tot[FATE_OK] + tot[FATE_ERR] + tot[FATE_TIMEOUT];

	for (int i = 0; i < nth; ++i) {
		struct bg const *b = &bgs[i];
		printf("BG name=%s", b->name);
		if (b->probe) {
			uint64_t ct[FATE_N];
			probe_totals(b->probe, ct);
			print_tail(ct, &b->probe->hist[H_ALL], (uint64_t)slow_us * 1000);
			whole &= !ct[FATE_LOST] && ct[FATE_SCHED] ==
			         ct[FATE_OK] + ct[FATE_ERR] + ct[FATE_TIMEOUT];
			continue;
		}
		for (int k = 0; k < BG_KEYS; ++k)
			if (b->key[k])
				printf(" %s=%llu", b->key[k], (unsigned long long)b->n[k]);
		if (b->lat->count)
			print_tail(nullptr, b->lat, (uint64_t)slow_us * 1000);
		else
			putchar('\n');
	}

	/* How evenly the damage fell: each probe connection's p90 and p99, and
	 * each probe kind's, against the median one's.  Each connection had its
	 * share of the arrivals, so none is left out.  And each job connection's
	 * calls. */
	double v[PK_N + 1024];
	size_t nv;
	printf("FAIR probe");
	for (int pct = 90; pct <= 99; pct += 9) {
		char name[32];
		nv = 0;
		for (int i = 0; i < p.nka && i < 1024; ++i)
			if (p.conn_hist[i].count)
				v[nv++] = hist_pct(&p.conn_hist[i], pct / 100.0) / 1e3;
		snprintf(name, sizeof name, "conn_p%d_us", pct);
		printf(" conns=%zu", nv);
		print_spread(name, v, nv);
		nv = 0;
		for (int k = p.kind_first; k < p.kind_end; ++k)
			if (p.hist[k].count)
				v[nv++] = hist_pct(&p.hist[k], pct / 100.0) / 1e3;
		snprintf(name, sizeof name, "kind_p%d_us", pct);
		printf(" kinds=%zu", nv);
		print_spread(name, v, nv);
	}
	putchar('\n');
	for (int i = 0; i < nth; ++i) {
		struct bg *b = &bgs[i];
		if (!b->per || (b->mode != JOBS && b->mode != JOBS_INTERACTIVE))
			continue;
		nv = 0;
		for (int k = 0; k < b->count && k < 1024; ++k)
			v[nv++] = (double)b->per[k];
		qsort(v, nv, sizeof *v, cmp_double);
		printf("FAIR %s conns=%zu calls_min=%.0f", b->name, nv, nv ? v[0] : 0);
		print_spread("calls", v, nv);
		putchar('\n');
	}
	for (int i = 0; i < nth; ++i)
		free(bgs[i].per);

	probe_fini(&cp);
	probe_fini(&p);
	if (!whole)
		fputs("gen: FAIL: not every scheduled arrival was accounted for\n", stderr);
	return whole ? 0 : 1;
}
