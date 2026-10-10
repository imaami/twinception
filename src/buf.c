/** @file Byte buffer for sockets, TLS and pipes. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"

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
			if (!cap) {
				cap = need;
			} else {
				do cap = cap > max / 2 ? max : 2 * cap;
				while (cap < need);
			}
			unsigned char *data = realloc(buf->data, cap);
			if (!data)
				return (struct span){};
			buf->data = data;
			buf->cap = cap;
		}
		if (buf->head) {
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

void
buf_trim (struct buf *buf,
          size_t      keep)
{
	size_t len = buf->tail - buf->head;
	if (!len) {
		buf_free(buf);
		return;
	}
	size_t cap = len > keep ? len : keep;
	if (cap >= buf->cap)
		return;
	if (buf->head) {
		memmove(buf->data, buf->data + buf->head, len);
		buf->head = 0;
		buf->tail = len;
	}
	/* Should shrinking fail, the larger block simply stays. */
	unsigned char *data = realloc(buf->data, cap);
	if (data) {
		buf->data = data;
		buf->cap = cap;
	}
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
	free(buf->data);
	*buf = (struct buf){};
}
