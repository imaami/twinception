/** @file git.h
 *
 * The git tool (S8): committed repository contents only.
 *
 * A call names a configured repository and an allowed command: read
 * (numbered lines of a committed file), log, show, diff, grep, blame,
 * ls-tree, cat-file or rev-parse. help:true describes its options and limits
 * without running git. Values belong in the same option string as their
 * flag. Paths follow "--", relative to the repository top, with no ".."
 * component or long-form pathspec magic.
 *
 * Revision roots are references or hashes with ^/~ ancestry or ^{commit}. Each
 * root is resolved to an immutable ID and checked for reachability from HEAD,
 * branches, tags or remote-tracking refs before a read/show/cat-file :path
 * suffix is applied. Raw blobs, index selectors, reflogs, private stash
 * objects and revision expansions are unavailable. Log accepts A..B/A...B
 * ranges; diff requires exactly two roots, separately or as one explicit
 * range. Other commands default to HEAD. Blame's --ignore-rev roots undergo
 * the same checks.
 *
 * Each call creates a private bare view with a snapshot of the approved refs
 * and shared object storage, preserving validated shallow boundaries. It imports
 * no worktree, index or local config; attributes and mailmaps come from its fixed
 * HEAD when present. Git runs without replacement
 * objects, system/global config, remote protocols or lazy fetching. Settings
 * disable executable hooks,
 * fsmonitor, pagers, external diffs, textconv and signature verification.
 * Repository discovery cannot ascend above a configured repository. The
 * source's metadata and object storage remain trusted; this is not an OS sandbox.
 * No command reads the index or uncommitted files as its source. Internal
 * validation failures are generic. Failed read/show/cat-file diagnostics
 * are replaced so they cannot disclose whether a local path exists. Output
 * is cut at 4 MiB. read numbers the lines before the cut and does not
 * capture git's stderr. One deadline spans discovery, authorization and
 * execution, sending SIGTERM, then SIGKILL a second later. Grep's
 * exit status 1 means no matches; other nonzero exits are errors.
 *
 * Calls run as children (child.h) on their client loop. Their output is made
 * UTF-8 a share per step (mcp.h); dropping a call kills its git child.
 */
#ifndef ASKAI_SRC_GIT_H_
#define ASKAI_SRC_GIT_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "cstr_view.h"
#include "dstr.h"
#include "git_run.h"
#include "oid.h"

struct child;
struct git;
struct git_view;
struct mcp_slot;
struct plugin;

enum {
	GIT_REPO_OPTS = 11,    /**< options of git for a repository, with nullptr */
	GIT_ROOTS     = 24,    /**< revisions in a pin, at most */
};

/** A repository of the git tool, with its name, its real path and the
 *  options of git for the repository. The path, the setting
 *  safe.directory=<path> of the options and the name are in one
 *  allocation that starts at path.
 */
struct git_repo {
	struct cstr_view name;
	char            *path;
	char const      *opts[GIT_REPO_OPTS];  /**< the options of the git tool
	                                            and safe.directory, up to a
	                                            nullptr */
};

/** The phases of a pin, in their order. */
enum git_pin_phase : uint64_t {
	GIT_PIN_META,
	GIT_PIN_REFS,
	GIT_PIN_RESOLVE,
	GIT_PIN_CHECK,
	GIT_PIN_DONE,
};

/**
 * The revisions that a tool call reads from a repository of the git tool,
 * resolved to commits that HEAD, a branch, a tag or a remote-tracking ref
 * reaches, in a private view of the repository (git_view.h). Each phase
 * runs one git process as a child of the call, before one deadline.
 */
struct git_pin {
	struct buf             metadata;  /**< the output of GIT_PIN_META */
	struct dstr            text;      /**< the roots, each with a NUL */
	struct child          *child;     /**< the git of the phase, and after
	                                       GIT_PIN_DONE the caller's child */
	struct git_view       *view;
	struct git const      *git;
	struct git_repo const *repo;      /**< nullptr for an unknown name */
	uint64_t               deadline;  /**< a loop_clock() time */
	size_t                 n_roots;
	enum git_pin_phase     phase;
	struct oid             roots[GIT_ROOTS];  /**< the commit IDs */
};

/** Initializes @p pin for the repository named @p repo in @p git and the
 *  deadline @p deadline.
 */
extern void
git_pin_init (struct git_pin   *pin,
              struct git const *git,
              struct cstr_view  repo,
              uint64_t          deadline);

/** Kills the child of @p pin if it runs, and frees the view and the
 *  buffers of @p pin.
 */
extern void
git_pin_fini (struct git_pin *pin);

/** Adds the revision @p word, a ref name or a hash with ^ and ~ suffixes,
 *  to the roots of @p pin. Returns 0, EINVAL or ENOMEM.
 */
extern int
git_pin_add (struct git_pin   *pin,
             struct cstr_view  word);

/** Takes the output of the ended git of @p pin, if any, and spawns the git
 *  of the next phase in @p slot, which it parks. Returns 0, also when the
 *  pin is done and no child runs; EINVAL, EACCES or EIO with the git
 *  tool's text in @p *why; ENOMEM; or the error of mcp_slot_spawn().
 */
extern int
git_pin_step (struct git_pin   *pin,
              struct mcp_slot  *slot,
              struct cstr_view *why);

/** Ends the call in @p slot after git_pin_step() or the spawn of the
 *  program @p name for the call returned the error @p e. For ENOMEM, the
 *  call ends as mcp_slot_fail() ends a call. For another error, the call
 *  fails with the text @p why of git_pin_step(), or with the text of
 *  child_spawn_why() for @p name if git_pin_step() stored no text.
 */
extern void
git_step_failed (struct mcp_slot  *slot,
                 int               e,
                 struct cstr_view  why,
                 char const       *name);

/** Writes to @p dir where the git processes of @p pin run, their options
 *  and their environment. They run in the repository, or in the view of
 *  @p pin once the view exists.
 */
extern void
git_pin_dir (struct git_pin const *pin,
             struct git_run_dir   *dir);

/**
 * The git tool for the @p n repositories at @p paths, whose names are apart,
 * each call's deadline @p timeout ns after it starts; nullptr, with the error
 * in @p *e: EINVAL if a path has no name, ENOMEM if out of memory, or as
 * realpath() says if a path does not resolve.
 */
extern struct git *
git_create (char const *const paths[],
            size_t             n,
            uint64_t           timeout,
            int               *e);

extern void
git_destroy (struct git **p_git);

/** The plugin of the git tool. It is built into askai (src/plugin.h), and
 *  its service, which is also its part on every loop, is a struct git.
 */
extern struct plugin const git_plugin;

#endif /* ASKAI_SRC_GIT_H_ */
