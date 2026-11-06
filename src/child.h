/** @file child.h
 *
 * Child processes (design section 4). Each child is spawned with a
 * pidfd, in a session of its own. The steps of the loop that spawned it
 * read its standard output from a pipe into a capped buffer. The pidfd
 * becomes readable when the child exits, so askai does not use SIGCHLD.
 * The child is reaped by its pid after its output has also ended.
 * child_run() instead runs a child on the calling thread, its output in a
 * file, and reaps it through its pidfd.
 */
#ifndef TWINCEPTION_SRC_CHILD_H_
#define TWINCEPTION_SRC_CHILD_H_

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "buf.h"
#include "cstr_view.h"
#include "list.h"
#include "loop.h"

/** The maximum number of children of loops (child_create()) in the
 *  process that are not reaped.
 */
enum { CHILD_MAX = 16 };

/** How a child is spawned. */
enum child_flag {
	CHILD_STDERR = 1u << 0,  /**< its standard error goes to its output too */
};

/** The bits of child.flags. Only child.c uses the bits of CHILD_EVENTS:
 *  what the next step of the child is to look at.
 */
enum child_state : uint32_t {
	CHILD_CUT    = 1u << 0,  /**< its output went past the cap, or memory
	                              ran out for it, which killed it */
	CHILD_LATE   = 1u << 1,  /**< its deadline came, which killed it or is
	                              about to */
	CHILD_OUT    = 1u << 2,  /**< its output can be read, or has ended */
	CHILD_EXIT   = 1u << 3,  /**< it has exited */
	CHILD_DUE    = 1u << 4,  /**< its deadline has come */
	CHILD_EVENTS = CHILD_OUT | CHILD_EXIT | CHILD_DUE,
};

/**
 * A child process, from its spawn until it is reaped and its owner has let
 * go of it. Its session's process group is killed, with SIGKILL, once its
 * output goes past the cap, at its deadline, or as its owner lets go of it
 * before it has ended. A child with a grace gets SIGTERM instead, which
 * lets it clean up, and at its deadline SIGKILL once the grace has passed
 * too. No signal goes to the group once the child is reaped, as its id
 * may name another group then.
 *
 * The child is reaped only after it has exited and its output has ended.
 * Between the exit and the reap, the child is a zombie. The kernel does
 * not reuse the id of a zombie, and the id of the child is also the id of
 * its process group. So a kill also reaches a process of the group that
 * has the output pipe open after the child has exited.
 */
struct child {
	struct list        hook;     /**< on its owner's list, until freed */
	struct loop_ref    proc;     /**< the pidfd, until the child exits */
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
	pid_t              pid;      /**< the id of the child, its session and
	                                  its process group, from the watch of
	                                  the pidfd until the reap, and 0 at
	                                  other times */
	int                code;     /**< CLD_EXITED, CLD_KILLED or CLD_DUMPED
	                                  from waitid() after the child exits,
	                                  and 0 before that or if waitid()
	                                  failed */
	int                status;   /**< its exit status, or the signal that
	                                  killed it */
	uint32_t           flags;    /**< enum child_state */
};

/** Returns the exit status of @p c if it exited before its deadline and its
 *  output was not cut, else -1.
 */
static inline int
child_status (struct child const *c)
{
	return c->flags & (CHILD_CUT | CHILD_LATE) || c->code != CLD_EXITED ? -1 : c->status;
}

/** Returns true if @p c exited with status 0 before its deadline and its
 *  output was not cut, and false if @p c is nullptr.
 */
static inline bool
child_ok (struct child const *c)
{
	return c && !child_status(c);
}

/**
 * Writes why @p c, a child that ran @p name, failed into the @p size bytes
 * at @p buf: it took longer than its @p timeout, in ns, a signal killed it,
 * it exited with a status other than 0, or the reason is unknown. Returns
 * a view of the text, or an empty view if @p c exited with status 0 before
 * its deadline. A cut also kills the child, so the caller tests CHILD_CUT
 * first.
 */
extern struct cstr_view
child_why (struct child const *c,
           char const         *name,
           uint64_t            timeout,
           char               *buf,
           size_t              size);

/**
 * Writes why child_create() could not spawn @p name, with the error @p e,
 * into the @p size bytes at @p buf: "Cannot run NAME: ERROR.", which also
 * goes to the log as a warning. Returns a view of the text, or for EBUSY a
 * view of a static text that asks to try again.
 */
extern struct cstr_view
child_spawn_why (int         e,
                 char const *name,
                 char       *buf,
                 size_t      size);

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
	uint64_t           flags;     /**< enum child_flag, widened over the
	                                   padding */
};

/** The descriptors of a child that child_run() runs, each -1 if it has
 *  none: its input, else cmd->in or /dev/null; its output, else /dev/null;
 *  the directory it runs in, else the caller's; and a descriptor that it
 *  keeps as descriptor 3, such as a file's lock.
 */
struct child_fds {
	int in;
	int out;
	int dir;
	int keep;
};

/** Returns a struct child_fds whose descriptors are all -1. */
static inline struct child_fds
child_fds (void)
{
	return (struct child_fds){ .in = -1, .out = -1, .dir = -1, .keep = -1 };
}

/**
 * Spawn @p cmd: its standard error /dev/null, but with CHILD_STDERR its
 * output too; its output a pipe, its signal mask empty and every signal's
 * action the default, and it inherits no other descriptor. The steps of
 * @p loop read its output into out, up to cmd->max bytes, and kill it past
 * those, or at its deadline. Once it is reaped and its output is all in, a
 * step calls @p done with it, which may destroy it. It goes on @p list
 * until destroyed. The child counts toward CHILD_MAX until it is reaped.
 * Returns the child, or nullptr with the error in @p *e. The error is
 * EBUSY if there are already CHILD_MAX children that are not reaped, or
 * else the error that stopped the spawn.
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

/**
 * Run @p cmd to its end on the calling thread, which no loop needs: spawned
 * as child_create() spawns it, but with the descriptors @p fds, its output
 * into a file such as a memfd, uncapped and unread. If cmd->deadline, a
 * time of loop_clock(), is set, at that time it gets SIGTERM, and SIGKILL
 * once its grace has passed too, or SIGKILL at once without a grace, and the
 * run fails with ETIMEDOUT. It does not count toward CHILD_MAX. Returns 0
 * and its exit status in @p *status, EIO if a signal killed it, or the error
 * that stopped the spawn or the wait.
 */
extern int
child_run (struct child_cmd const *cmd,
           struct child_fds        fds,
           int                    *status);

/**
 * Run @p cmd as child_run() does, its output into a file in memory in place
 * of fds.out, and read the output into the @p size bytes at @p buf, its
 * length into @p *len. Returns 0 and the exit status in @p *status, EFBIG
 * if the output fills @p size bytes, or the error of the run or the read.
 */
extern int
child_capture (struct child_cmd const *cmd,
               struct child_fds        fds,
               char                   *buf,
               size_t                  size,
               size_t                 *len,
               int                    *status);

/** Write the @p n words at @p prefix, then the words at @p words up to its
 *  nullptr, and a nullptr to the @p cap pointers at @p argv. Returns 0, or
 *  E2BIG if they do not fit.
 */
extern int
child_argv (char const        **argv,
            size_t              cap,
            char const *const  *prefix,
            size_t              n,
            char const *const  *words);

/** Let go of @p *p_c, which becomes nullptr: free it, if it has ended;
 *  else kill it, stop reading its output, and free it once it is reaped.
 */
extern void
child_destroy (struct child **p_c);

/** Lets go of @p *p_c, which becomes nullptr, without killing it. The
 *  child runs until it ends, its output reaches the cap, or its deadline
 *  comes, and its loop frees it then.
 */
extern void
child_release (struct child **p_c);

/** Free every child on @p list as its loop stops; any still alive is killed
 *  and left to the process's exit to reap.
 */
extern void
child_destroy_all (struct list *list);

/** Open /dev/null on each of the descriptors 0 to 2 that is closed, for the
 *  other direction than its use, so that a read of 0 or a write of 1 or 2
 *  still fails. A program that starts children calls this first: the file
 *  actions of a child replace its descriptors 0 to 2 before they copy the
 *  ones it gets, so one of those with such a number would be lost or
 *  swapped. Returns 0 or an error number.
 */
extern int
child_std_open (void);

#endif /* TWINCEPTION_SRC_CHILD_H_ */
