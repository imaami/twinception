/** @file tls.h
 *
 * TLS (S9, design section 3): the context the TLS listeners'
 * connections shake hands by, and a connection's end of TLS,
 * which its client loop drives a step at a time.
 *
 * The context is OpenSSL's SSL_CTX: TLS 1.3 only, ALPN http/1.1, key
 * exchange X25519MLKEM768 first, one ticket a connection and no session
 * cache, a certificate chain whose key is ECDSA on a curve TLS 1.3 signs
 * with or Ed25519, or RSA only if askai's configuration allows it
 * (CFG_TLS_RSA).  Once made it is never changed, only shared by reference
 * (R5): every connection's SSL takes one of its own.
 *
 * A connection's SSL reads and writes the client's socket itself, a record
 * a call, never ahead: the client's bytes wait in the socket, which epoll
 * reports, but for the rest of a record a read had no room for, or part of
 * one (tls_pending()).  Each function here clears the thread's OpenSSL
 * error queue before it calls OpenSSL and after a failure, as every
 * connection of a loop shares that queue; after tls_close(), the
 * tls_destroy() that follows it does.
 */
#ifndef TWINCEPTION_SRC_TLS_H_
#define TWINCEPTION_SRC_TLS_H_

#include <stddef.h>
#include <sys/types.h>

struct config;
struct ssl_ctx_st;
struct ssl_st;

enum {
	TLS_RECORD = 16 << 10,  /**< plaintext bytes of a record, at most */
};

/** Where a connection's handshake stands after a flight. */
enum tls_shake {
	TLS_SHAKEN,  /**< done */
	TLS_READ,    /**< it waits for the client to send */
	TLS_WRITE,   /**< it waits for room to send */
	TLS_FAILED,
};

/**
 * A context from @p chain, @p chain_len bytes of PEM text, the leaf's
 * certificate and then any it needs to be trusted by, and @p key, @p key_len
 * bytes of PEM text, its unencrypted private key, as @p cfg allows; nullptr,
 * with ENOMEM in @p *e if out of memory, or EINVAL with what is wrong in
 * @p why, of @p len bytes.  Neither text is kept.
 */
extern struct ssl_ctx_st *
tls_ctx_create (struct config const *cfg,
                char const          *chain,
                size_t               chain_len,
                char const          *key,
                size_t               key_len,
                char                *why,
                size_t               len,
                int                 *e);

/** Another reference to @p ctx. */
extern struct ssl_ctx_st *
tls_ctx_get (struct ssl_ctx_st *ctx);

/** Put a reference to @p ctx, if not nullptr: the last frees it. */
extern void
tls_ctx_put (struct ssl_ctx_st *ctx);

/** The server's end of TLS for the client on socket @p fd, by @p ctx;
 *  nullptr if out of memory.
 */
extern struct ssl_st *
tls_create (struct ssl_ctx_st *ctx,
            int                fd);

/** Shake hands a flight further, a flight of the server's being sent
 *  whole; or, after the handshake, send what a read left to send.
 */
extern enum tls_shake
tls_shake (struct ssl_st *ssl);

/** As recv() into @p buf, from a record at most: 0 once the client has
 *  closed, or -EAGAIN if it waits for the socket, to read or to send first
 *  (tls_blocked()), else -EPROTO.
 */
extern ssize_t
tls_recv (struct ssl_st *ssl,
          void          *buf,
          size_t         len);

/** As send() of @p buf, a record of it: -EAGAIN if it waits for room,
 *  else -EPROTO.  Once it has waited, it must be called again with
 *  the same bytes, which may have moved, and at least as many, as a call
 *  that waited may have sent a record before, which the next one counts;
 *  so calls of TLS_RECORD bytes at most never send part of one unseen.
 */
extern ssize_t
tls_send (struct ssl_st *ssl,
          void const    *buf,
          size_t         len);

/** Whether bytes the client sent wait in @p ssl, which the socket no longer
 *  holds: a record's rest, or part of one.
 */
extern bool
tls_pending (struct ssl_st const *ssl);

/** Whether @p ssl's last call waited for room to send. */
extern bool
tls_blocked (struct ssl_st const *ssl);

/** Say the connection ends (close_notify), if @p ssl is not nullptr and its
 *  handshake is done, as far as the socket takes it now; tls_destroy(),
 *  which must follow, frees @p ssl and clears what this leaves in the error
 *  queue.
 */
extern void
tls_close (struct ssl_st *ssl);

/** Free @p *p_ssl, if not nullptr, saying nothing more to the client, and
 *  set it to nullptr.
 */
extern void
tls_destroy (struct ssl_st **p_ssl);

#endif /* TWINCEPTION_SRC_TLS_H_ */
