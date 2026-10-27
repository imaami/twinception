/** @file The git tool (S8): committed repository contents only.
 *
 * A call names a configured repository and an allowed command: log, show,
 * diff, grep, blame, ls-tree, cat-file or rev-parse. help:true describes its
 * options and limits without running git. Values belong in the same option
 * string as their flag. Paths follow "--", relative to the repository top,
 * with no ".." component or long-form pathspec magic.
 *
 * Revision roots are references or hashes with ^/~ ancestry or ^{commit}. Each
 * root is resolved to an immutable ID and checked for reachability from HEAD,
 * branches, tags or remote-tracking refs before a show/cat-file :path suffix is
 * applied. Raw blobs, index selectors, reflogs, private stash objects and revision
 * expansions are unavailable. Log accepts A..B/A...B ranges; diff requires
 * exactly two roots, separately or as one explicit range. Other commands
 * default to HEAD. Blame's --ignore-rev roots undergo the same checks.
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
 * validation failures are generic. Failed show/cat-file diagnostics
 * are replaced so they cannot disclose whether a local path exists. Output
 * is cut at 4 MiB; one deadline spans discovery, authorization and execution,
 * sending SIGTERM, then SIGKILL a second later. Grep's
 * exit status 1 means no matches; other nonzero exits are errors.
 *
 * Calls run as children (child.h) on their client loop. Their output is made
 * UTF-8 a share per step (mcp.h); dropping a call kills its git child.
 */
#ifndef TWINCEPTION_SRC_GIT_H_
#define TWINCEPTION_SRC_GIT_H_

#include <stddef.h>
#include <stdint.h>

#include "mcp.h"

struct git;
struct plugin;

/** The words of the environment, after PATH, of every git that askai runs,
 *  the git tool's, the notes' and the journal's: no home, system or global
 *  configuration, terminal prompt, pager, lazy fetch or remote protocol
 *  (GIT_ALLOW_PROTOCOL, which a repository's configuration cannot
 *  override, stops them all). */
#define GIT_ENV_FIXED \
	"HOME=/nonexistent", "LC_ALL=C.UTF-8", "GIT_CONFIG_NOSYSTEM=1", \
	"GIT_CONFIG_GLOBAL=/dev/null", "GIT_TERMINAL_PROMPT=0", "GIT_PAGER=cat", \
	"GIT_NO_LAZY_FETCH=1", "GIT_ALLOW_PROTOCOL=none"

/** The options before the command of every git that askai runs: no pager,
 *  replacement objects or remote protocol, nor a program that a
 *  repository's configuration names for the fsmonitor, gpg (which a log's
 *  format may ask for) or hooks, and no automatic gc or maintenance. */
#define GIT_OPTIONS_FIXED \
	"--no-pager", "--no-replace-objects", \
	"-c", "core.fsmonitor=false", \
	"-c", "core.hooksPath=/dev/null", \
	"-c", "gpg.program=false", \
	"-c", "gpg.ssh.program=false", \
	"-c", "gpg.x509.program=false", \
	"-c", "protocol.allow=never", \
	"-c", "gc.auto=0", \
	"-c", "maintenance.auto=false"

/**
 * The name of the repository listed as @p path: the last component of the
 * path as given, less a .git suffix, of @p *len bytes, 1 to 64 of
 * [A-Za-z0-9_.-] not beginning with '.'; nullptr if it has none such.
 */
extern char const *
git_name (char const *path,
          size_t     *len);

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

/** The plugin of the git tool.  It is built into askai (src/plugin.h), and
 *  its service is a struct git. */
extern struct plugin const git_plugin;

/** Creates the git tool's part of a client loop (src/mcp.h).  The service
 *  argument points to the struct git.  The only error number is ENOMEM. */
extern mcp_part_create_fn git_loop_create;

/** Kills the children that the git calls on the loop run.  Then it frees
 *  the part and sets the caller's pointer to nullptr. */
extern mcp_part_destroy_fn git_loop_destroy;

#endif /* TWINCEPTION_SRC_GIT_H_ */
