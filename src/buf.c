/** @file Byte buffer for sockets, TLS and pipes, and its stash (buf.h). */
#include <errno.h>
#include <stdbit.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "hist.h"

_Atomic uint32_t buf_pressure;

static thread_local struct buf_stash *mine;

/* The class whose blocks hold @p size bytes; BUF_CLASSES or more if none. */
static unsigned
class_of (size_t size)
{
	unsigned bits = size > 1 ? (unsigned)stdc_bit_width(size - 1) : 0;
	return bits > BUF_CLASS_MIN ? bits - BUF_CLASS_MIN : 0;
}

/* The size of the storage that holds @p size bytes. */
static size_t
block (size_t size)
{
	unsigned c = class_of(size);
	return c < BUF_CLASSES ? (size_t)1 << (BUF_CLASS_MIN + c) : size;
}

/* Give a block of class @p c back to the allocator: blocks of a class are
 * alike, so it counts as one that went unused, if any did. */
static void
drop (struct buf_stash *s,
      unsigned          c)
{
	size_t size = (size_t)1 << (BUF_CLASS_MIN + c);
	void *data = s->top[c];
	memcpy(&s->top[c], data, sizeof s->top[c]);
	free(data);
	--s->n[c];
	s->low[c] -= s->low[c] > 0;
	s->idle[c] -= s->idle[c] > 0;
	hist_add(&s->metrics.retained, -(uint64_t)size);
	hist_add(&s->metrics.released, size);
}

/* Storage for @p size bytes, from the stash if it has a block; *cap gets
 * the storage's size.  A miss first gives back the stash's largest blocks
 * that would keep, with what buffers hold, more than twice their peak. */
static unsigned char *
take (size_t  size,
      size_t *cap)
{
	unsigned c = class_of(size);
	struct buf_stash *s = mine;
	*cap = block(size);
	if (c >= BUF_CLASSES || !s)
		return malloc(*cap);
	unsigned char *data = s->top[c];
	if (s->n[c]) {
		memcpy(&s->top[c], data, sizeof s->top[c]);
		if (--s->n[c] < s->low[c])
			s->low[c] = s->n[c];
		hist_add(&s->metrics.hits, 1);
		hist_add(&s->metrics.retained, -(uint64_t)*cap);
	} else {
		hist_add(&s->metrics.misses, 1);
		size_t held = s->held + *cap;
		size_t room = 2 * (held > s->peak ? held : s->peak) - held;
		for (unsigned d = BUF_CLASSES; d-- && hist_get(&s->metrics.retained) > room; )
			while (s->n[d] && hist_get(&s->metrics.retained) > room)
				drop(s, d);
		if (!(data = malloc(*cap)))
			return nullptr;
	}
	if ((s->held += *cap) > s->peak)
		s->peak = s->held;
	return data;
}

/* Give up the @p cap bytes of storage at @p data: to the stash, if the
 * thread has one, they are a class's block and memory pressure is not
 * critical. */
static void
give (unsigned char *data,
      size_t         cap)
{
	unsigned c = class_of(cap);
	struct buf_stash *s = mine;
	if (!data)
		return;
	if (!s || c >= BUF_CLASSES) {
		free(data);
		return;
	}
	/* A block may come from another thread's stash. */
	s->held -= s->held < cap ? s->held : cap;
	if (atomic_load_explicit(&buf_pressure, memory_order_relaxed) >= BUF_CRITICAL) {
		free(data);
		hist_add(&s->metrics.released, cap);
		return;
	}
	memcpy(data, &s->top[c], sizeof s->top[c]);
	s->top[c] = data;
	++s->n[c];
	hist_add(&s->metrics.retained, cap);
}

/* Move the readable bytes to the start of new storage for at least @p size
 * bytes; false if out of memory. */
static bool
move (struct buf *buf,
      size_t      size)
{
	size_t cap;
	unsigned char *data = take(size, &cap);
	if (!data)
		return false;
	size_t len = buf->tail - buf->head;
	if (len)
		memcpy(data, buf->data + buf->head, len);
	give(buf->data, buf->cap);
	*buf = (struct buf){ .data = data, .tail = len, .cap = cap };
	return true;
}

struct span
buf_reserve (struct buf *buf,
             size_t      min,
             size_t      max)
{
	size_t len = buf->tail - buf->head;
	if (len > max || min > max - len) {
		errno = ENOBUFS;
		return (struct span){};
	}

	if (buf->cap - buf->tail < min) {
		/* Grow unless moving the bytes down repays itself or the storage
		 * is already as large as it may get, when they move regardless
		 * (see struct buf). */
		size_t need = len + min;
		size_t cap = buf->cap;
		if (cap < need || (buf->head < cap / 2 && cap < max)) {
			size_t size = cap > max / 2 ? max : 2 * cap;
			if (!move(buf, size > need ? size : need))
				return (struct span){};
		} else if (buf->head) {
			memmove(buf->data, buf->data + buf->head, len);
			buf->head = 0;
			buf->tail = len;
		}
	}

	size_t room = buf->cap - buf->tail;
	if (room > max - len)
		room = max - len;
	return room ? (struct span){ buf->data + buf->tail, room } : (struct span){};
}

void
buf_cut (struct buf *buf,
         size_t      off,
         size_t      len)
{
	if (!off) {
		buf_consume(buf, len);
		return;
	}
	unsigned char *at = buf->data + buf->head + off;
	memmove(at, at + len, buf->tail - buf->head - off - len);
	buf->tail -= len;
}

size_t
buf_trim (struct buf *buf)
{
	size_t len = buf->tail - buf->head;
	if (!len) {
		buf_free(buf);
		return 0;
	}
	/* Should moving fail, the larger block simply stays. */
	return block(len) < buf->cap && move(buf, len) ? len : 0;
}

void
buf_wipe (struct buf *buf)
{
	if (buf->data)
		memset_explicit(buf->data, 0, buf->cap);
	buf->head = buf->tail = 0;
}

void
buf_free (struct buf *buf)
{
	give(buf->data, buf->cap);
	*buf = (struct buf){};
}

void
buf_stash_attach (struct buf_stash *stash)
{
	mine = stash;
}

void
buf_stash_age (struct buf_stash *stash)
{
	memcpy(stash->idle, stash->low, sizeof stash->idle);
	memcpy(stash->low, stash->n, sizeof stash->low);
}

size_t
buf_stash_release (struct buf_stash  *stash,
                   enum buf_pressure  level,
                   size_t             max)
{
	size_t done = 0;
	for (unsigned c = BUF_CLASSES; c--; ) {
		size_t units = buf_units((size_t)1 << (BUF_CLASS_MIN + c));
		size_t n = level >= BUF_CRITICAL ? stash->n[c]
		         : stash->low[c] < stash->idle[c] ? stash->low[c] : stash->idle[c];
		for (; n && (!done || done + units <= max); --n, done += units)
			drop(stash, c);
		if (n)
			return done;
	}
	/* All is given back: the peak begins again. */
	if (level >= BUF_CRITICAL)
		stash->peak = stash->held;
	return done;
}
