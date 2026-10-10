/** @file Byte buffer for sockets, TLS and pipes. */
#ifndef TWINCEPTION_SRC_BUF_H_
#define TWINCEPTION_SRC_BUF_H_

#include <stddef.h>

/**
 * Binary byte buffer for sockets, TLS and pipes; [head, tail) is readable.
 *
 * A zeroed buffer is empty and owns no storage.  Writers reserve free space
 * at the tail, fill it (recv() straight into it, say) and commit what they
 * wrote; readers peek at the readable bytes and consume them from the head.
 * Consuming the last readable byte rewinds the buffer.  Below the maximum,
 * the readable bytes move down only when a reservation finds too little room
 * at the tail while at least half the storage lies consumed before the head,
 * so each consumed byte pays for at most one moved byte: amortized O(1).  At
 * the maximum, where the storage cannot grow, they move whenever the tail is
 * short, however few bytes were consumed, so that a reservation fails only
 * for want of room, and a parser holding part of a frame of up to max - min
 * bytes can always read the rest.  Each such move costs up to max - min bytes
 * and frees at least min, so a buffer that runs full wants a generous min.
 * Text belongs in a dstr; this is for bytes.
 */
struct buf {
	unsigned char *data;
	size_t         head;
	size_t         tail;
	size_t         cap;
};

/** A run of bytes in a buffer, valid until the buffer next changes. */
struct span {
	unsigned char *ptr;
	size_t         len;
};

/**
 * At least @p min free bytes at the tail, doubling up to @p max; empty on ENOBUFS.
 *
 * The span covers the free bytes at the tail, which can be more than @p min,
 * but never more than would take the readable bytes past @p max.  It is empty,
 * and the buffer unchanged, when the readable bytes and @p min together exceed
 * @p max (errno ENOBUFS, the backpressure signal) or memory runs out (ENOMEM).
 * A reservation may move the readable bytes: pointers into them do not survive
 * it, offsets from the head do.
 */
extern struct span
buf_reserve (struct buf *buf,
             size_t      min,
             size_t      max);

/** Make @p len bytes written into the last reservation readable. */
static inline void
buf_commit (struct buf *buf,
            size_t      len)
{
	buf->tail += len;
}

/** The readable bytes. */
static inline struct span
buf_peek (struct buf const *buf)
{
	return buf->head < buf->tail
	       ? (struct span){ buf->data + buf->head, buf->tail - buf->head }
	       : (struct span){};
}

/** Drop @p len readable bytes from the head, rewinding once none are left. */
static inline void
buf_consume (struct buf *buf,
             size_t      len)
{
	buf->head += len;
	if (buf->head == buf->tail)
		buf->head = buf->tail = 0;
}

/**
 * Drop @p len readable bytes that begin @p off bytes after the head, moving
 * the readable bytes after them down: it costs as many bytes as follow them.
 */
extern void
buf_cut (struct buf *buf,
         size_t      off,
         size_t      len);

/** Shrink the storage to the readable bytes or @p keep, if larger; free it when empty. */
extern void
buf_trim (struct buf *buf,
          size_t      keep);

/**
 * Zero all storage, consumed bytes included, and empty the buffer: for secrets
 * only.  Copies that growth left in freed memory are beyond its reach, so
 * reserve a secret's full size before writing it.
 */
extern void
buf_wipe (struct buf *buf);

/** Free the storage, leaving an empty buffer. */
extern void
buf_free (struct buf *buf);

#endif /* TWINCEPTION_SRC_BUF_H_ */
