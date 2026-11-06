/** @file curl_loop.h
 *
 * libcurl's multi-socket interface on an event loop (src/loop.h). Socket
 * events and curl's timer, which stays in the loop's heap, only queue bulk
 * steps, and the steps drive curl. A socket that cannot be watched fails
 * only the transfer that asked for it.
 */
#ifndef TWINCEPTION_SRC_CURL_LOOP_H_
#define TWINCEPTION_SRC_CURL_LOOP_H_

#include <curl/curl.h>

#include "list.h"
#include "loop.h"

struct curl_loop;

/** Reports that curl ended @p transfer, the CURLOPT_PRIVATE of its handle,
 *  with @p result. It runs on the loop, outside curl's callbacks, so it may
 *  remove that handle and others.
 */
typedef void curl_loop_done_cb (struct curl_loop *cl,
                                void             *transfer,
                                CURLcode          result);

/** Reports that the socket of @p transfer cannot be watched, with the error
 *  @p e and its text @p why. It runs in curl's socket callback, which must
 *  not remove a handle, so the transfer ends on a later step. With
 *  @p transfer nullptr, curl could not act on an event, @p e is ENOMEM or
 *  EIO, and @p why is curl's text of the error; that call runs outside
 *  curl's callbacks.
 */
typedef void curl_loop_fail_cb (struct curl_loop *cl,
                                void             *transfer,
                                int               e,
                                char const       *why);

/** A curl multi handle on a loop. The owner adds its transfers to multi
 *  and removes them, may set multi's options and may use loop; curl_loop.c
 *  alone uses the other members.
 */
struct curl_loop {
	struct list        sockets;  /**< watched for curl */
	struct loop_step   timeout;  /**< tells curl its timer expired */
	struct loop_timer  timer;    /**< curl's, in the heap from init to fini */
	struct loop       *loop;
	CURLM             *multi;
	curl_loop_done_cb *done;
	curl_loop_fail_cb *fail;
};

/** Make @p cl's multi handle on @p loop, which reports to @p done and
 *  @p fail. Returns 0, ENOMEM, or EIO if curl refuses an option; on
 *  failure nothing is left to finalize.
 */
extern int
curl_loop_init (struct curl_loop  *cl,
                struct loop       *loop,
                curl_loop_done_cb *done,
                curl_loop_fail_cb *fail);

/** Free @p cl's multi handle, after the owner removed its transfers, and
 *  stop watching curl's sockets. Without a multi handle, as when @p cl is
 *  zeroed, failed to initialize or was finalized, nothing happens.
 */
extern void
curl_loop_fini (struct curl_loop *cl);

/** Tell curl about the CURL_CSELECT_* events @p what on @p fd, or with
 *  CURL_SOCKET_TIMEOUT and 0 have it run the transfers that are due, as
 *  after adding one, and report the transfers that ended.
 */
extern void
curl_loop_action (struct curl_loop *cl,
                  curl_socket_t     fd,
                  int               what);

#endif /* TWINCEPTION_SRC_CURL_LOOP_H_ */
