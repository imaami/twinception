/** @file This file declares the notes of --notes.  The upstream and the
 *  clients add notes to notebooks as they work (src/mcp.h).
 *
 * A notebook is one side's notes of a thread.  Each notebook is a bare git
 * repository in the directory that --notes names, at the path WHO/SIDE/ID.
 * WHO is the principal of the client (src/policy.h) as 16 hexadecimal
 * digits, or "anonymous".  SIDE is "upstream" for the notes that the
 * upstream writes in the turns of an ask call, and "client" for the notes
 * of the client.  ID is the id of the thread: 32 random hexadecimal digits.
 * A thread has up to two notebooks, one of each side, under one id.  Each
 * side adds notes only to its own notebook and reads both.  askai makes a
 * notebook's repository when its first note is added.
 *
 * A note is a file.  Its author chooses the name of the file and its text.
 * The name can have directories in it.  Adding a note adds one commit to
 * the branch "notes", and the commit adds the file.  No note is ever
 * changed or removed.  git fast-import writes the commits, and git log and
 * git cat-file read them.  They run as children (src/child.h) in a fixed
 * environment, in the directory that --notes names.  When two calls add
 * notes to one thread at once, one of the two can fail.
 */
#ifndef TWINCEPTION_SRC_NOTES_H_
#define TWINCEPTION_SRC_NOTES_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"

struct notes;

enum {
	NOTES_TEXT = 4 << 20,  /**< bytes of the text of a note, at most */
	NOTES_NAME = 255,      /**< bytes of the name of a note, at most */
	NOTES_MAX  = 16 << 20, /**< bytes of the output of git, at most */
	NOTES_ID   = 32,       /**< hexadecimal digits of the id of a thread */
	NOTES_REPO = 80,       /**< bytes of the argument of git that names the
	                            repository of a thread, NUL included */
	NOTES_TIP  = 64,       /**< hexadecimal digits of a commit's name, at
	                            most */
	NOTES_ARGV = 24,       /**< elements of the argv of git, nullptr
	                            included */
};

/** The time from the start of git to its deadline, in nanoseconds. */
static constexpr uint64_t NOTES_NS = 10'000'000'000;

/** The time from SIGTERM to SIGKILL at the deadline of a git that writes,
 *  in nanoseconds. */
static constexpr uint64_t NOTES_GRACE = 1'000'000'000;

/** The two kinds of threads. */
enum notes_side : uint8_t {
	NOTES_UPSTREAM,  /**< a thread of the upstream in ask calls */
	NOTES_CLIENT,    /**< a thread of a client */
};

/** The commands of git that write and read the notes. */
enum notes_cmd : uint8_t {
	NOTES_CREATE,  /**< makes the repository of a new thread */
	NOTES_CHECK,   /**< checks the name of a new note.  notes_check_input()
	                    writes its input. */
	NOTES_ADD,     /**< adds a note.  notes_add_input() writes its input. */
	NOTES_LIST,    /**< writes the list of the notes, the newest first */
	NOTES_READ,    /**< writes a note.  notes_read_input() writes its
	                    input. */
};

/** A note in the output of the command NOTES_LIST. */
struct notes_entry {
	struct span name;
	uint64_t    time;   /**< when it was added, in seconds since the
	                         epoch */
	uint64_t    lines;  /**< the number of lines of its text, or
	                         NOTES_BINARY if git sees no text in it */
};

/** The lines of a note that git sees as binary (struct notes_entry). */
static constexpr uint64_t NOTES_BINARY = UINT64_MAX;

/**
 * Creates the notes in the directory @p path.  If the directory does not
 * exist, the function makes it with the mode 0700.  Returns the notes.  On
 * failure it returns nullptr, stores the error number in @p *e, and writes
 * a message of at most @p len bytes to @p why.  A directory that is a git
 * repository is refused, because older versions of askai wrote all the
 * notes to one repository.
 */
extern struct notes *
notes_create (char const *path,
              char       *why,
              size_t      len,
              int        *e);

extern void
notes_destroy (struct notes **p_notes);

/** Returns the environment that git runs in. */
extern char *const *
notes_env (struct notes const *notes);

/** Writes the id of a new thread, 128 random bits as hexadecimal digits,
 *  to @p id, with a NUL after it.  Returns 0, or the error number if the
 *  kernel gave no random bits. */
extern int
notes_id (char id[NOTES_ID + 1]);

/** Returns true if the @p len bytes at @p s are the id of a thread. */
extern bool
notes_is_id (char const *s,
             size_t      len);

/** Writes to @p repo the argument of git that names the repository of a
 *  thread.  @p principal is the principal of the client, @p side is the
 *  side of the thread, and @p id is its id. */
extern void
notes_repo (uint64_t        principal,
            enum notes_side side,
            char const     *id,
            char            repo[NOTES_REPO]);

/** Whether the repository that @p repo names, which notes_repo() wrote,
 *  exists.  false for a null argument. */
extern bool
notes_exists (struct notes const *notes,
              char const         *repo);

/**
 * Removes the lock files of git that are older than a minute from the
 * repository that @p repo names.  git leaves a lock file behind if it is
 * killed while it writes, and the file makes every later write to the
 * repository fail.  No git of askai runs for a minute, so no git that runs
 * has a lock file of that age.
 */
extern void
notes_unlock (struct notes const *notes,
              char const         *repo);

/** Writes the argv of the command @p cmd for the repository @p repo to
 *  @p argv.  @p repo must outlive @p argv. */
extern void
notes_argv (struct notes const *notes,
            char const         *repo,
            enum notes_cmd      cmd,
            char const         *argv[NOTES_ARGV]);

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
                   char const *name,
                   size_t      len);

/**
 * Reads @p out, the output of the command NOTES_CHECK.  If the thread has
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
 * wrote.  The commit is on top of the commit @p tip if @p tip is not empty.
 * Returns 0 or ENOMEM.
 */
extern int
notes_add_input (struct buf      *in,
                 enum notes_side  side,
                 char const      *tip,
                 struct span      file);

/** Writes the input of the command NOTES_READ for the valid name of @p len
 *  bytes at @p name to @p in.  Returns 0 or ENOMEM. */
extern int
notes_read_input (struct buf *in,
                  char const *name,
                  size_t      len);

/**
 * Reads @p out, the output of the command NOTES_READ, and stores the text
 * of the note in @p text.  Returns 0, ENOENT if no note has the name,
 * EISDIR if the name is a directory of notes, and EIO if the output is not
 * what the command writes.
 */
extern int
notes_read_out (struct span  out,
                struct span *text);

/**
 * Reads the note before the offset @p *end in @p out, the output of the
 * command NOTES_LIST, into @p entry, and sets @p *end to the offset of its
 * start.  @p *end starts at out.len, so the notes come oldest first.  An
 * incomplete note at the end of a cut output is skipped.  Returns false if
 * no note is before @p *end.
 */
extern bool
notes_entry_before (struct span         out,
                    size_t             *end,
                    struct notes_entry *entry);

#endif /* TWINCEPTION_SRC_NOTES_H_ */
