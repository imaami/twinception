/** @file ledger.h
 *
 * The ledger: records of tool calls that run on after askai has
 * answered the requests that started them. Such a call is a task of MCP's
 * tasks extension (2026-07-28), or a job, which a client starts with the
 * tool background or an ask call with --background. A client reads a
 * record from any connection, on any loop (src/mcp.c).
 *
 * Each record has an owner and an id of 128 random bits. The owner is a
 * 64-bit key: the principal of the client's identity or Unix user, or a
 * hash of an anonymous client's address. ledger_find() returns a record
 * only to a caller with the owner's key. The call of a record runs on one
 * loop. As the call ends, it writes its result into the record and then
 * stores the status, last, with release ordering. A reader that loads a
 * status of an ended call with acquire ordering can read the rest of the
 * record without the lock, because nothing changes it after that.
 *
 * The ledger removes a record LEDGER_KEEP seconds after its call ends. It
 * removes ended records sooner, the oldest first, while it has more than
 * LEDGER_BYTES of results. When it has LEDGER_MAX records, a new record
 * makes it remove the oldest ended record of the owner with the most
 * ended records, so that owners with many records do not remove the
 * records of owners with few. When an owner with LEDGER_MINE records
 * opens another record, the ledger removes the owner's oldest ended
 * record. It never removes a record whose call runs. An owner can open a
 * job only while fewer than LEDGER_RUNNING of its records have calls that
 * run. The ledger's lock protects its table of records, its list of ended
 * records and its owners, with their timers and waits.
 *
 * An owner also has timers and waits. A timer is a time of the loop clock
 * and a note. Nothing runs at that time: a timer whose time has passed has
 * fired. A wait is a call of the owner that waits for the owner's records'
 * calls to end and for the owner's timers to fire. The ledger posts the
 * message of each wait of the owner when a record's call ends and when the
 * owner sets a timer. It posts the message again only after the handler
 * of the message has called ledger_wait_heard(). The handler therefore
 * calls ledger_wait_heard() before the wait calls ledger_tell(), so that
 * ledger_tell() returns every end and every timer that came after the post.
 * ledger_tell() returns each fired timer, and each ended record that no
 * one has read, to one wait only, and removes the timer. A wait that names
 * records gets those that have ended, read or not. The waiting call sets
 * a timer of its own loop for the time of the first timer that has not
 * fired. The ledger removes an owner when it has no records, no timers and
 * no waits. It removes a fired timer that no wait has returned LEDGER_KEEP
 * seconds after the timer's time.
 */
#ifndef ASKAI_SRC_LEDGER_H_
#define ASKAI_SRC_LEDGER_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "cstr_view.h"
#include "dstr.h"
#include "hexid.h"
#include "htab.h"
#include "list.h"
#include "loop.h"
#include "util.h"

enum {
	LEDGER_MAX     = 1024,  /**< records in the ledger at most */
	LEDGER_KEEP    = 3600,  /**< seconds from the end of a record's call to
	                             the record's removal */
	LEDGER_RUNNING = 8,     /**< an owner can open a job only while fewer
	                             of its records have calls that run */
	LEDGER_MINE    = 64,    /**< records of one owner before ledger_open()
	                             removes the owner's oldest ended record */
	LEDGER_TIMERS  = 8,     /**< timers of one owner, set or fired */
	LEDGER_NOTE    = 200,   /**< bytes of a timer's note */
	LEDGER_WAITS   = 4,     /**< waits of one owner at once */
	LEDGER_OWNERS  = 1024,  /**< owners in the ledger at most */
};

/** Bytes of results the ledger keeps, at most, but for the newest's. */
static constexpr size_t LEDGER_BYTES = 64 << 20;

/** The bit of the key of an owner that is an anonymous client's address.
 *  The ledger has at most LEDGER_OWNERS / 2 such owners, so that anonymous
 *  clients cannot fill the table of owners for identities and Unix users.
 */
#define LEDGER_ANONYMOUS (UINT64_C(1) << 63)

/** Where a task stands, working or how its call ended: X(ID, name), the
 *  constant LEDGER_ID and its word, as MCP's tasks extension names it.
 */
#define LEDGER_STATUS_LIST(X)                                                     \
        X(WORKING,   "working")                                                   \
        X(COMPLETED, "completed")  /* its result is the call's */                 \
        X(FAILED,    "failed")     /* with an error: its result, if it has one */ \
        X(CANCELLED, "cancelled")  /* as its client asked */

#define LEDGER_STATUS_ENUM(id, ...) LEDGER_##id,
enum ledger_status {
	LEDGER_STATUS_LIST(LEDGER_STATUS_ENUM)
	LEDGER_STATUSES
};
#undef LEDGER_STATUS_ENUM

/** The bits of the state of a record besides its status, an enum
 *  ledger_status in the bits LEDGER_STATUS.
 */
enum ledger_state : uint64_t {
	LEDGER_STATUS = 3u,       /**< the bits of the status */
	LEDGER_ASKED  = 1u << 2,  /**< a client asked to cancel the task */
	LEDGER_TOLD   = 1u << 3,  /**< the owner has read the record of the
	                               ended call */
};

static_assert(LEDGER_STATUSES - 1 <= LEDGER_STATUS);

/** The kinds of records, one X(ID, name) row per kind. The kind of a
 *  record is what made the record. The enum constant of a kind is
 *  LEDGER_ID, and name is the word for the kind.
 */
#define LEDGER_KIND_LIST(X)                                                       \
        X(TASK, "task")  /* a tool call that became an MCP task */                \
        X(JOB,  "job")   /* a client's background wrapper or detached ask call */

#define LEDGER_KIND_ENUM(id, ...) LEDGER_##id,
enum ledger_kind : uint8_t {
	LEDGER_KIND_LIST(LEDGER_KIND_ENUM)
	LEDGER_KINDS
};
#undef LEDGER_KIND_ENUM

/** The words for the kinds and the statuses of records, which the log,
 *  the answers of a client's jobs and wait, and tasks/get use; the
 *  statuses as MCP's tasks extension names them.
 */
extern char const *const ledger_kinds[LEDGER_KINDS];
extern struct cstr_view const ledger_statuses[LEDGER_STATUSES];

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
	_Atomic uint64_t timers;   /**< timers set */
	_Atomic uint64_t kept;     /**< records now */
	_Atomic uint64_t bytes;    /**< of their results now */
	_Atomic uint64_t waits;    /**< waits now */
};

struct ledger;
struct ledger_owner;

/** A timer: the time it fires at, its number, and its note. */
struct ledger_timer {
	uint64_t                   due;   /**< the loop_clock() time when the
	                                       timer fires, or 0 if this entry
	                                       is unused */
	uint32_t                   id;    /**< the number of the timer, from 1.
	                                       ledger_timer_set() numbers the
	                                       timers of all owners with one
	                                       counter. */
	UTIL_TEXT(LEDGER_NOTE + 8) note;  /**< the note, with 8 more bytes than
	                                       LEDGER_NOTE for the NUL and for 7
	                                       bytes that would otherwise be
	                                       padding */
};

/** A call of an owner that waits for the owner's records' calls to end
 *  and for its timers to fire. The ledger has a reference to it while the
 *  wait is in the ledger, and each posted message has one.
 */
struct ledger_wait {
	struct msg           wake;    /**< posted to loop, msg.fn the waiting
	                                   call's, when a record of the owner
	                                   ends or the owner sets a timer; the
	                                   handler calls ledger_wait_heard()
	                                   first and drops the reference that the
	                                   message came with last */
	struct list          hook;    /**< the ledger's: on its owner's list of
	                                   waits */
	struct ledger_wait  *hit;     /**< the ledger's: the next wait to post */
	struct ledger_owner *owner;   /**< the ledger's, used only under its
	                                   lock; nullptr once the wait is out of
	                                   the ledger */
	struct loop         *loop;    /**< the waiting call's */
	struct handle        who;     /**< names the waiting call on its loop */
	_Atomic uint32_t     refs;
	_Atomic uint32_t     posted;  /**< bool: wake is posted, and its handler
	                                   has not called ledger_wait_heard() */
};

/** What ledger_list() or ledger_tell() returns of one owner, as the ledger
 *  had it under its lock.
 */
struct ledger_look {
	struct ledger_task  *tasks[LEDGER_MINE];     /**< references to records */
	struct ledger_timer  timers[LEDGER_TIMERS];
	uint64_t             due;       /**< loop_clock() when the first timer
	                                     that has not fired fires;
	                                     UINT64_MAX if none */
	uint64_t             more;      /**< records of the owner that tasks
	                                     leaves out */
	uint32_t             n_tasks;
	uint32_t             n_timers;
};

/** A record of a task or a job. The ledger has a reference to it while
 *  the record is in the ledger, the call one while it runs, and each
 *  reader and each posted cancel one.
 */
struct ledger_task {
	struct msg           cancel;     /**< posted to loop, msg.fn the call's,
	                                      once a client asks to cancel the
	                                      task while it works; the handler
	                                      drops the reference it came with */
	struct list          hook;       /**< the ledger's: on its ended list */
	struct list          mine;       /**< the ledger's: on its owner's list
	                                      of records, oldest first */
	struct htab_node     node;       /**< the ledger's: in its table, by
	                                      the hash of the id; once out of
	                                      it, on a chain to drop */
	struct ledger_owner *owner;      /**< the ledger's, used only under its
	                                      lock; nullptr once the record is
	                                      out of the ledger */
	struct loop         *loop;       /**< the call's */
	struct dstr          result;     /**< the reply of the call: '"result":'
	                                      or '"error":', its value, and a '}'
	                                      (src/mcp.c) */
	size_t               size;       /**< the bytes of the whole reply, or 0
	                                      if there is no reply */
	uint64_t             created;    /**< CLOCK_REALTIME ns */
	uint64_t             ended;      /**< likewise, once its call has */
	uint64_t             due;        /**< loop_clock() when the ledger
	                                      removes the record, once ended */
	struct handle        who;        /**< names the call on its loop */
	_Atomic uint64_t     state;      /**< enum ledger_state, and the status,
	                                      an end published last */
	_Atomic uint32_t     refs;
	uint16_t             tool;       /**< the id of the call's tool
	                                      (src/registry.h) */
	enum ledger_kind     kind;
	struct hexid         id;
};

/** The status of @p task, loaded with acquire ordering: once it is not
 *  LEDGER_WORKING, the end of the call and its result can be read.
 */
static inline enum ledger_status
ledger_task_status (struct ledger_task const *task)
{
	return atomic_load_explicit(&task->state, memory_order_acquire) & LEDGER_STATUS;
}

/** An empty ledger; nullptr, with the error in @p *e, if it cannot be made. */
extern struct ledger *
ledger_create (int *e);

/** Free the ledger and its records, once no loop runs. */
extern void
ledger_destroy (struct ledger **p_ledger);

/** Returns the counts of the work of @p ledger. */
extern struct ledger_metrics const *
ledger_counts (struct ledger const *ledger);

/**
 * Open a working record of @p kind, under a new id, for the owner @p owner
 * and the call of the tool @p tool that @p who names on @p loop. @p fn
 * handles a cancel of the call. If the owner already has LEDGER_MINE
 * records, the ledger removes the owner's oldest ended record, if it has
 * one. Returns a reference to the record, which belongs to the call. Any
 * loop can then look the record up with ledger_find(). On failure,
 * returns nullptr with the error in @p *e:
 * - EBUSY for a job, while the owner has LEDGER_RUNNING records whose
 *   calls run;
 * - ENOSPC while the ledger has LEDGER_MAX records whose calls run, or
 *   while @p owner is not an owner of the ledger and the ledger has
 *   LEDGER_OWNERS owners, or, for an @p owner with LEDGER_ANONYMOUS,
 *   LEDGER_OWNERS / 2 owners with LEDGER_ANONYMOUS;
 * - EINVAL for a null pointer or an unknown kind;
 * - EEXIST if the random id is already in the ledger;
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

/** Restore a record from durable storage, copying @p id and @p result.
 *  Preserves @p created and @p ended, in CLOCK_REALTIME ns; for an ended
 *  record, an @p ended of 0 uses the current time. An ended record gets a
 *  fresh LEDGER_KEEP retention period. A working record requires @p loop
 *  and @p fn, and an empty result and an @p ended of 0. An ended record
 *  needs neither a loop nor a cancel handler; a cancelled record has no
 *  result.
 *
 *  Returns a reference for the caller, as ledger_open() does. Its limits
 *  and errors apply, except the running limit applies only to working
 *  jobs. EEXIST means the id is already in the ledger, under any owner:
 *  the existing record is never replaced or changed. EINVAL also covers
 *  no id, malformed statuses and result spans. No file I/O is performed.
 */
extern struct ledger_task *
ledger_restore (struct ledger      *ledger,
                uint64_t            owner,
                enum ledger_kind    kind,
                uint16_t            tool,
                struct hexid const *id,
                uint64_t            created,
                uint64_t            ended,
                enum ledger_status  status,
                struct span         result,
                struct loop        *loop,
                struct handle       who,
                void              (*fn) (struct loop *loop,
                                         struct msg  *msg),
                int                *e);

/** Makes room for the result of the call of @p task, of @p len bytes.
 *  Returns 0, or ENOMEM.
 */
extern int
ledger_task_reserve (struct ledger_task *task,
                     size_t              len);

/** Appends @p bytes to the result of the call of @p task. Returns 0, or
 *  EPIPE if the result would exceed the room that ledger_task_reserve()
 *  made.
 */
extern int
ledger_task_write (struct ledger_task *task,
                   struct span         bytes);

/**
 * The task's call has ended, as @p status says, its result kept only if
 * whole: publish the end, and let go of the oldest ended records past
 * LEDGER_BYTES of results, but this one. The call's reference stays its
 * own to drop. Does nothing for a null pointer, a status that is not an
 * end, or a record whose call has already ended.
 */
extern void
ledger_end (struct ledger      *ledger,
            struct ledger_task *task,
            enum ledger_status  status);

/** The record with the id @p id, as a reference, if the record is in the
 *  ledger and its owner is @p owner; else nullptr.
 */
extern struct ledger_task *
ledger_find (struct ledger      *ledger,
             uint64_t            owner,
             struct hexid const *id);

/** Remove an ended record owned by @p owner, dropping the ledger's
 *  reference. Other references remain valid. Returns 0, ENOENT if the
 *  id is absent or belongs to another owner, EBUSY if its call still
 *  works, or EINVAL for a null pointer or no id. Does not remove or cancel
 *  a working call.
 */
extern int
ledger_remove (struct ledger      *ledger,
               uint64_t            owner,
               struct hexid const *id);

/** Ask to cancel the task: its call's loop gets the cancel, once, if the
 *  task works.
 */
extern void
ledger_task_cancel (struct ledger_task *task);

/** Drop a reference to the record; the last frees it. */
extern void
ledger_task_put (struct ledger_task *task);

/**
 * Set a timer of the owner @p owner that fires at @p due, a time of
 * loop_clock() other than 0, with the @p len bytes at @p note as its note.
 * Stores the timer's number in @p *id, and posts the messages of the
 * owner's waits. One counter numbers the timers of all owners, so the
 * number of a timer does not depend on the owner's earlier timers.
 * Returns 0, or:
 * - EBUSY while the owner has LEDGER_TIMERS timers;
 * - ENOSPC while @p owner is not an owner of the ledger and the ledger has
 *   LEDGER_OWNERS owners, or, for an @p owner with LEDGER_ANONYMOUS,
 *   LEDGER_OWNERS / 2 owners with LEDGER_ANONYMOUS;
 * - EINVAL for a null pointer, a @p due of 0, or more than LEDGER_NOTE
 *   bytes of note;
 * - ENOMEM.
 */
extern int
ledger_timer_set (struct ledger    *ledger,
                  uint64_t          owner,
                  uint64_t          due,
                  struct cstr_view  note,
                  uint32_t         *id);

/** Sets the first timer of @p timers whose due is 0 to @p due, the number
 *  @p id and the note @p note. Returns 0, EBUSY if every timer is set or
 *  fired, or EINVAL for a @p due of 0 or a note longer than LEDGER_NOTE
 *  bytes.
 */
extern int
ledger_timer_add (struct ledger_timer timers[static LEDGER_TIMERS],
                  uint64_t            due,
                  uint32_t            id,
                  struct cstr_view    note);

/** Returns whether a timer of @p timers is set or fired. */
extern bool
ledger_timer_any (struct ledger_timer const timers[static LEDGER_TIMERS]);

/** Removes the timer of @p timers whose number is @p id, if it has not
 *  fired by @p now. Returns 0, EALREADY if it has fired, or ENOENT if no
 *  timer of @p timers with that number is set or fired.
 */
extern int
ledger_timer_drop (struct ledger_timer timers[static LEDGER_TIMERS],
                   uint32_t            id,
                   uint64_t            now);

/** Remove the timer of the owner @p owner whose number is @p id, if it has
 *  not fired. Returns 0, ENOENT if the owner has no such timer, EALREADY
 *  if the timer has fired and no ledger_tell() has returned it yet, or
 *  EINVAL for a null pointer. For a fired timer that the ledger has
 *  removed LEDGER_KEEP seconds after its time, it returns ENOENT.
 */
extern int
ledger_timer_cancel (struct ledger *ledger,
                     uint64_t       owner,
                     uint32_t       id);

/**
 * Put a wait of the owner @p owner into the ledger, for the call that
 * @p who names on @p loop. @p fn handles the wait's message. Returns the
 * wait, whose reference the caller passes to ledger_wait_leave(). On
 * failure, returns nullptr with the error in @p *e:
 * - EBUSY while the owner has LEDGER_WAITS waits;
 * - ENOSPC while @p owner is not an owner of the ledger and the ledger has
 *   LEDGER_OWNERS owners, or, for an @p owner with LEDGER_ANONYMOUS,
 *   LEDGER_OWNERS / 2 owners with LEDGER_ANONYMOUS;
 * - EINVAL for a null pointer;
 * - ENOMEM.
 */
extern struct ledger_wait *
ledger_wait_enter (struct ledger  *ledger,
                   uint64_t        owner,
                   struct loop    *loop,
                   struct handle   who,
                   void          (*fn) (struct loop *loop,
                                        struct msg  *msg),
                   int            *e);

/** The handler of the message of @p wait calls this first: from then on,
 *  the ledger can post the message again. Does nothing for nullptr.
 */
extern void
ledger_wait_heard (struct ledger_wait *wait);

/** Take the wait @p *p_wait out of the ledger, drop the reference to it
 *  that ledger_wait_enter() returned, and set @p *p_wait to nullptr. The
 *  ledger then posts the wait's message no more. Does nothing for a null
 *  pointer.
 */
extern void
ledger_wait_leave (struct ledger       *ledger,
                   struct ledger_wait **p_wait);

/** Drop a reference to the wait; the last frees it. */
extern void
ledger_wait_put (struct ledger_wait *wait);

/**
 * Store in @p out what the owner @p owner has: references to its newest
 * LEDGER_MINE records, the newest first, and the number of the others; its
 * timers, set or fired, by number; and the time of the first timer that has
 * not fired. First removes the ended records whose time is up, and the
 * owner's fired timers that have stayed LEDGER_KEEP seconds. Marks no
 * record read. Returns 0, or EINVAL for a null pointer, @p out then
 * empty if it is not nullptr. ledger_look_put() drops the references.
 */
extern int
ledger_list (struct ledger      *ledger,
             uint64_t            owner,
             struct ledger_look *out);

/**
 * Store in @p out what has happened for the wait @p wait: references to
 * the ended records of the wait's owner, the oldest first, at most
 * LEDGER_MINE, and the owner's fired timers, by number, which the ledger
 * then removes. If @p n is 0, the records are those that no one has read.
 * Otherwise they are those whose ids are among the @p n ids at @p ids,
 * read or not. Marks the records read, so no other ledger_tell() returns
 * an unread record or a timer that this call returns. @p out->due is the
 * time of the first timer that has not fired. Returns 0, or EINVAL for a
 * null pointer or a wait out of the ledger, @p out then empty if it is
 * not nullptr. ledger_look_put() drops the references.
 */
extern int
ledger_tell (struct ledger      *ledger,
             struct ledger_wait *wait,
             struct hexid const *ids,
             size_t              n,
             struct ledger_look *out);

/** Drop the references to records in @p look, which is then empty of them.
 *  Does nothing for nullptr.
 */
extern void
ledger_look_put (struct ledger_look *look);

#endif /* ASKAI_SRC_LEDGER_H_ */
