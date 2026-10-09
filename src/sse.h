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

extern int
sse_feed (struct sse *sse,
          void const *data,
          size_t      len);

extern int
sse_finish (struct sse *sse);

extern void
sse_fini (struct sse *sse);

#endif /* TWINCEPTION_SRC_SSE_H_ */
