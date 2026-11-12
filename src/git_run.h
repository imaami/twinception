/** @file git_run.h
 *
 * The argv and the environment of the git processes that askai and
 * askai-cli start. A struct git_run_dir describes where git runs, the
 * options of the caller and the environment. git_run_init() writes the
 * argv of one command for a struct git_run_dir into a struct git_run.
 * child_create(), child_run() or child_capture() runs the struct
 * child_cmd of the struct git_run. git_run_now() runs one command on the
 * calling thread with the deadline GIT_RUN_NS after the call, turns the
 * end of git into an error number, and reads the name of an object that
 * git prints. git_run_path() reads a line with an absolute path from the
 * output of git. git_run_name() returns the name of the repository at a
 * path, and git_run_name_valid() tests such a name.
 */
#ifndef ASKAI_SRC_GIT_RUN_H_
#define ASKAI_SRC_GIT_RUN_H_

#include <stddef.h>
#include <stdint.h>

#include "child.h"
#include "clk.h"
#include "compat.h"
#include "cstr_view.h"
#include "util.h"

struct oid;

/** The environment entries after PATH of every git process that askai
 *  starts for the git tool, the notes and the journal, and of every git
 *  process of askai-cli. They set LC_ALL to C.UTF-8, and turn off the
 *  home, system and global configuration, the terminal prompt, the pager,
 *  lazy fetching and every remote protocol. A repository's configuration
 *  cannot override GIT_ALLOW_PROTOCOL.
 */
#define GIT_ENV_FIXED \
        "HOME=/nonexistent", "LC_ALL=C.UTF-8", "GIT_CONFIG_NOSYSTEM=1", \
        "GIT_CONFIG_GLOBAL=/dev/null", "GIT_TERMINAL_PROMPT=0", "GIT_PAGER=cat", \
        "GIT_NO_LAZY_FETCH=1", "GIT_ALLOW_PROTOCOL=none"

/** The options before the command of every git process that askai and
 *  askai-cli start. They turn off the pager, replacement objects, every
 *  remote protocol, automatic gc, maintenance and the signature checks of
 *  a log. They also stop git from running the program that the
 *  configuration of a repository sets for the fsmonitor, for gpg or for
 *  hooks. The format of a log can make git run gpg.
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

/** The time from SIGTERM to SIGKILL at the deadline of a git process that
 *  askai or askai-cli runs, in nanoseconds. It is one second. git removes
 *  its lock files after SIGTERM.
 */
#define GIT_GRACE NS

/** The time in nanoseconds that a git process may run when askai or
 *  askai-cli runs git for a repository of its own, such as the journal.
 *  The git tool uses the timeout of the configuration instead.
 */
#define GIT_RUN_NS (10 * NS)

/** The words of git init that make a bare repository in ".", the
 *  directory where git runs, with the initial branch @p branch, a string
 *  literal, and with no files from a template.
 */
#define GIT_INIT_BARE_WORDS(branch) \
        "init", "--bare", "--quiet", "--template=", \
        "--initial-branch=" branch, "--", "."

enum {
	/** The number of entries of GIT_ENV_FIXED. */
	GIT_RUN_ENV_FIXED = array_size(((char *[]){ GIT_ENV_FIXED })),
	/** The most words that git_run_dir_argv() writes before the words of
	 *  a command, without the options of the caller: "git",
	 *  GIT_OPTIONS_FIXED, "-C", the path and "--git-dir=.". */
	GIT_RUN_PREFIX    = 1 + array_size(((char const *[]){ GIT_OPTIONS_FIXED })) + 3,
	GIT_RUN_ARGV      = 128,  /**< words in the argv of git, with nullptr */
	/** The entries in the environment of git: PATH, GIT_ENV_FIXED, at
	 *  most 3 entries of the caller, and nullptr. */
	GIT_RUN_ENV       = 1 + GIT_RUN_ENV_FIXED + 3 + 1,
};

/** The bits of git_run_dir.flags. The bits of a command are enum
 *  child_flag, as for any child.
 */
enum git_run_dir_flag : uint32_t {
	GIT_RUN_DOT = 1u << 0,  /**< write "--git-dir=." after the options, so
	                             that git uses the directory as the
	                             repository */
};

/** Where git runs, the options of the caller, and the environment of git.
 */
struct git_run_dir {
	char const        *path;              /**< the directory after -C, or
	                                           nullptr to run git in
	                                           fds.dir of the child */
	char const *const *opts;              /**< the options after
	                                           GIT_OPTIONS_FIXED, up to a
	                                           nullptr, or nullptr */
	char              *env[GIT_RUN_ENV];  /**< PATH, GIT_ENV_FIXED, the
	                                           entries of the caller, and
	                                           nullptr */
	uint64_t           flags;             /**< enum git_run_dir_flag,
	                                           widened over the padding */
};

/** A git command: the struct child_cmd to run, and the argv that c.argv
 *  points at.
 */
struct git_run {
	struct child_cmd c;
	char const      *argv[GIT_RUN_ARGV];
};

/**
 * Initializes @p dest for git in the directory @p path, with the options
 * @p opts and the bits @p flags of enum git_run_dir_flag. The environment
 * of @p dest is PATH, GIT_ENV_FIXED and the entries at @p env up to their
 * nullptr, or no more entries if @p env is nullptr. @p dest points at
 * @p path, at @p opts and at the entries of @p env, which must stay valid
 * as long as @p dest is in use. Returns 0, EINVAL if @p dest is nullptr
 * or @p flags contains another bit, or E2BIG for more than 3 entries at
 * @p env. After E2BIG, @p dest is all zero.
 */
extern int
git_run_dir_init (struct git_run_dir *dest,
                  char const         *path,
                  char const *const  *opts,
                  char *const        *env,
                  uint32_t            flags);

/**
 * Writes the argv of git for @p dir to the @p cap pointers at @p argv:
 * "git", GIT_OPTIONS_FIXED, the options of @p dir, "-C" and the path if
 * the path is not nullptr, and "--git-dir=." with GIT_RUN_DOT. Then
 * writes the words at @p words up to their nullptr, if @p words is not
 * nullptr, and a nullptr. Returns 0, EINVAL if @p dir or @p argv is
 * nullptr, or E2BIG if the words and the nullptr do not fit.
 */
extern int
git_run_dir_argv (struct git_run_dir const *dir,
                  char const *const        *words,
                  char const              **argv,
                  size_t                    cap);

/**
 * Initializes @p dest for the git command @p words, up to their nullptr,
 * in @p dir. dest->argv is what git_run_dir_argv() writes. dest->c is a
 * copy of @p how with the file "git", the argv dest->argv and the
 * environment of @p dir. The input, the cap, the deadline, the grace and
 * the bits of enum child_flag are those of @p how. A command that writes
 * to a repository sets .grace = GIT_GRACE, so that git removes its lock
 * files at SIGTERM. A command whose output must contain the errors sets
 * .flags = CHILD_STDERR. @p dir and the words must stay valid until the
 * spawn, because @p dest points at them. Returns 0, EINVAL if a pointer
 * is nullptr, or E2BIG if the argv does not fit.
 */
extern int
git_run_init (struct git_run           *dest,
              struct git_run_dir const *dir,
              char const *const        *words,
              struct child_cmd const   *how);

/**
 * Runs the git command @p words, up to their nullptr, in @p dir on the
 * calling thread with the descriptors @p fds, as child_cmd_run() runs a
 * command. The function initializes a struct git_run as git_run_init()
 * does, with the deadline GIT_RUN_NS after the call. The input, the cap,
 * the grace and the bits of enum child_flag are those of @p how. The
 * function does not read how.deadline. If @p out is not nullptr, the
 * content of @p out is replaced with the output, up to the cap. Otherwise
 * git writes to fds.out.
 *
 * Returns EINVAL if @p oid is not nullptr without @p out, or the error of
 * git_run_init(). After these two errors, @p out and @p oid are as they
 * were. Otherwise the function returns what child_cmd_run() returns,
 * which is EIO for an exit status other than 0, and ETIMEDOUT for a git
 * that runs longer than GIT_RUN_NS.
 *
 * If @p oid is not nullptr, the function copies the name of an object in
 * the output, one name and a line feed, into @p oid. Another output is
 * EIO. The exit status 1, which rev-parse --verify --quiet returns for a
 * missing ref, is then no error, and @p oid is empty. @p oid is also
 * empty after an error of the run.
 */
extern int
git_run_now (struct git_run_dir const *dir,
             char const *const        *words,
             struct child_cmd          how,
             struct child_fds          fds,
             struct buf               *out,
             struct oid               *oid);

/**
 * Stores the next line of @p rest, a part of the output of git, in
 * @p path, and removes the line and its line feed from @p rest. Returns 0,
 * or EINVAL if a pointer is nullptr or the line is not an absolute path
 * without a NUL.
 */
extern int
git_run_path (struct cstr_view *rest,
              struct cstr_view *path);

/** Returns true if @p name is a valid name of a repository: 1 to 64 of
 *  [A-Za-z0-9_.-], not starting with a dot. Reads at most name.len bytes.
 */
static inline bool
git_run_name_valid (struct cstr_view name)
{
	return name.len && name.len <= 64 && *name.data != '.' &&
	       cstr_view_spn(name, UTIL_ALNUM "_.-") == name.len;
}

/**
 * Returns the name of the repository at @p path: the last component of the
 * path, without a .git suffix, if git_run_name_valid() accepts it. Returns
 * an empty view for a path without a valid name.
 */
extern struct cstr_view
git_run_name (char const *path);

#endif /* ASKAI_SRC_GIT_RUN_H_ */
