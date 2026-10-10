/** @file HTTP/1.1 transport for request streams (src/stream.h). */
#ifndef TWINCEPTION_SRC_HTTPD_H_
#define TWINCEPTION_SRC_HTTPD_H_

#include <stddef.h>
#include <stdint.h>

struct httpd;
struct loop;
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
	struct httpd_limits limits;
	char const *const  *origins;  /**< allowed non-loopback Origins; "*": any */
	char const         *listen;   /**< [host:]port */
	char const         *token;    /**< bearer token clients must send, or nullptr */
};

/** Serve @p app, whose fields may number STREAM_FIELDS at most. */
extern int
httpd_init (struct httpd            **p_httpd,
            struct loop              *loop,
            struct stream_app const  *app,
            struct httpd_cfg const   *cfg);

extern void
httpd_fini (struct httpd **p_httpd);

/**
 * Stop accepting clients, close the idle ones and end the others once their
 * response is sent, or at @p deadline (CLOCK_MONOTONIC ns), a share a step,
 * and have the loop exit once none is left.
 */
extern void
httpd_drain (struct httpd *srv,
             uint64_t      deadline);

#endif /* TWINCEPTION_SRC_HTTPD_H_ */
