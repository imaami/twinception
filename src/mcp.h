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
struct roster;
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
 * (src/ledger.h), once it has run a second.  A tool call is in @p roster
 * (src/roster.h) from its start until it ends or becomes a task, so that a
 * notifications/cancelled of its caller reaches it.  @p cfg, @p manual, @p tasks, @p git, @p notes,
 * @p ledger and @p roster must outlive the object.
 * nullptr, with the error in @p *e, if it cannot be made.
 */
extern struct mcp_static *
mcp_static_create (struct config const *cfg,
                   struct manual       *manual,
                   struct task_pool    *tasks,
                   struct git          *git,
                   struct notes        *notes,
                   struct ledger       *ledger,
                   struct roster       *roster,
                   int                 *e);

extern void
mcp_static_destroy (struct mcp_static **p_static);

/**
 * Creates the MCP application of the client loop @p loop.  askai calls the
 * function on the thread of @p loop.  @p home is the loop of home, which
 * runs the upstream calls of the client loops as jobs (src/job.h).  The
 * function creates the part of each built-in plugin that has parts, in the
 * order of the plugins.  These plugins are man and git.  The new object
 * records its statistics in @p counts, whose atomic members must be valid.
 * It answers GET /metrics from @p metrics.
 *
 * Returns the new object.  On failure it returns nullptr and stores the
 * error number in @p *e.  The error number is ENOMEM, or the error number
 * of the function that failed to create a part.
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
 * Frees memory that @p mcp has only for reuse.  askai calls it under memory
 * pressure of the level @p level.  The function counts what it frees in
 * units of release work (buf_units()).
 *
 * First it frees one pooled tokener whose string buffer is longer than a
 * limit, if the pools have one.  The limit is LOOP_STEP_PARSE bytes, or 0
 * bytes at BUF_CRITICAL.  After it has freed a tokener, it returns the
 * units of the buffer and frees nothing else.
 *
 * Otherwise it frees finished requests, and then jobs, that are stored for
 * reuse.  Below BUF_CRITICAL it frees only the ones that have been unused
 * since the mcp_age() call before the last one.  It always frees the first
 * such request.  It returns before it frees a later request that would make
 * the total greater than @p max units.  It frees jobs, one unit each, while
 * the total is less than @p max.  If the total is still less than @p max
 * after the jobs, it calls instance_table_relieve() for the plugins'
 * instances with the units that are left (src/instance.h).
 *
 * Returns the number of units freed.  It returns 0 when nothing is left to
 * free.
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
 * The call API.  A kind of tool (src/tool.h) uses these functions for a
 * call, which runs in a slot.  A call ends once: with mcp_slot_tell(),
 * mcp_slot_finish() or mcp_slot_fail(), or when mcp_slot_offset() or
 * mcp_slot_ready() ends it as failed.  The end cancels a step() that is
 * queued, and askai can then free the call's arguments.  The output that
 * the kind's own word points to stays valid until drop().  askai can run
 * drop() before the function that ends the call returns.  After the end,
 * the functions that would end or change the call do nothing, and
 * mcp_slot_write(), mcp_slot_name() and mcp_slot_offset() return EINVAL.
 * After drop() has returned, the kind must not use the slot.  Another
 * thread completes a call with loop_post() and the handle that
 * mcp_slot_name() stores.  The message's function then gets the call with
 * mcp_slot_named().
 *
 * Each exported function checks its inputs before it uses them, because a
 * plugin calls it across the boundary of a shared object.  It refuses
 * nullptr for the slot or the loop, a text with a length and no bytes, and
 * nullptr where it stores a result.  Then a function that returns an error
 * number returns EINVAL, a function that returns a pointer returns nullptr,
 * mcp_slot_principal() returns 0, and the other functions do nothing.
 * mcp_slot_tell() with a slot and a text with a length and no bytes ends
 * the call as failed.
 */

/** Creates the part of a built-in plugin for the client loop @p loop.
 *  @p service is the plugin's service, the object that all loops share.
 *  askai calls the function on the thread of @p loop, before the loop's
 *  first call.  Returns the part.  On failure it returns nullptr and stores
 *  the error number in @p *e.  Only the built-in plugins have parts. */
typedef void *mcp_part_create_fn (void        *service,
                                  struct loop *loop,
                                  int         *e);

/** Destroys the part @p *p_part and sets @p *p_part to nullptr.  askai
 *  calls the function when the loop stops, after it has dropped every call
 *  on the loop.  mcp_create() also calls it for the parts that it has
 *  created if it cannot create the part of a later plugin. */
typedef void mcp_part_destroy_fn (void **p_part);

/** Returns the part of the call's plugin on the call's loop.  Only the
 *  built-in plugins have parts, so askai does not export this function. */
extern void *
mcp_slot_part (struct mcp_slot const *slot);

/** Returns the call's instance: the bytes that instance_init() of the
 *  tool's plugin initialized for the call's user on the call's loop
 *  (src/plugin.h).  It returns nullptr if the plugin has no instances.
 *  askai destroys an instance only when no call uses it and no function of
 *  the plugin is running.  A kind can therefore use the instance until the
 *  function that ends the call returns. */
EXPORT
extern void *
mcp_slot_instance (struct mcp_slot const *slot);

/** Returns the loop that runs the call.  A kind passes it to the functions
 *  of src/loop_export.h, for example loop_timer_set() and loop_room(). */
EXPORT
extern struct loop *
mcp_slot_loop (struct mcp_slot const *slot);

/** Returns the principal of the call's user.  policy_grant()
 *  (src/policy.h) determines the user of a client in this order.  If the
 *  client sent a token and the policy has tokens, the user is the identity
 *  that has the token.  Otherwise, if the client is connected over a Unix
 *  socket, the user is the identity that lists the client's Unix user, or
 *  else the Unix user itself.  Otherwise the client is anonymous, and its
 *  principal is 0.  A call that a turn of the client's ask call makes has
 *  the principal of the client. */
EXPORT
extern uint64_t
mcp_slot_principal (struct mcp_slot const *slot);

/** Returns the address of the call's own word, a pointer that the kind can
 *  set.  The word is nullptr when the call starts.  If the kind sets it,
 *  askai calls the kind's drop() when it drops the call.  A kind without
 *  drop() must not set it. */
EXPORT
extern void **
mcp_slot_own (struct mcp_slot *slot);

/** Takes the call's offset from the member "offset" of @p args.  Returns 0
 *  if the member is missing or is a whole number from 0.  Otherwise it ends
 *  the call as failed with a text that says why, and returns EINVAL.  It
 *  also returns EINVAL if the call has ended. */
EXPORT
extern int
mcp_slot_offset (struct mcp_slot    *slot,
                 struct json_object *args);

/**
 * Appends @p text to the call's output.  A byte that is not part of UTF-8
 * becomes '?'.  With TOOL_MORE in @p flags, up to 3 bytes at the end of
 * @p text that do not complete a character are not taken, so that the next
 * write can complete the character.  Stores the number of bytes taken in
 * @p *taken and counts them as LOOP_COPY work.
 *
 * Returns 0.  On failure it takes nothing.  It returns EFBIG if the output
 * would grow past TOOL_OUTPUT_MAX, ENOMEM if memory runs out, and EINVAL if
 * the call has ended.
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

/** Makes the call wait.  start() calls it last, if the call goes on after
 *  start() returns.  askai can free the call's arguments in this function.
 *  For a kind with TOOL_STREAM, the reply becomes an event stream if the
 *  client accepts one.  Does nothing if the call has ended. */
EXPORT
extern void
mcp_slot_park (struct mcp_slot *slot);

/** Queues the step() of the call's kind as a bulk step of the loop.  If the
 *  kind has no step(), the call ends as failed instead.  Does nothing if the
 *  call has ended. */
EXPORT
extern void
mcp_slot_ready (struct mcp_slot *slot);

/** Stores in @p *who a handle that names the call, for a message that
 *  another thread posts with loop_post().  A call has one handle until it
 *  ends or is dropped, so a second call of this function stores the same
 *  handle.  Returns 0, or ENOMEM.  If the call has ended, the function
 *  stores the handle 0 and returns EINVAL. */
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

/** Ends the call with @p text as its whole output, without an offset.  The
 *  text is cut at TOOL_TELL_MAX bytes, or before its first byte that is not
 *  part of UTF-8.  With TOOL_FAILED in @p flags, the call fails.  Does
 *  nothing if the call has ended. */
EXPORT
extern void
mcp_slot_tell (struct mcp_slot  *slot,
               struct tool_text  text,
               uint32_t          flags);

/** Ends the call with the output that mcp_slot_write() wrote.  A client's
 *  call gets the output from the offset that mcp_slot_offset() took.  A
 *  turn's call gets the part of the output that the turn takes.  With
 *  TOOL_FAILED in @p flags, the call fails.  Does nothing if the call has
 *  ended. */
EXPORT
extern void
mcp_slot_finish (struct mcp_slot *slot,
                 uint32_t         flags);

/**
 * Ends the call and its request with the error number @p e.  The reply is
 * an HTTP 503 error for ESHUTDOWN and an HTTP 500 error, "Out of memory",
 * for any other error number.  For a call of a turn, or of a job that the
 * turns run in the background, the request is the ask call.  If the reply
 * of the ask call is already being sent, only the call ends.  Does nothing
 * if the call has ended.
 *
 * A call that fails for a reason of its own ends with mcp_slot_tell() or
 * mcp_slot_finish() and TOOL_FAILED instead.
 */
EXPORT
extern void
mcp_slot_fail (struct mcp_slot *slot,
               int              e);

#endif /* TWINCEPTION_SRC_MCP_H_ */
