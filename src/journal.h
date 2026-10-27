/** @file Reasoning text archives and bounded, private recovery snapshots.
 *
 * One worker appends decoded reasoning to persistent plain files and commits
 * them only at reasoning boundaries. Recovery controls never enter Git.
 * Completed checkpoints expire when the private recovery cache reaches its
 * 1024-job or 256 MiB limit. A terminal-record reserve adds at most 16 MiB
 * plus framing; atomic replacement uses at most 64 MiB of staging space.
 * Active and legacy-migrated jobs are retained. Each participant has an
 * exclusive writer lock. No disk or Git operation runs on an event loop.
 */
#ifndef TWINCEPTION_SRC_JOURNAL_H_
#define TWINCEPTION_SRC_JOURNAL_H_

#include <stddef.h>
#include <stdint.h>

struct journal;
struct loop;

enum {
	JOURNAL_ID         = 32,
	JOURNAL_KIND       = 32,
	JOURNAL_RECORD_MAX = 16 << 20,
	JOURNAL_LOAD_MAX   = 64 << 20,
};

struct journal_record {
	struct journal_record *next;
	uint64_t               sequence;
	size_t                 len;
	char                   id[JOURNAL_ID + 1];
	char                   kind[JOURNAL_KIND + 1];
	unsigned char          data[];
};

typedef void
journal_done_cb (struct loop *loop,
                 void        *arg,
                 int          error);

typedef void
journal_load_cb (struct loop           *loop,
                 void                  *arg,
                 int                    error,
                 struct journal_record *records);

/** Opens or creates a bare repository and starts its writer.  Called at
 * startup, off the loops.  Participant is 1..64 ASCII letters, digits,
 * underscores or hyphens.  Missing path components are made with 0700;
 * symbolic links and parent-directory components are refused.
 */
extern struct journal *
journal_create (char const *path,
                char const *participant,
                char       *why,
                size_t      why_len,
                int        *e);

/** Stops accepting writes, drains the queue, and joins the writer.  Call
 * after producers stop and before any destination loop is finalized.
 * The operation is idempotent; queued callbacks remain for loop_fini().
 */
extern void
journal_stop (struct journal *journal);

/** Drains accepted writes and joins the writer.  Loops receiving callbacks
 * must remain allocated until this returns, then drain their inboxes.
 */
extern void
journal_destroy (struct journal **p_journal);

/** Copies and queues one record, or returns an errno without accepting it.
 * EAGAIN means the queue is full.  A callback, if supplied, runs on loop
 * after the private recovery snapshot is durable.  After a record of a job
 * is lost, later records of that job fail with the same error until a
 * "state" snapshot or a terminal record ("result" or "cancel") replaces
 * them all; other jobs are not affected.  Other kinds than "state",
 * "meta", "tool-intent", "tool-result", "result", "cancel" and "legacy"
 * fail with EINVAL.
 * Records of kind "cancel" with at most 1024 payload bytes can use a
 * reserved allowance of 1024 queue entries and 2 MiB beyond the ordinary
 * 256-entry/32 MiB limit, so a full stream queue cannot prevent recording
 * one cancellation for each job in the ledger.
 * A null journal disables recording and returns 0 (without a callback).
 */
extern int
journal_put (struct journal  *journal,
             char const      *id,
             char const      *kind,
             void const      *data,
             size_t           len,
             struct loop     *loop,
             journal_done_cb *done,
             void            *arg);

/** Append decoded reasoning at its logical byte offset. Replayed bytes
 * must match the existing file; only a new suffix is appended. Completion
 * means the bytes reached the file, not that each delta was synchronized.
 * The file survives interruption and is synchronized at thought_end().
 * An empty write at offset zero begins a new replay verification attempt;
 * it resets the verified prefix without creating or changing a file.
 */
extern int
journal_thought (struct journal  *journal,
                 char const      *id,
                 uint32_t         turn,
                 uint64_t         offset,
                 void const      *data,
                 size_t           len,
                 struct loop     *loop,
                 journal_done_cb *done,
                 void            *arg);

/** Synchronize and commit this turn's reasoning if it changed. Unchanged
 * content is a successful asynchronous no-op. Intermediate boundaries leave
 * an incompletely replayed existing file alone; final requires its entire
 * prefix to have been verified. Missing files are successful no-ops.
 */
extern int
journal_thought_end (struct journal  *journal,
                     char const      *id,
                     uint32_t         turn,
                     bool             final,
                     struct loop     *loop,
                     journal_done_cb *done,
                     void            *arg);

/** Loads matching records in sequence order on the writer.  Null id/kind
 * matches all. New-format snapshots contain only recovery controls. The
 * callback owns its records even on error, and releases
 * them with journal_records_destroy().  Reads exceeding JOURNAL_LOAD_MAX
 * fail with EFBIG instead of growing without bound.
 */
extern int
journal_load (struct journal  *journal,
              char const      *id,
              char const      *kind,
              struct loop     *loop,
              journal_load_cb *done,
              void            *arg);

/** Loads the latest turn's private recovery controls. Missing new-format
 * snapshots fall back to the legacy WAL, read-only. No state returns an
 * empty list. Ownership and callbacks follow journal_load().
 */
extern int
journal_load_current (struct journal  *journal,
                      char const      *id,
                      struct loop     *loop,
                      journal_load_cb *done,
                      void            *arg);

extern void
journal_records_destroy (struct journal_record **p_records);

extern uint64_t
journal_seed (struct journal const *journal);

extern char const *
journal_path (struct journal const *journal);

#endif /* TWINCEPTION_SRC_JOURNAL_H_ */
