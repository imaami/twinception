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

/** How a child is spawned. */
enum child_flag {
	CHILD_STDERR = 1u << 0,  /**< its standard error goes to its output too */
};

/**
 * A child process, from its spawn until it is reaped and its owner has let
 * go of it.  Its session's process group is killed, with SIGKILL, once its
 * output goes past the cap, at its deadline, or as its owner lets go of it
 * before it has ended; at its deadline, if it has a grace, with SIGTERM
 * first, and SIGKILL once the grace has passed too; never once it is
 * reaped, as its id may name another group then.
 */
struct child {
	struct list        hook;     /**< on its owner's list, until freed */
	struct loop_ref    proc;     /**< its pidfd, until it is reaped */
	struct loop_ref    pipe;     /**< its output's read end, until the
	                                  output ends or the child is killed */
	struct loop_step   step;
	struct loop_timer  timer;    /**< its deadline */
	struct buf         out;      /**< its output, up to the cap */
	uint64_t           grace;    /**< ns from SIGTERM to SIGKILL at its
	                                  deadline; 0: SIGKILL at once */
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
	bool               late;     /**< its deadline came, which killed it or
	                                  is about to */
};

/** What a child runs, and how. */
struct child_cmd {
	char const        *file;      /**< found through askai's PATH */
	char const *const *argv;
	char *const       *envp;
	struct span        in;        /**< its standard input; empty: /dev/null */
	size_t             max;       /**< the cap on its output, in bytes */
	uint64_t           deadline;  /**< loop_clock(); 0: none */
	uint64_t           grace;     /**< ns from SIGTERM to SIGKILL at the
	                                   deadline; 0: SIGKILL at once */
	uint32_t           flags;     /**< enum child_flag */
};

/**
 * Spawn @p cmd: its standard error /dev/null, but with CHILD_STDERR its
 * output too; its output a pipe, its signal mask empty and every signal's
 * action the default, and it inherits no other descriptor.  The steps of
 * @p loop read its output into out, up to cmd->max bytes, and kill it past
 * those, or at its deadline.  Once it is reaped and its output is all in, a
 * step calls @p done with it, which may destroy it.  It goes on @p list
 * until destroyed.  Returns the child; nullptr, with the error in @p *e:
 * EBUSY with CHILD_MAX children alive, or why it could not be spawned.
 * Spawning blocks the caller until the child has executed its file, about
 * 0.1 ms.
 */
extern struct child *
child_create (struct loop            *loop,
              struct list            *list,
              struct child_cmd const *cmd,
              void                  (*done) (struct child *c),
              void                   *owner,
              int                    *e);

/** Let go of @p *p_c, which becomes nullptr: free it, if it has ended;
 *  else kill it, stop reading its output, and free it once it is reaped. */
extern void
child_destroy (struct child **p_c);

/** Free every child on @p list as its loop stops; any still alive is killed
 *  and left to the process's exit to reap. */
extern void
child_destroy_all (struct list *list);

#endif /* TWINCEPTION_SRC_CHILD_H_ */
