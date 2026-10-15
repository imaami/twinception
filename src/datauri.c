/** @file RFC 2397 data: URIs (datauri.h). */
#include <errno.h>
#include <string.h>
#include <strings.h>

#include "datauri.h"

/* The value of base64 digit @p c of the standard alphabet, or -1. */
static int
datauri_digit (unsigned char c)
{
	return c >= 'A' && c <= 'Z' ? c - 'A'
	     : c >= 'a' && c <= 'z' ? c - 'a' + 26
	     : c >= '0' && c <= '9' ? c - '0' + 52
	     : c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* The value of hexadecimal digit @p c, or -1. */
static int
datauri_hex (unsigned char c)
{
	return c >= '0' && c <= '9' ? c - '0'
	     : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
}

/* Checks base64 data in @p d and counts its bytes: digits, then at most
 * two '=' that fill the last group of four. */
static int
datauri_parse_base64 (struct datauri *d)
{
	size_t n = d->len;
	size_t pad = 0;
	while (n && d->data[n - 1] == '=' && pad < 2) {
		--n;
		++pad;
	}
	if (n % 4 == 1 || (pad && (n + pad) % 4))
		return EINVAL;
	for (size_t i = 0; i < n; ++i)
		if (datauri_digit((unsigned char)d->data[i]) < 0)
			return EINVAL;
	d->len = n;
	d->size = n / 4 * 3 + (n % 4 ? n % 4 - 1 : 0);
	return 0;
}

/* Checks percent-encoded data in @p d and counts its bytes. */
static int
datauri_parse_percent (struct datauri *d)
{
	size_t size = 0;
	for (size_t i = 0; i < d->len; ++size) {
		if (d->data[i] != '%') {
			++i;
			continue;
		}
		if (d->len - i < 3)
			return EINVAL;
		if (datauri_hex((unsigned char)d->data[i + 1]) < 0 ||
		    datauri_hex((unsigned char)d->data[i + 2]) < 0)
			return EINVAL;
		i += 3;
	}
	d->size = size;
	return 0;
}

int
datauri_parse (char const     *uri,
               size_t          len,
               struct datauri *d)
{
	static char const scheme[] = "data:";
	static char const base64[] = ";base64";
	size_t s = sizeof scheme - 1;
	size_t b = sizeof base64 - 1;
	if (!uri || !d || len < s || strncasecmp(uri, scheme, s))
		return EINVAL;
	char const *comma = memchr(uri + s, ',', len - s);
	if (!comma)
		return EINVAL;
	size_t head = (size_t)(comma - uri);
	*d = (struct datauri){
		.data = comma + 1,
		.len = len - head - 1,
		.base64 = head - s >= b && !strncasecmp(comma - b, base64, b),
	};
	return d->base64 ? datauri_parse_base64(d) : datauri_parse_percent(d);
}

void
datauri_decode (struct datauri const *d,
                unsigned char        *out)
{
	if (!d || !out)
		return;
	if (!d->base64) {
		for (size_t i = 0; i < d->len; ) {
			if (d->data[i] == '%') {
				*out++ = (unsigned char)(datauri_hex((unsigned char)d->data[i + 1]) << 4 |
				                         datauri_hex((unsigned char)d->data[i + 2]));
				i += 3;
			} else {
				*out++ = (unsigned char)d->data[i++];
			}
		}
		return;
	}
	uint32_t v = 0;
	for (size_t i = 0; i < d->len; ++i) {
		v = v << 6 | (uint32_t)datauri_digit((unsigned char)d->data[i]);
		if (i % 4 == 3) {
			*out++ = (unsigned char)(v >> 16);
			*out++ = (unsigned char)(v >> 8);
			*out++ = (unsigned char)v;
			v = 0;
		}
	}
	/* The last group of two or three digits gives one or two bytes. */
	size_t rest = d->len % 4;
	if (rest == 2) {
		*out = (unsigned char)(v >> 4);
	} else if (rest == 3) {
		*out++ = (unsigned char)(v >> 10);
		*out = (unsigned char)(v >> 2);
	}
}
