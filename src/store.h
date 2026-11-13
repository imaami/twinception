/** @file store.h
 *
 * The session repository that --notes names, as a git store. Every askai
 * instance with the same --notes path uses the same repository, and the
 * notes are branches in it. The functions of this module open the
 * repository, run git in it through git_run (src/git_run.h), write the
 * input of a commit for git fast-import, and recognize a write that raced
 * another writer. A git that writes ends before its lock file is
 * STORE_STALE seconds old. A lock file is removed only if it is older than
 * the boot of the system, never because of its age alone.
 */
#ifndef ASKAI_SRC_STORE_H_
#define ASKAI_SRC_STORE_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "cstr_view.h"
#include "git_run.h"
#include "oid.h"

struct child;
struct mcp_slot;
struct store;

enum {
	STORE_WORDS   = 3,                   /**< words of a command of enum
	                                          store_cmd, at most */
	STORE_MAX     = 16 << 20,            /**< bytes of the output of git,
	                                          at most */
	STORE_PATH    = 255,                 /**< bytes of the path of a file
	                                          in a commit, at most */
	STORE_QUOTED  = 2 * STORE_PATH + 2,  /**< bytes of a path that
	                                          store_quote() writes, at
	                                          most */
	STORE_MESSAGE = 1024,                /**< bytes of the message of a
	                                          commit without its trailer,
	                                          NUL included, at most */
	STORE_TRIES   = 3,                   /**< commits of one write that
	                                          races other writers, at
	                                          most */
	STORE_STALE   = 60,                  /**< seconds of age past which a
	                                          lock file of a ref is older
	                                          than every write of askai, so
	                                          that its writer died or
	                                          hangs */
	STORE_CAUSE   = 200,                 /**< bytes of the cause of a git
	                                          that failed (store_cause()),
	                                          at most */
};

/** The commands of git in the repository. The words of a command are the
 *  arguments that the caller of store_command() or store_spawn() passes,
 *  at most STORE_WORDS.
 */
enum store_cmd : uint8_t {
	STORE_CREATE,    /**< Runs git init to make the repository. The caller
	                      passes no words. */
	STORE_CHECK,     /**< Runs git cat-file --batch-check. The output lists
	                      the kind and the size of each object that the
	                      input names. The caller passes no words. */
	STORE_COMMIT,    /**< Runs git fast-import with the input that
	                      store_commit_input() writes. The caller passes no
	                      words. */
	STORE_LOG,       /**< Runs git log. The output lists the time, the id,
	                      the trailer lines Asked-by and the subject of each
	                      commit, and the number of lines added and deleted
	                      in each file. The caller passes a ref or a commit,
	                      then a path or nullptr. */
	STORE_CALLS,     /**< Runs git log -n 64. The output lists the id, the
	                      trailer lines Ask and Call and the subject of each
	                      of the newest 64 commits, separated by spaces, and
	                      a NUL after each commit. The caller passes a ref
	                      or a commit. */
	STORE_READ,      /**< Runs git cat-file --batch. The output contains
	                      each object that the input names. The caller
	                      passes no words. */
	STORE_REFS,      /**< Runs git for-each-ref. The caller passes the
	                      --format option, then one or two patterns. */
	STORE_FILES,     /**< Runs git ls-tree -r. The output lists the files of
	                      a tree in tree order, or the files under a path.
	                      The caller passes a ref or a commit, then a path
	                      or nullptr. */
	STORE_HEAD,      /**< Runs git rev-parse --verify. The output is the
	                      full id of a commit. The caller passes a ref or a
	                      commit. */
	STORE_REACH,     /**< Runs git rev-list --max-count=1 --stdin. Each
	                      line of the input is a commit, or a ^ and a
	                      commit. The output is the id of a commit that the
	                      commits without ^ reach and the commits with ^ do
	                      not, or empty if there is none. The caller passes
	                      no words. */
	STORE_DIFF,      /**< Runs git diff on a path. The caller passes two
	                      commits, then the path. */
	STORE_STAT,      /**< Runs git diff --numstat. The output lists the
	                      number of lines added and deleted in each file
	                      between two commits. The caller passes two
	                      commits. */
	STORE_GREP,      /**< Runs git grep -IniEz. The output lists the lines
	                      of the files that match the patterns in the input.
	                      The caller passes a commit. */
};

/** Opens the repository at @p path. An empty directory or a missing one
 *  becomes a bare repository when store_init() runs as askai starts. A
 *  nonempty directory that is not a repository is refused, so that askai
 *  never starts a session over the old per-thread layout. Returns the
 *  store, or returns nullptr with an error number in @p *e and a message
 *  in the @p len bytes at @p why.
 */
extern struct store *
store_create (char const *path,
              char       *why,
              size_t      len,
              int        *e);

/** Frees the store at @p *p_store and sets @p *p_store to nullptr. */
extern void
store_destroy (struct store **p_store);

/** Returns the canonical git common directory of @p store, also if
 *  --notes names an attached worktree.
 */
extern char const *
store_directory (struct store const *store);

/** Returns true if the repository exists. Returns false for a null
 *  @p store.
 */
extern bool
store_exists (struct store const *store);

/** Initializes @p dest with the git command that runs @p cmd in @p store
 *  with @p in as its input, which may be empty, as git_run_init() does.
 *  The words of the command come from @p words, in the order that enum
 *  store_cmd lists them. The function reads one word for each word of the
 *  command, at most STORE_WORDS, and does not read @p words for a command
 *  without words. A nullptr word ends the argv at that word. The output of
 *  git is cut at STORE_MAX bytes, and the deadline of git is GIT_RUN_NS
 *  after @p now, a time of loop_clock(). At the deadline of a command
 *  that writes, STORE_CREATE or STORE_COMMIT, git is sent SIGTERM, and
 *  SIGKILL after GIT_GRACE. The output of such a command includes the
 *  errors of git. Before STORE_COMMIT, the function runs store_lock() for
 *  @p ref, the ref that the commit updates, and logs an error of
 *  store_lock(). The argv of @p dest points to the paths of @p store and
 *  to the words, which must outlive @p dest. Returns 0, or EINVAL for a
 *  null @p store or @p dest, an unknown @p cmd, a null @p words for a
 *  command with words, or STORE_COMMIT without @p ref.
 */
extern int
store_command (struct store const *store,
               struct git_run     *dest,
               enum store_cmd      cmd,
               char const *const  *words,
               char const         *ref,
               struct span         in,
               uint64_t            now);

/** Spawns git for the call in @p slot, as mcp_slot_spawn() does, to run
 *  the command that store_command() writes for @p cmd, @p words, @p ref
 *  and @p in. The deadline of git is GIT_RUN_NS after the start of the
 *  turn of the loop of @p slot. The words, @p ref and @p in only need to
 *  last until the function returns. Returns the child, or returns nullptr
 *  with an error number in @p *e: EINVAL for a null @p slot or for the
 *  arguments that store_command() refuses, or the error of
 *  mcp_slot_spawn().
 */
extern struct child *
store_spawn (struct store const *store,
             struct mcp_slot    *slot,
             enum store_cmd      cmd,
             char const *const  *words,
             char const         *ref,
             struct span         in,
             int                *e);

/** Runs @p cmd in @p store on the calling thread, without a loop, as
 *  store_spawn() runs it for a call. The command is the one that
 *  store_command() writes for @p cmd, @p words, @p ref and @p in, with the
 *  lock check of @p ref before STORE_COMMIT, and the deadline of git is
 *  GIT_RUN_NS after the start of the run. child_capture() appends the
 *  output of git to @p out and writes how git ended to @p end. Returns
 *  what child_capture() returns: 0 if git exited before the deadline,
 *  whatever its exit status, ETIMEDOUT, EIO for a signal, EFBIG for an
 *  output longer than STORE_MAX bytes, or the error that stopped the spawn
 *  or the read. Returns EINVAL for a null @p out or @p end or for the
 *  arguments that store_command() refuses, with @p end all zero.
 */
extern int
store_run (struct store const *store,
           enum store_cmd      cmd,
           char const *const  *words,
           char const         *ref,
           struct span         in,
           struct buf         *out,
           struct child_end   *end);

/** Creates the repository of @p store with STORE_CREATE on the calling
 *  thread, as store_run() runs it, unless the repository exists. The
 *  function locks the directory of the repository (flock()) while it
 *  checks and creates, so that services that start at the same time create
 *  the repository once. store_create() waits for the lock before it
 *  refuses a directory that is not a repository. Returns what store_run()
 *  returns, with how git ended in @p *end. If the repository exists, no
 *  git runs, and @p *end is an exit with status 0. Returns EINVAL for a
 *  null argument, or the error of flock().
 */
extern int
store_init (struct store const *store,
            struct buf         *out,
            struct child_end   *end);

/** Lets go of @p *p_c, a git that runs a command of store_command(), and
 *  sets @p *p_c to nullptr. A git that writes runs to its end, because a
 *  kill could leave a lock file behind. Any other git is killed.
 */
extern void
store_drop (struct child **p_c);

/** Returns true if @p out, the output of a git that writes, shows that
 *  another writer changed the ref, or created the repository, at the same
 *  time. Other instances of askai can share the repository.
 */
extern bool
store_raced (struct span out);

/** Returns why a git that writes failed, from its output @p out. The cause
 *  is the last line of the output, which is the error of git. A git
 *  fast-import that fails writes the line "fast-import: dumping crash
 *  report to FILE" after its error, and the cause is then the line before
 *  that line. The cause is cut at STORE_CAUSE bytes, at the end of a UTF-8
 *  character. The view is a part of @p out, without a NUL. The view is
 *  empty for an output without an error line.
 */
extern struct cstr_view
store_cause (struct span out);

/** The kinds of the objects in the output of STORE_CHECK and STORE_READ
 *  (store_kind()).
 */
enum store_kind : uint8_t {
	STORE_KIND_MISSING,  /**< no object has the name */
	STORE_KIND_BLOB,
	STORE_KIND_TREE,
	STORE_KIND_COMMIT,
	STORE_KIND_OTHER,    /**< another kind, or a line that is not valid */
};

/** Returns the kind of the object of @p line, a line of the output of
 *  STORE_CHECK or the first line of an object in the output of STORE_READ,
 *  without its line feed: "OID KIND SIZE", or "NAME missing". Stores OID
 *  in @p *oid, or no name for STORE_KIND_MISSING and for a line whose OID
 *  is not a name, which is STORE_KIND_OTHER. For a line with a KIND, the
 *  function stores SIZE in @p *size.
 */
extern enum store_kind
store_kind (struct cstr_view  line,
            struct oid       *oid,
            struct cstr_view *size);

/** Adds to @p *used, the bytes of one output of STORE_READ so far, the
 *  bytes of an object with @p bytes bytes of data, and returns true, or
 *  returns false with @p *used unchanged if the output would then be
 *  longer than STORE_MAX.
 */
extern bool
store_read_fits (uint64_t *used,
                 uint64_t  bytes);

/** Reads the next object of @p *out, the output of STORE_READ, and drops
 *  it from @p *out. For a blob, the function stores its ID in @p *id and
 *  its data, a view of @p *out, in @p *data, and returns 0. Otherwise
 *  @p *id is no name and @p *data is empty, and the function returns
 *  ENOENT for a name that names no object, EISDIR for a tree, or EIO with
 *  @p *out unchanged for output that is not a whole object or for an
 *  object of another kind.
 */
extern int
store_object_next (struct span *out,
                   struct oid  *id,
                   struct span *data);

/** Writes to @p in the input of STORE_CHECK for the path @p name at
 *  @p ref, a ref or a commit: a line for @p ref, a line for each directory
 *  in the path, and a line for the path. Returns 0, EINVAL for a null
 *  argument, or the error number that buf_printf() returns.
 */
extern int
store_check_input (struct buf       *in,
                   char const       *ref,
                   struct cstr_view  name);

/**
 * Reads @p out, the output of STORE_CHECK for the input that
 * store_check_input() writes. If the ref is a commit, the function writes
 * the commit to @p tip, else no name. If a file has the path, the function
 * writes the id and the size of the blob to @p id and @p bytes. Otherwise
 * @p id is no name and @p bytes is 0. Returns 0 if a file of the path can
 * be added, EEXIST if a file has the path, EISDIR if files are in a
 * directory of the path, ENOTDIR if a directory in the path is a file, EIO
 * if the output is not what the command writes, also for a ref that names
 * an object other than a commit, and EINVAL for a null @p tip, @p id or
 * @p bytes.
 */
extern int
store_checked (struct span  out,
               struct oid  *tip,
               struct oid  *id,
               uint64_t    *bytes);

/** Writes to @p *age how many seconds ago the lock file of the ref @p ref
 *  was last changed, or 0 without a lock file. A lock file last changed
 *  more than STORE_STALE before the system booted belongs to no running
 *  process, as a crash can leave it, and is removed instead. With the
 *  margin, a step of the clock cannot make a live lock look older than the
 *  boot. Any other lock file stays, as its writer may be outside askai.
 *  Returns 0 or an error number, EINVAL for a null argument.
 */
extern int
store_lock (struct store const *store,
            char const         *ref,
            int64_t            *age);

/** Returns how many seconds ago the lock file of the ref @p ref was last
 *  changed if that is more than STORE_STALE (store_lock()), else 0. The
 *  function logs a lock file that old as "TOOL: DIR/REF.lock is N s old",
 *  where TOOL is @p tool, the name of the caller.
 */
extern int64_t
store_stale (struct store const *store,
             char const         *ref,
             char const         *tool);

/** Writes @p name to @p dest in double quotes, with a backslash before
 *  each double quote and backslash in it, as git fast-import reads a path,
 *  and returns the end. @p dest must have room for 2 * name.len + 2 bytes,
 *  STORE_QUOTED for a path of STORE_PATH bytes. The function writes no
 *  NUL.
 */
extern char *
store_quote (char             *dest,
             struct cstr_view  name);

/**
 * Writes to @p in the head of a file part of the input of STORE_COMMIT,
 * for the file @p name whose data is @p len bytes long. After the head,
 * the function adds @p len bytes for the data, which it leaves unwritten,
 * and a line feed. It stores the offset and the bytes of the data in
 * @p *part. The caller then writes the data at the offset part->at from
 * the head of @p in, as store_part_file() does. Returns 0, EINVAL for
 * a null argument, a name longer than STORE_PATH or a failed snprintf() of
 * the data command, or ENOMEM.
 */
extern int
store_part_head (struct extent    *part,
                 struct buf       *in,
                 struct cstr_view  name,
                 size_t            len);

/**
 * Writes to @p in a file part of the input of STORE_COMMIT: the file
 * @p name with the data @p data. The function fills @p *part as
 * store_part_head() does. A file part replaces any file with the same
 * path. Returns 0, EINVAL for a name longer than STORE_PATH, or ENOMEM.
 */
extern int
store_part_file (struct extent    *part,
                 struct buf       *in,
                 struct cstr_view  name,
                 struct cstr_view  data);

/**
 * Writes to @p in the part of the input of STORE_COMMIT that deletes the
 * file @p name. Returns 0, EINVAL for a null argument or a name longer than
 * STORE_PATH, or ENOMEM.
 */
extern int
store_part_delete (struct buf       *in,
                   struct cstr_view  name);

/**
 * Writes to @p in a file part of the input of STORE_COMMIT that adds the
 * blob @p id of the repository as the file @p name, without its data. git
 * refuses the commit if the repository has no such blob. Returns 0, EINVAL
 * for a null argument, an @p id that is no name or a name longer than
 * STORE_PATH, or ENOMEM.
 */
extern int
store_part_blob (struct buf       *in,
                 struct cstr_view  name,
                 struct oid const *id);

/** One line of the trailer of a commit message, written as KEY: VALUE. */
struct store_trailer {
	struct cstr_view key;    /**< letters, digits, underscores and hyphens */
	struct cstr_view value;  /**< one line */
};

/** The name and the email address of the author or the committer of a
 *  commit, written as NAME <EMAIL>. Neither contains <, >, a line feed or a
 *  NUL. The data pointer of neither view is nullptr, so an empty email
 *  address is CSTR_VIEW("").
 */
struct store_ident {
	struct cstr_view name;
	struct cstr_view email;
};

/** A commit, as store_commit_input() writes it for STORE_COMMIT. */
struct store_commit {
	struct store_ident          author;      /**< without a name, git
	                                              writes the committer as the
	                                              author */
	struct store_ident          committer;
	struct cstr_view            message;     /**< without a final line
	                                              feed */
	struct span                 files;       /**< zero or more file parts,
	                                              one after another, from
	                                              store_part_head(),
	                                              store_part_file(),
	                                              store_part_blob() and
	                                              store_part_delete() */
	struct store_trailer const *trailers;    /**< the trailer after the
	                                              message, or nullptr */
	size_t                      n_trailers;  /**< lines of the trailer */
	char const                 *ref;         /**< the ref that the commit
	                                              updates */
	struct oid const           *tip;         /**< the parent, or no name for
	                                              a root commit, which git
	                                              refuses if the ref
	                                              exists */
};

/**
 * Writes to @p in the input of STORE_COMMIT for @p commit. The commit
 * message is commit->message, then a blank line and the trailer if
 * commit->n_trailers is not 0. Without file parts, the tree of a root
 * commit is empty, and the tree of any other commit is the tree of its
 * parent. store_committed() reads the id of the new commit from the
 * output. Returns 0, EINVAL for a null argument, a message with a null
 * data pointer, of STORE_MESSAGE bytes or more or with a NUL, a ref with
 * a line feed, an ident or a trailer line that is not valid, or the error
 * of buf_printf() or buf_put(), after which @p in can contain part of the
 * input.
 */
extern int
store_commit_input (struct store_commit const *commit,
                    struct buf                *in);

/**
 * Reads @p out, the output of STORE_COMMIT, and writes the id of the new
 * commit, its last line, to @p commit. Returns 0, EINVAL for a null
 * @p commit, or EIO with no name in @p commit if the last line is not a
 * commit id.
 */
extern int
store_committed (struct span  out,
                 struct oid  *commit);

#endif /* ASKAI_SRC_STORE_H_ */
