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
#include "clk.h"
#include "cstr_view.h"
#include "dstr.h"
#include "oid.h"
#include "util.h"

struct child;
struct git;
struct git_view;
struct mcp_slot;
struct plugin;

/** The environment entries after PATH of every git process that askai
 *  starts for the git tool, the notes and the journal. They set LC_ALL to
 *  C.UTF-8, and turn off the home, system and global configuration, the
 *  terminal prompt, the pager, lazy fetching and every remote protocol. A
 *  repository's configuration cannot override GIT_ALLOW_PROTOCOL.
 */
#define GIT_ENV_FIXED \
        "HOME=/nonexistent", "LC_ALL=C.UTF-8", "GIT_CONFIG_NOSYSTEM=1", \
        "GIT_CONFIG_GLOBAL=/dev/null", "GIT_TERMINAL_PROMPT=0", "GIT_PAGER=cat", \
        "GIT_NO_LAZY_FETCH=1", "GIT_ALLOW_PROTOCOL=none"

/** The options before the command of every git process that askai starts.
 *  They turn off the pager, replacement objects, every remote protocol,
 *  automatic gc, maintenance and the signature checks of a log. They also
 *  keep git from running a program that a repository's configuration
 *  names for the fsmonitor, for gpg or for hooks. The format of a log can
 *  make git run gpg.
 */
#define GIT_OPTIONS_FIXED \
        "--no-pager", "--no-replace-objects", \
        "-c", "core.fsmonitor=false", \
        "-c", "core.hooksPath=/dev/null", \
        "-c", "gpg.program=false", \
        "-c", "gpg.ssh.program=false", \
        "-c", "gpg.x509.program=false", \
        "-c", "protocol.allow=never", \
        "-c", "gc.auto=0", \
        "-c", "maintenance.auto=false", \
        "-c", "log.showSignature=false"

/** The time from SIGTERM to SIGKILL at the deadline of a git process of
 *  the git tool, of the journal, or of the notes that writes, in
 *  nanoseconds: the second of the file comment.
 */
static constexpr uint64_t GIT_GRACE = NS;

enum {
	GIT_ARGV  = 128,  /**< words in the argv of a git command, with nullptr */
	GIT_ENV   = 13,   /**< entries in git's environment, with nullptr */
	GIT_ROOTS = 24,   /**< revisions in a pin, at most */
	GIT_WHY   = 1024, /**< bytes of an error message about a call, NUL included */
};

/** A repository of the git tool: its name, its real path, and the git
 *  setting safe.directory=<path>, in one allocation that starts at path.
 */
struct git_repo {
	struct cstr_view name;
	char            *path;
	char            *safe;
};

/** A message about what is wrong with a tool call. The message starts
 *  empty, and util_text_printf() appends to it.
 */
struct git_why {
	UTIL_TEXT(GIT_WHY);
};

/** The phases of a pin, in their order. */
enum git_pin_phase : uint64_t {
	GIT_PIN_META, GIT_PIN_REFS, GIT_PIN_RESOLVE, GIT_PIN_CHECK, GIT_PIN_DONE,
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

/** Writes to @p argv git and its options for @p pin up to the subcommand,
 *  and to @p env git's environment. Returns the number of words.
 */
extern size_t
git_pin_argv (struct git_pin const *pin,
              char const          **argv,
              char                **env);

/**
 * Returns the name of the repository at @p path: the last component of the
 * path, without a .git suffix. A valid name is 1 to 64 of [A-Za-z0-9_.-]
 * and does not start with a dot. Returns an empty view for a path without
 * a valid name.
 */
extern struct cstr_view
git_name (char const *path);

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
