/** @file The ledger: records of tool calls that run on after askai has
 *  answered the requests that started them.  Such a call is a task of MCP's
 *  tasks extension (2026-07-28), or a job, which a client starts with the
 *  tool background.  A client reads a record from any connection, on any
 *  loop (src/mcp.c).
 *
 * Each record has an owner and an id of 128 random bits.  The owner is a
 * 64-bit key: the principal of the client's identity or Unix user, or a
 * hash of an anonymous client's address.  ledger_find() returns a record
 * only to a caller with the owner's key.  The call of a record runs on one
 * loop.  As the call ends, it writes its result into the record and then
 * stores the status, last, with release ordering.  A reader that loads a
 * status of an ended call with acquire ordering can read the rest of the
 * record without the lock, because nothing changes it after that.
 *
 * The ledger removes a record LEDGER_KEEP seconds after its call ends.  It
 * removes ended records sooner, the oldest first, while it has more than
 * LEDGER_BYTES of results.  When it has LEDGER_MAX records, a new record
 * makes it remove the oldest ended record of the owner with the most
 * ended records, so that owners with many records do not remove the
 * records of owners with few.  When an owner with LEDGER_MINE records
 * opens another record, the ledger removes the owner's oldest ended
 * record.  It never removes a record whose call runs.  An owner can open a
 * job only while fewer than LEDGER_RUNNING of its records have calls that
 * run.  The ledger's lock protects its table of records, its list of ended
 * records and its owners.
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
	LEDGER_ID      = 32,    /**< hex digits of an id */
	LEDGER_MAX     = 1024,  /**< records in the ledger at most */
	LEDGER_KEEP    = 3600,  /**< seconds from the end of a record's call to
	                             the record's removal */
	LEDGER_RUNNING = 8,     /**< an owner can open a job only while fewer
	                             of its records have calls that run */
	LEDGER_MINE    = 64,    /**< records of one owner before ledger_open()
	                             removes the owner's oldest ended record */
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

/** What made a record. */
enum ledger_kind : uint16_t {
	LEDGER_TASK,  /**< a tool call that became a task of MCP's tasks
	                   extension */
	LEDGER_JOB,   /**< a call of the tool background of a client */
};

/** What the ledger has done: any thread writes, any reads (src/hist.h). */
struct ledger_metrics {
	_Atomic uint64_t opened;   /**< records of tasks opened */
	_Atomic uint64_t jobs;     /**< records of jobs opened */
	_Atomic uint64_t ended[LEDGER_STATUSES];  /**< by status; working: none */
	_Atomic uint64_t gone;     /**< records removed, after LEDGER_KEEP or to
	                                make room */
	_Atomic uint64_t full;     /**< records not opened: no room */
	_Atomic uint64_t refused;  /**< jobs not opened: their owner had
	                                LEDGER_RUNNING records whose calls ran */
	_Atomic uint64_t kept;     /**< records now */
	_Atomic uint64_t bytes;    /**< of their results now */
};

struct ledger;
struct ledger_owner;

/** A record of a task or a job.  The ledger has a reference to it while
 *  the record is in the ledger, the call one while it runs, and each
 *  reader and each posted cancel one. */
struct ledger_task {
	struct msg           cancel;     /**< posted to loop, msg.fn the call's,
	                                      once a client asks to cancel the
	                                      task while it works; the handler
	                                      drops the reference it came with */
	struct list          hook;       /**< the ledger's: on its ended list */
	struct list          mine;       /**< the ledger's: on its owner's list
	                                      of records, oldest first */
	struct ledger_task  *next;       /**< the ledger's: in its bucket */
	struct ledger_owner *owner;      /**< the ledger's, used only under its
	                                      lock; nullptr once the record is
	                                      out of the ledger */
	struct loop         *loop;       /**< the call's */
	char                *result;     /**< the call's result, as JSON, once
	                                      whole; nullptr if none */
	size_t               len;        /**< bytes of it written */
	size_t               size;       /**< and to come */
	uint64_t             hash;       /**< of its id */
	uint64_t             created;    /**< CLOCK_REALTIME ns */
	uint64_t             ended;      /**< likewise, once its call has */
	uint64_t             due;        /**< loop_clock() when the ledger
	                                      removes the record, once ended */
	struct handle        who;        /**< names the call on its loop */
	_Atomic uint64_t     status;     /**< enum ledger_status: an end
	                                      published last */
	_Atomic uint32_t     refs;
	_Atomic uint32_t     asked;      /**< bool: a client asked to cancel */
	_Atomic uint32_t     told;       /**< bool: the owner has read the
	                                      record of the ended call */
	uint16_t             tool;       /**< the id of the call's tool
	                                      (src/registry.h) */
	enum ledger_kind     kind;
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
 * Open a working record of @p kind, under a new id, for the owner @p owner
 * and the call of the tool @p tool that @p who names on @p loop.  @p fn
 * handles a cancel of the call.  If the owner already has LEDGER_MINE
 * records, the ledger removes the owner's oldest ended record, if it has
 * one.  Returns a reference to the record, which belongs to the call.  Any
 * loop can then look the record up with ledger_find().  On failure,
 * returns nullptr with the error in @p *e:
 * - EBUSY for a job, while the owner has LEDGER_RUNNING records whose
 *   calls run;
 * - ENOSPC while the ledger has LEDGER_MAX records whose calls run;
 * - EINVAL for a null pointer or an unknown kind;
 * - ENOMEM, or the error of getrandom().
 */
extern struct ledger_task *
ledger_open (struct ledger    *ledger,
             uint64_t          owner,
             enum ledger_kind  kind,
             uint16_t          tool,
             struct loop      *loop,
             struct handle     who,
             void            (*fn) (struct loop *loop,
                                    struct msg  *msg),
             int              *e);

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
 * own to drop.  Does nothing for a null pointer, a status that is not an
 * end, or a record whose call has already ended.
 */
extern void
ledger_end (struct ledger      *ledger,
            struct ledger_task *task,
            enum ledger_status  status);

/** The record with the id @p id, of @p len bytes, as a reference, if the
 *  record is in the ledger and its owner is @p owner; else nullptr. */
extern struct ledger_task *
ledger_find (struct ledger *ledger,
             uint64_t       owner,
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
