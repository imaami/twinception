/** @file Unit tests for data: URIs (src/datauri.c). */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "datauri.h"

static unsigned failures;

#define CHECK(cond) \
	((cond) ? (void)0 \
	        : (void)(++failures, fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	                                     __FILE__, __LINE__, #cond)))

/* Whether @p uri decodes to the @p len bytes at @p want. */
static bool
decodes (char const *uri,
         void const *want,
         size_t      len)
{
	struct datauri d;
	unsigned char out[64];
	if (datauri_parse(uri, strlen(uri), &d) || d.size != len || len > sizeof out)
		return false;
	memset(out, 0xa5, sizeof out);
	datauri_decode(&d, out);
	return !memcmp(out, want, len) && (len == sizeof out || out[len] == 0xa5);
}

/* Whether @p uri is refused. */
static bool
refused (char const *uri)
{
	struct datauri d;
	return datauri_parse(uri, strlen(uri), &d) == EINVAL;
}

int
main (void)
{
	/* Base64, with and without padding, any case of the scheme and of
	 * ";base64", with a media type and parameters or none. */
	CHECK(decodes("data:;base64,AAEC/w==", "\x00\x01\x02\xff", 4));
	CHECK(decodes("data:;base64,AAEC/w", "\x00\x01\x02\xff", 4));
	CHECK(decodes("DATA:application/octet-stream;BASE64,SGVsbG8=", "Hello", 5));
	CHECK(decodes("data:image/png;name=a.png;base64,SGVsbG8h", "Hello!", 6));
	CHECK(decodes("data:;base64,", "", 0));
	CHECK(decodes("data:;base64,+/+/", "\xfb\xff\xbf", 3));
	/* Percent-encoded data, the default. */
	CHECK(decodes("data:,a%20b%2C", "a b,", 4));
	CHECK(decodes("data:text/plain;charset=utf-8,%00%FFx", "\x00\xffx", 3));
	CHECK(decodes("data:,", "", 0));
	/* A ";base64" that is not at the end of the head is a parameter. */
	CHECK(decodes("data:;base64x,SGk", "SGk", 3));

	/* Refused: not a data: URI, no comma, digits outside the alphabet,
	 * padding in the middle or too much of it, a group of one digit, and
	 * a broken percent escape. */
	CHECK(refused("http://example.com/a.png"));
	CHECK(refused("data:"));
	CHECK(refused("data:;base64"));
	CHECK(refused("data:;base64,SGV-bG8"));
	CHECK(refused("data:;base64,SGV=bG8="));
	CHECK(refused("data:;base64,SGVsbA==="));
	CHECK(refused("data:;base64,SGVsbG8=x"));
	CHECK(refused("data:;base64,A"));
	CHECK(refused("data:;base64,AAAAA"));
	CHECK(refused("data:,%zz"));
	CHECK(refused("data:,%4"));
	CHECK(refused("data:,abc%"));

	struct datauri d;
	CHECK(datauri_parse(nullptr, 0, &d) == EINVAL);
	CHECK(datauri_parse("data:,", 6, nullptr) == EINVAL);

	if (failures)
		fprintf(stderr, "datauri: %u checks failed\n", failures);
	else
		puts("datauri: all checks passed");
	return failures != 0;
}
