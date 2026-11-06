/** @file sse.h
 *
 * Incremental Server-Sent Events interface.
 */
#ifndef ASKAI_SRC_SSE_H_
#define ASKAI_SRC_SSE_H_

#include <stddef.h>

#include "dstr.h"

/** The event callback of a parser. It is called for each event that
 *  contains at least one data line, with the @p len bytes of the event
 *  data at @p data. The bytes are not NUL-terminated. A nonzero return
 *  value is an error, and sse_feed() returns it.
 */
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

extern int
sse_feed (struct sse *sse,
          void const *data,
          size_t      len);

extern void
sse_fini (struct sse *sse);

#endif /* ASKAI_SRC_SSE_H_ */
