/** @file Incremental Server-Sent Events interface. */
#ifndef TWINCEPTION_SRC_SSE_H_
#define TWINCEPTION_SRC_SSE_H_

#include <stddef.h>

#include "dstr.h"

typedef int sse_event_cb (char const *data,
                          size_t      len,
                          void       *user);

struct sse {
	struct dstr   input;
	struct dstr   data;
	sse_event_cb *event;
	void         *user;
};

/**
 * RAII-style initializer: an empty parser that reports events to @p event.
 * Assigning it over a parser that holds input leaks; sse_fini() it first.
 */
extern struct sse
sse (sse_event_cb *event,
     void         *user);

/** Drop what @p sse holds of a stream, keeping its buffers, to parse
 *  another. */
extern void
sse_reset (struct sse *sse);

extern int
sse_feed (struct sse *sse,
          void const *data,
          size_t      len);

/** The stream has ended: its last line, unterminated, and its last event
 *  are dispatched.  Returns 0, ENOMEM, or the event callback's error. */
extern int
sse_end (struct sse *sse);

extern void
sse_fini (struct sse *sse);

#endif /* TWINCEPTION_SRC_SSE_H_ */
