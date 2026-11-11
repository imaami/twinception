/** @file buf.h
 *
 * Byte buffer for sockets, TLS and pipes.
 */
#ifndef ASKAI_SRC_BUF_H_
#define ASKAI_SRC_BUF_H_

#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * Binary byte buffer for sockets, TLS and pipes; [head, tail) is readable.
 *
 * A zeroed buffer is empty and owns no storage. Writers reserve free space
 * at the tail, fill it (recv() straight into it, say) and commit what they
 * wrote; readers peek at the readable bytes and consume them from the head.
 * Consuming the last readable byte rewinds the buffer. Below the maximum,
 * the readable bytes move down only when a reservation finds too little room
 * at the tail while at least half the storage lies consumed before the head,
 * so each consumed byte pays for at most one moved byte: amortized O(1). At
 * the maximum, where the storage cannot grow, they move whenever the tail is
 * short, however few bytes were consumed, so that a reservation fails only
 * for want of room, and a parser holding part of a frame of up to max - min
 * bytes can always read the rest. Each such move costs up to max - min bytes
 * and frees at least min, so a buffer that runs full wants a generous min.
 * Storage is a block of a size class, taken from the thread's stash when it
 * has one (struct buf_stash), and given back there.
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

/** A run of bytes in a buffer or a text by offset. Unlike a span, an
 *  extent stays valid when the storage of the bytes moves.
 */
struct extent {
	size_t at;   /**< offset of the first byte */
	size_t len;  /**< bytes of the run */
};

/** Size classes of storage: powers of two from 4 KiB to 32 MiB, glibc's
 *  largest mmap threshold. Larger storage is allocated and freed as is.
 */
enum {
	BUF_CLASS_MIN = 12,  /**< log2 of the smallest class */
	BUF_CLASSES   = 14,
	BUF_UNIT      = 17,  /**< log2 of the bytes per unit of release work: a
	                          mapped block costs as many pages to give back */
};

/**
 * Memory pressure levels (design section 5). Without pressure, storage
 * stays in the stashes for reuse; under it, the threads give back first what
 * went unused, then all they can.
 */
enum buf_pressure {
	BUF_EASY,      /**< none */
	BUF_HIGH,      /**< give back what went unused for a window */
	BUF_CRITICAL,  /**< give back all that is not in use */
};

/** The process's memory pressure, an enum buf_pressure: one thread, the
 *  brainstem, sets it, and any thread may read it.
 */
extern _Atomic uint32_t buf_pressure;

/** A count of objects or blocks kept for reuse, and the fewest kept through
 *  windows of reuse: those went unused. Zeroed, it counts none.
 */
struct buf_kept {
	size_t n;
	size_t low;   /**< fewest kept in this window */
	size_t idle;  /**< fewest kept in the window before */
};

/** Count one taken off @p kept for reuse. */
static inline void
buf_kept_take (struct buf_kept *kept)
{
	if (--kept->n < kept->low)
		kept->low = kept->n;
}

/** Count one taken off @p kept to free it: it counts as one that went
 *  unused, if any did.
 */
static inline void
buf_kept_drop (struct buf_kept *kept)
{
	--kept->n;
	kept->low -= kept->low > 0;
	kept->idle -= kept->idle > 0;
}

/** How many of @p kept a relief at @p level frees: at BUF_HIGH those unused
 *  through the window before and this one so far, at BUF_CRITICAL all.
 */
static inline size_t
buf_kept_unused (struct buf_kept const *kept,
                 enum buf_pressure      level)
{
	return level >= BUF_CRITICAL ? kept->n
	     : kept->low < kept->idle ? kept->low : kept->idle;
}

/** Begin a window of reuse of @p kept. */
static inline void
buf_kept_age (struct buf_kept *kept)
{
	kept->idle = kept->low;
	kept->low = kept->n;
}

/** What a stash keeps and has done: its thread writes, any reads (hist.h). */
struct buf_stash_metrics {
	_Atomic uint64_t retained;  /**< bytes kept for reuse */
	_Atomic uint64_t hits;      /**< storage taken from the stash */
	_Atomic uint64_t misses;    /**< storage it lacked, which malloc() gave */
	_Atomic uint64_t released;  /**< bytes given back to the allocator */
	_Atomic uint64_t reliefs;   /**< pressure reliefs its thread began */
	_Atomic uint64_t trims;     /**< allocator trims it ran, as the last
	                                 thread of a relief, for them all */
	_Atomic uint64_t trim_ns;   /**< the time those took */
};

/**
 * A thread's store of buffer storage, for reuse without allocator calls
 * (design section 5). Storage a buffer gives up goes to the stash of the
 * thread that gives it up, and storage a buffer needs comes from there first:
 * a thread keeps its working space. Each class is a LIFO list threaded
 * through its blocks. What it keeps and its buffers hold stays within twice
 * the most they held at once: a miss gives back its largest blocks first
 * where it would not. Otherwise storage goes back to the allocator only
 * under memory pressure: at once while it is critical, and as the thread
 * calls buf_stash_release().
 */
struct buf_stash {
	struct buf_stash_metrics metrics;
	void                    *top[BUF_CLASSES];   /**< the last block given */
	struct buf_kept          kept[BUF_CLASSES];  /**< blocks kept */
	size_t                   held;               /**< bytes of blocks that
	                                                  buffers hold */
	size_t                   peak;               /**< the most held at once
	                                                  since all was released */
};

/**
 * Make at least @p min bytes free at the tail, doubling up to @p max, and
 * give them in @p room. Returns 0; ENOBUFS, the backpressure signal, when
 * the readable bytes and @p min together exceed @p max; or ENOMEM.
 *
 * @p room covers the free bytes at the tail, which can be more than @p min,
 * but never more than would take the readable bytes past @p max. On an
 * error it is empty, and the buffer unchanged. A reservation may move the
 * readable bytes: pointers into them do not survive it, offsets from the
 * head do. Storage grows to the class that holds the double, or what is
 * needed, but not past the class that holds @p max.
 */
extern int
buf_reserve (struct buf  *buf,
             size_t       min,
             size_t       max,
             struct span *room);

/** Append text, formatted as by vprintf(), growing the buffer as it needs.
 *  Returns 0, buf_reserve()'s error, or EOVERFLOW if vsnprintf() cannot
 *  format it.
 */
[[gnu::format(printf, 2, 0)]]
extern int
buf_vprintf (struct buf *buf,
             char const *fmt,
             va_list     ap);

/** As buf_vprintf(), with the format's arguments. */
[[gnu::format(printf, 2, 3)]]
extern int
buf_printf (struct buf *buf,
            char const *fmt,
            ...);

/**
 * Read from @p fd, from where its file stands to its end, into @p buf,
 * which may then hold at most @p max readable bytes. The first reservation
 * is for @p size bytes and one more, so that a file of the @p size that
 * fstat() gave is read into storage that does not grow and leaves no copy.
 * Returns 0; ENOBUFS if the file goes on past @p max, at once if @p size
 * does; or the error of buf_reserve() or read(). On an error, the bytes
 * read stay in @p buf.
 */
extern int
buf_read (struct buf *buf,
          int         fd,
          size_t      size,
          size_t      max);

/** Make @p len bytes written into the last reservation readable. */
static inline void
buf_commit (struct buf *buf,
            size_t      len)
{
	buf->tail += len;
}

/** Append the @p len bytes at @p data, reserving room for them with
 *  buf_reserve() up to @p max. Returns 0, also for @p len 0, or
 *  buf_reserve()'s error.
 */
static inline int
buf_put (struct buf *buf,
         void const *data,
         size_t      len,
         size_t      max)
{
	if (!len)
		return 0;
	struct span s;
	int e = buf_reserve(buf, len, max, &s);
	if (!e) {
		memcpy(s.ptr, data, len);
		buf_commit(buf, len);
	}
	return e;
}

/** The readable bytes. */
static inline struct span
buf_peek (struct buf const *buf)
{
	return buf->head < buf->tail
	       ? (struct span){ buf->data + buf->head, buf->tail - buf->head }
	       : (struct span){};
}

/** Drop all readable bytes, rewinding the buffer. */
static inline void
buf_clear (struct buf *buf)
{
	buf->head = buf->tail = 0;
}

/** Drop @p len readable bytes from the head, rewinding once none are left. */
static inline void
buf_consume (struct buf *buf,
             size_t      len)
{
	buf->head += len;
	if (buf->head == buf->tail)
		buf_clear(buf);
}

/**
 * Drop @p len readable bytes that begin @p off bytes after the head, moving
 * the readable bytes after them down: it costs as many bytes as follow them.
 */
extern void
buf_cut (struct buf *buf,
         size_t      off,
         size_t      len);

/**
 * Move the readable bytes into the smallest class that holds them, if that is
 * smaller, or give the storage up if there are none; returns the bytes moved.
 */
extern size_t
buf_trim (struct buf *buf);

/**
 * Zero all storage, consumed bytes included, and empty the buffer: for secrets
 * only. Copies that growth left in freed memory are beyond its reach, so
 * reserve a secret's full size before writing it.
 */
extern void
buf_wipe (struct buf *buf);

/** Give the storage up, to the stash if the thread has one, leaving an
 *  empty buffer.
 */
extern void
buf_fini (struct buf *buf);

/** Make @p stash, in zeroed storage just allocated, valid: its atomic
 *  members are initialized in place (src/hist.h).
 */
static inline void
buf_stash_init (struct buf_stash *stash)
{
	struct buf_stash_metrics *m = &stash->metrics;
	atomic_init(&m->retained, 0);
	atomic_init(&m->hits, 0);
	atomic_init(&m->misses, 0);
	atomic_init(&m->released, 0);
	atomic_init(&m->reliefs, 0);
	atomic_init(&m->trims, 0);
	atomic_init(&m->trim_ns, 0);
}

/** Make @p stash, zeroed to begin, the calling thread's; nullptr leaves the
 *  thread with none.
 */
extern void
buf_stash_attach (struct buf_stash *stash);

/** Begin a window of reuse, which the thread does every so often: at
 *  BUF_HIGH, buf_stash_release() gives back the blocks unused through the
 *  last window and this one so far.
 */
extern void
buf_stash_age (struct buf_stash *stash);

/** The units of release work that giving back @p bytes counts: one per
 *  2^BUF_UNIT bytes, and at least one.
 */
static inline size_t
buf_units (size_t bytes)
{
	return bytes >> BUF_UNIT ? bytes >> BUF_UNIT : 1;
}

/**
 * Give the stash's blocks back to the allocator, the largest first, up to
 * @p max units of release work (buf_units()), or one block if that is more:
 * at BUF_HIGH those unused since the last window began, at BUF_CRITICAL all
 * of them. Returns the units; 0 once none are left to give back.
 */
extern size_t
buf_stash_release (struct buf_stash  *stash,
                   enum buf_pressure  level,
                   size_t             max);

#endif /* ASKAI_SRC_BUF_H_ */
