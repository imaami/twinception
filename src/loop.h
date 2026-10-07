/** @file Small epoll dispatcher interface. */
#ifndef TWINCEPTION_SRC_LOOP_H_
#define TWINCEPTION_SRC_LOOP_H_

#include <stdint.h>

#include "list.h"

struct loop;
struct loop_ref;

typedef int loop_cb (struct loop     *loop,
                     struct loop_ref *ref,
                     uint32_t         events);

typedef void loop_release_cb (struct loop_ref *ref);

struct loop_ref {
	struct list      hook;
	loop_cb         *cb;
	loop_release_cb *release;
	void            *owner;
	uint32_t         events;
	int              fd;
};

struct loop {
	struct list refs;
	struct list dead;
	int         epfd;
	int         error;
	int         exit;
};

extern int
loop_init (struct loop *loop);

extern void
loop_fini (struct loop *loop);

extern int
loop_watch (struct loop     *loop,
            struct loop_ref *ref,
            int              fd,
            uint32_t         events,
            loop_cb         *cb,
            void            *owner);

extern int
loop_modify (struct loop     *loop,
             struct loop_ref *ref,
             uint32_t         events);

extern void
loop_unwatch (struct loop     *loop,
              struct loop_ref *ref);

/**
 * Unwatch @p ref and call @p release on it once the current batch of events
 * has been dispatched.  Events already fetched for @p ref are skipped, so its
 * owner may be freed by @p release without racing the dispatcher.
 */
extern void
loop_release (struct loop     *loop,
              struct loop_ref *ref,
              loop_release_cb *release);

extern int
loop_exec (struct loop *loop);

extern void
loop_exit (struct loop *loop);

#endif /* TWINCEPTION_SRC_LOOP_H_ */
