/** @file mcp.c
 *
 * MCP's Streamable HTTP transport and JSON-RPC semantics, over request
 * streams (src/stream.h).
 *
 * Two protocol eras are served side by side. Legacy clients (revisions up to
 * 2025-11-25, which includes llama.cpp) open with an initialize handshake;
 * modern clients (2026-07-28) are stateless and declare their version in each
 * request's _meta, mirrored into HTTP headers that must match the body.
 *
 * A request's body is copied and pre-scanned for its JSON's nesting, values,
 * object members and number lengths as it arrives, then parsed within the
 * step's share of work (loop.h), a slice a bulk step once it exceeds that, by
 * tokeners the loop reuses. A large message is freed the same way, by its own
 * request after the reply has gone out; the request's stream ends only then,
 * so its client's next request waits for that, and memory for messages is
 * bounded by the requests under way. An upstream answer is parsed and freed
 * in slices too. Replies are built from fragments, the unchanging ones
 * serialized once in the shared, immutable mcp_static, so no json-c value is
 * shared between replies or loops, and go out a piece a step, each piece once
 * the transport has room for it. A finished request is kept for reuse, its
 * buffers' storage given back to the loop's stash (src/buf.h) and that of its
 * answer and pre-scan kept, until memory pressure has it freed; the one that
 * ended last is reused first, so steady requests reuse the same few. So are
 * jobs.
 *
 * A tool call's upstream request is a job that home runs (src/job.h): the
 * request writes the upstream body, then hands it to a job, names itself by a
 * handle (src/loop.h) and submits the job. The job's completion looks the
 * request up by that handle, and is dropped if the request has gone; else the
 * request parses the answer as before, on this loop, and then lets go of the
 * job. A request that goes while it waits cancels its job, freeing its
 * handle.
 *
 * A tool call answers on an event stream (SSE) when the client accepts one
 * and its era has them (all but 2024-11-05): id-less data: events, and a ':'
 * comment every keepalive seconds while the upstream works. A call whose
 * params._meta has a progressToken gets notifications/progress on its event
 * stream while it waits, at most one each PROGRESS_NS, as home's PROGRESS
 * messages say how far it is: its place in its principal's queue, then the
 * chunks of its answer, which home reads from a streamed answer into its
 * text, so that the request only escapes that into the reply. An ask call
 * with params._meta.askai.stream true also receives notifications/askai/delta:
 * bounded text or reasoning pieces copied from an immutable job mailbox.
 * Home waits for its acknowledgment. The transcript writer stores decoded
 * reasoning independently of these transport notifications. Closing the
 * stream, which in HTTP/1.1 means closing the connection or its sending side,
 * cancels the call in every era. The caller of a call can also cancel it
 * with notifications/cancelled, on any connection of any loop.
 * mcp_req_cancel() looks the call up in the roster (src/roster.h) by the
 * caller, the session and the request id.
 *
 * The man, git and notes tools are built-in plugins (src/plugin.h). Their
 * calls go through the call API (src/mcp.h). The three tools are in
 * src/manual.h, src/git.h and src/notes_tool.h.
 *
 * A test_task call, when askai offers the test aid, runs CPU work of the
 * slices it asks for as a task (src/task.h), which may run on any client
 * loop, and waits for it, named by a handle likewise; a call that goes
 * cancels the task.
 *
 * With --client-jobs, a client's call of background runs a call of another
 * tool as a job. The request answers the client with the job's id, and
 * then runs the call with a stand-in stream, which writes the reply into
 * the job's record in the ledger (src/ledger.h), as a call that becomes a
 * task does. A client's timers and waits are in the ledger too.
 *
 * askai answers GET /metrics with the metrics if the grant of the client
 * permits it (src/policy.h), and renders a share of them in each step
 * (src/metrics.h). Draining, tool calls that wait for the upstream, and
 * those yet to start, end with an error, which on an event stream is its
 * last event.
 */
#define _GNU_SOURCE /* memrchr() */
#include <errno.h>
#include <inttypes.h>
#include <json-c/printbuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ai.h"
#include "buf.h"
#include "clk.h"
#include "config.h"
#include "dstr.h"
#include "findings.h"
#include "git.h"
#include "home.h"
#include "instance.h"
#include "jb.h"
#include "job.h"
#include "journal.h"
#include "ledger.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "manual.h"
#include "mcp.h"
#include "metrics.h"
#include "notes.h"
#include "notes_tool.h"
#include "notice.h"
#include "plugin.h"
#include "policy.h"
#include "registry.h"
#include "review.h"
#include "roster.h"
#include "schema.h"
#include "shelf.h"
#include "stream.h"
#include "task.h"
#include "uploads.h"
#include "util.h"
#include "web.h"

enum {
	PARSE_ERROR         = -32700,
	INVALID_REQUEST     = -32600,
	METHOD_NOT_FOUND    = -32601,
	INVALID_PARAMS      = -32602,
	INTERNAL_ERROR      = -32603,
	HEADER_MISMATCH     = -32020,
	MISSING_CAPABILITY  = -32021,
	UNSUPPORTED_VERSION = -32022,
};

enum {
	MAX_ID       = 1 << 10,   /* string request id bytes: every reply repeats it */
	MAX_ECHO     = 256,       /* bytes of a client's string an error repeats */
	MAX_SCALAR   = 1 << 10,   /* bytes of a JSON number or literal, which json-c
	                             converts whole: no valid one needs more */
	CACHE_TTL_MS = 300'000,   /* how long modern clients may cache lists */
	UPLOAD_MAX   = 16 << 20,  /* bytes of a conversation's upstream request:
	                             past those, its context is full */
	CHAT_HISTORY = 1024,      /* earlier visible messages of a chat */
	CALLS_MAX    = 16 << 20,  /* bytes of a turn's tool calls */
	LEND_MAX     = 64 << 10,  /* bytes of a tool's output that a turn takes,
	                             at most */
	SUMMARY_MAX  = 4 << 20,   /* bytes of compact's summary */
	JOBS_MAX     = 8,         /* jobs that an ask call's turns may start */
	WAIT_MAX     = 600,       /* seconds that a wait may take */
	WAIT_IDS     = 16,        /* jobs that a client's wait may name */
	TIMER_MAX    = 3600,      /* seconds that a timer may be set to */
	TRACE_MAX    = 48,        /* tool calls the trace tells of */
	TRACE_ARGS   = 200,       /* bytes of a call's arguments in the trace
	                             and in the log */
	SHOWN        = TRACE_ARGS + sizeof "...",  /* bytes of those in the log,
	                                              with their mark and NUL */
	PROMPT_LOG   = 100,       /* bytes of a prompt that the log shows, as
	                             escaped JSON */
	ERROR_LOG    = 200,       /* bytes of a failure's text that it shows,
	                             likewise */
	NAME_LOG     = 100,       /* bytes of a tool's name from a client or the
	                             upstream that it shows, likewise */
	CALL_ID      = sizeof "call_4294967295_4294967295",  /* bytes of a
	                                                        made-up call id */
	POLL_MS      = 1000,      /* between a task's polls, as its client is asked */
	ISO_TIME     = sizeof "2026-10-13T12:34:56.789Z",
};

/* The upstream requests of a client's jobs are in home's background class,
 * and home queues at most JOB_QUEUED requests of one principal in one
 * class. The principal of a job's requests is the owner of the job's
 * record, so home can queue a request of each job of an owner that runs.
 */
static_assert((int)LEDGER_RUNNING <= (int)JOB_QUEUED, "home queues the jobs that run");

static constexpr uint64_t PROGRESS_NS = 100'000'000;  /* between a call's
                                                         notifications */
static constexpr uint64_t TASK_NS = 1'000'000'000;  /* a call's run before it
                                                       becomes a task, for a
                                                       client that takes them */

/** The protocol revisions that askai supports, newest first. The clients
 *  of the first MODERN revisions send their version in _meta. The last,
 *  PRE_STREAMS, is the revision before Streamable HTTP, whose clients do
 *  not accept event streams.
 */
static struct cstr_view const versions[] = {
	CSTR_VIEW_INIT("2026-07-28"),
	CSTR_VIEW_INIT("2025-11-25"),
	CSTR_VIEW_INIT("2025-06-18"),
	CSTR_VIEW_INIT("2025-03-26"),
	CSTR_VIEW_INIT("2024-11-05"),
};
enum { MODERN = 1, PRE_STREAMS = ARRAY_SIZE(versions) - 1 };

static char const meta_version[] = "io.modelcontextprotocol/protocolVersion";
static char const meta_caps[] = "io.modelcontextprotocol/clientCapabilities";
static char const meta_server[] = "io.modelcontextprotocol/serverInfo";
static char const meta_tasks[] = "io.modelcontextprotocol/tasks";
/* The extension by which a client hands askai a file that a model does not
 * write: a data: URI in an argument that x-mcp-file marks.
 */
static char const meta_files[] = "io.modelcontextprotocol/files";

/* The bytes of the files that one request may hand askai inline, at most,
 * once decoded (maxInlineBytes of the extension), if --max-body has room
 * for them in base64 and FILES_ROOM bytes of the rest of the request.
 */
enum { FILES_INLINE = 4 << 20, FILES_ROOM = 64 << 10 };

/** The header field of a JSON response. */
#define JSON_FIELD "Content-Type: application/json\r\n"

/** The request header fields read here, in field() order. */
enum { F_TYPE, F_ACCEPT, F_VERSION, F_METHOD, F_NAME, F_SESSION, F_N };

static char const *const field_names[F_N] = {
	[F_TYPE]    = "content-type",
	[F_ACCEPT]  = "accept",
	[F_VERSION] = "mcp-protocol-version",
	[F_METHOD]  = "mcp-method",
	[F_NAME]    = "mcp-name",
	[F_SESSION] = "mcp-session-id",
};

/* The kinds of tool that askai may offer, each its row of mcp_rows. A role
 * can grant the kinds up to KIND_NOTES, which clients list and call. askai
 * offers the kinds in KINDS_TURNS only to the turns of ask calls. It offers
 * the kinds in KINDS_AGENDA to the turns, with --upstream-tools, and with
 * --client-jobs also to clients, as a role grants them. A kind's tool has
 * one id for both, and the start() of such a kind checks the role of its
 * slot: ROLE_OWN is a client's call. KIND_NONE is the kind of a tool that
 * askai does not offer.
 */
enum mcp_kind : uint32_t {
	KIND_ASK,
	KIND_MAN,
	KIND_TASK,
	KIND_GIT,
	KIND_NOTES,
	KIND_COMPACT,
	KIND_FINDINGS,
	KIND_BACKGROUND,
	KIND_JOBS,
	KIND_WAIT,
	KIND_TIMER,
	KIND_NONE,
	KINDS
};

/* The kinds of object that the handles of a client loop name
 * (loop_handle()). loop_lookup() returns an object only for the kind that
 * its handle was made with, so a handle of a request never gives a call
 * slot, and the reverse.
 */
enum mcp_handle : uint32_t {
	HANDLE_REQ = 1,  /* a struct mcp_req */
	HANDLE_SLOT,     /* a struct mcp_slot */
};

/* The rule by which askai lends a kind of tool to the turns.
 * mcp_lending_lent() applies the rules.
 */
enum mcp_lend : uint8_t {
	LEND_NEVER,    /* never */
	LEND_GRANTED,  /* if the client's role lets the upstream call it */
	LEND_ALWAYS,   /* always */
	LEND_WITH,     /* if the turns get a LEND_GRANTED or LEND_ALWAYS tool */
	LEND_DETACH,   /* if the turns get a LEND_GRANTED or LEND_ALWAYS tool
	                  with TOOL_DETACH */
};

/* What the trace tells of a call besides its tool's name: its arguments'
 * JSON text; of a notes call's, which are the upstream's own, only the
 * action; or nothing.
 */
enum mcp_trace : uint8_t {
	TRACE_JSON,
	TRACE_ACTION,
	TRACE_NAME,
};

/* Kind flag (bit 33, above TOOL_FLAGS) for the kinds in KINDS_AGENDA. A
 * client's background call cannot start them. A client's call of them never
 * becomes a task. askai does not resume an interrupted turn that called one.
 */
static constexpr uint64_t KF_AGENDA = 1ull << 33;

/* Kind flag for findings (bit 34, above TOOL_FLAGS). A findings call
 * changes only the review record, and the job checkpoint saves the record.
 * The journal does not record the call, so a resumed turn runs it again.
 */
static constexpr uint64_t KF_RERUN = 1ull << 34;

/* The row of a kind of tool in mcp_rows. */
struct mcp_row {
	struct tool_kind     kind;      /* the functions of the tool, if askai
	                                   implements the tool itself */
	struct plugin const *plugin;    /* the built-in plugin whose only tool
	                                   this is, or nullptr */
	tool_definition_fn  *upstream;  /* the definition for the turns, if it
	                                   differs from the one that clients
	                                   list */
	char const          *sized;     /* the argument whose size the log
	                                   shows instead of its value */
	char const          *option;    /* the option that turns the tool on,
	                                   for the log. It is nullptr for the
	                                   kinds in KINDS_TURNS and the test
	                                   aid, which the log leaves out while
	                                   they are off. */
	struct cstr_view     name;      /* the name; name.data is nullptr for
	                                   the ask tool, whose name comes from
	                                   the configuration */
	uint8_t              lend;      /* enum mcp_lend: how the tool is lent
	                                   to the turns. For a tool of a plugin
	                                   with LEND_NEVER here, mcp_lend_of()
	                                   uses the flags of its kind. */
	uint8_t              metric;    /* enum metrics_lent: the row of the
	                                   tool in the metrics of the turns */
	uint32_t             trace;     /* enum mcp_trace: what the trace shows
	                                   of the calls, widened over the
	                                   padding */
};

/* A tool that askai offers. listed is its definition as tools/list lists
 * it, if a role may grant it, and lent is its definition in the API of the
 * upstream, if a mask of st->lending other than by[LEND_NEVER] contains
 * the tool. Each definition begins with a comma. tool is the entry of the
 * tool in the registry. mcp_row_of(&tool) returns the row with the other
 * attributes of the tool.
 */
struct mcp_offer {
	struct dstr          listed;
	struct dstr          lent;
	struct registry_tool tool;
};

/* Each member is a mask of the ids of tools in cfg->tools_upstream. by[l]
 * contains the tools with the lending rule l (enum mcp_lend), of[k] the
 * tool of kind k (enum mcp_kind), and detach the tools that can run in the
 * background (TOOL_DETACH). mcp_lending_lent() selects the lent tools.
 */
struct mcp_lending {
	tool_mask by[LEND_DETACH + 1];
	tool_mask of[KINDS];
	tool_mask detach;
};

/* Returns the tools of @p l that the turns of an ask call may call, by the
 * rules of enum mcp_lend. @p upstream is the mask of the tools that the
 * role of the client grants to the upstream.
 */
static tool_mask
mcp_lending_lent (struct mcp_lending const *l,
                  tool_mask                 upstream)
{
	tool_mask lent = (upstream & l->by[LEND_GRANTED]) | l->by[LEND_ALWAYS];
	tool_mask more = lent & l->detach ? l->by[LEND_DETACH] : 0;
	return lent ? lent | l->by[LEND_WITH] | more : 0;
}

/* A plugin whose tools askai offers. entry is the entry of the plugin in
 * the registry, whose member loaded is nullptr for a built-in plugin. A
 * built-in plugin can provide a service, the object that every loop
 * shares, and a part of the service on each client loop. A plugin that
 * askai loaded from a file provides neither.
 */
struct mcp_plugin {
	struct registry_plugin  entry;
	void                   *service;
	mcp_part_create_fn     *part_create;
	mcp_part_destroy_fn    *part_destroy;
};

struct mcp_static {
	struct ai_endpoint const *ep;
	struct mcp_tool const    *tool;   /**< the ask tool's, as configured */
	struct mcp_offer          offers[TOOL_MAX + 1];  /**< by id; TOOL_MAX's,
	                                                      that of a tool askai
	                                                      does not offer */
	/** The plugins that have a tool, indexed by part. There is at most
	 *  one plugin for each offer. Entry 0 is the plugin of askai's own
	 *  kinds, and its service is this mcp_static. */
	struct mcp_plugin         plugins[TOOL_MAX];
	uint32_t                  n_offers;
	uint32_t                  n_granted;      /**< those before the first
	                                               that only turns are given */
	struct mcp_lending        lending;
	struct dstr               list_tail;    /**< closes a modern tools/list
	                                             result after its tools */
	struct dstr               discover;     /**< the server/discover result */
	struct dstr               init_tail;    /**< an initialize result after the
	                                             version */
	struct dstr               modern_tail;  /**< closes a modern result */
	struct dstr               server_meta;  /**< a modern result's _meta
	                                             member */
	struct dstr               versions;     /**< the supported versions, an array */
	struct task_pool         *tasks;        /**< test_task's, if askai offers
	                                             it */
	struct notes_tool         notes_tool;   /**< the notes tool's service */
	struct journal           *journal;      /**< reasoning and recovery state */
	struct ledger            *ledger;       /**< the tasks' */
	struct roster            *roster;       /**< the tool calls that run and
	                                             are not tasks */
	struct web               *web;          /**< the plugins' HTTP requests */
	size_t                    files_inline; /**< bytes of a file that a client
	                                             hands askai inline, at most:
	                                             FILES_INLINE, or less, as
	                                             --max-body says */
	struct uploads           *uploads;      /**< the files that turns
	                                             attached, if the upstream
	                                             takes files (ai_upload()) */
	char                     *files_url;    /**< the URL of their uploads */
	char const              **files_lines;  /**< the header lines of their
	                                             uploads, nullptr-terminated */
	size_t                    files_url_len;
	uint64_t                  seed;         /**< of the calls' keys' hash */
	struct mcp_limits         lim;
	uint32_t                  turns;        /**< upstream calls an ask call
	                                             makes at most */
	uint32_t                  n_plugins;
};

/** Where a request stands. */
enum mcp_phase {
	PHASE_BODY,      /* taking the body in */
	PHASE_PARSE,     /* parsing it, a slice a step */
	PHASE_DISPATCH,  /* handing the message on, in a step of its own */
	PHASE_BUILD,     /* writing the upstream request, a slice a step */
	PHASE_JOURNAL,   /* waiting for durable storage, off the loop */
	PHASE_CALL,      /* waiting for the upstream */
	PHASE_TOOL,      /* waiting for its own call, which works in its slot's
	                    steps */
	PHASE_CALLS,     /* running a turn's tool calls, each in a slot of its
	                    own, and writing their results in order */
	PHASE_DOZE,      /* its turns answered: waiting for what their jobs and
	                    timers do next */
	PHASE_ANSWER,    /* parsing its answer, a slice a step */
	PHASE_TEXT,      /* taking the tool's output from that, in a step of its own */
	PHASE_ARGS,      /* parsing a turn's tool calls' arguments, a slice a step */
	PHASE_TURN,      /* writing the turn into the upstream request, likewise */
	PHASE_RESULT,    /* writing a tool call's result into the request */
	PHASE_ADD,       /* writing what askai adds to the prompt into it */
	PHASE_WAKE,      /* writing the message that wakes the turns, likewise */
	PHASE_SHOW,      /* copying a task's result into the reply, likewise */
	PHASE_REPLY,     /* escaping the output into the reply, a slice a step */
	PHASE_SEND,      /* sending the reply, a piece a step */
	PHASE_END,       /* waiting for its end record to be durable, off the
	                    loop; nothing but that changes how it ended */
	PHASE_FREE,      /* freeing the message, a slice a step, then ending */
	PHASE_METRICS,   /* rendering the metrics, a share a step */
};

/* Where a client's string goes in the upstream request. */
struct mcp_splice {
	struct json_object *str;
	size_t              at;  /* offset into the draft's text */
};

struct mcp_req;

/* Whose a slot's call is, which says where its output goes. */
enum mcp_role : uint8_t {
	ROLE_OWN,   /* the request's own: the client's call, or the request's */
	ROLE_TURN,  /* one of a turn's calls, whose result a turn takes */
	ROLE_JOB,   /* a job a turn started in the background */
};

/* A tool call that a request runs. It is the client's call or a call of a
 * turn. req->self also holds the answer of an ask call. The notes git
 * commands that an ask call runs for itself also use req->self.
 */
struct mcp_slot {
	struct loop_step    step;      /* the step that runs kind->step() */
	struct dstr         answer;    /* the output of the call. Its storage
	                                  is kept for reuse. */
	struct mcp_req     *req;       /* the request that runs the call */
	void               *own;       /* the call's own word (mcp_slot_own()).
	                                  The kind's drop() releases it. */
	void               *inst;      /* The instance of the tool's plugin for
	                                  the user of the request, or nullptr
	                                  if the plugin has no instances.
	                                  mcp_slot_start() sets it with
	                                  instance_table_get(), and
	                                  mcp_slot_drop() clears it with
	                                  instance_table_put(). */
	struct span         lent;      /* the output text if own holds it, such
	                                  as the page text of a man call */
	struct web_wait    *web;       /* the last HTTP request of the call, in
	                                  progress or answered, until the next
	                                  request or mcp_slot_drop() */
	size_t              skip;      /* the output offset at which the window
	                                  given to a turn starts */
	size_t              window;    /* the length of that window in bytes */
	uint64_t            start;     /* loop_clock() when the call began */
	struct handle       call;      /* the handle that mcp_slot_name() makes
	                                  for the post that completes the call */
	struct json_object *args;      /* the arguments of a job, for the jobs tool */
	struct mcp_slot    *of;        /* the job whose output it gives, if any */
	struct review_read *read;      /* the lines of a file that its output
	                                  shows, for the review record
	                                  (mcp_slot_source()), if any */
	char const         *shows;     /* the selector of the notebook that a
	                                  notes read or diff shows, or nullptr.
	                                  mcp_slot_done() puts it in the notice
	                                  and the journal and then clears it. */
	uint16_t            note_len;  /* the length of note in bytes */
	uint8_t             tool;      /* the offer id of the tool, or TOOL_MAX */
	uint8_t             role;      /* enum mcp_role */
	bool                done;      /* true once the call has ended. A
	                                  call ends when it has its output,
	                                  when it fails, when it is
	                                  cancelled, and when askai calls
	                                  its kind's drop(). The functions
	                                  of the call API that would end or
	                                  change a call do nothing for an
	                                  ended call. */
	bool                failed;    /* the call failed */
	bool                cancelled; /* the job was cancelled */
	char                note[513]; /* text appended to the tool output. Its
	                                  size fills the trailing padding. */
};

static_assert(TOOL_MAX <= UINT8_MAX, "a slot's tool holds TOOL_MAX");

struct mcp_agenda;

static void mcp_agenda_destroy (struct mcp_agenda **p_agenda);

/** A request, from its stream's head to its end or cancellation. */
struct mcp_req {
	struct list          hook;       /* on mcp->calls while a tool call runs,
	                                    on mcp->ending while its end record
	                                    is written, then on mcp->freeing,
	                                    then spare */
	struct list          task_hook;  /* on mcp->tasks while the call is a
	                                    task */
	struct loop_step     step;       /* the phase's work, and keep-alives */
	struct loop_timer    timer;      /* the next keep-alive */
	struct buf           body;       /* the request body */
	struct buf           upload;     /* the upstream request, until its job
	                                    takes it */
	struct buf           reply;      /* our response body, until sent */
	struct jb_parser     parser;     /* of the body, then of the answer */
	struct jb_reaper     reaper;     /* what is freed a slice a step */
	struct mcp_splice   *splice;     /* the places of the strings of the
	                                    client in the draft, in draft order.
	                                    A spare request reuses the array. */
	struct mcp_slot      self;       /* its own call: its answer, the tool
	                                    that a client calls, and the notes'
	                                    git it reads */
	struct dstr          trace;      /* the tool calls the turns made, for the
	                                    client; likewise */
	struct buf           calls;      /* a turn's tool calls, as its job had
	                                    them, while they run */
	struct buf           attached;   /* the files that a turn's calls
	                                    attached, as its API's fragments
	                                    have them, until its last result */
	struct mcp_slot     *slots;      /* JOB_CALLS slots for the tool calls
	                                    of a turn, from the first turn with
	                                    calls until mcp_req_finish() */
	struct mcp_agenda   *agenda;     /* the jobs and timers of the turns,
	                                    from the first background, wait or
	                                    timer call until mcp_req_finish() */
	struct review       *review;     /* the review record if the turns are
	                                    lent findings (mcp_req_review_make()) */
	struct ledger_task  *task;       /* the ledger record of the call once it
	                                    becomes a task or a job */
	struct ledger_task  *admitted;   /* durable admission, before its reply */
	struct journal_record *recovered; /* current turn's records, until submit */
	struct json_object  *saved_tools[JOB_CALLS]; /* completed calls to replay */
	struct handle        archive_call; /* an asynchronous journal operation */
	uint64_t             archive_deadline; /* absolute realtime deadline */
	uint64_t             replay_filed; /* current turn's restored attachments */
	uint32_t             intents;    /* tool intents made durable this turn */
	int                  archive_error;
	bool                 detach_pending;
	bool                 archived_end;
	bool                 recovering;
	bool                 suspended;
	bool                 withdrawn;  /* a notifications/cancelled ended it */
	char                 archive[LEDGER_ID + 1];
	struct ledger_task  *shown;      /* the record whose result tasks/get or
	                                    a client's jobs copies, while it
	                                    does */
	struct roster_call  *rostered;   /* the record of the request's tool
	                                    call in the roster, from the start
	                                    of the call until the call ends or
	                                    becomes a task */
	struct stream       *s;          /* nullptr once it is gone; stand_in
	                                    once the call is a task */
	struct mcp          *mcp;
	struct job          *job;        /* the tool call's, once submitted, until
	                                    let go of */
	struct json_object  *msg;        /* parsed, until dispatched; then the
	                                    upstream's answer, until read */
	struct json_object  *id;         /* the request's id, if any */
	struct json_object  *token;      /* a tool call's progressToken, if any */
	struct json_object  *draft;      /* the upstream request without the
	                                    client's strings, until written */
	struct json_object  *prompt;     /* the client's prompt, which the draft
	                                    holds, until written */
	struct json_object  *args[JOB_CALLS];  /* a turn's tool calls' arguments,
	                                          each until its call; nullptr
	                                          if they are no object */
	struct cstr_view     text;       /* the serialized draft */
	size_t               off;        /* bytes of the next splice written, of
	                                    the answer escaped, or metrics done */
	size_t               values;     /* of msg */
	size_t               prompt_at;  /* where the prompt's text ends in the
	                                    upload: what askai adds goes there */
	size_t               sent;       /* bytes of the upload as a turn went */
	size_t               filed;      /* bytes of the files that the turns
	                                    attached since the last turn's usage,
	                                    which the upload does not hold */
	size_t               at;         /* where the call whose result is written
	                                    next is in calls */
	size_t               launch_at;  /* and the call launched next */
	size_t               delta_at;   /* header of the published mailbox piece */
	size_t               delta_read; /* bytes consumed from that piece */
	size_t               replayed;   /* bytes of the job's replay that the
	                                    turn has written */
	uint64_t             head;       /* loop_clock() at the request's head */
	uint64_t             ping_at;    /* when the next keep-alive is due */
	uint64_t             promote_at; /* when the call becomes a task, if its
	                                    client takes them */
	uint64_t             noted;      /* when the last notification went */
	uint64_t             base;       /* what the progress was as the turn
	                                    began */
	uint64_t             principal;  /* The principal of the request's
	                                    client. mcp_begin() copies it from
	                                    the stream, because s can be nullptr
	                                    when a call is dropped. */
	uint64_t             generation; /* The value that
	                                    instance_table_generation() returned
	                                    when the request began. */
	uint64_t             asked;      /* its ask call's number among those of
	                                    the loop, for the log; 0: none */
	uint64_t             turn_at;    /* loop time as its last turn was
	                                    submitted */
	tool_mask            lent;       /* the tools its turns may call */
	struct stream        stand_in;   /* a task's stream, which writes the
	                                    reply into its record */
	struct handle        call;       /* names the request to its job's
	                                    completion, while it waits for that */
	struct jb_scan       scan;       /* the body's pre-scan */
	int                  error;      /* to end the stream with */
	uint32_t             place;      /* its first place in the queue that a
	                                    notification gave */
	uint32_t             next_note;  /* 1 + the last progress it gave; 0:
	                                    none */
	uint32_t             turn;       /* upstream calls made */
	uint32_t             used;       /* tokens of the last turn's request and
	                                    answer, as its usage says; 0: unknown */
	uint32_t             context;    /* tokens the upstream's context holds;
	                                    0: unknown */
	uint32_t             traced;     /* tool calls the turns made */
	uint32_t             launched;   /* the turn's calls launched */
	uint16_t             splices;    /* the entries of splice in use */
	uint16_t             splice_max; /* the entries of splice */
	uint16_t             next;       /* the splice to write next, or piece */
	uint16_t             part;       /* the part of the turn being written,
	                                    or its call under way */
	uint16_t             n_calls;    /* the turn's tool calls */
	uint8_t              phase;      /* enum mcp_phase */
	uint8_t              method;     /* enum metrics_method */
	uint8_t              asks;       /* enum job_ask: JOB_BACKGROUND for
	                                    test_task's prompt and for a
	                                    client's job */
	uint8_t              forced;     /* compactions askai made since a turn
	                                    went through */
	uint8_t              reminded;   /* the quarters of the context, 0 to 2,
	                                    at which the turns were last reminded
	                                    to write notes, since the last
	                                    compaction */
	bool                 modern;
	bool                 sse_ok;     /* the client takes an event stream */
	bool                 sse;        /* the response is one */
	bool                 failed;     /* the answer reports an error */
	bool                 tick;       /* a keep-alive or notification may be
	                                    due */
	bool                 progress;   /* the job's progress moved on */
	bool                 blocked;    /* waiting for on_writable() */
	bool                 delta;      /* the job's immutable mailbox is published */
	bool                 listed;     /* the list that the turn's items go in
	                                    has one */
	bool                 branch;     /* the caller's branch has notes */
	bool                 filing;     /* the conversation has a file that the
	                                    turns attached, since it last began
	                                    again from the prompt */
	bool                 stopped;    /* the turns ran out with tools called */
	bool                 cut;        /* the turn's answer reached the output
	                                    token limit */
	bool                 logged;     /* the end of its tool call is logged */
	char                 thread[NOTES_ID + 1];  /* the id of the thread of
	                                               the notes */
	char                 repo[NOTES_REPO];      /* the caller's own notes branch */
	char                 client[POLICY_WHO];  /* the client for the log,
	                                             which mcp_begin() writes
	                                             from its stream; "": an
	                                             anonymous TCP client */
	char                 call_args[SHOWN];  /* with --verbose, the arguments
	                                           of its tool call for the
	                                           log */
};

/* A job's archive id is also its ledger id, or a thread's id, and the
 * journal's: the same 32 hexadecimal digits.
 */
static_assert((int)LEDGER_ID == (int)NOTES_ID && (int)LEDGER_ID == (int)JOURNAL_ID &&
              sizeof ((struct job *)nullptr)->archive == LEDGER_ID + 1);

/* What an ask call's turns have under way besides themselves: the jobs
 * they started in the background, the timers they set, a wait call that
 * waits for those, what happened that the next result or message says, and
 * the answers they gave before what happened woke them.
 */
struct mcp_agenda {
	struct mcp_slot     jobs[JOBS_MAX];         /* by number - 1 */
	struct ledger_timer timers[LEDGER_TIMERS];  /* due 0: none set */
	struct loop_timer   alarm;     /* the first due: a timer, or the wait's
	                                  end */
	struct dstr         notices;   /* what happened, which none said yet */
	struct dstr         told;      /* what the result or message being
	                                  written says */
	struct dstr         said;      /* those answers, each with what woke the
	                                  turns after it, which the reply begins
	                                  with */
	struct mcp_req     *req;
	struct mcp_slot    *wait;      /* the wait call's slot, while it waits */
	uint64_t            wait_end;  /* when it waits no more */
	uint32_t            wait_for;  /* bits of the jobs it waits for; 0: any */
	uint32_t            n_jobs;
	uint32_t            n_timers;  /* timers ever set, their numbers */
	uint32_t            waited;    /* seconds the wait may take */
};

struct mcp {
	struct list               calls;      /* requests with a tool call under
	                                         way */
	struct list               tasks;      /* requests whose call is a task */
	struct list               freeing;    /* requests that free their
	                                         message */
	struct list               ending;     /* requests that write their end
	                                         record */
	struct shelf              spare;      /* finished requests */
	struct shelf              jobs;       /* jobs that requests released */
	struct loop_step          drain;      /* ends the calls when the loop
	                                         drains */
	struct stream_app         app;
	struct jb_pool            pool;       /* for request bodies */
	struct jb_pool            answers;    /* for upstream answers */
	struct metrics_src const *metrics;    /* the metrics of every loop */
	struct metrics_mcp       *counts;
	struct loop              *loop;
	struct loop              *home;       /* the loop where jobs run */
	struct mcp_static const  *st;
	struct notes_tool_loop   *notes;      /* the part of the notes tool, or
	                                         nullptr without --notes */
	struct instance_table     instances;  /* the instances of the plugins
	                                         for the users whose calls run
	                                         on this loop */
	uint64_t                  asked;      /* the number of ask calls begun
	                                         on this loop, for the log */
	uint64_t                  draining;   /* bool, widened over the padding */
	void                     *parts[];    /* the parts of the plugins, in
	                                         the order of st->plugins[] */
};

/* The thread's MCP: the handler of jobs' completions finds it here. */
static thread_local struct mcp *self;

/* Tool output for a call refused for want of room (design section 10). */
static char const busy[] = "Too many tool calls are under way; try again in a "
                           "few seconds.";

/* A run of bytes: a piece of JSON text, or words for a client or the
 * model. mcp_slot_result() writes words as text, whatever their flag.
 */
struct mcp_piece {
	struct cstr_view bytes;
	uint64_t         text;  /* bool, widened over the padding. If it is
	                           set, mcp_req_put() and mcp_req_upload()
	                           escape the bytes into a JSON string, else
	                           they write the bytes as JSON text */
};

#define FRAG(s) ((struct mcp_piece){ { s, sizeof s - 1 }, false })
#define TEXT(s) ((struct mcp_piece){ { s, sizeof s - 1 }, true })
#define DSTR(d) ((struct mcp_piece){ cstr_view_dstr(d), false })
#define BYTES(b) ((struct mcp_piece){ (b), false })
#define ESC_VIEW(v) ((struct mcp_piece){ (v), true })
#define ESC(p, n) ((struct mcp_piece){ { (char const *)(p), (n) }, true })
#define RAW(p, n) ((struct mcp_piece){ { (char const *)(p), (n) }, false })
#define SPAN(s) ((struct span){ (unsigned char *)(uintptr_t)(s), sizeof s - 1 })

/** Returns true if the field @p s is present and its bytes equal @p str. */
static bool
mcp_span_is (struct span      s,
             struct cstr_view str)
{
	return s.ptr && s.len == str.len && !memcmp(s.ptr, str.data, str.len);
}

/** Returns true if @p v is a supported revision older than the modern ones. */
static bool
mcp_is_legacy (struct cstr_view v)
{
	size_t i = cstr_view_index(versions, ARRAY_SIZE(versions), v);
	return i >= MODERN && i < ARRAY_SIZE(versions);
}

static bool
mcp_is_json (struct span type)
{
	static char const json[] = "application/json";
	size_t n = sizeof json - 1;
	return type.len >= n && !strncasecmp((char const *)type.ptr, json, n) &&
	       (type.len == n || type.ptr[n] == ';' || type.ptr[n] == ' ' ||
	        type.ptr[n] == '\t');
}

/* Whether a request's target is the metrics. */
static bool
mcp_is_metrics (struct span target)
{
	static char const path[] = "/metrics";
	size_t n = sizeof path - 1;
	return target.len >= n && !memcmp(target.ptr, path, n) &&
	       (target.len == n || target.ptr[n] == '?');
}

/** Returns the enum metrics_method of @p method. An empty method matches
 *  the empty name of METHOD_OTHER.
 */
static uint8_t
mcp_method_index (struct cstr_view method)
{
	static struct cstr_view const names[METHOD_N] = {
		[METHOD_INITIALIZE]   = CSTR_VIEW_INIT("initialize"),
		[METHOD_PING]         = CSTR_VIEW_INIT("ping"),
		[METHOD_TOOLS_LIST]   = CSTR_VIEW_INIT("tools/list"),
		[METHOD_TOOLS_CALL]   = CSTR_VIEW_INIT("tools/call"),
		[METHOD_DISCOVER]     = CSTR_VIEW_INIT("server/discover"),
		[METHOD_TASKS_GET]    = CSTR_VIEW_INIT("tasks/get"),
		[METHOD_TASKS_UPDATE] = CSTR_VIEW_INIT("tasks/update"),
		[METHOD_TASKS_CANCEL] = CSTR_VIEW_INIT("tasks/cancel"),
	};
	size_t i = cstr_view_index(names, METHOD_N, method);
	return i < METHOD_N ? (uint8_t)i : METHOD_OTHER;
}

/* Whether an Accept field lists text/event-stream. */
static bool
mcp_accepts_sse (struct span accept)
{
	static char const sse[] = "text/event-stream";
	size_t n = sizeof sse - 1;
	char const *p = (char const *)accept.ptr;
	char const *end = p + accept.len;
	while (p < end) {
		while (p < end && (*p == ' ' || *p == '\t' || *p == ','))
			++p;
		char const *comma = memchr(p, ',', (size_t)(end - p));
		char const *stop = comma ? comma : end;
		char const *semi = memchr(p, ';', (size_t)(stop - p));
		char const *type = semi ? semi : stop;
		while (type > p && (type[-1] == ' ' || type[-1] == '\t'))
			--type;
		if ((size_t)(type - p) == n && !strncasecmp(p, sse, n))
			return true;
		p = stop;
	}
	return false;
}

/* Seconds since @p start, a loop_clock(). */
static double
mcp_elapsed (uint64_t start)
{
	return (double)(loop_clock() - start) / 1e9;
}

/* Write @p len bytes of @p p, from its byte @p off, into @p buf, as they
 * are or escaped, within @p max bytes of it.
 */
static int
mcp_piece_write (struct mcp_piece const *p,
                 size_t                  off,
                 size_t                  len,
                 struct buf             *buf,
                 size_t                  max)
{
	struct span s;
	if (p->text) {
		int e = buf_reserve(buf, 6 * len, max, &s);
		if (e)
			return e;
		buf_commit(buf, jb_escape((char *)s.ptr, p->bytes.data + off, len));
		return 0;
	}
	int e = buf_reserve(buf, len, max, &s);
	if (e)
		return e;
	memcpy(s.ptr, p->bytes.data + off, len);
	buf_commit(buf, len);
	return 0;
}

/* Free the message, or the answer, now if the step has the share for it,
 * else leave it to the request's steps, a few values each: freeing one takes
 * as long as parsing it.
 */
static void
mcp_req_dispose (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	struct json_object *msg = req->msg;
	req->msg = nullptr;
	if (!msg)
		return;
	if (req->values <= loop_room(loop, LOOP_VALUES)) {
		json_object_put(msg);
		loop_work(loop, LOOP_VALUES, req->values);
	} else {
		jb_reaper_take(&req->reaper, msg);
		loop_ready_bulk(loop, &req->step);
	}
}

/* Enter @p phase, in a bulk step. */
static void
mcp_req_enter (struct mcp_req *req,
               enum mcp_phase  phase)
{
	req->phase = phase;
	loop_ready_bulk(req->mcp->loop, &req->step);
}

/** Returns the bytes of memory of the spare request @p req and its buffers. */
static uint64_t
mcp_req_kept (struct mcp_req const *req)
{
	return sizeof *req + dstr_get_block_count(&req->self.answer) * DSTR_BLK_SIZE +
	       dstr_get_block_count(&req->trace) * DSTR_BLK_SIZE +
	       req->scan.cap * sizeof *req->scan.members +
	       req->reaper.cap * sizeof *req->reaper.stack +
	       req->splice_max * sizeof *req->splice;
}

/* End the stream, if the request still has it, and keep the request for
 * reuse.
 */
static void
mcp_req_end (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	hist_record(&mcp->counts->methods[req->method], loop_clock() - req->head);
	if (req->s)
		req->s->ops->end(req->s, req->error);
	if (req->hook.next)
		list_del(&req->hook);
	loop_unready(&req->step);
	shelf_put(&mcp->spare, &req->hook);
	hist_add(&mcp->counts->kept.spares, 1);
	hist_add(&mcp->counts->kept.bytes[0], mcp_req_kept(req));
	atomic_store_explicit(&mcp->counts->kept.bytes[1],
	                      mcp->pool.bytes + mcp->answers.bytes, memory_order_relaxed);
}

/* Returns a request to begin: the spare request that ended last, or a new
 * request if there is none. Returns nullptr if out of memory. The answer,
 * the pre-scan and the reaper of a spare request are empty, but their
 * storage and the storage of the splice array are not freed.
 */
static struct mcp_req *
mcp_spare_take (struct mcp *mcp)
{
	struct list *node = shelf_take(&mcp->spare);
	if (!node) {
		struct mcp_req *req = malloc(sizeof *req);
		if (req)
			*req = (struct mcp_req){ .self.answer = DSTR_INIT, .trace = DSTR_INIT };
		return req;
	}
	struct mcp_req *req = container_of(node, struct mcp_req, hook);
	hist_add(&mcp->counts->kept.spares, -1ULL);
	hist_add(&mcp->counts->kept.bytes[0], -mcp_req_kept(req));
	return req;
}

/* Free the spare request that ended first, and the values it was left to
 * free.
 */
static void
mcp_spare_drop (struct mcp *mcp)
{
	struct mcp_req *req = container_of(shelf_drop(&mcp->spare), struct mcp_req,
	                                   hook);
	hist_add(&mcp->counts->kept.spares, -1ULL);
	hist_add(&mcp->counts->kept.bytes[0], -mcp_req_kept(req));
	jb_reaper_fini(&req->reaper);
	jb_scan_fini(&req->scan);
	dstr_cleanup(&req->self.answer);
	dstr_cleanup(&req->trace);
	free(req->splice);
	free(req);
}

static void mcp_on_job (struct loop *loop, struct msg *msg);

/* A job for a tool call: the one let go of last if there is one; nullptr if
 * out of memory.
 */
static struct job *
mcp_take_job (struct mcp *mcp)
{
	struct list *node = shelf_take(&mcp->jobs);
	if (node) {
		hist_add(&mcp->counts->kept.jobs, -1ULL);
		return container_of(node, struct job, spare);
	}
	struct job *job = malloc(sizeof *job);
	if (job)
		job_init(job, mcp->loop, mcp_on_job);
	return job;
}

/* Keep @p job, whose last reference has gone, for reuse. */
static void
mcp_shelve_job (struct mcp *mcp,
                struct job *job)
{
	home_job_recovery_fini(job);
	job->journal = nullptr;
	job->archive[0] = '\0';
	job_fini(job);
	shelf_put(&mcp->jobs, &job->spare);
	hist_add(&mcp->counts->kept.jobs, 1);
}

/* Free the request's handle: what it waited for finds no request. */
static void
mcp_req_forget (struct mcp_req *req)
{
	loop_forget(req->mcp->loop, req->call);
	req->call = (struct handle){};
}

static void mcp_slot_step (struct loop *loop, struct loop_step *step);

/* A slot of @p req for a call in @p role, with no call yet. */
static struct mcp_slot
mcp_slot (struct mcp_req *req,
          enum mcp_role   role)
{
	return (struct mcp_slot){
		.step.fn = mcp_slot_step,
		.answer = DSTR_INIT,
		.req = req,
		.tool = TOOL_MAX,
		.role = role
	};
}

/* Make @p req's slots for a turn's calls, unless it has them. */
static int
mcp_req_slots_make (struct mcp_req *req)
{
	if (req->slots)
		return 0;
	req->slots = malloc(JOB_CALLS * sizeof *req->slots);
	if (!req->slots)
		return ENOMEM;
	for (size_t k = 0; k < JOB_CALLS; ++k)
		req->slots[k] = mcp_slot(req, ROLE_TURN);
	return 0;
}

static void mcp_slot_drop (struct mcp_slot *slot);

/* Let go of what the turn's calls launched wait for, and of their output,
 * which the slots' storage keeps for reuse.
 */
static void
mcp_req_slots_drop (struct mcp_req *req)
{
	if (req->agenda)
		req->agenda->wait = nullptr;
	for (uint32_t k = 0; req->slots && k < req->launched; ++k) {
		struct mcp_slot *slot = &req->slots[k];
		slot->of = nullptr;
		mcp_slot_drop(slot);
		dstr_clear(&slot->answer);
		slot->skip = slot->window = 0;
		slot->note_len = 0;
		slot->tool = TOOL_MAX;
		slot->done = slot->failed = false;
	}
	req->launched = 0;
}

/* The offer of the tool of @p slot's call. */
static struct mcp_offer const *
mcp_slot_offer (struct mcp_slot const *slot)
{
	return &slot->req->mcp->st->offers[slot->tool];
}

/* The attributes of a tool of a plugin that askai loaded from a file. The
 * flags of its kind decide whether the turns are lent it. The turns'
 * metrics count the calls of all such tools in one row. The trace shows
 * the arguments of its calls.
 */
static struct mcp_row const mcp_loaded_row = { .metric = LENT_PLUGIN };

/** Returns the row of the registry tool @p t, or mcp_loaded_row for a
 *  tool of a plugin that askai loaded from a file.
 */
static struct mcp_row const *
mcp_row_of (struct registry_tool const *t)
{
	/* mcp_register() stored the row of each tool of askai in data. */
	return t->data ? t->data : &mcp_loaded_row;
}

/* "byte" or "bytes", for a count of @p n bytes in the log. */
static char const *
mcp_bytes (size_t n)
{
	return n == 1 ? "byte" : "bytes";
}

/** Writes @p text into the @p size bytes at @p buf as a JSON string on one
 *  line, followed by "..." if the string is cut. The string contains the
 *  longest prefix of whole characters whose escapes fit in @p max bytes,
 *  and in @p size bytes with the quotes, the "..." and the NUL.
 */
static void
mcp_quote (char             *buf,
           size_t            size,
           struct cstr_view  text,
           size_t            max)
{
	static constexpr size_t frame = sizeof "\"\"...";
	if (size < frame) {
		if (size)
			buf[0] = '\0';
		return;
	}
	size_t n = jb_escape_fit(text.data, text.len, max < size - frame ? max : size - frame);
	buf[0] = '"';
	size_t end = 1 + jb_escape(buf + 1, text.data, n);
	buf[end++] = '"';
	if (n < text.len) {
		memcpy(buf + end, "...", sizeof "..." - 1);
		end += sizeof "..." - 1;
	}
	buf[end] = '\0';
}

/* The arguments of a call as the log shows them, written so far. */
struct mcp_shown {
	char     *buf;  /* SHOWN bytes */
	size_t    len;
	uint64_t  cut;  /* bool: cut at TRACE_ARGS bytes; widened over the
	                   padding */
};

/** Appends @p s to @p w, up to TRACE_ARGS bytes in all, in whole
 *  characters and whole escapes. @p s is JSON text, or the inside of a
 *  JSON string if @p text is true, and then the function escapes it.
 */
static void
mcp_shown_put (struct mcp_shown *w,
               struct cstr_view  s,
               bool              text)
{
	if (w->cut || !s.len)
		return;
	size_t room = TRACE_ARGS - w->len;
	char escaped[TRACE_ARGS];
	if (text) {
		size_t k = jb_escape_fit(s.data, s.len, room);
		w->cut = k < s.len;
		s = (struct cstr_view){ escaped, jb_escape(escaped, s.data, k) };
	}
	size_t k = util_clip(s.data, s.len, room);
	w->cut |= k < s.len;
	memcpy(w->buf + w->len, s.data, k);
	w->len += k;
}

static void mcp_shown_object (struct mcp_shown *w, struct json_object *v, char const *sized);

/** Appends @p v to @p w as compact JSON, as JB_WIRE writes it, until
 *  TRACE_ARGS bytes are written. If @p v is an object, mcp_shown_object()
 *  writes it with @p sized.
 */
static void
mcp_shown_value (struct mcp_shown   *w,
                 struct json_object *v,
                 char const         *sized)
{
	switch (json_object_get_type(v)) {
	case json_type_object:
		mcp_shown_object(w, v, sized);
		return;
	case json_type_array: {
		size_t n = json_object_array_length(v);
		mcp_shown_put(w, CSTR_VIEW("["), false);
		for (size_t i = 0; i < n && !w->cut; ++i) {
			mcp_shown_put(w, (struct cstr_view){ ",", i != 0 }, false);
			mcp_shown_value(w, json_object_array_get_idx(v, i), nullptr);
		}
		mcp_shown_put(w, CSTR_VIEW("]"), false);
		return;
	}
	case json_type_string:
		mcp_shown_put(w, CSTR_VIEW("\""), false);
		mcp_shown_put(w, jb_view(v), true);
		mcp_shown_put(w, CSTR_VIEW("\""), false);
		return;
	default: {
		/* The JSON text of a scalar is short. */
		size_t len = 0;
		char const *json = json_object_to_json_string_length(v, JB_WIRE, &len);
		if (json)
			mcp_shown_put(w, (struct cstr_view){ json, len }, false);
		return;
	}
	}
}

/** Appends the object @p v to @p w like mcp_shown_value(). A string value
 *  is replaced by its size if it is the value of the member @p sized, or
 *  if it is a data: URI, which is a file.
 */
static void
mcp_shown_object (struct mcp_shown   *w,
                  struct json_object *v,
                  char const         *sized)
{
	struct cstr_view sep = CSTR_VIEW("\"");
	struct json_object_iter it;
	mcp_shown_put(w, CSTR_VIEW("{"), false);
	json_object_object_foreachC(v, it) {
		if (w->cut)
			return;
		mcp_shown_put(w, sep, false);
		sep = CSTR_VIEW(",\"");
		mcp_shown_put(w, cstr_view(it.key), true);
		mcp_shown_put(w, CSTR_VIEW("\":"), false);
		struct cstr_view s = jb_view(it.val);
		if (!s.data || !((sized && !strcmp(it.key, sized)) ||
		                 !strncasecmp(s.data, "data:", sizeof "data:" - 1))) {
			mcp_shown_value(w, it.val, nullptr);
			continue;
		}
		char text[40];
		int n = snprintf(text, sizeof text, "\"(%zu %s)\"", s.len, mcp_bytes(s.len));
		if (n > 0 && n < (int)sizeof text)
			mcp_shown_put(w, (struct cstr_view){ text, (size_t)n }, false);
	}
	mcp_shown_put(w, CSTR_VIEW("}"), false);
}

/** Writes @p args, the arguments of a call of @p offer, or @p none without
 *  arguments, into the SHOWN bytes at @p buf as compact JSON on one line,
 *  cut to TRACE_ARGS bytes with "...". The value of the argument that the
 *  member sized of the row of the tool specifies, such as the text of a
 *  note, is replaced by its size. Only the shown bytes are written.
 */
static void
mcp_args_shown (struct mcp_offer const *offer,
                struct json_object     *args,
                struct cstr_view        none,
                char                    buf[static SHOWN])
{
	struct mcp_shown w = { .buf = buf };
	if (args)
		mcp_shown_value(&w, args, mcp_row_of(&offer->tool)->sized);
	else
		mcp_shown_put(&w, none, false);
	if (w.cut) {
		memcpy(buf + w.len, "...", sizeof "..." - 1);
		w.len += sizeof "..." - 1;
	}
	buf[w.len] = '\0';
}

/* The client of @p req as the log names it. */
static char const *
mcp_req_client (struct mcp_req const *req)
{
	return *req->client ? req->client : "anonymous";
}

/** Logs the end of the client's tool call of @p req once. @p how is the
 *  text after the client in the line, such as "answered" or "failed". The
 *  line also contains the tool, the time, the size of the output @p out,
 *  the task or the job that the call runs as, if any, for an ask call the
 *  turns and their tool calls, the text of @p out if @p failed is true,
 *  and with --verbose the arguments. A failure is logged at LOG_WARN and
 *  any other end at LOG_VERBOSE, with separate rate limits, so that the
 *  lines of --verbose do not suppress failures.
 */
static void
mcp_req_log_call (struct mcp_req   *req,
                  char const       *how,
                  struct cstr_view  out,
                  bool              failed)
{
	static struct log_site sites[2];
	enum log_level level = failed ? LOG_WARN : LOG_VERBOSE;
	if (req->logged || req->self.tool == TOOL_MAX)
		return;
	req->logged = true;
	if (level > log_threshold)
		return;
	char ask[128] = "";
	if (req->asked)
		util_format(ask, sizeof ask, "; ask call %" PRIu64 ".%" PRIu64 ", %" PRIu32
		            " turn%s, %" PRIu32 " tool call%s%s", req->mcp->loop->id, req->asked,
		            req->turn, req->turn == 1 ? "" : "s", req->traced,
		            req->traced == 1 ? "" : "s",
		            req->stopped ? ", stopped at --max-turns" : "");
	char record[sizeof "; task " + LEDGER_ID] = "";
	if (req->task)
		util_format(record, sizeof record, "; %s %.*s",
		            req->task->kind == LEDGER_JOB ? "job" : "task", LEDGER_ID,
		            req->task->id);
	char text[ERROR_LOG + 8] = "";
	if (failed && out.len)
		mcp_quote(text, sizeof text, out, ERROR_LOG);
	log_put(&sites[failed], "tool call %s from %s %s after %.1f s (%zu %s)%s%s%s%s%s%s",
	        mcp_slot_offer(&req->self)->tool.name.data, mcp_req_client(req), how,
	        mcp_elapsed(req->self.start), out.len, mcp_bytes(out.len), record, ask,
	        *req->call_args ? "; arguments " : "", req->call_args, *text ? "; error " : "",
	        text);
}

/* Drops the call in @p slot. If the call's own word is set, the call
 * counts as ended first, so the call API ignores an attempt of the kind's
 * drop() to end the call. Then the function calls the kind's drop(), or it
 * logs a warning if the kind has no drop(). A request drops its own slot
 * between two turns too, and its own slot has no own word, so that slot
 * does not count as ended then. Last, the function releases the call's
 * HTTP request and its reply, puts the call's instance back
 * (instance_table_put()), clears the lent output and slot->shows, frees the
 * lines that the output shows, removes the call's step from the loop's
 * queue and frees the call's handle.
 */
static void
mcp_slot_drop (struct mcp_slot *slot)
{
	if (slot->own) {
		slot->done = true;
		struct mcp_offer const *offer = mcp_slot_offer(slot);
		if (offer->tool.kind->drop)
			offer->tool.kind->drop(slot);
		else
			LOG(LOG_WARN, "a call of %s set its own word, but its kind has no "
			    "drop()", offer->tool.name.data);
	}
	slot->own = nullptr;
	web_wait_leave(&slot->web);
	instance_table_put(&slot->req->mcp->instances, &slot->inst);
	slot->lent = (struct span){};
	review_read_destroy(&slot->read);
	slot->shows = nullptr;
	loop_unready(&slot->step);
	if (slot->call.bits)
		loop_forget(slot->req->mcp->loop, slot->call);
	slot->call = (struct handle){};
}

/* Let go of the request's job, if it has one: cancel it while the request
 * waits for it, freeing the request's handle, so that its completion finds
 * no request; else tell home the result has been read. Its calls let go
 * of what they hold, as their kinds do, and of their handles likewise.
 */
static void
mcp_req_drop (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct job *job = req->job;
	mcp_slot_drop(&req->self);
	mcp_req_slots_drop(req);
	if (!job) {
		if (req->call.bits)
			mcp_req_forget(req);
		return;
	}
	req->job = nullptr;
	req->delta = false;
	job_post(job, mcp->home, &job->to_home, &job->home_bits,
	         (mcp->draining || req->suspended) && job->asks & JOB_DURABLE
	         ? JOB_SUSPEND : JOB_CANCEL);
	if (!req->call.bits) {
		if (job_put(job, 1))
			mcp_shelve_job(mcp, job);
		return;
	}
	/* A job that waits keeps the front's reference for its completion. */
	mcp_req_forget(req);
	hist_add(&mcp->counts->calls.cancelled, 1);
}

/* Have the request wait among the calls under way, which a drain ends, if it
 * is not among them yet; while the server drains, the drain's next step
 * ends it.
 */
static void
mcp_req_wait (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	/* A reply under way is no longer a call that a drain could end. */
	if (req->phase >= PHASE_REPLY)
		return;
	if (!req->hook.next)
		list_append(&mcp->calls, &req->hook);
	if (mcp->draining)
		loop_ready_bulk(mcp->loop, &mcp->drain);
}

/* Take the request off the calls under way, or off the calls that write
 * their end record, if it is on either list.
 */
static void
mcp_req_unwait (struct mcp_req *req)
{
	if (req->hook.next) {
		list_del(&req->hook);
		req->hook = (struct list){};
	}
}

/* What the request waited for has come: free its handle, and take it off the
 * calls under way.
 */
static void
mcp_req_wake (struct mcp_req *req)
{
	mcp_req_forget(req);
	mcp_req_unwait(req);
}

/* The request waits for its tool's call: on the calls that a drain ends,
 * its stream told so, its message freed.
 */
static void
mcp_req_park (struct mcp_req *req)
{
	mcp_req_wait(req);
	req->s->ops->waiting(req->s);
	mcp_req_dispose(req);
	req->phase = PHASE_TOOL;
}

/* @p slot's call waits no more: its request, if the slot is its own; a
 * turn's call leaves it among the calls a drain ends.
 */
static void
mcp_slot_unwait (struct mcp_slot *slot)
{
	if (slot == &slot->req->self)
		mcp_req_unwait(slot->req);
}

/* Let go of the arguments of the turn's calls @p k to @p end. */
static void
mcp_req_args_free (struct mcp_req *req,
                   size_t          k,
                   size_t          end)
{
	for (; k < end; ++k)
		if (req->args[k]) {
			jb_reaper_take(&req->reaper, req->args[k]);
			req->args[k] = nullptr;
		}
	if (req->reaper.n)
		loop_ready_bulk(req->mcp->loop, &req->step);
}

static uint64_t mcp_req_owner (struct mcp_req const *req);

/* How the call of a task or a job ends: cancelled as its client asked, by a
 * cancel or a notifications/cancelled, failed if req->error is set or its
 * reply, in the record, is an error or not whole, else completed.
 */
static enum ledger_status
mcp_req_status (struct mcp_req const *req)
{
	static char const erred[] = "\"error\":";
	size_t n = sizeof erred - 1;
	struct ledger_task const *task = req->task;
	bool failed = task && (!task->result || task->len != task->size ||
	                      (task->len >= n && !memcmp(task->result, erred, n)));
	return req->error == ECANCELED || req->withdrawn ? LEDGER_CANCELLED
	     : req->error || failed ? LEDGER_FAILED : LEDGER_COMPLETED;
}

/* The call of a task or a job ends: publish how (mcp_req_status()), and
 * drop the call's reference to the record.
 */
static void
mcp_req_task_end (struct mcp_req *req)
{
	struct ledger_task *task = req->task;
	ledger_end(req->mcp->st->ledger, task, mcp_req_status(req));
	if (req->suspended)
		(void)ledger_remove(req->mcp->st->ledger, mcp_req_owner(req),
		                    (struct span){ (unsigned char *)task->id, LEDGER_ID });
	loop_forget(req->mcp->loop, task->who);
	ledger_task_put(task);
	req->task = nullptr;
	list_del(&req->task_hook);
}

/* Takes the request's tool call out of the roster, if it is in the roster,
 * and frees the handle of the request that the record has. After this, a
 * notifications/cancelled does not reach the call.
 */
static void
mcp_req_leave (struct mcp_req *req)
{
	if (!req->rostered)
		return;
	loop_forget(req->mcp->loop, req->rostered->who);
	roster_leave(req->mcp->st->roster, &req->rostered);
}

/* The reply is queued, or the stream gone: let go of everything at once but
 * the values still to free, and end once those are freed.
 */
static bool mcp_req_archive_end (struct mcp_req *req);

static void
mcp_req_finish (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	loop_timer_stop(mcp->loop, &req->timer);
	req->tick = false;
	/* Its job, slots, handles and agenda go first, so that no work of the
	 * call reaches it while its end is written. */
	mcp_req_drop(req);
	mcp_agenda_destroy(&req->agenda);
	review_destroy(&req->review);
	/* Neither a drain nor a notifications/cancelled reaches the call while
	 * its end is written. A cancel may supersede a pending state, intent
	 * or result write: FIFO writes keep that evidence, followed by the
	 * end, and the obsolete callback must not launch any more work. */
	mcp_req_leave(req);
	mcp_req_unwait(req);
	if (req->archive_call.bits)
		loop_forget(mcp->loop, req->archive_call);
	req->archive_call = (struct handle){};
	jb_parser_fini(&req->parser, req->phase == PHASE_ANSWER || req->phase == PHASE_ARGS
	                             ? &mcp->answers : &mcp->pool,
	               &req->reaper);
	if (mcp_req_archive_end(req))
		return;
	if (req->admitted) {
		ledger_end(mcp->st->ledger, req->admitted, LEDGER_CANCELLED);
		loop_forget(mcp->loop, req->admitted->who);
		ledger_task_put(req->admitted);
		req->admitted = nullptr;
	}
	journal_records_destroy(&req->recovered);
	for (size_t k = 0; k < JOB_CALLS; ++k) {
		json_object_put(req->saved_tools[k]);
		req->saved_tools[k] = nullptr;
	}
	if (req->task)
		mcp_req_task_end(req);
	ledger_task_put(req->shown);
	req->shown = nullptr;
	mcp_req_dispose(req);
	mcp_req_args_free(req, 0, ARRAY_SIZE(req->args));
	json_object_put(req->draft);
	json_object_put(req->id);
	json_object_put(req->token);
	req->draft = req->id = req->token = req->prompt = nullptr;
	jb_scan_reset(&req->scan);
	buf_fini(&req->body);
	buf_fini(&req->upload);
	buf_fini(&req->reply);
	buf_fini(&req->calls);
	buf_fini(&req->attached);
	dstr_clear(&req->self.answer);
	dstr_clear(&req->trace);
	for (size_t k = 0; req->slots && k < JOB_CALLS; ++k)
		dstr_cleanup(&req->slots[k].answer);
	free(req->slots);
	req->slots = nullptr;
	if (!req->reaper.n) {
		mcp_req_end(req);
		return;
	}
	req->phase = PHASE_FREE;
	list_append(&mcp->freeing, &req->hook);
	loop_ready_bulk(mcp->loop, &req->step);
}

/* The reply could not be made or sent, for @p e: end the stream with that. */
static void
mcp_req_abort (struct mcp_req *req,
               int             e)
{
	/* A call whose end is being written ends as it did: when the write is
	 * done, or at once if askai drains or its loop stops, as no journal
	 * callback reaches the call then. */
	if (req->phase != PHASE_END)
		req->error = e;
	else if (!req->mcp->draining && !req->mcp->loop->exit)
		return;
	mcp_req_finish(req);
}

/* Answer a request MCP takes no part in with an HTTP error, the stream
 * having no request state: @p message, JSON string content, and the header
 * @p fields.
 */
static void
mcp_http_error (struct stream *s,
                uint16_t       status,
                char const    *message,
                struct span    fields)
{
	char body[160];
	int n = snprintf(body, sizeof body, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":"
	                 "-32600,\"message\":\"%s\"}}", message);
	if (n < 0 || n >= (int)sizeof body) {
		s->ops->end(s, EOVERFLOW);
		return;
	}
	if (!s->ops->head(s, status, fields, (size_t)n))
		(void)s->ops->data(s, (struct span){ (unsigned char *)body, (size_t)n });
	s->ops->end(s, 0);
}

/* The same for a request with state, which goes. */
static void
mcp_req_http_error (struct mcp_req *req,
                    uint16_t        status,
                    char const     *message)
{
	mcp_http_error(req->s, status, message, SPAN(JSON_FIELD));
	req->s = nullptr;
	mcp_req_finish(req);
}

/* Queue response bytes, remembering transport backpressure. Home archives
 * reasoning independently of this transport.
 */
static int
mcp_req_write (struct mcp_req *req,
               struct span     bytes)
{
	int e = req->s->ops->data(req->s, bytes);
	if (e != EAGAIN)
		return e;
	req->blocked = true;
	return 0;
}

/* Send as much of the reply as the step has the share for, the rest a piece
 * a bulk step, each once the transport has room for it; then finish.
 */
static void
mcp_req_send (struct mcp_req *req)
{
	if (req->blocked)
		return;
	struct loop *loop = req->mcp->loop;
	struct span reply = buf_peek(&req->reply);
	size_t room = loop_room(loop, LOOP_SEND);
	size_t n = reply.len < room ? reply.len : room;
	int e = n ? mcp_req_write(req, (struct span){ reply.ptr, n }) : 0;
	if (e) {
		mcp_req_abort(req, e);
		return;
	}
	buf_consume(&req->reply, n);
	if (n == reply.len)
		mcp_req_finish(req);
	else if (!req->blocked)
		loop_ready_bulk(loop, &req->step);
}

/* Append the @p n pieces at @p p to the reply. */
static int
mcp_req_put (struct mcp_req         *req,
             struct mcp_piece const *p,
             size_t                  n)
{
	for (size_t i = 0; i < n; ++i) {
		if (!p[i].bytes.len)
			continue;
		int e = mcp_piece_write(&p[i], 0, p[i].bytes.len, &req->reply, SIZE_MAX);
		if (e)
			return e;
	}
	return 0;
}

/* Begin the reply: on an event stream, an event; then
 * '{"jsonrpc":"2.0","id":ID,', without the id if the request has none, and
 * @p key, '"result":' or '"error":'. An empty key begins a reply whose key
 * and value are those that a record of the ledger has. The reply of a
 * task's call begins with only @p key, and the record of the task has the
 * reply (mcp_req_show()).
 */
static int
mcp_req_open (struct mcp_req   *req,
              struct mcp_piece  key)
{
	if (req->task) {
		buf_consume(&req->reply, buf_peek(&req->reply).len);
		return mcp_req_put(req, &key, 1);
	}
	struct mcp_piece id[3] = {};
	if (req->id) {
		size_t len = 0;
		char const *json = json_object_to_json_string_length(req->id, JB_WIRE, &len);
		if (!json)
			return ENOMEM;
		id[0] = FRAG("\"id\":");
		id[1] = RAW(json, len);
		id[2] = FRAG(",");
	}
	struct mcp_piece const p[] = {
		req->sse ? FRAG("data: ") : FRAG(""),
		FRAG("{\"jsonrpc\":\"2.0\","),
		id[0],
		id[1],
		id[2],
		key,
	};
	buf_consume(&req->reply, buf_peek(&req->reply).len);
	return mcp_req_put(req, p, ARRAY_SIZE(p));
}

/* The call is answered: take it off the calls under way, which a drain
 * ends, and let go of what it waits for. Send the reply, as JSON with
 * @p status and the header @p fields or as the stream's last event, then
 * finish the request; if making the reply failed with @p e, end the stream
 * with that instead.
 */
static void
mcp_req_close (struct mcp_req *req,
               uint16_t        status,
               struct span     fields,
               int             e)
{
	struct stream *s = req->s;
	mcp_req_unwait(req);
	mcp_req_drop(req);
	loop_timer_stop(req->mcp->loop, &req->timer);
	if (e) {
		mcp_req_abort(req, e);
		return;
	}
	e = req->sse ? mcp_req_put(req, &FRAG("\n\n"), 1)
	             : s->ops->head(s, status, fields, buf_peek(&req->reply).len);
	if (e) {
		mcp_req_abort(req, e);
		return;
	}
	req->phase = PHASE_SEND;
	mcp_req_send(req);
}

/* Write a JSON-RPC error into the reply; @p message must be JSON string
 * content already, and the @p n_data pieces at @p data, if any, its data.
 */
static int
mcp_req_error_put (struct mcp_req         *req,
                   int                     code,
                   struct mcp_piece        message,
                   struct mcp_piece const *data,
                   size_t                  n_data)
{
	char num[16];
	int n = snprintf(num, sizeof num, "%d", code);
	if (n < 0 || n >= (int)sizeof num)
		return EOVERFLOW;
	struct mcp_piece const p[] = {
		FRAG("{\"code\":"),
		RAW(num, (size_t)n),
		FRAG(",\"message\":\""),
		message,
		n_data ? FRAG("\",\"data\":") : FRAG("\""),
	};
	int e = mcp_req_open(req, FRAG("\"error\":"));
	if (e)
		return e;
	e = mcp_req_put(req, p, ARRAY_SIZE(p));
	if (e)
		return e;
	e = mcp_req_put(req, data, n_data);
	return e ? e : mcp_req_put(req, &FRAG("}}"), 1);
}

/* Reply with a JSON-RPC error, as mcp_req_error_put() writes it; a tool
 * call that ends so is logged as failed.
 */
static void
mcp_req_error (struct mcp_req         *req,
               uint16_t                status,
               int                     code,
               struct mcp_piece        message,
               struct mcp_piece const *data,
               size_t                  n_data)
{
	mcp_req_log_call(req, "failed", message.bytes, true);
	mcp_req_close(req, status, SPAN(JSON_FIELD),
	              mcp_req_error_put(req, code, message, data, n_data));
}

/* End the call with an error reply: 503, Server is shutting down, for
 * ESHUTDOWN, as the server drains; 500, Out of memory, for any other error,
 * be it memory, a handle or a page's lookup the call could not have, or
 * text it could not format.
 */
static void
mcp_req_fail (struct mcp_req *req,
              int             e)
{
	/* The reply is being sent, or the request is being freed. A job of the
	 * turns that fails now does not change the reply. */
	if (req->phase >= PHASE_SEND)
		return;
	if (e == ESHUTDOWN) {
		mcp_req_error(req, 503, INTERNAL_ERROR, FRAG("Server is shutting down"),
		              nullptr, 0);
		return;
	}
	mcp_req_error(req, 500, INTERNAL_ERROR, FRAG("Out of memory"), nullptr, 0);
}

/* Reply with a result made of the @p n pieces at @p result, which close
 * it, and the header @p fields.
 */
static void
mcp_req_result (struct mcp_req         *req,
                struct mcp_piece const *result,
                size_t                  n,
                struct span             fields)
{
	int e = mcp_req_open(req, FRAG("\"result\":"));
	mcp_req_close(req, 200, fields, e ? e : mcp_req_put(req, result, n));
}

/** Begins the reply of @p req with a tool result whose text begins with
 *  @p text. More of the text can follow.
 */
static int
mcp_req_tool_open (struct mcp_req   *req,
                   struct cstr_view  text)
{
	struct mcp_piece const p[] = {
		FRAG("{\"content\":[{\"type\":\"text\",\"text\":\""),
		ESC_VIEW(text),
	};
	int e = mcp_req_open(req, FRAG("\"result\":"));
	return e ? e : mcp_req_put(req, p, ARRAY_SIZE(p));
}

/* Write into @p tail, of @p size bytes, how the turns ended, after what they
 * called: the calls left untold, whether they were stopped, and where the
 * notes are; returns its length.
 */
static size_t
mcp_req_trace_tail (struct mcp_req const *req,
                    char                 *tail,
                    size_t                size)
{
	size_t n = 0;
	if (req->traced > TRACE_MAX)
		n = util_format(tail, size, "- and %" PRIu32 " more\n",
		                req->traced - TRACE_MAX);
	if (req->stopped)
		n += util_format(tail + n, size - n, "It was stopped after %" PRIu32
		                 " turns, still calling tools.\n", req->turn);
	if (req->branch)
		n += util_format(tail + n, size - n, "Its notes are saved in the shared session repository. "
		                 "The ask-chain thread is %s.", req->thread);
	return n;
}

/* Close the result's text; then, for an ask call whose turns called tools,
 * or kept notes, what they called and where the notes are, as a text of its
 * own. A call without turns that get tools has no trace. A client's call
 * of the notes tool has no turns, and its thread does not belong to an ask
 * call.
 */
static int
mcp_req_trace_put (struct mcp_req *req)
{
	static char const none[] = "The other AI called no tools.\n";
	if (!req->lent || (!dstr_len(&req->trace) && !req->branch && !req->stopped))
		return mcp_req_put(req, &FRAG("\"}"), 1);
	char head[160];
	char tail[256];
	struct mcp_piece intro = TEXT(none);
	if (req->traced)
		intro = ESC(head, util_format(head, sizeof head, "The other AI made %" PRIu32
		                              " tool call%s in %" PRIu32 " turn%s:\n",
		                              req->traced, req->traced > 1 ? "s" : "",
		                              req->turn, req->turn > 1 ? "s" : ""));
	struct mcp_piece const p[] = {
		FRAG("\"},{\"type\":\"text\",\"text\":\""),
		intro,
		ESC_VIEW(cstr_view_dstr(&req->trace)),
		ESC(tail, mcp_req_trace_tail(req, tail, sizeof tail)),
		FRAG("\"}"),
	};
	return mcp_req_put(req, p, ARRAY_SIZE(p));
}

/* End a tool result, its trace after its text, and send it. The result of
 * a task's call ends without the modern tail, which mcp_req_show() adds for
 * a modern reader of the record.
 */
static void
mcp_req_tool_close (struct mcp_req *req)
{
	struct mcp_static const *st = req->mcp->st;
	struct mcp_piece const p[] = {
		FRAG("]"),
		req->failed ? FRAG(",\"isError\":true") : FRAG(""),
		req->modern && !req->task ? DSTR(&st->modern_tail) : FRAG("}"),
		FRAG("}"),
	};
	int e = mcp_req_trace_put(req);
	mcp_req_close(req, 200, SPAN(JSON_FIELD),
	              e ? e : mcp_req_put(req, p, ARRAY_SIZE(p)));
}

static void mcp_slot_done (struct mcp_slot *slot, bool failed);

/** Ends the call in @p slot with the short text @p text, failed if
 *  @p failed is true. For a call of a turn or a job, the text becomes the
 *  answer of the slot for mcp_slot_done(). A client's call is logged and
 *  answered with the text. The text flag of @p text is ignored.
 */
static void
mcp_slot_result (struct mcp_slot  *slot,
                 struct mcp_piece  text,
                 bool              failed)
{
	struct mcp_req *req = slot->req;
	slot->done = true;
	if (slot->role != ROLE_OWN) {
		/* An offset does not apply to the answer of a failed call. */
		if (failed)
			slot->skip = 0;
		if (cstr_view_store(text.bytes, &slot->answer)) {
			mcp_req_fail(req, ENOMEM);
			return;
		}
		mcp_slot_done(slot, failed);
		return;
	}
	req->failed = failed;
	mcp_req_log_call(req, failed ? "failed" : "answered", text.bytes, failed);
	int e = mcp_req_tool_open(req, text.bytes);
	if (e) {
		mcp_req_abort(req, e);
		return;
	}
	mcp_req_tool_close(req);
}

/* The output of @p slot's call: the job's whose output it gives, if any;
 * else what it lends, if anything; else what it read or built.
 */
static struct span
mcp_slot_output (struct mcp_slot const *slot)
{
	if (slot->of)
		return mcp_slot_output(slot->of);
	return slot->lent.ptr ? slot->lent
	     : (struct span){ (unsigned char *)(uintptr_t)dstr_get(&slot->answer),
	                      dstr_len(&slot->answer) };
}

/* The request's output: a streamed answer's text, in the job's result while
 * the request holds the job, else its own call's.
 */
static struct span
mcp_req_output (struct mcp_req const *req)
{
	return req->job ? buf_peek(&req->job->result) : mcp_slot_output(&req->self);
}

/* Escape as much of the output into the reply as the step has the share
 * for; the rest goes on in bulk steps. Then close the result, which lets
 * go of the job, or the page, if the output was its.
 */
static void
mcp_req_reply (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	struct span text = mcp_req_output(req);
	size_t len = text.len - req->off;
	size_t room = loop_room(loop, LOOP_ESCAPE);
	size_t n = len < room ? len : room;
	int e = mcp_req_put(req, &ESC(text.ptr + req->off, n), 1);
	loop_work(loop, LOOP_ESCAPE, n);
	req->off += n;
	if (e) {
		mcp_req_abort(req, e);
		return;
	}
	if (n < len) {
		loop_ready_bulk(loop, &req->step);
		return;
	}
	mcp_req_tool_close(req);
}

/** Ends the call in @p slot with its output, failed if @p failed is true.
 *  mcp_slot_done() handles the call of a turn or a job. The reply to a
 *  client's call is written a slice in each step. It begins with
 *  agenda->said and continues with the output from the first character
 *  at or after slot->skip, or from 0 for an ask call with lent tools.
 */
static void
mcp_slot_respond (struct mcp_slot *slot,
                  bool             failed)
{
	slot->done = true;
	if (slot->role != ROLE_OWN) {
		mcp_slot_done(slot, failed);
		return;
	}
	struct mcp_req *req = slot->req;
	struct span text = mcp_req_output(req);
	loop_timer_stop(req->mcp->loop, &req->timer);
	size_t off = req->lent ? 0 : slot->skip;
	if (off > text.len) {
		char why[96];
		size_t n = util_format(why, sizeof why, "The output has %zu bytes, fewer "
		                       "than the offset.", text.len);
		mcp_slot_result(slot, ESC(why, n), true);
		return;
	}
	mcp_req_log_call(req, failed ? "failed" : "answered",
	                 (struct cstr_view){ (char const *)text.ptr, text.len }, failed);
	while (off < text.len && (text.ptr[off] & 0xc0) == 0x80)
		++off;
	req->failed = failed;
	req->phase = PHASE_REPLY;
	req->off = off;
	int e = mcp_req_tool_open(req, req->agenda ? cstr_view_dstr(&req->agenda->said)
	                                           : (struct cstr_view){});
	if (e) {
		mcp_req_abort(req, e);
		return;
	}
	mcp_req_reply(req);
}

/* Write a notification of progress @p value, saying the @p len bytes of
 * @p text, which need no escaping, into the reply. A tool notification
 * identifies a call; other progress reports its current status.
 */
static int
mcp_req_notify_put (struct mcp_req *req,
                    uint64_t        value,
                    char const     *text,
                    size_t          len,
                    bool            tool)
{
	char num[24];
	int n = snprintf(num, sizeof num, "%" PRIu64, value);
	if (n < 0 || n >= (int)sizeof num)
		return EOVERFLOW;
	size_t token_len = 0;
	char const *token = json_object_to_json_string_length(req->token, JB_WIRE,
	                                                      &token_len);
	if (!token)
		return ENOMEM;
	struct mcp_piece const p[] = {
		FRAG("data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\","
		     "\"params\":{\"progressToken\":"),
		RAW(token, token_len),
		FRAG(",\"progress\":"),
		RAW(num, (size_t)n),
		FRAG(",\"message\":\""),
		RAW(text, len),
		FRAG("\",\"_meta\":{\"askai\":{\"kind\":\""),
		tool ? FRAG("tool") : FRAG("status"),
		FRAG("\"}}}}\n\n"),
	};
	return mcp_req_put(req, p, ARRAY_SIZE(p));
}

/* Send a notification of progress @p value, saying the @p len bytes of
 * @p text, on the call's event stream.
 */
static void
mcp_req_notify (struct mcp_req *req,
                uint64_t        value,
                char const     *text,
                size_t          len,
                bool            tool)
{
	if (!mcp_req_notify_put(req, value, text, len, tool))
		(void)mcp_req_write(req, buf_peek(&req->reply));
	buf_consume(&req->reply, buf_peek(&req->reply).len);
	req->noted = req->mcp->loop->now;
	req->next_note = value + 1;
}

/* Publish text from the immutable mailbox in bounded UTF-8 pieces. Home
 * cannot change it, advance its parser or deliver DONE before the DRAIN
 * acknowledgment. Transport backpressure therefore also bounds the mailbox.
 */
static void
mcp_req_delta (struct mcp_req *req)
{
	struct job *job = req->job;
	struct loop *loop = req->mcp->loop;
	struct span bytes = buf_peek(&job->delta);
	size_t id_len;
	char const *id = json_object_to_json_string_length(req->id, JB_WIRE, &id_len);
	if (!id) {
		mcp_req_abort(req, ENOMEM);
		return;
	}
	char turn[24];
	size_t turn_len = util_format(turn, sizeof turn, "%" PRIu32, req->turn);
	while (req->delta_at < bytes.len) {
		if (req->blocked)
			return;
		struct job_delta header;
		memcpy(&header, bytes.ptr + req->delta_at, sizeof header);
		char const *text = (char const *)bytes.ptr + req->delta_at + sizeof header;
		size_t room = loop_room(loop, LOOP_ESCAPE);
		size_t send = loop_room(loop, LOOP_SEND);
		/* Escaping a byte needs at most six bytes; ids are <= MAX_ID. */
		size_t overhead = 6 * MAX_ID + 256;
		send = send > overhead ? (send - overhead) / 6 : 0;
		room = room < send ? room : send;
		size_t n = util_clip(text + req->delta_read, header.len - req->delta_read,
		                    room < 8192 ? room : 8192);
		if (!n) {
			loop_ready_bulk(loop, &req->step);
			return;
		}
		struct mcp_piece const p[] = {
			FRAG("data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/askai/delta\","
			     "\"params\":{\"requestId\":"),
			RAW(id, id_len), FRAG(",\"turn\":"), RAW(turn, turn_len),
			FRAG(",\"kind\":\""), header.thought ? FRAG("reasoning") : FRAG("text"),
			FRAG("\",\"text\":\""), ESC(text + req->delta_read, n), FRAG("\"}}\n\n"),
		};
		int e = mcp_req_put(req, p, ARRAY_SIZE(p));
		loop_work(loop, LOOP_ESCAPE, n);
		struct span notice = buf_peek(&req->reply);
		if (e) {
			mcp_req_abort(req, e);
			return;
		}
		req->delta_read += n;
		if (req->delta_read == header.len) {
			req->delta_at += sizeof header + header.len;
			req->delta_read = 0;
		}
		e = mcp_req_write(req, notice);
		loop_work(loop, LOOP_SEND, notice.len);
		buf_consume(&req->reply, notice.len);
		if (e) {
			mcp_req_abort(req, e);
			return;
		}
	}
	if (req->blocked)
		return;
	req->delta = false;
	job_post(job, req->mcp->home, &job->to_home, &job->home_bits, JOB_DRAIN);
	if (req->tick)
		loop_ready_bulk(loop, &req->step);
}

/* Notify the client how far the call has come, as its job's progress says:
 * its place in its principal's queue, then the tokens of its answer, thinking
 * or text, as a progress that grows with each notification, and with each
 * turn. Held back by output the client has yet to read, it goes in a later
 * step.
 */
static void
mcp_req_progress (struct mcp_req *req)
{
	uint64_t progress = atomic_load_explicit(&req->job->progress,
	                                         memory_order_relaxed);
	uint32_t place = (uint32_t)progress & ~JOB_THINKING;
	uint32_t tokens = (uint32_t)(progress >> 32);
	if (place > req->place)
		req->place = place;
	req->progress = false;
	/* Queued, a job has no tokens; running, no place. */
	uint64_t value = req->base + (req->place - place) + tokens;
	if (value < req->next_note)
		return;
	char text[64];
	size_t n = req->lent ? util_format(text, sizeof text, "turn %" PRIu32 ", ",
	                                   req->turn)
	                     : 0;
	if (place)
		n += util_format(text + n, sizeof text - n, "queued, position %" PRIu32,
		                 place);
	else
		n += util_format(text + n, sizeof text - n, "%s, %" PRIu32 " tokens",
		                 progress & JOB_THINKING ? "thinking" : "generating",
		                 tokens);
	mcp_req_notify(req, value, text, n, false);
}

/* Notify the client that a turn calls @p tool, a tool askai names, unless
 * the call has no progressToken, or output the client has yet to read holds
 * the notification back.
 */
static void
mcp_req_notify_tool (struct mcp_req *req,
                     char const     *tool)
{
	if (!req->token || !req->sse || req->blocked)
		return;
	char text[64];
	size_t n = util_format(text, sizeof text, "turn %" PRIu32 ", calling %s",
	                       req->turn, tool);
	mcp_req_notify(req, req->next_note, text, n, true);
}

/* Set the request's timer for the first due of a keep-alive, a notification
 * of progress and its call's becoming a task; held back, it waits for
 * on_writable() for the last two.
 */
static void
mcp_req_arm (struct mcp_req *req)
{
	uint64_t next = req->progress && !req->blocked ? req->noted + PROGRESS_NS
	                                               : UINT64_MAX;
	next = req->ping_at < next ? req->ping_at : next;
	next = req->promote_at < next && !req->blocked ? req->promote_at : next;
	(void)loop_timer_set(req->mcp->loop, &req->timer, next);
}

static struct ledger_task *mcp_req_record (struct mcp_req *req,
                                           enum ledger_kind kind, uint32_t tool,
                                           int *e);
static void mcp_req_promote (struct mcp_req *req, struct ledger_task *task);

/* While a call waits: its becoming a task, if that is due; else a
 * keep-alive, if one is due, and a notification of its progress, if that
 * moved on, the last went PROGRESS_NS ago and the client has read what it
 * was sent; then the timer for the next of them. A call that cannot
 * become a task goes on as it is.
 */
static void
mcp_req_tick (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	uint32_t keepalive = req->mcp->st->lim.keepalive;
	if (req->promote_at <= loop->now && !req->blocked) {
		req->promote_at = UINT64_MAX;
		int e;
		struct ledger_task *task = mcp_req_record(req, LEDGER_TASK, req->self.tool, &e);
		if (task) {
			mcp_req_promote(req, task);
			return;
		}
		LOG(LOG_VERBOSE, "tool call kept from being a task: %s", strerror(e));
	}
	if (keepalive && req->ping_at <= loop->now) {
		if (!req->blocked)
			(void)mcp_req_write(req, SPAN(":\n\n"));
		req->ping_at = loop->now + keepalive * NS;
	}
	req->progress &= req->phase == PHASE_CALL;
	if (req->progress && req->noted + PROGRESS_NS <= loop->now && !req->blocked)
		mcp_req_progress(req);
	mcp_req_arm(req);
}

/* The job's progress moved on: notify the client, now or once PROGRESS_NS
 * have passed since the last time, if the call has an event stream and a
 * progressToken, and waits for its job.
 */
static void
mcp_req_progressed (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	if (!req->token || !req->sse || req->phase != PHASE_CALL)
		return;
	req->progress = true;
	uint64_t due = req->noted + PROGRESS_NS;
	if (due <= loop->now ||
	    loop_timer_set(loop, &req->timer, due < req->timer.due ? due : req->timer.due)) {
		req->tick = true;
		loop_ready(loop, &req->step);
	}
}

static void mcp_req_turn_done (struct mcp_req *req);
static void mcp_req_compact_full (struct mcp_req *req);

/** Returns true if @p rest begins, in any case, with the part of the
 *  context that overflowed: its size, length or window.
 */
static bool
mcp_overflow_what (struct cstr_view rest)
{
	static struct cstr_view const what[] = {
		CSTR_VIEW_INIT("size"), CSTR_VIEW_INIT("length"), CSTR_VIEW_INIT("window"),
	};
	for (size_t k = 0; k < ARRAY_SIZE(what); ++k)
		if (rest.len >= what[k].len && !strncasecmp(rest.data, what[k].data, what[k].len))
			return true;
	return false;
}

/** Returns true if the reason @p why of a failed turn reports that the
 *  request did not fit the context of the upstream: "exceeds the available
 *  context size" of llama-server, "maximum context length" of OpenAI,
 *  "context window" of others, and "exceeds the maximum number of tokens
 *  allowed" of Gemini.
 */
static bool
mcp_overflowed (struct cstr_view why)
{
	static char const word[] = "context ";
	static char const gemini[] = "exceeds the maximum number of tokens";
	size_t n = sizeof word - 1;
	if (memmem(why.data, why.len, gemini, sizeof gemini - 1))
		return true;
	for (size_t i = 0; i + n <= why.len; ++i)
		if (!strncasecmp(why.data + i, word, n) &&
		    mcp_overflow_what(cstr_view_slice(why, i + n, SIZE_MAX)))
			return true;
	return false;
}

/** Handles a turn of @p req that failed for @p why. If the turns attached
 *  files, their uploads are removed from the table, because the files of a
 *  failed conversation can be gone from the store of the upstream. Returns
 *  true if the turns start again from the prompt: the request did not fit
 *  the context, askai did not compact it since a turn went through, and
 *  turns went before this turn.
 */
static bool
mcp_req_refit (struct mcp_req   *req,
               struct cstr_view  why)
{
	if (req->filing)
		uploads_forget(req->mcp->st->uploads, mcp_req_owner(req));
	return req->turn > 1 && !req->forced && mcp_overflowed(why);
}

/** Logs, with --verbose, how the turn of @p req that @p job ran ended, the
 *  HTTP status, the time and the size of the result. The line of a
 *  streamed answer also contains the tokens of the context that the turn
 *  used, the stop reason and the number of tool calls.
 */
static void
mcp_req_log_turn (struct mcp_req const *req,
                  struct job const     *job)
{
	static char const *const ends[] = {
		[JOB_ANSWERED]  = "answered",
		[JOB_TEXT]      = "answered",
		[JOB_FAILED]    = "failed",
		[JOB_BUSY]      = "refused as busy",
		[JOB_CANCELLED] = "cancelled",
		[JOB_SUSPENDED] = "suspended",
	};
	static_assert(ARRAY_SIZE(ends) == JOB_CANCELLED + 1);
	if (log_threshold < LOG_VERBOSE)
		return;
	uint8_t status = job->status < ARRAY_SIZE(ends) ? job->status : JOB_FAILED;
	char http[24] = "no HTTP status";
	if (job->http)
		util_format(http, sizeof http, "HTTP %" PRIu16, job->http);
	size_t len = buf_peek(&job->result).len;
	if (status != JOB_TEXT) {
		bool body = status == JOB_ANSWERED;
		LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " turn %" PRIu32 ": %s, %s "
		    "after %.1f s, %s%zu %s of %s", req->mcp->loop->id, req->asked, req->turn,
		    body && job->http >= 400 ? "failed" : ends[status], http,
		    mcp_elapsed(req->turn_at), body ? "not streamed, " : "", len, mcp_bytes(len),
		    body ? "body" : "error text");
		return;
	}
	long told = req->mcp->st->ep->context;
	uint64_t context = told > 0 ? (uint64_t)told : job->context;
	uint64_t used = job->used;
	char tokens[96] = "tokens unknown";
	if (used && context)
		util_format(tokens, sizeof tokens, "%" PRIu64 " of %" PRIu64 " tokens (%" PRIu64
		            " %%)", used, context, used * 100 / context);
	else if (used)
		util_format(tokens, sizeof tokens, "%" PRIu64 " tokens of a context of "
		            "unknown size", used);
	char reason[sizeof job->reason + 8] = "none";
	if (*job->reason)
		mcp_quote(reason, sizeof reason, cstr_view(job->reason), sizeof job->reason);
	uint32_t calls = 0;
	for (size_t k = 0; k < JOB_CALLS; ++k)
		calls += buf_peek(&job->calls[k]).len != 0;
	LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " turn %" PRIu32 ": answered, %s "
	    "after %.1f s, %s, stop reason %s, %" PRIu32 " tool call%s, %zu %s of text",
	    req->mcp->loop->id, req->asked, req->turn, http, mcp_elapsed(req->turn_at),
	    tokens, reason, calls, calls == 1 ? "" : "s", len, mcp_bytes(len));
}

/** Handles the end of the job that @p req waits for. An answer that is not
 *  streamed is parsed in PHASE_ANSWER, and a streamed answer ends a turn.
 *  Otherwise the reason in the result of the job becomes the failed output,
 *  unless the turn did not fit the context and the turns start again from
 *  the compacted prompt. With lent tools, the request moves the upstream
 *  request, which contains the conversation, out of the job.
 */
static void
mcp_req_job_done (struct mcp_req *req)
{
	struct job *job = req->job;
	struct span why = buf_peek(&job->result);
	mcp_req_log_turn(req, job);
	buf_fini(&job->key);
	/* job_put() drops the reference of the message. The request still owns
	 * the reference of this loop. */
	(void)job_put(job, 1);
	mcp_req_wake(req);
	if (req->lent) {
		req->upload = job->request;
		job->request = (struct buf){};
	}
	buf_fini(&job->request);
	switch (job->status) {
	case JOB_ANSWERED:
		mcp_req_enter(req, PHASE_ANSWER);
		return;
	case JOB_TEXT:
		mcp_req_turn_done(req);
		return;
	case JOB_CANCELLED:
		mcp_req_fail(req, ESHUTDOWN);
		return;
	case JOB_SUSPENDED:
		req->suspended = true;
		break;
	case JOB_BUSY:
		why = SPAN(busy);
		break;
	default:
		/* The result of a failed call contains the reason, unless the call
		 * never started. */
		if (!why.len) {
			mcp_req_error(req, 500, INTERNAL_ERROR,
			              FRAG("Cannot start the upstream request"), nullptr, 0);
			return;
		}
		if (mcp_req_refit(req, (struct cstr_view){ (char const *)why.ptr, why.len })) {
			mcp_req_drop(req);
			mcp_req_compact_full(req);
			return;
		}
	}
	/* The reason is copied before mcp_req_drop() releases the result. */
	bool ok = dstr_cat_n(&req->self.answer, (char const *)why.ptr, why.len);
	mcp_req_drop(req);
	if (!ok) {
		mcp_req_fail(req, ENOMEM);
		return;
	}
	mcp_slot_respond(&req->self, true);
}

/*
 * A job's completion, or the job itself, handed back by home. A completion
 * goes to the request that waits for it, which parses the answer a slice a
 * step, or replies with why there is none, and lets go of the job; one whose
 * request has gone is dropped, the request having let go as it went. Once
 * the loop has stopped, MCP is gone, and jobs are freed.
 */
static void
mcp_on_job (struct loop *loop,
            struct msg  *msg)
{
	struct job *job = container_of(msg, struct job, to_front);
	uint32_t bits = atomic_exchange(&job->front_bits, 0);
	if (loop->exit) {
		/* The last reference frees the job: a completion brings the
		 * front's too, as its request has gone. */
		uint32_t refs = bits & JOB_RETURN ? 0 : bits & JOB_DONE ? 2 : 1;
		if (refs && !job_put(job, refs))
			return;
		job_fini(job);
		home_job_recovery_fini(job);
		free(job);
		return;
	}
	if (bits & JOB_RETURN) {
		mcp_shelve_job(self, job);
		return;
	}
	struct mcp_req *req = loop_lookup(loop, job->call, HANDLE_REQ);
	if (bits & JOB_DELTA && req) {
		if (req->sse) {
			req->delta_at = req->delta_read = 0;
			req->delta = true;
			loop_ready_bulk(loop, &req->step);
		} else {
			job_post(job, self->home, &job->to_home, &job->home_bits, JOB_DRAIN);
		}
	}
	if (!(bits & JOB_DONE)) {
		if (req)
			mcp_req_progressed(req);
		if (job_put(job, 1))
			mcp_shelve_job(self, job);
		return;
	}
	if (req) {
		mcp_req_job_done(req);
		return;
	}
	hist_add(&self->counts->calls.dropped, 1);
	if (job_put(job, 2))
		mcp_shelve_job(self, job);
}

/* Serialize a client's string as "", noting where its text goes:
 * mcp_req_build() escapes that into the upstream request a slice at a time.
 */
static int
mcp_splice_mark (struct json_object *str,
                 struct printbuf    *pb,
                 int                 level,
                 int                 flags)
{
	(void)level;
	(void)flags;
	struct mcp_req *req = json_object_get_userdata(str);
	if (req->splices == req->splice_max ||
	    printbuf_strappend(pb, "\"\"") < 0)
		return -1;
	req->splice[req->splices++] = (struct mcp_splice){ str, (size_t)pb->bpos - 1 };
	return 0;
}

/* Write the draft from where splice @p k - 1 goes to where splice @p k goes,
 * or to its end.
 */
static int
mcp_req_copy (struct mcp_req *req,
              size_t          k)
{
	size_t from = k ? req->splice[k - 1].at : 0;
	size_t len = (k < req->splices ? req->splice[k].at : req->text.len) - from;
	if (!len)
		return 0;
	return mcp_piece_write(&BYTES(req->text), from, len, &req->upload,
	                       SIZE_MAX);
}

/* Writes the call's session field and a line break into @p job's key, if
 * the request has a session field. A field cannot contain a line break.
 * Home uses the key to tell principals apart. Returns 0, or the error
 * number of buf_reserve().
 */
static int
mcp_key (struct job  *job,
         struct span  session)
{
	job->session = 0;
	if (!session.ptr)
		return 0;
	struct span s;
	int e = buf_reserve(&job->key, session.len + 1, SIZE_MAX, &s);
	if (e)
		return e;
	memcpy(s.ptr, session.ptr, session.len);
	s.ptr[session.len] = '\n';
	buf_commit(&job->key, session.len + 1);
	job->session = (uint32_t)(session.len + 1);
	return 0;
}

/** Writes into @p key the key of a call in the roster and stores its seeded
 *  hash in @p *hash, so that no client can choose keys that fill one bucket
 *  of the roster. The key contains the principal of the client of @p req,
 *  the address of the client if the principal is 0, the session field and
 *  a line break if the request contains one, and the JSON text of the
 *  request id @p id. Returns 0 or ENOMEM.
 */
static int
mcp_req_key (struct mcp_req const *req,
             struct json_object   *id,
             struct buf           *key,
             uint64_t             *hash)
{
	struct stream const *s = req->s;
	uint64_t who[3] = { req->principal };
	if (!who[0])
		memcpy(who + 1, s->peer, sizeof s->peer);
	struct span session = s->ops->field(s, F_SESSION);
	size_t len = 0;
	char const *json = json_object_to_json_string_length(id, JB_WIRE, &len);
	if (!json)
		return ENOMEM;
	size_t head = sizeof who + (session.ptr ? session.len + 1 : 0);
	size_t total = head + len;
	struct span k;
	int e = buf_reserve(key, total, SIZE_MAX, &k);
	if (e)
		return e;
	memcpy(k.ptr, who, sizeof who);
	if (session.ptr) {
		memcpy(k.ptr + sizeof who, session.ptr, session.len);
		k.ptr[head - 1] = '\n';
	}
	memcpy(k.ptr + head, json, len);
	buf_commit(key, total);
	*hash = util_hash(req->mcp->st->seed, k.ptr, total);
	return 0;
}

/** Returns the owner of the records in the ledger that @p req opens and
 *  reads: the principal of the client, or for an anonymous client, whose
 *  principal is 0, a hash of its address with the bit LEDGER_ANONYMOUS set,
 *  which is never set in a principal (src/policy.h). mcp_req_submit() uses
 *  the same hash for the jobs of an anonymous client without a session.
 */
static uint64_t
mcp_req_owner (struct mcp_req const *req)
{
	if (req->principal)
		return req->principal;
	struct stream const *s = req->s;
	return util_hash(~req->mcp->st->seed, s->peer, sizeof s->peer) | LEDGER_ANONYMOUS;
}

/* A journal callback names a request rather than keeping its address: a
 * disconnect or shutdown may have ended it before the disk write ends.
 */
struct mcp_archive_wait {
	struct handle who;
	void (*done) (struct mcp_req *req, int error);
};

/* Name @p req by a handle in @p *who for a journal callback, which no other
 * may be waiting for: 0, EBUSY, or why the handle could not be made.
 */
static int
mcp_req_archive_wait (struct mcp_req *req,
                      struct handle  *who)
{
	return req->archive_call.bits ? EBUSY
	     : loop_handle(req->mcp->loop, req, HANDLE_REQ, who);
}

/* The request that @p who, the handle of a journal callback, names, its
 * handle freed; nullptr if the request has ended, or the loop ends.
 */
static struct mcp_req *
mcp_req_archived (struct loop  *loop,
                  struct handle who)
{
	struct mcp_req *req = loop->exit ? nullptr : loop_lookup(loop, who, HANDLE_REQ);
	if (req) {
		loop_forget(loop, who);
		req->archive_call = (struct handle){};
	}
	return req;
}

static void
mcp_archive_back (struct loop *loop,
                  void        *arg,
                  int          error)
{
	struct mcp_archive_wait *wait = arg;
	struct mcp_req *req = mcp_req_archived(loop, wait->who);
	void (*done)(struct mcp_req *, int) = wait->done;
	free(wait);
	wait = nullptr;
	if (req)
		done(req, error);
}

/* Copy recovery state into the writer's bounded queue. A callback follows
 * the durable private checkpoint; this control data does not enter Git.
 */
static int
mcp_req_archive (struct mcp_req    *req,
                 char const        *kind,
                 struct json_object *obj,
                 void             (*done) (struct mcp_req *, int))
{
	if (!obj)
		return ENOMEM;
	size_t len = 0;
	char const *text = json_object_to_json_string_length(obj, JB_WIRE, &len);
	struct mcp_archive_wait *wait = done ? calloc(1, sizeof *wait) : nullptr;
	int e = !text || (done && !wait) ? ENOMEM : 0;
	if (!e && done) {
		wait->done = done;
		e = mcp_req_archive_wait(req, &wait->who);
	}
	if (!e)
		e = journal_put(req->mcp->st->journal, req->archive, kind, text, len,
		                done ? req->mcp->loop : nullptr,
		                done ? mcp_archive_back : nullptr, wait);
	json_object_put(obj);
	if (e) {
		if (wait && wait->who.bits)
			loop_forget(req->mcp->loop, wait->who);
		free(wait);
		wait = nullptr;
	} else if (wait) {
		req->archive_call = wait->who;
	}
	return e;
}

static void
mcp_req_archive_finished (struct mcp_req *req,
                          int             error)
{
	if (error) {
		LOG(LOG_ERROR, "cannot persist job %s: %s", req->archive, strerror(error));
		req->error = error;
	}
	mcp_req_finish(req);
}

/* A shutdown leaves the last provider checkpoint resumable. Explicit
 * cancellation and completed results are durable before ledger publish.
 * Foreground calls also become terminal, so their checkpoints can expire.
 */
static bool
mcp_req_archive_end (struct mcp_req *req)
{
	bool draining = req->mcp->draining;
	if (!*req->archive || req->archived_end || req->suspended ||
	    (draining && (req->task || req->admitted)))
		return false;
	req->archived_end = true;
	struct ledger_task const *task = req->task;
	enum ledger_status status = mcp_req_status(req);
	size_t len = !task || status == LEDGER_CANCELLED || task->len != task->size ? 0 : task->len;
	struct json_object *obj = jb_object(
		"status", json_object_new_int(status),
		"ended", json_object_new_uint64(clk_realtime_ns()),
		"result", json_object_new_string_len(task && task->result ? task->result : "",
		                                     (int)len), nullptr);
	int e = mcp_req_archive(req, status == LEDGER_CANCELLED ? "cancel" : "result",
	                        obj, draining ? nullptr : mcp_req_archive_finished);
	if (e) {
		req->error = e;
		LOG(LOG_ERROR, "cannot persist job %s: %s", req->archive, strerror(e));
		return false;
	}
	/* The writer drains after the loops; no request must wait for it on a
	 * loop which is itself being dismantled. */
	if (draining)
		return false;
	loop_unready(&req->step);
	list_append(&req->mcp->ending, &req->hook);
	req->phase = PHASE_END;
	return true;
}

/* Write the call's key into @p job, and name the request by a handle for
 * the job's completion.
 */
static int
mcp_req_name (struct mcp_req *req,
              struct job     *job)
{
	struct stream *s = req->s;
	int e = mcp_key(job, s->ops->field(s, F_SESSION));
	return e ? e : loop_handle(req->mcp->loop, req, HANDLE_REQ, &req->call);
}

/** Hands the upstream request of @p req to a job and submits the job to
 *  home. The principal of the job is the principal of the client, with the
 *  peer UINT64_MAX twice, the peer of no client. For an anonymous client,
 *  it is a hash of the session field or of the address. So all jobs of an
 *  identity share one principal. Returns 0 or an error number.
 */
static int
mcp_req_submit (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct loop *loop = mcp->loop;
	struct job *job = mcp_take_job(mcp);
	if (!job)
		return ENOMEM;
	job->journal = mcp->st->journal;
	memcpy(job->archive, req->archive, sizeof job->archive);
	job->archive_turn = req->turn + 1;
	if (req->recovering) {
		int re = home_job_restore(job, req->recovered);
		journal_records_destroy(&req->recovered);
		if (re) {
			mcp_shelve_job(mcp, job);
			return re;
		}
		req->recovering = false;
	}
	int e = mcp_req_name(req, job);
	if (e) {
		mcp_shelve_job(mcp, job);
		return e;
	}
	long ms = mcp->st->ep->timeout_ms;
	job->request = req->upload;
	req->upload = (struct buf){};
	job->call = req->call;
	/* A resumed job ends at the realtime deadline of its checkpoint, at
	 * least 1 ns from now, and another job after the timeout, if set. */
	uint64_t wall = req->archive_deadline ? clk_realtime_ns() : 0;
	job->deadline = wall ? loop->now + (req->archive_deadline > wall
	                                    ? req->archive_deadline - wall : 1)
	              : ms > 0 ? loop->now + (uint64_t)ms * 1'000'000 : 0;
	if (req->principal) {
		job->principal = req->principal;
		job->session = 0;
		job->peer[0] = job->peer[1] = UINT64_MAX;
	} else {
		memcpy(job->peer, req->s->peer, sizeof job->peer);
		uint64_t seed = mcp->st->seed;
		job->principal = job->session
		               ? util_hash(seed, buf_peek(&job->key).ptr, job->session)
		               : util_hash(~seed, job->peer, sizeof job->peer);
	}
	uint8_t asks = req->asks | (req->token && req->sse ? JOB_NOTIFY : 0);
	if (req->task && *req->archive)
		asks |= JOB_DURABLE;
	if (req->lent) {
		asks |= JOB_TOOLS;
		hist_add(&mcp->counts->calls.turns, 1);
	}
	job->asks = asks;
	req->sent = buf_peek(&job->request).len;
	req->base = req->next_note;
	req->place = 0;
	req->turn_at = loop_clock();
	++req->turn;
	/* Two references, one for this loop and one for home. job_post() adds
	 * the reference of the message. */
	atomic_store_explicit(&job->refs, 2, memory_order_relaxed);
	req->job = job;
	req->phase = PHASE_CALL;
	job_post(job, mcp->home, &job->to_home, &job->home_bits, JOB_SUBMIT);
	return 0;
}

static void mcp_req_turn_ready (struct mcp_req *req);

/* Escape as much of the client's string under way into the upstream request
 * as the step has the share for, and once it is all in, the draft after it:
 * 0 once all is written; EAGAIN while more is to come; or ENOMEM.
 */
static int
mcp_req_build_slice (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	if (req->next == req->splices)
		return 0;
	struct json_object *str = req->splice[req->next].str;
	size_t all = (size_t)json_object_get_string_len(str);
	size_t len = all - req->off;
	size_t room = loop_room(loop, LOOP_ESCAPE);
	size_t n = len < room ? len : room;
	if (n) {
		int e = mcp_piece_write(&ESC(json_object_get_string(str), all), req->off, n,
		                        &req->upload, SIZE_MAX);
		if (e)
			return e;
		loop_work(loop, LOOP_ESCAPE, n);
		req->off += n;
	}
	if (n < len)
		return EAGAIN;
	if (str == req->prompt)
		req->prompt_at = buf_peek(&req->upload).len;
	req->off = 0;
	int e = mcp_req_copy(req, ++req->next);
	if (e)
		return e;
	return req->next < req->splices ? EAGAIN : 0;
}

/* Write the upstream request, a slice a step; once all is written, submit
 * it, after what askai adds to the prompt if anything.
 */
static void
mcp_req_build (struct mcp_req *req)
{
	int e = mcp_req_build_slice(req);
	if (e == EAGAIN) {
		loop_ready_bulk(req->mcp->loop, &req->step);
		return;
	}
	if (e) {
		mcp_req_error(req, 500, INTERNAL_ERROR,
		              FRAG("Cannot start the upstream request"), nullptr, 0);
		return;
	}
	json_object_put(req->draft);
	req->draft = req->prompt = nullptr;
	req->next = 0;
	mcp_req_turn_ready(req);
}

/* Open the reply as an event stream, which keep-alives keep open while the
 * upstream works. The timer, for those and the notifications of progress,
 * is in the heap from now on, so setting it cannot fail.
 */
static void
mcp_req_stream (struct mcp_req *req)
{
	static char const fields[] = "Content-Type: text/event-stream\r\n"
	                             "Cache-Control: no-cache\r\n"
	                             "X-Accel-Buffering: no\r\n";
	struct mcp *mcp = req->mcp;
	struct stream *s = req->s;
	req->sse = !s->ops->head(s, 200, SPAN(fields), STREAM_OPEN);
	uint32_t keepalive = mcp->st->lim.keepalive;
	if (req->sse && keepalive)
		req->ping_at = mcp->loop->now + keepalive * NS;
	mcp_req_arm(req);
}

/* Write @p ns, CLOCK_REALTIME, into @p out as ISO 8601 UTC, to the
 * millisecond; returns its length, 0 if it cannot be told.
 */
static size_t
mcp_iso_time (char     out[static ISO_TIME],
              uint64_t ns)
{
	time_t secs = (time_t)(ns / NS);
	struct tm tm;
	if (!gmtime_r(&secs, &tm))
		return 0;
	return util_format(out, ISO_TIME, "%04d-%02d-%02dT%02d:%02d:%02d.%03" PRIu64 "Z",
	                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
	                   tm.tm_min, tm.tm_sec, ns % NS / 1'000'000);
}

/* The stand-in stream of a task's call takes its reply, of @p length bytes,
 * into the task's record; it has no client to tell more.
 */
static int
mcp_stand_in_head (struct stream *s,
                   uint16_t       status,
                   struct span    fields,
                   size_t         length)
{
	(void)status;
	(void)fields;
	struct mcp_req *req = s->user;
	return length == STREAM_OPEN ? EPIPE : ledger_task_reserve(req->task, length);
}

static int
mcp_stand_in_data (struct stream *s,
                   struct span    bytes)
{
	struct mcp_req *req = s->user;
	return ledger_task_write(req->task, bytes);
}

static void
mcp_stand_in_end (struct stream *s,
                  int            error)
{
	(void)s;
	(void)error;
}

static void
mcp_stand_in_waiting (struct stream *s)
{
	(void)s;
}

static struct span
mcp_stand_in_field (struct stream const *s,
                    size_t               i)
{
	(void)s;
	(void)i;
	return (struct span){};
}

static struct stream_ops const stand_in_ops = {
	.head = mcp_stand_in_head,
	.data = mcp_stand_in_data,
	.end = mcp_stand_in_end,
	.waiting = mcp_stand_in_waiting,
	.field = mcp_stand_in_field,
};

/* A client asked to cancel the task or the job whose cancel @p msg is: end
 * its call, if it still runs here, as a hang-up would, the record
 * cancelled.
 */
static void
mcp_on_task_cancel (struct loop *loop,
                    struct msg  *msg)
{
	struct ledger_task *task = container_of(msg, struct ledger_task, cancel);
	struct mcp_req *req = loop->exit ? nullptr : loop_lookup(loop, task->who, HANDLE_REQ);
	ledger_task_put(task);
	if (!req)
		return;
	mcp_req_log_call(req, "cancelled by its client", (struct cstr_view){}, false);
	mcp_req_abort(req, ECANCELED);
}

/* This is the function of the message that roster_cancel() posts when a
 * client cancels a tool call with notifications/cancelled. @p msg is the
 * member cancel of the call's record. If the call still runs on @p loop
 * and its reply has not begun, the function ends the call with
 * mcp_req_finish(), as a hang-up does, and its end record says cancelled.
 * Before that, it answers a JSON request with the status 202 and no body.
 * An event stream ends without a response.
 */
static void
mcp_on_withdraw (struct loop *loop,
                 struct msg  *msg)
{
	struct roster_call *call = container_of(msg, struct roster_call, cancel);
	struct mcp_req *req = loop->exit ? nullptr : loop_lookup(loop, call->who, HANDLE_REQ);
	roster_call_put(call);
	if (!req || req->phase >= PHASE_REPLY)
		return;
	if (!req->sse)
		(void)req->s->ops->head(req->s, 202, (struct span){}, 0);
	mcp_req_log_call(req, "cancelled by notifications/cancelled", (struct cstr_view){},
	                 false);
	req->withdrawn = true;
	mcp_req_finish(req);
}

/* Puts the request's tool call into the roster, so that a
 * notifications/cancelled of the same caller on any loop reaches the call.
 * The record has a new handle of the request, which mcp_req_leave() frees.
 * Returns 0, or ENOMEM.
 */
static int
mcp_req_enrol (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct buf key = {};
	uint64_t hash;
	struct handle who;
	int e = mcp_req_key(req, req->id, &key, &hash);
	if (!e)
		e = loop_handle(mcp->loop, req, HANDLE_REQ, &who);
	if (!e) {
		req->rostered = roster_enter(mcp->st->roster, buf_peek(&key), hash, mcp->loop,
		                             who, mcp_on_withdraw, &e);
		if (e)
			loop_forget(mcp->loop, who);
	}
	buf_fini(&key);
	return e;
}

static_assert(TOOL_MAX <= UINT16_MAX, "a record's tool is an id");

/* Opens a record of @p kind in the ledger for the call of the tool @p tool
 * that @p req runs, for any loop to find, and names the request by a new
 * handle for the record's cancel. On failure, returns nullptr with the
 * error in @p *e: that of ledger_open(), or ENOMEM if the request cannot
 * be named.
 */
static struct ledger_task *
mcp_req_record (struct mcp_req   *req,
                enum ledger_kind  kind,
                uint32_t          tool,
                int              *e)
{
	struct mcp *mcp = req->mcp;
	struct handle who;
	*e = loop_handle(mcp->loop, req, HANDLE_REQ, &who);
	if (*e)
		return nullptr;
	struct ledger_task *task = ledger_open(mcp->st->ledger, mcp_req_owner(req), kind,
	                                       (uint16_t)tool, mcp->loop, who,
	                                       mcp_on_task_cancel, e);
	if (!task)
		loop_forget(mcp->loop, who);
	return task;
}

/* Answers the call's client with the result that the @p n pieces at @p p
 * make, which close the result and the reply. From then on, the call runs
 * as the task or the job of the record @p task: its stream is the stand-in,
 * which writes the call's reply into the record. If the answer cannot go,
 * the function ends the record and the call, and returns the error.
 */
static int
mcp_req_detach (struct mcp_req         *req,
                struct ledger_task     *task,
                struct mcp_piece const *p,
                size_t                  n)
{
	struct mcp *mcp = req->mcp;
	struct stream *s = req->s;
	int e = mcp_req_open(req, FRAG("\"result\":"));
	e = e ? e : mcp_req_put(req, p, n);
	e = e ? e : req->sse ? mcp_req_put(req, &FRAG("\n\n"), 1)
	          : s->ops->head(s, 200, SPAN(JSON_FIELD), buf_peek(&req->reply).len);
	e = e ? e : mcp_req_write(req, buf_peek(&req->reply));
	buf_consume(&req->reply, buf_peek(&req->reply).len);
	if (e) {
		ledger_end(mcp->st->ledger, task, LEDGER_CANCELLED);
		loop_forget(mcp->loop, task->who);
		ledger_task_put(task);
		mcp_req_abort(req, e);
		return e;
	}
	/* The request has its answer. From now on a client can cancel the
	 * call only with tasks/cancel or a client's jobs. The call opens no
	 * event stream and becomes no task. */
	mcp_req_leave(req);
	req->stand_in = *s;
	req->stand_in.ops = &stand_in_ops;
	s->ops->end(s, 0);
	req->s = &req->stand_in;
	req->task = task;
	list_append(&mcp->tasks, &req->task_hook);
	req->sse_ok = req->sse = req->blocked = req->progress = false;
	req->ping_at = req->promote_at = UINT64_MAX;
	json_object_put(req->token);
	req->token = nullptr;
	loop_timer_stop(mcp->loop, &req->timer);
	return 0;
}

/* Answers the call's client with the task of the record @p task, which the
 * call runs as from now on, as mcp_req_detach() describes.
 */
static void
mcp_req_promote (struct mcp_req     *req,
                 struct ledger_task *task)
{
	static_assert(POLL_MS == 1000, "the reply says");
	char at[ISO_TIME];
	size_t n = mcp_iso_time(at, task->created);
	struct mcp_piece const p[] = {
		FRAG("{\"resultType\":\"task\","),
		DSTR(&req->mcp->st->server_meta),
		FRAG(",\"taskId\":\""),
		RAW(task->id, LEDGER_ID),
		FRAG("\",\"status\":\"working\",\"createdAt\":\""),
		RAW(at, n),
		FRAG("\",\"lastUpdatedAt\":\""),
		RAW(at, n),
		FRAG("\",\"ttlMs\":null,\"pollIntervalMs\":1000}}"),
	};
	if (!mcp_req_detach(req, task, p, ARRAY_SIZE(p)))
		LOG(LOG_VERBOSE, "tool call %s from %s became task %.*s after %.1f s",
		    mcp_slot_offer(&req->self)->tool.name.data, mcp_req_client(req), LEDGER_ID,
		    task->id, mcp_elapsed(req->head));
}

/* Whether @p id may name a request, or a progress token: a number, or a
 * string of at most MAX_ID bytes, as every reply or notification repeats
 * it.
 */
static bool
mcp_id_valid (struct json_object const *id)
{
	return json_object_is_type(id, json_type_string)
	     ? json_object_get_string_len(id) <= MAX_ID
	     : json_object_is_type(id, json_type_int) ||
	       json_object_is_type(id, json_type_double);
}

/* The call API (src/mcp.h): what a kind of tool does with a call. */

void *
mcp_slot_part (struct mcp_slot const *slot)
{
	return slot->req->mcp->parts[mcp_slot_offer(slot)->tool.part];
}

void *
mcp_slot_instance (struct mcp_slot const *slot)
{
	return slot ? slot->inst : nullptr;
}

/** Returns the record of the loading of the plugin of the tool of the call
 *  in @p slot, or nullptr if the plugin is built into askai.
 */
static struct plugin_loaded const *
mcp_slot_loaded (struct mcp_slot const *slot)
{
	return slot->req->mcp->st->plugins[mcp_slot_offer(slot)->tool.part].entry.loaded;
}

void const *
mcp_slot_config (struct mcp_slot const *slot)
{
	if (!slot)
		return nullptr;
	struct plugin_loaded const *loaded = mcp_slot_loaded(slot);
	return loaded ? loaded->config : nullptr;
}

struct loop *
mcp_slot_loop (struct mcp_slot const *slot)
{
	return slot ? slot->req->mcp->loop : nullptr;
}

uint64_t
mcp_slot_principal (struct mcp_slot const *slot)
{
	return slot ? slot->req->principal : 0;
}

bool
mcp_slot_from_client (struct mcp_slot const *slot)
{
	return slot && slot->role == ROLE_OWN;
}

void **
mcp_slot_own (struct mcp_slot *slot)
{
	return slot ? &slot->own : nullptr;
}

/* @p slot's call waits no more: let go of its name, if it has one, and of
 * its step, if queued. The function also releases the call's HTTP
 * request if the request is under way. A reply that has come stays until
 * mcp_slot_drop(), so that drop() can still use it.
 */
static void
mcp_slot_wake (struct mcp_slot *slot)
{
	if (slot->web && !web_wait_done(slot->web))
		web_wait_leave(&slot->web);
	if (slot->call.bits)
		loop_forget(slot->req->mcp->loop, slot->call);
	slot->call = (struct handle){};
	loop_unready(&slot->step);
	mcp_slot_unwait(slot);
}

int
mcp_slot_tell (struct mcp_slot  *slot,
               struct cstr_view  text,
               uint32_t          flags)
{
	if (!slot || (!text.data && text.len))
		return EFAULT;
	if (slot->done || flags & ~(uint32_t)TOOL_FAILED)
		return EINVAL;
	size_t len = jb_utf8(text.data, text.len < TOOL_TELL_MAX ? text.len : TOOL_TELL_MAX);
	mcp_slot_wake(slot);
	/* The output is the text whole, not lent, nor from an offset. */
	slot->skip = 0;
	slot->lent = (struct span){};
	mcp_slot_result(slot, ESC(text.data, len), flags & TOOL_FAILED);
	return 0;
}

int
mcp_slot_finish (struct mcp_slot *slot,
                 uint32_t         flags)
{
	if (!slot)
		return EFAULT;
	if (slot->done || flags & ~(uint32_t)TOOL_FAILED)
		return EINVAL;
	mcp_slot_wake(slot);
	mcp_slot_respond(slot, flags & TOOL_FAILED);
	return 0;
}

int
mcp_slot_fail (struct mcp_slot *slot,
               int              e)
{
	if (!slot)
		return EFAULT;
	if (slot->done)
		return EINVAL;
	slot->done = true;
	mcp_req_fail(slot->req, e);
	return 0;
}

int
mcp_slot_offset (struct mcp_slot    *slot,
                 struct json_object *args)
{
	if (!slot)
		return EFAULT;
	if (slot->done)
		return EINVAL;
	struct json_object *o = jb_get(args, "offset");
	int64_t n = !o ? 0
	          : json_object_is_type(o, json_type_int) ? json_object_get_int64(o) : -1;
	slot->skip = n > 0 ? (size_t)n : 0;
	if (n >= 0)
		return 0;
	mcp_slot_tell(slot, CSTR_VIEW("The offset argument must be a whole number of bytes, "
	                              "from 0."), TOOL_FAILED);
	return EINVAL;
}

int
mcp_slot_write (struct mcp_slot  *slot,
                struct cstr_view  text,
                uint32_t          flags,
                size_t           *taken)
{
	if (!taken)
		return EFAULT;
	*taken = 0;
	if (!slot || (!text.data && text.len))
		return EFAULT;
	if (slot->done || flags & ~(uint32_t)TOOL_MORE)
		return EINVAL;
	struct dstr *answer = &slot->answer;
	size_t had = dstr_len(answer);
	if (text.len > TOOL_OUTPUT_MAX - had)
		return EFBIG;
	/* Valid UTF-8 at the start of the text is copied at once, and the rest
	 * a chunk at a time, mended by jb_mend(). With TOOL_MORE, an incomplete
	 * character at the end is left for the next write. On failure, nothing
	 * is appended. */
	size_t i = jb_utf8(text.data, text.len);
	bool ok = dstr_cat_n(answer, text.data, i);
	for (bool last = i == text.len; ok && !last; ) {
		char chunk[4 << 10];
		size_t left = text.len - i;
		size_t n = left < sizeof chunk ? left : sizeof chunk;
		last = n == left;
		memcpy(chunk, text.data + i, n);
		n = jb_mend(chunk, n, last && !(flags & TOOL_MORE));
		ok = dstr_cat_n(answer, chunk, n);
		i += n;
	}
	if (!ok) {
		(void)dstr_erase(answer, had, SIZE_MAX);
		return ENOMEM;
	}
	loop_work(slot->req->mcp->loop, LOOP_COPY, i);
	*taken = i;
	return 0;
}

struct cstr_view
mcp_slot_answer (struct mcp_slot const *slot)
{
	return cstr_view_dstr(&slot->answer);
}

void
mcp_slot_lend (struct mcp_slot  *slot,
               struct cstr_view  text)
{
	slot->lent = (struct span){ (unsigned char *)(uintptr_t)text.data, text.len };
}

/* Return the maximum number of bytes of a tool's output that a turn reads
 * in one result. That is an eighth of the context in bytes, at about 4
 * bytes per token, kept between 2048 and LEND_MAX. It is LEND_MAX if the
 * context size is not known.
 */
static size_t
mcp_req_lend_max (struct mcp_req const *req)
{
	size_t eighth = (size_t)req->context / 2;
	return !req->context ? LEND_MAX : eighth < 2048 ? 2048
	       : eighth < LEND_MAX ? eighth : LEND_MAX;
}

/* A job gets a turn's window, because a turn of the same request reads the
 * job's output with the jobs tool.
 */
size_t
mcp_slot_window_max (struct mcp_slot const *slot)
{
	return slot->role == ROLE_OWN ? TOOL_OUTPUT_MAX : mcp_req_lend_max(slot->req);
}

struct review *
mcp_slot_review (struct mcp_slot const *slot)
{
	return slot->req->review;
}

uint32_t
mcp_slot_turn (struct mcp_slot const *slot)
{
	return slot->req->turn;
}

int
mcp_slot_source (struct mcp_slot            *slot,
                 struct review_source const *src)
{
	if (!slot || !src)
		return EFAULT;
	if (slot->done)
		return EINVAL;
	review_read_destroy(&slot->read);
	/* A turn's window is at most mcp_slot_window_max() bytes of the output,
	 * so no window shows a longer single line whole. Keep no such line, so
	 * that review_pending_add() does not scan it. */
	if (!slot->req->review ||
	    (src->first == src->last && src->len > mcp_slot_window_max(slot)))
		return 0;
	int e = 0;
	slot->read = review_read_create(src, mcp_slot_answer(slot), &e);
	if (slot->read)
		loop_work(slot->req->mcp->loop, LOOP_COPY, src->len);
	return e;
}

int
mcp_slot_shows (struct mcp_slot *slot,
                char const      *selector)
{
	if (!slot || !selector)
		return EFAULT;
	if (slot->done)
		return EINVAL;
	slot->shows = selector;
	return 0;
}

int
mcp_slot_noted (struct mcp_slot *slot)
{
	if (!slot)
		return EFAULT;
	if (slot->done)
		return EINVAL;
	slot->req->branch = true;
	return 0;
}

/* Its request parks, if the slot is its own; a turn's call keeps it among
 * the calls a drain ends.
 */
int
mcp_slot_park (struct mcp_slot *slot)
{
	if (!slot)
		return EFAULT;
	if (slot->done)
		return EINVAL;
	struct mcp_req *req = slot->req;
	if (mcp_slot_offer(slot)->tool.kind->flags & TOOL_STREAM && req->sse_ok && !req->sse)
		mcp_req_stream(req);
	if (slot == &req->self)
		mcp_req_park(req);
	else
		mcp_req_wait(req);
	return 0;
}

int
mcp_slot_ready (struct mcp_slot *slot)
{
	if (!slot)
		return EFAULT;
	if (slot->done)
		return EINVAL;
	if (!mcp_slot_offer(slot)->tool.kind->step) {
		(void)mcp_slot_tell(slot, CSTR_VIEW("The tool's kind has no step(), so the "
		                                    "call cannot go on."), TOOL_FAILED);
		return EINVAL;
	}
	loop_ready_bulk(slot->req->mcp->loop, &slot->step);
	return 0;
}

int
mcp_slot_name (struct mcp_slot *slot,
               struct handle   *who)
{
	if (!who)
		return EFAULT;
	*who = (struct handle){};
	if (!slot)
		return EFAULT;
	if (slot->done)
		return EINVAL;
	int e = slot->call.bits ? 0 : loop_handle(slot->req->mcp->loop, slot, HANDLE_SLOT,
	                                          &slot->call);
	if (!e)
		*who = slot->call;
	return e;
}

struct mcp_slot *
mcp_slot_named (struct loop const *loop,
                struct handle      who)
{
	return loop && !loop->exit ? loop_lookup(loop, who, HANDLE_SLOT) : nullptr;
}

/* The reply to @p wait, the HTTP request of the call that @p who names, has
 * come. The step of the call is queued, unless the call has ended. If the
 * call is gone, or the loop has stopped, the wait is freed.
 */
static void
mcp_slot_fetch_back (struct loop     *loop,
                     struct handle    who,
                     struct web_wait *wait)
{
	struct mcp_slot *slot = mcp_slot_named(loop, who);
	if (!slot || slot->web != wait) {
		web_wait_leave(&wait);
		return;
	}
	if (!slot->done)
		loop_ready_bulk(loop, &slot->step);
}

/* Sends @p ask's request as the HTTP request of @p slot's call, whose step
 * then takes the reply: 0, or the error of mcp_slot_name() or of
 * web_wait_create().
 */
static int
mcp_slot_send (struct mcp_slot *slot,
               struct web_ask  *ask)
{
	struct mcp *mcp = slot->req->mcp;
	int e = mcp_slot_name(slot, &ask->who);
	if (e)
		return e;
	ask->loop = mcp->loop;
	ask->home = mcp->home;
	ask->back = mcp_slot_fetch_back;
	struct web_wait *wait = web_wait_create(mcp->st->web, ask, &e);
	if (!wait)
		return e;
	web_wait_leave(&slot->web);
	slot->web = wait;
	if (web_wait_done(wait))
		loop_ready_bulk(mcp->loop, &slot->step);
	return 0;
}

int
mcp_slot_fetch (struct mcp_slot          *slot,
                struct web_request const *request)
{
	if (!slot || !request || !request->url || (!request->body.data && request->body.len))
		return EFAULT;
	if (slot->done)
		return EINVAL;
	struct cstr_view url = { request->url, strnlen(request->url, WEB_URL_MAX + 1) };
	char const *accept = request->accept;
	size_t accept_len = 0;
	if (accept) {
		accept_len = strnlen(accept, WEB_ACCEPT_MAX + 1);
		if (accept_len - 1 >= WEB_ACCEPT_MAX)
			return EINVAL;
	}
	size_t scheme = cstr_view_starts(url, CSTR_VIEW("http://")) ? sizeof "http://" - 1
	              : cstr_view_starts(url, CSTR_VIEW("https://")) ? sizeof "https://" - 1 : 0;
	/* The authority ends at the first '/', '?' or '#'. A user name or a
	 * password in it ends in '@'. For a URL that is too long, authority
	 * is 0, and the URL is refused. */
	size_t authority = scheme && url.len <= WEB_URL_MAX
	                 ? strcspn(url.data + scheme, "/?#") : 0;
	if (!authority || memchr(url.data + scheme, '@', authority) ||
	    request->body.len > WEB_SEND_MAX || request->max - 1 >= WEB_BODY_MAX ||
	    request->timeout - 1 >= WEB_TIMEOUT_MAX || request->keep > WEB_KEEP_MAX)
		return EINVAL;
	for (size_t i = 0; i < url.len; ++i)
		if ((unsigned char)url.data[i] <= ' ' || (unsigned char)url.data[i] > '~')
			return EINVAL;
	for (size_t i = 0; i < accept_len; ++i)
		if ((unsigned char)accept[i] < ' ' || (unsigned char)accept[i] > '~')
			return EINVAL;
	if (!mcp_slot_offer(slot)->tool.kind->step)
		return mcp_slot_ready(slot);
	if (slot->web && !web_wait_done(slot->web))
		return EBUSY;
	struct plugin_loaded const *loaded = mcp_slot_loaded(slot);
	struct web_ask ask = {
		.request = request,
		.plugin = loaded ? loaded->manifest.name : nullptr,
		.url = url.len,
		.accept = accept_len
	};
	return mcp_slot_send(slot, &ask);
}

int
mcp_slot_fetched (struct mcp_slot const *slot,
                  struct web_response   *response)
{
	if (!response)
		return EFAULT;
	*response = (struct web_response){};
	if (!slot)
		return EFAULT;
	if (slot->done)
		return EINVAL;
	if (!slot->web)
		return ENOENT;
	if (!web_wait_done(slot->web))
		return EINPROGRESS;
	web_wait_response(slot->web, response, slot->req->mcp->loop->now);
	return 0;
}

/* Serialize the tools that the turns of @p obj's userdata, a request, may
 * call: their definitions as the upstream's API has them, each after a
 * comma, the first's left out, in the API's array.
 */
static int
mcp_lent_text (struct json_object *obj,
               struct printbuf    *pb,
               int                 level,
               int                 flags)
{
	(void)level;
	(void)flags;
	struct mcp_req const *req = json_object_get_userdata(obj);
	struct mcp_static const *st = req->mcp->st;
	struct ai_format const *f = ai_format(st->ep->api);
	int e = printbuf_memappend(pb, f->tools.data, (int)f->tools.len);
	size_t skip = 1;
	for (uint32_t id = 0; e >= 0 && id < st->n_offers; ++id) {
		struct dstr const *def = &st->offers[id].lent;
		if (!(req->lent >> id & 1))
			continue;
		e = printbuf_memappend(pb, dstr_get(def) + skip, (int)(dstr_len(def) - skip));
		skip = 0;
	}
	if (e >= 0)
		e = printbuf_memappend(pb, f->tools_end.data, (int)f->tools_end.len);
	return e < 0 ? -1 : 0;
}

/** Drafts the upstream request of @p req with @p prompt, @p system and the
 *  chat history @p history, and writes the draft up to the first string of
 *  the client. mcp_req_build() escapes the strings of the client, which
 *  can be long, into the upload. The default system prompt and the lent
 *  tools are part of the draft. Returns 0 or an error number.
 */
static int
mcp_req_draft (struct mcp_req     *req,
               struct json_object *prompt,
               struct json_object *system,
               struct json_object *history)
{
	struct mcp_static const *st = req->mcp->st;
	size_t n = jb_len(history);
	/* The draft contains a splice for each message of the history, one for
	 * the prompt and one for the system prompt. */
	size_t const need = n + 2;
	if (need > req->splice_max) {
		struct mcp_splice *splice = realloc(req->splice, need * sizeof *splice);
		if (!splice)
			return ENOMEM;
		req->splice = splice;
		req->splice_max = (uint16_t)need;
	}
	for (size_t k = 0; k < n; ++k)
		json_object_set_serializer(jb_get(jb_at(history, k), "content"),
		                           mcp_splice_mark, req, nullptr);
	char const *sys = st->ep->system;
	struct json_object *instructions = nullptr;
	if (system) {
		json_object_set_serializer(system, mcp_splice_mark, req, nullptr);
		instructions = *jb_text(system) ? json_object_get(system) : nullptr;
	} else if (sys && *sys) {
		instructions = json_object_new_string(sys);
		if (!instructions)
			return ENOMEM;
	}
	struct json_object *tools = nullptr;
	if (req->lent) {
		tools = json_object_new_array();
		if (!tools) {
			json_object_put(instructions);
			return ENOMEM;
		}
		json_object_set_serializer(tools, mcp_lent_text, req, nullptr);
	}
	json_object_set_serializer(prompt, mcp_splice_mark, req, nullptr);
	req->draft = ai_request(st->ep, instructions, prompt, history, tools);
	json_object_put(instructions);
	json_object_put(tools);
	req->prompt = prompt;
	if (!req->draft)
		return ENOMEM;
	req->text.data = json_object_to_json_string_length(req->draft, JB_WIRE, &req->text.len);
	return req->text.data ? mcp_req_copy(req, 0) : ENOMEM;
}

/** Logs, with --verbose, the start of the ask call of @p req with the
 *  prompt @p prompt. The line contains the client, the size and the start
 *  of the prompt, and the thread of the notes, if any. @p resume is true
 *  if the call goes on with an earlier thread.
 */
static void
mcp_req_log_ask (struct mcp_req const *req,
                 struct json_object   *prompt,
                 bool                  resume)
{
	if (log_threshold < LOG_VERBOSE)
		return;
	struct cstr_view text = jb_view(prompt);
	char quoted[PROMPT_LOG + 8];
	mcp_quote(quoted, sizeof quoted, text, PROMPT_LOG);
	char thread[NOTES_ID + 32] = "no thread";
	if (*req->thread)
		util_format(thread, sizeof thread, "thread %s (%s)", req->thread,
		            resume ? "resumed" : "new");
	LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " from %s: prompt %zu %s, %s, %s",
	    req->mcp->loop->id, req->asked, mcp_req_client(req), text.len, mcp_bytes(text.len),
	    thread, quoted);
}

static int mcp_req_background (struct mcp_req *req, uint32_t tool, bool own_tool);

/* The roles of the messages of a chat history. */
static struct cstr_view const roles[] = { CSTR_VIEW_INIT("user"), CSTR_VIEW_INIT("assistant") };

/* Only visible user and assistant text can be supplied as chat history.
 * Tool calls, tool results and system messages remain server controlled.
 */
static bool
mcp_history_valid (struct json_object *history)
{
	size_t n = json_object_is_type(history, json_type_array)
	         ? json_object_array_length(history) : SIZE_MAX;
	if (n > CHAT_HISTORY)
		return false;
	for (size_t k = 0; k < n; ++k) {
		struct json_object *message = json_object_array_get_idx(history, k);
		struct cstr_view role = jb_view(jb_get(message, "role"));
		if (!json_object_is_type(message, json_type_object) ||
		    json_object_object_length(message) != 2 ||
		    !jb_text(jb_get(message, "content")) ||
		    cstr_view_index(roles, ARRAY_SIZE(roles), role) == ARRAY_SIZE(roles))
			return false;
	}
	return true;
}

/** Keep the writer's identity in both initial and compacted prompts. */
static int
mcp_req_notes_identity (struct mcp_req *req)
{
	if (!*req->repo)
		return 0;
	return dstr_printf(&req->self.answer,
	                   "\n\n[askai: participant %s. Your writable notebook "
	                   "is %s. Use notes action notebooks to discover "
	                   "peers; selectors identify their authors.]\n",
	                   notes_writer(req->mcp->st->notes_tool.notes),
	                   notes_selector(req->repo)) ? 0 : ENOMEM;
}

/* Create req->review if the tool mask @p lent contains findings. The record
 * is created with REVIEW_READ_LOG if @p lent also contains git. Returns 0 or
 * ENOMEM.
 */
static int
mcp_req_review_make (struct mcp_req *req,
                     tool_mask       lent)
{
	struct mcp_lending const *l = &req->mcp->st->lending;
	int e = 0;
	if (lent & l->of[KIND_FINDINGS])
		req->review = review_create(lent & l->of[KIND_GIT] ? REVIEW_READ_LOG : 0, &e);
	return e;
}

/* Goes on with the ask call in @p slot, req->self, once the notes of the
 * caller are in the prompt. The ask call builds its upstream request.
 */
static void
mcp_slot_resumed (struct mcp_slot *slot)
{
	mcp_req_enter(slot->req, PHASE_BUILD);
}

/* Start a call that asks the upstream, with the arguments @p args, of the
 * class the request asks for, lending the turns the tools req->lent says,
 * if any: build its upstream request, then submit its job. The notes the turns keep, if askai keeps
 * them, go in the thread the call goes on with, else in a new one.
 */
static void
mcp_req_ask (struct mcp_req     *req,
             struct json_object *args,
             bool                background)
{
	struct mcp_static const *st = req->mcp->st;
	struct notes const *notes = req->lent & st->lending.of[KIND_NOTES]
	                          ? st->notes_tool.notes : nullptr;
	/* Argument problems go back to the model so it can correct itself. */
	struct json_object *prompt = jb_get(args, "prompt");
	struct json_object *system = jb_get(args, "system");
	struct json_object *thread = jb_get(args, "thread");
	struct json_object *history = nullptr;
	bool has_history = json_object_object_get_ex(args, "messages", &history);
	char const *text = jb_text(prompt);
	size_t len = 0;
	char const *id = jb_str(thread, &len);
	struct mcp_piece problem =
		!text || !*text ? TEXT("The prompt argument must be a non-empty string.")
		: system && !jb_text(system) ? TEXT("The system argument must be a string.")
		: has_history && !mcp_history_valid(history)
		? TEXT("The messages argument must be an array of at most 1024 earlier "
		       "messages, each with only role (user or assistant) and string content.")
		: !thread ? (struct mcp_piece){}
		: !notes ? TEXT("No notes are kept for you here, so there is no thread to "
		                "go on with.")
		: !id || !notes_is_id(id, len)
		? TEXT("The thread argument must be a thread's id, as an earlier answer "
		       "gave it: 32 hexadecimal digits.")
		: (struct mcp_piece){};
	if (problem.bytes.data) {
		mcp_slot_result(&req->self, problem, true);
		return;
	}
	if (background && !req->task) {
		if (st->journal)
			req->detach_pending = true;
		else if (mcp_req_background(req, req->self.tool, true))
			return;
	}
	/* A thread's id comes with notes only (the problems above). */
	if (id) {
		memcpy(req->thread, id, sizeof req->thread);
	} else if (notes && notes_id(req->thread)) {
		mcp_req_error(req, 500, INTERNAL_ERROR, FRAG("Cannot make a thread's id"),
		              nullptr, 0);
		return;
	}
	if (notes)
		notes_repo(notes, req->principal, NOTES_UPSTREAM, req->repo);
	req->asked = ++req->mcp->asked;
	mcp_req_log_ask(req, prompt, id != nullptr);
	mcp_req_wait(req);
	int e = mcp_req_draft(req, prompt, system, history);
	if (!e)
		e = mcp_req_notes_identity(req);
	if (!e)
		e = mcp_req_review_make(req, req->lent);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	req->s->ops->waiting(req->s);
	if (req->sse_ok && !req->detach_pending)
		mcp_req_stream(req);
	/* Every ask starts with this participant's session notes. */
	bool resume = notes && notes_exists(notes);
	mcp_req_dispose(req);
	if (resume) {
		notes_tool_loop_prompt(req->mcp->notes, &req->self, &req->self.answer,
		                       NOTES_TOOL_RESUME, mcp_req_lend_max(req), mcp_slot_resumed);
		return;
	}
	mcp_req_enter(req, PHASE_BUILD);
}

/* test_task's work: a kernel of CPU work, run a slice at a time. */
struct mcp_spin {
	struct task   task;    /* first, as task_put() frees it */
	uint64_t      left;    /* slices to run */
	uint64_t      slices;  /* run */
	uint64_t      x;       /* the kernel's state */
	struct handle call;    /* the call's, on its owner's loop */
};

/* The most slices a test_task call may ask for: 25 s of work. */
enum { SPIN_MAX = 100'000 };

static enum task_state
mcp_spin_slice (struct task *task,
                struct loop *loop,
                uint64_t     until)
{
	(void)loop;
	struct mcp_spin *s = container_of(task, struct mcp_spin, task);
	uint64_t x = s->x;
	do {
		for (unsigned i = 0; i < 1024; ++i) {
			x ^= x << 13;
			x ^= x >> 7;
			x ^= x << 17;
		}
	} while (loop_clock() < until);
	s->x = x;
	++s->slices;
	return --s->left ? TASK_MORE : TASK_DONE;
}

/* A test_task call's task is back: reply with what it did, unless the call
 * has gone. Once the loop has stopped, MCP is gone, and the task is only
 * let go of.
 */
static void
mcp_spin_back (struct loop *loop,
               struct msg  *msg)
{
	struct task *task = container_of(msg, struct task, msg);
	struct mcp_spin *s = container_of(task, struct mcp_spin, task);
	struct mcp_slot *slot = mcp_slot_named(loop, s->call);
	if (!slot) {
		task_put(task, 1);
		return;
	}
	*mcp_slot_own(slot) = nullptr;
	char text[96];
	size_t n = util_format(text, sizeof text, "Ran %" PRIu64 " slices, and moved "
	                       "%" PRIu64 " times between loops.", s->slices,
	                       task->moves);
	task_put(task, 2);
	mcp_slot_tell(slot, (struct cstr_view){ text, n }, 0);
}

/* Run the @p slices a test_task call in @p slot asks for as a task, which
 * the call waits for.
 */
static void
mcp_slot_spin (struct mcp_slot    *slot,
               struct json_object *slices)
{
	static_assert(SPIN_MAX == 100'000, "the problem says");
	struct mcp *mcp = slot->req->mcp;
	struct loop *loop = mcp->loop;
	int64_t n = json_object_is_type(slices, json_type_int)
	          ? json_object_get_int64(slices) : 0;
	if (n < 1 || n > SPIN_MAX) {
		mcp_slot_tell(slot, CSTR_VIEW("The slices argument must be a whole number "
		                              "from 1 to 100000."), TOOL_FAILED);
		return;
	}
	struct mcp_spin *s = malloc(sizeof *s);
	int e = s ? mcp_slot_name(slot, &s->call) : ENOMEM;
	if (e) {
		free(s);
		mcp_slot_fail(slot, e);
		return;
	}
	task_init(&s->task, mcp->st->tasks, loop, mcp_spin_slice, mcp_spin_back);
	s->left = (uint64_t)n;
	s->slices = 0;
	s->x = loop->now | 1;
	*mcp_slot_own(slot) = &s->task;
	mcp_slot_park(slot);
	task_submit(&s->task, loop);
}

/* Start a test_task call in @p slot, the request's own: a task of the
 * slices its arguments @p args ask for; or, with a prompt instead, a call
 * that asks the upstream as background work.
 */
static void
mcp_slot_task (struct mcp_slot    *slot,
               struct json_object *args)
{
	struct json_object *slices = jb_get(args, "slices");
	if (!jb_get(args, "prompt")) {
		mcp_slot_spin(slot, slices);
		return;
	}
	if (slices) {
		mcp_slot_tell(slot, CSTR_VIEW("Give slices or a prompt, not both."), TOOL_FAILED);
		return;
	}
	slot->req->asks = JOB_BACKGROUND;
	mcp_req_ask(slot->req, args, false);
}

/* Let go of a test_task call's task, which cancels it if it runs. */
static void
mcp_slot_task_drop (struct mcp_slot *slot)
{
	task_drop(*mcp_slot_own(slot));
}

static void mcp_req_calls_next (struct mcp_req *req);
static int mcp_req_slice (struct mcp_req *req, struct jb_pool *pool,
                          struct cstr_view json);
static void mcp_req_turn_submit (struct mcp_req *req);
static void mcp_req_add_start (struct mcp_req *req);

/* Append the literal @p s to @p d. */
#define CAT(d, s) dstr_cat_n(d, s, sizeof s - 1)

/* Write piece @p q into the upload from its byte req->off on, as far as the
 * step's shares allow.
 */
static int
mcp_req_upload_piece (struct mcp_req         *req,
                      struct mcp_piece const *q)
{
	struct loop *loop = req->mcp->loop;
	enum loop_unit kind = q->text ? LOOP_ESCAPE : LOOP_COPY;
	while (req->off < q->bytes.len) {
		size_t left = q->bytes.len - req->off;
		size_t room = loop_room(loop, kind);
		size_t k = left < room ? left : room;
		int e = k ? mcp_piece_write(q, req->off, k, &req->upload, UPLOAD_MAX)
		          : EAGAIN;
		if (e)
			return e;
		loop_work(loop, kind, k);
		req->off += k;
	}
	return 0;
}

/* Write the @p n pieces at @p p into the upload, from piece req->next and
 * its byte req->off on, as far as the step's shares allow. Returns 0 once
 * all are written; EAGAIN if the step ran out first; ENOMEM; or ENOBUFS if
 * the upload would pass UPLOAD_MAX, its context full.
 */
static int
mcp_req_upload (struct mcp_req         *req,
                struct mcp_piece const *p,
                size_t                  n)
{
	for (; req->next < n; ++req->next, req->off = 0) {
		int e = mcp_req_upload_piece(req, &p[req->next]);
		if (e)
			return e;
	}
	req->next = 0;
	return 0;
}

/* The upload could not be written, as @p e says: past UPLOAD_MAX, its
 * context is full; else out of memory, the call ends.
 */
static void
mcp_req_upload_failed (struct mcp_req *req,
                       int             e)
{
	if (e == ENOBUFS) {
		mcp_req_compact_full(req);
		return;
	}
	mcp_req_fail(req, e);
}

/* Returns the tools that the turns of @p req may call, as
 * mcp_lending_lent() computes them for the client's role.
 */
static tool_mask
mcp_req_lent (struct mcp_req const *req)
{
	return mcp_lending_lent(&req->mcp->st->lending, req->s->grant.upstream);
}

/* The tokens that the upload, as written so far, takes: the last turn's
 * request and answer, as its usage said, and what was written after the
 * answer, at 4 bytes a token; or, unknown, all of it so.
 */
static uint64_t
mcp_req_tokens (struct mcp_req const *req)
{
	size_t len = buf_peek(&req->upload).len;
	return req->used ? req->used + (len > req->sent ? len - req->sent : 0) / 4 +
	                   req->filed / 4
	                 : (len + req->filed) / 4;
}

/** Stores the id of the call @p c, the call @p k of the turn of @p req, in
 *  @p *id. For a call without an id, the function makes up an id in the
 *  CALL_ID bytes at @p made. Returns 0, or EOVERFLOW if the made-up id
 *  does not fit.
 */
static int
mcp_req_call_id (struct mcp_req const *req,
                 struct ai_call const *c,
                 uint32_t              k,
                 char                 *made,
                 struct mcp_piece     *id)
{
	if (c->id.len) {
		*id = ESC_VIEW(c->id);
		return 0;
	}
	int n = snprintf(made, CALL_ID, "call_%" PRIu32 "_%" PRIu32, req->turn, k);
	if (n < 0 || n >= CALL_ID)
		return EOVERFLOW;
	*id = ESC(made, (size_t)n);
	return 0;
}

/* Whether the conversation fills the upstream's context, as far as known. */
static bool
mcp_req_full (struct mcp_req const *req)
{
	return req->context && req->used && mcp_req_tokens(req) >= req->context;
}

/* Whether @p a, if any, has what the turns may doze for: a job that runs, a
 * timer set, or what happened that they were not told yet.
 */
static bool
mcp_agenda_pending (struct mcp_agenda const *a)
{
	if (!a)
		return false;
	bool any = dstr_len(&a->notices) || dstr_len(&a->told);
	for (uint32_t k = 0; !any && k < a->n_jobs; ++k)
		any = !a->jobs[k].done;
	return any || ledger_timer_any(a->timers);
}

/* Whether the turns, which answered, doze until what their agenda has under
 * way happens, then go on: if the answer, streamed, has text, the turns may
 * take another, and the context has room.
 */
static bool
mcp_req_dozes (struct mcp_req const *req)
{
	struct job const *job = req->job;
	return job->status == JOB_TEXT && buf_peek(&job->result).len &&
	       req->turn < req->mcp->st->turns && !mcp_req_full(req) &&
	       mcp_agenda_pending(req->agenda);
}

/* Handle the end of a turn, once the answer of its job has arrived. First,
 * review_pending_log() moves the pending lines of the review record into its
 * read log (src/review.h). An answer that calls no tool is the answer of
 * the ask call, unless the turns doze after it. The answer of the last turn
 * is the answer of the ask call too. Otherwise parse the arguments of the
 * calls that the answer makes, then write the turn into the upload and run
 * the calls, or doze.
 */
static void
mcp_req_turn_done (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct job *job = req->job;
	long told = mcp->st->ep->context;
	req->used = job->used;
	req->filed = req->replay_filed;
	req->replay_filed = 0;
	req->context = told > 0 ? (uint32_t)told : job->context;
	req->cut = ai_cut(mcp->st->ep->api, job->reason);
	req->forced = 0;
	int e = req->review ? review_pending_log(req->review) : 0;
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	/* The calls are in the first n buffers of job->calls. */
	uint16_t n = 0;
	size_t bytes = 0;
	for (size_t k = 0; k < JOB_CALLS; ++k) {
		size_t len = buf_peek(&job->calls[k]).len;
		bytes += len;
		n += len != 0;
	}
	static_assert(CALLS_MAX == 16 << 20, "the result says");
	if (bytes > CALLS_MAX) {
		mcp_slot_result(&req->self, TEXT("The other AI's tool calls in one turn took "
		                              "more than 16 MiB, so none was run."), true);
		return;
	}
	if (!n && !mcp_req_dozes(req)) {
		mcp_req_enter(req, PHASE_TEXT);
		return;
	}
	if (req->turn >= mcp->st->turns) {
		req->stopped = true;
		hist_add(&mcp->counts->calls.stopped, 1);
		mcp_req_enter(req, PHASE_TEXT);
		return;
	}
	req->n_calls = n;
	req->part = 0;
	mcp_req_enter(req, PHASE_ARGS);
}

/* Parse the arguments @p args of the turn's call req->part into req->args,
 * nullptr if they are no JSON object: 0 once parsed, EAGAIN while the
 * parse goes on, or ENOMEM.
 */
static int
mcp_req_turn_arg (struct mcp_req   *req,
                  struct cstr_view  args)
{
	if (!args.len) {
		req->args[req->part] = json_object_new_object();
		return req->args[req->part] ? 0 : ENOMEM;
	}
	int e = mcp_req_slice(req, &req->mcp->answers, args);
	if (e == EAGAIN || e == ENOMEM)
		return e;
	struct json_object *v = req->msg;
	req->msg = nullptr;
	if (v && !json_object_is_type(v, json_type_object)) {
		jb_reaper_take(&req->reaper, v);
		v = nullptr;
	}
	req->args[req->part] = v;
	return 0;
}

/* Parse the arguments of the turn's tool calls, a slice a step; then write
 * the turn.
 */
static void
mcp_req_turn_args (struct mcp_req *req)
{
	for (; req->part < req->n_calls; ++req->part) {
		struct job_call h;
		struct ai_call c = job_call_parts(&h, buf_peek(&req->job->calls[req->part]).ptr);
		int e = mcp_req_turn_arg(req, c.args);
		if (e == EAGAIN) {
			loop_ready_bulk(req->mcp->loop, &req->step);
			return;
		}
		if (e) {
			mcp_req_fail(req, e);
			return;
		}
	}
	/* The turn goes after the request's messages, which it ends no more. */
	struct cstr_view end = ai_format(req->mcp->st->ep->api)->end;
	buf_cut(&req->upload, buf_peek(&req->upload).len - end.len, end.len);
	req->part = 0;
	req->replayed = 0;
	req->next = 0;
	req->off = 0;
	mcp_req_enter(req, PHASE_TURN);
}

/* The items of the job's replay that the turn has yet to write before its
 * byte @p to, as a piece: the first's comma left out if the list that they
 * go in has no item yet.
 */
static struct mcp_piece
mcp_req_items (struct mcp_req const *req,
               size_t                to)
{
	size_t from = req->replayed;
	if (to <= from)
		return (struct mcp_piece){};
	from += !req->listed;
	return RAW(buf_peek(&req->job->replay).ptr + from, to - from);
}

/** Stores the arguments of the call @p c, the call @p k of the turn of
 *  @p req, in @p *p in the form of the API @p f. If the arguments are not
 *  an object, the piece is "{}", so that the upstream can parse it.
 *  Returns 0, or ENOMEM if json-c cannot serialize the arguments.
 */
static int
mcp_req_call_args (struct mcp_req const   *req,
                   struct ai_format const *f,
                   struct ai_call const   *c,
                   uint32_t                k,
                   struct mcp_piece       *p)
{
	struct json_object *v = req->args[k];
	if (!(f->flags & AI_ARGS_RAW)) {
		*p = v && c->args.len ? ESC_VIEW(c->args) : TEXT("{}");
		return 0;
	}
	size_t len = 0;
	char const *json = v ? json_object_to_json_string_length(v, JB_WIRE, &len)
	                     : nullptr;
	*p = json ? RAW(json, len) : FRAG("{}");
	return v && !json ? ENOMEM : 0;
}

/** Writes the call @p c, the call req->part - 1 of the turn, into the
 *  upload of @p req in the form of the API @p f, after @p items, the items
 *  of the replay before the call. Returns 0, or the error of
 *  mcp_req_call_id(), mcp_req_call_args() or mcp_req_upload().
 */
static int
mcp_req_call_put (struct mcp_req         *req,
                  struct ai_format const *f,
                  struct ai_call const   *c,
                  struct mcp_piece        items)
{
	uint32_t k = req->part - 1u;
	char made[CALL_ID];
	struct mcp_piece id;
	struct mcp_piece args;
	int e = mcp_req_call_id(req, c, k, made, &id);
	if (e || (e = mcp_req_call_args(req, f, c, k, &args)))
		return e;
	struct mcp_piece const p[] = {
		items,
		BYTES(req->listed || items.bytes.len ? f->call_next : f->call),
		id,
		BYTES(f->name),
		ESC_VIEW(c->name),
		BYTES(f->args),
		args,
		BYTES(f->args_end),
		c->extra.len ? BYTES(f->extra) : (struct mcp_piece){},
		BYTES(c->extra),
		BYTES(f->call_end),
	};
	e = mcp_req_upload(req, p, ARRAY_SIZE(p));
	if (!e)
		req->listed = true;
	return e;
}

/* Write the turn's call under way, after the items of the replay before
 * it; or only those, as the API's calls are in the replay.
 */
static int
mcp_req_turn_call (struct mcp_req         *req,
                   struct ai_format const *f)
{
	struct job_call h;
	struct ai_call c = job_call_parts(&h, buf_peek(&req->job->calls[req->part - 1u]).ptr);
	struct mcp_piece items = mcp_req_items(req, h.replay);
	int e = f->call.len ? mcp_req_call_put(req, f, &c, items)
	                    : mcp_req_upload(req, &items, 1);
	if (e)
		return e;
	req->listed = req->listed || items.bytes.len;
	req->replayed = h.replay;
	return 0;
}

/* Write part req->part of the turn into the upload: first its head, with
 * its text and thinking if the API writes them, then each tool call, then
 * the replay's items after the last and its end, an answer's if it calls
 * none.
 */
static int
mcp_req_turn_part (struct mcp_req *req)
{
	struct ai_format const *f = ai_format(req->mcp->st->ep->api);
	struct job *job = req->job;
	if (req->part > req->n_calls) {
		struct mcp_piece const p[] = {
			mcp_req_items(req, buf_peek(&job->replay).len),
			BYTES(req->n_calls ? f->turn_end : f->answer_end),
		};
		return mcp_req_upload(req, p, ARRAY_SIZE(p));
	}
	if (req->part)
		return mcp_req_turn_call(req, f);
	struct span text = buf_peek(&job->result);
	struct span thought = buf_peek(&job->thought);
	bool thinks = thought.len && f->thought.len;
	struct mcp_piece const p[] = {
		BYTES(f->turn),
		BYTES(f->text),
		ESC(text.ptr, f->text.len ? text.len : 0),
		thinks ? BYTES(f->thought) : (struct mcp_piece){},
		ESC(thought.ptr, thinks ? thought.len : 0),
		req->n_calls ? BYTES(f->calls) : (struct mcp_piece){},
	};
	int e = mcp_req_upload(req, p, ARRAY_SIZE(p));
	if (!e)
		req->listed = !f->turn.len;
	return e;
}

static void mcp_req_doze (struct mcp_req *req);

/* Write the turn into the upload, a slice a step; then keep its calls, let
 * go of the job and run the first; or doze, if it calls none.
 */
static void
mcp_req_turn_write (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	for (; req->part <= req->n_calls + 1u; ++req->part) {
		int e = mcp_req_turn_part(req);
		if (e == EAGAIN) {
			loop_ready_bulk(loop, &req->step);
			return;
		}
		if (e) {
			mcp_req_upload_failed(req, e);
			return;
		}
	}
	if (!req->n_calls) {
		mcp_req_doze(req);
		return;
	}
	int e = 0;
	for (uint16_t k = 0; !e && k < req->n_calls; ++k) {
		struct span c = buf_peek(&req->job->calls[k]);
		e = mcp_piece_write(&RAW(c.ptr, c.len), 0, c.len, &req->calls, CALLS_MAX);
		loop_work(loop, LOOP_COPY, c.len);
	}
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	/* The usage counts the turn: what is written after it counts on. */
	req->sent = buf_peek(&req->upload).len;
	mcp_req_drop(req);
	e = mcp_req_slots_make(req);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	req->part = 0;
	req->at = req->launch_at = 0;
	mcp_req_calls_next(req);
}

/** Adds a line for the tool call @p c with the arguments @p args to the
 *  trace of @p req: the name of the tool and, as @p trace selects, the
 *  first TRACE_ARGS bytes of the arguments or the action. The line ends in
 *  mcp_req_trace_end(). Returns 0 or ENOMEM.
 */
static int
mcp_req_trace (struct mcp_req       *req,
               struct ai_call const *c,
               struct json_object   *args,
               enum mcp_trace        trace)
{
	if (req->traced++ >= TRACE_MAX)
		return 0;
	char const *name = c->name.data;
	if (!CAT(&req->trace, "- ") ||
	    !dstr_cat_n(&req->trace, name, jb_utf8(name, util_clip(name, c->name.len, 64))))
		return ENOMEM;
	switch (trace) {
	case TRACE_NAME:
		return 0;
	case TRACE_ACTION:
		struct cstr_view action = jb_view(jb_get(args, "action"));
		return !action.data || (CAT(&req->trace, " ") &&
		                        dstr_cat_n(&req->trace, action.data,
		                                   util_clip(action.data, action.len, 16)))
		       ? 0 : ENOMEM;
	default:
		char const *json = c->args.data;
		size_t n = jb_utf8(json, util_clip(json, c->args.len, TRACE_ARGS));
		if (n && !(CAT(&req->trace, " ") && dstr_cat_n(&req->trace, json, n)))
			return ENOMEM;
		return n < c->args.len && !CAT(&req->trace, "...") ? ENOMEM : 0;
	}
}

/* End the trace's line for the call under way: whether it failed, or the
 * bytes of its output.
 */
static int
mcp_req_trace_end (struct mcp_req *req,
                   bool            failed,
                   size_t          bytes)
{
	if (req->traced > TRACE_MAX)
		return 0;
	if (failed)
		return CAT(&req->trace, ": failed\n") ? 0 : ENOMEM;
	return dstr_printf(&req->trace, ": %zu bytes\n", bytes) ? 0 : ENOMEM;
}

/** Returns the id of the tool that askai offers with the name @p name, or
 *  TOOL_MAX. No tool name is empty. The ids of the tools that a role can
 *  grant are below st->n_granted.
 */
static uint32_t
mcp_static_tool_of (struct mcp_static const *st,
                    struct cstr_view         name)
{
	uint32_t id = 0;
	while (id < st->n_offers && !cstr_view_equal(st->offers[id].tool.name, name))
		++id;
	return id < st->n_offers ? id : TOOL_MAX;
}

/** Returns @p id if the turns of @p req may call the tool @p id, or
 *  TOOL_MAX.
 */
static uint32_t
mcp_req_lent_tool (struct mcp_req const *req,
                   uint32_t              id)
{
	return id < TOOL_MAX && req->lent >> id & 1 ? id : TOOL_MAX;
}

/** Returns true if the role of the client of @p req grants the tool @p id. */
static bool
mcp_req_granted (struct mcp_req const *req,
                 uint32_t              id)
{
	return id < req->mcp->st->n_granted && req->s->grant.tools >> id & 1;
}

/* The words of the log for the arguments of a turn's tool call of @p req
 * that are not a JSON object, so that the call was not run: the output
 * token limit cut them short, or the model wrote another value.
 */
static struct cstr_view
mcp_req_no_args (struct mcp_req const *req)
{
	return req->cut ? CSTR_VIEW("cut short by the output token limit")
	                : CSTR_VIEW("not a JSON object");
}

/** Logs, with --verbose, a turn's call of the tool @p name, which the
 *  turns of @p req may not call, with its arguments @p args. @p known is
 *  the id of the tool, or TOOL_MAX if askai offers no such tool. The line
 *  quotes the name, which comes from the upstream.
 */
static void
mcp_req_log_refused (struct mcp_req const *req,
                     struct cstr_view      name,
                     uint32_t              known,
                     struct json_object   *args)
{
	if (log_threshold < LOG_VERBOSE)
		return;
	char quoted[NAME_LOG + 8];
	mcp_quote(quoted, sizeof quoted, name, NAME_LOG);
	char shown[SHOWN];
	mcp_args_shown(&req->mcp->st->offers[known], args, mcp_req_no_args(req), shown);
	LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " turn %" PRIu32 ": tool call %s "
	    "refused: %s; arguments %s", req->mcp->loop->id, req->asked, req->turn, quoted,
	    known < TOOL_MAX ? "not lent to this ask call" : "askai offers no such tool",
	    shown);
}

static void mcp_agenda_alarm (struct loop *loop, struct loop_timer *timer);

/* An agenda for the turns of @p req, whose jobs are its own slots; nullptr,
 * with ENOMEM in @p *e, if out of memory.
 */
static struct mcp_agenda *
mcp_agenda_create (struct mcp_req *req,
                   int            *e)
{
	struct mcp_agenda *a = malloc(sizeof *a);
	*e = a ? 0 : ENOMEM;
	if (!a)
		return nullptr;
	*a = (struct mcp_agenda){
		.alarm.fn = mcp_agenda_alarm,
		.notices = DSTR_INIT,
		.told = DSTR_INIT,
		.said = DSTR_INIT,
		.req = req
	};
	for (size_t k = 0; k < JOBS_MAX; ++k)
		a->jobs[k] = mcp_slot(req, ROLE_JOB);
	return a;
}

/* Stop the agenda at @p *p_agenda, its jobs and timers, and free it. */
static void
mcp_agenda_destroy (struct mcp_agenda **p_agenda)
{
	struct mcp_agenda *a = *p_agenda;
	if (!a)
		return;
	*p_agenda = nullptr;
	loop_timer_stop(a->req->mcp->loop, &a->alarm);
	for (size_t k = 0; k < JOBS_MAX; ++k) {
		mcp_slot_drop(&a->jobs[k]);
		dstr_cleanup(&a->jobs[k].answer);
		json_object_put(a->jobs[k].args);
	}
	dstr_cleanup(&a->notices);
	dstr_cleanup(&a->told);
	dstr_cleanup(&a->said);
	free(a);
}

/* The agenda of @p req's turns, made if it has none yet; nullptr, with the
 * error in @p *e, if it cannot be.
 */
static struct mcp_agenda *
mcp_req_agenda (struct mcp_req *req,
                int            *e)
{
	*e = 0;
	if (!req->agenda)
		req->agenda = mcp_agenda_create(req, e);
	return req->agenda;
}

/** Sets the alarm of @p a to the first time that is due, the due of a
 *  timer or the end of the wait, if any. Returns 0 or ENOMEM.
 */
static int
mcp_agenda_arm (struct mcp_agenda *a)
{
	struct loop *loop = a->req->mcp->loop;
	uint64_t due = a->wait ? a->wait_end : UINT64_MAX;
	for (size_t i = 0; i < ARRAY_SIZE(a->timers); ++i)
		if (a->timers[i].due && a->timers[i].due < due)
			due = a->timers[i].due;
	if (due == UINT64_MAX) {
		loop_timer_stop(loop, &a->alarm);
		return 0;
	}
	return loop_timer_set(loop, &a->alarm, due);
}

/* End the wait that @p a's turn waits in: it says what happened, or that
 * nothing did in the seconds it took.
 */
static void
mcp_agenda_wait_end (struct mcp_agenda *a)
{
	struct mcp_slot *slot = a->wait;
	a->wait = nullptr;
	bool said = dstr_len(&a->notices)
	          ? dstr_cat_n(&slot->answer, dstr_get(&a->notices), dstr_len(&a->notices))
	          : dstr_printf(&slot->answer, "Nothing happened in %" PRIu32 " s.", a->waited);
	dstr_clear(&a->notices);
	int e = said ? mcp_agenda_arm(a) : ENOMEM;
	if (e) {
		mcp_req_fail(a->req, e);
		return;
	}
	mcp_slot_respond(slot, false);
}

/* What happened wakes @p a's turns, which dozed: a message of the user's
 * says what, and that the next turn is their last if it is, and so does
 * the reply, before the answer that follows.
 */
static void
mcp_agenda_wake (struct mcp_agenda *a)
{
	struct mcp_req *req = a->req;
	bool ok = dstr_cat_n(&a->told, dstr_get(&a->notices), dstr_len(&a->notices)) &&
	          dstr_cat_n(&a->said, dstr_get(&a->told), dstr_len(&a->told)) &&
	          CAT(&a->said, "\n") &&
	          (req->turn + 1u < req->mcp->st->turns ||
	           CAT(&a->told, NOTICE_LAST "\n"));
	dstr_clear(&a->notices);
	if (!ok) {
		mcp_req_fail(req, ENOMEM);
		return;
	}
	req->next = 0;
	req->off = 0;
	mcp_req_enter(req, PHASE_WAKE);
}

/* The turns answered, the answer in the upload, with what their agenda has
 * under way: keep it for the reply, let go of the job, and doze until what
 * happens next wakes them, or wake them now if something has.
 */
static void
mcp_req_doze (struct mcp_req *req)
{
	struct mcp_agenda *a = req->agenda;
	struct span text = buf_peek(&req->job->result);
	if (!dstr_cat_n(&a->said, (char const *)text.ptr, text.len) ||
	    !CAT(&a->said, "\n\n")) {
		mcp_req_fail(req, ENOMEM);
		return;
	}
	loop_work(req->mcp->loop, LOOP_COPY, text.len);
	/* The usage counts the turn: what is written after it counts on. */
	req->sent = buf_peek(&req->upload).len;
	mcp_req_drop(req);
	if (dstr_len(&a->notices) || dstr_len(&a->told)) {
		mcp_agenda_wake(a);
		return;
	}
	req->phase = PHASE_DOZE;
	mcp_req_wait(req);
}

/* The job in @p slot has its output, or failed: the next result says so,
 * and a wait for it ends, or it wakes the turns, if they doze.
 */
static void
mcp_agenda_job_done (struct mcp_slot *slot)
{
	struct mcp_agenda *a = slot->req->agenda;
	uint32_t n = (uint32_t)(slot - a->jobs) + 1;
	if (!dstr_printf(&a->notices, "[askai: job %" PRIu32 ", %s, %s: call jobs with the "
	                 "action result and job %" PRIu32 " for its output.]\n", n,
	                 mcp_slot_offer(slot)->tool.name.data,
	                 slot->failed ? "failed" : "finished", n)) {
		mcp_req_fail(slot->req, ENOMEM);
		return;
	}
	if (a->wait && (!a->wait_for || a->wait_for >> (n - 1) & 1))
		mcp_agenda_wait_end(a);
	else if (a->req->phase == PHASE_DOZE)
		mcp_agenda_wake(a);
}

/* Appends the notice that the timer @p t fired, with its note, to @p d.
 * Returns false if out of memory.
 */
static bool
mcp_notice_timer (struct dstr               *d,
                  struct ledger_timer const *t)
{
	return dstr_printf(d, "[askai: timer %" PRIu32 " fired%s%.*s.]\n", t->id,
	                   t->len ? ": " : "", (int)t->len, t->note);
}

/** Handles the alarm @p timer of an agenda. The timers that are due fire,
 *  and the function adds a notice of each to the agenda
 *  (mcp_notice_timer()). A wait ends if a timer fired or if its time is
 *  up. Otherwise a timer that fired wakes the turns if they doze.
 */
static void
mcp_agenda_alarm (struct loop       *loop,
                  struct loop_timer *timer)
{
	struct mcp_agenda *a = container_of(timer, struct mcp_agenda, alarm);
	bool fired = false;
	for (size_t i = 0; i < ARRAY_SIZE(a->timers); ++i) {
		struct ledger_timer *t = &a->timers[i];
		if (!t->due || t->due > loop->now)
			continue;
		t->due = 0;
		fired = true;
		if (!mcp_notice_timer(&a->notices, t)) {
			mcp_req_fail(a->req, ENOMEM);
			return;
		}
	}
	if (a->wait && (fired || a->wait_end <= loop->now)) {
		mcp_agenda_wait_end(a);
		return;
	}
	int e = mcp_agenda_arm(a);
	if (e)
		mcp_req_fail(a->req, e);
	else if (fired && a->req->phase == PHASE_DOZE)
		mcp_agenda_wake(a);
}

/**
 * Starts the call of @p slot with the arguments @p args, and stores the
 * time of the start in the slot.
 *
 * A client can send any JSON value as the arguments. If @p args is neither
 * nullptr nor an object, the call ends with an error text. Otherwise the
 * function looks up the instance of the plugin of the tool for the user of
 * the request with instance_table_get(), and then calls start() of the
 * kind of the tool. If instance_table_get() returns EBUSY, the call ends
 * as failed. Any other error of instance_table_get() ends the whole
 * request.
 *
 * No function of a plugin is running when this function is called.
 */
static void
mcp_slot_start (struct mcp_slot    *slot,
                struct json_object *args)
{
	slot->start = loop_clock();
	if (args && !json_object_is_type(args, json_type_object)) {
		mcp_slot_tell(slot, CSTR_VIEW("Tool arguments must be an object."), TOOL_FAILED);
		return;
	}
	struct mcp_req *req = slot->req;
	struct mcp *mcp = req->mcp;
	struct mcp_offer const *offer = mcp_slot_offer(slot);
	int e = instance_table_get(&mcp->instances, mcp->st->plugins[offer->tool.part].entry.plugin,
	                           req->principal, req->generation, &slot->inst);
	if (e == EBUSY)
		mcp_slot_result(slot, TEXT(busy), true);
	else if (e)
		mcp_req_fail(req, e);
	else
		offer->tool.kind->start(slot, args);
}

/* The words for the kinds and the statuses of the records of the ledger,
 * which the answers of a client's jobs and wait use.
 */
static char const *const record_kinds[] = {
	[LEDGER_TASK] = "task",
	[LEDGER_JOB]  = "job",
};
static char const *const record_status[LEDGER_STATUSES] = {
	[LEDGER_WORKING]   = "running",
	[LEDGER_COMPLETED] = "completed",
	[LEDGER_FAILED]    = "failed",
	[LEDGER_CANCELLED] = "cancelled",
};

/* The answer to a client's call of background, timer or wait that the
 * ledger refuses for want of room.
 */
static char const no_room[] = "askai has no room for more jobs, timers or waits "
                              "now; try again later.";

/* Answers a client's call in @p slot that the ledger refused with @p e:
 * with @p busy for EBUSY, and with no_room for ENOSPC. Any other error
 * fails the call.
 */
static void
mcp_slot_refused (struct mcp_slot  *slot,
                  int               e,
                  struct mcp_piece  busy)
{
	if (e == EBUSY || e == ENOSPC)
		mcp_slot_result(slot, e == EBUSY ? busy : TEXT(no_room), true);
	else
		mcp_slot_fail(slot, e);
}

/* Returns the argument seconds of @p args if it is a whole number from 1
 * to @p max, else 0.
 */
static int64_t
mcp_seconds (struct json_object *args,
             int64_t             max)
{
	struct json_object *o = jb_get(args, "seconds");
	int64_t secs = json_object_is_type(o, json_type_int) ? json_object_get_int64(o) : 0;
	return secs >= 1 && secs <= max ? secs : 0;
}

/* Returns the whole seconds from @p from to @p to, or 0 if @p to is not
 * later.
 */
static uint64_t
mcp_seconds_since (uint64_t from,
                   uint64_t to)
{
	return to > from ? (to - from) / NS : 0;
}

/* Detach a client call into the ledger, with its result read through the
 * same tool when @p own_tool, else through jobs.
 */
static int
mcp_req_background (struct mcp_req *req,
                    uint32_t        tool,
                    bool            own_tool)
{
	static_assert(LEDGER_RUNNING == 8, "the problem says");
	struct mcp_static const *st = req->mcp->st;
	int e;
	struct ledger_task *task = req->admitted;
	req->admitted = nullptr;
	if (!task)
		task = mcp_req_record(req, LEDGER_JOB, tool, &e);
	if (!task) {
		mcp_slot_refused(&req->self, e, TEXT("You have 8 jobs and tasks running, the most "
		                                     "at once: wait for one to end."));
		return e;
	}
	char text[512];
	size_t n = own_tool
	         ? util_format(text, sizeof text, "Started job %.*s. Call %s with "
	                       "{\"job\":\"%.*s\"} in one second for its status or result; add "
	                       "\"action\":\"cancel\" to cancel it.", LEDGER_ID, task->id,
	                       st->offers[tool].tool.name.data, LEDGER_ID, task->id)
	         : util_format(text, sizeof text, "Started job %.*s. Call wait to wait for "
	                       "it, then jobs with the action result and this id.",
	                       LEDGER_ID, task->id);
	struct mcp_piece const p[] = {
		FRAG("{\"content\":[{\"type\":\"text\",\"text\":\""),
		ESC(text, n),
		FRAG("\"}]"),
		req->modern ? DSTR(&st->modern_tail) : FRAG("}"),
		FRAG("}"),
	};
	return mcp_req_detach(req, task, p, ARRAY_SIZE(p));
}

/** Starts the client's call of background in @p slot, which is req->self,
 *  with the members tool and arguments of @p args. The request answers
 *  with the id of the job at once and then runs the call as the job.
 */
static void
mcp_slot_client_background (struct mcp_slot    *slot,
                            struct json_object *args)
{
	struct mcp_req *req = slot->req;
	struct mcp_static const *st = req->mcp->st;
	uint32_t id = mcp_static_tool_of(st, jb_view(jb_get(args, "tool")));
	struct mcp_offer const *offer = &st->offers[id];
	struct json_object *call = jb_get(args, "arguments");
	struct mcp_piece problem =
		!mcp_req_granted(req, id) || offer->tool.kind->flags & KF_AGENDA
		? TEXT("The tool argument must name a tool of yours other than background, "
		       "jobs, wait and timer.")
		: call && !json_object_is_type(call, json_type_object)
		? TEXT("The arguments argument must be an object.")
		: (struct mcp_piece){};
	if (problem.bytes.data) {
		mcp_slot_result(slot, problem, true);
		return;
	}
	if (mcp_req_background(req, id, false))
		return;
	req->asks |= JOB_BACKGROUND;
	slot->tool = (uint8_t)id;
	/* The log line of the call's end shows the arguments of the job's
	 * call, by the rules of its tool. */
	if (log_threshold >= LOG_VERBOSE)
		mcp_args_shown(offer, call, CSTR_VIEW("none"), req->call_args);
	LOG(LOG_VERBOSE, "tool call background from %s became job %.*s, a call of %s",
	    mcp_req_client(req), LEDGER_ID, req->task->id, offer->tool.name.data);
	/* Last, as the start of the job's call may end the request. */
	mcp_slot_start(slot, call);
}

/** Starts the call that @p args describes as a job in the background, in a
 *  slot of the agenda. The turns must be lent the tool, and the flags of
 *  its kind must contain TOOL_DETACH. The call in @p slot returns the
 *  number of the job. mcp_slot_client_background() handles a client's call.
 */
static void
mcp_slot_background (struct mcp_slot    *slot,
                     struct json_object *args)
{
	if (slot->role == ROLE_OWN) {
		mcp_slot_client_background(slot, args);
		return;
	}
	struct mcp_req *req = slot->req;
	struct mcp_static const *st = req->mcp->st;
	struct json_object *call = jb_get(args, "arguments");
	uint32_t id = mcp_req_lent_tool(req, mcp_static_tool_of(st, jb_view(jb_get(args, "tool"))));
	struct mcp_piece problem =
		!(st->offers[id].tool.kind->flags & TOOL_DETACH)
		? TEXT("The tool argument must name a tool of yours that may run in the "
		       "background, such as git or man.")
		: call && !json_object_is_type(call, json_type_object)
		? TEXT("The arguments argument must be an object.")
		: (struct mcp_piece){};
	if (problem.bytes.data) {
		mcp_slot_result(slot, problem, true);
		return;
	}
	int e;
	struct mcp_agenda *a = mcp_req_agenda(req, &e);
	if (!a) {
		mcp_req_fail(req, e);
		return;
	}
	if (a->n_jobs == JOBS_MAX) {
		mcp_slot_result(slot, TEXT("You have started 8 jobs, the most a task may."),
		                true);
		return;
	}
	struct mcp_slot *job = &a->jobs[a->n_jobs];
	/* job->args is a deep copy of the arguments. mcp_req_args_free()
	 * frees the arguments of the call of background once its result is
	 * written, and the job usually runs longer than that. */
	int bad = call ? json_object_deep_copy(call, &job->args, nullptr)
	               : !(job->args = json_object_new_object());
	if (bad) {
		mcp_req_fail(req, ENOMEM);
		return;
	}
	job->tool = (uint8_t)id;
	uint32_t n = ++a->n_jobs;
	char text[64];
	mcp_slot_result(slot, ESC(text, util_format(text, sizeof text, "Started job %"
	                                            PRIu32 ".", n)), false);
	/* Last, as the job's start may end the request. */
	mcp_slot_start(job, job->args);
}

/* List the jobs into @p slot's answer: each with its tool, its arguments,
 * and where it stands.
 */
static int
mcp_slot_jobs_list (struct mcp_slot *slot)
{
	struct mcp_agenda const *a = slot->req->agenda;
	uint32_t n = a ? a->n_jobs : 0;
	if (!n)
		return CAT(&slot->answer, "You have started no jobs.") ? 0 : ENOMEM;
	for (uint32_t k = 0; k < n; ++k) {
		struct mcp_slot const *job = &a->jobs[k];
		size_t len = 0;
		char const *args = json_object_to_json_string_length(job->args, JB_WIRE, &len);
		char state[48];
		if (job->cancelled)
			util_format(state, sizeof state, "cancelled");
		else if (!job->done)
			util_format(state, sizeof state, "running");
		else
			util_format(state, sizeof state, "%s, %zu bytes",
			            job->failed ? "failed" : "finished", mcp_slot_output(job).len);
		if (!args || !dstr_printf(&slot->answer, "job %" PRIu32 ": %s %.*s%s: %s\n",
		                          k + 1, mcp_slot_offer(job)->tool.name.data,
		                          (int)util_clip(args, len, 100), args,
		                          len > 100 ? "..." : "", state))
			return ENOMEM;
	}
	return 0;
}

/* Give @p job's output, as the jobs tool's call in @p slot asks with
 * @p args: from its offset, once it has finished.
 */
static void
mcp_slot_jobs_result (struct mcp_slot    *slot,
                      struct mcp_slot    *job,
                      struct json_object *args)
{
	if (!job->done) {
		mcp_slot_result(slot, TEXT("That job still runs: wait for it."), true);
		return;
	}
	if (mcp_slot_offset(slot, args))
		return;
	slot->of = job;
	mcp_slot_respond(slot, job->failed);
}

/* Cancel @p job, as the jobs tool's call in @p slot asks, if it runs. */
static void
mcp_slot_jobs_cancel (struct mcp_slot *slot,
                      struct mcp_slot *job)
{
	if (job->done) {
		mcp_slot_result(slot, TEXT("That job runs no more."), true);
		return;
	}
	mcp_slot_drop(job);
	job->done = job->cancelled = true;
	mcp_slot_result(slot, TEXT("Cancelled the job."), false);
}

/* Returns a reference to the record that the argument job of @p args
 * names, for a client's call of jobs in @p slot. If the owner of the call
 * has no such record, returns nullptr, and the call answers why.
 */
static struct ledger_task *
mcp_slot_client_find (struct mcp_slot    *slot,
                      struct json_object *args,
                      bool                own_tool)
{
	struct mcp_req *req = slot->req;
	size_t len = 0;
	char const *id = jb_str(jb_get(args, "job"), &len);
	if (!id || len != LEDGER_ID) {
		mcp_slot_result(slot, TEXT("The job argument must be the id of a job of yours."),
		                true);
		return nullptr;
	}
	struct ledger_task *task = ledger_find(req->mcp->st->ledger, mcp_req_owner(req), id,
	                                       len);
	if (task && own_tool && task->tool != slot->tool) {
		ledger_task_put(task);
		task = nullptr;
	}
	if (!task)
		mcp_slot_result(slot, own_tool
		                ? TEXT("No job of yours from this tool has that id; askai stores "
		                       "a job for up to an hour after it ends.")
		                : TEXT("No job of yours has that id; askai stores a job for "
		                       "an hour after it ends."), true);
	return task;
}

/* Writes the records and the timers of the owner of a client's call of
 * jobs into the answer of the call's @p slot: the records, the newest
 * first, then the timers. Returns 0, or ENOMEM.
 */
static int
mcp_slot_client_list (struct mcp_slot *slot)
{
	struct mcp_req *req = slot->req;
	struct mcp_static const *st = req->mcp->st;
	struct dstr *d = &slot->answer;
	struct ledger_look look;
	int e = ledger_list(st->ledger, mcp_req_owner(req), &look);
	if (e)
		return e;
	uint64_t day = clk_realtime_ns();
	uint64_t now = loop_clock();
	bool ok = true;
	for (uint32_t i = 0; ok && i < look.n_tasks; ++i) {
		struct ledger_task const *t = look.tasks[i];
		enum ledger_status status = atomic_load_explicit(&t->status,
		                                                 memory_order_acquire);
		bool told = atomic_load_explicit(&t->told, memory_order_relaxed);
		ok = dstr_printf(d, "%s %.*s: %s: ", record_kinds[t->kind], LEDGER_ID, t->id,
		                 st->offers[t->tool].tool.name.data) &&
		     (status == LEDGER_WORKING
		      ? dstr_printf(d, "running for %" PRIu64 " s\n",
		                    mcp_seconds_since(t->created, day))
		      : dstr_printf(d, "%s %" PRIu64 " s ago%s\n", record_status[status],
		                    mcp_seconds_since(t->ended, day), told ? "" : ", unread"));
	}
	if (ok && look.more)
		ok = dstr_printf(d, "and %" PRIu64 " older records\n", look.more);
	for (uint32_t i = 0; ok && i < look.n_timers; ++i) {
		struct ledger_timer const *t = &look.timers[i];
		ok = (t->due > now ? dstr_printf(d, "timer %" PRIu32 ": fires in %" PRIu64 " s",
		                                 t->id, (t->due - now + NS - 1) / NS)
		                   : dstr_printf(d, "timer %" PRIu32 ": fired %" PRIu64 " s ago, "
		                                 "unread", t->id, (now - t->due) / NS)) &&
		     dstr_printf(d, "%s%.*s\n", t->len ? ": " : "", (int)t->len, t->note);
	}
	if (ok && !look.n_tasks && !look.n_timers)
		ok = CAT(d, "You have no jobs and no timers.");
	ledger_look_put(&look);
	return ok ? 0 : ENOMEM;
}

/* Answers a client's call of jobs in @p slot with the result of the job
 * that @p args names, once the job has ended: the reply of the job's call,
 * which the record has, and which mcp_req_show() copies.
 */
static void
mcp_slot_client_result (struct mcp_slot    *slot,
                        struct json_object *args,
                        bool                own_tool)
{
	struct mcp_req *req = slot->req;
	struct ledger_task *task = mcp_slot_client_find(slot, args, own_tool);
	if (!task)
		return;
	enum ledger_status status = atomic_load_explicit(&task->status, memory_order_acquire);
	if (own_tool && status == LEDGER_WORKING) {
		ledger_task_put(task);
		mcp_slot_result(slot, TEXT("That job is still running. Call this same tool with "
		                           "the same job id in one second for its result."), false);
		return;
	}
	struct mcp_piece problem =
		status == LEDGER_WORKING ? TEXT("That job still runs: wait for it.")
		: status == LEDGER_CANCELLED ? TEXT("That job was cancelled.")
		: !task->result ? TEXT("That job failed, and askai stored no result of it.")
		: (struct mcp_piece){};
	if (problem.bytes.data) {
		ledger_task_put(task);
		mcp_slot_result(slot, problem, true);
		return;
	}
	atomic_store_explicit(&task->told, 1, memory_order_relaxed);
	slot->done = true;
	req->shown = task;
	req->off = 0;
	int e = mcp_req_open(req, FRAG(""));
	if (e) {
		mcp_req_abort(req, e);
		return;
	}
	mcp_req_enter(req, PHASE_SHOW);
}

/* Cancels the job or the timer that @p args names, for a client's call of
 * jobs in @p slot.
 */
static void
mcp_slot_client_cancel (struct mcp_slot    *slot,
                        struct json_object *args,
                        bool                own_tool)
{
	struct mcp_req *req = slot->req;
	struct json_object *timer = jb_get(args, "timer");
	if (!jb_get(args, "job") == !timer) {
		mcp_slot_result(slot, TEXT("The job argument or the timer argument must name "
		                           "what to cancel."), true);
		return;
	}
	char text[192];
	if (timer) {
		if (!json_object_is_type(timer, json_type_int)) {
			mcp_slot_result(slot, TEXT("The timer argument must be the number of a "
			                           "timer of yours."), true);
			return;
		}
		int64_t n = json_object_get_int64(timer);
		int e = n < 1 || n > UINT32_MAX ? ENOENT
		      : ledger_timer_cancel(req->mcp->st->ledger, mcp_req_owner(req),
		                            (uint32_t)n);
		size_t len = !e ? util_format(text, sizeof text, "Cancelled timer %" PRId64 ".", n)
		           : e == EALREADY ? util_format(text, sizeof text, "Timer %" PRId64 " has "
		                                         "fired, and a call of wait reports it.", n)
		           : util_format(text, sizeof text, "You have no timer %" PRId64 " set.", n);
		mcp_slot_result(slot, ESC(text, len), e != 0);
		return;
	}
	struct ledger_task *task = mcp_slot_client_find(slot, args, own_tool);
	if (!task)
		return;
	bool runs = atomic_load_explicit(&task->status, memory_order_acquire) ==
	            LEDGER_WORKING;
	size_t len = 0;
	if (runs) {
		ledger_task_cancel(task);
		len = util_format(text, sizeof text, "askai is cancelling %s %.*s. %s",
		                  record_kinds[task->kind], LEDGER_ID, task->id, own_tool
		                  ? "Call this same tool with the job id to check its status."
		                  : "Call wait to wait for its end.");
	}
	ledger_task_put(task);
	mcp_slot_result(slot, runs ? ESC(text, len) : TEXT("That job runs no more."), !runs);
}

/* A client's call of jobs in @p slot, the request's own: by the argument
 * action of @p args, list the records and the timers of the call's owner,
 * answer with the result of a job, or cancel a job or a timer.
 */
static void
mcp_slot_client_jobs (struct mcp_slot    *slot,
                      struct json_object *args)
{
	struct json_object *action = jb_get(args, "action");
	if (jb_is(action, "result")) {
		mcp_slot_client_result(slot, args, false);
		return;
	}
	if (jb_is(action, "cancel")) {
		mcp_slot_client_cancel(slot, args, false);
		return;
	}
	if (!jb_is(action, "list")) {
		mcp_slot_result(slot, TEXT("The action argument must be list, result or cancel."),
		                true);
		return;
	}
	int e = mcp_slot_client_list(slot);
	if (e) {
		mcp_slot_fail(slot, e);
		return;
	}
	mcp_slot_respond(slot, false);
}

/* The jobs tool's call in @p slot: list the jobs, or give the output of
 * the one @p args names, or cancel it. A client's call is
 * mcp_slot_client_jobs()'s.
 */
static void
mcp_slot_jobs (struct mcp_slot    *slot,
               struct json_object *args)
{
	if (slot->role == ROLE_OWN) {
		mcp_slot_client_jobs(slot, args);
		return;
	}
	struct mcp_agenda *a = slot->req->agenda;
	struct json_object *action = jb_get(args, "action");
	if (jb_is(action, "list")) {
		int e = mcp_slot_jobs_list(slot);
		if (e) {
			mcp_req_fail(slot->req, e);
			return;
		}
		mcp_slot_respond(slot, false);
		return;
	}
	bool result = jb_is(action, "result");
	struct json_object *o = jb_get(args, "job");
	int64_t n = json_object_is_type(o, json_type_int) ? json_object_get_int64(o) : 0;
	struct mcp_slot *job = a && n >= 1 && n <= a->n_jobs ? &a->jobs[n - 1] : nullptr;
	struct mcp_piece problem =
		!result && !jb_is(action, "cancel")
		? TEXT("The action argument must be list, result or cancel.")
		: !job ? TEXT("The job argument must be the number of a job you started.")
		: job->cancelled ? TEXT("That job was cancelled.")
		: (struct mcp_piece){};
	if (problem.bytes.data) {
		mcp_slot_result(slot, problem, true);
		return;
	}
	if (result)
		mcp_slot_jobs_result(slot, job, args);
	else
		mcp_slot_jobs_cancel(slot, job);
}

/* A client's wait: the own word of the slot of its call of wait. */
struct mcp_wait {
	struct loop_timer   alarm;  /* the wait's end, or the time of the first
	                               timer of the owner that has not fired, if
	                               that is sooner */
	struct ledger_wait *wait;   /* the wait in the ledger */
	struct mcp_slot    *slot;   /* the slot of the call */
	uint64_t            end;    /* the loop's time when the wait ends */
	uint32_t            secs;   /* the seconds that the call asked for */
	uint32_t            n;      /* the ids in ids */
	char                ids[WAIT_IDS][LEDGER_ID];  /* the ids of the jobs
	                                                  that the call names,
	                                                  copied, because the
	                                                  call's message is freed
	                                                  as the call parks */
};

/* Writes the events in @p look, of the client's wait @p w, into the answer
 * of the wait's call: each job that ended and each timer that fired, or
 * else that nothing happened. Returns 0, or ENOMEM.
 */
static int
mcp_wait_report (struct mcp_wait const    *w,
                 struct ledger_look const *look)
{
	struct mcp_static const *st = w->slot->req->mcp->st;
	struct dstr *d = &w->slot->answer;
	bool ok = true;
	for (uint32_t i = 0; ok && i < look->n_tasks; ++i) {
		struct ledger_task const *t = look->tasks[i];
		enum ledger_status status = atomic_load_explicit(&t->status,
		                                                 memory_order_acquire);
		char const *kind = record_kinds[t->kind];
		char const *tool = st->offers[t->tool].tool.name.data;
		ok = status == LEDGER_CANCELLED
		   ? dstr_printf(d, "[askai: %s %.*s, %s, cancelled.]\n", kind, LEDGER_ID, t->id,
		                 tool)
		   : dstr_printf(d, "[askai: %s %.*s, %s, %s: call jobs with the action result "
		                 "and job %.*s for its result.]\n", kind, LEDGER_ID, t->id, tool,
		                 record_status[status], LEDGER_ID, t->id);
	}
	for (uint32_t i = 0; ok && i < look->n_timers; ++i)
		ok = mcp_notice_timer(d, &look->timers[i]);
	if (ok && !look->n_tasks && !look->n_timers)
		ok = dstr_printf(d, "Nothing happened in %" PRIu32 " s.", w->secs);
	return ok ? 0 : ENOMEM;
}

/* Checks the client's wait @p w. If ledger_tell() returns jobs that ended
 * or timers that fired, or the wait's time is up, the wait's call answers
 * with what happened. Else the wait goes on: its alarm is set for its end,
 * or for the time of the owner's first timer, if that is sooner. On the
 * first check, @p first, the call then parks.
 */
static void
mcp_wait_check (struct mcp_wait *w,
                bool             first)
{
	struct mcp_slot *slot = w->slot;
	struct mcp *mcp = slot->req->mcp;
	struct ledger_look look;
	int e = ledger_tell(mcp->st->ledger, w->wait, w->ids[0], w->n, &look);
	if (!e && !look.n_tasks && !look.n_timers && mcp->loop->now < w->end) {
		e = loop_timer_set(mcp->loop, &w->alarm, look.due < w->end ? look.due : w->end);
		if (!e && first)
			e = mcp_slot_park(slot);
		if (!e)
			return;
	}
	loop_timer_stop(mcp->loop, &w->alarm);
	if (!e)
		e = mcp_wait_report(w, &look);
	ledger_look_put(&look);
	if (e)
		mcp_slot_fail(slot, e);
	else
		mcp_slot_finish(slot, 0);
}

/* The alarm of a client's wait: its end, or a timer of its owner. */
static void
mcp_wait_alarm (struct loop       *loop,
                struct loop_timer *timer)
{
	(void)loop;
	struct mcp_wait *w = container_of(timer, struct mcp_wait, alarm);
	if (!w->slot->done)
		mcp_wait_check(w, false);
}

/* The message of a client's wait, @p msg, which the ledger posts when a
 * record of the wait's owner ends or the owner sets a timer. If the wait's
 * call still runs on @p loop, its step checks the wait again.
 */
static void
mcp_on_wake (struct loop *loop,
             struct msg  *msg)
{
	struct ledger_wait *w = container_of(msg, struct ledger_wait, wake);
	ledger_wait_heard(w);
	struct mcp_slot *slot = mcp_slot_named(loop, w->who);
	ledger_wait_put(w);
	if (slot && !slot->done)
		loop_ready_bulk(loop, &slot->step);
}

/* A client's call of wait in @p slot, the request's own, for @p secs
 * seconds: until a job of the call's owner ends, of those that @p args
 * names or any, or a timer of the owner fires. A job named that has ended,
 * an ended job that no one has read, or a timer that fired before the call,
 * ends the call at once.
 */
static void
mcp_slot_client_wait (struct mcp_slot    *slot,
                      struct json_object *args,
                      int64_t             secs)
{
	static_assert(WAIT_IDS == 16 && LEDGER_WAITS == 4, "the problems say");
	static char const bad_list[] = "The jobs argument must list at most 16 ids of "
	                               "jobs of yours.";
	struct mcp_req *req = slot->req;
	struct mcp *mcp = req->mcp;
	struct ledger *ledger = mcp->st->ledger;
	uint64_t owner = mcp_req_owner(req);
	struct json_object *list = jb_get(args, "jobs");
	size_t n = !list ? 0 : json_object_is_type(list, json_type_array)
	         ? json_object_array_length(list) : SIZE_MAX;
	if (n > WAIT_IDS) {
		mcp_slot_result(slot, TEXT(bad_list), true);
		return;
	}
	struct mcp_wait *w = malloc(sizeof *w);
	if (!w) {
		mcp_slot_fail(slot, ENOMEM);
		return;
	}
	char why[160];
	struct mcp_piece problem = {};
	for (size_t i = 0; !problem.bytes.data && i < n; ++i) {
		size_t len = 0;
		char const *id = jb_str(jb_at(list, i), &len);
		struct ledger_task *task = id && len == LEDGER_ID
		                         ? ledger_find(ledger, owner, id, len) : nullptr;
		if (task) {
			ledger_task_put(task);
			memcpy(w->ids[i], id, LEDGER_ID);
			continue;
		}
		problem = !id ? TEXT(bad_list)
		        : ESC(why, util_format(why, sizeof why, "No job of yours has the id %.*s; "
		                               "askai stores a job for an hour after it ends.",
		                               (int)util_clip(id, len, LEDGER_ID), id));
	}
	if (problem.bytes.data) {
		free(w);
		mcp_slot_result(slot, problem, true);
		return;
	}
	w->alarm = (struct loop_timer){ .fn = mcp_wait_alarm };
	w->slot = slot;
	w->end = mcp->loop->now + (uint64_t)secs * NS;
	w->secs = (uint32_t)secs;
	w->n = (uint32_t)n;
	struct handle who;
	int e = mcp_slot_name(slot, &who);
	w->wait = e ? nullptr : ledger_wait_enter(ledger, owner, mcp->loop, who, mcp_on_wake,
	                                          &e);
	if (!w->wait) {
		free(w);
		mcp_slot_refused(slot, e, TEXT("You have 4 waits under way, the most at once."));
		return;
	}
	slot->own = w;
	mcp_wait_check(w, true);
}

/* The step of a client's call of wait, which the message of its wait
 * queues.
 */
static void
mcp_slot_wait_step (struct mcp_slot *slot)
{
	if (slot->own && !slot->done)
		mcp_wait_check(slot->own, false);
}

/* Stops the alarm of the wait of a client's call of wait in @p slot,
 * removes the wait from the ledger and frees it.
 */
static void
mcp_slot_wait_drop (struct mcp_slot *slot)
{
	struct mcp_wait *w = slot->own;
	struct mcp *mcp = slot->req->mcp;
	loop_timer_stop(mcp->loop, &w->alarm);
	ledger_wait_leave(mcp->st->ledger, &w->wait);
	free(w);
}

/* The wait tool's call in @p slot: until a job of those @p args names, or
 * any, finishes, a timer fires, or its seconds are up. What happened and
 * no result said yet ends it at once. A client's call is
 * mcp_slot_client_wait()'s.
 */
static void
mcp_slot_wait (struct mcp_slot    *slot,
               struct json_object *args)
{
	static_assert(WAIT_MAX == 600 && JOBS_MAX <= 32, "the problem says");
	struct mcp_req *req = slot->req;
	int64_t secs = mcp_seconds(args, WAIT_MAX);
	if (!secs) {
		mcp_slot_result(slot, TEXT("The seconds argument must be a whole number from 1 "
		                           "to 600."), true);
		return;
	}
	if (slot->role == ROLE_OWN) {
		mcp_slot_client_wait(slot, args, secs);
		return;
	}
	struct json_object *list = jb_get(args, "jobs");
	uint32_t n_jobs = req->agenda ? req->agenda->n_jobs : 0;
	uint32_t mask = 0;
	bool listed = !list || json_object_is_type(list, json_type_array);
	struct json_object *item;
	for (size_t i = 0; listed && (item = jb_at(list, i)); ++i) {
		int64_t n = json_object_is_type(item, json_type_int)
		          ? json_object_get_int64(item) : 0;
		listed = n >= 1 && n <= n_jobs;
		mask |= listed ? 1u << (n - 1) : 0;
	}
	if (!listed) {
		mcp_slot_result(slot, TEXT("The jobs argument must list the numbers of jobs you "
		                           "started."), true);
		return;
	}
	int e;
	struct mcp_agenda *a = mcp_req_agenda(req, &e);
	if (!a) {
		mcp_req_fail(req, e);
		return;
	}
	a->wait = slot;
	a->wait_for = mask;
	a->wait_end = req->mcp->loop->now + (uint64_t)secs * NS;
	a->waited = (uint32_t)secs;
	/* A job waited for that has finished already ends it, too. */
	for (uint32_t k = 0; mask && k < n_jobs; ++k)
		if (mask >> k & 1 && a->jobs[k].done &&
		    !dstr_printf(&a->notices, "[askai: job %" PRIu32 " runs no more.]\n", k + 1)) {
			mcp_req_fail(req, ENOMEM);
			return;
		}
	if (dstr_len(&a->notices)) {
		mcp_agenda_wait_end(a);
		return;
	}
	e = mcp_agenda_arm(a);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	mcp_slot_park(slot);
}

/** Sets a timer of the turns of @p req that fires at @p due with the note
 *  @p note, and stores the number of the timer in @p *id. Returns 0,
 *  EBUSY while all LEDGER_TIMERS timers of the turns are set, or ENOMEM.
 */
static int
mcp_agenda_timer (struct mcp_req   *req,
                  uint64_t          due,
                  struct cstr_view  note,
                  uint32_t         *id)
{
	int e;
	struct mcp_agenda *a = mcp_req_agenda(req, &e);
	if (!a)
		return e;
	uint32_t const next = a->n_timers + 1;
	if (!ledger_timer_add(a->timers, due, next, note.data, note.len))
		return EBUSY;
	*id = a->n_timers = next;
	return mcp_agenda_arm(a);
}

/** Starts the call of the timer tool in @p slot, which sets a timer to fire
 *  after the seconds in @p args, with the note in @p args. For a call of a
 *  turn, the timer is a timer of the turns. For a client's call, the timer
 *  is a timer of the owner of the call in the ledger.
 */
static void
mcp_slot_timer (struct mcp_slot    *slot,
                struct json_object *args)
{
	static_assert(TIMER_MAX == 3600 && LEDGER_NOTE == 200 && LEDGER_TIMERS == 8,
	              "the problems say");
	struct mcp_req *req = slot->req;
	int64_t secs = mcp_seconds(args, TIMER_MAX);
	struct json_object *o = jb_get(args, "note");
	struct cstr_view note = jb_view(o);
	struct mcp_piece problem =
		!secs ? TEXT("The seconds argument must be a whole number from 1 to 3600.")
		: o && (!note.data || note.len > LEDGER_NOTE ||
		        jb_utf8(note.data, note.len) < note.len)
		? TEXT("The note argument must be a string of at most 200 bytes.")
		: (struct mcp_piece){};
	if (problem.bytes.data) {
		mcp_slot_result(slot, problem, true);
		return;
	}
	uint64_t due = req->mcp->loop->now + (uint64_t)secs * NS;
	uint32_t id;
	int e = slot->role == ROLE_OWN
	      ? ledger_timer_set(req->mcp->st->ledger, mcp_req_owner(req), due, note.data,
	                         note.len, &id)
	      : mcp_agenda_timer(req, due, note, &id);
	if (e) {
		mcp_slot_refused(slot, e, TEXT("You have 8 timers set, the most at once."));
		return;
	}
	char said[64];
	mcp_slot_result(slot, ESC(said, util_format(said, sizeof said, "Timer %" PRIu32
	                                            " will fire in %" PRId64 " s.", id,
	                                            secs)), false);
}

static void mcp_req_result_begin (struct mcp_req *req, struct mcp_slot *slot);

/* A turn's call of a tool that it may not call, or that askai does not
 * offer.
 */
static void
mcp_slot_none (struct mcp_slot    *slot,
               struct json_object *args)
{
	(void)args;
	mcp_slot_result(slot, TEXT("No tool of that name is yours to call."), true);
}

static void
mcp_req_intent_saved (struct mcp_req *req,
                      int             error)
{
	if (error) {
		mcp_req_error(req, 500, INTERNAL_ERROR, FRAG("Cannot persist job state"), nullptr, 0);
		return;
	}
	req->intents |= UINT32_C(1) << req->launched;
	mcp_req_enter(req, PHASE_CALLS);
}

/** Launches the next tool call of the turn of @p req in its slot. A call
 *  of a tool with KF_SERIAL and the call after it wait until the results
 *  before them are written. The call goes to the lent tool of its name, or
 *  to KIND_NONE. With a journal, the intent of the call becomes durable
 *  first, and a result that the journal recorded ends the call.
 */
static void
mcp_req_call_launch (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	uint32_t k = req->launched;
	if (k == req->n_calls)
		return;
	struct job_call h;
	struct ai_call c = job_call_parts(&h, buf_peek(&req->calls).ptr + req->launch_at);
	uint32_t known = mcp_static_tool_of(mcp->st, c.name);
	uint32_t id = mcp_req_lent_tool(req, known);
	struct registry_tool const *tool = &mcp->st->offers[id].tool;
	uint64_t const flags = tool->kind->flags;
	if (k > req->part &&
	    (flags | mcp_slot_offer(&req->slots[k - 1])->tool.kind->flags) & KF_SERIAL)
		return;
	if (mcp->draining) {
		mcp_req_fail(req, ESHUTDOWN);
		return;
	}
	/* mcp_req_args_free() frees the arguments after the result is written. */
	struct json_object *args = req->args[k];
	struct json_object *saved = req->saved_tools[k];
	if (*req->archive && !(flags & KF_RERUN) && !(req->intents >> k & 1) && !saved) {
		struct json_object *intent = jb_object(
			"index", json_object_new_int64(k),
			"agenda", json_object_new_boolean((flags & KF_AGENDA) != 0),
			"name", json_object_new_string_len(c.name.data, (int)c.name.len),
			"arguments", args ? json_object_get(args) : json_object_new_object(),
			nullptr);
		int e = mcp_req_archive(req, "tool-intent", intent, mcp_req_intent_saved);
		if (e) {
			mcp_req_fail(req, e);
			return;
		}
		loop_unready(&req->step);
		req->phase = PHASE_JOURNAL;
		return;
	}
	struct mcp_slot *slot = &req->slots[k];
	slot->tool = (uint8_t)id;
	req->launch_at += job_call_size(&h);
	req->launched = k + 1;
	hist_add(&mcp->counts->calls.lent[mcp_row_of(tool)->metric], 1);
	mcp_req_notify_tool(req, tool->name.data);
	loop_ready_bulk(mcp->loop, &req->step);
	if (saved) {
		size_t len = 0;
		char const *notebook = jb_str(jb_get(saved, "notebook"), &len);
		char repo[NOTES_REPO];
		if (notebook && notes_select(notebook, len, repo)) {
			mcp_req_fail(req, EILSEQ);
			return;
		}
		slot->shows = notebook;
		char const *text = jb_str(jb_get(saved, "text"), &len);
		struct json_object *source = jb_get(saved, "source");
		if (source && req->review) {
			int e = 0;
			slot->read = review_read_create_json(source, (struct cstr_view){ text, len }, &e);
			if (e) {
				mcp_req_fail(req, e == EINVAL ? EILSEQ : e);
				return;
			}
		}
		slot->start = loop_clock();
		slot->skip = jb_uint(jb_get(saved, "skip"), 0);
		mcp_slot_result(slot, ESC(text, len), json_object_get_boolean(jb_get(saved, "failed")));
		return;
	}
	if (id == TOOL_MAX)
		mcp_req_log_refused(req, c.name, known, args);
	if (!args) {
		/* mcp_slot_start() does not run for this call, so this line sets
		 * slot->start. mcp_slot_done() logs the time since slot->start. */
		slot->start = loop_clock();
		mcp_slot_result(slot, req->cut
		                ? TEXT("Your answer reached the output token limit, which "
		                       "cut these arguments short, so the call was not run.")
		                : TEXT("Your arguments were not a JSON object, so the call "
		                       "was not run."), true);
		return;
	}
	mcp_slot_start(slot, args);
}

/* Go on with the turn's tool calls: write the next one's result once it
 * has its output, in the calls' order, else launch another; once all are
 * written, submit the next turn, or, its context full, compact it first.
 */
static void
mcp_req_calls_next (struct mcp_req *req)
{
	if (req->part == req->n_calls) {
		mcp_req_slots_drop(req);
		buf_fini(&req->calls);
		buf_fini(&req->attached);
		if (mcp_req_full(req))
			mcp_req_compact_full(req);
		else
			mcp_req_turn_submit(req);
		return;
	}
	struct mcp_slot *slot = &req->slots[req->part];
	if (slot->done) {
		mcp_req_result_begin(req, slot);
		return;
	}
	req->phase = PHASE_CALLS;
	mcp_req_call_launch(req);
}

/* Take the window of the output @p out of @p slot's call that the turn
 * takes into slot->skip and slot->window: from slot->skip, or the output's
 * end, a character's start, up to mcp_req_lend_max() bytes, to a line's end
 * if one is near.
 */
static void
mcp_slot_window (struct mcp_slot *slot,
                 struct span      out)
{
	size_t skip = slot->skip < out.len ? slot->skip : out.len;
	while (skip < out.len && (out.ptr[skip] & 0xc0) == 0x80)
		++skip;
	size_t cap = mcp_req_lend_max(slot->req);
	size_t len = out.len - skip;
	size_t window = len;
	if (len > cap) {
		window = util_clip((char const *)out.ptr + skip, len, cap);
		size_t from = window - window / 4;
		unsigned char const *nl = memrchr(out.ptr + skip + from, '\n', window - from);
		if (nl)
			window = (size_t)(nl - out.ptr - skip) + 1;
	}
	slot->skip = skip;
	slot->window = window;
}

/* Say in slot->note where the window is in the output of @p len bytes of
 * @p slot's call, unless it is all of it, or that slot->skip was @p past
 * its end; returns the note's length.
 */
static size_t
mcp_slot_note_window (struct mcp_slot *slot,
                      size_t           len,
                      bool             past)
{
	size_t end = slot->skip + slot->window;
	if (end < len) {
		if (mcp_slot_offer(slot)->tool.kind->flags & TOOL_PAGED)
			return util_format(slot->note, sizeof slot->note, "\n\n[Bytes %zu to %zu "
			                   "of %zu: for the rest, call again with \"offset\": "
			                   "%zu.]", slot->skip, end, len, end);
		return util_format(slot->note, sizeof slot->note, "\n\n[Bytes %zu to %zu of "
		                   "%zu: the rest is cut.]", slot->skip, end, len);
	}
	if (past)
		return util_format(slot->note, sizeof slot->note, "[The output has %zu bytes, "
		                   "fewer than the offset.]", len);
	if (slot->skip)
		return util_format(slot->note, sizeof slot->note, "\n\n[Bytes %zu to %zu of "
		                   "%zu: its end.]", slot->skip, len, len);
	return 0;
}

static void mcp_agenda_job_done (struct mcp_slot *slot);

/**
 * Handles the end of the call in @p slot, which is a call of a turn or a
 * job. The call has produced its output or has failed. For a call of a
 * turn, the function records the result in the journal if req->archive is
 * set, sets slot->skip and slot->window to the part of the output that the
 * next upstream request contains, and writes the note about that part into
 * slot->note. Then it queues the step of the request. The step writes the
 * results of the calls into the upstream request in the order of the
 * calls. For a job, the function calls mcp_agenda_job_done(), which adds a
 * notice about the job to the agenda.
 *
 * With --verbose, the function logs one line with the ask call, the turn,
 * the name of the tool, whether the call failed, the time that the call
 * took, the size of the output in bytes, the arguments, as mcp_args_shown()
 * writes them, without the text of a note, and the text of a failure. The
 * line never contains the output of a call that succeeds, which can be the
 * text of the upstream's notes. A failed call is logged at the verbose
 * level, because a model that calls a tool with wrong arguments is not an
 * error of askai. A call of a tool that the turns may not call is logged
 * by mcp_req_log_refused() instead. A call that ends in another way, such
 * as a job that a jobs call cancels or a call that ends with its request,
 * ends in mcp_slot_drop(), which logs no line.
 */
static void
mcp_slot_done (struct mcp_slot *slot,
               bool             failed)
{
	/* The words of the log line for a call of a turn and for a job. This
	 * function is not called for ROLE_OWN. */
	static char const *const label[] = {
		[ROLE_TURN] = "tool call",
		[ROLE_JOB]  = "background tool call",
	};
	struct mcp_req *req = slot->req;
	struct mcp_offer const *offer = mcp_slot_offer(slot);
	struct span out = mcp_slot_output(slot);
	bool job = slot->role == ROLE_JOB;
	/* A job is not in req->slots, so its k is -1. */
	ptrdiff_t const k = job ? -1 : slot - req->slots;
	if (log_threshold >= LOG_VERBOSE && slot->tool != TOOL_MAX) {
		/* A turn's call whose arguments are not a JSON object ends at once
		 * in mcp_req_call_launch(), so req->cut is still the value of its
		 * turn. */
		char shown[SHOWN];
		mcp_args_shown(offer, job ? slot->args : req->args[k],
		               job ? CSTR_VIEW("none") : mcp_req_no_args(req), shown);
		char text[ERROR_LOG + 8] = "";
		if (failed && out.len)
			mcp_quote(text, sizeof text,
			          (struct cstr_view){ (char const *)out.ptr, out.len }, ERROR_LOG);
		LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " turn %" PRIu32 ": %s %s %s "
		    "after %.1f s (%zu %s); arguments %s%s%s", req->mcp->loop->id, req->asked,
		    req->turn, label[slot->role], offer->tool.name.data,
		    failed ? "failed" : "answered", mcp_elapsed(slot->start), out.len,
		    mcp_bytes(out.len), shown, *text ? "; error " : "", text);
	}
	slot->failed = failed;
	char const *notebook = !failed ? slot->shows : nullptr;
	slot->shows = nullptr;
	if (job) {
		mcp_agenda_job_done(slot);
		return;
	}
	if (*req->archive && !(offer->tool.kind->flags & KF_RERUN) && !req->saved_tools[k]) {
		struct span files = buf_peek(&req->attached);
		struct json_object *result = jb_object(
			"index", json_object_new_int64(k),
			"failed", json_object_new_boolean(failed),
			"skip", json_object_new_uint64(slot->skip),
			"branch", json_object_new_boolean(req->branch),
			"filing", json_object_new_boolean(req->filing),
			"filed", json_object_new_uint64(req->filed),
			"attached", json_object_new_string_len(files.len ? (char const *)files.ptr : "", (int)files.len),
			"text", json_object_new_string_len(out.len ? (char const *)out.ptr : "", (int)out.len),
			nullptr);
		if (notebook)
			result = jb_set_or_free(result, "notebook", json_object_new_string(notebook));
		if (slot->read)
			result = jb_set_or_free(result, "source", review_read_json(slot->read, &(int){}));
		int e = mcp_req_archive(req, "tool-result", result, nullptr);
		if (e)
			req->archive_error = e;
	}
	bool past = slot->skip > out.len;
	mcp_slot_window(slot, out);
	slot->note_len = (uint16_t)mcp_slot_note_window(slot, out.len, past);
	if (notebook)
		slot->note_len += (uint16_t)util_format(slot->note + slot->note_len,
		                                        sizeof slot->note - slot->note_len,
		                                        "\n\n" NOTICE_NOTEBOOK, notebook);
	loop_ready_bulk(req->mcp->loop, &req->step);
}

/* Writes a notice about the context of @p req into the @p size bytes at
 * @p buf, and returns its length. @p tokens is the number of tokens in the
 * context after the turn's results. From two thirds of the context on, the
 * notice asks the turns to call compact, after each turn, if they have it.
 * Below two thirds,
 * if the turns have the notes tool, the notice reminds them to write notes,
 * once when the context is a quarter full and once when it is half full.
 * Otherwise there is no notice. With --verbose, a notice is logged.
 */
static size_t
mcp_req_context_note (struct mcp_req *req,
                      char           *buf,
                      size_t          size,
                      uint64_t        tokens)
{
	uint64_t context = req->context;
	if (!context)
		return 0;
	struct mcp_lending const *l = &req->mcp->st->lending;
	bool full = tokens * 3 >= context * 2;
	uint8_t quarters = tokens * 2 >= context ? 2 : tokens * 4 >= context;
	if (full ? !(req->lent & l->of[KIND_COMPACT])
	         : quarters <= req->reminded || !(req->lent & l->of[KIND_NOTES]))
		return 0;
	LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " turn %" PRIu32 ": context "
	    "notice, %" PRIu64 " %% full, %" PRIu64 " of %" PRIu64 " tokens: %s",
	    req->mcp->loop->id, req->asked, req->turn, tokens * 100 / context, tokens,
	    context, full ? "a request to call compact" : "a reminder to write notes");
	if (!full)
		req->reminded = quarters;
	return util_format(buf, size, full ? "\n\n" NOTICE_COMPACT : "\n\n" NOTICE_REMIND,
	                   tokens * 100 / context, tokens, context);
}

/* Write the result of the turn's call in @p slot, the next in order. After
 * the turn's last result, askai adds the notice about the context, if any,
 * and a notice that the next turn is the last one, if it is.
 */
static void
mcp_req_result_begin (struct mcp_req  *req,
                      struct mcp_slot *slot)
{
	struct mcp_agenda *a = req->agenda;
	if (a && dstr_len(&a->notices) && !dstr_len(&a->told)) {
		if (!dstr_cat_n(&a->told, dstr_get(&a->notices), dstr_len(&a->notices))) {
			mcp_req_fail(req, ENOMEM);
			return;
		}
		dstr_clear(&a->notices);
	}
	size_t n = slot->note_len;
	if (req->part + 1u == req->n_calls) {
		size_t size = sizeof slot->note;
		uint64_t tokens = mcp_req_tokens(req) + (slot->window + n) / 4;
		n += mcp_req_context_note(req, slot->note + n, size - n, tokens);
		if (req->turn + 1u == req->mcp->st->turns)
			n += util_format(slot->note + n, size - n, "\n\n" NOTICE_LAST);
	}
	slot->note_len = (uint16_t)n;
	req->next = 0;
	req->off = 0;
	mcp_req_enter(req, PHASE_RESULT);
}

/** Writes the result of the call @p c, the call under way, into the upload
 *  of @p req, and after the last result of the turn the attached files
 *  and the end of the messages. Returns 0 or an error number.
 */
static int
mcp_req_result_put (struct mcp_req       *req,
                    struct ai_call const *c)
{
	struct ai_format const *f = ai_format(req->mcp->st->ep->api);
	struct mcp_slot const *slot = &req->slots[req->part];
	struct span out = mcp_slot_output(slot);
	struct span attached = buf_peek(&req->attached);
	struct cstr_view told = req->agenda ? cstr_view_dstr(&req->agenda->told)
	                                    : (struct cstr_view){};
	char made[CALL_ID];
	struct mcp_piece id;
	int e = mcp_req_call_id(req, c, req->part, made, &id);
	if (e)
		return e;
	/* With AI_RESULT_NAMED, the key of a result is the name of the tool,
	 * and the id is appended if the call came with an id or askai made one
	 * up. */
	struct mcp_piece key = id;
	struct mcp_piece by = {};
	struct mcp_piece also = {};
	if (f->flags & AI_RESULT_NAMED) {
		key = ESC_VIEW(c->name);
		if (c->id.len || f->call.len) {
			by = BYTES(f->result_id);
			also = id;
		}
	}
	struct mcp_piece const p[] = {
		BYTES(req->part ? f->result_sep : f->results),
		BYTES(f->result),
		key,
		by,
		also,
		BYTES(f->output),
		ESC(out.len ? out.ptr + slot->skip : nullptr, slot->window),
		ESC(slot->note, slot->note_len),
		ESC("\n\n", told.len ? 2 : 0),
		ESC_VIEW(told),
		BYTES(f->result_end),
		RAW(attached.ptr, attached.len),
		BYTES(f->results_end),
		BYTES(f->end),
	};
	bool last = req->part + 1u == req->n_calls;
	return mcp_req_upload(req, p, last ? ARRAY_SIZE(p) : ARRAY_SIZE(p) - 3);
}

/* Pass review_pending_add() the lines of a committed file that the result in
 * @p slot shows whole. The lines of a jobs call are those of its job
 * (slot->of). Returns 0 if the result shows no such lines, or the error
 * of review_pending_add().
 */
static int
mcp_req_shown (struct mcp_req        *req,
               struct mcp_slot const *slot)
{
	struct review_read *read = (slot->of ? slot->of : slot)->read;
	if (!read)
		return 0;
	/* review_pending_add() scans the output from the numbered lines to the end
	 * of the window. */
	loop_work(req->mcp->loop, LOOP_COPY, slot->skip + slot->window);
	struct span out = mcp_slot_output(slot);
	struct cstr_view text = { (char const *)out.ptr, out.len };
	return review_pending_add(req->review, read, text, slot->skip, slot->window, req->turn);
}

/* Write the result of the turn's call req->part into the upload, one slice
 * in each step. Then write its line of the trace, pass the lines that it
 * shows to mcp_req_shown(), drop the call's output and go on with the next
 * call.
 */
static void
mcp_req_result_write (struct mcp_req *req)
{
	struct job_call h;
	struct ai_call c = job_call_parts(&h, buf_peek(&req->calls).ptr + req->at);
	int e = mcp_req_result_put(req, &c);
	if (e == EAGAIN) {
		loop_ready_bulk(req->mcp->loop, &req->step);
		return;
	}
	if (e) {
		mcp_req_upload_failed(req, e);
		return;
	}
	struct mcp_slot *slot = &req->slots[req->part];
	(void)mcp_req_trace(req, &c, req->args[req->part],
	                    mcp_row_of(&mcp_slot_offer(slot)->tool)->trace);
	(void)mcp_req_trace_end(req, slot->failed, mcp_slot_output(slot).len);
	e = mcp_req_shown(req, slot);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	mcp_slot_drop(slot);
	slot->of = nullptr;
	dstr_clear(&slot->answer);
	slot->note_len = 0;
	slot->skip = slot->window = 0;
	if (req->agenda)
		dstr_clear(&req->agenda->told);
	req->at += job_call_size(&h);
	mcp_req_args_free(req, req->part, req->part + 1u);
	++req->part;
	mcp_req_calls_next(req);
}

static void
mcp_req_archive_submitted (struct mcp_req *req,
                           int             error)
{
	if (!error && req->mcp->draining)
		error = ESHUTDOWN;
	if (error) {
		mcp_req_error(req, 500, INTERNAL_ERROR, FRAG("Cannot persist job state"), nullptr, 0);
		return;
	}
	if (req->detach_pending) {
		req->detach_pending = false;
		if (mcp_req_background(req, req->self.tool, true))
			return;
	}
	int e = mcp_req_submit(req);
	if (e)
		mcp_req_fail(req, e);
}

/* The endpoint fingerprint never archives URL credentials or headers. */
static uint64_t
mcp_archive_endpoint (struct mcp_static const *st)
{
	struct ai_endpoint const *ep = st->ep;
	uint64_t hash = util_hash(st->seed, ep->url, strlen(ep->url));
	return ep->model ? util_hash(hash, ep->model, strlen(ep->model)) : hash;
}

static int
mcp_req_checkpoint (struct mcp_req *req)
{
	struct mcp_static const *st = req->mcp->st;
	int e = req->archive_error;
	if (!e && req->detach_pending && !req->admitted)
		req->admitted = mcp_req_record(req, LEDGER_JOB, req->self.tool, &e);
	struct ledger_task const *task = req->task ? req->task : req->admitted;
	if (!e && !*req->archive) {
		if (task)
			memcpy(req->archive, task->id, LEDGER_ID);
		else
			e = notes_id(req->archive);
	}
	if (e)
		return e;
	struct json_object *lent = json_object_new_array();
	for (uint32_t i = 0; lent && i < st->n_offers; ++i)
		if (req->lent >> i & 1)
			lent = jb_push_or_free(lent,
			                       json_object_new_string(st->offers[i].tool.name.data));
	struct span upload = buf_peek(&req->upload);
	uint64_t now = clk_realtime_ns();
	req->archive_deadline = st->ep->timeout_ms > 0
	                      ? now + (uint64_t)st->ep->timeout_ms * 1'000'000 : 0;
	struct json_object *state = jb_object(
		"version", json_object_new_int(1),
		"owner", json_object_new_uint64(mcp_req_owner(req)),
		"created", json_object_new_uint64(task ? task->created : now),
		"tool", json_object_new_string(st->offers[req->self.tool].tool.name.data),
		"api", json_object_new_int(st->ep->api),
		"provider", json_object_new_int(st->ep->provider),
		"endpoint", json_object_new_uint64(mcp_archive_endpoint(st)),
		"request", json_object_new_string_len((char const *)upload.ptr, (int)upload.len),
		"turn", json_object_new_int64(req->turn + 1),
		"deadline", json_object_new_uint64(req->archive_deadline),
		"prompt_at", json_object_new_uint64(req->prompt_at),
		"used", json_object_new_int64(req->used),
		"context", json_object_new_int64(req->context),
		"traced", json_object_new_int64(req->traced),
		"trace", json_object_new_string_len(dstr_get(&req->trace), (int)dstr_len(&req->trace)),
		"thread", json_object_new_string(req->thread),
		"branch", json_object_new_boolean(req->branch),
		"filing", json_object_new_boolean(req->filing),
		"filed", json_object_new_uint64(req->filed),
		"reminded", json_object_new_int(req->reminded),
		"forced", json_object_new_int(req->forced),
		"asks", json_object_new_int(req->asks),
		"agenda", json_object_new_boolean(req->agenda != nullptr),
		"lent", lent, nullptr);
	/* Only a task's checkpoint can be resumed, so only it keeps the review
	 * record. */
	if (task && req->review)
		state = jb_set_or_free(state, "review", review_json(req->review, &(int){}));
	req->intents = 0;
	for (size_t k = 0; k < JOB_CALLS; ++k) {
		json_object_put(req->saved_tools[k]);
		req->saved_tools[k] = nullptr;
	}
	e = mcp_req_archive(req, "state", state, mcp_req_archive_submitted);
	if (!e) {
		loop_unready(&req->step);
		req->phase = PHASE_JOURNAL;
	}
	return e;
}

struct mcp_recover_wait {
	struct handle who;
	struct json_object *args;
	typeof(mcp_slot_client_result) *handler;
};

/** Returns the tools in the array of names @p names that mcp_req_lent()
 *  returns for the client of @p req now. The checkpoint of a job stores
 *  names, because the ids of the tools change with the configuration.
 */
static tool_mask
mcp_archive_lent (struct mcp_req     *req,
                  struct json_object *names)
{
	tool_mask saved = 0;
	for (size_t k = 0, n = jb_len(names); k < n; ++k) {
		uint32_t id = mcp_static_tool_of(req->mcp->st, jb_view(jb_at(names, k)));
		saved |= id < TOOL_MAX ? (tool_mask)1 << id : 0;
	}
	return mcp_req_lent(req) & saved;
}

static struct json_object *
mcp_archive_json (struct journal_record const *record)
{
	if (!record)
		return nullptr;
	struct json_object *obj = json_tokener_parse((char const *)record->data);
	if (!json_object_is_type(obj, json_type_object)) {
		json_object_put(obj);
		return nullptr;
	}
	return obj;
}

static void
mcp_recover_back (struct loop           *loop,
                  void                  *arg,
                  int                    error,
                  struct journal_record *records)
{
	struct mcp_recover_wait *wait = arg;
	struct mcp_req *req = mcp_req_archived(loop, wait->who);
	struct json_object *args = wait->args;
	typeof(mcp_slot_client_result) *handler = wait->handler;
	free(wait);
	wait = nullptr;
	if (!req)
		goto out;
	struct mcp_static const *st = req->mcp->st;
	uint64_t const owner = mcp_req_owner(req);
	size_t id_len = 0;
	char const *id = jb_str(jb_get(args, "job"), &id_len);
	struct ledger_task *found = ledger_find(st->ledger, owner, id, id_len);
	if (found) {
		ledger_task_put(found);
		handler(&req->self, args, true);
		goto out;
	}
	struct journal_record *checkpoint = nullptr, *end = nullptr;
	for (struct journal_record *r = records; r; r = r->next) {
		if (!strcmp(r->kind, "state")) {
			checkpoint = r;
			end = nullptr;
		} else if (!strcmp(r->kind, "result") || !strcmp(r->kind, "cancel")) {
			end = r;
		}
	}
	struct json_object *state = mcp_archive_json(checkpoint);
	struct cstr_view tool = jb_view(jb_get(state, "tool"));
	struct mcp_piece problem =
		error ? TEXT("Cannot read the durable job journal.")
		: !state || jb_int(jb_get(state, "version"), 0) != 1
		? TEXT("No recoverable job of yours has that id.")
		: jb_uint(jb_get(state, "owner"), 0) != owner ||
		  !cstr_view_equal(tool, st->offers[req->self.tool].tool.name)
		? TEXT("No job of yours from this tool has that id.")
		: (struct mcp_piece){};
	if (problem.bytes.data)
		goto failed;
	uint64_t created = jb_uint(jb_get(state, "created"), 0);
	if (end) {
		struct json_object *result = mcp_archive_json(end);
		size_t len = 0;
		char const *text = jb_str(jb_get(result, "result"), &len);
		int64_t status = jb_int(jb_get(result, "status"), 0);
		struct ledger_task *task = nullptr;
		int e = EINVAL;
		if (text && status >= LEDGER_COMPLETED && status <= LEDGER_CANCELLED)
			task = ledger_restore(st->ledger, owner, LEDGER_JOB, req->self.tool,
			                       (struct span){ (unsigned char *)(uintptr_t)id, id_len },
			                       created, jb_uint(jb_get(result, "ended"), 0),
			                       (enum ledger_status)status,
			                       (struct span){ (unsigned char *)(uintptr_t)text, len },
			                       nullptr, (struct handle){}, nullptr, &e);
		json_object_put(result);
		if (!task && e != EEXIST) {
			problem = TEXT("Cannot restore this job's saved result.");
			goto failed;
		}
		ledger_task_put(task);
		json_object_put(state);
		handler(&req->self, args, true);
		goto out;
	}
	if (jb_int(jb_get(state, "api"), 0) != st->ep->api ||
	    jb_int(jb_get(state, "provider"), 0) != st->ep->provider ||
	    jb_uint(jb_get(state, "endpoint"), 0) != mcp_archive_endpoint(st)) {
		problem = TEXT("This job belongs to a different upstream endpoint, model or API.");
		goto failed;
	}
	if (json_object_get_boolean(jb_get(state, "agenda"))) {
		problem = TEXT("This interrupted job had background tools or timers. Their "
		               "outcomes must be reconciled before continuing.");
		goto failed;
	}
	uint32_t intents = 0, results = 0;
	struct json_object *last_tool_result = nullptr;
	for (struct journal_record *r = checkpoint->next; r; r = r->next) {
		bool intent = !strcmp(r->kind, "tool-intent");
		if (!intent && strcmp(r->kind, "tool-result"))
			continue;
		struct json_object *obj = mcp_archive_json(r);
		int64_t k = jb_int(jb_get(obj, "index"), 0);
		problem = !obj || k < 0 || k >= JOB_CALLS ||
		          (!intent && !jb_text(jb_get(obj, "text")))
		        ? TEXT("This job's tool journal is invalid; no calls were repeated.")
		        : intent && json_object_get_boolean(jb_get(obj, "agenda"))
		        ? TEXT("This interrupted turn started or used background tools or "
		               "timers. Reconcile their outcomes before continuing.")
		        : (struct mcp_piece){};
		if (problem.bytes.data) {
			json_object_put(obj);
			goto failed;
		}
		if (intent) {
			intents |= UINT32_C(1) << k;
			json_object_put(obj);
			continue;
		}
		results |= UINT32_C(1) << k;
		json_object_put(req->saved_tools[k]);
		req->saved_tools[k] = obj;
		last_tool_result = obj;
	}
	if (intents & ~results) {
		problem = TEXT("A tool call was interrupted with an unknown outcome; reconcile its "
		               "effects before continuing. AskAI has not repeated it.");
		goto failed;
	}
	/* Before the job is admitted again, a failure here leaves its
	 * checkpoint as it was, with no end record. */
	struct json_object *local = last_tool_result ? last_tool_result : state;
	struct cstr_view attached = jb_view(jb_get(local, "attached"));
	if (attached.len &&
	    mcp_piece_write(&BYTES(attached), 0, attached.len, &req->attached, SIZE_MAX)) {
		problem = TEXT("Cannot restore this turn's saved attachments.");
		goto failed;
	}
	struct cstr_view request = jb_view(jb_get(state, "request"));
	int64_t turn = jb_int(jb_get(state, "turn"), 0);
	uint64_t prompt_at = jb_uint(jb_get(state, "prompt_at"), 0);
	struct json_object *names = jb_get(state, "lent");
	if (!request.len || turn < 1 || turn > UINT32_MAX || prompt_at > request.len ||
	    !json_object_is_type(names, json_type_array)) {
		problem = TEXT("This job's saved request is invalid; no provider request was sent.");
		goto failed;
	}
	/* req->lent is set only after the job is admitted (below), so a poll
	 * that fails here answers with the problem alone, without the turns'
	 * trace (mcp_req_trace_put()). */
	struct json_object const *review = jb_get(state, "review");
	tool_mask lent = mcp_archive_lent(req, names);
	int e = mcp_req_review_make(req, lent);
	if (!e && req->review && review)
		e = review_restore(req->review, review);
	if (e) {
		problem = e == ENOMEM ? TEXT("Cannot restore this job's saved review record now.")
		        : TEXT("This job's saved review record is invalid; no provider request "
		               "was sent.");
		goto failed;
	}
	e = mcp_piece_write(&BYTES(request), 0, request.len, &req->upload, SIZE_MAX);
	struct cstr_view trace = jb_view(jb_get(state, "trace"));
	if (!e && trace.len && !dstr_cat_n(&req->trace, trace.data, trace.len))
		e = ENOMEM;
	struct handle who = {};
	if (!e)
		e = loop_handle(loop, req, HANDLE_REQ, &who);
	struct ledger_task *task = !e
		? ledger_restore(st->ledger, owner, LEDGER_JOB, req->self.tool,
		                 (struct span){ (unsigned char *)(uintptr_t)id, id_len },
		                 created, 0, LEDGER_WORKING, (struct span){}, loop, who,
		                 mcp_on_task_cancel, &e) : nullptr;
	if (!task) {
		if (who.bits)
			loop_forget(loop, who);
		problem = TEXT("Cannot resume this job now; it may already be resuming on another "
		               "request.");
		goto failed;
	}
	req->admitted = task;
	memcpy(req->archive, id, LEDGER_ID);
	req->turn = (uint32_t)turn - 1;
	req->prompt_at = prompt_at;
	req->archive_deadline = jb_uint(jb_get(state, "deadline"), 0);
	req->lent = lent;
	req->asks = (uint8_t)jb_int(jb_get(state, "asks"), 0);
	req->used = (uint32_t)jb_int(jb_get(state, "used"), 0);
	req->context = (uint32_t)jb_int(jb_get(state, "context"), 0);
	req->traced = (uint32_t)jb_int(jb_get(state, "traced"), 0);
	req->reminded = (uint8_t)jb_int(jb_get(state, "reminded"), 0);
	req->forced = (uint8_t)jb_int(jb_get(state, "forced"), 0);
	req->branch = json_object_get_boolean(jb_get(local, "branch"));
	req->filing = json_object_get_boolean(jb_get(local, "filing"));
	req->filed = jb_uint(jb_get(local, "filed"), 0);
	req->replay_filed = last_tool_result ? req->filed : 0;
	struct cstr_view thread = jb_view(jb_get(state, "thread"));
	if (thread.data && notes_is_id(thread.data, thread.len))
		memcpy(req->thread, thread.data, NOTES_ID);
	if (st->notes_tool.notes)
		notes_repo(st->notes_tool.notes, req->principal, NOTES_UPSTREAM, req->repo);
	req->intents = intents;
	req->recovering = true;
	req->asked = ++req->mcp->asked;
	req->recovered = checkpoint->next;
	checkpoint->next = nullptr;
	json_object_put(state);
	/* As an ask does: the turns parse their answers into req->msg. */
	mcp_req_dispose(req);
	mcp_req_wait(req);
	if (mcp_req_background(req, req->self.tool, true))
		goto out;
	if (handler == mcp_slot_client_cancel)
		ledger_task_cancel(req->task);
	e = mcp_req_submit(req);
	if (e)
		mcp_req_fail(req, e);
	goto out;
failed:
	json_object_put(state);
	mcp_slot_result(&req->self, problem, true);
out:
	json_object_put(args);
	journal_records_destroy(&records);
}

/* Return true once this poll is waiting for disk, or has answered an
 * admission error. Simultaneous polls converge at ledger_restore().
 * @p handler is mcp_slot_client_result() or mcp_slot_client_cancel().
 * mcp_recover_back() calls it for a job that is in the ledger or has ended,
 * and cancels a resumed job if @p handler is mcp_slot_client_cancel().
 */
static bool
mcp_req_recover (struct mcp_req                 *req,
                 struct json_object             *args,
                 typeof(mcp_slot_client_result) *handler)
{
	if (!req->mcp->st->journal)
		return false;
	size_t len = 0;
	char const *id = jb_str(jb_get(args, "job"), &len);
	if (!id || !notes_is_id(id, len))
		return false;
	struct ledger_task *task = ledger_find(req->mcp->st->ledger, mcp_req_owner(req), id, len);
	if (task) {
		ledger_task_put(task);
		return false;
	}
	struct mcp_recover_wait *wait = calloc(1, sizeof *wait);
	int e = wait ? mcp_req_archive_wait(req, &wait->who) : ENOMEM;
	if (!e) {
		wait->args = json_object_get(args);
		wait->handler = handler;
		e = journal_load_current(req->mcp->st->journal, id, req->mcp->loop,
		                         mcp_recover_back, wait);
	}
	if (e) {
		if (wait) {
			if (wait->who.bits)
				loop_forget(req->mcp->loop, wait->who);
			json_object_put(wait->args);
			free(wait);
			wait = nullptr;
		}
		mcp_slot_result(&req->self, TEXT("Cannot load the durable job journal now."), true);
		return true;
	}
	req->archive_call = wait->who;
	mcp_req_wait(req);
	req->s->ops->waiting(req->s);
	req->phase = PHASE_JOURNAL;
	return true;
}

/* Submit the next turn only after its request and ownership are durable. */
static void
mcp_req_turn_submit (struct mcp_req *req)
{
	if (req->mcp->draining) {
		mcp_req_fail(req, ESHUTDOWN);
		return;
	}
	mcp_req_wait(req);
	int e = req->mcp->st->journal ? mcp_req_checkpoint(req) : mcp_req_submit(req);
	if (e)
		mcp_req_error(req, 500, INTERNAL_ERROR,
		              FRAG("Cannot persist or start the upstream request"), nullptr, 0);
}

/* The first turn's upload is written: submit it, after what askai adds to
 * the prompt, if anything.
 */
static void
mcp_req_turn_ready (struct mcp_req *req)
{
	if (dstr_len(&req->self.answer))
		mcp_req_add_start(req);
	else
		mcp_req_turn_submit(req);
}

/* Write what askai adds to the prompt, in the request's answer, at the end
 * of the prompt's text: the conversation after it goes.
 */
static void
mcp_req_add_start (struct mcp_req *req)
{
	struct span up = buf_peek(&req->upload);
	buf_cut(&req->upload, req->prompt_at, up.len - req->prompt_at);
	buf_fini(&req->attached);
	req->filed = 0;
	req->filing = false;
	req->next = 0;
	req->off = 0;
	mcp_req_enter(req, PHASE_ADD);
}

/** Writes the text that askai adds to the prompt, from the answer of
 *  @p req, or in PHASE_WAKE the message that wakes the turns, from the told
 *  member of the agenda, into the upload, a slice in each step. Then it
 *  ends the prompt or the message and the messages and submits the turn.
 */
static void
mcp_req_add_write (struct mcp_req *req)
{
	struct ai_format const *f = ai_format(req->mcp->st->ep->api);
	bool wake = req->phase == PHASE_WAKE;
	struct dstr *text = wake ? &req->agenda->told : &req->self.answer;
	struct mcp_piece const p[] = {
		wake ? BYTES(f->user) : (struct mcp_piece){},
		ESC_VIEW(cstr_view_dstr(text)),
		BYTES(f->prompt_end),
		BYTES(f->end),
	};
	int e = mcp_req_upload(req, p, ARRAY_SIZE(p));
	if (e == EAGAIN) {
		loop_ready_bulk(req->mcp->loop, &req->step);
		return;
	}
	if (e == ENOBUFS && !wake) {
		mcp_req_error(req, 500, INTERNAL_ERROR,
		              FRAG("The task and its notes do not fit the upstream request"),
		              nullptr, 0);
		return;
	}
	if (e) {
		mcp_req_upload_failed(req, e);
		return;
	}
	dstr_clear(text);
	mcp_req_turn_submit(req);
}

/** Appends @p text and "[askai: go on with the task.]" to the text that
 *  askai adds to the prompt of @p req, and writes that text into the
 *  upload.
 */
static void
mcp_req_go_on (struct mcp_req   *req,
               struct cstr_view  text)
{
	struct dstr *d = &req->self.answer;
	if (!dstr_cat_n(d, text.data, text.len) ||
	    !CAT(d, "[askai: go on with the task.]")) {
		mcp_req_fail(req, ENOMEM);
		return;
	}
	mcp_req_add_start(req);
}

/** Goes on with the ask call in @p slot, req->self, after a compaction put
 *  the notes into the prompt.
 */
static void
mcp_slot_compacted (struct mcp_slot *slot)
{
	mcp_req_go_on(slot->req, CSTR_VIEW("\n"));
}

/** Adds the file of @p bytes bytes with the URI @p uri to req->attached, in
 *  the form of the API of the upstream (ai_format()). The URI is printable
 *  ASCII (ai_uploaded()). Returns 0 or ENOMEM.
 */
static int
mcp_req_attach (struct mcp_req   *req,
                struct cstr_view  uri,
                size_t            bytes)
{
	struct ai_format const *f = ai_format(req->mcp->st->ep->api);
	/* A turn makes at most JOB_CALLS calls, so the files add a few KiB. */
	struct span to;
	int e = buf_reserve(&req->attached, f->file.len + 6 * uri.len + f->file_end.len,
	                    SIZE_MAX, &to);
	if (e)
		return e;
	char *p = (char *)to.ptr;
	memcpy(p, f->file.data, f->file.len);
	size_t n = f->file.len + jb_escape(p + f->file.len, uri.data, uri.len);
	memcpy(p + n, f->file_end.data, f->file_end.len);
	buf_commit(&req->attached, n + f->file_end.len);
	req->filed += bytes;
	req->filing = true;
	return 0;
}

/** Sends @p body to the file store of the upstream as the HTTP request of
 *  the call in @p slot. Returns 0, or the error of mcp_slot_send(), such
 *  as EAGAIN if askai sends too many requests at once.
 */
static int
mcp_slot_post_file (struct mcp_slot  *slot,
                    struct cstr_view  body)
{
	static char const accept[] = "application/json";
	struct mcp_static const *st = slot->req->mcp->st;
	struct web_request const request = {
		.url = st->files_url,
		.body = body,
		.accept = accept,
		.max = 64 << 10,
		.timeout = WEB_TIMEOUT_MAX
	};
	struct web_ask ask = {
		.request = &request,
		.url = st->files_url_len,
		.accept = sizeof accept - 1,
		.lines = st->files_lines
	};
	return mcp_slot_send(slot, &ask);
}

/* Returns EFBIG and appends why to @p out if the context of @p req has no
 * room for a file of @p bytes bytes, about a quarter as many tokens. Returns
 * 0 if the context has room or its size is not known, and ENOMEM if the
 * words do not fit in memory.
 */
static int
mcp_req_fits (struct mcp_req const *req,
              size_t                bytes,
              struct dstr          *out)
{
	size_t used = mcp_req_tokens(req);
	if (!req->context || used + bytes / 4 < req->context)
		return 0;
	return dstr_printf(out, "The note is too large to attach: its file of %zu bytes, "
	                   "about %zu tokens, does not fit the context of %" PRIu32
	                   " tokens, of which about %zu are used. read shows it in "
	                   "parts.", bytes, bytes / 4, req->context, used) ? EFBIG : ENOMEM;
}

int
mcp_slot_attach (struct mcp_slot  *slot,
                 struct cstr_view  id,
                 struct cstr_view  file,
                 size_t           *bytes,
                 struct dstr      *out)
{
	if (!slot || !bytes || !out || (!id.data && id.len) || (!file.data && file.len))
		return EFAULT;
	*bytes = 0;
	if (slot->done)
		return EINVAL;
	struct mcp_req *req = slot->req;
	struct mcp_static const *st = req->mcp->st;
	if (!st->uploads)
		return ENOTSUP;
	if (file.data) {
		int e = mcp_req_fits(req, file.len, out);
		e = e ? e : mcp_slot_post_file(slot, file);
		if (e == EAGAIN)
			return CAT(out, "askai sends too many requests at once to upload the "
			           "note now. Try again later.") ? EAGAIN : ENOMEM;
		return e ? e : EINPROGRESS;
	}
	char uri[UPLOADS_URI];
	size_t file_bytes = 0;
	size_t uri_len = uploads_find(st->uploads, mcp_req_owner(req), id, clk_realtime().tv_sec,
	                              uri, &file_bytes);
	int e = uri_len ? mcp_req_fits(req, file_bytes, out) : ENOENT;
	if (e)
		return e;
	LOG(LOG_VERBOSE, "upstream file %.*s attached again", (int)uri_len, uri);
	*bytes = file_bytes;
	return mcp_req_attach(req, (struct cstr_view){ uri, uri_len }, file_bytes);
}

int
mcp_slot_attached (struct mcp_slot  *slot,
                   struct cstr_view  id,
                   size_t            bytes,
                   struct dstr      *out)
{
	if (!slot || !out || (!id.data && id.len))
		return EFAULT;
	if (slot->done)
		return EINVAL;
	struct mcp_req *req = slot->req;
	struct mcp_static const *st = req->mcp->st;
	struct web_response r;
	int e = mcp_slot_fetched(slot, &r);
	if (e)
		return e;
	dstr_auto uri = DSTR_INIT;
	dstr_auto why = DSTR_INIT;
	int64_t expires = 0;
	e = r.e ? (dstr_cat(&why, r.error) ? EIO : ENOMEM)
	    : ai_uploaded(st->ep->api, r.status, r.body.data, r.body.len, &uri, &expires, &why);
	struct cstr_view const u = cstr_view_dstr(&uri);
	if (!e && u.len > UPLOADS_URI)
		e = dstr_printf(&why, "The upstream named the file with a URI of more "
		                "than %d bytes.", UPLOADS_URI) ? EIO : ENOMEM;
	if (e == EIO)
		return CAT(out, "The upload of the note as a file failed: ") &&
		       dstr_cat_n(out, dstr_get(&why), dstr_len(&why)) ? EIO : ENOMEM;
	if (e)
		return e;
	LOG(LOG_VERBOSE, "upstream file %s uploaded: %zu bytes", u.data, bytes);
	e = mcp_req_attach(req, u, bytes);
	if (!e && expires)
		(void)uploads_keep(st->uploads, mcp_req_owner(req), id, u, bytes, expires,
		                   clk_realtime().tv_sec);
	return e;
}

/* Do the work of @p step's slot, as its call's kind does. */
static void
mcp_slot_step (struct loop      *loop,
               struct loop_step *step)
{
	(void)loop;
	struct mcp_slot *slot = container_of(step, struct mcp_slot, step);
	mcp_slot_offer(slot)->tool.kind->step(slot);
}

/* Compaction has left the summary, if any, in what askai adds to the
 * prompt. review_pending_drop() drops the pending lines of the review record
 * (see src/review.h). If there is a record, a notice about the task of the
 * ask call and the record follow the summary, cut to mcp_req_lend_max()
 * bytes. The caller's notes follow if req->branch is set. They are left out
 * for a job resumed without --notes.
 */
static void
mcp_req_compact_notes (struct mcp_req *req)
{
	review_pending_drop(req->review);
	int e = mcp_req_notes_identity(req);
	if (!e && req->review)
		e = CAT(&req->self.answer, "\n\n[askai: askai keeps your task word for word "
		        "through every compaction of this ask call: the system text, the earlier "
		        "messages and the prompt, which is all that comes before askai's first "
		        "[askai: ...] line. You need not copy the task into notes or a summary.]\n")
		    ? review_render(req->review, &req->self.answer, mcp_req_lend_max(req))
		    : ENOMEM;
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	if (req->branch && req->mcp->notes) {
		notes_tool_loop_prompt(req->mcp->notes, &req->self, &req->self.answer,
		                       NOTES_TOOL_COMPACT, mcp_req_lend_max(req), mcp_slot_compacted);
		return;
	}
	mcp_req_go_on(req, CSTR_VIEW("\n\n"));
}

/** Handles the compact call of a turn in @p slot with the summary in
 *  @p args. askai replaces the conversation after the prompt with the
 *  summary and the notes. Later calls of the turn do not run.
 */
static void
mcp_slot_compact (struct mcp_slot    *slot,
                  struct json_object *args)
{
	static_assert(SUMMARY_MAX == 4 << 20, "the problem says");
	struct mcp_req *req = slot->req;
	size_t len = 0;
	char const *summary = jb_str(jb_get(args, "summary"), &len);
	if (!summary || !len || len > SUMMARY_MAX) {
		mcp_slot_result(slot, TEXT("The summary argument must be a string of 1 "
		                           "byte to 4 MiB."), true);
		return;
	}
	/* The trace line shows the name of the tool that the turn called. */
	struct registry_tool const *t = &mcp_slot_offer(slot)->tool;
	struct ai_call const c = { .name = t->name };
	(void)mcp_req_trace(req, &c, args, mcp_row_of(t)->trace);
	(void)mcp_req_trace_end(req, false, len);
	hist_add(&req->mcp->counts->calls.compacted[0], 1);
	LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " turn %" PRIu32 ": compacted by "
	    "the upstream's compact call, summary %zu %s", req->mcp->loop->id, req->asked,
	    req->turn, len, mcp_bytes(len));
	req->reminded = 0;
	struct dstr *d = &req->self.answer;
	dstr_clear(d);
	int e = CAT(d, "\n\n[askai: you compacted the context: the conversation since the "
	            "task is gone, your turns and the tools' results with it. Your summary "
	            "of it:]\n\n") && dstr_cat_n(d, summary, jb_utf8(summary, len)) ? 0 : ENOMEM;
	mcp_req_args_free(req, req->part, ARRAY_SIZE(req->args));
	mcp_req_slots_drop(req);
	buf_fini(&req->calls);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	if (req->mcp->draining) {
		mcp_req_fail(req, ESHUTDOWN);
		return;
	}
	mcp_req_compact_notes(req);
}

/* The context is full, as the upstream said or as askai reckons: what
 * follows the prompt goes, and askai adds the notes to it, unless that was
 * done already since a turn went through.
 */
static void
mcp_req_compact_full (struct mcp_req *req)
{
	if (req->mcp->draining) {
		mcp_req_fail(req, ESHUTDOWN);
		return;
	}
	if (req->forced++) {
		mcp_slot_result(&req->self, TEXT("The upstream's context cannot hold the task "
		                              "and the notes kept for it."), true);
		return;
	}
	hist_add(&req->mcp->counts->calls.compacted[1], 1);
	LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " turn %" PRIu32 ": context full, "
	    "compacted by askai", req->mcp->loop->id, req->asked, req->turn);
	req->reminded = 0;
	mcp_req_drop(req);
	mcp_req_args_free(req, 0, ARRAY_SIZE(req->args));
	buf_fini(&req->calls);
	dstr_clear(&req->self.answer);
	int e = CAT(&req->self.answer, "\n\n[askai: your context filled up, so the "
	            "conversation since the task is gone, your turns and the tools' results "
	            "with it.]") ? 0 : ENOMEM;
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	mcp_req_compact_notes(req);
}

#undef CAT

/* Handles the output of the git that reads the notes for the prompt of the
 * ask call in @p slot, req->self. The read parked the request among the
 * calls under way. The request does not wait once git has ended, so the
 * step takes it off that list first.
 */
static void
mcp_slot_ask_step (struct mcp_slot *slot)
{
	mcp_slot_unwait(slot);
	notes_tool_step(slot);
}

/* Start an ask call in @p slot, the request's own, its turns lent the tools
 * that the client's role grants.
 */
static void
mcp_slot_ask (struct mcp_slot    *slot,
              struct json_object *args)
{
	struct mcp_req *req = slot->req;
	bool background = req->mcp->st->ep->background;
	struct json_object *override = nullptr;
	bool has_override = json_object_object_get_ex(args, "background", &override);
	if (has_override && (!json_object_is_type(override, json_type_boolean) ||
	                     (json_object_get_boolean(override) &&
	                      (!background || (req->asks & JOB_STREAM))))) {
		mcp_slot_result(slot, TEXT("The background argument must be false, or true "
		                           "when server background mode is enabled and live "
		                           "streaming is not requested."), true);
		return;
	}
	if (background && args) {
		bool job = json_object_object_get_ex(args, "job", nullptr);
		struct json_object *action;
		bool has_action = json_object_object_get_ex(args, "action", &action);
		struct mcp_piece problem =
			job && (json_object_object_get_ex(args, "prompt", nullptr) ||
			        json_object_object_get_ex(args, "system", nullptr) ||
			        json_object_object_get_ex(args, "messages", nullptr) ||
			        has_override || (req->asks & JOB_STREAM) ||
			        json_object_object_get_ex(args, "thread", nullptr) ||
			        json_object_object_get_ex(args, "timer", nullptr))
			? TEXT("Give a job id to check or cancel, or a prompt to start a job, not both.")
			: has_action && !job
			? TEXT("The action argument requires a job id.")
			: has_action && !jb_is(action, "result") && !jb_is(action, "cancel")
			? TEXT("The action argument must be result or cancel.")
			: (struct mcp_piece){};
		if (problem.bytes.data) {
			mcp_slot_result(slot, problem, true);
			return;
		}
		if (job) {
			req->promote_at = UINT64_MAX;
			mcp_req_arm(req);
			typeof(mcp_slot_client_result) *handler =
				has_action && jb_is(action, "cancel") ? mcp_slot_client_cancel
				                                      : mcp_slot_client_result;
			if (!mcp_req_recover(req, args, handler))
				handler(slot, args, true);
			return;
		}
	}
	if (has_override)
		background = json_object_get_boolean(override);
	else if (req->asks & JOB_STREAM)
		background = false;
	req->lent = mcp_req_lent(req);
	mcp_req_ask(req, args, background);
}

/** Refuses the client's call of the tool @p name in @p req, which the role
 *  of the client does not grant. @p known is true if a role can grant the
 *  tool. The log quotes an unknown name, which comes from the client.
 */
static void
mcp_req_tools_refuse (struct mcp_req   *req,
                      struct cstr_view  name,
                      bool              known)
{
	static struct {
		char const       *why;
		struct cstr_view  head;
	} const refusals[] = {
		{ "the call names no tool",     CSTR_VIEW_INIT("Unknown tool: ") },
		{ "askai offers no such tool",  CSTR_VIEW_INIT("Unknown tool: ") },
		{ "the role does not grant it", CSTR_VIEW_INIT("Tool not available: ") },
	};
	size_t r = known ? 2 : name.data != nullptr;
	struct cstr_view what = r ? name : CSTR_VIEW("(none)");
	struct cstr_view head = refusals[r].head;
	char quoted[NAME_LOG + 8];
	mcp_quote(quoted, sizeof quoted, what, NAME_LOG);
	LOG(LOG_WARN, "tool call %s from %s refused: %s", r == 1 ? quoted : what.data,
	    mcp_req_client(req), refusals[r].why);
	char msg[32 + 6 * 100];
	memcpy(msg, head.data, head.len);
	size_t n = head.len + jb_escape(msg + head.len, what.data,
	                                util_clip(what.data, what.len, 100));
	mcp_req_error(req, 200, INVALID_PARAMS, RAW(msg, n), nullptr, 0);
}

/** Starts the client's tool call in @p req with the members name,
 *  arguments and _meta.progressToken of @p params. The role of the client
 *  must grant the tool, whose name is compared over its full length, as
 *  the Mcp-Name header is. If @p tasks is true, the call becomes a task
 *  after TASK_NS, unless the flags of its kind contain KF_AGENDA.
 */
static void
mcp_req_tools_call (struct mcp_req     *req,
                    struct json_object *params,
                    bool                tasks)
{
	struct mcp_static const *st = req->mcp->st;
	if (req->mcp->draining) {
		mcp_req_fail(req, ESHUTDOWN);
		return;
	}
	struct cstr_view name = jb_view(jb_get(params, "name"));
	uint32_t id = mcp_static_tool_of(st, name);
	if (!mcp_req_granted(req, id)) {
		mcp_req_tools_refuse(req, name, id < st->n_granted);
		return;
	}
	struct mcp_offer const *offer = &st->offers[id];
	struct json_object *meta = jb_get(params, "_meta");
	struct json_object *stream = nullptr;
	bool has_stream = json_object_object_get_ex(jb_get(meta, "askai"), "stream", &stream);
	bool streamed = has_stream && json_object_get_boolean(stream);
	bool ask = offer->tool.kind->start == mcp_slot_ask;
	if (has_stream && (!json_object_is_type(stream, json_type_boolean) ||
	                   (streamed && (!req->sse_ok || !ask)))) {
		mcp_req_error(req, 200, INVALID_PARAMS,
		              FRAG("askai.stream must be boolean; true requires the ask tool "
		                   "and an SSE-capable client"), nullptr, 0);
		return;
	}
	if (streamed)
		req->asks |= JOB_STREAM;
	int e = mcp_req_enrol(req);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	struct json_object *token = jb_get(meta, "progressToken");
	req->token = mcp_id_valid(token) ? json_object_get(token) : nullptr;
	if (tasks && !streamed && !(offer->tool.kind->flags & KF_AGENDA)) {
		req->promote_at = req->mcp->loop->now + TASK_NS;
		mcp_req_arm(req);
	}
	req->self.tool = (uint8_t)id;
	struct json_object *args = jb_get(params, "arguments");
	if (log_threshold >= LOG_VERBOSE)
		mcp_args_shown(offer, args, CSTR_VIEW("none"), req->call_args);
	mcp_slot_start(&req->self, args);
}

static void
mcp_req_initialize (struct mcp_req     *req,
                    struct json_object *params)
{
	static char const head[] = JSON_FIELD "Mcp-Session-Id: ";
	struct cstr_view version = jb_view(jb_get(params, "protocolVersion"));
	if (!mcp_is_legacy(version))
		version = versions[MODERN];

	/* askai does not store the session ID. The ID only scopes the request
	 * IDs that a client cancels. */
	char fields[sizeof head - 1 + 32 + 2];
	struct span f = SPAN(JSON_FIELD);
	if (!util_random_hex(fields + sizeof head - 1, 16)) {
		memcpy(fields, head, sizeof head - 1);
		memcpy(fields + sizeof fields - 2, "\r\n", 2);
		f = (struct span){ (unsigned char *)fields, sizeof fields };
	}

	struct mcp_piece const p[] = {
		FRAG("{\"protocolVersion\":\""),
		BYTES(version),
		DSTR(&req->mcp->st->init_tail),
		FRAG("}"),
	};
	mcp_req_result(req, p, ARRAY_SIZE(p), f);
}

/* Handles the notifications/cancelled in @p req. The notification cancels
 * the calls of the same caller that have the request id in @p params, and
 * these calls can run on any loop. roster_cancel() posts the message of
 * each such record to its call's loop, and mcp_on_withdraw() ends the call
 * there. An id that no request can have names no call. If memory runs
 * out, nothing is cancelled.
 */
static void
mcp_req_cancel (struct mcp_req     *req,
                struct json_object *params)
{
	struct json_object *id = jb_get(params, "requestId");
	struct buf key = {};
	uint64_t hash;
	if (mcp_id_valid(id) && !mcp_req_key(req, id, &key, &hash))
		roster_cancel(req->mcp->st->roster, buf_peek(&key), hash);
	buf_fini(&key);
}

/* Reply with a result serialized once, @p text. */
static void
mcp_req_result_of (struct mcp_req    *req,
                   struct dstr const *text)
{
	struct mcp_piece const p[] = { DSTR(text), FRAG("}") };
	mcp_req_result(req, p, ARRAY_SIZE(p), SPAN(JSON_FIELD));
}

/** Replies to the tools/list of @p req with the definitions of the tools
 *  that the role of the client grants. Each definition begins with a
 *  comma, and the comma of the first definition in the list is left out.
 */
static void
mcp_req_tools_list (struct mcp_req *req)
{
	struct mcp_static const *st = req->mcp->st;
	/* One piece for each tool that a role can be granted, and three more. */
	struct mcp_piece p[TOOL_MAX + 3] = { FRAG("{\"tools\":[") };
	size_t n = 1;
	for (uint32_t id = 0; id < st->n_granted; ++id) {
		struct cstr_view def = cstr_view_dstr(&st->offers[id].listed);
		size_t skip = n == 1;
		if (req->s->grant.tools >> id & 1)
			p[n++] = BYTES(cstr_view_slice(def, skip, SIZE_MAX));
	}
	p[n++] = req->modern ? DSTR(&st->list_tail) : FRAG("]}");
	p[n++] = FRAG("}");
	mcp_req_result(req, p, n, SPAN(JSON_FIELD));
}

/* Copy the result of the task that tasks/get shows into the reply, a slice
 * a step; then close it. The record has the reply of the task's call:
 * '"result":{...}' or '"error":{...}', and a '}' that closes the object
 * around it. A result in the record has no modern tail. For a modern
 * reader of a result, the copy leaves out the record's last two bytes, and
 * the modern tail and a '}' follow the copy. The reply of tasks/get ends
 * with one more '}'.
 */
static void
mcp_req_show (struct mcp_req *req)
{
	static char const key[] = "\"result\":";
	struct mcp_static const *st = req->mcp->st;
	struct loop *loop = req->mcp->loop;
	struct ledger_task *task = req->shown;
	bool tail = req->modern && task->len > sizeof key &&
	            !memcmp(task->result, key, sizeof key - 1);
	size_t left = (tail ? task->len - 2 : task->len) - req->off;
	size_t room = loop_room(loop, LOOP_COPY);
	size_t n = left < room ? left : room;
	int e = mcp_req_put(req, &RAW(task->result + req->off, n), 1);
	loop_work(loop, LOOP_COPY, n);
	req->off += n;
	if (!e && n < left) {
		loop_ready_bulk(loop, &req->step);
		return;
	}
	ledger_task_put(task);
	req->shown = nullptr;
	struct mcp_piece const p[] = {
		tail ? DSTR(&st->modern_tail) : FRAG(""),
		RAW("}}", (size_t)tail + (req->method == METHOD_TASKS_GET)),
	};
	mcp_req_close(req, 200, SPAN(JSON_FIELD), e ? e : mcp_req_put(req, p, ARRAY_SIZE(p)));
}

/** Answers the tasks/get of @p req with the status of @p task. After the
 *  call of the task ends, the answer also contains the result or the error
 *  of the call, which mcp_req_show() copies. @p task is a reference, which
 *  this function or mcp_req_show() releases.
 */
static void
mcp_req_task_show (struct mcp_req     *req,
                   struct ledger_task *task)
{
	static_assert(POLL_MS == 1000, "the reply says");
#define S(s) { CSTR_VIEW_INIT(s), false }
	static struct mcp_piece const statuses[LEDGER_STATUSES] = {
		[LEDGER_WORKING]   = S("working"),
		[LEDGER_COMPLETED] = S("completed"),
		[LEDGER_FAILED]    = S("failed"),
		[LEDGER_CANCELLED] = S("cancelled"),
	};
	static struct mcp_piece const said[LEDGER_STATUSES] = {
		[LEDGER_FAILED]    = S(",\"statusMessage\":\"The tool call failed: its error "
		                       "says why.\""),
		[LEDGER_CANCELLED] = S(",\"statusMessage\":\"The tool call was cancelled, "
		                       "as asked.\""),
	};
#undef S
	struct mcp_static const *st = req->mcp->st;
	enum ledger_status status = atomic_load_explicit(&task->status, memory_order_acquire);
	bool whole = false;
	uint64_t updated_at = task->created;
	char ttl[24] = "null";
	size_t n_ttl = sizeof "null" - 1;
	/* A tasks/get of a record whose call ended marks the record read.
	 * The reply then contains the end of the call, and the time to live is
	 * the run time of the call plus the time that the record stays. */
	if (status != LEDGER_WORKING) {
		atomic_store_explicit(&task->told, 1, memory_order_relaxed);
		whole = task->result != nullptr;
		updated_at = task->ended;
		uint64_t ran = task->ended > task->created ? task->ended - task->created : 0;
		n_ttl = util_format(ttl, sizeof ttl, "%" PRIu64,
		                    ran / 1'000'000 + LEDGER_KEEP * 1000);
	}
	char created[ISO_TIME];
	char updated[ISO_TIME];
	struct mcp_piece const p[] = {
		FRAG("{\"resultType\":\"complete\","),
		DSTR(&st->server_meta),
		FRAG(",\"taskId\":\""),
		RAW(task->id, LEDGER_ID),
		FRAG("\",\"status\":\""),
		statuses[status],
		FRAG("\",\"createdAt\":\""),
		RAW(created, mcp_iso_time(created, task->created)),
		FRAG("\",\"lastUpdatedAt\":\""),
		RAW(updated, mcp_iso_time(updated, updated_at)),
		FRAG("\",\"ttlMs\":"),
		RAW(ttl, n_ttl),
		FRAG(",\"pollIntervalMs\":1000"),
		said[status],
		/* A result or error goes after a comma, and closes the object. */
		whole ? FRAG(",")
		: status == LEDGER_FAILED ? FRAG(",\"error\":{\"code\":-32603,\"message\":"
		                                 "\"Internal error\"}}")
		: FRAG("}"),
	};
	int e = mcp_req_open(req, FRAG("\"result\":"));
	e = e ? e : mcp_req_put(req, p, ARRAY_SIZE(p));
	if (e || !whole) {
		ledger_task_put(task);
		mcp_req_close(req, 200, SPAN(JSON_FIELD), e ? e : mcp_req_put(req, &FRAG("}"), 1));
		return;
	}
	req->shown = task;
	req->off = 0;
	mcp_req_enter(req, PHASE_SHOW);
}

/* tasks/get, tasks/update or tasks/cancel, of the task that @p params'
 * taskId names, which only its owner can look up; for a client that
 * takes tasks, as @p tasks says its request declares. askai asks for no
 * input, so tasks/update has none to take.
 */
static void
mcp_req_tasks (struct mcp_req     *req,
               struct json_object *params,
               bool                tasks)
{
	struct mcp_static const *st = req->mcp->st;
	if (!tasks) {
		struct mcp_piece const data[] = {
			FRAG("{\"requiredCapabilities\":{\"extensions\":{\""),
			FRAG(meta_tasks),
			FRAG("\":{}}}}"),
		};
		mcp_req_error(req, 400, MISSING_CAPABILITY,
		              FRAG("Missing required client capability"), data,
		              ARRAY_SIZE(data));
		return;
	}
	size_t len = 0;
	char const *id = jb_str(jb_get(params, "taskId"), &len);
	struct ledger_task *task = id ? ledger_find(st->ledger, mcp_req_owner(req), id, len)
	                              : nullptr;
	if (!task) {
		mcp_req_error(req, 200, INVALID_PARAMS,
		              FRAG("Failed to retrieve task: Task not found"), nullptr, 0);
		return;
	}
	if (req->method == METHOD_TASKS_GET) {
		mcp_req_task_show(req, task);
		return;
	}
	if (req->method == METHOD_TASKS_CANCEL)
		ledger_task_cancel(task);
	ledger_task_put(task);
	struct mcp_piece const p[] = {
		FRAG("{\"resultType\":\"complete\","),
		DSTR(&st->server_meta),
		FRAG("}}"),
	};
	mcp_req_result(req, p, ARRAY_SIZE(p), SPAN(JSON_FIELD));
}

/* Answer a valid request, or start the tool call it asks for, unless its era
 * has no such method; @p tasks says whether it declares the tasks
 * extension.
 */
static void
mcp_req_route (struct mcp_req     *req,
               struct json_object *params,
               bool                tasks)
{
	/* The eras that have each method, bit 0 legacy, bit 1 modern. */
	static uint8_t const eras[METHOD_N] = {
		[METHOD_INITIALIZE]   = 1,
		[METHOD_PING]         = 1,
		[METHOD_TOOLS_LIST]   = 3,
		[METHOD_TOOLS_CALL]   = 3,
		[METHOD_DISCOVER]     = 2,
		[METHOD_TASKS_GET]    = 2,
		[METHOD_TASKS_UPDATE] = 2,
		[METHOD_TASKS_CANCEL] = 2,
	};
	struct mcp_static const *st = req->mcp->st;
	if (!(eras[req->method] >> req->modern & 1)) {
		/* Legacy clients take a 404 for an expired session, so keep 200. */
		mcp_req_error(req, req->modern ? 404 : 200, METHOD_NOT_FOUND,
		              FRAG("Method not found"), nullptr, 0);
		return;
	}
	switch (req->method) {
	case METHOD_TOOLS_CALL:
		mcp_req_tools_call(req, params, tasks);
		return;
	case METHOD_TASKS_GET:
	case METHOD_TASKS_UPDATE:
	case METHOD_TASKS_CANCEL:
		mcp_req_tasks(req, params, tasks);
		return;
	case METHOD_TOOLS_LIST:
		mcp_req_tools_list(req);
		return;
	case METHOD_DISCOVER:
		mcp_req_result_of(req, &st->discover);
		return;
	case METHOD_INITIALIZE:
		mcp_req_initialize(req, params);
		return;
	default:
		mcp_req_result(req, &FRAG("{}}"), 1, SPAN(JSON_FIELD));
	}
}

/** Validates the metadata of the modern request of @p req with the method
 *  @p method, then answers it. @p meta is the member _meta of @p params,
 *  and @p version is the protocol version in @p meta.
 */
static void
mcp_req_modern (struct mcp_req     *req,
                struct json_object *params,
                struct json_object *meta,
                struct cstr_view    version,
                struct cstr_view    method)
{
	struct stream *s = req->s;
	struct json_object *caps = jb_get(meta, meta_caps);
	if (!version.data || !json_object_is_type(caps, json_type_object)) {
		mcp_req_error(req, 400, INVALID_PARAMS,
		              FRAG("Request _meta must carry protocolVersion and "
		                   "clientCapabilities"), nullptr, 0);
		return;
	}
	if (!mcp_span_is(s->ops->field(s, F_VERSION), version)) {
		mcp_req_error(req, 400, HEADER_MISMATCH,
		              FRAG("MCP-Protocol-Version header does not match the "
		                   "request _meta"), nullptr, 0);
		return;
	}
	if (cstr_view_index(versions, ARRAY_SIZE(versions), version) >= MODERN) {
		struct mcp_piece const data[] = {
			FRAG("{\"supported\":"),
			DSTR(&req->mcp->st->versions),
			FRAG(",\"requested\":\""),
			ESC(version.data, util_clip(version.data, version.len, MAX_ECHO)),
			FRAG("\"}"),
		};
		mcp_req_error(req, 400, UNSUPPORTED_VERSION,
		              FRAG("Unsupported protocol version"), data, ARRAY_SIZE(data));
		return;
	}
	if (!mcp_span_is(s->ops->field(s, F_METHOD), method)) {
		mcp_req_error(req, 400, HEADER_MISMATCH,
		              FRAG("Mcp-Method header does not match the request method"),
		              nullptr, 0);
		return;
	}
	/* Mcp-Name mirrors the tool of a tool call and the task of a task
	 * method. */
	bool call = req->method == METHOD_TOOLS_CALL;
	bool task = req->method >= METHOD_TASKS_GET && req->method <= METHOD_TASKS_CANCEL;
	struct cstr_view name = jb_view(jb_get(params, call ? "name" : "taskId"));
	if ((call || task) && name.data && !mcp_span_is(s->ops->field(s, F_NAME), name)) {
		mcp_req_error(req, 400, HEADER_MISMATCH,
		              call ? FRAG("Mcp-Name header does not match the tool name")
		                   : FRAG("Mcp-Name header does not match the task id"),
		              nullptr, 0);
		return;
	}
	req->sse_ok = mcp_accepts_sse(s->ops->field(s, F_ACCEPT));
	struct json_object *ext = jb_get(jb_get(caps, "extensions"), meta_tasks);
	mcp_req_route(req, params, json_object_is_type(ext, json_type_object));
}

/** Answers the request of @p req with the method @p method and the id
 *  @p id in the protocol era of the client.
 */
static void
mcp_req_request (struct mcp_req     *req,
                 struct cstr_view    method,
                 struct json_object *id)
{
	struct stream *s = req->s;
	struct json_object *params = jb_get(req->msg, "params");
	req->method = mcp_method_index(method);
	if (!mcp_id_valid(id)) {
		mcp_req_error(req, 400, INVALID_REQUEST, FRAG("Invalid request id"), nullptr, 0);
		return;
	}
	req->id = json_object_get(id);
	if (params && !json_object_is_type(params, json_type_object)) {
		mcp_req_error(req, 400, INVALID_PARAMS, FRAG("Params must be an object"),
		              nullptr, 0);
		return;
	}
	struct json_object *meta = jb_get(params, "_meta");
	struct cstr_view version = jb_view(jb_get(meta, meta_version));
	struct span header = s->ops->field(s, F_VERSION);
	struct cstr_view field = { (char const *)header.ptr, header.len };
	req->modern = (version.data && !mcp_is_legacy(version)) ||
	              (field.data && !mcp_is_legacy(field));
	if (req->modern) {
		mcp_req_modern(req, params, meta, version, method);
		return;
	}
	/* A legacy client without the header uses the revision 2025-03-26,
	 * which supports event streams. The revision 2024-11-05 does not. */
	req->sse_ok = mcp_accepts_sse(s->ops->field(s, F_ACCEPT)) &&
	              !mcp_span_is(header, versions[PRE_STREAMS]);
	mcp_req_route(req, params, false);
}

/* Take a response or a notification, which have no answer. */
static void
mcp_req_accept (struct mcp_req *req)
{
	req->method = METHOD_NOTIFY;
	(void)req->s->ops->head(req->s, 202, (struct span){}, 0);
	mcp_req_finish(req);
}

/** Answers the message of @p req, or starts the tool call of the message.
 *  A tool call frees the parts of the message that it does not use when
 *  it starts to wait.
 */
static void
mcp_req_message (struct mcp_req *req)
{
	struct json_object *msg = req->msg;
	struct cstr_view method = jb_view(jb_get(msg, "method"));
	struct json_object *id;
	bool has_id = json_object_object_get_ex(msg, "id", &id);
	if (!jb_is(jb_get(msg, "jsonrpc"), "2.0")) {
		mcp_req_error(req, 400, INVALID_REQUEST, FRAG("Invalid Request"), nullptr, 0);
		return;
	}
	if (!method.data) {
		/* askai sends no requests, so a message without a method is a
		 * response, which contains an id and a result or an error. */
		if (has_id && (jb_get(msg, "result") || jb_get(msg, "error")))
			mcp_req_accept(req);
		else
			mcp_req_error(req, 400, INVALID_REQUEST, FRAG("Invalid Request"),
			              nullptr, 0);
		return;
	}
	if (!has_id) {
		if (cstr_view_equal(method, CSTR_VIEW("notifications/cancelled")))
			mcp_req_cancel(req, jb_get(msg, "params"));
		mcp_req_accept(req);
		return;
	}
	mcp_req_request(req, method, id);
}

/* Hand the parsed message on. */
static void
mcp_req_dispatch (struct mcp_req *req)
{
	if (!req->msg) {
		mcp_req_error(req, 400, PARSE_ERROR, FRAG("Parse error"), nullptr, 0);
		return;
	}
	mcp_req_message(req);
}

/* Parse as much of @p json as the step has the share for, with a tokener from
 * @p pool. Returns EAGAIN while bytes remain. Once the parse is over, its
 * value is in req->msg, and the function returns 0, or ENOMEM or EINVAL with
 * req->msg nullptr (jb_parser_step()).
 */
static int
mcp_req_slice (struct mcp_req   *req,
               struct jb_pool   *pool,
               struct cstr_view  json)
{
	struct loop *loop = req->mcp->loop;
	struct jb_parser *parser = &req->parser;
	uint64_t bytes = loop_room(loop, LOOP_PARSE);
	uint64_t values = loop_room(loop, LOOP_VALUES);
	if (bytes < 4 || !values)
		return EAGAIN;
	size_t off = parser->off;
	uint32_t seen = parser->cut.values;
	size_t copied = parser->cut.copied;
	int e = jb_parser_step(parser, pool, json.data, json.len, bytes, (uint32_t)values);
	loop_work(loop, LOOP_PARSE, parser->off - off);
	loop_work(loop, LOOP_VALUES, parser->cut.values - seen);
	loop_work(loop, LOOP_COPY, parser->cut.copied - copied);
	if (e != EAGAIN) {
		req->values = parser->cut.values;
		req->msg = jb_parser_take(parser);
		jb_parser_fini(parser, pool, &req->reaper);
	}
	return e;
}

/* Parse as much of the body as the step has the share for, the rest a slice
 * a bulk step; the message is handed on at once if one step parsed it all,
 * else in a step of its own.
 */
static void
mcp_req_parse (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	bool whole = !req->parser.off;
	struct span body = buf_peek(&req->body);
	int e = mcp_req_slice(req, &req->mcp->pool,
	                      (struct cstr_view){ (char const *)body.ptr, body.len });
	if (e == EAGAIN) {
		loop_ready_bulk(loop, &req->step);
		return;
	}
	buf_fini(&req->body);
	if (e == ENOMEM) {
		mcp_req_fail(req, e);
		return;
	}
	req->phase = PHASE_DISPATCH;
	if (whole)
		mcp_req_dispatch(req);
	else
		loop_ready_bulk(loop, &req->step);
}

static void
mcp_req_reasoning_done (struct mcp_req *req,
                        int             error)
{
	if (error) {
		mcp_req_fail(req, error);
		return;
	}
	mcp_req_enter(req, PHASE_TEXT);
}

/* A provider may ignore stream=true and send one complete JSON body.
 * Extract its exposed reasoning once, before releasing that parsed body;
 * response text and wire JSON are not transcript records.
 */
static int
mcp_req_reasoning (struct mcp_req *req)
{
	struct journal *journal = req->mcp->st->journal;
	struct job const *job = req->job;
	if (!journal || !*req->archive || !req->msg || job->http < 200 ||
	    job->http >= 300 || jb_get(req->msg, "error"))
		return 0;
	struct dstr thought = dstr("");
	int e = ai_thoughts(req->mcp->st->ep->api, req->msg, &thought);
	size_t len = dstr_len(&thought);
	if (e) {
		dstr_cleanup(&thought);
		return e;
	}
	struct mcp_archive_wait *wait = calloc(1, sizeof *wait);
	if (!wait) {
		dstr_cleanup(&thought);
		return ENOMEM;
	}
	wait->done = mcp_req_reasoning_done;
	e = mcp_req_archive_wait(req, &wait->who);
	if (!e && len)
		e = journal_thought(journal, req->archive, job->archive_turn, 0,
		                    dstr_get(&thought), len, nullptr, nullptr, nullptr);
	loop_work(req->mcp->loop, LOOP_COPY, len);
	dstr_cleanup(&thought);
	if (!e)
		e = journal_thought_end(journal, req->archive, job->archive_turn, true,
		                        req->mcp->loop, mcp_archive_back, wait);
	if (e) {
		if (wait->who.bits)
			loop_forget(req->mcp->loop, wait->who);
		free(wait);
		wait = nullptr;
		return e;
	}
	req->archive_call = wait->who;
	req->phase = PHASE_JOURNAL;
	return 0;
}

/* Parse as much of the upstream's answer as the step has the share for, and
 * check it is UTF-8 text; once it is parsed, or found not to be JSON, read the
 * tool's output from it in a step of its own, as that copies the output.
 */
static void
mcp_req_answer (struct mcp_req *req)
{
	struct span json = buf_peek(&req->job->result);
	char const *text = json.len ? (char const *)json.ptr : "";
	size_t off = req->parser.off;
	int e = mcp_req_slice(req, &req->mcp->answers, (struct cstr_view){ text, json.len });
	if (e == ENOMEM) {
		mcp_req_fail(req, e);
		return;
	}
	bool over = e != EAGAIN;
	size_t end = !over ? req->parser.off : req->msg ? json.len : off;
	if (jb_utf8(text + off, end - off) < end - off) {
		jb_parser_fini(&req->parser, &req->mcp->answers, &req->reaper);
		mcp_req_dispose(req);
		over = true;
	}
	if (over) {
		req->phase = PHASE_TEXT;
		e = mcp_req_reasoning(req);
		if (e) {
			mcp_req_fail(req, e);
			return;
		}
		if (req->phase == PHASE_JOURNAL)
			return;
	}
	loop_ready_bulk(req->mcp->loop, &req->step);
}

/** Reads the output of the tool from the answer of the job of @p req,
 *  then frees the answer and releases the job. home already read the text
 *  of a streamed answer into the result of the job.
 */
static void
mcp_req_text (struct mcp_req *req)
{
	struct job *job = req->job;
	struct span body = buf_peek(&job->result);
	/* A streamed answer without text after req->stopped is set is a failure. */
	if (job->status == JOB_TEXT) {
		mcp_slot_respond(&req->self, req->stopped && !body.len);
		return;
	}
	bool failed = true;
	int e = ai_answer(req->mcp->st->ep->api, job->http, req->msg,
	                  body.len ? (char const *)body.ptr : "", body.len, &req->self.answer,
	                  &failed);
	loop_work(req->mcp->loop, LOOP_COPY, dstr_len(&req->self.answer));
	mcp_req_dispose(req);
	mcp_req_drop(req);
	if (e) {
		mcp_req_fail(req, ENOMEM);
		return;
	}
	if (failed && mcp_req_refit(req, cstr_view_dstr(&req->self.answer))) {
		mcp_req_compact_full(req);
		return;
	}
	mcp_slot_respond(&req->self, failed);
}

/* Render a share of the metrics into the reply; once all are in, send it. */
static void
mcp_req_metrics (struct mcp_req *req)
{
	static char const fields[] = "Content-Type: text/plain; version=0.0.4; "
	                             "charset=utf-8\r\n";
	int e = metrics_render(req->mcp->metrics, &req->off, &req->reply);
	if (e == EAGAIN) {
		loop_ready_bulk(req->mcp->loop, &req->step);
		return;
	}
	mcp_req_close(req, 200, SPAN(fields), e);
}

/* The work of each phase that works in steps; the others wait. */
static void (*const mcp_phase_steps[]) (struct mcp_req *req) = {
	[PHASE_PARSE]    = mcp_req_parse,
	[PHASE_DISPATCH] = mcp_req_dispatch,
	[PHASE_BUILD]    = mcp_req_build,
	[PHASE_ANSWER]   = mcp_req_answer,
	[PHASE_TEXT]     = mcp_req_text,
	[PHASE_ARGS]     = mcp_req_turn_args,
	[PHASE_TURN]     = mcp_req_turn_write,
	[PHASE_CALLS]    = mcp_req_calls_next,
	[PHASE_RESULT]   = mcp_req_result_write,
	[PHASE_ADD]      = mcp_req_add_write,
	[PHASE_WAKE]     = mcp_req_add_write,
	[PHASE_SHOW]     = mcp_req_show,
	[PHASE_REPLY]    = mcp_req_reply,
	[PHASE_SEND]     = mcp_req_send,
	[PHASE_METRICS]  = mcp_req_metrics,
};
static_assert(ARRAY_SIZE(mcp_phase_steps) == PHASE_METRICS + 1);

/* A keep-alive, if one is due before the reply, a slice of what is left to
 * free, then the phase's work.
 */
static void
mcp_req_step (struct loop      *loop,
              struct loop_step *step)
{
	struct mcp_req *req = container_of(step, struct mcp_req, step);
	if (req->delta) {
		mcp_req_delta(req);
		return;
	}
	if (req->tick && req->phase < PHASE_REPLY) {
		req->tick = false;
		mcp_req_tick(req);
	}
	if (req->reaper.n) {
		loop_work(loop, LOOP_VALUES, jb_reaper_step(&req->reaper,
		                                            loop_room(loop, LOOP_VALUES)));
		if (req->reaper.n) {
			loop_ready_bulk(loop, step);
		} else if (req->phase == PHASE_FREE) {
			mcp_req_end(req);
			return;
		}
	}
	if (mcp_phase_steps[req->phase])
		mcp_phase_steps[req->phase](req);
}

static void
mcp_req_timer (struct loop       *loop,
               struct loop_timer *timer)
{
	struct mcp_req *req = container_of(timer, struct mcp_req, timer);
	req->tick = true;
	loop_ready(loop, &req->step);
}

/* Begin a request on @p s, in @p phase and counted as @p method; nullptr,
 * the stream answered, if out of memory.
 */
static struct mcp_req *
mcp_begin (struct mcp           *mcp,
           struct stream        *s,
           enum mcp_phase        phase,
           enum metrics_method   method)
{
	struct mcp_req *req = mcp_spare_take(mcp);
	if (!req) {
		mcp_http_error(s, 500, "Out of memory", SPAN(JSON_FIELD));
		return nullptr;
	}
	*req = (struct mcp_req){
		.step.fn = mcp_req_step,
		.timer.fn = mcp_req_timer,
		.reaper = req->reaper,
		.scan = req->scan,
		.self = { .step.fn = mcp_slot_step, .answer = req->self.answer, .req = req,
		          .tool = TOOL_MAX },
		.trace = req->trace,
		.splice = req->splice,
		.splice_max = req->splice_max,
		.s = s,
		.mcp = mcp,
		.head = loop_clock(),
		.principal = s->grant.principal,
		.generation = instance_table_generation(&mcp->instances),
		.ping_at = UINT64_MAX,
		.promote_at = UINT64_MAX,
		.phase = phase,
		.method = method
	};
	/* The client's name is in the policy, which a reload can free after
	 * the head, so it is copied. The log names a client on a Unix socket
	 * by its uid, which is in peer[1] (src/stream.h), with the name from
	 * the policy after it in parentheses. */
	struct cstr_view const who = s->grant.who;
	if (s->peer[0] != UINT64_MAX) {
		if (who.len)
			memcpy(req->client, who.data, who.len < sizeof req->client
			                              ? who.len : sizeof req->client - 1);
	} else {
		size_t n = util_format(req->client, sizeof req->client, "uid %" PRIu64,
		                       s->peer[1]);
		if (who.len)
			(void)util_format(req->client + n, sizeof req->client - n, " (%.*s)",
			                  (int)who.len, who.data);
	}
	s->user = req;
	return req;
}

static void
mcp_on_head (void          *ctx,
             struct stream *s,
             struct span    method,
             struct span    target,
             size_t         length)
{
	struct mcp *mcp = ctx;
	if (mcp_span_is(method, CSTR_VIEW("GET")) && mcp_is_metrics(target)) {
		if (s->grant.metrics)
			(void)mcp_begin(mcp, s, PHASE_METRICS, METHOD_METRICS);
		else
			mcp_http_error(s, 403, "Forbidden", SPAN(JSON_FIELD));
		return;
	}
	if (!mcp_span_is(method, CSTR_VIEW("POST"))) {
		/* No standalone event stream (GET) and no sessions to DELETE. */
		mcp_http_error(s, 405, "Method Not Allowed",
		               SPAN(JSON_FIELD "Allow: POST, OPTIONS\r\n"));
		return;
	}
	if (!mcp_is_json(s->ops->field(s, F_TYPE))) {
		mcp_http_error(s, 415, "Unsupported Media Type", SPAN(JSON_FIELD));
		return;
	}
	struct mcp_req *req = mcp_begin(mcp, s, PHASE_BODY, METHOD_OTHER);
	/* A body of known length is taken in without growing. */
	if (req && length && length != STREAM_OPEN)
		(void)buf_reserve(&req->body, length, length, &(struct span){});
}

static void
mcp_on_body (struct stream *s,
             struct span    bytes,
             bool           last)
{
	struct mcp_req *req = s->user;
	if (req->phase == PHASE_METRICS) {
		/* A body to a GET says nothing. */
		if (last)
			loop_ready_bulk(req->mcp->loop, &req->step);
		return;
	}
	struct mcp_limits const *lim = &req->mcp->st->lim;
	size_t n = jb_scan(&req->scan, (char const *)bytes.ptr, bytes.len, lim->depth,
	                   lim->values, lim->members, MAX_SCALAR);
	if (n < bytes.len) {
		char c = (char)bytes.ptr[n];
		mcp_req_http_error(req, 400,
		                   req->scan.state == JB_SCAN_SCALAR && req->scan.run == MAX_SCALAR
		                   ? "Request JSON has too long a number"
		                   : c == ':' ? "Request JSON object has too many members"
		                   : (c == '{' || c == '[') && req->scan.depth == lim->depth
		                   ? "Request JSON is nested too deeply"
		                   : req->scan.values == lim->values
		                   ? "Request JSON has too many values"
		                   : "Out of memory");
		return;
	}
	if (bytes.len && mcp_piece_write(&RAW(bytes.ptr, bytes.len), 0, bytes.len,
	                                 &req->body, SIZE_MAX)) {
		mcp_req_http_error(req, 500, "Out of memory");
		return;
	}
	if (last) {
		req->phase = PHASE_PARSE;
		mcp_req_parse(req);
	}
}

/* The output has drained: send on. */
static void
mcp_on_writable (struct stream *s)
{
	struct mcp_req *req = s->user;
	req->blocked = false;
	req->tick = true;
	loop_ready_bulk(req->mcp->loop, &req->step);
}

static void
mcp_on_cancel (struct stream *s)
{
	struct mcp_req *req = s->user;
	req->s = nullptr;
	/* A call that frees its message has ended, and its end record, if it
	 * has one, is written or was withheld by a drain on purpose. */
	if (req->phase == PHASE_FREE)
		return;
	/* A reply that has gone whole ends as it did (mcp_req_abort()). */
	if (req->phase != PHASE_END)
		mcp_req_log_call(req, "cancelled by a closed connection", (struct cstr_view){}, false);
	mcp_req_abort(req, ECANCELED);
}

/* The on_policy() function of the stream application. The loop of @p ctx,
 * a struct mcp, has replaced its policy after a reload. The instances that
 * exist were made for users as the old policy identified them. After
 * instance_table_flush(), no call of a later request gets one of them.
 */
static void
mcp_on_policy (void *ctx)
{
	struct mcp *mcp = ctx;
	instance_table_flush(&mcp->instances);
}

struct stream_app const *
mcp_app (struct mcp *mcp)
{
	return &mcp->app;
}

/* Draining: end the tool calls under way with an error, as many a step as
 * its share of requests allows.
 */
static void
mcp_drain_step (struct loop      *loop,
                struct loop_step *step)
{
	struct mcp *mcp = container_of(step, struct mcp, drain);
	for (; !list_is_empty(&mcp->calls); loop_work(loop, LOOP_REQS, 1)) {
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		mcp_req_fail(container_of(mcp->calls.next, struct mcp_req, hook), ESHUTDOWN);
	}
}

/* Destroy the first @p n of @p mcp's parts, last first. */
static void
mcp_destroy_parts (struct mcp *mcp,
                   uint32_t    n)
{
	while (n--) {
		struct mcp_plugin const *p = &mcp->st->plugins[n];
		if (p->part_destroy)
			p->part_destroy(&mcp->parts[n]);
	}
}

struct mcp *
mcp_create (struct loop              *loop,
            struct loop              *home,
            struct mcp_static const  *st,
            struct metrics_src const *metrics,
            struct metrics_mcp       *counts,
            int                      *e)
{
	struct mcp *mcp = malloc(sizeof *mcp + st->n_plugins * sizeof *mcp->parts);
	if (!mcp) {
		*e = ENOMEM;
		return nullptr;
	}
	*mcp = (struct mcp){
		.drain.fn = mcp_drain_step,
		.app = {
			.fields = field_names,
			.n_fields = F_N,
			.lists = 1 << F_ACCEPT,
			.ctx = mcp,
			.on_head = mcp_on_head,
			.on_body = mcp_on_body,
			.on_writable = mcp_on_writable,
			.on_cancel = mcp_on_cancel,
			.on_policy = mcp_on_policy
		},
		.pool = {
			.depth = st->lim.depth,
			.scalar = MAX_SCALAR,
			.flags = JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8
		},
		.answers = {
			.scalar = MAX_SCALAR,
			.flags = JSON_TOKENER_VALIDATE_UTF8
		},
		.metrics = metrics,
		.loop = loop,
		.home = home,
		.st = st,
		.counts = counts
	};
	list_init(&mcp->calls);
	list_init(&mcp->tasks);
	list_init(&mcp->freeing);
	list_init(&mcp->ending);
	list_init(&mcp->spare.list);
	list_init(&mcp->jobs.list);
	*e = instance_table_init(&mcp->instances, loop);
	if (*e) {
		free(mcp);
		return nullptr;
	}
	for (uint32_t i = 0; i < st->n_plugins; ++i) {
		struct mcp_plugin const *p = &st->plugins[i];
		/* For a plugin without part_create(), the part is the service of
		 * the plugin. A plugin that askai loaded from a file provides
		 * neither, so its part is nullptr. */
		int pe = 0;
		mcp->parts[i] = p->service;
		if (p->part_create && !(mcp->parts[i] = p->part_create(p->service, loop, &pe))) {
			*e = pe ? pe : ENOMEM;
			mcp_destroy_parts(mcp, i);
			instance_table_fini(&mcp->instances);
			free(mcp);
			return nullptr;
		}
		if (p->entry.plugin == &notes_tool_plugin)
			mcp->notes = mcp->parts[i];
	}
	self = mcp;
	return mcp;
}

void
mcp_destroy (struct mcp **p_mcp)
{
	struct mcp *mcp = *p_mcp;
	if (!mcp)
		return;
	*p_mcp = nullptr;
	mcp->draining = true;

	/* The transport has cancelled every stream by now, but for the
	 * stand-ins of the calls that are tasks, which end here, their tasks
	 * failed. The calls whose end record is still being written end here
	 * too, as no journal callback reaches them now. Then only requests
	 * still freeing their message are left, and what the parts do, such as
	 * the man fetches. The parts go once what they keep is given back. */
	while (!list_is_empty(&mcp->tasks))
		mcp_req_abort(container_of(mcp->tasks.next, struct mcp_req, task_hook),
		              ESHUTDOWN);
	while (!list_is_empty(&mcp->ending))
		mcp_req_abort(container_of(mcp->ending.next, struct mcp_req, hook), ESHUTDOWN);
	while (!list_is_empty(&mcp->freeing))
		mcp_req_end(container_of(mcp->freeing.next, struct mcp_req, hook));
	while (mcp_relieve(mcp, BUF_CRITICAL, SIZE_MAX))
		;
	instance_table_fini(&mcp->instances);
	mcp_destroy_parts(mcp, mcp->st->n_plugins);
	loop_unready(&mcp->drain);
	self = nullptr;
	free(mcp);
}

void
mcp_age (struct mcp *mcp)
{
	shelf_age(&mcp->spare);
	shelf_age(&mcp->jobs);
}

size_t
mcp_relieve (struct mcp        *mcp,
             enum buf_pressure  level,
             size_t             max)
{
	/* A tokener keeps the buffer of the longest string it parsed, which
	 * may be mapped: at high pressure one longer than a slice goes, at
	 * critical any, each in a call of its own. */
	size_t keep = level >= BUF_CRITICAL ? 0 : LOOP_STEP_PARSE;
	size_t size = jb_pool_release(&mcp->pool, keep);
	if (size || (size = jb_pool_release(&mcp->answers, keep))) {
		atomic_store_explicit(&mcp->counts->kept.bytes[1], mcp->pool.bytes +
		                      mcp->answers.bytes, memory_order_relaxed);
		return buf_units(size);
	}
	size_t done = 0;
	for (size_t n = shelf_unused(&mcp->spare, level); n; --n) {
		size_t units = buf_units(mcp_req_kept(container_of(mcp->spare.list.next,
		                                                   struct mcp_req, hook)));
		if (done && done + units > max)
			return done;
		mcp_spare_drop(mcp);
		done += units;
	}
	/* Jobs keep no storage, so each is a unit. */
	for (size_t n = shelf_unused(&mcp->jobs, level); n && done < max; --n, ++done) {
		hist_add(&mcp->counts->kept.jobs, -1ULL);
		free(container_of(shelf_drop(&mcp->jobs), struct job, spare));
	}
	/* Freeing one spare request can count more units than max, so done
	 * can be greater than max here. */
	if (done < max)
		done += instance_table_relieve(&mcp->instances, level, max - done);
	return done;
}

void
mcp_drain (struct mcp *mcp)
{
	mcp->draining = true;
	loop_ready_bulk(mcp->loop, &mcp->drain);
}

/** Returns the definition of the ask tool. @p service is the mcp_static,
 *  which contains the configured name and description of the tool. The
 *  definition contains the argument thread if the turns can be lent the
 *  notes tool, which requires --notes and --upstream-tools.
 */
static struct json_object *
mcp_tool_definition (void const *service)
{
	struct mcp_static const *st = service;
	struct cstr_view const keys[] = { CSTR_VIEW("role"), CSTR_VIEW("content") };
	struct json_object *props = jb_object(
		"prompt", schema_property("string",
			"The new question or task. Include the context it needs, or supply "
			"earlier user and assistant turns in messages for a continuing chat."),
		"system", schema_property("string",
			"Optional system instructions for the other AI, such as a role "
			"or an answer format."),
		"messages", jb_object(
			"type", json_object_new_string("array"),
			"maxItems", json_object_new_int(CHAT_HISTORY),
			"description", json_object_new_string(
				"Earlier visible chat messages, in order, before the new prompt. "
				"Each has role user or assistant and plain string content. The "
				"client retains this history; thread does not retain messages."),
			"items", schema_object(
				jb_object("role", schema_enum(schema_names(roles, ARRAY_SIZE(roles)),
				                              nullptr),
				          "content", schema_property("string", nullptr),
				          nullptr),
				schema_names(keys, ARRAY_SIZE(keys))),
			nullptr),
		"background", schema_property("boolean",
			"False waits for the answer on this connection. True requires server "
			"background mode and returns a job id. Omission follows the server "
			"setting, unless live streaming is requested."),
		nullptr);
	if (st->lending.of[KIND_NOTES])
		props = jb_set_or_free(props, "thread", schema_property("string",
			"Optional ask-chain id from an earlier answer: 32 hexadecimal "
			"digits. Notes are shared by repository path and loaded at the "
			"start of every ask, independently of this argument."));
	if (st->ep->background) {
		struct cstr_view const actions[] = { CSTR_VIEW("result"), CSTR_VIEW("cancel") };
		props = jb_set_or_free(props, "job", schema_property("string",
			"The job id returned by this tool. Give job instead of prompt, "
			"system or thread to check its status or read its completed result."));
		props = jb_set_or_free(props, "action", jb_object(
			"type", json_object_new_string("string"),
			"enum", schema_names(actions, ARRAY_SIZE(actions)),
			"default", json_object_new_string("result"),
			"description", json_object_new_string(
				"With job: result checks its status or returns its completed "
				"answer; cancel stops the job. Omit action to use result."),
			nullptr));
	}
	return jb_object(
		"name", json_object_new_string(st->tool->name),
		"title", json_object_new_string("Ask AI"),
		"description", schema_text3(st->tool->description, st->ep->background
			? "\n\nWith prompt, start a job and return its id immediately. The job "
			  "keeps running after this connection closes. Later call this same "
			  "tool with job and no prompt to check its status or read its result; "
			  "action cancel stops it. Results are kept for up to an hour after "
			  "completion. Set background false to wait for the answer on this "
			  "connection."
			: "", ""),
		"inputSchema", schema_object(props, st->ep->background
		                                    ? json_object_new_array()
		                                    : jb_list(json_object_new_string("prompt"))),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(!st->ep->background),
			"openWorldHint", json_object_new_boolean(1),
			nullptr),
		nullptr);
}

static struct json_object *
mcp_task_definition (void const *service)
{
	(void)service;
	static_assert(SPIN_MAX == 100'000 && TASK_SLICE_NS == 250'000,
	              "the description says");
	return jb_object(
		"name", json_object_new_string("test_task"),
		"title", json_object_new_string("Test task"),
		"description", json_object_new_string(
			"A test aid: CPU work of the slices asked for, 250 us each, run "
			"as a task on the server's event loops, which says how many "
			"slices ran and how often the task moved between loops; or, "
			"given a prompt instead, a question to the upstream AI as "
			"background work, which interactive calls go before."),
		"inputSchema", jb_object(
			"type", json_object_new_string("object"),
			"properties", jb_object(
				"slices", jb_set_or_free(schema_count(1, nullptr), "maximum",
				                         json_object_new_int(SPIN_MAX)),
				"prompt", schema_property("string", nullptr),
				nullptr),
			"additionalProperties", json_object_new_boolean(0),
			nullptr),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(1),
			"openWorldHint", json_object_new_boolean(0),
			nullptr),
		nullptr);
}

/* Returns the definition of the compact tool. Only turns get this tool.
 * @p service is the mcp_static. Its lending member says whether the turns
 * also get findings.
 */
static struct json_object *
mcp_compact_definition (void const *service)
{
	struct mcp_static const *st = service;
	static_assert(SUMMARY_MAX == 4 << 20, "the description says");
	return jb_object(
		"name", json_object_new_string("compact"),
		"description", schema_text3(
			"Free your context: the conversation since the task, your turns and "
			"the tools' results, is replaced by your summary of it, and your notes",
			st->lending.of[KIND_FINDINGS] ? " and askai's review record are" : " are",
			" shown again; the task stays as the user gave it. Note first what "
			"you must keep. Call it when askai says that the context fills up, "
			"or once you are done with a part of the work whose details you no "
			"longer need. Calls after it in the same turn do not run."),
		"inputSchema", schema_object(
			jb_object(
				"summary", schema_property("string",
					"Where the task stands: what is done, what you found, "
					"and what you will do next; at most 4 MiB."),
				nullptr),
			jb_list(json_object_new_string("summary"))),
		nullptr);
}

/* The definition of background as the turns get it; nullptr if out of
 * memory.
 */
static struct json_object *
mcp_background_definition (void const *service)
{
	(void)service;
	static_assert(JOBS_MAX == 8, "the description says");
	return jb_object(
		"name", json_object_new_string("background"),
		"description", json_object_new_string(
			"Start a call of another tool as a job in the background, and go on "
			"at once: askai answers with the job's number, and tells you when it "
			"finishes: with a later tool result, by ending a wait, or in a "
			"message after your answer. Then read its output with jobs. Use it "
			"for calls that take long, while you do other work; at most 8 jobs "
			"a task."),
		"inputSchema", schema_object(
			jb_object(
				"tool", schema_property("string", "The name of the tool to call, "
				                        "such as git or man."),
				"arguments", schema_property("object",
					"Its arguments, as you would give them to it."),
				nullptr),
			jb_list(json_object_new_string("tool"))),
		nullptr);
}

/* Returns the actions of the jobs tool as a JSON array, or nullptr if out
 * of memory.
 */
static struct json_object *
mcp_jobs_actions (void)
{
	struct cstr_view const names[] = { CSTR_VIEW("list"), CSTR_VIEW("result"),
	                                   CSTR_VIEW("cancel") };
	return schema_names(names, ARRAY_SIZE(names));
}

/* The definition of jobs as the turns get it, likewise. */
static struct json_object *
mcp_jobs_definition (void const *service)
{
	(void)service;
	return jb_object(
		"name", json_object_new_string("jobs"),
		"description", json_object_new_string(
			"The jobs you started with background: list them, read the output "
			"of one that has finished, a window at a time, or cancel one that "
			"runs."),
		"inputSchema", schema_object(
			jb_object(
				"action", schema_enum(mcp_jobs_actions(),
					"list the jobs, read a job's result, or cancel it."),
				"job", schema_property("integer", "For result and cancel: the "
				                       "job's number."),
				"offset", schema_property("integer", "For result: the byte of the "
				                          "output to start from, to read on."),
				nullptr),
			jb_list(json_object_new_string("action"))),
		nullptr);
}

/* The definition of wait as the turns get it, likewise. */
static struct json_object *
mcp_wait_definition (void const *service)
{
	(void)service;
	static_assert(WAIT_MAX == 600, "the description says");
	return jb_object(
		"name", json_object_new_string("wait"),
		"description", json_object_new_string(
			"Wait until a job finishes, of those you name or any, or a timer "
			"fires, or the seconds are up, whichever comes first; askai then "
			"says what happened. Call it rather than jobs again and again."),
		"inputSchema", schema_object(
			jb_object(
				"seconds", schema_property("integer", "The longest to wait: 1 to "
				                           "600."),
				"jobs", jb_object(
					"type", json_object_new_string("array"),
					"items", schema_property("integer", nullptr),
					"description", json_object_new_string(
						"The jobs to wait for; any if none."),
					nullptr),
				nullptr),
			jb_list(json_object_new_string("seconds"))),
		nullptr);
}

/* Returns the definition of timer for the turns, or nullptr if out of
 * memory.
 */
static struct json_object *
mcp_timer_definition (void const *service)
{
	(void)service;
	static_assert(TIMER_MAX == 3600 && LEDGER_NOTE == 200, "the description says");
	return jb_object(
		"name", json_object_new_string("timer"),
		"description", json_object_new_string(
			"Set a timer, and go on at once: when it fires, askai tells you, "
			"with a later tool result, by ending a wait, or in a message after "
			"your answer."),
		"inputSchema", schema_object(
			jb_object(
				"seconds", schema_property("integer", "When it fires: in 1 to 3600 "
				                           "seconds."),
				"note", schema_property("string", "What it is for, which askai "
				                        "says as it fires; at most 200 bytes."),
				nullptr),
			jb_list(json_object_new_string("seconds"))),
		nullptr);
}

/* Returns the definition of background as clients list it, with
 * --client-jobs. @p service is the mcp_static, which has the name of the
 * ask tool. Returns nullptr if out of memory.
 */
static struct json_object *
mcp_background_client_definition (void const *service)
{
	static_assert(LEDGER_RUNNING == 8 && LEDGER_KEEP == 3600, "the description says");
	struct mcp_static const *st = service;
	char about[1024];
	util_format(about, sizeof about, "Start a call of another of your tools, such as %s, "
	            "as a job in the background, and continue at once: the result contains "
	            "the id of the job. The job runs without this request and without your "
	            "connection. A call of wait ends when the job ends, and jobs returns "
	            "the result of the job or cancels the job. At most 8 of your jobs and "
	            "tasks run at once, and askai stores the result of a job for up to an "
	            "hour after the job ends.", st->tool->name);
	char tool[256];
	util_format(tool, sizeof tool, "The name of the tool to call, such as %s; not "
	            "background, jobs, wait or timer.", st->tool->name);
	return jb_object(
		"name", json_object_new_string("background"),
		"description", json_object_new_string(about),
		"inputSchema", schema_object(
			jb_object(
				"tool", schema_property("string", tool),
				"arguments", schema_property("object", "The arguments for the tool, "
				                             "the same as in a direct call."),
				nullptr),
			jb_list(json_object_new_string("tool"))),
		nullptr);
}

/* Returns the definition of jobs as clients list it, with --client-jobs;
 * nullptr if out of memory.
 */
static struct json_object *
mcp_jobs_client_definition (void const *service)
{
	(void)service;
	return jb_object(
		"name", json_object_new_string("jobs"),
		"description", json_object_new_string(
			"List your jobs, which background started, and your timers; return "
			"the result of a job that has ended; or cancel a job that runs or a "
			"timer that has not fired."),
		"inputSchema", schema_object(
			jb_object(
				"action", schema_enum(mcp_jobs_actions(),
					"list lists your jobs and timers, result returns the "
					"result of a job, and cancel cancels a job or a timer."),
				"job", schema_property("string", "The id of the job for result and "
				                       "cancel, from the result of background."),
				"timer", schema_property("integer", "The number of the timer for "
				                         "cancel, from the result of timer."),
				nullptr),
			jb_list(json_object_new_string("action"))),
		nullptr);
}

/* Returns the definition of wait as clients list it, with --client-jobs;
 * nullptr if out of memory.
 */
static struct json_object *
mcp_wait_client_definition (void const *service)
{
	(void)service;
	static_assert(WAIT_MAX == 600 && WAIT_IDS == 16, "the description says");
	return jb_object(
		"name", json_object_new_string("wait"),
		"description", json_object_new_string(
			"Wait until one of your jobs ends, of those that you name or any, or "
			"one of your timers fires, or the seconds pass, whichever comes "
			"first. The result then lists what happened. A wait uses no upstream "
			"slot: call it instead of calling jobs again and again. If your "
			"client has a timeout for requests, wait for fewer seconds than "
			"that."),
		"inputSchema", schema_object(
			jb_object(
				"seconds", schema_property("integer", "The longest time to wait, "
				                           "from 1 to 600 seconds."),
				"jobs", jb_object(
					"type", json_object_new_string("array"),
					"items", schema_property("string", nullptr),
					"description", json_object_new_string(
						"The ids of the jobs to wait for, at most 16; without "
						"ids, any job."),
					nullptr),
				nullptr),
			jb_list(json_object_new_string("seconds"))),
		nullptr);
}

/* Returns the definition of timer that clients list with --client-jobs, or
 * nullptr if out of memory.
 */
static struct json_object *
mcp_timer_client_definition (void const *service)
{
	(void)service;
	static_assert(TIMER_MAX == 3600 && LEDGER_NOTE == 200, "the description says");
	return jb_object(
		"name", json_object_new_string("timer"),
		"description", json_object_new_string(
			"Set a timer, and continue at once: the result contains the number of "
			"the timer. When the timer fires, a call of wait ends, and its result "
			"contains the number and the note of the timer."),
		"inputSchema", schema_object(
			jb_object(
				"seconds", schema_property("integer", "The seconds until the timer "
				                           "fires, from 1 to 3600."),
				"note", schema_property("string", "What the timer is for, at most "
				                        "200 bytes; the result of a wait contains it "
				                        "when the timer fires."),
				nullptr),
			jb_list(json_object_new_string("seconds"))),
		nullptr);
}

/* Serialize @p val into @p text, then free it. */
static int
mcp_serialize (struct dstr        *text,
               struct json_object *val)
{
	size_t len = 0;
	char const *json = val ? json_object_to_json_string_length(val, JB_WIRE, &len)
	                       : nullptr;
	int e = json && dstr_cat_n(text, json, len) ? 0 : ENOMEM;
	json_object_put(val);
	return e;
}

/* Serialize @p val into @p text after a comma, an item of a list, then free
 * it.
 */
static int
mcp_static_item (struct dstr        *text,
                 struct json_object *val)
{
	if (dstr_cat_n(text, ",", 1))
		return mcp_serialize(text, val);
	json_object_put(val);
	return ENOMEM;
}

/* Make @p result a modern list result: cacheable, complete, with our _meta. */
static struct json_object *
mcp_modern_list (struct json_object *result,
                 struct json_object *info)
{
	result = jb_set_or_free(result, "ttlMs", json_object_new_int(CACHE_TTL_MS));
	result = jb_set_or_free(result, "cacheScope", json_object_new_string("public"));
	result = jb_set_or_free(result, "resultType", json_object_new_string("complete"));
	return jb_set_or_free(result, "_meta", jb_object(meta_server, json_object_get(info),
	                                                 nullptr));
}

/** Pushes the supported versions onto the array @p list and serializes
 *  it. Then the function serializes the server/discover result and the
 *  tails of the result of initialize and of the modern results, with
 *  @p info, which describes askai. Returns 0, or the error of jb_push()
 *  or mcp_serialize(), or ENOMEM.
 */
static int
mcp_static_tails (struct mcp_static  *st,
                  struct json_object *info,
                  struct json_object *list)
{
	for (size_t i = 0; i < ARRAY_SIZE(versions); ++i) {
		int e = jb_push(list, jb_string(versions[i]));
		if (e)
			return e;
	}
	dstr_auto text = DSTR_INIT;
	int e = mcp_serialize(&text, json_object_get(info));
	if (e)
		return e;
	e = mcp_serialize(&st->versions, json_object_get(list));
	if (e)
		return e;
	struct json_object *discover = jb_object(
		"supportedVersions", json_object_get(list),
		"capabilities", jb_object(
			"tools", jb_object(nullptr),
			"extensions", jb_object(
				meta_tasks, jb_object(nullptr),
				meta_files, jb_object("maxInlineBytes", json_object_new_int64(
				                              (int64_t)st->files_inline), nullptr),
				nullptr),
			nullptr),
		nullptr);
	e = mcp_serialize(&st->discover, mcp_modern_list(discover, info));
	if (e)
		return e;
	if (!dstr_printf(&st->init_tail, "\",\"capabilities\":{\"tools\":{}},"
	                 "\"serverInfo\":%s}", dstr_get(&text)) ||
	    !dstr_printf(&st->server_meta, "\"_meta\":{\"%s\":%s}", meta_server,
	                 dstr_get(&text)) ||
	    !dstr_printf(&st->modern_tail, ",\"resultType\":\"complete\",%s}",
	                 dstr_get(&st->server_meta)))
		return ENOMEM;
	/* tools/list lists the tools that the role of the caller grants, so a
	 * cache must not serve the list of one caller to another caller. */
	return dstr_printf(&st->list_tail, "],\"ttlMs\":%d,\"cacheScope\":\"private\"%s",
	                   CACHE_TTL_MS, dstr_get(&st->modern_tail)) ? 0 : ENOMEM;
}

/* mcp_host_plugin is the plugin of askai's own kinds. It has no instances
 * and no part_create(). Its service is the mcp_static, and mcp_create()
 * uses the service as the plugin's part on every loop. It is the plugin
 * at index 0 of mcp_static.plugins.
 */
static struct plugin const mcp_host_plugin = {};

static struct mcp_row const mcp_rows[KINDS] = {
#define N(s) .name = CSTR_VIEW_INIT(s)
	[KIND_ASK]        = { { .definition = mcp_tool_definition, .start = mcp_slot_ask,
	                        .step = mcp_slot_ask_step, .drop = notes_tool_drop },
	                      .metric = LENT_NONE },
	[KIND_MAN]        = { .plugin = &manual_plugin, N("man"), .metric = LENT_MAN,
	                      .option = "--man" },
	[KIND_TASK]       = { { .definition = mcp_task_definition, .start = mcp_slot_task,
	                        .drop = mcp_slot_task_drop },
	                      N("test_task"), LEND_NEVER, LENT_NONE },
	[KIND_GIT]        = { .plugin = &git_plugin, N("git"), .metric = LENT_GIT,
	                      .option = "--git PATH" },
	[KIND_NOTES]      = { .plugin = &notes_tool_plugin,
	                      .upstream = notes_tool_upstream_definition,
	                      N("notes"), LEND_ALWAYS, LENT_NOTES, TRACE_ACTION,
	                      .sized = "text", .option = "--notes DIR" },
	[KIND_COMPACT]    = { { .definition = mcp_compact_definition, .start = mcp_slot_compact,
	                        .flags = KF_SERIAL },
	                      N("compact"), LEND_WITH, LENT_COMPACT, TRACE_NAME,
	                      .sized = "summary" },
	[KIND_FINDINGS]   = { { .definition = findings_definition, .start = findings_start,
	                        .step = findings_step, .drop = findings_drop,
	                        .flags = KF_SERIAL | KF_RERUN | TOOL_PAGED },
	                      N("findings"), LEND_WITH, LENT_FINDINGS, TRACE_ACTION,
	                      .sized = "claim" },
	/* With --client-jobs, clients list the next four kinds with a
	 * definition of their own. */
	[KIND_BACKGROUND] = { { .definition = mcp_background_client_definition,
	                        .start = mcp_slot_background, .flags = KF_AGENDA },
	                      .upstream = mcp_background_definition,
	                      N("background"), LEND_DETACH, LENT_BACKGROUND,
	                      .option = "--client-jobs" },
	[KIND_JOBS]       = { { .definition = mcp_jobs_client_definition,
	                        .start = mcp_slot_jobs,
	                        .flags = TOOL_PAGED | KF_SERIAL | KF_AGENDA },
	                      .upstream = mcp_jobs_definition,
	                      N("jobs"), LEND_DETACH, LENT_JOBS, .option = "--client-jobs" },
	[KIND_WAIT]       = { { .definition = mcp_wait_client_definition,
	                        .start = mcp_slot_wait, .step = mcp_slot_wait_step,
	                        .drop = mcp_slot_wait_drop,
	                        .flags = TOOL_STREAM | KF_SERIAL | KF_AGENDA },
	                      .upstream = mcp_wait_definition,
	                      N("wait"), LEND_WITH, LENT_WAIT, .option = "--client-jobs" },
	[KIND_TIMER]      = { { .definition = mcp_timer_client_definition,
	                        .start = mcp_slot_timer, .flags = KF_AGENDA },
	                      .upstream = mcp_timer_definition,
	                      N("timer"), LEND_WITH, LENT_TIMER, .option = "--client-jobs" },
	[KIND_NONE]       = { { .start = mcp_slot_none },
	                      N("an unknown tool"), LEND_NEVER, LENT_NONE },
#undef N
};

/** Serializes the replies that never change. @p info describes askai, and
 *  @p list is the array of the supported versions. The function serializes
 *  the definition for tools/list of each tool that a role can grant, and
 *  the definition in the API of the upstream of each tool that can be lent,
 *  from the upstream function of its row if the row defines one.
 */
static int
mcp_static_build (struct mcp_static  *st,
                  struct json_object *info,
                  struct json_object *list)
{
	struct mcp_lending const *l = &st->lending;
	tool_mask lendable = l->by[LEND_GRANTED] | l->by[LEND_ALWAYS] | l->by[LEND_WITH] |
	                     l->by[LEND_DETACH];
	for (uint32_t id = 0; id < st->n_offers; ++id) {
		struct mcp_offer *o = &st->offers[id];
		/* definition() of a plugin that askai loaded from a file receives
		 * the configuration of the plugin. */
		struct mcp_plugin const *p = &st->plugins[o->tool.part];
		void const *service = p->entry.loaded ? p->entry.loaded->config : p->service;
		struct json_object *def = o->tool.kind->definition(service);
		int e = !def ? ENOMEM : id < st->n_granted
		      ? mcp_static_item(&o->listed, json_object_get(def)) : 0;
		if (!e && lendable >> id & 1) {
			tool_definition_fn *upstream = mcp_row_of(&o->tool)->upstream;
			struct json_object *lent = upstream ? upstream(service)
			                                    : json_object_get(def);
			e = lent ? mcp_static_item(&o->lent, ai_tool(st->ep->api, lent)) : ENOMEM;
			json_object_put(lent);
		}
		json_object_put(def);
		if (e)
			return e;
	}
	return mcp_static_tails(st, info, list);
}

/* Returns the lending rule of the registry tool @p t. A tool of askai's own
 * kinds has the rule of its row, and so does a tool whose row has a rule
 * other than LEND_NEVER, such as notes. Any other tool of a plugin, built
 * in or loaded, has LEND_GRANTED if its kind has TOOL_LENT, and LEND_NEVER
 * otherwise.
 */
static enum mcp_lend
mcp_lend_of (struct registry_tool const *t)
{
	struct mcp_row const *row = mcp_row_of(t);
	if (t->kind == &row->kind || row->lend != LEND_NEVER)
		return row->lend;
	return t->kind->flags & TOOL_LENT ? LEND_GRANTED : LEND_NEVER;
}

/* Returns the masks of the tools in the registry of @p cfg that askai lends
 * the turns. All masks are empty without --upstream-tools. A mask holds only
 * tools in cfg->tools_upstream.
 */
static struct mcp_lending
mcp_lending (struct config const *cfg)
{
	struct mcp_lending l = {};
	if (!(cfg->flags & CFG_UPSTREAM_TOOLS))
		return l;
	struct registry const *reg = &cfg->registry;
	for (uint32_t id = 0; id < reg->n_tools; ++id) {
		struct registry_tool const *t = &reg->tools[id];
		tool_mask bit = (tool_mask)1 << id & cfg->tools_upstream;
		l.by[mcp_lend_of(t)] |= bit;
		l.detach |= t->kind->flags & TOOL_DETACH ? bit : 0;
		if (t->data)
			l.of[(struct mcp_row const *)t->data - mcp_rows] |= bit;
	}
	return l;
}

/* The kinds background, jobs, wait and timer, as bits. */
static constexpr uint32_t KINDS_AGENDA = 1u << KIND_BACKGROUND | 1u << KIND_JOBS |
                                         1u << KIND_WAIT | 1u << KIND_TIMER;

/* The kinds compact and findings, which only the turns get, as bits. */
static constexpr uint32_t KINDS_TURNS = 1u << KIND_COMPACT | 1u << KIND_FINDINGS;

/* The kinds of askai's own tools that @p cfg offers to clients, as bits. */
static uint32_t
mcp_offered_clients (struct config const *cfg)
{
	return 1u << KIND_ASK | (cfg->flags & CFG_MAN ? 1u << KIND_MAN : 0) |
	       (cfg->flags & CFG_TEST_TASK ? 1u << KIND_TASK : 0) |
	       (cfg->n_gits ? 1u << KIND_GIT : 0) | (cfg->notes ? 1u << KIND_NOTES : 0) |
	       (cfg->flags & CFG_CLIENT_JOBS ? KINDS_AGENDA : 0);
}

/* The kinds of askai's own tools that @p cfg offers to clients or to the
 * turns, as bits.
 */
static uint32_t
mcp_offered (struct config const *cfg)
{
	uint32_t turns = KINDS_TURNS | KINDS_AGENDA;
	return mcp_offered_clients(cfg) | (cfg->flags & CFG_UPSTREAM_TOOLS ? turns : 0);
}

int
mcp_register (struct registry     *reg,
              struct config const *cfg)
{
	uint32_t offered = mcp_offered(cfg);
	uint32_t granted = mcp_offered_clients(cfg);
	/* registry_add() inserts each tool that a role can grant before the
	 * kinds in KINDS_TURNS. */
	for (enum mcp_kind k = 0; k < KIND_NONE; ++k) {
		if (!(offered >> k & 1))
			continue;
		struct mcp_row const *row = &mcp_rows[k];
		struct plugin const *plugin = &mcp_host_plugin;
		struct registry_tool tool = {
			.name = row->name.data ? row->name : cstr_view(cfg->tool.name),
			.kind = &row->kind,
			.data = row,
		};
		/* The man, git and notes tools are kinds of built-in plugins. */
		if (row->plugin) {
			plugin = row->plugin;
			tool.kind = plugin->tools;
		}
		int e = registry_add(reg, plugin, &tool,
		                     granted >> k & 1 ? REGISTRY_GRANTED : 0);
		if (e)
			return e;
	}
	return 0;
}

/** Returns true if the member tools of the configuration file of @p cfg
 *  allows clients the tool of @p reg with the name @p name, or if @p name
 *  is not the name of a tool that a role can grant.
 */
static bool
mcp_allowed (struct registry const *reg,
             struct config const   *cfg,
             struct cstr_view       name)
{
	for (uint32_t id = 0; id < reg->n_granted; ++id)
		if (cstr_view_equal(reg->tools[id].name, name))
			return cfg->tools_clients >> id & 1;
	return true;
}

/** Logs askai's own tools for the clients of @p cfg, each on or off, with
 *  the option that turns an off tool on, or "tools" if the member tools of
 *  the configuration file denies it. A tool without such an option, such
 *  as test_task, is not logged while it is off.
 */
static void
mcp_report_own (struct registry const *reg,
                struct config const   *cfg)
{
	uint32_t on = mcp_offered_clients(cfg);
	struct cstr_view const ask = cstr_view(cfg->tool.name);
	struct cstr_view names[KIND_NONE];
	int width = 0;
	for (enum mcp_kind k = 0; k < KIND_NONE; ++k) {
		names[k] = mcp_rows[k].name.data ? mcp_rows[k].name : ask;
		width = (int)names[k].len > width ? (int)names[k].len : width;
	}
	LOG_ALL(LOG_INFO, "built-in tools for clients:");
	for (enum mcp_kind k = 0; k < KIND_NONE; ++k) {
		char const *option = mcp_rows[k].option;
		if (on >> k & 1)
			LOG_ALL(LOG_INFO, "  %-*s  %s", width, names[k].data,
			        mcp_allowed(reg, cfg, names[k]) ? "on" : "off (tools)");
		else if (option)
			LOG_ALL(LOG_INFO, "  %-*s  off (%s)", width, names[k].data, option);
	}
}

/* Logs the git repositories of @p cfg, each with its name and path. */
static void
mcp_report_gits (struct config const *cfg)
{
	int width = 0;
	for (size_t i = 0; i < cfg->n_gits; ++i) {
		size_t len = 0;
		git_name(cfg->gits[i], &len);
		width = (int)len > width ? (int)len : width;
	}
	LOG_ALL(LOG_INFO, "git repositories:");
	for (size_t i = 0; i < cfg->n_gits; ++i) {
		size_t len = 0;
		char const *repo = git_name(cfg->gits[i], &len);
		LOG_ALL(LOG_INFO, "  %-*.*s  %s", width, (int)len, repo, cfg->gits[i]);
	}
}

/** Logs the plugin @p p of @p reg with its file, whether options were
 *  given to it, and its tools. A tool that the member tools of the
 *  configuration file of @p cfg denies to clients is marked "off (tools)".
 */
static void
mcp_report_plugin (struct registry const      *reg,
                   struct config const        *cfg,
                   struct plugin_loaded const *p)
{
	LOG_ALL(LOG_INFO, "plugin %s:", p->manifest.name);
	LOG_ALL(LOG_INFO, "  file     %s", p->path);
	LOG_ALL(LOG_INFO, "  options  %s", p->options ? "given" : "none");
	for (uint32_t k = 0; k < p->n_tools; ++k) {
		char const *name = p->manifest.tools[k];
		LOG_ALL(LOG_INFO, "  %-7s  %s%s", k ? "" : "tools", name,
		        mcp_allowed(reg, cfg, cstr_view(name)) ? "" : "  off (tools)");
	}
}

void
mcp_report (struct registry const *reg,
            struct config const   *cfg)
{
	if (!reg || !cfg)
		return;
	mcp_report_own(reg, cfg);
	if (cfg->n_gits)
		mcp_report_gits(cfg);
	if (cfg->notes)
		LOG_ALL(LOG_INFO, "notes directory: %s", cfg->notes);
	uint32_t loaded = 0;
	for (uint32_t i = 0; i < reg->n_plugins; ++i) {
		struct plugin_loaded const *p = reg->plugins[i].loaded;
		if (!p)
			continue;
		mcp_report_plugin(reg, cfg, p);
		++loaded;
	}
	if (!loaded)
		LOG_ALL(LOG_INFO, "plugins: none");
	if (!(cfg->flags & CFG_UPSTREAM_TOOLS))
		LOG_ALL(LOG_INFO, "upstream tools: off (--upstream-tools)");
}

tool_mask
mcp_lent (void const *cfg,
          tool_mask   tools,
          tool_mask   upstream)
{
	/* mcp_register() adds the ask tool first, with the id 0. */
	if (!cfg || !(tools & 1))
		return 0;
	struct mcp_lending l = mcp_lending(cfg);
	return mcp_lending_lent(&l, upstream);
}

/** Returns the record of the plugin of the registry entry @p entry for
 *  @p st. The record of a built-in plugin also contains its service and
 *  its two part functions. The service of askai's own kinds is @p st, the
 *  service of the plugin of the man tool is @p manual, the service of the
 *  plugin of the git tool is @p git, and the service of the plugin of the
 *  notes tool is st->notes_tool. Each of these three plugins creates a part
 *  on every client loop.
 */
static struct mcp_plugin
mcp_static_plugin (struct mcp_static            *st,
                   struct registry_plugin const *entry,
                   struct manual                *manual,
                   struct git                   *git)
{
	struct mcp_plugin const own[] = {
		{ .entry.plugin = &mcp_host_plugin, .service = st },
		{ .entry.plugin = &manual_plugin, .service = manual,
		  .part_create = manual_loop_create, .part_destroy = manual_loop_destroy },
		{ .entry.plugin = &git_plugin, .service = git,
		  .part_create = git_loop_create, .part_destroy = git_loop_destroy },
		{ .entry.plugin = &notes_tool_plugin, .service = &st->notes_tool,
		  .part_create = notes_tool_loop_create, .part_destroy = notes_tool_loop_destroy },
	};
	for (size_t i = 0; i < ARRAY_SIZE(own); ++i)
		if (own[i].entry.plugin == entry->plugin)
			return own[i];
	return (struct mcp_plugin){ .entry = *entry };
}

/** Prepares the attach action of the notes tool if the upstream of @p st
 *  accepts files (ai_upload_url()). The caller calls this function only
 *  with --upstream-tools and --notes. The URL of the uploads is on the host
 *  of the URL of the upstream, so that the key goes to no other host. The
 *  header lines of the uploads are the lines of the upstream and of its
 *  upload API. The function also creates the table of the files. Returns
 *  0, ENOMEM, or the error of uploads_create().
 */
static int
mcp_static_uploads (struct mcp_static *st)
{
	struct ai_endpoint const *ep = st->ep;
	struct ai_upload const *up = ai_upload(ep->api);
	if (!up || !(st->files_url = ai_upload_url(ep)))
		return 0;
	st->files_url_len = strlen(st->files_url);
	static char const *const none[] = { nullptr };
	char const *const *headers = ep->headers ? ep->headers : none;
	size_t n = 0;
	size_t m = 0;
	while (headers[n])
		++n;
	while (up->lines[m])
		++m;
	st->files_lines = malloc((n + m + 1) * sizeof *st->files_lines);
	if (!st->files_lines)
		return ENOMEM;
	memcpy(st->files_lines, headers, n * sizeof *st->files_lines);
	memcpy(st->files_lines + n, up->lines, (m + 1) * sizeof *st->files_lines);
	int e = 0;
	st->uploads = uploads_create(&e);
	return e;
}

/* The bytes of a file that a client may hand askai inline with requests of
 * @p body bytes at most, base64 taking 4 bytes for 3.
 */
static size_t
mcp_files_inline (size_t body)
{
	size_t room = body > FILES_ROOM ? (body - FILES_ROOM) / 4 * 3 : 0;
	return room < FILES_INLINE ? room : FILES_INLINE;
}

struct mcp_static *
mcp_static_create (struct config const *cfg,
                   struct manual       *manual,
                   struct task_pool    *tasks,
                   struct git          *git,
                   struct notes        *notes,
                   struct ledger       *ledger,
                   struct roster       *roster,
                   struct web          *web,
                   struct journal      *journal,
                   int                 *e)
{
	struct registry const *reg = &cfg->registry;
	struct mcp_static *st = malloc(sizeof *st);
	if (!st) {
		*e = ENOMEM;
		return nullptr;
	}
	*st = (struct mcp_static){
		.ep = &cfg->ep,
		.tool = &cfg->tool,
		.offers[TOOL_MAX].tool = {
			.name = mcp_rows[KIND_NONE].name,
			.kind = &mcp_rows[KIND_NONE].kind,
			.data = &mcp_rows[KIND_NONE]
		},
		.n_offers = reg->n_tools,
		.n_granted = reg->n_granted,
		.n_plugins = reg->n_plugins,
		.list_tail = DSTR_INIT,
		.discover = DSTR_INIT,
		.init_tail = DSTR_INIT,
		.modern_tail = DSTR_INIT,
		.server_meta = DSTR_INIT,
		.versions = DSTR_INIT,
		.tasks = tasks,
		.journal = journal,
		.ledger = ledger,
		.roster = roster,
		.web = web,
		.files_inline = mcp_files_inline(cfg->http.limits.body),
		.seed = journal ? journal_seed(journal) : util_seed(),
		.lending = mcp_lending(cfg),
		.lim = cfg->mcp,
		.turns = cfg->turns
	};
	for (uint32_t i = 0; i < reg->n_plugins; ++i)
		st->plugins[i] = mcp_static_plugin(st, &reg->plugins[i], manual, git);
	/* A zeroed dstr is not an empty string (dstr.h). */
	for (uint32_t id = 0; id < reg->n_tools; ++id)
		st->offers[id] = (struct mcp_offer){ .listed = DSTR_INIT, .lent = DSTR_INIT,
		                                     .tool = reg->tools[id] };
	if (cfg->flags & CFG_UPSTREAM_TOOLS && notes && (*e = mcp_static_uploads(st))) {
		mcp_static_destroy(&st);
		return nullptr;
	}
	st->notes_tool = (struct notes_tool){ .notes = notes, .files_inline = st->files_inline,
	                                      .attach = st->uploads != nullptr };

	/* Built with json-c, which escapes as replies need, then kept as text. */
	struct json_object *info = jb_object("name", json_object_new_string("askai"),
	                                     "title", json_object_new_string("Ask AI"),
	                                     "version", json_object_new_string("0.1.0"),
	                                     nullptr);
	struct json_object *list = json_object_new_array();
	*e = info && list ? mcp_static_build(st, info, list) : ENOMEM;
	json_object_put(list);
	json_object_put(info);
	if (*e)
		mcp_static_destroy(&st);
	return st;
}

void
mcp_static_destroy (struct mcp_static **p_static)
{
	struct mcp_static *st = *p_static;
	if (!st)
		return;
	*p_static = nullptr;
	for (uint32_t id = 0; id < st->n_offers; ++id) {
		dstr_cleanup(&st->offers[id].listed);
		dstr_cleanup(&st->offers[id].lent);
	}
	dstr_cleanup(&st->list_tail);
	dstr_cleanup(&st->discover);
	dstr_cleanup(&st->init_tail);
	dstr_cleanup(&st->modern_tail);
	dstr_cleanup(&st->server_meta);
	dstr_cleanup(&st->versions);
	uploads_destroy(&st->uploads);
	free(st->files_url);
	free(st->files_lines);
	free(st);
}
