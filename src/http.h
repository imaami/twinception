/** @file Asynchronous HTTP interface used by the model client. */
#ifndef TWINCEPTION_SRC_HTTP_H_
#define TWINCEPTION_SRC_HTTP_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include <curl/curl.h>

#include "hist.h"
#include "loop.h"

typedef size_t http_write_cb (char const *data,
                              size_t      len,
                              void       *user);

typedef void http_done_cb (CURLcode    result,
                           long        status,
                           char const *curl_error,
                           void       *user);

struct blockers;
struct http;
struct http_request;

/** What the bridge has done, its loop the one writer (src/hist.h); ns. */
struct http_metrics {
	struct hist      wait;        /**< from a request to its start in curl */
	struct hist      run;         /**< from then to its end */
	_Atomic uint64_t lookups[2];  /**< host lookups that failed, succeeded */
};

extern int
http_init (struct http **p_http,
           struct loop  *loop);

extern void
http_fini (struct http **p_http);

/**
 * Look up upstream host names on @p blockers rather than in libcurl, which
 * then starts no resolver thread: a request to a host by name waits for the
 * addresses, which are kept for a minute and handed to libcurl through
 * CURLOPT_RESOLVE.  A proxy that libcurl takes from the environment it still
 * looks up itself.
 */
extern void
http_resolver (struct http     *http,
               struct blockers *blockers);

extern struct http_metrics const *
http_metrics (struct http const *http);

/**
 * Start an asynchronous JSON POST of the @p len bytes at @p body, on a later
 * step.  @p headers is an optional nullptr-terminated list of complete header
 * lines; @p timeout_ms <= 0 means no total timeout.  The headers are copied,
 * and so is the body if @p copy; otherwise it must stay as it is until
 * @p done_cb runs or the request is cancelled.
 */
extern int
http_post_json (struct http         *http,
                char const          *url,
                char const          *body,
                size_t               len,
                bool                 copy,
                char const *const   *headers,
                long                 timeout_ms,
                http_write_cb       *write_cb,
                http_done_cb        *done_cb,
                void                *user,
                struct http_request **p_request);

extern void
http_cancel (struct http         *http,
             struct http_request **p_request);

#endif /* TWINCEPTION_SRC_HTTP_H_ */
