/** @file Tickets for transfers of notes over HTTP (ticket.h). */
#include <errno.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <string.h>
#include <sys/random.h>
#include <sys/types.h>

#include "ticket.h"

/* The ticket's fields as bytes: the direction, the side, the principal and
 * the expiry (each 8 bytes, little-endian), the thread, the length of the
 * name and the name; then the first MAC bytes of the HMAC of these. */
enum {
	FIXED = 2 + 8 + 8 + TICKET_THREAD + 1,
	MAC   = 16,
	RAW   = FIXED + TICKET_NAME + MAC,
};
static_assert((RAW * 4 + 2) / 3 + 1 <= TICKET_TEXT);

static char const digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                             "0123456789-_";

/* The value of base64url digit @p c, or -1. */
static int
ticket_digit (unsigned char c)
{
	char const *d = c ? strchr(digits, c) : nullptr;
	return d ? (int)(d - digits) : -1;
}

int
ticket_key_init (struct ticket_key *key)
{
	if (!key)
		return EINVAL;
	/* For so few bytes, getrandom() returns fewer bytes only if a signal
	 * interrupts it before it starts. */
	ssize_t got;
	while ((got = getrandom(key->bytes, sizeof key->bytes, 0)) < 0)
		if (errno != EINTR)
			return errno;
	return got == (ssize_t)sizeof key->bytes ? 0 : EIO;
}

/* The HMAC of the @p len bytes at @p raw with @p key, its first MAC bytes
 * in @p mac; false if libcrypto fails. */
static bool
ticket_mac (struct ticket_key const *key,
            unsigned char const     *raw,
            size_t                   len,
            unsigned char            mac[MAC])
{
	unsigned char full[EVP_MAX_MD_SIZE];
	unsigned n = 0;
	if (!HMAC(EVP_sha256(), key->bytes, (int)sizeof key->bytes, raw, len, full, &n) ||
	    n < MAC)
		return false;
	memcpy(mac, full, MAC);
	return true;
}

static void
ticket_put64 (unsigned char *p,
              uint64_t       v)
{
	for (size_t i = 0; i < 8; ++i, v >>= 8)
		p[i] = (unsigned char)v;
}

static uint64_t
ticket_get64 (unsigned char const *p)
{
	uint64_t v = 0;
	for (size_t i = 8; i--; )
		v = v << 8 | p[i];
	return v;
}

/* Whether @p c is a lowercase hexadecimal digit. */
static bool
ticket_hex (char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/* Whether the fields of @p t are valid. */
static bool
ticket_valid (struct ticket const *t)
{
	if ((t->dir != TICKET_UPLOAD && t->dir != TICKET_DOWNLOAD) || t->side > 1 ||
	    !t->name_len)
		return false;
	for (size_t i = 0; i < TICKET_THREAD; ++i)
		if (!ticket_hex(t->thread[i]))
			return false;
	return true;
}

size_t
ticket_write (struct ticket_key const *key,
              struct ticket const     *t,
              char                    *out,
              size_t                   size)
{
	if (!key || !t || !out || size < TICKET_TEXT || !ticket_valid(t))
		return 0;
	unsigned char raw[RAW];
	raw[0] = t->dir;
	raw[1] = t->side;
	ticket_put64(raw + 2, t->principal);
	ticket_put64(raw + 10, t->expiry);
	memcpy(raw + 18, t->thread, TICKET_THREAD);
	raw[FIXED - 1] = t->name_len;
	memcpy(raw + FIXED, t->name, t->name_len);
	size_t len = FIXED + t->name_len;
	if (!ticket_mac(key, raw, len, raw + len))
		return 0;
	len += MAC;
	/* Base64url without padding: 4 digits for each 3 bytes, and 2 or 3
	 * for the 1 or 2 bytes at the end. */
	size_t k = 0;
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)raw[i] << 16;
		size_t n = len - i < 3 ? len - i : 3;
		if (n > 1)
			v |= (uint32_t)raw[i + 1] << 8;
		if (n > 2)
			v |= raw[i + 2];
		for (size_t j = 0; j <= n; ++j)
			out[k++] = digits[v >> (18 - 6 * j) & 63];
	}
	out[k] = '\0';
	return k;
}

int
ticket_read (struct ticket_key const *key,
             char const              *text,
             size_t                   len,
             uint64_t                 now,
             struct ticket           *t)
{
	if (!key || !text || !t || len >= TICKET_TEXT || len % 4 == 1)
		return EINVAL;
	unsigned char raw[RAW];
	size_t n = 0;
	uint32_t v = 0;
	for (size_t i = 0; i < len; ++i) {
		int d = ticket_digit((unsigned char)text[i]);
		if (d < 0)
			return EINVAL;
		v = v << 6 | (uint32_t)d;
		if (i % 4 == 3 || i == len - 1) {
			size_t got = i % 4;
			v <<= 6 * (3 - got);
			for (size_t j = 0; j < got; ++j)
				raw[n++] = (unsigned char)(v >> (16 - 8 * j));
			v = 0;
		}
	}
	if (n < FIXED + 1 + MAC || n != (size_t)FIXED + raw[FIXED - 1] + MAC)
		return EINVAL;
	size_t body = n - MAC;
	unsigned char mac[MAC];
	if (!ticket_mac(key, raw, body, mac) || CRYPTO_memcmp(mac, raw + body, MAC))
		return EINVAL;
	*t = (struct ticket){
		.principal = ticket_get64(raw + 2),
		.expiry = ticket_get64(raw + 10),
		.name_len = raw[FIXED - 1],
		.dir = raw[0],
		.side = raw[1],
	};
	memcpy(t->thread, raw + 18, TICKET_THREAD);
	memcpy(t->name, raw + FIXED, t->name_len);
	if (!ticket_valid(t))
		return EINVAL;
	return t->expiry < now ? ETIMEDOUT : 0;
}
