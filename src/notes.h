/** @file notes.h
 *
 * The notes of the session. --notes names one git repository, the session
 * of every askai that names it (src/member.h). Each member of the session
 * has one notebook, the branch refs/heads/KIND/NAME/notes, where KIND is
 * person or agent and NAME is the name of the member. Every member can
 * read every notebook, and only the member writes its notebook. Each write
 * that changes a note adds a commit whose message says what the write did.
 * Earlier contents stay in the history of the notebook. A write that
 * leaves the bytes of a note as they are adds no commit. A repository can
 * have worktrees, and git resolves their packed refs. Concurrent changes
 * to one branch fail rather than overwrite another writer. The review
 * record of a thread is a commit on refs/review/AGENT/WHO/THREAD, outside
 * the notebooks. The repository is a store of src/store.h.
 */
#ifndef ASKAI_SRC_NOTES_H_
#define ASKAI_SRC_NOTES_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "cstr_view.h"
#include "member.h"
#include "oid.h"
#include "store.h"
#include "util.h"
#include "xlist.h"

struct hexid;

enum {
	NOTES_NAME = STORE_PATH, /**< bytes of the name of a note, at most */
	NOTES_REF  = 128,        /**< bytes of the ref of a notebook or of a
	                              review record, NUL included */
};

/** The actions of the notes tool: X(ID, name, flags, fetch), the constant
 *  NOTES_ACTION_ID of enum notes_action, the name, and the flags and the
 *  fetch value of the action, which only notes_tool.c uses. Every caller
 *  can use the actions of NOTES_COMMON_ACTION_LIST, the first
 *  NOTES_ACTION_ATTACH actions of NOTES_ALL_ACTION_LIST. Only a turn whose
 *  upstream takes files can use attach. The commit message of a write
 *  begins with the name, in upper case at the first letter.
 */
#define NOTES_COMMON_ACTION_LIST(X)                                                                               \
        X(ADD,     "add",     NOTE_AUTHOR | NOTE_WRITE | NOTE_FILE,                 0)                            \
        X(APPEND,  "append",  NOTE_AUTHOR | NOTE_WRITE | NOTE_FILE,                 FETCH_REPLACE | FETCH_APPEND) \
        X(REPLACE, "replace", NOTE_AUTHOR | NOTE_WRITE | NOTE_FILE,                 FETCH_REPLACE)                \
        X(DELETE,  "delete",  NOTE_AUTHOR | NOTE_WRITE | NOTE_FILE,                 FETCH_REPLACE | FETCH_DELETE) \
        X(RESTORE, "restore", NOTE_AUTHOR | NOTE_WRITE | NOTE_FILE | NOTE_REVISION, FETCH_READ | FETCH_RESTORE)   \
        X(MEMBERS, "members", NOTE_OFFSET,                                          FETCH_MEMBERS)                \
        X(LIST,    "list",    NOTE_OFFSET,                                          FETCH_FILES)                  \
        X(SEARCH,  "search",  NOTE_OFFSET,                                          FETCH_GREP)                   \
        X(HISTORY, "history", NOTE_OFFSET,                                          FETCH_LIST)                   \
        X(READ,    "read",    NOTE_FILE | NOTE_OFFSET | NOTE_REVISION,              FETCH_READ)                   \
        X(DIFF,    "diff",    NOTE_FILE | NOTE_OFFSET,                              FETCH_DIFF)                   \
        X(COPY,    "copy",    NOTE_AUTHOR | NOTE_FILE,                              FETCH_READ | FETCH_COPY)
#define NOTES_ALL_ACTION_LIST(X) NOTES_COMMON_ACTION_LIST(X)                                                      \
        X(ATTACH,  "attach",  NOTE_FILE,                                            FETCH_READ | FETCH_ATTACH)

enum notes_action : uint8_t {
#define NOTES_ACTION_ENUM(id, ...) NOTES_ACTION_##id,
	NOTES_ALL_ACTION_LIST(NOTES_ACTION_ENUM)
#undef NOTES_ACTION_ENUM
};

/** The names of the actions. Each is NUL-terminated. */
static struct cstr_view const notes_action_names[] = { NOTES_ALL_ACTION_LIST(XL_VIEW) };

/** A current note in the output of STORE_FILES or STORE_CHECK. Views
 *  borrow that output. From STORE_CHECK, the name is empty.
 */
struct notes_file {
	struct span name;
	uint64_t    bytes;
	struct oid  id;     /**< the name of the blob */
};

/** A change to a note in the output of STORE_LOG. */
struct notes_entry {
	struct span name;
	struct span subject; /**< the commit's subject; borrows the history output */
	struct span asker;   /**< the trailer line Asked-by of the commit, or
	                          empty; borrows the history output */
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
	struct span asker;
	size_t      end;
	size_t      first;
	uint64_t    time;
	struct oid  commit;
};

/** The lines of a note that git sees as binary (struct notes_entry). */
static constexpr uint64_t NOTES_BINARY = UINT64_MAX;

/** Writes to @p ref the ref of the notebook of @p member, a member with a
 *  name: refs/heads/KIND/NAME/notes.
 */
extern void
notes_ref (struct member const *member,
           char                 ref[NOTES_REF]);

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

/** Writes to @p words the words of the command of the current stage, and
 *  returns the command: pin the tip of the notebook branch @p repo
 *  (STORE_HEAD), authorize explicit revisions by ancestry
 *  (STORE_ANCESTOR), then run @p cmd, STORE_READ, STORE_DIFF, STORE_STAT or
 *  STORE_GREP, on those exact snapshots. The words point into @p revision
 *  and @p repo.
 */
extern enum store_cmd
notes_revision_words (struct notes_revision const *revision,
                      char const                  *repo,
                      enum store_cmd               cmd,
                      char const                  *words[static STORE_WORDS]);

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

/** Writes the input of the command STORE_CHECK for the valid name @p name
 *  in the notebook branch @p repo to @p in. Returns 0, EINVAL for a null
 *  argument, or the error number that buf_printf() returns.
 */
extern int
notes_check_input (struct buf       *in,
                   char const       *repo,
                   struct cstr_view  name);

/**
 * Reads @p out, the output of the command STORE_CHECK. If the branch has
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
 * Returns 0, EINVAL for a null argument, a file without an id or a failed
 * snprintf() of the size, or EIO if the digest fails.
 */
extern int
notes_file_same (struct notes_file const *file,
                 struct span              data,
                 bool                    *same);

/** Writes the input of the command STORE_READ for the valid name @p name
 *  in the notebook branch or commit @p repo to @p in. Returns 0, EINVAL
 *  for a null argument, or the error number that buf_printf() returns.
 */
extern int
notes_read_input (struct buf       *in,
                  char const       *repo,
                  struct cstr_view  name);

/**
 * Reads @p out, the output of the command STORE_READ, and stores the text
 * of the note in @p text, and its object id in @p id unless @p id is
 * nullptr. Returns 0, ENOENT if no note has the name,
 * EISDIR if the name is a directory of notes, and EIO if the output is not
 * what the command writes, also if it is incomplete.
 */
extern int
notes_read_out (struct span  out,
                struct span *text,
                struct oid  *id);

/** Writes to @p ref the ref of the review record of @p thread for the
 *  agent @p agent and the principal @p principal of the client of the ask
 *  call (src/policy.h): refs/review/AGENT/WHO/THREAD, where WHO is the
 *  principal in 16 hexadecimal digits, or anonymous for 0.
 */
extern void
notes_review (char const         *agent,
              uint64_t            principal,
              struct hexid const *thread,
              char                ref[NOTES_REF]);

/**
 * Reads @p out, the output of the command STORE_READ for the input that
 * notes_check_input() writes for a ref of notes_review() and the name
 * review.json. Stores the commit of the ref in @p tip and the text of
 * review.json in @p text. Returns 0, ENOENT if the ref does not exist, or
 * EIO for any other output, such as a ref that is not a commit or a commit
 * without review.json.
 */
extern int
notes_review_out (struct span  out,
                  struct oid  *tip,
                  struct span *text);

/**
 * Reads the next current note from @p out, the output of STORE_FILES, into
 * @p file, consuming its complete record. Returns 0, ENOENT at the end, or
 * EIO for an incomplete or malformed record; errors leave @p out unchanged.
 */
extern int
notes_file_next (struct span       *out,
                 struct notes_file *file);

/**
 * Reads a note in @p out, the output of STORE_LOG, into @p entry and
 * advances @p cursor backwards. Start with cursor.end = out.len and its other
 * fields zero. Changes come oldest first, including all files of a multi-file
 * commit. Incomplete records at a cut end are skipped. Returns false at end.
 */
extern bool
notes_entry_before (struct span          out,
                    struct notes_cursor *cursor,
                    struct notes_entry  *entry);

#endif /* ASKAI_SRC_NOTES_H_ */
