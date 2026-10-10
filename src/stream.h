/** @file Request streams: all an application such as MCP sees of HTTP.
 *
 * A transport, HTTP/1.1 now and HTTP/2 later, turns each request into a
 * stream.  It tells the application about the stream upward (struct
 * stream_app), and the application answers downward (struct stream_ops), so
 * that it never sees how the transport frames messages.
 *
 * The application gets a stream with on_head(), then its body, decoded, as it
 * arrives, through on_body(), the last piece marked.  It answers with head(),
 * any data() and one end(), after which the stream is the transport's again.
 * Should the transport lose the stream first, as when the client goes away
 * or the body breaks a limit, on_cancel() takes it back instead; it never
 * follows end().  The transport makes no upcall from within a downcall, so
 * the application may answer from any upcall or from a step of its own.
 *
 * Once the application waits for something outside its loop to answer, such
 * as an upstream call, it says so with waiting(): from then on a client that
 * only shuts down its sending side, which HTTP/1.1 cannot tell from one going
 * away, cancels the stream too.  Until then that client is still answered.
 */
#ifndef TWINCEPTION_SRC_STREAM_H_
#define TWINCEPTION_SRC_STREAM_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"

/** The body length of a request or response not known up front. */
static constexpr size_t STREAM_OPEN = SIZE_MAX;

/** The most request header fields an application may ask for. */
enum { STREAM_FIELDS = 8 };

struct stream;

/** Downward: the application's answer. */
struct stream_ops {
	/**
	 * Begin the response.  @p fields are whole header lines, each ending in
	 * CRLF, and @p length is the body's length in bytes, or STREAM_OPEN for
	 * a body that only end() ends, such as an event stream.  Returns 0, or
	 * EPIPE once the stream can carry no response.
	 */
	int         (*head)    (struct stream *s,
	                        uint16_t       status,
	                        struct span    fields,
	                        size_t         length);
	/**
	 * Queue body bytes.  EAGAIN, with the bytes queued, asks to wait for
	 * on_writable().  EPIPE as above, or when the bytes would take the
	 * output past the transport's limit, which a response may pass only
	 * while it is all the output and has not been asked to wait.
	 */
	int         (*data)    (struct stream *s,
	                        struct span    bytes);
	/** End the response; with an @p error, abort it.  Ends the stream too. */
	void        (*end)     (struct stream *s,
	                        int            error);
	/** The response waits for something outside the loop: a client that
	 *  shuts down its sending side cancels the stream from now on. */
	void        (*waiting) (struct stream *s);
	/** The value of request header field @p i of the application's list,
	 *  empty if absent.  Valid until the transport next reads. */
	struct span (*field)   (struct stream const *s,
	                        size_t               i);
};

/** Upward: what a transport tells its application. */
struct stream_app {
	/** Header field names the application reads with field(), in lower
	 *  case; each may appear at most once in a request, but a list. */
	char const *const *fields;
	size_t             n_fields;
	/** Bit i set: field i is a list (RFC 9110 5.3), which may come in
	 *  several lines; field() gives the first. */
	uint64_t           lists;
	void              *ctx;
	/** A request: its method and target, and its body's length or
	 *  STREAM_OPEN.  @p method and @p target are valid during the call. */
	void (*on_head)     (void          *ctx,
	                     struct stream *s,
	                     struct span    method,
	                     struct span    target,
	                     size_t         length);
	/** More of the body, valid during the call; @p last once it is all in. */
	void (*on_body)     (struct stream *s,
	                     struct span    bytes,
	                     bool           last);
	/** The output that made data() say EAGAIN has drained. */
	void (*on_writable) (struct stream *s);
	/** The stream is gone before its end(). */
	void (*on_cancel)   (struct stream *s);
};

struct stream {
	struct stream_ops const *ops;
	void                    *user;     /**< the application's own */
	uint64_t                 trusted;  /**< bool, widened over the padding: the
	                                        client showed the token or, none
	                                        being set, is on loopback and names
	                                        no other host, and may see
	                                        diagnostics */
};

#endif /* TWINCEPTION_SRC_STREAM_H_ */
