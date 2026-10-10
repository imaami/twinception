/** @file Unit tests for the byte buffer (src/buf.c). */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"

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
	buf_trim(&buf, 0);
	buf_wipe(&buf);
	buf_free(&buf);
	CHECK(!buf.data && !buf.cap);

	/* A first reservation allocates exactly what it asks for. */
	s = buf_reserve(&buf, 100, 1000);
	CHECK(s.ptr && s.len == 100 && buf.cap == 100);
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
	unsigned char block[64];
	memset(block, 'g', sizeof block);
	CHECK(put(&buf, block, 64, 1000));
	CHECK(buf.cap == 64);
	CHECK(put(&buf, block, 1, 1000) && buf.cap == 128);
	CHECK(put(&buf, block, 64, 1000) && buf.cap == 256);
	/* Doubling stops at the maximum. */
	struct span s = buf_reserve(&buf, 600, 1000);
	CHECK(s.len >= 600 && buf.cap == 1000);
	buf_commit(&buf, 600);
	CHECK(buf_peek(&buf).len == 729);

	/* Past the maximum: ENOBUFS, and nothing changes. */
	unsigned char *data = buf.data;
	errno = 0;
	s = buf_reserve(&buf, 272, 1000);
	CHECK(!s.ptr && !s.len && errno == ENOBUFS);
	CHECK(buf.data == data && buf.cap == 1000 && buf_peek(&buf).len == 729);
	s = buf_reserve(&buf, 271, 1000);
	CHECK(s.len == 271);

	/* A maximum below the capacity also caps the span. */
	errno = 0;
	CHECK(!buf_reserve(&buf, 10, 735).len && errno == ENOBUFS);
	s = buf_reserve(&buf, 5, 735);
	CHECK(s.len == 6 && buf.cap == 1000);

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
	unsigned char bytes[256];
	for (size_t i = 0; i < sizeof bytes; ++i)
		bytes[i] = (unsigned char)i;

	/* Less than half consumed: the buffer grows instead of moving down. */
	struct buf buf = {};
	CHECK(put(&buf, bytes, 256, SIZE_MAX) && buf.cap == 256);
	buf_consume(&buf, 100);
	struct span s = buf_reserve(&buf, 50, SIZE_MAX);
	CHECK(buf.cap == 512 && !buf.head && s.len == 512 - 156);
	CHECK(holds(&buf, bytes + 100, 156));
	buf_free(&buf);

	/* Half or more consumed: the bytes move down in place. */
	CHECK(put(&buf, bytes, 256, SIZE_MAX));
	buf_consume(&buf, 128);
	unsigned char *data = buf.data;
	s = buf_reserve(&buf, 100, SIZE_MAX);
	CHECK(buf.data == data && buf.cap == 256 && !buf.head && s.len == 128);
	CHECK(holds(&buf, bytes + 128, 128));

	/* Compaction that would not make room grows instead. */
	buf_consume(&buf, 1);
	CHECK(put(&buf, bytes, 1, SIZE_MAX));
	buf_consume(&buf, 127);
	s = buf_reserve(&buf, 256, SIZE_MAX);
	CHECK(buf.cap == 512 && !buf.head && s.len == 511);
	CHECK(holds(&buf, bytes, 1));
	buf_free(&buf);

	/* At the maximum the bytes move down, however few were consumed, but
	 * only for room within the maximum. */
	CHECK(put(&buf, bytes, 200, 200) && buf.cap == 200);
	buf_consume(&buf, 10);
	errno = 0;
	CHECK(!buf_reserve(&buf, 11, 200).len && errno == ENOBUFS && buf.head == 10);
	s = buf_reserve(&buf, 10, 200);
	CHECK(buf.cap == 200 && !buf.head && s.len == 10);
	CHECK(holds(&buf, bytes + 10, 190));
	buf_free(&buf);
}

static void
test_trim (void)
{
	unsigned char bytes[4096];
	memset(bytes, 't', sizeof bytes);
	struct buf buf = {};
	CHECK(put(&buf, bytes, sizeof bytes, SIZE_MAX));
	buf_consume(&buf, 4000);

	/* No shrinking below the keep size, nor above the capacity. */
	buf_trim(&buf, 8192);
	CHECK(buf.cap == 4096 && holds(&buf, bytes, 96));
	buf_trim(&buf, 1024);
	CHECK(buf.cap == 1024 && !buf.head && holds(&buf, bytes, 96));
	buf_trim(&buf, 0);
	CHECK(buf.cap == 96 && holds(&buf, bytes, 96));

	/* An empty buffer is freed, whatever the keep size. */
	buf_consume(&buf, 96);
	buf_trim(&buf, 1 << 20);
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
	CHECK(buf.data && buf.cap == 13 && !buf_peek(&buf).len);
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

/*
 * Random operations against a plain array model.  Besides the contents, it
 * checks the growth rules: every reservation within the maximum succeeds, the
 * capacity never exceeds the maximum, and below it a move without growth only
 * happens with half the storage consumed, so the bytes moved there never
 * exceed the bytes consumed.  At the maximum, moves are counted apart.
 */
static void
test_model (void)
{
	enum { MAX = 1 << 16, OPS = 200'000 };
	static unsigned char model[MAX];
	size_t len = 0;
	uint64_t state = 0x9E3779B97F4A7C15ULL;
	uint64_t consumed = 0;
	uint64_t moved = 0;
	uint64_t forced = 0;
	struct buf buf = {};

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
				buf_trim(&buf, (size_t)(r >> 8) % 8192);
			break;
		default:
			if (!(r & 0xff0)) {
				buf_wipe(&buf);
				len = 0;
			}
			break;
		}
		CHECK(holds(&buf, model, len));
	}
	CHECK(moved <= consumed);
	printf("buf model: %u ops, %" PRIu64 " bytes consumed, %" PRIu64
	       " moved without growing, %" PRIu64 " at the maximum\n", OPS,
	       consumed, moved, forced);
	buf_free(&buf);
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
	test_model();
	if (failures) {
		fprintf(stderr, "buf: %u checks failed\n", failures);
		return 1;
	}
	puts("buf: PASS");
	return 0;
}
