/** @file Unit tests for the byte buffer (src/buf.c). */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "hist.h"
#include "util.h"

/* Huge reservations must fail with ENOMEM rather than abort under ASan. */
char const *__asan_default_options (void);
char const *
__asan_default_options (void)
{
	return "allocator_may_return_null=1";
}

static unsigned failures;

#define CHECK(cond) \
	((cond) ? (void)0 \
	        : (void)(++failures, fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	                                     __FILE__, __LINE__, #cond)))

/* Append @p len bytes of @p src through a reservation of exactly @p len. */
static bool
put (struct buf *buf,
     void const *src,
     size_t      len,
     size_t      max)
{
	struct span s = buf_reserve(buf, len, max);
	if (s.len < len)
		return false;
	memcpy(s.ptr, src, len);
	buf_commit(buf, len);
	return true;
}

static bool
holds (struct buf const *buf,
       void const       *bytes,
       size_t            len)
{
	struct span s = buf_peek(buf);
	return s.len == len && (!len || !memcmp(s.ptr, bytes, len));
}

static void
test_empty (void)
{
	struct buf buf = {};
	struct span s = buf_peek(&buf);
	CHECK(!s.ptr && !s.len);
	CHECK(!buf_trim(&buf));
	buf_wipe(&buf);
	buf_free(&buf);
	CHECK(!buf.data && !buf.cap);

	/* A first reservation takes the smallest class, and the span covers
	 * what of it the maximum allows. */
	s = buf_reserve(&buf, 100, 1000);
	CHECK(s.ptr && s.len == 1000 && buf.cap == 4096);
	buf_commit(&buf, 0);
	CHECK(!buf_peek(&buf).len);
	buf_free(&buf);
}

static void
test_round_trip (void)
{
	struct buf buf = {};
	CHECK(put(&buf, "hello, world", 12, SIZE_MAX));
	CHECK(holds(&buf, "hello, world", 12));
	buf_consume(&buf, 7);
	CHECK(holds(&buf, "world", 5) && buf.head == 7);

	/* Consuming the rest rewinds without freeing. */
	unsigned char *data = buf.data;
	buf_consume(&buf, 5);
	CHECK(!buf.head && !buf.tail && buf.data == data && !buf_peek(&buf).len);

	/* The span covers all free space, and a zero reservation succeeds. */
	struct span s = buf_reserve(&buf, 1, SIZE_MAX);
	CHECK(s.ptr == buf.data && s.len == buf.cap);
	s = buf_reserve(&buf, 0, SIZE_MAX);
	CHECK(s.len == buf.cap);
	buf_free(&buf);
}

static void
test_growth (void)
{
	struct buf buf = {};
	static unsigned char block[4096];
	memset(block, 'g', sizeof block);
	CHECK(put(&buf, block, 4096, 50'000));
	CHECK(buf.cap == 4096);
	CHECK(put(&buf, block, 1, 50'000) && buf.cap == 8192);
	CHECK(put(&buf, block, 4096, 50'000) && buf.cap == 16384);
	/* Doubling stops at the class that holds the maximum. */
	struct span s = buf_reserve(&buf, 30'000, 50'000);
	CHECK(s.len == 50'000 - 8193 && buf.cap == 65536);
	buf_commit(&buf, 30'000);
	CHECK(buf_peek(&buf).len == 38'193);

	/* Past the maximum: ENOBUFS, and nothing changes. */
	unsigned char *data = buf.data;
	errno = 0;
	s = buf_reserve(&buf, 11'808, 50'000);
	CHECK(!s.ptr && !s.len && errno == ENOBUFS);
	CHECK(buf.data == data && buf.cap == 65536 && buf_peek(&buf).len == 38'193);
	s = buf_reserve(&buf, 11'807, 50'000);
	CHECK(s.len == 11'807);

	/* A maximum below the capacity also caps the span. */
	errno = 0;
	CHECK(!buf_reserve(&buf, 10, 38'199).len && errno == ENOBUFS);
	s = buf_reserve(&buf, 5, 38'199);
	CHECK(s.len == 6 && buf.cap == 65536);

	/* A smaller maximum than the readable bytes, and overflowing sums. */
	errno = 0;
	CHECK(!buf_reserve(&buf, 300, 500).len && errno == ENOBUFS);
	errno = 0;
	CHECK(!buf_reserve(&buf, SIZE_MAX, SIZE_MAX).len && errno == ENOBUFS);
	buf_free(&buf);

	/* Out of memory: ENOMEM, and nothing changes. */
	CHECK(put(&buf, "abc", 3, SIZE_MAX));
	data = buf.data;
	errno = 0;
	CHECK(!buf_reserve(&buf, SIZE_MAX / 2, SIZE_MAX).len && errno == ENOMEM);
	CHECK(buf.data == data && holds(&buf, "abc", 3));
	buf_free(&buf);
}

static void
test_compaction (void)
{
	static unsigned char bytes[4096];
	for (size_t i = 0; i < sizeof bytes; ++i)
		bytes[i] = (unsigned char)i;

	/* Less than half consumed: the buffer grows instead of moving down. */
	struct buf buf = {};
	CHECK(put(&buf, bytes, 4096, SIZE_MAX) && buf.cap == 4096);
	buf_consume(&buf, 1000);
	struct span s = buf_reserve(&buf, 500, SIZE_MAX);
	CHECK(buf.cap == 8192 && !buf.head && s.len == 8192 - 3096);
	CHECK(holds(&buf, bytes + 1000, 3096));
	buf_free(&buf);

	/* Half or more consumed: the bytes move down in place. */
	CHECK(put(&buf, bytes, 4096, SIZE_MAX));
	buf_consume(&buf, 2048);
	unsigned char *data = buf.data;
	s = buf_reserve(&buf, 1000, SIZE_MAX);
	CHECK(buf.data == data && buf.cap == 4096 && !buf.head && s.len == 2048);
	CHECK(holds(&buf, bytes + 2048, 2048));

	/* Compaction that would not make room grows instead. */
	CHECK(put(&buf, bytes, 2048, SIZE_MAX));
	buf_consume(&buf, 2048);
	s = buf_reserve(&buf, 3000, SIZE_MAX);
	CHECK(buf.cap == 8192 && !buf.head && s.len == 8192 - 2048);
	CHECK(holds(&buf, bytes, 2048));
	buf_free(&buf);

	/* At the maximum the bytes move down, however few were consumed, but
	 * only for room within the maximum. */
	CHECK(put(&buf, bytes, 4096, 4096) && buf.cap == 4096);
	buf_consume(&buf, 10);
	errno = 0;
	CHECK(!buf_reserve(&buf, 11, 4096).len && errno == ENOBUFS && buf.head == 10);
	s = buf_reserve(&buf, 10, 4096);
	CHECK(buf.cap == 4096 && !buf.head && s.len == 10);
	CHECK(holds(&buf, bytes + 10, 4086));
	buf_free(&buf);
}

static void
test_trim (void)
{
	static unsigned char bytes[64 << 10];
	memset(bytes, 't', sizeof bytes);
	struct buf buf = {};
	CHECK(put(&buf, bytes, sizeof bytes, SIZE_MAX) && buf.cap == sizeof bytes);
	buf_consume(&buf, sizeof bytes - 5000);

	/* The bytes move into the smallest class that holds them, if smaller. */
	CHECK(buf_trim(&buf) == 5000);
	CHECK(buf.cap == 8192 && !buf.head && holds(&buf, bytes, 5000));
	CHECK(!buf_trim(&buf) && buf.cap == 8192);
	buf_consume(&buf, 2000);
	CHECK(buf_trim(&buf) == 3000 && buf.cap == 4096 && holds(&buf, bytes, 3000));

	/* An empty buffer gives its storage up. */
	buf_consume(&buf, 3000);
	CHECK(!buf_trim(&buf));
	CHECK(!buf.data && !buf.cap && !buf.head && !buf.tail);
}

static void
test_cut (void)
{
	struct buf buf = {};
	CHECK(put(&buf, "..head|body|next", 16, SIZE_MAX));
	buf_consume(&buf, 2);

	/* From the middle, the bytes after move down; from the head, nothing
	 * moves; and cutting the rest rewinds. */
	buf_cut(&buf, 5, 5);
	CHECK(holds(&buf, "head|next", 9) && buf.head == 2);
	buf_cut(&buf, 9, 0);
	CHECK(holds(&buf, "head|next", 9));
	buf_cut(&buf, 0, 5);
	CHECK(holds(&buf, "next", 4) && buf.head == 7);
	buf_cut(&buf, 2, 2);
	CHECK(holds(&buf, "ne", 2));
	buf_cut(&buf, 0, 2);
	CHECK(!buf.head && !buf.tail);
	buf_free(&buf);
}

static void
test_wipe (void)
{
	struct buf buf = {};
	CHECK(put(&buf, "secret-secret", 13, SIZE_MAX));
	buf_consume(&buf, 7);
	buf_wipe(&buf);
	CHECK(buf.data && buf.cap == 4096 && !buf_peek(&buf).len);
	bool zero = true;
	for (size_t i = 0; i < buf.cap; ++i)
		zero &= !buf.data[i];
	CHECK(zero);
	buf_free(&buf);
}

/* xorshift64*: a fixed, portable sequence. */
static uint64_t
rnd (uint64_t *state)
{
	*state ^= *state >> 12;
	*state ^= *state << 25;
	*state ^= *state >> 27;
	return *state * 0x2545F4914F6CDD1DULL;
}

static uint64_t
stashed (struct buf_stash const *stash)
{
	return hist_get(&stash->metrics.retained);
}

/*
 * Random operations against a plain array model.  Besides the contents, it
 * checks the growth rules: every reservation within the maximum succeeds, the
 * capacity never exceeds the maximum, and below it a move without growth only
 * happens with half the storage consumed, so the bytes moved there never
 * exceed the bytes consumed.  At the maximum, moves are counted apart.  The
 * storage comes from a stash, which another buffer takes from and fills too,
 * and which gives storage back now and then: none may be in two places.
 */
static void
test_model (void)
{
	enum { MAX = 1 << 16, OPS = 200'000 };
	static unsigned char model[MAX];
	static unsigned char noise[MAX];
	size_t len = 0;
	uint64_t state = 0x9E3779B97F4A7C15ULL;
	uint64_t consumed = 0;
	uint64_t moved = 0;
	uint64_t forced = 0;
	struct buf buf = {};
	struct buf other = {};
	struct buf_stash stash = {};
	memset(noise, 'n', sizeof noise);
	buf_stash_attach(&stash);

	for (unsigned op = 0; op < OPS && !failures; ++op) {
		uint64_t r = rnd(&state);
		switch (r % 8) {
		case 0: case 1: case 2: {
			size_t min = 1 + (size_t)(r >> 8) % 4096;
			size_t head = buf.head;
			size_t cap = buf.cap;
			unsigned char *data = buf.data;
			struct span s = buf_reserve(&buf, min, MAX);
			if (len + min > MAX) {
				CHECK(!s.len && buf.cap == cap && buf.head == head);
				break;
			}
			CHECK(s.len >= min && buf.cap <= MAX);
			if (buf.cap == cap && head && !buf.head) {
				CHECK(buf.data == data && (head >= cap / 2 || cap == MAX));
				*(cap < MAX ? &moved : &forced) += len;
			}
			size_t n = (size_t)(r >> 32) % (s.len + 1);
			for (size_t i = 0; i < n; ++i)
				s.ptr[i] = model[len + i] = (unsigned char)(r >> (i % 8 * 8));
			buf_commit(&buf, n);
			len += n;
			break;
		}
		case 3: case 4: case 5: {
			size_t n = len ? (size_t)(r >> 8) % (len + 1) : 0;
			buf_consume(&buf, n);
			memmove(model, model + n, len - n);
			len -= n;
			consumed += n;
			break;
		}
		case 6:
			if (!(r & 0xf0))
				(void)buf_trim(&buf);
			break;
		default:
			if (!(r & 0xff0)) {
				buf_wipe(&buf);
				len = 0;
			} else if (!(r & 0x30)) {
				(void)put(&other, noise, 1 + (size_t)(r >> 16) % MAX, SIZE_MAX);
				if (r & 0x40)
					buf_free(&other);
			} else if (!(r & 0x300)) {
				buf_stash_age(&stash);
				(void)buf_stash_release(&stash, r & 0x400 ? BUF_CRITICAL
				                                           : BUF_HIGH,
				                        1 + (size_t)(r >> 16) % 4);
			}
			break;
		}
		CHECK(holds(&buf, model, len));
		CHECK(stashed(&stash) + stash.held <= 2 * stash.peak);
	}
	CHECK(moved <= consumed);
	printf("buf model: %u ops, %" PRIu64 " bytes consumed, %" PRIu64
	       " moved without growing, %" PRIu64 " at the maximum; stash: %" PRIu64
	       " hits, %" PRIu64 " misses\n", OPS, consumed, moved, forced,
	       hist_get(&stash.metrics.hits), hist_get(&stash.metrics.misses));
	buf_free(&buf);
	buf_free(&other);
	(void)buf_stash_release(&stash, BUF_CRITICAL, SIZE_MAX);
	CHECK(!stashed(&stash) && !stash.held);
	buf_stash_attach(nullptr);
}

static void
test_stash (void)
{
	static unsigned char bytes[8192];
	struct buf_stash stash = {};
	buf_stash_attach(&stash);

	/* Storage given up goes to the stash, and back out of it, last in
	 * first out. */
	struct buf a = {};
	struct buf b = {};
	CHECK(put(&a, bytes, 1, SIZE_MAX) && put(&b, bytes, 1, SIZE_MAX));
	unsigned char *data = b.data;
	buf_free(&a);
	buf_free(&b);
	CHECK(stash.n[0] == 2 && stashed(&stash) == 8192);
	CHECK(hist_get(&stash.metrics.misses) == 2 && !hist_get(&stash.metrics.hits));
	CHECK(put(&a, bytes, 1, SIZE_MAX) && a.data == data);
	CHECK(stash.n[0] == 1 && stashed(&stash) == 4096);
	CHECK(hist_get(&stash.metrics.hits) == 1);

	/* Growth gives the smaller block up, and trimming the larger. */
	CHECK(put(&a, bytes, 8192, SIZE_MAX) && a.cap == 16384);
	CHECK(stash.n[0] == 2 && stashed(&stash) == 8192);
	buf_consume(&a, 8192);
	CHECK(buf_trim(&a) == 1 && a.cap == 4096 && stash.n[2] == 1 && stash.n[0] == 1);
	buf_free(&a);
	CHECK(stash.n[0] == 2 && stashed(&stash) == 8192 + 16384);

	/* At high pressure, what went unused since the last window began goes:
	 * none before a window has passed, all that went unused after. */
	CHECK(!buf_stash_release(&stash, BUF_HIGH, 256) && stashed(&stash) == 24576);
	buf_stash_age(&stash);
	CHECK(!buf_stash_release(&stash, BUF_HIGH, 256));
	CHECK(put(&a, bytes, 1, SIZE_MAX));
	buf_free(&a);
	buf_stash_age(&stash);
	CHECK(buf_stash_release(&stash, BUF_HIGH, 256) == 2);
	CHECK(stash.n[0] == 1 && !stash.n[2] && stashed(&stash) == 4096);
	CHECK(hist_get(&stash.metrics.released) == 4096 + 16384);
	CHECK(!buf_stash_release(&stash, BUF_HIGH, 256));

	/* At critical pressure all goes, as many blocks a call as allowed. */
	struct buf many[3] = {};
	for (size_t i = 0; i < ARRAY_SIZE(many); ++i)
		CHECK(put(&many[i], bytes, 1, SIZE_MAX));
	for (size_t i = 0; i < ARRAY_SIZE(many); ++i)
		buf_free(&many[i]);
	CHECK(stash.n[0] == 3);
	CHECK(buf_stash_release(&stash, BUF_CRITICAL, 2) == 2 && stash.n[0] == 1);
	CHECK(buf_stash_release(&stash, BUF_CRITICAL, 2) == 1 && !stashed(&stash));
	CHECK(!buf_stash_release(&stash, BUF_CRITICAL, 2));
	CHECK(!stash.held && !stash.peak);

	/* A block counts a unit of release work per 128 KiB. */
	static unsigned char large[256 << 10];
	CHECK(put(&a, large, sizeof large, SIZE_MAX) && a.cap == sizeof large);
	CHECK(put(&b, bytes, 1, SIZE_MAX));
	buf_free(&a);
	buf_free(&b);
	CHECK(buf_stash_release(&stash, BUF_CRITICAL, 2) == 2 && stashed(&stash) == 4096);
	CHECK(buf_stash_release(&stash, BUF_CRITICAL, 2) == 1 && !stashed(&stash));

	/* What the stash keeps and buffers hold stays within twice their peak:
	 * a miss past it gives back the largest blocks first. */
	struct buf wave[8] = {};
	for (size_t i = 0; i < 4; ++i)
		CHECK(put(&wave[i], large, 40 << 10, SIZE_MAX) && wave[i].cap == 64 << 10);
	CHECK(stash.held == 256 << 10 && stash.peak == 256 << 10);
	for (size_t i = 0; i < 4; ++i)
		buf_free(&wave[i]);
	uint64_t released = hist_get(&stash.metrics.released);
	for (size_t i = 0; i < ARRAY_SIZE(wave); ++i)
		CHECK(put(&wave[i], large, 20 << 10, SIZE_MAX) && wave[i].cap == 32 << 10);
	for (size_t i = 0; i < ARRAY_SIZE(wave); ++i)
		buf_free(&wave[i]);
	CHECK(stash.n[4] == 4 && stash.n[3] == 8 && stashed(&stash) == 2 * stash.peak);
	CHECK(hist_get(&stash.metrics.released) == released);
	CHECK(put(&b, large, 10 << 10, SIZE_MAX) && b.cap == 16 << 10);
	CHECK(stash.n[4] == 3 && stash.n[3] == 8);
	CHECK(hist_get(&stash.metrics.released) == released + (64 << 10));
	CHECK(stashed(&stash) + stash.held <= 2 * stash.peak);
	buf_free(&b);

	/* Under critical pressure storage given up goes back at once, a trim's
	 * too, and a release of all lets the peak start again. */
	CHECK(buf_stash_release(&stash, BUF_CRITICAL, SIZE_MAX) && !stashed(&stash));
	CHECK(put(&a, large, 64 << 10, SIZE_MAX) && a.cap == 64 << 10);
	CHECK(stash.peak == 64 << 10);
	buf_consume(&a, (64 << 10) - 100);
	atomic_store(&buf_pressure, BUF_CRITICAL);
	released = hist_get(&stash.metrics.released);
	CHECK(buf_trim(&a) == 100 && a.cap == 4096 && !stashed(&stash));
	CHECK(hist_get(&stash.metrics.released) == released + (64 << 10));
	buf_free(&a);
	CHECK(!stashed(&stash) && !stash.held);
	atomic_store(&buf_pressure, BUF_EASY);

	/* A thread without a stash frees what it gives up. */
	buf_stash_attach(nullptr);
	CHECK(put(&a, bytes, 1, SIZE_MAX));
	buf_free(&a);
	CHECK(!stash.n[0] && !stashed(&stash));
}

#undef CHECK

int
main (void)
{
	test_empty();
	test_round_trip();
	test_growth();
	test_compaction();
	test_trim();
	test_cut();
	test_wipe();
	test_stash();
	test_model();
	if (failures) {
		fprintf(stderr, "buf: %u checks failed\n", failures);
		return 1;
	}
	puts("buf: PASS");
	return 0;
}
