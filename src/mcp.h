/** @file MCP's Streamable HTTP transport and JSON-RPC semantics, over request
 *  streams (src/stream.h). */
#ifndef TWINCEPTION_SRC_MCP_H_
#define TWINCEPTION_SRC_MCP_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "export.h"
#include "loop_export.h"
#include "tool.h"

struct config;
struct git;
struct ledger;
struct manual;
struct mcp;
struct mcp_static;
struct metrics_mcp;
struct metrics_src;
struct notes;
struct registry;
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
 * Adds the tools of askai's own that @p cfg offers to the registry @p reg
 * (src/registry.h).  The ask tool, the man tool, test_task and the git tool
 * are added in that order as tools that a role can grant.  The tools that
 * only the turns of an ask call use are added after them.
 * Returns 0, or the error number of registry_add().
 */
extern int
mcp_register (struct registry     *reg,
              struct config const *cfg);

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
 * tools and their ids are those of @p cfg's registry (src/registry.h), which
 * also has the tools of the plugins that askai loaded.  A tool
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
 * @p home (src/job.h), with a part of each plugin (src/plugin.h), made in
 * turn on its thread.  It counts into @p counts, whose atomic members are
 * valid, and GET /metrics renders @p metrics.  nullptr, with the error in
 * @p *e, if out of memory, or as a part's making says.
 */
extern struct mcp *
mcp_create (struct loop              *loop,
            struct loop              *home,
            struct mcp_static const  *st,
            struct metrics_src const *metrics,
            struct metrics_mcp       *counts,
            int                      *e);

/** Cancel every request under way, give back what is kept, destroy the
 *  parts, last first, and free MCP. */
extern void
mcp_destroy (struct mcp **p_mcp);

/** Begin a window of reuse, as buf_stash_age() does for a stash. */
extern void
mcp_age (struct mcp *mcp);

/**
 * Free some of what is kept for reuse: a pooled tokener, at BUF_HIGH one
 * whose string buffer outgrew a slice, or else finished requests, then jobs,
 * up to @p max units of release work (buf_units()) or one request if that is
 * more, at BUF_HIGH those unused since the last window began, then what the
 * plugins' parts keep, as their relieve() says.  Returns the units; 0 once
 * none are left to free.
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

/*
 * The call API: what a kind of tool (src/tool.h) does with a call, its
 * slot.  A call ends once: by tell, finish, fail, or a refused offset.  The
 * end cancels a queued step(), and may free the call and its arguments; own
 * and lent output stay until drop(), which the end may run before it
 * returns: after the end, touch neither the call nor what own holds.
 * Another thread completes a call by loop_post() naming it
 * (mcp_slot_name()); the handler finds it with mcp_slot_named().
 */

/** Its plugin's part on this loop. */
EXPORT
extern void *
mcp_slot_part (struct mcp_slot const *slot);

/** The kind's word for the call, nullptr at start. */
EXPORT
extern void **
mcp_slot_own (struct mcp_slot *slot);

/** Take @p args' "offset": 0, or EINVAL, the call ended saying why. */
EXPORT
extern int
mcp_slot_offset (struct mcp_slot    *slot,
                 struct json_object *args);

/**
 * Append @p text, made UTF-8: a byte that breaks it becomes '?', but with
 * TOOL_MORE up to 3 bytes ending no character are left to the next write.
 * 0, the bytes taken in @p *taken, counted as LOOP_COPY; or, nothing
 * taken, EFBIG past TOOL_OUTPUT_MAX, or ENOMEM.
 */
EXPORT
extern int
mcp_slot_write (struct mcp_slot  *slot,
                struct tool_text  text,
                uint32_t          flags,
                size_t           *taken);

/** Not exported yet: the output written, which mcp_slot_write() and
 *  mcp_slot_tell() must not be given: they append to it. */
extern struct tool_text
mcp_slot_answer (struct mcp_slot const *slot);

/** Not exported yet: lent output, which own keeps, that mcp_slot_finish()
 *  gives instead of what is written. */
extern void
mcp_slot_lend (struct mcp_slot  *slot,
               struct tool_text  text);

/** From start(), last: wait.  An own call's arguments are freed; with
 *  TOOL_STREAM, any role, an event stream opens if the client takes one. */
EXPORT
extern void
mcp_slot_park (struct mcp_slot *slot);

/** Queue step(), in bulk: the kind has one. */
EXPORT
extern void
mcp_slot_ready (struct mcp_slot *slot);

/** Name the call for a post: 0, or ENOMEM.  A call has one name until it
 *  ends or drops; naming it again gives the same handle. */
EXPORT
extern int
mcp_slot_name (struct mcp_slot *slot,
               struct handle   *who);

/**
 * In a post's handler: the call, or nullptr if it is gone, and once the
 * loop stops, when its part is gone too: on nullptr, free the message and
 * touch nothing else.  A lookup, as loop_lookup(): its object is the
 * loop's table.
 */
EXPORT
extern struct mcp_slot *
mcp_slot_named (struct loop const *loop,
                struct handle      who);

/** End with @p text whole, cut at TOOL_TELL_MAX or a non-UTF-8 byte. */
EXPORT
extern void
mcp_slot_tell (struct mcp_slot  *slot,
               struct tool_text  text,
               uint32_t          flags);

/** End with the output: an own call's from its offset, a turn's window. */
EXPORT
extern void
mcp_slot_finish (struct mcp_slot *slot,
                 uint32_t         flags);

/**
 * End the call and its request with @p e, ENOMEM or ESHUTDOWN; any other
 * is answered as ENOMEM is, 500 Out of memory.  A turn's fails its ask
 * call.  A call that fails for its own reasons ends by tell or finish with
 * TOOL_FAILED.
 */
EXPORT
extern void
mcp_slot_fail (struct mcp_slot *slot,
               int              e);

#endif /* TWINCEPTION_SRC_MCP_H_ */
