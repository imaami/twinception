/** @file Growable byte-buffer interface. */
#ifndef TWINCEPTION_SRC_BUF_H_
#define TWINCEPTION_SRC_BUF_H_

#include <stddef.h>

struct buf {
	char  *data;
	size_t len;
	size_t cap;
};

extern int
buf_reserve (struct buf *buf,
             size_t      extra);

extern int
buf_append (struct buf *buf,
            void const *data,
            size_t      len);

extern int
buf_append_char (struct buf *buf,
                 char        ch);

extern void
buf_consume (struct buf *buf,
             size_t      len);

extern char *
buf_take (struct buf *buf);

extern void
buf_reset (struct buf *buf);

extern void
buf_fini (struct buf *buf);

#endif /* TWINCEPTION_SRC_BUF_H_ */
