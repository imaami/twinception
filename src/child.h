/** @file Child processes (design section 4): each spawned with a pidfd, in a
 *  session of its own, its standard output read from a pipe into a capped
 *  buffer by the steps of the loop that spawned it, and reaped through the
 *  pidfd, with no SIGCHLD. */
#ifndef TWINCEPTION_SRC_CHILD_H_
#define TWINCEPTION_SRC_CHILD_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "buf.h"
#include "list.h"
#include "loop.h"

/** Children alive at once in the process, at most. */
enum { CHILD_MAX = 16 };

/**
 * A child process, from its spawn until it is reaped and its owner has let
 * go of it.  Its session's process group is killed, with SIGKILL, once its
 * output goes past the cap, at its deadline, or as its owner lets go of it
 * before it has ended; never once it is reaped, as its id may name another
 * group then.
 */
struct child {
	struct list        hook;     /**< on its owner's list, until freed */
	struct loop_ref    proc;     /**< its pidfd, until it is reaped */
	struct loop_ref    pipe;     /**< its output's read end, until the
	                                  output ends or the child is killed */
	struct loop_step   step;
	struct loop_timer  timer;    /**< its deadline */
	struct buf         out;      /**< its output, up to the cap */
	struct loop       *loop;
	/** Called once it is reaped and its output is all in; nullptr once its
	 *  owner has let go. */
	void             (*done) (struct child *c);
	void              *owner;
	size_t             max;      /**< the cap on its output, in bytes */
	pid_t              pid;      /**< its own, its session's and its group's */
	int                code;     /**< how it ended, as waitid() says:
	                                  CLD_EXITED, CLD_KILLED or CLD_DUMPED;
	                                  0 until it is reaped, or if waitid()
	                                  could not tell */
	int                status;   /**< its exit status, or the signal that
	                                  killed it */
	uint16_t           events;   /**< what the next step is to look at */
	bool               cut;      /**< its output went past the cap, or
	                                  memory ran out for it, which killed it */
	bool               late;     /**< its deadline killed it */
};

/**
 * Spawn @p file, found through askai's PATH, with @p argv and @p envp; its
 * standard input and error are /dev/null, its output a pipe, its signal mask
 * empty and every signal's action the default, and it inherits no other
 * descriptor.  The steps of @p loop read its output into out, up to @p max
 * bytes, and kill it past those, or at @p deadline (loop_clock(); 0 for
 * none).  Once it is reaped and its output is all in, a step calls @p done
 * with it, which may let go of it.  It goes on @p list until freed.  Returns
 * 0; EBUSY with CHILD_MAX children alive; or why it could not be spawned.
 * Spawning blocks the caller until the child has executed @p file, about
 * 0.1 ms.
 */
extern int
child_spawn (struct child **p_child,
             struct loop   *loop,
             struct list   *list,
             char const    *file,
             char *const    argv[],
             char *const    envp[],
             size_t         max,
             uint64_t       deadline,
             void         (*done) (struct child *c),
             void          *owner);

/** Let go of @p c: free it, if it has ended; else kill it, stop reading its
 *  output, and free it once it is reaped. */
extern void
child_drop (struct child *c);

/** Free every child on @p list as its loop stops; any still alive is killed
 *  and left to the process's exit to reap. */
extern void
child_free_all (struct list *list);

#endif /* TWINCEPTION_SRC_CHILD_H_ */
