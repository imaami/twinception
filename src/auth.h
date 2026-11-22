/** @file auth.h
 *
 * The renewed credential of the upstream: the bearer token of
 * --auth-command (src/bearer.h) or of the ChatGPT sign-in (src/chatgpt.h).
 * One object on the loop of home answers one question for every request
 * that goes to the upstream: which Authorization line to send now.
 *
 * The object keeps the line of its last read of the credential file, and
 * gives it at once while the token is valid for AUTH_EARLY more seconds
 * and the read is less than AUTH_REVIEW seconds old. Otherwise a blocker
 * reads the file, and renews the credential if it must: the lock of the
 * file, the HTTPS of a refresh and the command never hold up the loop.
 * One read answers every request that waits for it.
 */
#ifndef ASKAI_SRC_AUTH_H_
#define ASKAI_SRC_AUTH_H_

#include <stdint.h>

#include "blocker.h"
#include "compat.h"
#include "dstr.h"
#include "flag.h"
#include "list.h"
#include "loop.h"

struct ai_endpoint;

/** The flags of a wait, and of a read of bearer_header() or
 *  chatgpt_header().
 */
enum auth_flag : uint32_t {
	/** only the credential file answers: no command runs, nothing is
	 *  refreshed and no file is made. Without a valid token in the file,
	 *  the answer is an error.
	 */
	AUTH_ASK_FILE_ONLY = FLAG_OWN(0),
};

enum {
	AUTH_EARLY  = 60,  /**< a token is renewed, and not given to a new
	                        request, this many seconds before its end */
	AUTH_REVIEW = 60,  /**< the kept line is given for this many seconds
	                        after the read of the file that it is from, so
	                        that a sign-in of another process is seen */
};

struct auth_wait;

/** The answer to a wait, on the loop of the auth: the error @p e, or 0 and
 *  the Authorization line @p line, which is valid until the function
 *  returns. The wait is not linked, and the function may free it.
 */
typedef void auth_wait_fn (struct auth_wait *wait,
                           int               e,
                           char const       *line);

/** A request that waits for the credential. It is a member of the object
 *  of the request, such as a job. hook is an empty list while the request
 *  does not wait.
 */
struct auth_wait {
	struct list   hook;
	auth_wait_fn *done;
	struct flag64 flags;  /**< enum auth_flag, widened over the padding */
};

/** Returns a wait that does not wait, with @p done as its answer. The
 *  caller passes the address of the wait itself, for its empty list.
 */
static force_inline struct auth_wait
auth_wait (struct auth_wait *self,
           auth_wait_fn     *done)
{
	return (struct auth_wait){
		.hook = list(&self->hook),
		.done = done
	};
}

struct auth_io;

/** The credential of one upstream on one loop. Only that loop uses the
 *  object.
 */
struct auth {
	struct list       waiting;  /**< the waits for the read under way, or
	                                 for the next one */
	struct list       ready;    /**< the waits that the step answers */
	struct loop_step  deliver;  /**< answers the waits on ready */
	struct loop      *loop;
	struct blocker   *blockers;
	struct auth_io   *io;       /**< the read of a blocker; nullptr if the
	                                 upstream has no renewed credential */
	char const       *line;     /**< the Authorization line of the last
	                                 read, or nullptr */
	struct dstr       text;     /**< the bytes of line */
	uint64_t          expires;  /**< when the token of line ends, in
	                                 seconds of the real-time clock */
	uint64_t          review;   /**< the time of the loop from which the
	                                 file is read again */
	int32_t           error;    /**< of the read that the waits on ready
	                                 waited for */
	struct flag32     flags;    /**< AUTH_BUSY (src/auth.c) */
};

/** Initializes @p auth for the upstream @p ep on @p loop. The reads run on
 *  @p blockers; without blockers each wait is answered with ENOSYS.
 *  Returns 0, or ENOMEM. auth_fini() is safe after an error.
 */
extern int
auth_init (struct auth              *auth,
           struct loop              *loop,
           struct blocker           *blockers,
           struct ai_endpoint const *ep);

/** Frees what @p auth holds, after every wait has left or was answered. A
 *  read that still runs on a blocker is left to end by itself.
 */
extern void
auth_fini (struct auth *auth);

/** Whether the upstream of @p auth has a renewed credential. Without one,
 *  its requests are sent with the header lines of the endpoint alone, and
 *  the caller does not call auth_ask().
 */
static force_inline bool
auth_renewed (struct auth const *auth)
{
	return auth && auth->io;
}

/**
 * Asks for the Authorization line of a request. With a fresh line, the
 * function writes the line to @p *line and returns 0: the caller sends its
 * request before it returns to the loop, and @p wait is not used.
 * Otherwise @p *line is nullptr, and with 0 the function has linked @p wait
 * with @p flags of enum auth_flag: the function wait->done answers it
 * once, in a later step of the loop, unless auth_wait_leave() comes first.
 * Returns EIO without a clock; @p wait is not linked then.
 */
extern int
auth_ask (struct auth       *auth,
          struct auth_wait  *wait,
          uint32_t           flags,
          char const       **line);

/** Ends the wait of @p wait, which gets no answer. Does nothing if @p wait
 *  does not wait. A read that only this wait waited for still ends.
 */
static force_inline void
auth_wait_leave (struct auth_wait *wait)
{
	list_del(&wait->hook);
}

/** Returns words for the user about the error @p e of a wait of @p auth or
 *  of auth_ask(): those of the source of the credential (bearer_why(),
 *  chatgpt_why()).
 */
extern char const *
auth_why (struct auth const *auth,
          int                e);

#endif /* ASKAI_SRC_AUTH_H_ */
