/** @file TLS (tls.h). */
#include <errno.h>
#include <limits.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "tls.h"

/* A key's passphrase is never asked for: an encrypted key fails to load,
 * rather than have OpenSSL ask the terminal. */
static int
no_passphrase (char *buf,
               int   size,
               int   rwflag,
               void *u)
{
	(void)buf;
	(void)size;
	(void)rwflag;
	(void)u;
	return -1;
}

/* HTTP/1.1, among the protocols the client offers; or else the handshake
 * fails, as RFC 7301 says.  A client that offers none speaks it too.  The
 * list is scanned by hand: SSL_select_next_proto() points at a protocol of
 * the client's even when none matches. */
static int
alpn (SSL                  *ssl,
      unsigned char const **out,
      unsigned char        *outlen,
      unsigned char const  *in,
      unsigned int          inlen,
      void                 *arg)
{
	(void)ssl;
	(void)arg;
	static unsigned char const h11[] = "\x08http/1.1";
	for (unsigned int i = 0; i < inlen; i += 1u + in[i])
		if (inlen - i >= sizeof h11 - 1 && !memcmp(in + i, h11, sizeof h11 - 1)) {
			*out = in + i + 1;
			*outlen = sizeof h11 - 2;
			return SSL_TLSEXT_ERR_OK;
		}
	return SSL_TLSEXT_ERR_ALERT_FATAL;
}

/* What every connection of @p ctx does: TLS 1.3; its records read one a
 * call, never ahead, and the call done after a message of the client's that
 * is not data, such as a key update, as a client could send those without
 * end (one read went through 20 000 in 250 ms with AUTO_RETRY), and what is
 * read ahead lies where epoll does not see it; records written one at a
 * time, from output that may move between tries; one stateless ticket, as
 * no session is cached; a client that closes without close_notify ends its
 * input, as HTTP's framing catches a cut request. */
static bool
set_up (SSL_CTX *ctx)
{
	SSL_CTX_set_default_passwd_cb(ctx, no_passphrase);
	SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS | SSL_MODE_NO_AUTO_CHAIN |
	                      SSL_MODE_ENABLE_PARTIAL_WRITE |
	                      SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	SSL_CTX_clear_mode(ctx, SSL_MODE_AUTO_RETRY);
	SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
	SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
	SSL_CTX_set_alpn_select_cb(ctx, alpn, nullptr);
	return SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) &&
	       SSL_CTX_set_num_tickets(ctx, 1) &&
	       SSL_CTX_set1_groups_list(ctx, "?X25519MLKEM768:X25519:P-256");
}

/* The certificate chain from @p len bytes of PEM at @p pem: the leaf's, then
 * the others, which are sent as they are (NO_AUTO_CHAIN). */
static bool
use_chain (SSL_CTX    *ctx,
           char const *pem,
           size_t      len)
{
	BIO *bio = len <= INT_MAX ? BIO_new_mem_buf(pem, (int)len) : nullptr;
	X509 *leaf = bio ? PEM_read_bio_X509_AUX(bio, nullptr, no_passphrase, nullptr)
	                 : nullptr;
	bool ok = leaf && SSL_CTX_use_certificate(ctx, leaf) == 1;
	X509_free(leaf);
	while (ok) {
		X509 *ca = PEM_read_bio_X509(bio, nullptr, no_passphrase, nullptr);
		if (!ca) {
			/* The end of the text, or else an error. */
			unsigned long e = ERR_peek_last_error();
			ok = ERR_GET_LIB(e) == ERR_LIB_PEM &&
			     ERR_GET_REASON(e) == PEM_R_NO_START_LINE;
			if (ok)
				ERR_clear_error();
			break;
		}
		if (SSL_CTX_add0_chain_cert(ctx, ca) != 1) {
			X509_free(ca);
			ok = false;
		}
	}
	BIO_free(bio);
	return ok;
}

/* The private key from @p len bytes of PEM at @p pem. */
static bool
use_key (SSL_CTX    *ctx,
         char const *pem,
         size_t      len)
{
	BIO *bio = len <= INT_MAX ? BIO_new_mem_buf(pem, (int)len) : nullptr;
	EVP_PKEY *key = bio ? PEM_read_bio_PrivateKey(bio, nullptr, no_passphrase, nullptr)
	                    : nullptr;
	BIO_free(bio);
	bool ok = key && SSL_CTX_use_PrivateKey(ctx, key) == 1;
	EVP_PKEY_free(key);
	return ok;
}

/* What is wrong with the type of @p ctx's key, or nullptr: ECDSA on a curve
 * TLS 1.3 signs with, Ed25519, or RSA if @p cfg allows it. */
static char const *
key_type (SSL_CTX             *ctx,
          struct config const *cfg)
{
	EVP_PKEY *key = SSL_CTX_get0_privatekey(ctx);
	if (EVP_PKEY_is_a(key, "ED25519"))
		return nullptr;
	if (EVP_PKEY_is_a(key, "EC")) {
		char curve[32];
		size_t n;
		return EVP_PKEY_get_group_name(key, curve, sizeof curve, &n) &&
		       (!strcmp(curve, "prime256v1") || !strcmp(curve, "secp384r1") ||
		        !strcmp(curve, "secp521r1"))
		       ? nullptr : "the key is ECDSA on a curve other than P-256, P-384 "
		                   "or P-521";
	}
	if (EVP_PKEY_is_a(key, "RSA") || EVP_PKEY_is_a(key, "RSA-PSS"))
		return cfg->flags & CFG_TLS_RSA ? nullptr : "the key is RSA, which "
		                                            "needs --tls-allow-rsa";
	return "the key is neither ECDSA nor Ed25519";
}

struct ssl_ctx_st *
tls_new (struct config const *cfg,
         char const          *chain,
         size_t               chain_len,
         char const          *key,
         size_t               key_len,
         char                *why,
         size_t               len)
{
	ERR_clear_error();
	SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
	char const *what = !ctx ? "cannot make a TLS context"
	                 : !set_up(ctx) ? "cannot set TLS up"
	                 : !use_chain(ctx, chain, chain_len) ? "the certificate chain"
	                 : !use_key(ctx, key, key_len) ? "the key"
	                 : nullptr;
	/* OpenSSL says why, its last error, but when out of memory, which may
	 * be any of them, or none. */
	unsigned long e = 0;
	bool oom = false;
	for (unsigned long x; what && (x = ERR_get_error()); e = x)
		oom |= ERR_GET_REASON(x) == ERR_R_MALLOC_FAILURE;
	int code = !e || oom ? ENOMEM : EINVAL;
	if (!what && SSL_CTX_check_private_key(ctx) != 1) {
		what = "the key is not the certificate's";
		code = EINVAL;
	}
	if (!what && (what = key_type(ctx, cfg)))
		code = EINVAL;
	ERR_clear_error();
	if (!what)
		return ctx;
	SSL_CTX_free(ctx);
	char const *reason = e ? ERR_reason_error_string(e) : nullptr;
	int lib = ERR_GET_LIB(e);
	if (lib == ERR_LIB_PEM && ERR_GET_REASON(e) == PEM_R_BAD_PASSWORD_READ)
		snprintf(why, len, "the key is encrypted");
	else if (lib == ERR_LIB_X509 && (ERR_GET_REASON(e) == X509_R_KEY_VALUES_MISMATCH ||
	                                 ERR_GET_REASON(e) == X509_R_KEY_TYPE_MISMATCH))
		snprintf(why, len, "the key is not the certificate's");
	else
		snprintf(why, len, "%s%s%s", what, reason ? ": " : "", reason ? reason : "");
	errno = code;
	return nullptr;
}

struct ssl_ctx_st *
tls_get (struct ssl_ctx_st *ctx)
{
	SSL_CTX_up_ref(ctx);
	return ctx;
}

void
tls_put (struct ssl_ctx_st *ctx)
{
	SSL_CTX_free(ctx);
}

struct ssl_st *
tls_open (struct ssl_ctx_st *ctx,
          int                fd)
{
	ERR_clear_error();
	SSL *ssl = SSL_new(ctx);
	if (ssl && SSL_set_fd(ssl, fd) == 1) {
		SSL_set_accept_state(ssl);
		return ssl;
	}
	tls_free(ssl);
	return nullptr;
}

enum tls_shake
tls_shake (struct ssl_st *ssl)
{
	ERR_clear_error();
	int r = SSL_do_handshake(ssl);
	if (r == 1)
		return TLS_SHAKEN;
	switch (SSL_get_error(ssl, r)) {
	case SSL_ERROR_WANT_READ:
		return TLS_READ;
	case SSL_ERROR_WANT_WRITE:
		return TLS_WRITE;
	default:
		ERR_clear_error();
		return TLS_FAILED;
	}
}

ssize_t
tls_recv (struct ssl_st *ssl,
          void          *buf,
          size_t         len)
{
	size_t n = 0;
	ERR_clear_error();
	int r = SSL_read_ex(ssl, buf, len, &n);
	if (r > 0)
		return (ssize_t)n;
	switch (SSL_get_error(ssl, r)) {
	case SSL_ERROR_ZERO_RETURN:
		return 0;
	case SSL_ERROR_WANT_READ:
	case SSL_ERROR_WANT_WRITE:
		errno = EAGAIN;
		return -1;
	default:
		ERR_clear_error();
		errno = EPROTO;
		return -1;
	}
}

ssize_t
tls_send (struct ssl_st *ssl,
          void const    *buf,
          size_t         len)
{
	size_t n = 0;
	ERR_clear_error();
	int r = SSL_write_ex(ssl, buf, len, &n);
	if (r > 0)
		return (ssize_t)n;
	bool wait = SSL_get_error(ssl, r) == SSL_ERROR_WANT_WRITE;
	ERR_clear_error();
	errno = wait ? EAGAIN : EPROTO;
	return -1;
}

bool
tls_pending (struct ssl_st const *ssl)
{
	return SSL_has_pending(ssl);
}

bool
tls_blocked (struct ssl_st const *ssl)
{
	return SSL_want_write(ssl);
}

void
tls_end (struct ssl_st *ssl)
{
	if (SSL_is_init_finished(ssl)) {
		ERR_clear_error();
		(void)SSL_shutdown(ssl);
	}
	tls_free(ssl);
}

void
tls_free (struct ssl_st *ssl)
{
	SSL_free(ssl);
	ERR_clear_error();
}
