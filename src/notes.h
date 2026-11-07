/** @file notes.h
 *
 * Shared session notes. --notes names one Git repository, shared by
 * every participating askai instance. Each configured participant writes its
 * own refs/heads/notes/PARTICIPANT/WHO/SIDE branches. WHO is the client principal
 * or anonymous; SIDE is client or upstream. All notebooks can be read, but a
 * tool call's write branch is chosen by the server. Threads identify ask chains
 * and do not partition the notes. Each write that changes a note adds a
 * commit whose message says what the write did; earlier contents remain in
 * the notebook's history. A write that leaves a note's bytes as they are
 * adds no commit.
 * Existing repositories can have worktrees; Git resolves their packed refs.
 * Concurrent changes to one branch fail rather than overwrite another writer.
 */
#ifndef ASKAI_SRC_NOTES_H_
#define ASKAI_SRC_NOTES_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "clk.h"
#include "cstr_view.h"
#include "oid.h"
#include "util.h"

struct notes;

enum {
	NOTES_NAME = 255,      /**< bytes of the name of a note, at most */
	NOTES_MAX  = 16 << 20, /**< bytes of the output of git, at most */
	NOTES_REPO = 128,      /**< bytes of a full notebook branch ref, NUL included */
	NOTES_ARGV = 40,       /**< elements of the argv of git, nullptr
	                            included */
	NOTES_MESSAGE = 1024,  /**< bytes of the commit message of a note
	                            write, NUL included, at most */
};

/** The time from the start of git to its deadline, in nanoseconds. */
static constexpr uint64_t NOTES_NS = 10 * NS;

/** The age in seconds past which a lock file of a notebook has outlived
 *  every write of askai, so that its writer has died or hangs.
 */
static constexpr int64_t NOTES_STALE = 60;

/** The two authors within a participant and principal. */
enum notes_side : uint8_t {
	NOTES_UPSTREAM,  /**< the upstream in ask calls */
	NOTES_CLIENT,    /**< a client */
};

/** The commands of git that write and read the notes. */
enum notes_cmd : uint8_t {
	NOTES_CREATE,    /**< initializes the shared repository */
	NOTES_CHECK,     /**< checks the name of a new note. notes_check_input()
	                      writes its input. */
	NOTES_ADD,       /**< writes a note. notes_add_input() writes its
	                      input. */
	NOTES_LIST,      /**< writes the note history, the newest first */
	NOTES_READ,      /**< writes a note. notes_read_input() writes its
	                      input. */
	NOTES_BOOKS,     /**< lists all participant notebook selectors */
	NOTES_FILES,     /**< lists the current notes, in tree order */
	NOTES_HEAD,      /**< resolves a notebook branch to its full commit id */
	NOTES_ANCESTOR,  /**< checks that a commit is an ancestor of another */
	NOTES_DIFF,      /**< compares a note in two commits */
	NOTES_STAT,      /**< counts the lines that one commit changed against
	                      another */
	NOTES_GREP,      /**< finds the lines of the current notes that match
	                      the patterns of its input */
};

/** A current note in the output of NOTES_FILES or NOTES_CHECK. Views
 *  borrow that output. From NOTES_CHECK, the name is empty.
 */
struct notes_file {
	struct span name;
	uint64_t    bytes;
	struct oid  id;     /**< the name of the blob */
};

/** The offsets of the name and the text of a note in the buffer that
 *  notes_add_head() writes to, from the head of the buffer. The name
 *  includes its quotes.
 */
struct notes_part {
	size_t name_at;
	size_t name_len;
	size_t data_at;
	size_t data_len;
};

/** A change to a note in the output of NOTES_LIST. */
struct notes_entry {
	struct span name;
	struct span subject; /**< the commit's subject; borrows the history output */
	uint64_t    time;    /**< when it was committed, in seconds since the
	                          epoch */
	uint64_t    added;   /**< the number of lines added by this change, or
	                          NOTES_BINARY if git sees no text in it */
	uint64_t    removed; /**< the number of lines removed by it */
	struct oid  commit;
};

/** Reverse iterator over Git's note history. Initialize only end to the output
 * size. The cached commit boundary avoids rescanning multi-file commits.
 */
struct notes_cursor {
	struct span subject;
	size_t      end;
	size_t      first;
	uint64_t    time;
	struct oid  commit;
};

/** The lines of a note that git sees as binary (struct notes_entry). */
static constexpr uint64_t NOTES_BINARY = UINT64_MAX;

/** Opens the shared session repository at @p path for @p participant. An
 * empty directory is initialized lazily on its first note. A nonempty directory
 * that is not a repository is refused, preserving the old per-thread layout.
 * Returns nullptr and an error number and message on failure.
 */
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

/** Writes to @p repo the branch ref of the notebook of @p participant, a
 *  name that util_word() accepts, for the principal @p principal and the
 *  side @p side.
 */
extern void
notes_book (struct cstr_view participant,
            uint64_t         principal,
            enum notes_side  side,
            char             repo[NOTES_REPO]);

/** Writes the caller's server-selected branch ref to @p repo. */
extern void
notes_repo (struct notes const *notes,
            uint64_t            principal,
            enum notes_side     side,
            char                repo[NOTES_REPO]);

/** Validates an explicit PARTICIPANT/WHO/SIDE read selector and writes its
 * full branch ref to @p repo, unless @p repo is nullptr. Returns 0 or
 * EINVAL.
 */
extern int
notes_select (struct cstr_view name,
              char             repo[NOTES_REPO]);

/** Returns the public selector within a ref made by notes_book(),
 * notes_repo() or notes_select(). The view borrows @p repo.
 */
extern char const *
notes_selector (char const *repo);

/** Writes to @p *age how many seconds ago the lock file of the notebook
 *  branch @p repo was last changed, or 0 if it has none. A lock file last
 *  changed more than NOTES_STALE before the system booted belongs to no
 *  running process, as a crash can leave it, and is removed instead; the
 *  margin keeps a step of the clock from making a live lock look older than
 *  the boot. Any other lock file stays, as its writer may be outside
 *  askai. Returns 0 or an error number.
 */
extern int
notes_lock (struct notes const *notes,
            char const         *repo,
            int64_t            *age);

/** Whether the shared repository has been initialized. Branch existence is
 * resolved by Git, including packed refs.
 */
extern bool
notes_exists (struct notes const *notes);

/** Writes the argv of the command @p cmd to @p argv. The arguments of the
 *  command come from @p words, in this order: for NOTES_LIST a notebook
 *  branch or commit and a file name or nullptr, for NOTES_FILES and
 *  NOTES_HEAD a notebook branch or commit, for NOTES_ANCESTOR two commits,
 *  for NOTES_DIFF two commits and a file name, for NOTES_STAT two commits,
 *  and for NOTES_GREP a commit. The other commands do not read @p words,
 *  which may then be nullptr. The words must outlive @p argv.
 */
extern void
notes_argv (struct notes const *notes,
            enum notes_cmd      cmd,
            char const *const  *words,
            char const         *argv[NOTES_ARGV]);

/** The stage of a historical read or diff (notes.c). */
enum notes_revision_phase : uint8_t;

/** The arguments and the authorization stage of a historical read or diff.
 *  After the first stage, from, to and tip are the commits that the later
 *  stages check, read and compare, and the pinned tip can also guard a
 *  later write against changes since the authorization.
 */
struct notes_revision {
	UTIL_TEXT(NOTES_NAME + 1)  file;   /**< the name of the note */
	struct oid                 from;   /**< the older commit, or no name
	                                        for the tip */
	struct oid                 to;     /**< the newer commit, or no name for
	                                        the tip */
	struct oid                 tip;    /**< the notebook's tip, which the
	                                        first stage pins */
	enum notes_revision_phase  phase;
};

/** Initializes @p dest for a read or diff of the note @p file, whose name
 *  notes_name() accepts. The revisions @p from and @p to are commits, or no
 *  name for the notebook's pinned tip. Returns 0, or EINVAL for a null
 *  argument or a name that is not valid.
 */
extern int
notes_revision_init (struct notes_revision *dest,
                     struct cstr_view       file,
                     struct oid const      *from,
                     struct oid const      *to);

/** Sets @p revision to the stage that runs its command on the commits
 *  @p from and @p to, without the stages that check them. askai wrote or
 *  checked both commits itself.
 */
extern void
notes_revision_pin (struct notes_revision *revision,
                    struct oid const      *from,
                    struct oid const      *to);

/** Builds the current stage's Git argv: pin the selected notebook tip,
 *  authorize explicit revisions by ancestry, then run @p cmd, NOTES_READ,
 *  NOTES_DIFF, NOTES_STAT or NOTES_GREP, on those exact snapshots.
 *  @p revision and @p repo must outlive the argv.
 */
extern void
notes_revision_argv (struct notes_revision const *revision,
                     struct notes const          *notes,
                     char const                  *repo,
                     enum notes_cmd               cmd,
                     char const                  *argv[NOTES_ARGV]);

/** Consumes a successful authorization stage's complete output and advances
 *  to the next stage. Never call after failed, cancelled or truncated Git
 *  execution. Returns 0, EIO for malformed output, or EINVAL for an object-id
 *  width that does not match the repository or if already ready.
 */
extern int
notes_revision_next (struct notes_revision *revision,
                     struct span            out);

/** Whether the next command reads the authorized snapshots. */
extern bool
notes_revision_ready (struct notes_revision const *revision);

/**
 * Checks @p *name as the name of a note. Slashes at the start do not belong
 * to the name, and the function drops them from @p *name. The rest is a
 * valid name if it is 1 to NOTES_NAME bytes long, contains no control
 * characters, and if none of its parts between slashes is empty, ".", "..",
 * or a name that NTFS would read as ".git". git refuses those names: ".git"
 * or "git~1" in any case, followed only by spaces and dots, also before a
 * colon or a backslash. Returns 0 for a valid name, else EINVAL.
 */
extern int
notes_name (struct cstr_view *name);

/** Writes the input of the command NOTES_CHECK for the valid name @p name
 *  in the notebook branch @p repo to @p in. Returns 0, EINVAL for a null
 *  argument, or ENOMEM.
 */
extern int
notes_check_input (struct buf       *in,
                   char const       *repo,
                   struct cstr_view  name);

/**
 * Reads @p out, the output of the command NOTES_CHECK. If the branch has
 * notes, the function writes the name of the newest commit to @p tip, else
 * no name. If a note has the name, it writes the note's blob id and size
 * to @p note, else it zeroes @p note. Returns 0 if a note of the
 * name can be added. Returns EEXIST if a note of the name exists, EISDIR
 * if notes are in a directory of the name, ENOTDIR if a directory in the
 * name is a note, EIO if the output is not what the command writes, and
 * EINVAL for a null @p tip or @p note.
 */
extern int
notes_checked (struct span        out,
               struct oid        *tip,
               struct notes_file *note);

/**
 * Sets @p *same to whether @p data are the bytes of the blob whose id and
 * size @p file has. It hashes the bytes only if their number is the size.
 * Returns 0, EINVAL for a null argument or a file without an id, or EIO
 * if the digest fails.
 */
extern int
notes_file_same (struct notes_file const *file,
                 struct span              data,
                 bool                    *same);

/**
 * Writes to @p in the head of the file part of the input of the command
 * NOTES_ADD, for a note with the name @p name whose text is @p len bytes
 * long. After the head, the function adds @p len bytes for the text, which
 * it leaves unwritten, and a line feed. It stores the offsets of the name
 * and the text in @p *part. The caller then writes the text at the offset
 * part->data_at from the head of @p in, as notes_add_file() does. Returns
 * 0, EINVAL for a null argument or a name longer than NOTES_NAME, or
 * ENOMEM.
 */
extern int
notes_add_head (struct buf        *in,
                struct cstr_view   name,
                size_t             len,
                struct notes_part *part);

/**
 * Writes to @p in the file part of the input of the command NOTES_ADD. The
 * valid name of the note is @p name, and its text is @p text.
 * notes_add_input() puts the commit before the file part. The function
 * fills @p *part as notes_add_head() does. Returns 0, EINVAL for a name
 * longer than NOTES_NAME, or ENOMEM.
 */
extern int
notes_add_file (struct buf        *in,
                struct cstr_view   name,
                struct cstr_view   text,
                struct notes_part *part);

/**
 * Writes to @p in the part of the input of the command NOTES_ADD that
 * deletes the note of the valid name @p name. It stores the offset of the
 * name in @p *part, as notes_add_head() does, and no text. Returns 0,
 * EINVAL for a null argument or a name longer than NOTES_NAME, or ENOMEM.
 */
extern int
notes_delete_file (struct buf        *in,
                   struct cstr_view   name,
                   struct notes_part *part);

/**
 * Writes to @p in the input of the command NOTES_ADD. The input is a
 * commit with the message @p message, one line without its line feed, and
 * with @p file, the part that notes_add_file() wrote. The committer's name
 * is the selector of the notebook branch @p repo. A file replaces any
 * existing note with the same name. The parent is @p tip. A @p tip without
 * a name makes a root commit, which git refuses if the branch exists.
 * notes_add_out() reads the new commit's id from the output. Returns 0,
 * EINVAL for a null argument, a NUL in @p message or a message of
 * NOTES_MESSAGE bytes or more, or the error of buf_printf() or buf_put(),
 * after which @p in can contain part of the input.
 */
extern int
notes_add_input (struct buf       *in,
                 char const       *repo,
                 struct oid const *tip,
                 struct span       message,
                 struct span       file);

/**
 * Reads @p out, the output of the command NOTES_ADD, and writes the id of
 * the new commit, its last line, to @p commit. Returns 0, EINVAL for a
 * null @p commit, or EIO with no name in @p commit if the last line is not
 * a commit id.
 */
extern int
notes_add_out (struct span  out,
               struct oid  *commit);

/** Writes the input of the command NOTES_READ for the valid name @p name
 *  in the notebook branch or commit @p repo to @p in. Returns 0, EINVAL
 *  for a null argument, or ENOMEM.
 */
extern int
notes_read_input (struct buf       *in,
                  char const       *repo,
                  struct cstr_view  name);

/**
 * Reads @p out, the output of the command NOTES_READ, and stores the text
 * of the note in @p text, and its object id in @p id unless @p id is
 * nullptr. Returns 0, ENOENT if no note has the name,
 * EISDIR if the name is a directory of notes, and EIO if the output is not
 * what the command writes, also if it is incomplete.
 */
extern int
notes_read_out (struct span  out,
                struct span *text,
                struct oid  *id);

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

#endif /* ASKAI_SRC_NOTES_H_ */
