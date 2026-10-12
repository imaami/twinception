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
#include "util.h"

/* A key's passphrase is never asked for: an encrypted key fails to load,
 * rather than have OpenSSL ask the terminal. */
static int
tls_no_passphrase (char *buf,
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
tls_alpn (SSL                  *ssl,
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
tls_ctx_set_up (SSL_CTX *ctx)
{
	SSL_CTX_set_default_passwd_cb(ctx, tls_no_passphrase);
	SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS | SSL_MODE_NO_AUTO_CHAIN |
	                      SSL_MODE_ENABLE_PARTIAL_WRITE |
	                      SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	SSL_CTX_clear_mode(ctx, SSL_MODE_AUTO_RETRY);
	SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
	SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
	SSL_CTX_set_alpn_select_cb(ctx, tls_alpn, nullptr);
	return SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) &&
	       SSL_CTX_set_num_tickets(ctx, 1) &&
	       SSL_CTX_set1_groups_list(ctx, "?X25519MLKEM768:X25519:P-256");
}

/* The certificate chain from @p len bytes of PEM at @p pem: the leaf's, then
 * the others, which are sent as they are (NO_AUTO_CHAIN). */
static bool
tls_ctx_use_chain (SSL_CTX    *ctx,
                   char const *pem,
                   size_t      len)
{
	BIO *bio = len <= INT_MAX ? BIO_new_mem_buf(pem, (int)len) : nullptr;
	X509 *leaf = bio ? PEM_read_bio_X509_AUX(bio, nullptr, tls_no_passphrase, nullptr)
	                 : nullptr;
	bool ok = leaf && SSL_CTX_use_certificate(ctx, leaf) == 1;
	X509_free(leaf);
	for (X509 *ca; ok && (ca = PEM_read_bio_X509(bio, nullptr, tls_no_passphrase,
	                                             nullptr)); ) {
		ok = SSL_CTX_add0_chain_cert(ctx, ca) == 1;
		if (!ok)
			X509_free(ca);
	}
	BIO_free(bio);
	if (!ok)
		return false;
	/* No more certificates: the end of the text, or else an error. */
	unsigned long e = ERR_peek_last_error();
	if (ERR_GET_LIB(e) != ERR_LIB_PEM || ERR_GET_REASON(e) != PEM_R_NO_START_LINE)
		return false;
	ERR_clear_error();
	return true;
}

/* The private key from @p len bytes of PEM at @p pem. */
static bool
tls_ctx_use_key (SSL_CTX    *ctx,
                 char const *pem,
                 size_t      len)
{
	BIO *bio = len <= INT_MAX ? BIO_new_mem_buf(pem, (int)len) : nullptr;
	EVP_PKEY *key = bio ? PEM_read_bio_PrivateKey(bio, nullptr, tls_no_passphrase,
	                                              nullptr)
	                    : nullptr;
	BIO_free(bio);
	bool ok = key && SSL_CTX_use_PrivateKey(ctx, key) == 1;
	EVP_PKEY_free(key);
	return ok;
}

/* What is wrong with the type of @p ctx's key, or nullptr: ECDSA on a curve
 * TLS 1.3 signs with, Ed25519, or RSA if @p cfg allows it. */
static char const *
tls_ctx_key_type (SSL_CTX             *ctx,
                  struct config const *cfg)
{
	EVP_PKEY *key = SSL_CTX_get0_privatekey(ctx);
	if (EVP_PKEY_is_a(key, "ED25519"))
		return nullptr;
	if (EVP_PKEY_is_a(key, "RSA") || EVP_PKEY_is_a(key, "RSA-PSS"))
		return cfg->flags & CFG_TLS_RSA ? nullptr : "the key is RSA, which "
		                                            "needs --tls-allow-rsa";
	if (!EVP_PKEY_is_a(key, "EC"))
		return "the key is neither ECDSA nor Ed25519";
	static char const *const curves[] = { "prime256v1", "secp384r1", "secp521r1" };
	static char const other[] = "the key is ECDSA on a curve other than P-256, P-384 "
	                            "or P-521";
	char curve[32];
	size_t n;
	if (!EVP_PKEY_get_group_name(key, curve, sizeof curve, &n))
		return other;
	for (size_t i = 0; i < ARRAY_SIZE(curves); ++i)
		if (!strcmp(curve, curves[i]))
			return nullptr;
	return other;
}

/* Say in the @p len bytes at @p why that @p what failed, and OpenSSL's
 * reason, its last error, which the queue then lacks.  Returns ENOMEM if
 * OpenSSL ran out of memory, or reports no error, else EINVAL. */
static int
tls_fail (char       *why,
          size_t      len,
          char const *what)
{
	unsigned long e = 0;
	bool oom = false;
	for (unsigned long x; (x = ERR_get_error()); e = x)
		oom |= ERR_GET_REASON(x) == ERR_R_MALLOC_FAILURE;
	int lib = ERR_GET_LIB(e);
	int reason = ERR_GET_REASON(e);
	char const *text = e ? ERR_reason_error_string(e) : nullptr;
	if (lib == ERR_LIB_PEM && reason == PEM_R_BAD_PASSWORD_READ)
		util_format(why, len, "the key is encrypted");
	else if (lib == ERR_LIB_X509 && (reason == X509_R_KEY_VALUES_MISMATCH ||
	                                 reason == X509_R_KEY_TYPE_MISMATCH))
		util_format(why, len, "the key is not the certificate's");
	else if (text)
		util_format(why, len, "%s: %s", what, text);
	else
		util_format(why, len, "%s", what);
	return !e || oom ? ENOMEM : EINVAL;
}

/* Set @p ctx up, with the chain and the key, as tls_ctx_create() says. */
static int
tls_ctx_load (SSL_CTX             *ctx,
              struct config const *cfg,
              char const          *chain,
              size_t               chain_len,
              char const          *key,
              size_t               key_len,
              char                *why,
              size_t               len)
{
	if (!tls_ctx_set_up(ctx))
		return tls_fail(why, len, "cannot set TLS up");
	if (!tls_ctx_use_chain(ctx, chain, chain_len))
		return tls_fail(why, len, "the certificate chain");
	if (!tls_ctx_use_key(ctx, key, key_len))
		return tls_fail(why, len, "the key");
	char const *wrong = SSL_CTX_check_private_key(ctx) != 1
	                  ? "the key is not the certificate's"
	                  : tls_ctx_key_type(ctx, cfg);
	if (!wrong)
		return 0;
	util_format(why, len, "%s", wrong);
	return EINVAL;
}

struct ssl_ctx_st *
tls_ctx_create (struct config const *cfg,
                char const          *chain,
                size_t               chain_len,
                char const          *key,
                size_t               key_len,
                char                *why,
                size_t               len,
                int                 *e)
{
	ERR_clear_error();
	SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
	*e = ctx ? tls_ctx_load(ctx, cfg, chain, chain_len, key, key_len, why, len)
	         : tls_fail(why, len, "cannot make a TLS context");
	ERR_clear_error();
	if (*e) {
		SSL_CTX_free(ctx);
		return nullptr;
	}
	return ctx;
}

struct ssl_ctx_st *
tls_ctx_get (struct ssl_ctx_st *ctx)
{
	SSL_CTX_up_ref(ctx);
	return ctx;
}

void
tls_ctx_put (struct ssl_ctx_st *ctx)
{
	SSL_CTX_free(ctx);
}

struct ssl_st *
tls_create (struct ssl_ctx_st *ctx,
            int                fd)
{
	ERR_clear_error();
	SSL *ssl = SSL_new(ctx);
	if (ssl && SSL_set_fd(ssl, fd) == 1) {
		SSL_set_accept_state(ssl);
		return ssl;
	}
	tls_destroy(&ssl);
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
		return -EAGAIN;
	default:
		ERR_clear_error();
		return -EPROTO;
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
	return wait ? -EAGAIN : -EPROTO;
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
tls_close (struct ssl_st *ssl)
{
	if (!SSL_is_init_finished(ssl))
		return;
	ERR_clear_error();
	(void)SSL_shutdown(ssl);
	ERR_clear_error();
}

void
tls_destroy (struct ssl_st **p_ssl)
{
	if (!p_ssl || !*p_ssl)
		return;
	SSL_free(*p_ssl);
	*p_ssl = nullptr;
	ERR_clear_error();
}
