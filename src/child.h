/** @file child.h
 *
 * Child processes (design section 4). Each child is spawned with a
 * pidfd, in a session of its own. The steps of the loop that spawned it
 * read its standard output from a pipe into a capped buffer. The pidfd
 * becomes readable when the child exits, so askai does not use SIGCHLD.
 * The child is reaped by its pid after its output has also ended.
 * child_capture() and child_cmd_run() instead run a child on the calling
 * thread, with its output in a file, and reap the child through its
 * pidfd.
 */
#ifndef ASKAI_SRC_CHILD_H_
#define ASKAI_SRC_CHILD_H_

#include <signal.h>
#include <stdint.h>
#include <sys/types.h>

#include "buf.h"
#include "compat.h"
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

/** The bits of child_end.flags. The bits of CHILD_EVENTS mark the events
 *  that the next step of a child of a loop handles, and only child.c uses
 *  those bits.
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

/** How a child ended. For a child of a loop, flags can contain every bit
 *  of enum child_state. A synchronous run sets only CHILD_CUT and
 *  CHILD_LATE.
 */
struct child_end {
	int      code;    /**< CLD_EXITED, CLD_KILLED or CLD_DUMPED from
	                       waitid() after the child exits, and 0 before
	                       that or if waitid() failed */
	int      status;  /**< the exit status, or the number of the signal
	                       that killed the child */
	uint32_t flags;   /**< enum child_state */
};

static const_inline struct child_end
child_end (struct child_end const e)
{
	return e;
}

#define child_end(...) (child_end)((struct child_end){__VA_ARGS__})

/** Returns the exit status in @p end if the child exited before its
 *  deadline and its output was not cut, else -1.
 */
static force_inline int
child_end_status (struct child_end const *end)
{
	return end->flags & (CHILD_CUT | CHILD_LATE) || end->code != CLD_EXITED ? -1 : end->status;
}

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
	struct child_end   end;      /**< how the child ended, and the events
	                                  for its next step */
};

/** Returns the exit status of @p c if it exited before its deadline and its
 *  output was not cut, else -1.
 */
static force_inline int
child_status (struct child const *c)
{
	return child_end_status(&c->end);
}

/** Returns true if @p c exited with status 0 before its deadline and its
 *  output was not cut, and false if @p c is nullptr.
 */
static force_inline bool
child_ok (struct child const *c)
{
	return c && !child_status(c);
}

/**
 * Writes why a child that ran @p name failed into the @p size bytes at
 * @p buf. @p end is how the child ended. The function writes the first
 * reason in this list that applies:
 * - the child took longer than @p timeout ns;
 * - the output went past @p max bytes, written in the text as whole MiB,
 *   rounded down;
 * - a signal killed the child;
 * - the child exited with a status other than 0;
 * - the reason is unknown.
 * Returns a view of the text, or an empty view if the child exited with
 * status 0 before the deadline and the output was not cut.
 */
extern struct cstr_view
child_end_why (struct child_end const *end,
               char const             *name,
               uint64_t                timeout,
               size_t                  max,
               char                   *buf,
               size_t                  size);

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

static force_inline struct child_cmd
child_cmd (char const        *file,
           char const *const *argv,
           char *const       *envp,
           struct span        in,
           size_t             max,
           uint64_t           deadline,
           uint64_t           grace,
           enum child_flag    flags)
{
	return (struct child_cmd){
		.file     = file,
		.argv     = argv,
		.envp     = envp,
		.in       = in,
		.max      = max,
		.deadline = deadline,
		.grace    = grace,
		.flags    = flags
	};
}

/** The descriptors of a child that child_capture() or child_cmd_run()
 *  runs. A descriptor that the caller does not pass is -1. The input of
 *  the child is the member in, or else cmd->in, or else /dev/null. The
 *  output is the member out, or else /dev/null. The child runs in the
 *  directory of the member dir, or else in the directory of the caller.
 *  The child inherits the member keep as descriptor 3, such as the lock of
 *  a file.
 */
struct child_fds {
	int in;
	int out;
	int dir;
	int keep;
};

/** Returns a struct child_fds whose descriptors are all -1. */
static const_inline struct child_fds
child_fds (void)
{
	return (struct child_fds){
		.in   = -1,
		.out  = -1,
		.dir  = -1,
		.keep = -1
	};
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
 * Runs @p cmd to the end on the calling thread, without a loop, and
 * appends the output to @p out, up to cmd->max bytes. The child is spawned
 * as child_create() spawns it, but with the descriptors @p fds, and with a
 * file in memory in place of fds.out. If cmd->deadline, a time of
 * loop_clock(), is set, the function sends SIGTERM to the child at that
 * time and SIGKILL after cmd->grace, or SIGKILL at once if cmd->grace is
 * 0. The child does not count toward CHILD_MAX. The function writes the
 * code and the status of waitid() to @p *end, and sets CHILD_LATE in
 * end->flags if the deadline came. @p *end is all zero if the child was
 * not spawned.
 *
 * Returns 0 if the child exited before the deadline, with any exit
 * status. Returns ETIMEDOUT if the deadline came, EIO if a signal killed
 * the child before the deadline, or the error that stopped the spawn or
 * the wait. The output is read only if the child exited before the
 * deadline. The function then returns the error of the read, or EFBIG for
 * an output of more than cmd->max bytes. After EFBIG, end->flags contains
 * CHILD_CUT, and the first cmd->max bytes of the output are appended to
 * @p out.
 */
extern int
child_capture (struct child_cmd const *cmd,
               struct child_fds        fds,
               struct buf             *out,
               struct child_end       *end);

/**
 * Runs @p cmd to the end on the calling thread, and returns 0 only if the
 * child exited with status 0. If @p out is not nullptr, the function
 * removes the content of @p out and runs @p cmd as child_capture() does,
 * so that the content of @p out is the output, up to cmd->max bytes. If
 * @p out is nullptr, the function runs @p cmd in the same way with the
 * output of the child in fds.out, such as a memfd, without a cap, and
 * does not read the output. The function writes @p *end as
 * child_capture() does. Returns the error of the run or of the read, as
 * child_capture() returns it, or EIO if the child exited with a status
 * other than 0. After EIO, end->code is CLD_EXITED for such a status, and
 * another value if a signal killed the child.
 */
extern int
child_cmd_run (struct child_cmd const *cmd,
               struct child_fds        fds,
               struct buf             *out,
               struct child_end       *end);

/** Write the words of the @p n word arrays at @p parts, each up to its
 *  nullptr, and a nullptr to the @p cap pointers at @p argv. A nullptr in
 *  @p parts adds no words. Returns 0, or E2BIG if the words and the
 *  nullptr do not fit.
 */
extern int
child_argv (char const               **argv,
            size_t                     cap,
            char const *const *const  *parts,
            size_t                     n);

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

#endif /* ASKAI_SRC_CHILD_H_ */
