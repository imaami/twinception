/** @file mcp.h
 *
 * MCP's Streamable HTTP transport and JSON-RPC semantics, over request
 * streams (src/stream.h).
 */
#ifndef ASKAI_SRC_MCP_H_
#define ASKAI_SRC_MCP_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "cstr_view.h"
#include "export.h"
#include "loop_export.h"
#include "tool.h"

struct blocker;
struct child;
struct child_cmd;
struct config;
struct dstr;
struct git;
struct ledger;
struct journal;
struct list;
struct roster;
struct manual;
struct mcp;
struct mcp_static;
struct member_call;
struct member_registry;
struct metrics_mcp;
struct metrics_src;
struct oid;
struct registry;
struct seen;
struct seen_item;
struct store;
struct stream_app;
struct task_pool;
struct web;
struct web_request;
struct web_response;

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
 * (src/registry.h). The ask tool, the man tool, test_task, the git tool, the
 * code tool, the notes tool and the findings tool are added in that order as
 * tools that a role can grant, and with CFG_CLIENT_JOBS background, jobs,
 * wait and timer after them. The tools that only the turns of an ask call
 * use are added after them. Returns 0, or the error number of
 * registry_add().
 */
extern int
mcp_register (struct registry     *reg,
              struct config const *cfg);

/**
 * Logs at LOG_INFO, as lists with one item a line: askai's own tools for
 * @p cfg, each on or off, and for a tool that is off, the option that turns
 * it on, or "tools" if the configuration file's member tools denies it to
 * clients, but not test_task, a test aid, while it is off; the git
 * repositories and the notes directory; and each plugin of @p reg, which
 * mcp_register() filled for @p cfg, with its file, whether it got options,
 * and its tools, marked "off (tools)" if the member tools denies them to
 * clients. Without --upstream-tools, it logs that the upstream gets no
 * tools. The lines have no rate limit.
 */
extern void
mcp_report (struct registry const *reg,
            struct config const   *cfg);

/**
 * Returns the tools that the upstream may call in the turns of an ask call
 * of a client whose role grants the tools @p tools and lets the upstream
 * call the tools @p upstream, with the configuration @p cfg, a struct config
 * whose registry mcp_register() filled. The tools are those that an ask
 * call of such a client lends its turns, and none if @p tools lacks the ask
 * tool. Returns 0 if @p cfg is nullptr. The function is a policy_lent_fn
 * (src/policy.h).
 */
extern tool_mask
mcp_lent (void const *cfg,
          tool_mask   tools,
          tool_mask   upstream);

/**
 * Creates the state that the MCP applications of all loops share and that
 * does not change after this function returns: the configuration @p cfg
 * and the serialized replies that never change. The tools and their ids
 * are the tools of the registry of @p cfg (src/registry.h), which also
 * contains the tools of the plugins that askai loaded. @p manual is the
 * service of the man tool (src/manual.h), @p tasks runs the calls of the
 * test aid test_task (src/task.h), @p git is the service of the git tool
 * (src/git.h), @p store is the repository of the session of the notes
 * tool (src/store.h), and @p members is the registry of the session
 * (src/member.h). With CFG_UPSTREAM_TOOLS, an ask call runs turns,
 * each an upstream call, and the tool calls of each turn, until an answer
 * calls no tool.
 *
 * A tool call of a modern client that accepts tasks becomes a task in
 * @p ledger (src/ledger.h) after it runs a second. With CFG_CLIENT_JOBS,
 * a client's call of background starts a call as a job, and the jobs,
 * timers and waits of clients are in @p ledger too. A tool call is in
 * @p roster (src/roster.h) from its start until it ends or becomes a task,
 * so that a notifications/cancelled from its caller can cancel it. The
 * calls of the tools of plugins send their HTTP requests through @p web
 * (src/web.h). @p journal stores the reasoning and the recovery state of
 * jobs. These objects must outlive the new object.
 *
 * Returns the new object. On failure it returns nullptr and stores the
 * error number in @p *e.
 */
extern struct mcp_static *
mcp_static_create (struct config const          *cfg,
                   struct manual                *manual,
                   struct task_pool             *tasks,
                   struct git                   *git,
                   struct store const           *store,
                   struct member_registry const *members,
                   struct ledger                *ledger,
                   struct roster                *roster,
                   struct web                   *web,
                   struct journal               *journal,
                   int                          *e);

extern void
mcp_static_destroy (struct mcp_static **p_static);

/**
 * Creates the MCP application of the client loop @p loop. askai calls the
 * function on the thread of @p loop. @p home is the loop of home, which
 * runs the upstream calls of the client loops as jobs (src/job.h). The
 * function creates the part of each built-in plugin that has parts, in the
 * order of the plugins. These plugins are man and notes. The parts run
 * blocking work on @p blockers (src/blocker.h). The new object records its
 * statistics in @p counts, whose atomic members must be valid. It answers
 * GET /metrics from @p metrics.
 *
 * Returns the new object. On failure it returns nullptr and stores the
 * error number in @p *e. The error number is ENOMEM, or the error number
 * of the function that failed to create a part.
 */
extern struct mcp *
mcp_create (struct loop              *loop,
            struct loop              *home,
            struct blocker           *blockers,
            struct mcp_static const  *st,
            struct metrics_src const *metrics,
            struct metrics_mcp       *counts,
            int                      *e);

/** Cancel every request under way, give back what is kept, destroy the
 *  parts, last first, and free MCP.
 */
extern void
mcp_destroy (struct mcp **p_mcp);

/** Begin a window of reuse, as buf_stash_age() does for a stash. */
extern void
mcp_age (struct mcp *mcp);

/**
 * Frees memory that @p mcp has only for reuse. askai calls it under memory
 * pressure of the level @p level. The function counts what it frees in
 * units of release work (buf_units()).
 *
 * First it frees one pooled tokener whose string buffer is longer than a
 * limit, if the pools have one. The limit is LOOP_STEP_PARSE bytes, or 0
 * bytes at BUF_CRITICAL. After it has freed a tokener, it returns the
 * units of the buffer and frees nothing else.
 *
 * Otherwise it frees finished requests, and then jobs, that are stored for
 * reuse. Below BUF_CRITICAL it frees only the ones that have been unused
 * since the mcp_age() call before the last one. It always frees the first
 * such request. It returns before it frees a later request that would make
 * the total greater than @p max units. It frees jobs, one unit each, while
 * the total is less than @p max. If the total is still less than @p max
 * after the jobs, it calls notes_tool_loop_relieve() for the spare calls of
 * the notes tool, and then instance_table_relieve() for the plugins'
 * instances, each with the units that are left (src/notes_tool.h,
 * src/instance.h).
 *
 * Returns the number of units freed. It returns 0 when nothing is left to
 * free.
 */
extern size_t
mcp_relieve (struct mcp        *mcp,
             enum buf_pressure  level,
             size_t             max);

/** End the tool calls that wait for the upstream with an error, a share a
 *  step, and refuse new ones: the server drains.
 */
extern void
mcp_drain (struct mcp *mcp);

/** The stream application that serves MCP on @p mcp's loop. */
extern struct stream_app const *
mcp_app (struct mcp *mcp);

/*
 * The call API. A kind of tool (src/tool.h) uses these functions for a
 * call, which runs in a slot. A call ends once: with mcp_slot_tell(),
 * mcp_slot_finish() or mcp_slot_fail(), or when mcp_slot_offset() or
 * mcp_slot_ready() ends it as failed. The end cancels a step() that is
 * queued, and askai can then free the call's arguments. The output that
 * the kind's own word points to stays valid until drop(). askai can run
 * drop() before the function that ends the call returns. After the end,
 * the functions that would end or change the call do nothing, and
 * mcp_slot_write(), mcp_slot_name(), mcp_slot_offset(), mcp_slot_fetch()
 * and mcp_slot_fetched() return EINVAL.
 * After drop() has returned, the kind must not use the slot. Another
 * thread completes a call with loop_post() and the handle that
 * mcp_slot_name() stores. The message's function then gets the call with
 * mcp_slot_named().
 *
 * Each exported function checks its inputs before it uses them, because a
 * plugin calls it across the boundary of a shared object. A function that
 * returns an error number returns EFAULT for a pointer that is nullptr,
 * also for a text with a length and no bytes, and EINVAL for a call that
 * has ended and for flag bits that the function does not take. A function
 * that returns a pointer returns nullptr if the slot or the loop is
 * nullptr, and mcp_slot_principal() returns 0.
 */

/** Creates the part of a built-in plugin for the client loop @p loop.
 *  @p service is the plugin's service, the object that all loops share.
 *  @p blockers run the work of the part that blocks (src/blocker.h).
 *  @p children is the list of the children (src/child.h) of the loop,
 *  which askai frees after the parts. askai calls the function on the
 *  thread of @p loop, before the loop's first call. Returns the part. On
 *  failure it returns nullptr and stores the error number in @p *e. Parts
 *  exist only for the built-in plugins.
 */
typedef void *mcp_part_create_fn (void           *service,
                                  struct loop    *loop,
                                  struct blocker *blockers,
                                  struct list    *children,
                                  int            *e);

/** Destroys the part @p *p_part and sets @p *p_part to nullptr. askai
 *  calls the function when the loop stops, after it has dropped every call
 *  on the loop. mcp_create() also calls it for the parts that it has
 *  created if it cannot create the part of a later plugin.
 */
typedef void mcp_part_destroy_fn (void **p_part);

/** Kind flag (bit 32, above TOOL_FLAGS) for tools whose calls run alone. A
 *  turn's call of such a tool starts after the results of all earlier calls
 *  are written. Later calls start after its result is written. Only the
 *  kinds built into askai can set it. plugin.c refuses a loaded plugin
 *  whose kind has a flag outside TOOL_FLAGS.
 */
static constexpr uint64_t KF_SERIAL = 1ull << 32;

/** Kind flag (bit 34, above TOOL_FLAGS) for findings. The journal does not
 *  record the calls of such a tool, so a resumed turn runs them again. Only
 *  the kinds built into askai can set it.
 */
static constexpr uint64_t KF_RERUN = 1ull << 34;

/** Returns the part of the call's plugin on the call's loop. Only the
 *  built-in plugins have parts, so askai does not export this function.
 */
extern void *
mcp_slot_part (struct mcp_slot const *slot);

/** Spawns @p cmd for the call on the call's loop, as child_create()
 *  (src/child.h) does, and parks the call (mcp_slot_park()). When the
 *  child is reaped and askai has read all of its output, askai queues the
 *  step() of the call's kind, unless the kind has destroyed or released
 *  the child. Returns the child, or returns nullptr and stores the error
 *  number in @p *e. Only the built-in plugins run children, so askai does
 *  not export this function.
 */
extern struct child *
mcp_slot_spawn (struct mcp_slot        *slot,
                struct child_cmd const *cmd,
                int                    *e);

/** Returns the call's instance: the bytes that instance_init() of the
 *  tool's plugin initialized for the call's user on the call's loop
 *  (src/plugin.h). It returns nullptr if the plugin has no instances.
 *  askai destroys an instance only when no call uses it and no function of
 *  the plugin is running. A kind can therefore use the instance until the
 *  function that ends the call returns.
 */
EXPORT
extern void *
mcp_slot_instance (struct mcp_slot const *slot);

/** Returns the configuration that configure() of the call's plugin stored
 *  (src/plugin.h), or nullptr if the plugin has no configure(). All calls
 *  and loops share the configuration, and nothing changes it.
 */
EXPORT
extern void const *
mcp_slot_config (struct mcp_slot const *slot);

/** Returns the loop that runs the call. A kind passes it to the functions
 *  of src/loop_export.h, for example loop_timer_set() and loop_room().
 */
EXPORT
extern struct loop *
mcp_slot_loop (struct mcp_slot const *slot);

/** Returns the principal of the call's user. policy_grant()
 *  (src/policy.h) determines the user of a client in this order. If the
 *  client is connected over a Unix socket, the user is its Unix user. If a
 *  client over TCP sent a token and the policy has tokens, the user is the
 *  identity that has the token. Otherwise the client is anonymous, and its
 *  principal is 0. A call that a turn of the client's ask call makes has
 *  the principal of the client. Bit 63 of a principal is never set, and
 *  bit 62 is set for an identity and clear for a Unix user.
 */
EXPORT
extern uint64_t
mcp_slot_principal (struct mcp_slot const *slot);

/** Returns true if a client made the call, and false if a turn made it or
 *  @p slot is nullptr. A call that a client's background call runs as a
 *  job is the client's call. This function is not exported.
 */
extern bool
mcp_slot_from_client (struct mcp_slot const *slot);

/** Returns askai's ledger of tasks, jobs and timers (src/ledger.h). This
 *  function is not exported.
 */
extern struct ledger *
mcp_slot_ledger (struct mcp_slot const *slot);

/** Returns the owner of the records and the timers in the ledger that the
 *  call opens and reads: the principal of its user, or for an anonymous
 *  client a key of its address (src/ledger.h). This function is not
 *  exported.
 */
extern uint64_t
mcp_slot_owner (struct mcp_slot const *slot);

/** Returns the name of the tool whose id is @p id, as askai offers it.
 *  This function is not exported.
 */
extern char const *
mcp_slot_tool_name (struct mcp_slot const *slot,
                    uint32_t               id);

/** Returns the address of the call's own word, a pointer that the kind can
 *  set. The word is nullptr when the call starts. If the kind sets it,
 *  askai calls the kind's drop() when it drops the call. A kind without
 *  drop() must not set it.
 */
EXPORT
extern void **
mcp_slot_own (struct mcp_slot *slot);

/** Takes the call's offset from the member "offset" of @p args. Returns 0
 *  if the member is missing or is a whole number from 0. Otherwise it ends
 *  the call as failed with a text that says why, and returns EINVAL. It
 *  returns EFAULT if @p slot is nullptr, and EINVAL if the call has
 *  ended.
 */
EXPORT
extern int
mcp_slot_offset (struct mcp_slot    *slot,
                 struct json_object *args);

/**
 * Appends @p text to the call's output. A byte that is not part of UTF-8
 * becomes '?'. With TOOL_MORE in @p flags, up to 3 bytes at the end of
 * @p text that do not complete a character are not taken, so that the next
 * write can complete the character. Stores the number of bytes taken in
 * @p *taken and counts them as LOOP_COPY work.
 *
 * Returns 0. On failure it takes nothing. It returns EFAULT if @p slot or
 * @p taken is nullptr or @p text has a length and no bytes, EINVAL if the
 * call has ended or @p flags has other bits than TOOL_MORE, EFBIG if the
 * output would grow past TOOL_OUTPUT_MAX, and ENOMEM if memory runs out.
 */
EXPORT
extern int
mcp_slot_write (struct mcp_slot  *slot,
                struct cstr_view  text,
                uint32_t          flags,
                size_t           *taken);

/** Returns a view of the output that mcp_slot_write() wrote. The view is
 *  valid until the output changes, so it must not be passed to
 *  mcp_slot_write() or mcp_slot_tell(). This function is not exported yet.
 */
extern struct cstr_view
mcp_slot_answer (struct mcp_slot const *slot);

/** Sets the output of the call to @p text, whose bytes must stay valid
 *  until the drop() of the kind. mcp_slot_finish() then ends the call with
 *  @p text. This function is not exported yet.
 */
extern void
mcp_slot_lend (struct mcp_slot  *slot,
               struct cstr_view  text);

/** Returns the maximum number of bytes of the call's output that its caller
 *  reads in one result. For a call made by a turn or a job, that is the
 *  turn's window. For a client's call, it is TOOL_OUTPUT_MAX. This function
 *  is not exported yet.
 */
extern size_t
mcp_slot_window_max (struct mcp_slot const *slot);

/** Returns the ring of the calls of the call's ask call (src/seen.h), or
 *  nullptr before askai stores the first call or item of the ask call.
 *  This function is not exported.
 */
extern struct seen const *
mcp_slot_seen (struct mcp_slot const *slot);

/** Returns the current turn number of the ask call that the call belongs
 *  to. This function is not exported.
 */
extern uint32_t
mcp_slot_turn (struct mcp_slot const *slot);

/** Returns the number N of a turn's call, as the stamp [askai: call cN] of
 *  its result says, or 0 for the call of a client or a job. The calls of
 *  an ask call are numbered from 1 in call order across its turns. This
 *  function is not exported.
 */
extern uint32_t
mcp_slot_call (struct mcp_slot const *slot);

/** Returns true for a call of the turn that askai resumes after a restart.
 *  Such a call may have run before the restart, as the journal records no
 *  call of a kind with KF_RERUN. Returns false for any other call, and for
 *  the call of a client or a job. This function is not exported.
 */
extern bool
mcp_slot_rerun (struct mcp_slot const *slot);

/** The numbered lines of a committed file in the output of a git read or a
 *  code describe (mcp_slot_source()). The texts are views of the caller's
 *  storage.
 */
struct call_source {
	struct cstr_view repo;    /**< repository name */
	struct cstr_view commit;  /**< full commit ID */
	struct cstr_view path;    /**< path of the file in the commit */
	uint64_t         total;   /**< line count of the file, or 0 if
	                               unknown */
	size_t           at;      /**< offset of the numbered lines in the
	                               output */
	size_t           len;     /**< bytes of the numbered lines. There is one
	                               output line for each line from first to
	                               last, and each output line ends with a
	                               newline, except perhaps the last. */
	uint32_t         first;   /**< number of the first line, from 1 */
	uint32_t         last;    /**< number of the last line */
};

/** Stores a copy of @p src, which describes the numbered lines of a
 *  committed file in the call's output, written or lent, for the ring of
 *  the calls of the ask call (seen_keep()). It stores nothing for the call
 *  of a client, or if @p src is a single line longer than
 *  mcp_slot_window_max().
 *  Returns 0, EFAULT for nullptr, EINVAL if the call has ended or @p src
 *  does not fit the output (seen_read_create()), or ENOMEM. This function
 *  is not exported yet.
 */
extern int
mcp_slot_source (struct mcp_slot          *slot,
                 struct call_source const *src);

/** Records for the ring of the calls of the ask call (seen_print()) that
 *  the @p len bytes of the call's output from @p at on print the evidence
 *  item @p item. When askai writes the result, the bytes of the window
 *  that continue the bytes of the item shown before count as shown
 *  (seen_shown()). It records nothing for the call of a client or a job.
 *  Returns 0, EFAULT for nullptr, EINVAL if the call has ended or for the
 *  errors of seen_print(), or ENOMEM. This function is not exported.
 */
extern int
mcp_slot_evidence (struct mcp_slot        *slot,
                   struct seen_item const *item,
                   size_t                  at,
                   size_t                  len);

/** Sets the notebook that the call's output shows to @p notebook, the
 *  name of a member and its kind, such as "bro (agent)", which must stay
 *  valid until the kind's drop(). After the result of a turn's call that
 *  does not fail, askai adds a notice that names the notebook, and the
 *  journal records the notice. Returns 0, EFAULT for nullptr, or EINVAL if
 *  the call has ended. This function is not exported.
 */
extern int
mcp_slot_shows (struct mcp_slot *slot,
                char const      *notebook);

/** Returns the struct member_call of the request of the call
 *  (src/member.h): the login of its client, and for an ask call, the IDs
 *  and the asker that the commits of its turns name. Returns nullptr for a
 *  null @p slot. This function is not exported.
 */
extern struct member_call *
mcp_slot_member (struct mcp_slot *slot);

/** Sets the structured content of the result of a client's call to the
 *  JSON object @p json, whose bytes must stay valid until the kind's
 *  drop(). The result then carries the object as structuredContent beside
 *  its text. The function does nothing for the call of a turn or a job.
 *  Returns 0, EFAULT for nullptr, or EINVAL if the call has ended. This
 *  function is not exported.
 */
extern int
mcp_slot_structured (struct mcp_slot  *slot,
                     struct cstr_view  json);

/** Records that the call wrote in the session: a note in the caller's own
 *  notebook, or a finding. The trace of the ask call then names the
 *  conversation. The fact goes into the journal and the checkpoint.
 *  Returns 0, EFAULT if @p slot is nullptr, or EINVAL if the call has
 *  ended. This function is not exported.
 */
extern int
mcp_slot_noted (struct mcp_slot *slot);

/**
 * Attaches a file to the turn of the call, for the attach action of the
 * notes tool. @p id is the object id of the note, the key of the file in
 * the table of uploads (src/uploads.h).
 *
 * If file.data is nullptr, the function attaches the file that askai
 * uploaded for @p id and the owner of the ask call, if the upstream still
 * stores it. It stores the size of the file in @p *bytes and returns 0, or
 * returns ENOENT if the table contains no such file. Otherwise it sends
 * @p file to the file store of the upstream as the HTTP request of the
 * call and returns EINPROGRESS. The kind then parks the call, and its
 * step() reads the answer with mcp_slot_attached().
 *
 * If the context of the ask call has no room for the file, the function
 * appends why to @p out and returns EFBIG. If askai sends too many HTTP
 * requests at once, it appends why to @p out and returns EAGAIN. It
 * returns EFAULT for nullptr, EINVAL if the call has ended, ENOTSUP if the
 * upstream takes no files, and ENOMEM or the error of the request if those
 * fail. This function is not exported.
 */
extern int
mcp_slot_attach (struct mcp_slot  *slot,
                 struct oid const *id,
                 struct cstr_view  file,
                 size_t           *bytes,
                 struct dstr      *out);

/**
 * Takes the answer to the upload that mcp_slot_attach() sent for the call.
 * The function attaches the uploaded file of @p bytes bytes to the turn of
 * the call, keeps it in the table of uploads under the object id @p id if
 * the upstream says when it expires, and returns 0. With --verbose, it logs
 * the upload. It returns EINPROGRESS while the answer has not come. If the
 * upload failed, it appends why to @p out and returns EIO. It returns
 * EFAULT for nullptr, EINVAL if the call has ended, the error of
 * mcp_slot_fetched(), or ENOMEM. This function is not exported.
 */
extern int
mcp_slot_attached (struct mcp_slot  *slot,
                   struct oid const *id,
                   size_t            bytes,
                   struct dstr      *out);

/** Makes the call wait. start() calls it last, if the call goes on after
 *  start() returns. askai can free the call's arguments in this function.
 *  For a kind with TOOL_STREAM, the reply becomes an event stream if the
 *  client accepts one. Returns 0, EFAULT if @p slot is nullptr, or EINVAL
 *  if the call has ended.
 */
EXPORT
extern int
mcp_slot_park (struct mcp_slot *slot);

/** Queues the step() of the call's kind as a bulk step of the loop.
 *  Returns 0, EFAULT if @p slot is nullptr, or EINVAL if the call has ended.
 *  If the kind has no step(), the function ends the call as failed and
 *  returns EINVAL.
 */
EXPORT
extern int
mcp_slot_ready (struct mcp_slot *slot);

/**
 * Sends @p request for the call through askai, which copies the request
 * and the bytes that its members point to before the function returns.
 * The kind's start() then parks the call. When the response has come, or
 * the request has failed, askai queues the step() of the call's kind, and
 * step() reads the result with mcp_slot_fetched(). If the call ends before
 * that, askai cancels the request, unless request->keep is not 0.
 *
 * askai sends the header User-Agent with the value "askai (plugin NAME)",
 * where NAME is the name in the plugin's manifest, and the header Accept
 * with the value of request->accept. If the request has a body, askai
 * sends the header Content-Type with the value application/json. askai
 * follows no redirect.
 *
 * If request->keep is not 0, askai can use a response that it received for
 * an equal request, of any user and any loop, instead of sending the
 * request. Two requests are equal if they come from the same plugin and
 * have the same method, URL, Accept, max and body. askai uses a stored
 * response only if it received the response less than request->keep
 * seconds ago. A request that is equal to a request under way, with the
 * same timeout, waits for the response to that request if the keep of both
 * is not 0. askai stores a response only if the request's keep is not 0,
 * the status is 200 to 299, and the whole body came. A stored response
 * stays until the stored responses need more than 64 MiB of memory, or
 * until memory pressure. Then askai frees first the responses whose keep
 * has passed, the keep being the largest keep of the requests that waited
 * for the response.
 *
 * askai asks for a compressed response with the header Accept-Encoding,
 * and decodes it. request->max is the length of the decoded body.
 *
 * Returns 0, and the new request then replaces the response to the call's
 * previous request. On failure the function returns an error number:
 * - EFAULT if @p slot, @p request or request->url is nullptr, or if
 *   request->body.data is nullptr and request->body.len is not 0.
 * - EINVAL if the call has ended, if a member of @p request is out of its
 *   range (src/web.h), or if the kind has no step(). If the kind has no
 *   step(), the function also ends the call as failed, as mcp_slot_ready()
 *   does.
 * - EBUSY if a request of the call is under way.
 * - EAGAIN if askai sends WEB_RUNS requests already.
 * - ENOMEM if askai runs out of memory.
 */
EXPORT
extern int
mcp_slot_fetch (struct mcp_slot          *slot,
                struct web_request const *request);

/**
 * Stores the result of the call's last request in @p *response. The
 * pointers in @p *response stay valid until the call's next
 * mcp_slot_fetch(), or until askai drops the call. askai drops a call
 * after the call has ended, and after the kind's drop() has returned.
 * Returns 0. On failure it returns EFAULT if @p slot or @p response is
 * nullptr, EINVAL if the call has ended, EINPROGRESS while the request is
 * under way, or ENOENT if the call has made no request. If @p response is
 * not nullptr, a failure stores zeros and null pointers in @p *response.
 */
EXPORT
extern int
mcp_slot_fetched (struct mcp_slot const *slot,
                  struct web_response   *response);

/** Stores in @p *who a handle that names the call, for a message that
 *  another thread posts with loop_post(). A call has one handle until it
 *  ends or is dropped, so a second call of this function stores the same
 *  handle. Returns 0, or ENOMEM. It returns EFAULT if @p who or @p slot
 *  is nullptr, and EINVAL if the call has ended. If it fails and @p who is
 *  not nullptr, it stores the handle 0 in @p *who.
 */
EXPORT
extern int
mcp_slot_name (struct mcp_slot *slot,
               struct handle   *who);

/**
 * In a post's handler: the call, or nullptr if it is gone, and once the
 * loop stops, when its part is gone too: on nullptr, free the message and
 * touch nothing else. A lookup, as loop_lookup(): its object is the
 * loop's table.
 */
EXPORT
extern struct mcp_slot *
mcp_slot_named (struct loop const *loop,
                struct handle      who);

/** Ends the call with @p text as its whole output, without an offset. The
 *  text is cut at TOOL_TELL_MAX bytes, or before its first byte that is not
 *  part of UTF-8. With TOOL_FAILED in @p flags, the call fails. Returns
 *  0, EFAULT if @p slot is nullptr or @p text has a length and no bytes, or
 *  EINVAL if the call has ended or @p flags has other bits than
 *  TOOL_FAILED.
 */
EXPORT
extern int
mcp_slot_tell (struct mcp_slot  *slot,
               struct cstr_view  text,
               uint32_t          flags);

/** Formats a text from @p fmt and its arguments as printf() does, and ends
 *  the call with the text as mcp_slot_tell() does. A text of more than
 *  TOOL_TELL_MAX - 1 bytes is cut and ends with "...". Returns 0, EFAULT
 *  if @p slot or @p fmt is nullptr, or EINVAL as mcp_slot_tell() does.
 */
EXPORT
[[gnu::format(printf, 3, 4)]]
extern int
mcp_slot_tellf (struct mcp_slot *slot,
                uint32_t         flags,
                char const      *fmt,
                ...);

/** Ends the call with the output that mcp_slot_write() wrote. A client's
 *  call gets the output from the offset that mcp_slot_offset() took. A
 *  turn's call gets the part of the output that the turn takes. With
 *  TOOL_FAILED in @p flags, the call fails. Returns 0, EFAULT if @p slot is
 *  nullptr, or EINVAL if the call has ended or @p flags has other bits than
 *  TOOL_FAILED.
 */
EXPORT
extern int
mcp_slot_finish (struct mcp_slot *slot,
                 uint32_t         flags);

/**
 * Ends the call and its request with the error number @p e. The reply is
 * an HTTP 503 error for ESHUTDOWN and an HTTP 500 error, "Out of memory",
 * for any other error number. For a call of a turn, or of a job that the
 * turns run in the background, the request is the ask call. If the reply
 * of the ask call is already being sent, only the call ends. Returns 0,
 * EFAULT if @p slot is nullptr, or EINVAL if the call has ended.
 *
 * A call that fails for a reason of its own ends with mcp_slot_tell() or
 * mcp_slot_finish() and TOOL_FAILED instead.
 */
EXPORT
extern int
mcp_slot_fail (struct mcp_slot *slot,
               int              e);

#endif /* ASKAI_SRC_MCP_H_ */
