/** @file http.h
 *
 * Asynchronous HTTP interface used by the model client.
 */
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

struct blocker;
struct http;
struct http_request;

/** What the bridge has done, its loop the one writer (src/hist.h); ns. */
struct http_metrics {
	struct hist      wait;        /**< from a request to its start in curl */
	struct hist      run;         /**< from then to its end */
	_Atomic uint64_t lookups[2];  /**< host lookups that failed, succeeded */
};

/** A bridge on @p loop that counts into @p metrics, whose atomic members
 *  are valid, unless that is nullptr; nullptr, with the error in @p *e, if
 *  it cannot be made.
 */
extern struct http *
http_create (struct loop         *loop,
             struct http_metrics *metrics,
             int                 *e);

extern void
http_destroy (struct http **p_http);

/**
 * Look up upstream host names on @p blockers rather than in libcurl, which
 * then starts no resolver thread: a request to a host by name waits for the
 * addresses, which are kept for a minute and handed to libcurl through
 * CURLOPT_RESOLVE.  A proxy that libcurl takes from the environment it still
 * looks up itself.
 */
extern void
http_resolver (struct http    *http,
               struct blocker *blockers);

/**
 * Start an asynchronous POST of the @p len bytes at @p body, on a later
 * step, or a GET if @p body is nullptr.  @p headers is an optional
 * nullptr-terminated list of complete header lines; a POST has the
 * Content-Type application/json unless one of them is a Content-Type.
 * @p timeout_ms <= 0 means no total timeout.  The headers are copied, and
 * so is the body if @p copy; otherwise it must stay as it is until
 * @p done_cb runs or the request is cancelled.
 */
extern int
http_post_json (struct http          *http,
                char const           *url,
                char const           *body,
                size_t                len,
                bool                  copy,
                char const *const    *headers,
                long                  timeout_ms,
                http_write_cb        *write_cb,
                http_done_cb         *done_cb,
                void                 *user,
                struct http_request **p_request);

/** Sends the header Accept-Encoding with every encoding that libcurl can
 *  decode, and decodes the answer to @p request before the write callback
 *  gets it.  Call it before the step that starts the request, so right
 *  after http_post_json().  Returns 0, EFAULT if @p request is nullptr, or
 *  EIO if libcurl refuses the option.
 */
extern int
http_accept_encoding (struct http_request *request);

/** Override the method before the queued transfer starts. */
extern int
http_method (struct http_request *request,
             char const          *method);

extern void
http_cancel (struct http          *http,
             struct http_request **p_request);

/** Whether all of @p request has gone to the server, as far as libcurl can
 *  tell: its head and body are written.
 */
extern bool
http_sent (struct http_request const *request);

/** The status and Content-Type of the answer to @p request, once its head
 *  is in, as in a write callback; the type is nullptr if it has none.
 */
extern void
http_response (struct http_request const  *request,
               long                       *status,
               char const                **type);

/** In a done callback: the seconds the answer's Retry-After asked for, 0 if
 *  none.
 */
extern long
http_retry_after (struct http const *http);

/** Returns the Content-Type of the answer, or nullptr if the answer has
 *  none.  Only a done callback calls it, and the text stays valid until the
 *  callback returns.
 */
extern char const *
http_type (struct http const *http);

/** Keep at most @p n connections to a host at once, and @p n + 1 in all,
 *  idle ones included.
 */
extern void
http_connections (struct http *http,
                  long         n);

/** End @p request now with @p result, as if curl had: its done callback
 *  reports it.
 */
extern void
http_end (struct http          *http,
          struct http_request **p_request,
          CURLcode              result);

/**
 * Let @p request go on after its write callback returned
 * CURL_WRITEFUNC_PAUSE: the callback may take what curl held back before
 * this returns, and hold it back again.  An error leaves the request to the
 * caller to end, as with http_end().
 */
extern CURLcode
http_resume (struct http_request *request);

#endif /* TWINCEPTION_SRC_HTTP_H_ */
