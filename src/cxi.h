/** @file cxi.h
 *
 * askai runs askai-code, the helper of the code tool (src/code.h), once a
 * call as "askai-code KEY=VALUE ... -- GIT...", GIT being the git tool's
 * command line up to the subcommand (src/git.h). It answers with exit
 * status 0, or a tool error with status 1. A relative cdb is relative to
 * top. "askai-code --check cache=DIR [unconfined=1]" prints the version
 * of libclang and the confinement. The answer to callers is no text of the
 * tool: it names the callee, the build and the translation units, and
 * askai answers the call from the graphs of the build (src/code.h).
 *
 * With the word unit=N in place of an action, askai-code writes the graph
 * of one translation unit (src/graph.h) with exit status 0: the unit of
 * the entry N of the compile commands, the first entry being 0. Such a
 * request names no git and exports no commit: the exit status is CXI_STALE
 * if the tree of the cache is at another commit.
 */
#ifndef ASKAI_SRC_CXI_H_
#define ASKAI_SRC_CXI_H_

#include <stdint.h>

/** The actions of the code tool: X(ID, name). */
#define CXI_ACTION_LIST(X)            \
        X(DESCRIBE,    "describe")    \
        X(OUTLINE,     "outline")     \
        X(DIAGNOSTICS, "diagnostics") \
        X(CALLERS,     "callers")

#define CXI_ACTION_ENUM(id, ...) CXI_ACTION_##id,
enum cxi_action : uint8_t { CXI_ACTION_LIST(CXI_ACTION_ENUM) CXI_ACTIONS };
#undef CXI_ACTION_ENUM

/** The keys of a request, each passed as a word KEY=VALUE: X(ID, name). */
#define CXI_KEY_LIST(X)             \
        X(ACTION,     "action")     \
        X(REPO,       "repo")       \
        X(PATH,       "path")       \
        X(NAME,       "name")       \
        X(LINE,       "line")       \
        X(WARNING,    "warning")    \
        X(COMMIT,     "commit")     \
        X(DEADLINE,   "deadline")   \
        X(TOP,        "top")        \
        X(CDB,        "cdb")        \
        X(CACHE,      "cache")      \
        X(UNCONFINED, "unconfined") \
        X(UNIT,       "unit")

#define CXI_KEY_ENUM(id, ...) CXI_KEY_##id,
enum cxi_key : uint8_t { CXI_KEY_LIST(CXI_KEY_ENUM) CXI_KEYS };
#undef CXI_KEY_ENUM

/** The most warning options of a request. */
enum { CXI_WARNINGS = 16 };

/** The exit status of a request with unit=N if the tree of the cache is at
 *  another commit than that of the request.
 */
enum { CXI_STALE = 3 };

#endif /* ASKAI_SRC_CXI_H_ */
