/** @file HTTP/1.1 transport for request streams (src/stream.h): home's door,
 *  which accepts the clients and places each on a client loop, and the client
 *  loops' servers, which serve them (design section 3). */
#ifndef TWINCEPTION_SRC_HTTPD_H_
#define TWINCEPTION_SRC_HTTPD_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "buf.h"

struct httpd;
struct httpd_home;
struct httpd_hub;
struct loop;
struct metrics_conns;
struct policy;
struct ssl_ctx_st;
struct stream_app;

/**
 * Limits and deadlines on what clients send and hold.  The response to a
 * breach is given in parentheses; zero turns off the ones marked so.
 */
struct httpd_limits {
	size_t   body;           /**< request body bytes, decoded from chunks (413) */
	size_t   bodies;         /**< body bytes held for all clients at once (503) */
	size_t   client_bodies;  /**< of those, held for one client address (503);
	                              zero: a quarter, but at least body */
	size_t   out_pause;      /**< pending output that pauses a connection's requests,
	                              and asks a stream's application to wait */
	size_t   out_max;        /**< pending output that a response may not take
	                              past, but while it is all that is pending
	                              and keeps to out_pause (close) */
	uint32_t head;           /**< request line and header bytes, at most 65535 (431) */
	uint32_t fields;         /**< header fields, and trailer fields (431) */
	uint32_t clients;        /**< connections per client address (refused); zero: off */
	uint32_t body_rate;      /**< bytes per second a body must average from its
	                              start once body_grace is up (408); zero: off */
	uint32_t body_grace;     /**< seconds before body_rate applies */
	uint32_t head_time;      /**< seconds to send the head in (408); zero: off */
	uint32_t idle_time;      /**< seconds a keep-alive connection may idle (close); zero: off */
	uint32_t write_time;     /**< seconds output may stall (close); zero: off */
};

/** The defaults of doc/core-design.md, section 3. */
static inline struct httpd_limits
httpd_limits (void)
{
	return (struct httpd_limits){
		.body = 1 << 20,
		.bodies = 256 << 20,
		.out_pause = 256 << 10,
		.out_max = 1 << 20,
		.head = 16 << 10,
		.fields = 64,
		.clients = 256,
		.body_rate = 4 << 10,
		.body_grace = 5,
		.head_time = 10,
		.idle_time = 30,
		.write_time = 30
	};
}

struct httpd_cfg {
	struct httpd_limits  limits;
	char const *const   *origins;  /**< allowed non-loopback Origins; "*": any */
	char const *const   *listen;   /**< where to listen: [host:]port, or
	                                    unix:PATH, or tls:[host:]port for
	                                    TLS, each; nullptr ends them */
	struct policy       *policy;   /**< who clients are, and what they may do:
	                                    each loop's server takes a reference */
	struct ssl_ctx_st   *tls;      /**< what TLS listeners' clients shake
	                                    hands by (src/tls.h), if there are
	                                    any: home takes a reference */
};

/** @p addr, a listen address, less its "tls:", if it is TLS's; else
 *  nullptr. */
static inline char const *
httpd_tls (char const *addr)
{
	static char const tls[] = "tls:";
	return strncmp(addr, tls, sizeof tls - 1) ? nullptr : addr + sizeof tls - 1;
}

/**
 * What home's door and the client loops' servers share, for @p loops client
 * loops: made before any loop starts, freed once every loop is finished
 * (loop_fini()), as messages in the loops' inboxes may point into it.
 */
extern struct httpd_hub *
httpd_hub_create (size_t  loops,
                  int    *e);

extern void
httpd_hub_destroy (struct httpd_hub **p_hub);

/** What client loop @p i counts of its connections, for GET /metrics. */
extern struct metrics_conns const *
httpd_hub_metrics (struct httpd_hub const *hub,
                   size_t                  i);

/**
 * Home's door on home's @p loop: listen on cfg->listen, and from
 * httpd_home_open() on, accept clients and place each on a client loop of
 * @p hub.  Descriptors are budgeted for the connections and for @p calls,
 * the jobs in flight, which each may need one upstream.  nullptr, with the
 * error in @p *e, if it cannot be made.
 */
extern struct httpd_home *
httpd_home_create (struct loop            *loop,
                   struct httpd_hub       *hub,
                   struct httpd_cfg const *cfg,
                   _Atomic uint64_t const *calls,
                   int                    *e);

extern void
httpd_home_destroy (struct httpd_home **p_home);

/** Every client loop's server runs: accept clients, unless the door has
 *  drained already. */
extern void
httpd_home_open (struct httpd_home *home);

/** Have every client loop's server take @p policy, a reference each, for
 *  the requests that begin from then on, and let go of its last.  Returns 0,
 *  or EAGAIN unless the door is open, as before every client loop runs and
 *  once draining. */
extern int
httpd_home_policy (struct httpd_home *home,
                   struct policy     *policy);

/** Have the TLS listeners' clients from now on shake hands by @p ctx, a
 *  reference home takes, and let go of the last; with no TLS listener, do
 *  nothing.  Those who shook hands before go on as they are. */
extern void
httpd_home_tls (struct httpd_home *home,
                struct ssl_ctx_st *ctx);

/** Stop accepting clients: close the listener. */
extern void
httpd_home_drain (struct httpd_home *home);

/** A job was admitted, which may need a descriptor upstream: if clients and
 *  calls hold the descriptors they may, have the least recently active idle
 *  connection closed. */
extern void
httpd_home_crowd (struct httpd_home *home);

/** Begin a window of reuse of what home keeps, as buf_stash_age() does for
 *  a stash. */
extern void
httpd_home_age (struct httpd_home *home);

/** Free the connections and client addresses' entries kept for reuse, up
 *  to @p max units of release work, at BUF_HIGH those unused since the last
 *  window began; returns the units, 0 once none are left to free. */
extern size_t
httpd_home_relieve (struct httpd_home *home,
                    enum buf_pressure  level,
                    size_t             max);

/** Serve @p app, whose fields may number STREAM_FIELDS at most, on client
 *  loop @p i of @p hub, its @p loop; nullptr, with the error in @p *e, if
 *  it cannot. */
extern struct httpd *
httpd_create (struct loop             *loop,
              struct httpd_hub        *hub,
              size_t                   i,
              struct stream_app const *app,
              struct httpd_cfg const  *cfg,
              int                     *e);

/** Close every connection, give each back to home, and free the server. */
extern void
httpd_destroy (struct httpd **p_httpd);

/**
 * Close the idle connections and end the others once their response is
 * sent, or at @p deadline (CLOCK_MONOTONIC ns), a share a step, and have the
 * loop exit once none is left.  A connection home places from now on closes
 * at once.
 */
extern void
httpd_drain (struct httpd *srv,
             uint64_t      deadline);

#endif /* TWINCEPTION_SRC_HTTPD_H_ */
