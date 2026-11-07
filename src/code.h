/** @file code.h
 *
 * The code tool runs askai-code (src/cxi.h) on a commit of a repository of
 * the git tool (src/git.h) that --code lists with its compile commands. A
 * call resolves its revision as the git tool does, then runs askai-code as
 * a child process on the client loop of the call.
 */
#ifndef ASKAI_SRC_CODE_H_
#define ASKAI_SRC_CODE_H_

struct config;
struct git;
struct plugin;

/** The service of the code tool. */
struct code {
	struct git const    *git;
	struct config const *cfg;
};

/** The plugin of the code tool, built into askai (src/plugin.h). */
extern struct plugin const code_plugin;

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
