/** @file The notes (--notes): a git repository in which the upstream keeps
 *  notes for itself as it works, when askai lends it tools (src/mcp.h).
 *
 * Each thread of work has a branch, refs/heads/WHO/ID: WHO the principal of
 * the client whose calls it serves (src/policy.h), as 16 hexadecimal digits,
 * or "anonymous"; ID 32 hexadecimal digits, random, which askai makes as an
 * ask call begins and names in its answer, so that the client may go on
 * with the thread in a later call.  A note is a commit, its message the
 * note's text, its tree empty: git fast-import writes it, and git log reads
 * the notes back.  Both run as children (src/child.h), in a fixed
 * environment as the git tool's do, in askai's own repository, which askai
 * makes bare if it does not exist.  Two calls may write to one thread at
 * once: one of them may then fail, its note not written.
 */
#ifndef TWINCEPTION_SRC_NOTES_H_
#define TWINCEPTION_SRC_NOTES_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"

struct notes;

enum {
	NOTES_TEXT   = 8 << 10,  /**< bytes of a note, at most */
	NOTES_SEARCH = 1 << 10,  /**< bytes of what a read searches for */
	NOTES_READ   = 256,      /**< notes a read gives back, the newest */
	NOTES_MAX    = 1 << 20,  /**< bytes of git's output, at most: the
	                              newest notes it reads, the oldest of
	                              which may be cut off */
	NOTES_ID     = 32,       /**< hexadecimal digits of a thread's id */
	NOTES_REF    = 64,       /**< bytes of a thread's ref, its NUL included */
	NOTES_ARGV   = 24,       /**< a command's argv, its nullptr included */
};

/** From the start of a git that writes or reads notes to its deadline. */
static constexpr uint64_t NOTES_NS = 10'000'000'000;

/**
 * The notes repository at @p path, a bare one, made so if nothing is there;
 * nullptr, with the error in @p *e, if it is not one or cannot be made,
 * @p why, of @p len bytes, saying so.  Runs git and waits for it: for
 * startup.
 */
extern struct notes *
notes_create (char const *path,
              char       *why,
              size_t      len,
              int        *e);

extern void
notes_destroy (struct notes **p_notes);

/** The environment git runs in. */
extern char *const *
notes_env (struct notes const *notes);

/** A new thread's id, NUL-terminated, in @p id, of 128 random bits;
 *  returns 0, or why the kernel gave none. */
extern int
notes_id (char id[NOTES_ID + 1]);

/** Whether the @p len bytes at @p s are a thread's id. */
extern bool
notes_is_id (char const *s,
             size_t      len);

/** The ref of thread @p id, of the client whose principal is @p principal,
 *  in @p ref. */
extern void
notes_ref (uint64_t    principal,
           char const *id,
           char        ref[NOTES_REF]);

/** In @p argv, the git that writes a note, its input notes_input()'s;
 *  @p ref must outlive it. */
extern void
notes_write (struct notes const *notes,
             char const         *argv[NOTES_ARGV]);

/**
 * Write into @p in what has git fast-import commit a note of the @p len
 * bytes of UTF-8 text at @p text, with no NUL, to @p ref, on its tip if
 * @p after, else as its first; returns 0 or ENOMEM.
 */
extern int
notes_input (struct buf *in,
             char const *ref,
             char const *text,
             size_t      len,
             bool        after);

/**
 * In @p argv, the git that reads back the newest NOTES_READ notes of
 * @p ref, newest first, each ending in a NUL; of those, only the ones that
 * hold @p search, a line, regardless of case, if it is not nullptr.  @p ref
 * and @p search must outlive it.
 */
extern void
notes_read (struct notes const *notes,
            char const         *ref,
            char const         *search,
            char const         *argv[NOTES_ARGV]);

#endif /* TWINCEPTION_SRC_NOTES_H_ */
