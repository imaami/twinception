/** @file Asynchronous HTTP interface used by the model client. */
#ifndef TWINCEPTION_SRC_HTTP_H_
#define TWINCEPTION_SRC_HTTP_H_

#include <stddef.h>

#include <curl/curl.h>

#include "loop.h"

typedef size_t http_write_cb (char const *data,
                              size_t      len,
                              void       *user);

typedef void http_done_cb (CURLcode    result,
                           long        status,
                           char const *curl_error,
                           void       *user);

struct http;
struct http_request;

extern int
http_init (struct http **p_http,
           struct loop  *loop);

extern void
http_fini (struct http **p_http);

extern int
http_post_json (struct http       *http,
                char const        *url,
                char const        *body,
                http_write_cb     *write_cb,
                http_done_cb      *done_cb,
                void              *user,
                struct http_request **p_request);

extern void
http_cancel (struct http         *http,
             struct http_request **p_request);

#endif /* TWINCEPTION_SRC_HTTP_H_ */
