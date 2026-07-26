/** @file Growable byte-buffer implementation. */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"

int
buf_reserve (struct buf *buf,
             size_t      extra)
{
	if (extra > SIZE_MAX - buf->len - 1)
		return EOVERFLOW;

	size_t need = buf->len + extra + 1;
	if (need <= buf->cap)
		return 0;

	size_t cap = buf->cap ? buf->cap : 256;
	while (cap < need) {
		if (cap > SIZE_MAX / 2) {
			cap = need;
			break;
		}
		cap *= 2;
	}

	char *data = realloc(buf->data, cap);
	if (!data)
		return errno ? errno : ENOMEM;

	buf->data = data;
	buf->cap = cap;
	if (!buf->len)
		buf->data[0] = '\0';
	return 0;
}

int
buf_append (struct buf *buf,
            void const *data,
            size_t      len)
{
	if (!len)
		return 0;
	if (!data)
		return EFAULT;

	int e = buf_reserve(buf, len);
	if (e)
		return e;

	memcpy(buf->data + buf->len, data, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
	return 0;
}

int
buf_append_char (struct buf *buf,
                 char        ch)
{
	return buf_append(buf, &ch, 1);
}

void
buf_consume (struct buf *buf,
             size_t      len)
{
	if (len >= buf->len) {
		buf_reset(buf);
		return;
	}

	buf->len -= len;
	memmove(buf->data, buf->data + len, buf->len);
	buf->data[buf->len] = '\0';
}

char *
buf_take (struct buf *buf)
{
	char *ret = buf->data;
	*buf = (struct buf){0};
	return ret;
}

void
buf_reset (struct buf *buf)
{
	buf->len = 0;
	if (buf->data)
		buf->data[0] = '\0';
}

void
buf_fini (struct buf *buf)
{
	free(buf->data);
	*buf = (struct buf){0};
}
