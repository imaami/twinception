/** @file MCP's Streamable HTTP transport and JSON-RPC semantics, over request
 *  streams (src/stream.h). */
#ifndef TWINCEPTION_SRC_MCP_H_
#define TWINCEPTION_SRC_MCP_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "tool.h"

struct config;
struct git;
struct ledger;
struct loop;
struct manual;
struct mcp;
struct mcp_static;
struct metrics_mcp;
struct metrics_src;
struct notes;
struct stream_app;
struct task_pool;

struct mcp_tool {
	char const *name;
	char const *description;
};

/** Limits on requests; the response to a breach is given in parentheses. */
struct mcp_limits {
	uint32_t depth;      /**< JSON containers open at once in a body (400) */
	uint32_t values;     /**< JSON values, keys included, in a body (400) */
	uint32_t members;    /**< members of one JSON object in a body (400) */
	uint32_t keepalive;  /**< seconds between keep-alive comments on an event
	                          stream; zero: none */
};

/** The defaults of doc/core-design.md, sections 3 and 5. */
static inline struct mcp_limits
mcp_limits (void)
{
	return (struct mcp_limits){
		.depth = 32,
		.values = 1024,
		.members = 1024,
		.keepalive = 15
	};
}

/**
 * The tools that @p cfg offers and a client's role may list and call
 * (src/policy.h): their names into @p names, each at its id, the ask tool's
 * first, then man's, test_task's and git's, as offered.  Returns how many.
 */
extern size_t
mcp_tools (struct config const *cfg,
           char const          *names[TOOL_MAX]);

/**
 * What every loop's MCP shares, and nobody changes once it is made: the
 * configuration @p cfg, and the replies that never change, serialized.
 * With @p manual, it offers the tool man beside the ask tool: a page of the
 * manual (src/manual.h); with @p tasks, the test aid test_task, CPU work
 * done as a task (src/task.h); with @p git, the tool git (src/git.h).  With
 * CFG_UPSTREAM_TOOLS, the ask tool lends the upstream man and git, as the
 * client may call them, and with @p notes the tool notes (src/notes.h), and
 * the tool compact: its call then runs a loop of turns, each an upstream
 * call, and the tools each turn calls, until an answer calls none.  The
 * tools' ids are those of mcp_tools(), then notes' and compact's.  A tool
 * call of a modern client that takes tasks becomes one, in @p ledger
 * (src/ledger.h), once it has run a second.  @p cfg, @p manual, @p tasks,
 * @p git, @p notes and @p ledger must outlive it.
 * nullptr, with the error in @p *e, if it cannot be made.
 */
extern struct mcp_static *
mcp_static_create (struct config const *cfg,
                   struct manual       *manual,
                   struct task_pool    *tasks,
                   struct git          *git,
                   struct notes        *notes,
                   struct ledger       *ledger,
                   int                 *e);

extern void
mcp_static_destroy (struct mcp_static **p_static);

/**
 * MCP on @p loop, whose tool calls are jobs that home runs on its loop
 * @p home (src/job.h).  It counts into @p counts, whose atomic members are
 * valid, and GET /metrics renders @p metrics.  nullptr, with the error in
 * @p *e, if out of memory.
 */
extern struct mcp *
mcp_create (struct loop              *loop,
            struct loop              *home,
            struct mcp_static const  *st,
            struct metrics_src const *metrics,
            struct metrics_mcp       *counts,
            int                      *e);

/** Cancel every request under way, and free MCP. */
extern void
mcp_destroy (struct mcp **p_mcp);

/** Begin a window of reuse, as buf_stash_age() does for a stash. */
extern void
mcp_age (struct mcp *mcp);

/**
 * Free some of what is kept for reuse: a pooled tokener, at BUF_HIGH one
 * whose string buffer outgrew a slice, or else finished requests, then jobs,
 * up to @p max units of release work (buf_units()) or one request if that is
 * more, at BUF_HIGH those unused since the last window began.  Returns the
 * units; 0 once none are left to free.
 */
extern size_t
mcp_relieve (struct mcp        *mcp,
             enum buf_pressure  level,
             size_t             max);

/** End the tool calls that wait for the upstream with an error, a share a
 *  step, and refuse new ones: the server drains. */
extern void
mcp_drain (struct mcp *mcp);

/** The stream application that serves MCP on @p mcp's loop. */
extern struct stream_app const *
mcp_app (struct mcp *mcp);

#endif /* TWINCEPTION_SRC_MCP_H_ */
