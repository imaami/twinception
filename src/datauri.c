/** @file datauri.c
 *
 * RFC 2397 data: URIs.
 */
#include <errno.h>
#include <string.h>

#include "datauri.h"
#include "util.h"

/* The value of base64 digit @p c of the standard alphabet, or 64 if @p c
 * is not one.
 */
static unsigned
datauri_digit (unsigned char c)
{
	unsigned const u = c;
	return u - 'A' < 26 ? u - 'A'
	     : u - 'a' < 26 ? u - 'a' + 26
	     : u - '0' < 10 ? u - '0' + 52
	     : u == '+' ? 62 : u == '/' ? 63 : 64;
}

/* Checks base64 data in @p d and counts its bytes: digits, then at most
 * two '=' that fill the last group of four.
 */
static int
datauri_parse_base64 (struct datauri *d)
{
	char const *s = d->encoded.data;
	size_t n = d->encoded.len;
	size_t pad = 0;
	while (n && s[n - 1] == '=' && pad < 2) {
		--n;
		++pad;
	}
	if (n % 4 == 1 || (pad && (n + pad) % 4))
		return EINVAL;
	for (size_t i = 0; i < n; ++i)
		if (datauri_digit((unsigned char)s[i]) > 63)
			return EINVAL;
	d->encoded.len = n;
	d->size = n / 4 * 3 + (n % 4 ? n % 4 - 1 : 0);
	return 0;
}

/* Checks percent-encoded data in @p d and counts its bytes. */
static int
datauri_parse_percent (struct datauri *d)
{
	char const *s = d->encoded.data;
	size_t const len = d->encoded.len;
	size_t size = 0;
	for (size_t i = 0; i < len; ++size) {
		if (s[i] != '%') {
			++i;
			continue;
		}
		if (len - i < 3)
			return EINVAL;
		if (util_hex_digit((unsigned char)s[i + 1]) > 15 ||
		    util_hex_digit((unsigned char)s[i + 2]) > 15)
			return EINVAL;
		i += 3;
	}
	d->size = size;
	return 0;
}

int
datauri_init (struct datauri   *d,
              struct cstr_view  uri)
{
	struct cstr_view const scheme = CSTR_VIEW("data:");
	struct cstr_view const base64 = CSTR_VIEW(";base64");
	struct cstr_view rest = cstr_view_slice(uri, scheme.len, SIZE_MAX), media;
	if (!d || !cstr_view_starts_case(uri, scheme) || !cstr_view_cut(&rest, ',', &media))
		return EINVAL;
	*d = (struct datauri){
		.encoded = rest,
		.base64 = media.len >= base64.len &&
		          cstr_view_equal_case(cstr_view_slice(media, media.len - base64.len, SIZE_MAX),
		                               base64),
	};
	return d->base64 ? datauri_parse_base64(d) : datauri_parse_percent(d);
}

/* Decodes the percent-encoded data of @p d into the d->size bytes at
 * @p out.
 */
static void
datauri_decode_percent (struct datauri const *d,
                        unsigned char        *out)
{
	char const *s = d->encoded.data;
	for (size_t i = 0, len = d->encoded.len; i < len; ++out) {
		if (s[i] != '%') {
			*out = (unsigned char)s[i++];
			continue;
		}
		*out = (unsigned char)(util_hex_digit((unsigned char)s[i + 1]) << 4 |
		                       util_hex_digit((unsigned char)s[i + 2]));
		i += 3;
	}
}

/* Decodes the base64 data of @p d into the d->size bytes at @p out. */
static void
datauri_decode_base64 (struct datauri const *d,
                       unsigned char        *out)
{
	char const *s = d->encoded.data;
	size_t const len = d->encoded.len;
	uint32_t v = 0;
	for (size_t i = 0; i < len; ++i) {
		v = v << 6 | datauri_digit((unsigned char)s[i]);
		if (i % 4 == 3) {
			*out++ = (unsigned char)(v >> 16);
			*out++ = (unsigned char)(v >> 8);
			*out++ = (unsigned char)v;
			v = 0;
		}
	}
	/* The last group of two or three digits decodes to one or two bytes. */
	size_t rest = len % 4;
	if (rest == 2) {
		*out = (unsigned char)(v >> 4);
	} else if (rest == 3) {
		*out++ = (unsigned char)(v >> 10);
		*out = (unsigned char)(v >> 2);
	}
}

void
datauri_decode (struct datauri const *d,
                unsigned char        *out)
{
	if (d && out)
		(d->base64 ? datauri_decode_base64 : datauri_decode_percent)(d, out);
}
