/** @file Durable provider transcripts, in a repository separate from notes.
 *
 * Accepted records pass through a bounded queue to one writer thread.  The
 * writer appends checksummed records to its participant's write-ahead log,
 * synchronizes it, and commits a batch to refs/heads/transcripts/PARTICIPANT.
 * A completion means both the log and Git commit are durable.  Startup
 * imports complete records left in the log by an interrupted Git commit.
 * Each participant has an exclusive lock; distinct participants may share
 * the repository.  No disk or Git operation runs on an event loop.
 */
#ifndef TWINCEPTION_SRC_JOURNAL_H_
#define TWINCEPTION_SRC_JOURNAL_H_

#include <stddef.h>
#include <stdint.h>

struct journal;
struct loop;

enum {
	JOURNAL_ID = 32,
	JOURNAL_KIND = 32,
	JOURNAL_RECORD_MAX = 16 << 20,
	JOURNAL_LOAD_MAX = 64 << 20,
};

struct journal_record {
	struct journal_record *next;
	uint64_t               sequence;
	size_t                 len;
	char                   id[JOURNAL_ID + 1];
	char                   kind[JOURNAL_KIND + 1];
	unsigned char          data[];
};

typedef void journal_done_cb (struct loop *loop, void *arg, int error);
typedef void journal_load_cb (struct loop *loop, void *arg, int error,
                              struct journal_record *records);

/** Opens or creates a bare repository and starts its writer.  Called at
 *  startup, off the loops.  Participant is 1..64 ASCII letters, digits,
 *  underscores or hyphens.  Missing path components are made with 0700;
 *  symbolic links and parent-directory components are refused. */
extern struct journal *
journal_create (char const *path, char const *participant,
                char *why, size_t why_len, int *e);

/** Stops accepting writes, drains the queue, and joins the writer.  Call
 *  after producers stop and before any destination loop is finalized.
 *  The operation is idempotent; queued callbacks remain for loop_fini(). */
extern void journal_stop (struct journal *journal);

/** Drains accepted writes and joins the writer.  Loops receiving callbacks
 *  must remain allocated until this returns, then drain their inboxes. */
extern void journal_destroy (struct journal **p_journal);

/** Copies and queues one record, or returns an errno without accepting it.
 *  EAGAIN means the queue is full.  A callback, if supplied, runs on loop
 *  after persistence; otherwise journal_error() reports asynchronous errors.
 *  Records of kind "cancel" with at most 1024 payload bytes can use a
 *  reserved allowance of 1024 queue entries and 2 MiB beyond the ordinary
 *  256-entry/32 MiB limit, so a full stream queue cannot prevent recording
 *  one cancellation for each job in the ledger.
 *  A null journal disables recording and returns 0 (without a callback). */
extern int
journal_put (struct journal *journal, char const *id, char const *kind,
             void const *data, size_t len, struct loop *loop,
             journal_done_cb *done, void *arg);

/** Loads matching records in sequence order on the writer.  Null id/kind
 *  matches all.  The callback owns its records even on error, and releases
 *  them with journal_records_destroy().  Reads exceeding JOURNAL_LOAD_MAX
 *  fail with EFBIG instead of growing without bound. */
extern int
journal_load (struct journal *journal, char const *id, char const *kind,
              struct loop *loop, journal_load_cb *done, void *arg);

/** Loads only the newest "state" record of one job and the records after
 *  it.  Older turns remain in the archive but do not consume the recovery
 *  memory limit.  No state returns an empty list.  Ownership and callbacks
 *  follow journal_load(). */
extern int
journal_load_current (struct journal *journal, char const *id,
                      struct loop *loop, journal_load_cb *done, void *arg);

extern void journal_records_destroy (struct journal_record **p_records);
extern int journal_error (struct journal const *journal);
extern uint64_t journal_seed (struct journal const *journal);
extern char const *journal_path (struct journal const *journal);

#endif /* TWINCEPTION_SRC_JOURNAL_H_ */
