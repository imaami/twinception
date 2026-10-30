/** @file web.h
 *
 * HTTP requests that the calls of a plugin's tools send through askai
 * with mcp_slot_fetch() (src/mcp.h), and the responses that askai stores
 * for reuse.
 *
 * A plugin starts no thread and does not block (src/plugin.h), so it cannot
 * use libcurl or a socket itself.  A call of a plugin's tool passes a
 * struct web_request to askai instead, and parks.  askai sends the request
 * and receives the response on home's thread.  Then it queues the step() of
 * the call's kind, and step() reads the response with mcp_slot_fetched().
 *
 * askai can store a response for later requests.  The calls of every user
 * and every loop share the stored responses of a plugin.  A request is
 * equal to another request if both come from the same plugin and have the
 * same method, URL, Accept, header lines, max and body.
 *
 * askai's own tools send requests through the same service, such as the
 * uploads of notes (src/mcp.c), with struct web_ask.
 */
#ifndef TWINCEPTION_SRC_WEB_H_
#define TWINCEPTION_SRC_WEB_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "loop_export.h"

enum {
	WEB_URL_MAX     = 4 << 10,    /**< bytes of a URL, its NUL not counted */
	WEB_SEND_MAX    = 4 << 20,    /**< bytes of a request body */
	WEB_BODY_MAX    = 16 << 20,   /**< bytes of a response body */
	WEB_ACCEPT_MAX  = 256,        /**< bytes of a value of Accept */
	WEB_TIMEOUT_MAX = 300,        /**< seconds that a request may take */
	WEB_KEEP_MAX    = 7 * 86400,  /**< seconds of keep */
	WEB_RUNS        = 64,         /**< requests that askai sends at once */
};

/** An HTTP request that a call sends with mcp_slot_fetch().  askai copies
 *  the request and the bytes that its members point to.
 */
struct web_request {
	/** The URL, NUL-terminated.  It begins with http:// or https://, and
	 *  it has at most WEB_URL_MAX bytes.  Each byte is printable ASCII and
	 *  not a space.  The URL has no user name and no password. */
	char const *url;
	/** The body of a POST, which askai sends with the Content-Type
	 *  application/json, or nullptr for a GET.  askai does not check that
	 *  the body is JSON. */
	char const *body;
	/** The length of the body in bytes, at most WEB_SEND_MAX.  It is 0
	 *  for a GET. */
	size_t      len;
	/** The value of the header Accept, NUL-terminated: 1 to WEB_ACCEPT_MAX
	 *  bytes, each of them printable ASCII or a space.  If it is nullptr,
	 *  askai sends the Accept of libcurl, which accepts any type. */
	char const *accept;
	/** The largest length of the response body in bytes, from 1 to
	 *  WEB_BODY_MAX.  A longer body fails the request with EFBIG.  askai
	 *  asks for a compressed body, and max is the length after askai has
	 *  decoded it. */
	size_t      max;
	/** The seconds that the whole request may take, from 1 to
	 *  WEB_TIMEOUT_MAX.  A request that takes longer fails with ETIMEDOUT.
	 *  So does a request whose connection to the server is not set up
	 *  within 10 seconds. */
	uint32_t    timeout;
	/** 0, or at most WEB_KEEP_MAX seconds.  If it is not 0, askai stores
	 *  the response, and it can answer the request with a stored response
	 *  that is younger than keep.  See mcp_slot_fetch(). */
	uint32_t    keep;
};

/** The result of a call's request (mcp_slot_fetched()). */
struct web_response {
	/** The response body.  It can have any bytes, and it does not end in
	 *  a NUL.  It is empty if e is not 0. */
	char const *body;
	size_t      len;     /**< the length of the body in bytes */
	/** The value of Content-Type, NUL-terminated.  It is "" if the
	 *  response has no Content-Type. */
	char const *type;
	/** The reason for the failure, NUL-terminated text.  It is "" if e is
	 *  0. */
	char const *error;
	/** The HTTP status, or 0 if no status line came. */
	uint32_t    status;
	/** The seconds since askai received the response. */
	uint32_t    age;
	/** The seconds of the header Retry-After, or 0 if the response has
	 *  none. */
	uint32_t    retry;
	/**
	 * 0 if the whole response came.  Otherwise it is one of these error
	 * numbers:
	 * - ETIMEDOUT: the request took longer than its timeout, or the
	 *   connection was not set up within 10 seconds.
	 * - EFBIG: the response body is longer than the request's max.
	 * - ESHUTDOWN: askai is stopping.
	 * - ENOMEM: askai ran out of memory.
	 * - EIO: any other failure, which error describes.
	 */
	int         e;
};

/* The rest of this file is askai's side.  A plugin does not use it. */

struct blocker;
struct web;
struct web_wait;

/**
 * askai calls this function on the loop of @p wait when the wait has its
 * reply.  @p who is the handle that the caller of web_wait_create() gave.
 * The function must call web_wait_leave() for @p wait, at once or later.
 * askai also calls it inside loop_fini(), after the loop has stopped.
 */
typedef void web_back_fn (struct loop     *loop,
                          struct handle    who,
                          struct web_wait *wait);

/** A request of a call, after mcp_slot_fetch() has checked it, and the
 *  call that waits for its response.  A request of askai's own does not
 *  pass mcp_slot_fetch(): its URL may have a user name and a password,
 *  which the log leaves out, its body may be longer than WEB_SEND_MAX, and
 *  it may have more header lines.
 */
struct web_ask {
	struct web_request const *request;
	char const               *plugin;  /**< the name of the call's plugin,
	                                        for the User-Agent, or nullptr
	                                        for askai's own tools */
	struct loop              *loop;    /**< the call's loop */
	struct loop              *home;    /**< home's loop, which sends the
	                                        requests */
	web_back_fn              *back;    /**< the function that askai calls
	                                        with the reply */
	size_t                    url;     /**< the length of request->url */
	size_t                    accept;  /**< the length of request->accept,
	                                        or 0 if it is nullptr */
	char const *const        *lines;   /**< more header lines, which askai
	                                        checked, nullptr-terminated, or
	                                        nullptr; a line of User-Agent is
	                                        left out, as askai sends its
	                                        own */
	struct handle             who;     /**< names the call on its loop */
};

/**
 * Creates the service of HTTP requests, with no stored response.  All loops
 * share it.  askai creates it before the loops start, and destroys it after
 * every loop has called loop_fini().
 *
 * The service has a mutex.  A thread locks the mutex for each access to the
 * table of stored responses, to the list of the requests under way that
 * another request can wait for, and to the list of the waits of each
 * request.  While the mutex is locked, no thread makes a system call, calls
 * libcurl or allocates a body.
 *
 * Returns the service.  On failure it returns nullptr and stores ENOMEM, or
 * the error number of pthread_mutex_init(), in @p *e.
 */
extern struct web *
web_create (int *e);

/** Frees the service @p *p_web and its stored responses, and sets
 *  @p *p_web to nullptr.  No request may be under way.
 */
extern void
web_destroy (struct web **p_web);

/**
 * Starts the part of the service that runs on home's loop @p loop.  It runs
 * on the thread of @p loop.  The part has a curl bridge (src/http.h) of its
 * own, so that the requests of the plugins do not use the connections of
 * home's upstream calls.  The bridge has at most two connections to one
 * host at once, and it looks host names up on @p blockers.  Returns 0, or
 * the error number of http_create().
 */
extern int
web_home_start (struct web     *web,
                struct loop    *loop,
                struct blocker *blockers);

/** Ends every request that is under way with ESHUTDOWN, and posts the
 *  reply to each of its waits.  Then destroys the part of the service on
 *  home's loop.  It runs on home's thread after home's loop has stopped.
 */
extern void
web_home_stop (struct web *web);

/**
 * Frees stored responses under memory pressure of the level @p level, at
 * most @p max units of release work (buf_units()), or one response if that
 * is more.  At BUF_HIGH it frees only responses whose keep has passed.  At
 * BUF_CRITICAL it frees every stored response.  Returns the units, or 0
 * when no such response is left.
 */
extern size_t
web_relieve (struct web        *web,
             enum buf_pressure  level,
             size_t             max);

/**
 * Creates a wait for the response to the request of @p ask.  It runs on the
 * thread of ask->loop.
 *
 * If ask->request->keep is not 0, the function looks for a stored response
 * to an equal request that askai received less than keep seconds ago.  If
 * there is one, the wait has it at once, and web_wait_done() returns true.
 * Otherwise, if an equal request with the same timeout is under way and its
 * keep is not 0, the function adds the wait to the waits of that request.  Otherwise it posts
 * a new request to ask->home.  When the request ends, home posts the reply
 * to each of its waits, and askai calls ask->back on ask->loop for each of
 * them.
 *
 * Returns the wait.  On failure it returns nullptr and stores the error
 * number in @p *e: EAGAIN if WEB_RUNS requests are under way, EINVAL if
 * the header lines have more than 65535 bytes, or ENOMEM.
 */
extern struct web_wait *
web_wait_create (struct web           *web,
                 struct web_ask const *ask,
                 int                  *e);

/** Returns whether @p wait has its reply.  Only the thread of the wait's
 *  loop calls it.
 */
extern bool
web_wait_done (struct web_wait const *wait);

/** Stores the reply of @p wait in @p *response.  web_wait_done() must
 *  return true for @p wait.  The age is computed from @p now, the loop's time
 *  (loop_now()).  The pointers in @p *response stay valid until
 *  web_wait_leave().
 */
extern void
web_wait_response (struct web_wait const *wait,
                   struct web_response   *response,
                   uint64_t               now);

/**
 * Releases @p *p_wait and sets @p *p_wait to nullptr.  A wait that has its
 * reply is freed with its reference to the reply.  A wait whose request is
 * under way is removed from the request's list of waits and freed.  If the
 * request has no wait left and its keep is 0, home cancels it.  A wait
 * whose reply home has posted, and whose web_back_fn has not run yet, is
 * not freed here.  The web_back_fn calls this function for it again.
 */
extern void
web_wait_leave (struct web_wait **p_wait);

#endif /* TWINCEPTION_SRC_WEB_H_ */
