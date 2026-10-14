/** @file The roster is the table of the tool calls that run on the client
 *  loops and have not become tasks.  A notifications/cancelled that comes
 *  to any loop reaches the call that it names through the roster
 *  (src/mcp.c).
 *
 * A call's loop puts a record of the call into the roster when the call
 * starts.  The loop takes the record out when the call ends or becomes a
 * task.  The record has a key that
 * the call's loop writes.  The key consists of the caller, the session
 * field if the request has one, and the request id.  roster_cancel() looks
 * the key up under the roster's lock.  The lock guards the table, the
 * links of the records and their member asked.  After it has released the
 * lock, roster_cancel() posts the message of each matching record to the
 * loop of the record's call, at most once for each record.  The loop looks
 * the call up by the record's handle (src/loop.h).  The call's loop frees
 * that handle when the call leaves the roster, so loop_lookup() returns
 * nullptr for a cancel that arrives after the end of the call.
 *
 * A record has a reference count.  The count includes one reference for
 * the roster while the record is in the roster, and one for a posted
 * cancel message.  The call's loop drops both, and the last drop frees the
 * record.
 */
#ifndef TWINCEPTION_SRC_ROSTER_H_
#define TWINCEPTION_SRC_ROSTER_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "loop.h"

struct roster;

/** The record of a call.  The members key, len, hash, loop and who do not
 *  change while the record is in the roster, so they are read without the
 *  roster's lock. */
struct roster_call {
	/** The message that roster_cancel() posts to loop when a client asks
	 *  to cancel the call.  Its function drops the reference that
	 *  roster_cancel() took for the message. */
	struct msg           cancel;
	struct roster_call  *next;    /**< the next record in the bucket */
	struct roster_call  *hit;     /**< the next record that the same
	                                   roster_cancel() call matched */
	struct loop         *loop;    /**< the loop of the call */
	size_t               len;     /**< the length of key in bytes */
	uint64_t             hash;    /**< the seeded hash of key */
	struct handle        who;     /**< the handle of the call's request on
	                                   loop */
	_Atomic uint32_t     refs;    /**< the number of references */
	bool                 asked;   /**< true once a client has asked to
	                                   cancel the call */
	unsigned char        key[];   /**< the key, len bytes */
};

/** Creates an empty roster.  Returns the roster.  On failure it returns
 *  nullptr and stores the error number in @p *e. */
extern struct roster *
roster_create (int *e);

/** Frees the roster and sets @p *p_roster to nullptr.  No loop may run, and
 *  every call must have left the roster. */
extern void
roster_destroy (struct roster **p_roster);

/**
 * Puts a record of a call into the roster.  @p key is the call's key and
 * @p hash its seeded hash.  @p who is the handle of the call's request on
 * @p loop.  @p fn is the function of the message that roster_cancel()
 * posts.
 *
 * Returns the record.  The caller passes it to roster_leave(), which drops
 * the roster's reference.  If memory runs out, the function returns
 * nullptr and stores ENOMEM in @p *e.
 */
extern struct roster_call *
roster_enter (struct roster  *roster,
              struct span     key,
              uint64_t        hash,
              struct loop    *loop,
              struct handle   who,
              void          (*fn) (struct loop *loop,
                                   struct msg  *msg),
              int            *e);

/** Takes the record @p *p_call out of the roster, drops the roster's
 *  reference to it, and sets @p *p_call to nullptr.  The loop of the call
 *  calls this function.  After it returns, roster_cancel() does not match
 *  the record.
 *  It does nothing if @p *p_call is nullptr. */
extern void
roster_leave (struct roster       *roster,
              struct roster_call **p_call);

/** Asks to cancel each call in the roster whose key is @p key.  @p hash is
 *  the seeded hash of @p key.  The loop of each such call gets the record's
 *  message once, also if clients ask more than once.  Any thread can call
 *  this function. */
extern void
roster_cancel (struct roster *roster,
               struct span    key,
               uint64_t       hash);

/** Drops a reference to the record @p call.  The last drop frees the
 *  record.  It does nothing if @p call is nullptr. */
extern void
roster_call_put (struct roster_call *call);

#endif /* TWINCEPTION_SRC_ROSTER_H_ */
