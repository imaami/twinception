/** @file Unit tests for the gate of UTF-8 text (src/u8.c), which the parser
 *  of the submodule src/c.utf-8 reads, against a reference that decodes RFC
 *  3629 the plain way.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "u8.h"

/* Bytes at each edge of the ranges of UTF-8: NUL, ASCII, continuation
 * bytes, and lead bytes of each length, with the limits of the second byte
 * after 0xe0, 0xed, 0xf0 and 0xf4. */
static unsigned char const edge[] = {
	0x00, 0x41, 0x7f, 0x80, 0x8f, 0x90, 0x9f, 0xa0, 0xbf, 0xc0, 0xc1, 0xc2, 0xdf,
	0xe0, 0xe1, 0xec, 0xed, 0xee, 0xef, 0xf0, 0xf1, 0xf3, 0xf4, 0xf5, 0xf7, 0xff,
};

static uint64_t seed = 0x9e3779b97f4a7c15;

/* A pseudorandom number below @p n (xorshift64). */
static size_t
random_below (size_t n)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return (size_t)(seed % n);
}

/* The code point that the @p n bytes at @p s begin with, its length in
 * @p *len, or UINT32_MAX if they begin with no whole code point. */
static uint32_t
reference (unsigned char const *s,
           size_t               n,
           size_t              *len)
{
	static uint32_t const least[] = { 0, 0, 0x80, 0x800, 0x10000 };
	size_t k = !n ? 0 : s[0] < 0x80 ? 1 : s[0] < 0xc0 ? 0 : s[0] < 0xe0 ? 2 :
	           s[0] < 0xf0 ? 3 : s[0] < 0xf8 ? 4 : 0;
	if (!k || k > n)
		return UINT32_MAX;
	uint32_t c = k > 1 ? s[0] & (0x7fu >> k) : s[0];
	for (size_t i = 1; i < k; ++i) {
		if ((s[i] & 0xc0) != 0x80)
			return UINT32_MAX;
		c = c << 6 | (s[i] & 0x3fu);
	}
	if (c < least[k] || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
		return UINT32_MAX;
	*len = k;
	return c;
}

/* Whether the @p n bytes at @p s begin a code point that more bytes can
 * complete. Validity hangs on the lead byte and the second byte, so one of
 * the continuation bytes 0x80, 0x90 and 0xa0 repeated completes it if any
 * bytes do. */
static bool
reference_cut (unsigned char const *s,
               size_t               n)
{
	for (unsigned char x = 0x80; n < 4 && x <= 0xa0; x += 0x10) {
		unsigned char b[4] = { x, x, x, x };
		size_t k;
		memcpy(b, s, n);
		if (reference(b, sizeof b, &k) != UINT32_MAX && k > n)
			return true;
	}
	return false;
}

/* The bytes and the characters of the longest text that the @p n bytes at
 * @p s begin with, which ends at a NUL if @p nul is false. */
static size_t
reference_text (unsigned char const *s,
                size_t               n,
                bool                 nul,
                size_t              *count)
{
	size_t i = 0;
	*count = 0;
	for (size_t k; i < n && (nul || s[i]) && reference(s + i, n - i, &k) != UINT32_MAX; i += k)
		++*count;
	return i;
}

/* u8_mend() as the reference makes it. */
static size_t
reference_mend (unsigned char *p,
                size_t         n,
                bool           last)
{
	size_t i = 0, count;
	while ((i += reference_text(p + i, n - i, true, &count)) < n) {
		if (!last && reference_cut(p + i, n - i))
			return i;
		p[i++] = '?';
	}
	return n;
}

/* Whether u8_mend() makes of the @p n bytes at @p s what the reference
 * makes. */
static bool
mends (unsigned char const *s,
       size_t               n,
       bool                 last)
{
	unsigned char a[16], b[16];
	memcpy(a, s, n);
	memcpy(b, s, n);
	return u8_mend((char *)a, n, last) == reference_mend(b, n, last) && !memcmp(a, b, n);
}

/* Whether the @p n bytes at @p s, given to a gate @p step bytes at a time,
 * measure as the reference does, and fail it as they should. */
static bool
gates (unsigned char const *s,
       size_t               n,
       size_t               step)
{
	size_t count;
	size_t const len = reference_text(s, n, false, &count);
	bool const cut = len < n && reference_cut(s + len, n - len);
	struct u8_gate gate = u8_gate();
	int e = 0;
	for (size_t at = 0; at < n; at += step)
		e = u8_gate_pass(&gate, (struct cstr_view){ (char const *)s + at,
		                                            n - at < step ? n - at : step });
	if (gate.len != len || gate.count != count || u8_gate_end(&gate) != (len < n ? EILSEQ : 0))
		return false;
	/* A character that the bytes end inside fails only at the end. */
	return e == (len < n && !cut ? EILSEQ : 0);
}

int
main (void)
{
	/* The least and the greatest code point of each length. */
	uint32_t c = 0;
	CHECK(u8_char(CSTR_VIEW("A"), &c) == 1 && c == 'A');
	CHECK(u8_char(CSTR_VIEW("\xc2\x80"), &c) == 2 && c == 0x80);
	CHECK(u8_char(CSTR_VIEW("\xdf\xbf"), &c) == 2 && c == 0x7ff);
	CHECK(u8_char(CSTR_VIEW("\xe0\xa0\x80"), &c) == 3 && c == 0x800);
	CHECK(u8_char(CSTR_VIEW("\xed\x9f\xbf"), &c) == 3 && c == 0xd7ff);
	CHECK(u8_char(CSTR_VIEW("\xef\xbf\xbf"), &c) == 3 && c == 0xffff);
	CHECK(u8_char(CSTR_VIEW("\xf0\x90\x80\x80"), &c) == 4 && c == 0x10000);
	CHECK(u8_char(CSTR_VIEW("\xf4\x8f\xbf\xbf" "A"), &c) == 4 && c == 0x10ffff);

	/* NUL, overlong forms, surrogates, code points past U+10FFFF, bytes
	 * that begin none, and a code point cut short, if only by the view. */
	CHECK(!u8_char(CSTR_VIEW("\0"), &c) && !u8_char((struct cstr_view){}, &c));
	CHECK(!u8_char(CSTR_VIEW("\xc0\x80"), &c) && !u8_char(CSTR_VIEW("\xe0\x9f\xbf"), &c));
	CHECK(!u8_char(CSTR_VIEW("\xf0\x8f\xbf\xbf"), &c) && !u8_char(CSTR_VIEW("\xed\xa0\x80"), &c));
	CHECK(!u8_char(CSTR_VIEW("\xf4\x90\x80\x80"), &c) && !u8_char(CSTR_VIEW("\xf5\x80\x80\x80"), &c));
	CHECK(!u8_char(CSTR_VIEW("\x80"), &c) && !u8_char(CSTR_VIEW("\xff"), &c));
	CHECK(!u8_char((struct cstr_view){ "\xe2\x82\xac", 2 }, &c));

	/* Text is UTF-8 without a NUL, measured in bytes and characters; it
	 * stops before a byte that breaks it, and before a character that the
	 * bytes end inside. */
	int e = -1;
	struct u8_text t = u8_text((struct cstr_view){}, &e);
	CHECK(!e && !t.len && !t.count);
	t = u8_text(CSTR_VIEW("a\xc3\xa4" "b\xe2\x82\xac"), &e);
	CHECK(!e && t.len == 7 && t.count == 4 && t.view.len == t.len);
	t = u8_text(CSTR_VIEW("ab\0c"), &e);
	CHECK(e == EILSEQ && t.len == 2 && t.count == 2);
	t = u8_text(CSTR_VIEW("\xe2\x82\xac\xe2\x82"), &e);
	CHECK(e == EILSEQ && t.len == 3 && t.count == 1);
	t = u8_text(CSTR_VIEW("\xed\xa0\x80"), &e);
	CHECK(e == EILSEQ && !t.len && !t.count);
	t = u8_text((struct cstr_view){ nullptr, 1 }, &e);
	CHECK(e == EFAULT && !t.len);
	CHECK(!u8_check(CSTR_VIEW("\xc3\xa4")) && u8_check(CSTR_VIEW("\xc3")) == EILSEQ);
	for (size_t i = 0; i < 8; ++i) {
		char b[12] = "xxxxxxxx";
		memcpy(b + i, "\xf0\x9f\x98\x80", 4);
		t = u8_text((struct cstr_view){ b, i + 4 }, &e);
		CHECK(!e && t.len == i + 4 && t.count == i + 1);
		t = u8_text((struct cstr_view){ b, i + 3 }, &e);
		CHECK(e == EILSEQ && t.len == i && t.count == i);
	}

	/* A gate goes on with a character that a piece ends inside, but fails
	 * from a byte that breaks text on. */
	struct u8_gate g = u8_gate();
	CHECK(!u8_gate_pass(&g, CSTR_VIEW("a\xf0\x9f")) && g.len == 1 && u8_gate_end(&g) == EILSEQ);
	CHECK(!u8_gate_pass(&g, CSTR_VIEW("\x98")) && !u8_gate_pass(&g, CSTR_VIEW("\x80" "b")));
	CHECK(g.len == 6 && g.count == 3 && !u8_gate_end(&g));
	CHECK(u8_gate_pass(&g, CSTR_VIEW("c\xe2" "d")) == EILSEQ && g.len == 7 && g.count == 4);
	CHECK(u8_gate_pass(&g, CSTR_VIEW("e")) == EILSEQ && g.len == 7);
	CHECK(u8_gate_end(&g) == EILSEQ);

	/* Each byte that is no character's becomes a '?', but for a character
	 * cut short at the end while more bytes follow; NUL stays. */
	char m[] = "a\xff" "b\xe2\x82";
	CHECK(u8_mend(m, 5, false) == 3 && !memcmp(m, "a?b\xe2\x82", 5));
	CHECK(u8_mend(m, 5, true) == 5 && !memcmp(m, "a?b??", 5));
	char z[] = "\xe2(\xff\0";
	CHECK(u8_mend(z, 4, false) == 4 && !memcmp(z, "?(?\0", 4));

	/* Every string of up to four edge bytes, which u8_char() parses in a
	 * copy if they are fewer than four, else in place. */
	size_t wrong = 0;
	constexpr size_t E = sizeof edge;
	for (size_t v = 0; v < E * E * E * E; ++v) {
		unsigned char const b[4] = { edge[v % E], edge[v / E % E],
		                             edge[v / E / E % E], edge[v / E / E / E] };
		for (size_t n = 1; n <= sizeof b; ++n) {
			size_t len = 0;
			size_t const k = u8_char((struct cstr_view){ (char const *)b, n }, &c);
			uint32_t const want = reference(b, n, &len);
			wrong += want == UINT32_MAX || !want ? k != 0 : k != len || c != want;
		}
	}
	CHECK(!wrong);

	/* Strings of edge bytes: the text of each, given to a gate a few bytes
	 * at a time or all, and its mending. */
	wrong = 0;
	for (size_t i = 0; i < 50000; ++i) {
		unsigned char s[12];
		size_t const n = random_below(sizeof s + 1);
		for (size_t k = 0; k < n; ++k)
			s[k] = edge[random_below(E)];
		wrong += !mends(s, n, false) || !mends(s, n, true);
		for (size_t step = 1; step <= sizeof s; ++step)
			wrong += !gates(s, n, step);
		size_t count;
		t = u8_text((struct cstr_view){ (char const *)s, n }, &e);
		wrong += t.len != reference_text(s, n, false, &count) || t.count != count;
	}
	CHECK(!wrong);

	return check_report("u8");
}
