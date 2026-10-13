/** @file The ledger: the tasks of MCP's tasks extension (2026-07-28), tool
 *  calls that run on without the request that began them, which their
 *  clients poll from any connection, on any loop (src/mcp.c).
 *
 * A task's record goes into the ledger under an id of 128 random bits, by
 * which only its client's principal finds it.  Its call, on the loop that
 * runs it, writes its result into the record as it ends, then publishes the
 * end, its status last, with release: a reader that acquires a status that
 * has ended reads the rest, which no one changes from then on, without the
 * lock.  The record goes LEDGER_KEEP seconds after its end, or sooner, the
 * oldest ended first, once the ledger holds LEDGER_MAX records or more than
 * LEDGER_BYTES of results; a record whose call runs stays.  The ledger's
 * lock guards its table and its list of the ended.
 */
#ifndef TWINCEPTION_SRC_LEDGER_H_
#define TWINCEPTION_SRC_LEDGER_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "list.h"
#include "loop.h"

enum {
	LEDGER_ID   = 32,    /**< hex digits of an id */
	LEDGER_MAX  = 1024,  /**< records kept at most */
	LEDGER_KEEP = 600,   /**< seconds a record is kept after its call ends */
};

/** Bytes of results the ledger keeps, at most, but for the newest's. */
static constexpr size_t LEDGER_BYTES = 64 << 20;

/** Where a task stands: working, or how its call ended. */
enum ledger_status {
	LEDGER_WORKING,
	LEDGER_COMPLETED,  /**< its result is the call's */
	LEDGER_FAILED,     /**< with an error: its result, if it has one */
	LEDGER_CANCELLED,  /**< as its client asked */
	LEDGER_STATUSES
};

/** What the ledger has done: any thread writes, any reads (src/hist.h). */
struct ledger_metrics {
	_Atomic uint64_t opened;
	_Atomic uint64_t ended[LEDGER_STATUSES];  /**< by status; working: none */
	_Atomic uint64_t gone;    /**< records let go, their time up or for room */
	_Atomic uint64_t full;    /**< calls kept from being tasks: no room */
	_Atomic uint64_t kept;    /**< records now */
	_Atomic uint64_t bytes;   /**< of their results now */
};

struct ledger;

/** A task's record.  The ledger holds a reference while it keeps it, the
 *  call one while it runs, and each reader and posted cancel one. */
struct ledger_task {
	struct msg           cancel;     /**< posted to loop, msg.fn the call's,
	                                      once a client asks to cancel the
	                                      task while it works; the handler
	                                      drops the reference it came with */
	struct list          hook;       /**< the ledger's: on its ended list */
	struct ledger_task  *next;       /**< the ledger's: in its bucket */
	struct loop         *loop;       /**< the call's */
	char                *result;     /**< the call's result, as JSON, once
	                                      whole; nullptr if none */
	size_t               len;        /**< bytes of it written */
	size_t               size;       /**< and to come */
	uint64_t             principal;  /**< whose task it is */
	uint64_t             hash;       /**< of its id */
	uint64_t             created;    /**< CLOCK_REALTIME ns */
	uint64_t             ended;      /**< likewise, once its call has */
	uint64_t             due;        /**< loop_clock() as it goes, once
	                                      ended */
	struct handle        who;        /**< names the call on its loop */
	_Atomic uint64_t     status;     /**< enum ledger_status: an end
	                                      published last */
	_Atomic uint32_t     refs;
	_Atomic uint32_t     asked;      /**< bool: a client asked to cancel */
	char                 id[LEDGER_ID];
};

/** An empty ledger; nullptr, with the error in @p *e, if it cannot be made. */
extern struct ledger *
ledger_create (int *e);

/** Free the ledger and its records, once no loop runs. */
extern void
ledger_destroy (struct ledger **p_ledger);

extern struct ledger_metrics const *
ledger_metrics (struct ledger const *ledger);

/**
 * Open the record of a task of @p principal's, a working one under a new
 * id, for the call that @p who names on @p loop, whose @p fn handles its
 * cancel: a reference, the call's, once the record is in the ledger, for
 * any loop to find.  nullptr, with the error in @p *e: EBUSY while the
 * ledger holds LEDGER_MAX records whose calls run, ENOMEM, or getrandom()'s.
 */
extern struct ledger_task *
ledger_open (struct ledger  *ledger,
             uint64_t        principal,
             struct loop    *loop,
             struct handle   who,
             void          (*fn) (struct loop *loop,
                                  struct msg  *msg),
             int            *e);

/** The task's call: room for its result, of @p len bytes, to come; 0, or
 *  ENOMEM. */
extern int
ledger_task_reserve (struct ledger_task *task,
                     size_t              len);

/** The task's call: the next @p bytes of its result; 0, or EPIPE past the
 *  room made. */
extern int
ledger_task_write (struct ledger_task *task,
                   struct span         bytes);

/**
 * The task's call has ended, as @p status says, its result kept only if
 * whole: publish the end, and let go of the oldest ended records past
 * LEDGER_BYTES of results, but this one.  The call's reference stays its
 * own to drop.
 */
extern void
ledger_end (struct ledger      *ledger,
            struct ledger_task *task,
            enum ledger_status  status);

/** The record of the task @p id, of @p len bytes, names, as a reference, if
 *  the ledger keeps it and it is @p principal's; else nullptr. */
extern struct ledger_task *
ledger_find (struct ledger *ledger,
             uint64_t       principal,
             char const    *id,
             size_t         len);

/** Ask to cancel the task: its call's loop gets the cancel, once, if the
 *  task works. */
extern void
ledger_task_cancel (struct ledger_task *task);

/** Drop a reference to the record; the last frees it. */
extern void
ledger_task_put (struct ledger_task *task);

#endif /* TWINCEPTION_SRC_LEDGER_H_ */
