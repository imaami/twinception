/** @file code.h
 *
 * The code tool runs askai-code (src/cxi.h) on a commit of a repository of
 * the git tool (src/git.h) that --code lists with its compile commands. A
 * call resolves its revision as the git tool does, then runs askai-code as
 * a child process on the client loop of the call.
 *
 * For callers, askai keeps the graphs of the translation units of a build
 * (src/build.h): the compile commands of a repository at a commit. The
 * service keeps the builds for all client loops. The first call for a
 * build starts an askai-code for each translation unit, a few at a time,
 * on the loops of the calls that wait for the build, and a call waits
 * until the units are parsed or its deadline comes. A later call of any
 * loop gets its answer from the graphs, which no thread changes.
 */
#ifndef ASKAI_SRC_CODE_H_
#define ASKAI_SRC_CODE_H_

#include <pthread.h>
#include <stddef.h>

#include "list.h"
#include "mcp.h"

struct config;
struct git;
struct plugin;

/** The service of the code tool, for all client loops. lock guards the
 *  members after it, and what changes in a build after it is created.
 */
struct code {
	struct git const    *git;
	struct config const *cfg;
	pthread_mutex_t      lock;
	struct list          builds;  /**< the builds that askai keeps, the one
	                                   that a call used last at the end */
	size_t               bytes;   /**< of the graphs of the builds */
	size_t               parses;  /**< the processes of all loops that parse
	                                   a translation unit */
};

/** Initializes the service @p dest of the code tool for the repositories of
 *  @p git and the configuration @p cfg, without a build. Returns 0 or the
 *  error of pthread_mutex_init().
 */
extern int
code_init (struct code         *dest,
           struct git const    *git,
           struct config const *cfg);

/** Frees the builds of @p code. No loop runs. */
extern void
code_fini (struct code *code);

/** The plugin of the code tool, built into askai (src/plugin.h). */
extern struct plugin const code_plugin;

/** Creates the code tool's part of a client loop (src/mcp.h): the
 *  processes of the loop that parse for the builds of the service. The
 *  service argument points to the struct code. The only error number is
 *  ENOMEM.
 */
extern mcp_part_create_fn code_loop_create;

/** Kills the processes of the loop that parse, frees the part, and sets
 *  the caller's pointer to nullptr.
 */
extern mcp_part_destroy_fn code_loop_destroy;

/** Runs askai-code --check for @p cfg, which contains at least one value
 *  of --code. Sets CFG_CODE if the check succeeds, and sets
 *  cfg->code_check to the two lines of the check, or to the reason why the
 *  tool is off. Returns 0 or ENOMEM.
 */
extern int
code_check (struct config *cfg);

/** Logs the lines "code tool:" and "code repositories:" of @p cfg, in
 *  which CFG_CODE is set.
 */
extern void
code_report (struct config const *cfg);

#endif /* ASKAI_SRC_CODE_H_ */
