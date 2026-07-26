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

struct loop_ref {
	struct list hook;
	loop_cb     *cb;
	void        *owner;
	uint32_t     events;
	int          fd;
};

struct loop {
	struct list refs;
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

extern int
loop_exec (struct loop *loop);

extern void
loop_exit (struct loop *loop);

#endif /* TWINCEPTION_SRC_LOOP_H_ */
