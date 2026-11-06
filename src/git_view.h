/** @file git_view.h
 *
 * Private bare snapshots for the git tool.
 */
#ifndef TWINCEPTION_SRC_GIT_VIEW_H_
#define TWINCEPTION_SRC_GIT_VIEW_H_

#include "buf.h"

struct git_view;

/** The namespaces of the refs that a snapshot takes, as X(prefix). */
#define GIT_VIEW_REFS(X) X("refs/heads/") X("refs/tags/") X("refs/remotes/")

/**
 * Create a private bare repository sharing the discovered object directory.
 * Metadata is newline-terminated: absolute objects and shallow paths, sha1 or
 * sha256, then an optional HEAD object ID. Paths come from trusted Git discovery.
 * An existing shallow file must be regular, at most 4 MiB and contain complete
 * object ID lines. Refs are newline-terminated "OID refname" records from
 * the namespaces of GIT_VIEW_REFS. No source configuration, worktree, index
 * or other refs are imported. The newlines after the two paths in
 * @p metadata are overwritten with NULs; other input storage remains the
 * caller's. Returns nullptr with EINVAL for malformed input, ENOBUFS for
 * a shallow file over 4 MiB, or a system error in @p e.
 */
extern struct git_view *
git_view_create (struct span metadata,
                 struct span refs,
                 int        *e);

/** Remove the private files and directory, then free and clear @p view. */
extern void
git_view_destroy (struct git_view **view);

/** The directory of the private repository, valid until destruction. */
extern char const *
git_view_directory (struct git_view const *view);

/** Whether the snapshot has a committed HEAD. */
extern bool
git_view_has_head (struct git_view const *view);

#endif /* TWINCEPTION_SRC_GIT_VIEW_H_ */
