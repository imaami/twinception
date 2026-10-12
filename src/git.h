/** @file The git tool (S8): read-only git commands in the repositories that
 *  askai lists (--git), each run as a child (child.h) on the call's loop.
 *
 * A call names a repository, by git_name() of the path it was listed by,
 * and a command of an allowlist: log, show, diff, grep, blame, ls-files,
 * ls-tree, cat-file or rev-parse.  Its options must each be one that
 * command's allowlist has, a value given with it after '=' (or, for a few
 * short ones, attached); its revisions follow --end-of-options (but for
 * those that take it amiss), and none may start with '-' or '/' or have a
 * ".." component, so that none names a file outside the repository, as git
 * diff's would; its paths follow "--", relative, with no ".." component;
 * grep's pattern follows -e; and rev-parse takes revisions only.  So
 * nothing a client sends is an option the allowlist lacks: none writes a
 * file, reads one outside the repository, or runs a program.  git runs with
 * a fixed environment: without system or global configuration, a HOME that
 * does not exist, no repository found above the listed ones'
 * (GIT_CEILING_DIRECTORIES), and no remote protocol or lazy fetch
 * (GIT_ALLOW_PROTOCOL, GIT_NO_LAZY_FETCH), which no repository's
 * configuration overrides; and with settings that stop what a repository's
 * own configuration could have it run: the fsmonitor, a pager, an external
 * diff, textconv (a submodule's too), gpg and hooks, and its refreshing the
 * index.  The repository's other configuration, such as its filters, which
 * a work tree's files pass through, is trusted, as listing it says.  Its
 * output, its error included, is cut at GIT_MAX; at its deadline it gets
 * SIGTERM, and SIGKILL GIT_GRACE_NS later.
 */
#ifndef TWINCEPTION_SRC_GIT_H_
#define TWINCEPTION_SRC_GIT_H_

#include <stddef.h>
#include <stdint.h>

struct git;
struct json_object;

enum {
	GIT_MAX  = 2 << 20,  /**< output bytes, at most */
	GIT_ARGV = 128,      /**< a command's argv, its nullptr included */
	GIT_WHY  = 256,      /**< bytes of what is wrong with a call */
};

/** From SIGTERM to SIGKILL at a git call's deadline. */
static constexpr uint64_t GIT_GRACE_NS = 1'000'000'000;

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

/** The environment git runs in. */
extern char *const *
git_env (struct git const *git);

/** A call's deadline, in ns from its start. */
extern uint64_t
git_timeout (struct git const *git);

/** The tool's definition, for tools/list; nullptr if out of memory. */
extern struct json_object *
git_definition (struct git const *git);

/**
 * Fill @p argv, of GIT_ARGV entries, with the git command that @p args, a
 * call's arguments, ask for, its strings pointing into @p args and @p git,
 * which must outlive it.  Returns 0; or EINVAL, @p why, of GIT_WHY bytes,
 * saying what is wrong with the arguments, for the model to read.
 */
extern int
git_command (struct git const   *git,
             struct json_object *args,
             char const         *argv[],
             char               *why);

#endif /* TWINCEPTION_SRC_GIT_H_ */
