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
buf_class (size_t size)
{
	unsigned bits = size > 1 ? (unsigned)stdc_bit_width(size - 1) : 0;
	return bits > BUF_CLASS_MIN ? bits - BUF_CLASS_MIN : 0;
}

/* The size of the storage that holds @p size bytes. */
static size_t
buf_block (size_t size)
{
	unsigned c = buf_class(size);
	return c < BUF_CLASSES ? (size_t)1 << (BUF_CLASS_MIN + c) : size;
}

/* Give a block of class @p c back to the allocator: blocks of a class are
 * alike, so it counts as one that went unused, if any did. */
static void
buf_stash_drop (struct buf_stash *stash,
                unsigned          c)
{
	size_t size = (size_t)1 << (BUF_CLASS_MIN + c);
	void *data = stash->top[c];
	memcpy(&stash->top[c], data, sizeof stash->top[c]);
	free(data);
	--stash->n[c];
	stash->low[c] -= stash->low[c] > 0;
	stash->idle[c] -= stash->idle[c] > 0;
	hist_add(&stash->metrics.retained, -(uint64_t)size);
	hist_add(&stash->metrics.released, size);
}

/* Storage for @p size bytes, from @p stash if it has a block; *cap gets the
 * storage's size.  A miss first gives back the stash's largest blocks that
 * would keep, with what buffers hold, more than twice their peak. */
static unsigned char *
buf_stash_take (struct buf_stash *stash,
                size_t            size,
                size_t           *cap)
{
	unsigned c = buf_class(size);
	*cap = buf_block(size);
	if (c >= BUF_CLASSES || !stash)
		return malloc(*cap);
	unsigned char *data = stash->top[c];
	if (stash->n[c]) {
		memcpy(&stash->top[c], data, sizeof stash->top[c]);
		if (--stash->n[c] < stash->low[c])
			stash->low[c] = stash->n[c];
		hist_add(&stash->metrics.hits, 1);
		hist_add(&stash->metrics.retained, -(uint64_t)*cap);
	} else {
		hist_add(&stash->metrics.misses, 1);
		size_t held = stash->held + *cap;
		size_t room = 2 * (held > stash->peak ? held : stash->peak) - held;
		for (unsigned d = BUF_CLASSES; d-- && hist_get(&stash->metrics.retained) > room; )
			while (stash->n[d] && hist_get(&stash->metrics.retained) > room)
				buf_stash_drop(stash, d);
		if (!(data = malloc(*cap)))
			return nullptr;
	}
	if ((stash->held += *cap) > stash->peak)
		stash->peak = stash->held;
	return data;
}

/* Give up the @p cap bytes of storage at @p data: to @p stash, if there is
 * one, they are a class's block and memory pressure is not critical. */
static void
buf_stash_give (struct buf_stash *stash,
                unsigned char    *data,
                size_t            cap)
{
	unsigned c = buf_class(cap);
	if (!stash || c >= BUF_CLASSES) {
		free(data);
		return;
	}
	if (!data)
		return;
	/* A block may come from another thread's stash. */
	stash->held -= stash->held < cap ? stash->held : cap;
	if (atomic_load_explicit(&buf_pressure, memory_order_relaxed) >= BUF_CRITICAL) {
		free(data);
		hist_add(&stash->metrics.released, cap);
		return;
	}
	memcpy(data, &stash->top[c], sizeof stash->top[c]);
	stash->top[c] = data;
	++stash->n[c];
	hist_add(&stash->metrics.retained, cap);
}

/* Move the readable bytes to the start of new storage for at least @p size
 * bytes; 0 or ENOMEM. */
static int
buf_move (struct buf *buf,
          size_t      size)
{
	size_t cap;
	unsigned char *data = buf_stash_take(mine, size, &cap);
	if (!data)
		return ENOMEM;
	size_t len = buf->tail - buf->head;
	if (len)
		memcpy(data, buf->data + buf->head, len);
	buf_stash_give(mine, buf->data, buf->cap);
	*buf = (struct buf){ .data = data, .tail = len, .cap = cap };
	return 0;
}

int
buf_reserve (struct buf  *buf,
             size_t       min,
             size_t       max,
             struct span *room)
{
	*room = (struct span){};
	size_t len = buf->tail - buf->head;
	if (len > max || min > max - len)
		return ENOBUFS;

	if (buf->cap - buf->tail < min) {
		/* Grow unless moving the bytes down repays itself or the storage
		 * is already as large as it may get, when they move regardless
		 * (see struct buf). */
		size_t need = len + min;
		size_t cap = buf->cap;
		if (cap < need || (buf->head < cap / 2 && cap < max)) {
			size_t size = cap > max / 2 ? max : 2 * cap;
			int e = buf_move(buf, size > need ? size : need);
			if (e)
				return e;
		} else if (buf->head) {
			memmove(buf->data, buf->data + buf->head, len);
			buf->head = 0;
			buf->tail = len;
		}
	}

	size_t n = buf->cap - buf->tail;
	if (n > max - len)
		n = max - len;
	if (n)
		*room = (struct span){ buf->data + buf->tail, n };
	return 0;
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
		buf_fini(buf);
		return 0;
	}
	/* Should moving fail, the larger block simply stays. */
	return buf_block(len) < buf->cap && !buf_move(buf, len) ? len : 0;
}

void
buf_wipe (struct buf *buf)
{
	if (buf->data)
		memset_explicit(buf->data, 0, buf->cap);
	buf->head = buf->tail = 0;
}

void
buf_fini (struct buf *buf)
{
	buf_stash_give(mine, buf->data, buf->cap);
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
			buf_stash_drop(stash, c);
		if (n)
			return done;
	}
	/* All is given back: the peak begins again. */
	if (level >= BUF_CRITICAL)
		stash->peak = stash->held;
	return done;
}
