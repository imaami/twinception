/** @file Shared session notes. --notes names one Git repository, shared by
 * every participating askai instance. Each configured participant writes its
 * own refs/heads/notes/PARTICIPANT/WHO/SIDE branches. WHO is the client principal
 * or anonymous; SIDE is client or upstream. All notebooks can be read, but a
 * tool call's write branch is chosen by the server. Threads identify ask chains
 * and do not partition the notes. Replacing a note adds a commit; earlier
 * contents remain in the notebook's history.
 * Existing repositories can have worktrees; Git resolves their packed refs.
 * Concurrent changes to one branch fail rather than overwrite another writer.
 */
#ifndef TWINCEPTION_SRC_NOTES_H_
#define TWINCEPTION_SRC_NOTES_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "cstr_view.h"

struct notes;
struct notes_revision;

enum {
	NOTES_TEXT = 4 << 20,  /**< bytes of the text of a note, at most */
	NOTES_NAME = 255,      /**< bytes of the name of a note, at most */
	NOTES_MAX  = 16 << 20, /**< bytes of the output of git, at most */
	NOTES_ID   = 32,       /**< hexadecimal digits of the id of a thread */
	NOTES_REPO = 128,      /**< bytes of a full notebook branch ref, NUL included */
	NOTES_TIP  = 64,       /**< hexadecimal digits of a commit's name, at
	                            most */
	NOTES_ARGV = 25,       /**< elements of the argv of git, nullptr
	                            included */
};

/** The time from the start of git to its deadline, in nanoseconds. */
static constexpr uint64_t NOTES_NS = 10'000'000'000;

/** The time from SIGTERM to SIGKILL at the deadline of a git that writes,
 *  in nanoseconds. */
static constexpr uint64_t NOTES_GRACE = 1'000'000'000;

/** The age in seconds past which a lock file of a notebook has outlived
 *  every write of askai, so that its writer has died or hangs. */
static constexpr int64_t NOTES_STALE = 60;
static_assert(NOTES_NS + NOTES_GRACE < (uint64_t)NOTES_STALE * 1'000'000'000);

/** The two authors within a participant and principal. */
enum notes_side : uint8_t {
	NOTES_UPSTREAM,  /**< the upstream in ask calls */
	NOTES_CLIENT,    /**< a client */
};

/** The commands of git that write and read the notes. */
enum notes_cmd : uint8_t {
	NOTES_CREATE,  /**< initializes the shared repository */
	NOTES_CHECK,   /**< checks the name of a new note.  notes_check_input()
	                    writes its input. */
	NOTES_ADD,     /**< writes a note.  notes_add_input() writes its input. */
	NOTES_LIST,    /**< writes the note history, the newest first */
	NOTES_READ,    /**< writes a note.  notes_read_input() writes its
	                    input. */
	NOTES_BOOKS,   /**< lists all participant notebook selectors */
	NOTES_FORMAT,  /**< writes the Git object format for an unborn branch */
	NOTES_FILES,   /**< lists the current notes, in tree order */
	NOTES_HEAD,    /**< resolves a notebook branch to its full commit id */
};

/** A current note in the output of NOTES_FILES. Views borrow its output. */
struct notes_file {
	struct span name;
	struct span id;     /**< hexadecimal blob object id */
	uint64_t    bytes;
};

/** A change to a note in the output of NOTES_LIST. */
struct notes_entry {
	struct span name;
	struct span commit; /**< hexadecimal commit id; borrows the history output */
	uint64_t    time;   /**< when it was committed, in seconds since the
	                         epoch */
	uint64_t    lines;  /**< the number of lines added by this change, or
	                         NOTES_BINARY if git sees no text in it */
};

/** Reverse iterator over Git's note history. Initialize only end to the output
 * size. The cached commit boundary avoids rescanning multi-file commits. */
struct notes_cursor {
	struct span commit;
	size_t      end;
	size_t      first;
	uint64_t    time;
};

/** The lines of a note that git sees as binary (struct notes_entry). */
static constexpr uint64_t NOTES_BINARY = UINT64_MAX;

/** Opens the shared session repository at @p path for @p participant. An
 * empty directory is initialized lazily on its first note. A nonempty directory
 * that is not a repository is refused, preserving the old per-thread layout.
 * Returns nullptr and an error number and message on failure. */
extern struct notes *
notes_create (char const *path,
              char const *participant,
              char       *why,
              size_t      len,
              int        *e);

extern void
notes_destroy (struct notes **p_notes);

/** Returns the environment that git runs in. */
extern char *const *
notes_env (struct notes const *notes);

/** The canonical Git common directory, resolving worktree indirections. */
extern char const *
notes_directory (struct notes const *notes);

/** The configured writer identity, shared by this instance's notebooks. */
extern char const *
notes_writer (struct notes const *notes);

/** Writes the id of a new thread, 128 random bits as hexadecimal digits,
 *  to @p id, with a NUL after it.  Returns 0, or the error number if the
 *  kernel gave no random bits. */
extern int
notes_id (char id[NOTES_ID + 1]);

/** Returns true if the @p len bytes at @p s are the id of a thread. */
extern bool
notes_is_id (char const *s,
             size_t      len);

/** Whether @p s is a full, lowercase SHA-1 or SHA-256 Git object id. */
extern bool
notes_is_revision (char const *s,
                   size_t      len);

/** Writes the caller's server-selected branch ref to @p repo. */
extern void
notes_repo (struct notes const *notes,
            uint64_t            principal,
            enum notes_side     side,
            char                repo[NOTES_REPO]);

/** Validates an explicit PARTICIPANT/WHO/SIDE read selector and writes its
 * full branch ref to @p repo. Returns 0 or EINVAL. */
extern int
notes_select (char const *name,
              size_t      len,
              char        repo[NOTES_REPO]);

/** Returns the public selector within a ref made by notes_repo() or
 * notes_select(). The view borrows @p repo. */
extern char const *
notes_selector (char const *repo);

/** Writes to @p *age how many seconds ago the lock file of the notebook
 *  branch @p repo was last changed, or 0 if it has none.  A lock file last
 *  changed more than NOTES_STALE before the system booted belongs to no
 *  running process, as a crash can leave it, and is removed instead; the
 *  margin keeps a step of the clock from making a live lock look older than
 *  the boot.  Any other lock file stays, as its writer may be outside
 *  askai.  Returns 0 or an error number. */
extern int
notes_lock (struct notes const *notes,
            char const         *repo,
            int64_t            *age);

/** Whether the shared repository has been initialized. Branch existence is
 * resolved by Git, including packed refs. */
extern bool
notes_exists (struct notes const *notes);

/** Writes the argv of the command @p cmd for the notebook branch @p repo to
 *  @p argv.  @p repo must outlive @p argv. */
extern void
notes_argv (struct notes const *notes,
            char const         *repo,
            enum notes_cmd      cmd,
            char const         *argv[NOTES_ARGV]);

/** Owns a historical read or diff's arguments and authorization state.
 *  File names follow notes_name(); revisions are full lowercase commit ids
 *  or "current". Empty revisions also select the notebook's pinned tip.
 *  Returns nullptr and EINVAL or ENOMEM through @p e on failure. */
extern struct notes_revision *
notes_revision_create (struct cstr_view file,
                       struct cstr_view from,
                       struct cstr_view to,
                       int             *e);

extern void
notes_revision_destroy (struct notes_revision **p_revision);

/** Builds the current stage's Git argv: pin the selected notebook tip,
 *  authorize explicit revisions by ancestry, then read or diff those exact
 *  snapshots. @p revision and @p repo must outlive the argv. */
extern void
notes_revision_argv (struct notes_revision const *revision,
                     struct notes const          *notes,
                     char const                  *repo,
                     bool                         diff,
                     char const                  *argv[NOTES_ARGV]);

/** Lists only this literal filename's history, before Git's output is capped. */
extern void
notes_revision_history_argv (struct notes_revision const *revision,
                             struct notes const          *notes,
                             char const                  *repo,
                             char const                  *argv[NOTES_ARGV]);

/** Consumes a successful authorization stage's complete output and advances
 *  to the next stage. Never call after failed, cancelled or truncated Git
 *  execution. Returns 0, EIO for malformed output, or EINVAL for an object-id
 *  width that does not match the repository or if already ready. */
extern int
notes_revision_next (struct notes_revision *revision,
                     struct span            out);

/** Whether the next command reads the authorized snapshots. */
extern bool
notes_revision_ready (struct notes_revision const *revision);

extern struct cstr_view
notes_revision_file (struct notes_revision const *revision);

/** Immutable ids after authorization; views borrow @p revision. The pinned
 *  tip can also guard a later write against changes since authorization. */
extern char const *
notes_revision_from (struct notes_revision const *revision);

extern char const *
notes_revision_to (struct notes_revision const *revision);

extern char const *
notes_revision_tip (struct notes_revision const *revision);

/**
 * Checks the name of a note, the @p len bytes at @p name.  Slashes at the
 * start do not belong to the name, and the function stores their number in
 * @p *skip.  The rest is a valid name if it has 1 to NOTES_NAME bytes and
 * no control characters, and if none of its parts between slashes is
 * empty, ".", "..", or a name that NTFS would read as ".git".  git refuses
 * those names: ".git" or "git~1" in any case, followed only by spaces and
 * dots, also before a colon or a backslash.  Returns 0 for a valid name,
 * else EINVAL.
 */
extern int
notes_name (char const *name,
            size_t      len,
            size_t     *skip);

/** Writes the input of the command NOTES_CHECK for the valid name of
 *  @p len bytes at @p name to @p in.  Returns 0 or ENOMEM. */
extern int
notes_check_input (struct buf *in,
                   char const *repo,
                   char const *name,
                   size_t      len);

/**
 * Reads @p out, the output of the command NOTES_CHECK. If the branch has
 * notes, the function writes the name of the newest commit to @p tip, else
 * an empty string.  Returns 0 if a note of the name can be added.  Returns
 * EEXIST if a note of the name exists, EISDIR if notes are in a directory
 * of the name, ENOTDIR if a directory in the name is a note, and EIO if the
 * output is not what the command writes.
 */
extern int
notes_checked (struct span out,
               char        tip[NOTES_TIP + 1]);

/**
 * Writes to @p in the head of the file part of the input of the command
 * NOTES_ADD, for a note of the @p name_len bytes at @p name whose text has
 * @p len bytes, and reserves room for the text: the caller appends the
 * text and then a line feed, as notes_add_file() does.  Returns 0, EINVAL
 * for a null argument or a name longer than NOTES_NAME, or ENOMEM.
 */
extern int
notes_add_head (struct buf *in,
                char const *name,
                size_t      name_len,
                size_t      len);

/**
 * Writes to @p in the file part of the input of the command NOTES_ADD.  The
 * valid name of the note is the @p name_len bytes at @p name, and its text
 * is the @p len bytes at @p text.  notes_add_input() puts the commit before
 * the file part.  Returns 0, EINVAL for a name longer than NOTES_NAME, or
 * ENOMEM.
 */
extern int
notes_add_file (struct buf *in,
                char const *name,
                size_t      name_len,
                char const *text,
                size_t      len);

/**
 * Writes to @p in the input of the command NOTES_ADD.  The input is a
 * commit by the side @p side with @p file, the part that notes_add_file()
 * wrote. A file replaces any existing note with the same name. The parent
 * is @p tip, or a zero object id of the repository format for a root commit.
 * An empty tip means the SHA-1 zero object id.
 * Returns 0 or ENOMEM.
 */
extern int
notes_add_input (struct buf      *in,
                 char const      *repo,
                 enum notes_side  side,
                 char const      *tip,
                 struct span      file);

/** Writes the input of the command NOTES_READ for the valid name of @p len
 *  bytes at @p name to @p in.  Returns 0 or ENOMEM. */
extern int
notes_read_input (struct buf *in,
                  char const *repo,
                  char const *name,
                  size_t      len);

/**
 * Reads @p out, the output of the command NOTES_READ, and stores the text
 * of the note in @p text, and its object id, in hexadecimal, in @p id
 * unless @p id is nullptr.  Returns 0, ENOENT if no note has the name,
 * EISDIR if the name is a directory of notes, and EIO if the output is not
 * what the command writes.
 */
extern int
notes_read_out (struct span  out,
                struct span *text,
                struct span *id);

/** As notes_read_out(), but rejects incomplete output with EIO. Use this
 *  before copying or restoring bytes rather than displaying a partial read. */
extern int
notes_read_full_out (struct span  out,
                     struct span *text,
                     struct span *id);

/**
 * Reads the next current note from @p out, the output of NOTES_FILES, into
 * @p file, consuming its complete record. Returns 0, ENOENT at the end, or
 * EIO for an incomplete or malformed record; errors leave @p out unchanged.
 */
extern int
notes_file_next (struct span       *out,
                 struct notes_file *file);

/**
 * Reads a note in @p out, the output of NOTES_LIST, into @p entry and
 * advances @p cursor backwards. Start with cursor.end = out.len and its other
 * fields zero. Changes come oldest first, including all files of a multi-file
 * commit. Incomplete records at a cut end are skipped. Returns false at end.
 */
extern bool
notes_entry_before (struct span          out,
                    struct notes_cursor *cursor,
                    struct notes_entry  *entry);

#endif /* TWINCEPTION_SRC_NOTES_H_ */
