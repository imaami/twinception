/** @file MCP's Streamable HTTP transport and JSON-RPC semantics, over request
 *  streams (src/stream.h).
 *
 * Two protocol eras are served side by side.  Legacy clients (revisions up to
 * 2025-11-25, which includes llama.cpp) open with an initialize handshake;
 * modern clients (2026-07-28) are stateless and declare their version in each
 * request's _meta, mirrored into HTTP headers that must match the body.
 *
 * A request's body is copied and pre-scanned for its JSON's nesting, values,
 * object members and number lengths as it arrives, then parsed within the
 * step's share of work (loop.h), a slice a bulk step once it exceeds that, by
 * tokeners the loop reuses.  A large message is freed the same way, by its own
 * request after the reply has gone out; the request's stream ends only then,
 * so its client's next request waits for that, and memory for messages is
 * bounded by the requests under way.  An upstream answer is parsed and freed
 * in slices too.  Replies are built from fragments, the unchanging ones
 * serialized once in the shared, immutable mcp_static, so no json-c value is
 * shared between replies or loops, and go out a piece a step, each piece once
 * the transport has room for it.  A finished request is kept for reuse, its
 * buffers' storage given back to the loop's stash (src/buf.h) and that of its
 * answer and pre-scan kept, until memory pressure has it freed; the one that
 * ended last is reused first, so steady requests reuse the same few.  So are
 * jobs.
 *
 * A tool call's upstream request is a job that home runs (src/job.h): the
 * request writes the upstream body, then hands it to a job, names itself by a
 * handle (src/loop.h) and submits the job.  The job's completion looks the
 * request up by that handle, and is dropped if the request has gone; else the
 * request parses the answer as before, on this loop, and then lets go of the
 * job.  A request that goes while it waits cancels its job, freeing its
 * handle.
 *
 * A tool call answers on an event stream (SSE) when the client accepts one
 * and its era has them (all but 2024-11-05): id-less data: events, and a ':'
 * comment every keepalive seconds while the upstream works.  A call whose
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
 * cancels the call in every era.  The caller of a call can also cancel it
 * with notifications/cancelled, on any connection of any loop.
 * mcp_req_cancel() looks the call up in the roster (src/roster.h) by the
 * caller, the session and the request id.
 *
 * The man and git tools are the manual's (src/manual.h) and git's
 * (src/git.h), plugins built in (src/plugin.h), whose calls go through the
 * call API (src/mcp.h).
 *
 * A test_task call, when askai offers the test aid, runs CPU work of the
 * slices it asks for as a task (src/task.h), which may run on any client
 * loop, and waits for it, named by a handle likewise; a call that goes
 * cancels the task.
 *
 * With --client-jobs, a client's call of background runs a call of another
 * tool as a job.  The request answers the client with the job's id, and
 * then runs the call with a stand-in stream, which writes the reply into
 * the job's record in the ledger (src/ledger.h), as a call that becomes a
 * task does.  A client's timers and waits are in the ledger too.
 *
 * GET /metrics answers a trusted client (src/stream.h) with the metrics,
 * rendered a share a step (src/metrics.h).  Draining, tool calls that wait
 * for the upstream, and those yet to start, end with an error, which on an
 * event stream is its last event.
 */
#define _GNU_SOURCE /* memrchr() */
#include <errno.h>
#include <inttypes.h>
#include <json-c/printbuf.h>
#include <openssl/sha.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <time.h>

#include "ai.h"
#include "buf.h"
#include "child.h"
#include "config.h"
#include "datauri.h"
#include "dstr.h"
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
#include "plugin.h"
#include "policy.h"
#include "registry.h"
#include "roster.h"
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
	TIMERS_MAX   = LEDGER_TIMERS,  /* timers they may have set at once */
	WAIT_MAX     = 600,       /* seconds that a wait may take */
	WAIT_IDS     = 16,        /* jobs that a client's wait may name */
	TIMER_MAX    = 3600,      /* seconds that a timer may be set to */
	TIMER_NOTE   = LEDGER_NOTE,    /* bytes of a timer's note */
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
 * class.  The principal of a job's requests is the owner of the job's
 * record, so home can queue a request of each job of an owner that runs. */
static_assert((int)LEDGER_RUNNING <= (int)JOB_QUEUED, "home queues the jobs that run");

/* What git runs for in the notes (src/notes.h). */
enum mcp_fetch : uint16_t {
	FETCH_CREATE,      /* the notes tool's add: make the repository of a new
	                      thread */
	FETCH_CHECK,       /* the notes tool's add: check the name of the note */
	FETCH_ADD,         /* the notes tool's add */
	FETCH_LIST,        /* the notes tool's list */
	FETCH_READ,        /* the notes tool's read */
	FETCH_RESUME,      /* list the notes of the thread that an ask call goes
	                      on with */
	FETCH_COMPACT,     /* list the notes for compact */
	FETCH_FULL,        /* list the notes for a compaction of a context that
	                      filled up */
	FETCH_KIND = 7,    /* the mask of the values above */
	FETCH_NEWEST = 8,  /* a flag that is set with one of the last three
	                      values: read the newest note */
	FETCH_OTHER = 16,  /* a flag that is set with FETCH_LIST or FETCH_READ:
	                      the other side's notebook of the thread */
	FETCH_COPY = 32,   /* a flag that is set with FETCH_READ, then with the
	                      values of an add: a copy of a note of the other
	                      side's notebook into the caller's */
	FETCH_BOOKS = 128, /* lists the session's notebooks */
	FETCH_FORMAT = FETCH_BOOKS | FETCH_CHECK, /* object format for an unborn branch */
	FETCH_ATTACH = 64, /* a flag that is set with FETCH_READ: the attach
	                      action, which uploads the note as a file for the
	                      turn */
	FETCH_REPLACE = 256, /* replace an existing own note, retaining its history */
	FETCH_FILES = 512,   /* list the current tree, rather than its history */
	FETCH_REVISION = 1024, /* pin and authorize historical note revisions */
	FETCH_DIFF = 2048,    /* compare two authorized revisions */
	FETCH_RESTORE = 4096, /* restore historical bytes in a new own commit */
};

enum {
	NOTE_WRITE  = 1 << 0,
	NOTE_FILE   = 1 << 1,
	NOTE_OFFSET = 1 << 2,
	NOTE_UPLOAD = 1 << 3,
	NOTE_REVISION = 1 << 4,
};

/** The notes actions, shared by dispatch and the published tool schema. */
static struct {
	char const    *name;
	uint32_t       flags;
	enum mcp_fetch fetch;
	uint16_t       len;
} const mcp_notes_actions[] = {
#define NOTE(name, flags, fetch) { name, flags, fetch, sizeof name - 1 }
	NOTE("add",       NOTE_WRITE | NOTE_FILE, 0),
	NOTE("replace",   NOTE_WRITE | NOTE_FILE, FETCH_REPLACE),
	NOTE("restore",   NOTE_WRITE | NOTE_FILE | NOTE_REVISION, FETCH_READ | FETCH_RESTORE),
	NOTE("notebooks", NOTE_OFFSET,            FETCH_BOOKS),
	NOTE("list",      NOTE_OFFSET,            FETCH_LIST | FETCH_FILES),
	NOTE("history",   NOTE_OFFSET,            FETCH_LIST),
	NOTE("read",      NOTE_FILE | NOTE_OFFSET | NOTE_REVISION, FETCH_READ),
	NOTE("diff",      NOTE_FILE | NOTE_OFFSET, FETCH_READ | FETCH_DIFF),
	NOTE("copy",      NOTE_FILE,              FETCH_READ | FETCH_OTHER | FETCH_COPY),
	NOTE("attach",    NOTE_FILE | NOTE_UPLOAD, FETCH_READ | FETCH_ATTACH),
#undef NOTE
};

static constexpr uint64_t PROGRESS_NS = 100'000'000;  /* between a call's
                                                         notifications */
static constexpr uint64_t TASK_NS = 1'000'000'000;  /* a call's run before it
                                                       becomes a task, for a
                                                       client that takes them */

/** Newest first; the first MODERN revisions carry their version in _meta.
 *  The last, PRE_STREAMS, is the revision before Streamable HTTP, whose
 *  clients take no event stream. */
static char const versions[][sizeof "2026-07-28"] = {
	"2026-07-28",
	"2025-11-25",
	"2025-06-18",
	"2025-03-26",
	"2024-11-05",
};
enum { MODERN = 1, PRE_STREAMS = ARRAY_SIZE(versions) - 1 };

static char const meta_version[] = "io.modelcontextprotocol/protocolVersion";
static char const meta_caps[] = "io.modelcontextprotocol/clientCapabilities";
static char const meta_server[] = "io.modelcontextprotocol/serverInfo";
static char const meta_tasks[] = "io.modelcontextprotocol/tasks";
/* The extension by which a client hands askai a file that a model does not
 * write: a data: URI in an argument that x-mcp-file marks. */
static char const meta_files[] = "io.modelcontextprotocol/files";

/* The bytes of the files that one request may hand askai inline, at most,
 * once decoded (maxInlineBytes of the extension), if --max-body has room
 * for them in base64 and FILES_ROOM bytes of the rest of the request. */
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

/* The kinds of tool that askai may offer, each its row of mcp_rows.  A role
 * can grant the kinds up to KIND_NOTES, which clients list and call.  askai
 * offers KIND_COMPACT only to the turns of ask calls.  It offers the four
 * kinds after KIND_COMPACT to the turns, with --upstream-tools, and with
 * --client-jobs also to clients, as a role grants them.  A kind's tool has
 * one id for both, and the start() of such a kind checks the role of its
 * slot: ROLE_OWN is a client's call.  KIND_NONE is the kind of a tool that
 * askai does not offer. */
enum mcp_kind : uint32_t {
	KIND_ASK,
	KIND_MAN,
	KIND_TASK,
	KIND_GIT,
	KIND_NOTES,
	KIND_COMPACT,
	KIND_BACKGROUND,
	KIND_JOBS,
	KIND_WAIT,
	KIND_TIMER,
	KIND_NONE,
	KINDS
};

/* The kinds of object that the handles of a client loop name
 * (loop_handle()).  loop_lookup() returns an object only for the kind that
 * its handle was made with, so a handle of a request never gives a call
 * slot, and the reverse. */
enum mcp_handle : uint32_t {
	HANDLE_REQ = 1,  /* a struct mcp_req */
	HANDLE_SLOT,     /* a struct mcp_slot */
};

/* How a kind of tool is lent to the turns: never; as the client's role
 * grants it; whenever the turns are lent anything; beside another; or
 * beside one that may run in the background. */
enum mcp_lend : uint8_t {
	LEND_NEVER,
	LEND_GRANTED,
	LEND_ALWAYS,
	LEND_WITH,
	LEND_DETACH,
};

/* What the trace tells of a call besides its tool's name: its arguments'
 * JSON text; of a notes call's, which are the upstream's own, only the
 * action; or nothing. */
enum mcp_trace : uint8_t {
	TRACE_JSON,
	TRACE_ACTION,
	TRACE_NAME,
};

/* A flag of askai's own kinds, past TOOL_FLAGS: a turn's call of it keeps
 * to itself, running after the calls before it are written, and those after
 * it after it is. */
static constexpr uint64_t KF_SERIAL = 1ull << 32;

/* A flag of askai's own kinds, past TOOL_FLAGS: the kinds of background,
 * jobs, wait and timer.  A client's call of background cannot start a call
 * of such a kind, and a client's call of such a kind never becomes a task. */
static constexpr uint64_t KF_AGENDA = 1ull << 33;

/* A tool that askai offers: its definition as tools/list lists it, if a
 * role may grant it, and as the upstream's API has it, if it is lent, each
 * after a comma; its kind; its name; the argument whose value is logged as
 * its size; the function of its definition as the turns get it, if its kind
 * has one of its own for them; its plugin's part; how the turns are lent it;
 * its row in the turns' metrics; and what the trace shows of its calls. */
struct mcp_offer {
	struct dstr             listed;
	struct dstr             lent;
	struct tool_kind const *kind;
	char const             *name;
	char const             *sized;   /* or nullptr */
	tool_definition_fn     *upstream;
	uint32_t                len;
	uint8_t                 part;    /* its plugin's, in plugins[] and a
	                                    loop's parts[] */
	uint8_t                 lend;    /* enum mcp_lend */
	uint8_t                 metric;  /* enum metrics_lent */
	uint8_t                 trace;   /* enum mcp_trace */
};

/* The tools that askai lends the turns: by[LEND_GRANTED] as a role grants
 * them, by[LEND_ALWAYS] whenever the turns are lent anything, by[LEND_WITH]
 * beside those, and by[LEND_DETACH] beside one of the tools in detach,
 * which may run in the background.  compact is the bit of the tool compact
 * among them, if any. */
struct mcp_lending {
	tool_mask by[LEND_DETACH + 1];
	tool_mask detach;
	tool_mask compact;
};

/* Returns the tools that @p l lends the turns of an ask call of a client
 * whose role lets the upstream call @p upstream: those of them that are in
 * by[LEND_GRANTED], and those in by[LEND_ALWAYS].  If there are any, the
 * result also has the tools in by[LEND_WITH], and if one of them can run in
 * the background, the tools in by[LEND_DETACH]. */
static tool_mask
mcp_lending_lent (struct mcp_lending const *l,
                  tool_mask                 upstream)
{
	tool_mask lent = (upstream & l->by[LEND_GRANTED]) | l->by[LEND_ALWAYS];
	tool_mask more = lent & l->detach ? l->by[LEND_DETACH] : 0;
	return lent ? lent | l->by[LEND_WITH] | more : 0;
}

/* A plugin whose tools askai offers.  A built-in plugin can have a service,
 * the object that every loop shares, and a part of it on each client loop.
 * A plugin that askai loaded from a file has neither, and has its record
 * of the loading instead. */
struct mcp_plugin {
	struct plugin const        *plugin;
	void                       *service;
	mcp_part_create_fn         *part_create;
	mcp_part_destroy_fn        *part_destroy;
	struct plugin_loaded const *loaded;  /* nullptr for a built-in
	                                        plugin */
};

struct mcp_static {
	struct ai_endpoint const *ep;
	struct mcp_tool const    *tool;   /**< the ask tool's, as configured */
	struct mcp_offer          offers[TOOL_MAX + 1];  /**< by id; TOOL_MAX's,
	                                                      that of a tool askai
	                                                      does not offer */
	/** The plugins that have a tool, indexed by part.  There is at most
	 *  one plugin for each offer.  Entry 0 is the plugin of askai's own
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
	struct notes             *notes;        /**< the notes', with --notes */
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

/* A call of a tool that a request runs: the client's, or one of a turn's.
 * The request's own also holds its answer, and the notes' git that the
 * request reads for itself. */
struct mcp_slot {
	struct loop_step    step;      /* its work, its kind's step() */
	struct dstr         answer;    /* the tool's output; its storage stays
	                                  for reuse */
	struct mcp_req     *req;       /* whose */
	void               *own;       /* its kind's word for the call, which
	                                  its drop() lets go of: a man call's
	                                  page, a test_task call's task, a git
	                                  call's git */
	void               *inst;      /* The instance of the tool's plugin for
	                                  the user of the request, or nullptr
	                                  if the plugin has no instances.
	                                  mcp_slot_start() sets it with
	                                  instance_table_get(), and
	                                  mcp_slot_drop() clears it with
	                                  instance_table_put(). */
	struct span         lent;      /* its output, if own keeps it: a man
	                                  call's page's text */
	struct child       *child;     /* its notes' git, until its output is
	                                  taken */
	struct web_wait    *web;       /* its kind's last HTTP request, under
	                                  way or with its reply, until the
	                                  next request or mcp_slot_drop() */
	size_t              skip;      /* the byte of the output that its window,
	                                  given to a turn, starts at */
	size_t              window;    /* and the bytes of that */
	uint64_t            start;     /* loop_clock() as the call began */
	struct handle       call;      /* names it to the post that completes
	                                  it, as mcp_slot_name() makes it */
	struct json_object *args;      /* a job's arguments, shown by jobs */
	struct mcp_slot    *of;        /* the job whose output it gives, if any */
	uint16_t            note_len;  /* bytes of note */
	uint16_t            fetch;     /* enum mcp_fetch: what its notes' git is for */
	uint8_t             tool;      /* its call's tool, by its offer's id;
	                                  TOOL_MAX: none */
	uint8_t             role;      /* enum mcp_role */
	bool                done;      /* true once the call has ended.  A
	                                  call ends when it has its output,
	                                  when it fails, when it is
	                                  cancelled, and when askai calls
	                                  its kind's drop().  The functions
	                                  of the call API that would end or
	                                  change a call do nothing for an
	                                  ended call. */
	bool                failed;    /* the call failed */
	bool                cancelled; /* a job's call, cancelled */
	char                notebook[NOTES_REPO]; /* selected read branch; empty: own */
	char                note[511]; /* text appended to the tool output; fills
	                                  the slot's trailing alignment */
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
	struct mcp_splice    splice[2];  /* prompt and system, in draft order */
	struct mcp_splice   *history_splices; /* larger draft when chat history is given */
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
	struct mcp_slot     *slots;      /* those calls', JOB_CALLS of them, from
	                                    its first turn with calls to its end */
	struct mcp_agenda   *agenda;     /* its turns' jobs and timers, from the
	                                    first to its end */
	struct ledger_task  *task;       /* the record of the task or the job
	                                    that the call runs as, once it is
	                                    one */
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
	char const          *text;       /* the draft serialized */
	size_t               len;
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
	                                    client.  mcp_begin() copies it from
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
	uint16_t             splices;
	uint16_t             splice_max; /* capacity of the draft's splice array */
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
	                                             copied from its stream at
	                                             its head; "": anonymous */
	char                 call_args[SHOWN];  /* with --verbose, the arguments
	                                           of its tool call for the
	                                           log */
};

/* A job's archive id is also its ledger id, or a thread's id, and the
 * journal's: the same 32 hexadecimal digits. */
static_assert((int)LEDGER_ID == (int)NOTES_ID && (int)LEDGER_ID == (int)JOURNAL_ID &&
              sizeof ((struct job *)nullptr)->archive == LEDGER_ID + 1);

/* What an ask call's turns have under way besides themselves: the jobs
 * they started in the background, the timers they set, a wait call that
 * waits for those, what happened that the next result or message says, and
 * the answers they gave before what happened woke them. */
struct mcp_agenda {
	struct mcp_slot     jobs[JOBS_MAX];      /* by number - 1 */
	struct ledger_timer timers[TIMERS_MAX];  /* due 0: none set */
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
	struct list              calls;     /* requests with a tool call under way */
	struct list              tasks;     /* requests whose call is a task */
	struct list              freeing;   /* requests freeing their message */
	struct list              ending;    /* requests writing their end record */
	struct list              children;  /* the notes' gits */
	struct shelf             spare;     /* finished requests */
	struct shelf             jobs;      /* jobs let go of */
	struct loop_step         drain;     /* ends the calls, draining */
	struct stream_app        app;
	struct jb_pool           pool;      /* for request bodies */
	struct jb_pool           answers;   /* for upstream answers */
	struct metrics_src       metrics;   /* every loop's */
	struct metrics_mcp      *counts;
	struct loop             *loop;
	struct loop             *home;      /* where jobs run */
	struct mcp_static const *st;
	struct instance_table    instances; /* The instances of the plugins for
	                                       the users whose calls run on this
	                                       loop. */
	uint64_t                 asked;     /* ask calls begun, which the log
	                                       numbers */
	uint64_t                 draining;  /* bool, widened over the padding */
	void                    *parts[];   /* the plugins', as st->plugins[] */
};

/* The thread's MCP: the handler of jobs' completions finds it here. */
static thread_local struct mcp *self;

/* Tool output for a call refused for want of room (design section 10). */
static char const busy[] = "Too many tool calls are under way; try again in a "
                           "few seconds.";

/* A run of bytes: a piece of JSON text, or words for a client or the
 * model.  mcp_slot_say() and mcp_slot_result() take words as text,
 * whatever their flag. */
struct mcp_piece {
	char const *ptr;
	size_t      len;
	uint64_t    text;  /* bool, widened over the padding: as mcp_req_put()
	                      and mcp_req_upload() write the piece, text, which
	                      they escape into a JSON string, else JSON as it is */
};

#define FRAG(s) ((struct mcp_piece){ s, sizeof s - 1, false })
#define TEXT(s) ((struct mcp_piece){ s, sizeof s - 1, true })
#define DSTR(d) ((struct mcp_piece){ dstr_get(d), dstr_len(d), false })
#define BYTES(b) ((struct mcp_piece){ (b).ptr, (b).len, false })
#define ESC(p, n) ((struct mcp_piece){ (char const *)(p), (n), true })
#define RAW(p, n) ((struct mcp_piece){ (char const *)(p), (n), false })
#define SPAN(s) ((struct span){ (unsigned char *)(uintptr_t)(s), sizeof s - 1 })

/* Whether @p s, a field present, is the @p len bytes at @p str. */
static bool
mcp_span_is (struct span  s,
             char const  *str,
             size_t       len)
{
	return s.ptr && s.len == len && !memcmp(s.ptr, str, len);
}

/* Index into versions[], ARRAY_SIZE(versions) if @p v is unknown or absent. */
static size_t
mcp_version_index (char const *v,
                   size_t      len)
{
	if (len != sizeof *versions - 1)
		return ARRAY_SIZE(versions);
	size_t i = 0;
	while (i < ARRAY_SIZE(versions) && memcmp(versions[i], v, len))
		++i;
	return i;
}

static bool
mcp_is_legacy (char const *v,
               size_t      len)
{
	size_t i = mcp_version_index(v, len);
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

/* The enum metrics_method of @p method, of @p len bytes. */
static uint8_t
mcp_method_index (char const *method,
                  size_t      len)
{
#define M(s) { s, sizeof s - 1, false }
	static struct mcp_piece const names[METHOD_N] = {
		[METHOD_INITIALIZE] = M("initialize"),
		[METHOD_PING]       = M("ping"),
		[METHOD_TOOLS_LIST] = M("tools/list"),
		[METHOD_TOOLS_CALL] = M("tools/call"),
		[METHOD_DISCOVER]   = M("server/discover"),
		[METHOD_TASKS_GET]    = M("tasks/get"),
		[METHOD_TASKS_UPDATE] = M("tasks/update"),
		[METHOD_TASKS_CANCEL] = M("tasks/cancel"),
	};
#undef M
	uint8_t i = METHOD_N;
	while (--i && (!names[i].len || names[i].len != len ||
	               memcmp(method, names[i].ptr, len)))
		;
	return i;
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

/* The first @p max bytes of @p len at most, never ending within a character. */
static size_t
mcp_clip (char const *s,
          size_t      len,
          size_t      max)
{
	if (len <= max)
		return len;
	while (max && ((unsigned char)s[max] & 0xc0) == 0x80)
		--max;
	return max;
}

/* Write @p len bytes of @p p, from its byte @p off, into @p buf, as they
 * are or escaped, within @p max bytes of it. */
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
		buf_commit(buf, jb_escape((char *)s.ptr, p->ptr + off, len));
		return 0;
	}
	int e = buf_reserve(buf, len, max, &s);
	if (e)
		return e;
	memcpy(s.ptr, p->ptr + off, len);
	buf_commit(buf, len);
	return 0;
}

/* Free the message, or the answer, now if the step has the share for it,
 * else leave it to the request's steps, a few values each: freeing one takes
 * as long as parsing it. */
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

/* The bytes a finished request keeps for reuse. */
static uint64_t
mcp_req_kept (struct mcp_req const *req)
{
	return sizeof *req + dstr_get_block_count(&req->self.answer) * DSTR_BLK_SIZE +
	       dstr_get_block_count(&req->trace) * DSTR_BLK_SIZE +
	       req->scan.cap * sizeof *req->scan.members +
	       req->reaper.cap * sizeof *req->reaper.stack;
}

/* End the stream, if the request still has it, and keep the request for
 * reuse. */
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

/* A request to begin, the spare one that ended last if there is one;
 * nullptr if out of memory.  Its answer, pre-scan and reaper are empty, their
 * storage kept. */
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
 * free. */
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
	free(req);
}

static void mcp_on_job (struct loop *loop, struct msg *msg);

/* A job for a tool call: the one let go of last if there is one; nullptr if
 * out of memory. */
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
 * which the slots' storage keeps for reuse. */
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

/* "byte" or "bytes", for a count of @p n bytes in the log. */
static char const *
mcp_bytes (size_t n)
{
	return n == 1 ? "byte" : "bytes";
}

/* Write the @p len bytes at @p text into the @p size bytes at @p buf as a
 * JSON string, which is one line, followed by "..." if cut.  The string has
 * the longest prefix of whole characters whose escapes take at most @p max
 * bytes and fit in @p size bytes with the quotes, the mark and the NUL. */
static void
mcp_quote (char       *buf,
           size_t      size,
           char const *text,
           size_t      len,
           size_t      max)
{
	static constexpr size_t frame = sizeof "\"\"...";
	if (size < frame) {
		if (size)
			buf[0] = '\0';
		return;
	}
	size_t n = jb_escape_fit(text, len, max < size - frame ? max : size - frame);
	buf[0] = '"';
	size_t end = 1 + jb_escape(buf + 1, text, n);
	buf[end++] = '"';
	if (n < len) {
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

/* Append the @p n bytes at @p s, JSON text, or with @p text the inside of
 * a JSON string, which is escaped, to @p w, up to TRACE_ARGS bytes in
 * all, whole characters and whole escapes. */
static void
mcp_shown_put (struct mcp_shown *w,
               char const       *s,
               size_t            n,
               bool              text)
{
	if (w->cut || !n)
		return;
	size_t room = TRACE_ARGS - w->len;
	char escaped[TRACE_ARGS];
	if (text) {
		size_t k = jb_escape_fit(s, n, room);
		w->cut = k < n;
		n = jb_escape(escaped, s, k);
		s = escaped;
	}
	size_t k = mcp_clip(s, n, room);
	w->cut |= k < n;
	memcpy(w->buf + w->len, s, k);
	w->len += k;
}

/* Append @p v to @p w as compact JSON, as JB_WIRE writes it, until
 * TRACE_ARGS bytes are written.  The string value of the member @p sized
 * of an object, if not nullptr, is replaced by its size. */
static void
mcp_shown_value (struct mcp_shown   *w,
                 struct json_object *v,
                 char const         *sized)
{
	switch (json_object_get_type(v)) {
	case json_type_object: {
		bool first = true;
		mcp_shown_put(w, "{", 1, false);
		struct json_object_iter it;
		json_object_object_foreachC(v, it) {
			if (w->cut)
				return;
			/* A comma before each member but the first. */
			mcp_shown_put(w, &",\""[first], 2 - first, false);
			first = false;
			mcp_shown_put(w, it.key, strlen(it.key), true);
			mcp_shown_put(w, "\":", 2, false);
			/* A data: URI is a file, which the log shows by its size too. */
			bool string = json_object_is_type(it.val, json_type_string);
			bool by_size = string && ((sized && !strcmp(it.key, sized)) ||
			                          !strncasecmp(json_object_get_string(it.val),
			                                       "data:", sizeof "data:" - 1));
			if (!by_size) {
				mcp_shown_value(w, it.val, nullptr);
				continue;
			}
			char text[40];
			int len = json_object_get_string_len(it.val);
			int n = snprintf(text, sizeof text, "\"(%d %s)\"", len,
			                 mcp_bytes((size_t)len));
			if (n > 0 && n < (int)sizeof text)
				mcp_shown_put(w, text, (size_t)n, false);
		}
		mcp_shown_put(w, "}", 1, false);
		return;
	}
	case json_type_array: {
		size_t n = json_object_array_length(v);
		mcp_shown_put(w, "[", 1, false);
		for (size_t i = 0; i < n && !w->cut; ++i) {
			mcp_shown_put(w, ",", i != 0, false);
			mcp_shown_value(w, json_object_array_get_idx(v, i), nullptr);
		}
		mcp_shown_put(w, "]", 1, false);
		return;
	}
	case json_type_string:
		mcp_shown_put(w, "\"", 1, false);
		mcp_shown_put(w, json_object_get_string(v),
		              (size_t)json_object_get_string_len(v), true);
		mcp_shown_put(w, "\"", 1, false);
		return;
	default: {
		/* A scalar's text is short. */
		size_t len = 0;
		char const *json = json_object_to_json_string_length(v, JB_WIRE, &len);
		if (json)
			mcp_shown_put(w, json, len, false);
		return;
	}
	}
}

/* Write @p args, the arguments of a call of @p offer, into the SHOWN bytes
 * at @p buf for the log: compact JSON on one line, cut to TRACE_ARGS bytes
 * with "...", with the value of the argument that the offer names, a
 * note's text or compact's summary, replaced by its size; the text @p none
 * without arguments.  Only the bytes that are shown are written, however
 * large the arguments are. */
static void
mcp_args_shown (struct mcp_offer const *offer,
                struct json_object     *args,
                struct tool_text        none,
                char                    buf[static SHOWN])
{
	struct mcp_shown w = { .buf = buf };
	if (args)
		mcp_shown_value(&w, args, offer->sized);
	else
		mcp_shown_put(&w, none.ptr, none.len, false);
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

/* Logs, once, the end of the client's tool call of @p req, which @p how
 * names: the tool, the client, the time and the size of the output @p out;
 * the task or the job that the call runs as, if any; for an ask call its
 * turns and their tool calls; with --verbose its arguments; and if the call
 * @p failed, the text of @p out, if any.  A
 * failure is logged at LOG_WARN, and else the line is one of --verbose.
 * Each level has its own rate limit, so that the lines of --verbose do not
 * hold back the failures. */
static void
mcp_req_log_call (struct mcp_req *req,
                  char const     *how,
                  struct span     out,
                  bool            failed)
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
		mcp_quote(text, sizeof text, (char const *)out.ptr, out.len, ERROR_LOG);
	log_put(&sites[failed], "tool call %s from %s %s after %.1f s (%zu %s)%s%s%s%s%s%s",
	        mcp_slot_offer(&req->self)->name, mcp_req_client(req), how,
	        mcp_elapsed(req->self.start), out.len, mcp_bytes(out.len), record, ask,
	        *req->call_args ? "; arguments " : "", req->call_args, *text ? "; error " : "",
	        text);
}

/* Drops the call in @p slot.  If the call's own word is set, the call
 * counts as ended first, so the call API ignores an attempt of the kind's
 * drop() to end the call.  Then the function calls the kind's drop(), or it
 * logs a warning if the kind has no drop().  A request drops its own slot
 * between two turns too, and its own slot has no own word, so that slot
 * does not count as ended then.  Last, the function releases the call's
 * HTTP request and its reply, puts the call's instance back
 * (instance_table_put()), clears the lent output, destroys the child of
 * the call's notes, removes the call's step from the loop's queue and frees
 * the call's handle. */
static void
mcp_slot_drop (struct mcp_slot *slot)
{
	if (slot->own) {
		slot->done = true;
		struct mcp_offer const *offer = mcp_slot_offer(slot);
		if (offer->kind->drop)
			offer->kind->drop(slot);
		else
			LOG(LOG_WARN, "a call of %s set its own word, but its kind has no "
			    "drop()", offer->name);
	}
	slot->own = nullptr;
	web_wait_leave(&slot->web);
	instance_table_put(&slot->req->mcp->instances, &slot->inst);
	slot->lent = (struct span){};
	/* A git that writes notes runs to its end, because a kill could leave
	 * a lock file behind. */
	uint8_t kind = slot->fetch & FETCH_KIND;
	if (kind == FETCH_CREATE || kind == FETCH_ADD)
		child_release(&slot->child);
	else
		child_destroy(&slot->child);
	slot->fetch = 0;
	slot->notebook[0] = '\0';
	loop_unready(&slot->step);
	if (slot->call.bits)
		loop_forget(slot->req->mcp->loop, slot->call);
	slot->call = (struct handle){};
}

/* Let go of the request's job, if it has one: cancel it while the request
 * waits for it, freeing the request's handle, so that its completion finds
 * no request; else tell home the result has been read.  Its calls let go
 * of what they hold, as their kinds do, and of their handles likewise. */
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
 * ends it. */
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
 * their end record, if it is on either list. */
static void
mcp_req_unwait (struct mcp_req *req)
{
	if (req->hook.next) {
		list_del(&req->hook);
		req->hook = (struct list){};
	}
}

/* What the request waited for has come: free its handle, and take it off the
 * calls under way. */
static void
mcp_req_wake (struct mcp_req *req)
{
	mcp_req_forget(req);
	mcp_req_unwait(req);
}

/* The request waits for its tool's call: on the calls that a drain ends,
 * its stream told so, its message freed. */
static void
mcp_req_park (struct mcp_req *req)
{
	mcp_req_wait(req);
	req->s->ops->waiting(req->s);
	mcp_req_dispose(req);
	req->phase = PHASE_TOOL;
}

/* @p slot's call waits no more: its request, if the slot is its own; a
 * turn's call leaves it among the calls a drain ends. */
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
 * reply, in the record, is an error or not whole, else completed. */
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
 * drop the call's reference to the record. */
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
 * and frees the handle of the request that the record has.  After this, a
 * notifications/cancelled does not reach the call. */
static void
mcp_req_leave (struct mcp_req *req)
{
	if (!req->rostered)
		return;
	loop_forget(req->mcp->loop, req->rostered->who);
	roster_leave(req->mcp->st->roster, &req->rostered);
}

/* The reply is queued, or the stream gone: let go of everything at once but
 * the values still to free, and end once those are freed. */
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
	/* Neither a drain nor a notifications/cancelled reaches the call while
	 * its end is written.  A cancel may supersede a pending state, intent
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
	free(req->history_splices);
	req->history_splices = nullptr;
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
 * @p fields. */
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
 * reasoning independently of this transport. */
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
 * a bulk step, each once the transport has room for it; then finish. */
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
		if (!p[i].len)
			continue;
		int e = mcp_piece_write(&p[i], 0, p[i].len, &req->reply, SIZE_MAX);
		if (e)
			return e;
	}
	return 0;
}

/* Begin the reply: on an event stream, an event; then
 * '{"jsonrpc":"2.0","id":ID,', without the id if the request has none, and
 * @p key, '"result":' or '"error":'.  An empty key begins a reply whose key
 * and value are those that a record of the ledger has.  The reply of a
 * task's call begins with only @p key, and the record of the task has the
 * reply (mcp_req_show()). */
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
 * ends, and let go of what it waits for.  Send the reply, as JSON with
 * @p status and the header @p fields or as the stream's last event, then
 * finish the request; if making the reply failed with @p e, end the stream
 * with that instead. */
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
 * content already, and the @p n_data pieces at @p data, if any, its data. */
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
 * call that ends so is logged as failed. */
static void
mcp_req_error (struct mcp_req         *req,
               uint16_t                status,
               int                     code,
               struct mcp_piece        message,
               struct mcp_piece const *data,
               size_t                  n_data)
{
	mcp_req_log_call(req, "failed",
	                 (struct span){ (unsigned char *)(uintptr_t)message.ptr, message.len },
	                 true);
	mcp_req_close(req, status, SPAN(JSON_FIELD),
	              mcp_req_error_put(req, code, message, data, n_data));
}

/* End the call with an error reply: 503, Server is shutting down, for
 * ESHUTDOWN, as the server drains; 500, Out of memory, for any other error,
 * be it memory, a handle or a page's lookup the call could not have, or
 * text it could not format. */
static void
mcp_req_fail (struct mcp_req *req,
              int             e)
{
	/* The reply is being sent, or the request is being freed.  A job of the
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
 * it, and the header @p fields. */
static void
mcp_req_result (struct mcp_req         *req,
                struct mcp_piece const *result,
                size_t                  n,
                struct span             fields)
{
	int e = mcp_req_open(req, FRAG("\"result\":"));
	mcp_req_close(req, 200, fields, e ? e : mcp_req_put(req, result, n));
}

/* Begin a tool result with @p text; more of it may follow. */
static int
mcp_req_tool_open (struct mcp_req   *req,
                   struct mcp_piece  text)
{
	struct mcp_piece const p[] = {
		FRAG("{\"content\":[{\"type\":\"text\",\"text\":\""),
		ESC(text.ptr, text.len),
	};
	int e = mcp_req_open(req, FRAG("\"result\":"));
	return e ? e : mcp_req_put(req, p, ARRAY_SIZE(p));
}

/* Write into @p tail, of @p size bytes, how the turns ended, after what they
 * called: the calls left untold, whether they were stopped, and where the
 * notes are; returns its length. */
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
 * own.  A call without turns that get tools has no trace.  A client's call
 * of the notes tool has no turns, and its thread does not belong to an ask
 * call. */
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
		ESC(dstr_get(&req->trace), dstr_len(&req->trace)),
		ESC(tail, mcp_req_trace_tail(req, tail, sizeof tail)),
		FRAG("\"}"),
	};
	return mcp_req_put(req, p, ARRAY_SIZE(p));
}

/* End a tool result, its trace after its text, and send it.  The result of
 * a task's call ends without the modern tail, which mcp_req_show() adds for
 * a modern reader of the record. */
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

/* A tool result of @p text, short and ours, for @p slot's call; for a
 * turn's tool call, its output, which goes to the turn. */
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
		dstr_clear(&slot->answer);
		if (!dstr_cat_n(&slot->answer, text.ptr, text.len)) {
			mcp_req_fail(req, ENOMEM);
			return;
		}
		mcp_slot_done(slot, failed);
		return;
	}
	req->failed = failed;
	mcp_req_log_call(req, failed ? "failed" : "answered",
	                 (struct span){ (unsigned char *)(uintptr_t)text.ptr, text.len }, failed);
	int e = mcp_req_tool_open(req, text);
	if (e) {
		mcp_req_abort(req, e);
		return;
	}
	mcp_req_tool_close(req);
}

/* The output of @p slot's call: the job's whose output it gives, if any;
 * else what it lends, if anything; else what it read or built. */
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
 * the request holds the job, else its own call's. */
static struct span
mcp_req_output (struct mcp_req const *req)
{
	return req->job ? buf_peek(&req->job->result) : mcp_slot_output(&req->self);
}

/* Escape as much of the output into the reply as the step has the share
 * for; the rest goes on in bulk steps.  Then close the result, which lets
 * go of the job, or the page, if the output was its. */
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

/* The output of @p slot's call is in: for a turn's tool call, the turn
 * takes it; else reply with it, a slice at a time, from the offset that a
 * man or git call asked for, a character's start, or an ask call's whole,
 * after the answers that its turns dozed after. */
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
	mcp_req_log_call(req, failed ? "failed" : "answered", text, failed);
	while (off < text.len && (text.ptr[off] & 0xc0) == 0x80)
		++off;
	req->failed = failed;
	req->phase = PHASE_REPLY;
	req->off = off;
	int e = mcp_req_tool_open(req, req->agenda ? DSTR(&req->agenda->said)
	                                           : (struct mcp_piece){});
	if (e) {
		mcp_req_abort(req, e);
		return;
	}
	mcp_req_reply(req);
}

/* Write a notification of progress @p value, saying the @p len bytes of
 * @p text, which need no escaping, into the reply.  A tool notification
 * identifies a call; other progress reports its current status. */
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
 * @p text, on the call's event stream. */
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
 * acknowledgment. Transport backpressure therefore also bounds the mailbox. */
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
		size_t n = mcp_clip(text + req->delta_read, header.len - req->delta_read,
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
 * turn.  Held back by output the client has yet to read, it goes in a later
 * step. */
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
 * the notification back. */
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
 * on_writable() for the last two. */
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
 * was sent; then the timer for the next of them.  A call that cannot
 * become a task goes on as it is. */
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
 * progressToken, and waits for its job. */
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
static void mcp_req_unfile (struct mcp_req *req);

/* Whether the @p len bytes at @p rest begin with what of the context
 * overflowed: its size, length or window. */
static bool
mcp_overflow_what (char const *rest,
                   size_t      len)
{
#define W(s) { s, sizeof s - 1 }
	static struct {
		char const *s;
		size_t      n;
	} const what[] = { W("size"), W("length"), W("window") };
#undef W
	for (size_t k = 0; k < ARRAY_SIZE(what); ++k)
		if (len >= what[k].n && !strncasecmp(rest, what[k].s, what[k].n))
			return true;
	return false;
}

/* Whether @p why, a failed turn's reason, says that the request did not
 * fit the upstream's context: llama-server's "exceeds the available
 * context size", OpenAI's "maximum context length", others' "context
 * window", and Gemini's "exceeds the maximum number of tokens allowed". */
static bool
mcp_overflowed (char const *why,
                size_t      len)
{
	static char const word[] = "context ";
	static char const gemini[] = "exceeds the maximum number of tokens";
	size_t n = sizeof word - 1;
	if (memmem(why, len, gemini, sizeof gemini - 1))
		return true;
	for (size_t i = 0; i + n <= len; ++i)
		if (!strncasecmp(why + i, word, n) &&
		    mcp_overflow_what(why + i + n, len - i - n))
			return true;
	return false;
}

/* Whether a turn that failed for @p why, @p len bytes, should start the
 * conversation again from the prompt: it did not fit the context, which
 * askai has not compacted since a turn went through, and turns went
 * before it. */
static bool
mcp_req_refit (struct mcp_req const *req,
               char const           *why,
               size_t                len)
{
	return req->turn > 1 && !req->forced && mcp_overflowed(why, len);
}

/* Logs, with --verbose, how the turn of @p req's ask call that @p job ran
 * ended: how, its HTTP status, its time and the size of its result.  The
 * line of a streamed answer also has the tokens that the turn used of the
 * context, its stop reason and its tool calls.  An answer that is not
 * streamed, such as the body of an HTTP error, has none of these. */
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
	char tokens[96];
	if (used && context)
		util_format(tokens, sizeof tokens, "%" PRIu64 " of %" PRIu64 " tokens (%" PRIu64
		            " %%)", used, context, used * 100 / context);
	else if (used)
		util_format(tokens, sizeof tokens, "%" PRIu64 " tokens of a context of "
		            "unknown size", used);
	else
		util_format(tokens, sizeof tokens, "tokens unknown");
	char reason[sizeof job->reason + 8] = "none";
	if (*job->reason)
		mcp_quote(reason, sizeof reason, job->reason, strlen(job->reason),
		          sizeof job->reason);
	uint32_t calls = 0;
	for (size_t k = 0; k < JOB_CALLS; ++k)
		calls += buf_peek(&job->calls[k]).len != 0;
	LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " turn %" PRIu32 ": answered, %s "
	    "after %.1f s, %s, stop reason %s, %" PRIu32 " tool call%s, %zu %s of text",
	    req->mcp->loop->id, req->asked, req->turn, http, mcp_elapsed(req->turn_at),
	    tokens, reason, calls, calls == 1 ? "" : "s", len, mcp_bytes(len));
}

/* The job the request waits for has ended, home done with its upstream
 * request and its key: parse the answer, a slice a step, or reply with what
 * home says of it.  A request whose turns go on keeps the upstream request,
 * its conversation.  A streamed answer ends a turn, and with it the call if
 * it calls no tool, as no answer to a call that lends none does; one that
 * did not fit the context starts the turns again from the prompt,
 * compacted. */
static void
mcp_req_job_done (struct mcp_req *req)
{
	struct job *job = req->job;
	struct span why = buf_peek(&job->result);
	mcp_req_log_turn(req, job);
	buf_fini(&job->key);
	/* The message's reference; the request keeps the front's. */
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
		/* A failed call's result says why, unless it never started. */
		if (!why.len) {
			mcp_req_error(req, 500, INTERNAL_ERROR,
			              FRAG("Cannot start the upstream request"), nullptr, 0);
			return;
		}
		mcp_req_unfile(req);
		if (mcp_req_refit(req, (char const *)why.ptr, why.len)) {
			mcp_req_drop(req);
			mcp_req_compact_full(req);
			return;
		}
	}
	/* Why the call failed is its output, which the job holds till dropped. */
	bool ok = dstr_cat_n(&req->self.answer, (char const *)why.ptr, why.len);
	mcp_req_drop(req);
	if (!ok) {
		mcp_req_fail(req, ENOMEM);
		return;
	}
	mcp_slot_respond(&req->self, true);
}

/*
 * A job's completion, or the job itself, handed back by home.  A completion
 * goes to the request that waits for it, which parses the answer a slice a
 * step, or replies with why there is none, and lets go of the job; one whose
 * request has gone is dropped, the request having let go as it went.  Once
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
 * mcp_req_build() escapes that into the upstream request a slice at a time. */
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
	struct mcp_splice *splices = req->history_splices ? req->history_splices : req->splice;
	splices[req->splices++] = (struct mcp_splice){ str, (size_t)pb->bpos - 1 };
	return 0;
}

/* Write the draft from where splice @p k - 1 goes to where splice @p k goes,
 * or to its end. */
static int
mcp_req_copy (struct mcp_req *req,
              size_t          k)
{
	struct mcp_splice const *splices = req->history_splices ? req->history_splices
	                                                     : req->splice;
	size_t from = k ? splices[k - 1].at : 0;
	size_t len = (k < req->splices ? splices[k].at : req->len) - from;
	if (!len)
		return 0;
	return mcp_piece_write(&RAW(req->text, req->len), from, len, &req->upload,
	                       SIZE_MAX);
}

/* Writes the call's session field and a line break into @p job's key, if
 * the request has a session field.  A field cannot contain a line break.
 * Home uses the key to tell principals apart.  Returns 0, or the error
 * number of buf_reserve(). */
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

/* Writes into @p key the key of a call in the roster.  @p id is the request
 * id: the id of @p req for its own call, or the id that a
 * notifications/cancelled in @p req names.  The key begins with the
 * principal of the client of @p req.  If the principal is 0, the client's
 * address follows it.  Then come the session field and a line break, if the
 * request has a session field, and last the JSON text of @p id.  Stores the
 * hash of the key in @p *hash.  The hash is seeded, so that no client can
 * choose keys that fill one bucket of the roster.  Returns 0, or ENOMEM. */
static int
mcp_req_key (struct mcp_req const *req,
             struct json_object   *id,
             struct buf           *key,
             uint64_t             *hash)
{
	struct stream const *s = req->s;
	uint64_t const who[3] = {
		s->principal, s->principal ? 0 : s->peer[0], s->principal ? 0 : s->peer[1]
	};
	struct span session = s->ops->field(s, F_SESSION);
	size_t len = 0;
	char const *json = json_object_to_json_string_length(id, JB_WIRE, &len);
	if (!json)
		return ENOMEM;
	size_t head = sizeof who + (session.ptr ? session.len + 1 : 0);
	struct span k;
	int e = buf_reserve(key, head + len, SIZE_MAX, &k);
	if (e)
		return e;
	memcpy(k.ptr, who, sizeof who);
	if (session.ptr) {
		memcpy(k.ptr + sizeof who, session.ptr, session.len);
		k.ptr[head - 1] = '\n';
	}
	memcpy(k.ptr + head, json, len);
	buf_commit(key, head + len);
	*hash = util_hash(req->mcp->st->seed, k.ptr, head + len);
	return 0;
}

/* The owner of the records in the ledger that @p req opens and reads: the
 * principal of its client, without the bit LEDGER_ANONYMOUS.  The principal
 * of an anonymous client is 0, and its owner is then a hash of its address
 * with the bit LEDGER_ANONYMOUS set.  The hash is the one that
 * mcp_req_submit() uses as the principal of an upstream job of an anonymous
 * client without a session field. */
static uint64_t
mcp_req_owner (struct mcp_req const *req)
{
	struct stream const *s = req->s;
	if (s->principal)
		return s->principal & ~LEDGER_ANONYMOUS;
	return util_hash(~req->mcp->st->seed, s->peer, sizeof s->peer) | LEDGER_ANONYMOUS;
}

/* A journal callback names a request rather than keeping its address: a
 * disconnect or shutdown may have ended it before the disk write ends. */
struct mcp_archive_wait {
	struct handle who;
	void (*done) (struct mcp_req *req, int error);
};

/* Name @p req by a handle in @p *who for a journal callback, which no other
 * may be waiting for: 0, EBUSY, or why the handle could not be made. */
static int
mcp_req_archive_wait (struct mcp_req *req,
                      struct handle  *who)
{
	return req->archive_call.bits ? EBUSY
	     : loop_handle(req->mcp->loop, req, HANDLE_REQ, who);
}

/* The request that @p who, the handle of a journal callback, names, its
 * handle freed; nullptr if the request has ended, or the loop ends. */
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
 * the durable private checkpoint; this control data does not enter Git. */
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
 * Foreground calls also become terminal, so their checkpoints can expire. */
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
		"ended", json_object_new_uint64(util_realtime()),
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
 * the job's completion. */
static int
mcp_req_name (struct mcp_req *req,
              struct job     *job)
{
	struct stream *s = req->s;
	int e = mcp_key(job, s->ops->field(s, F_SESSION));
	return e ? e : loop_handle(req->mcp->loop, req, HANDLE_REQ, &req->call);
}

/* Hand the upstream request to a job, which the request names itself to for
 * the job's completion, with the call's key and principal: its client's
 * identity, else its session, else its client's address; and submit the
 * job to home.  An identity's jobs are one principal's, whatever their
 * sessions and addresses: home tells them by their hash and a peer no
 * client has (UINT64_MAX twice; a Unix socket's user's is UINT64_MAX and
 * its uid). */
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
	job->deadline = ms > 0 ? loop->now + (uint64_t)ms * 1'000'000 : 0;
	uint64_t wall = req->archive_deadline ? util_realtime() : 0;
	if (wall)
		job->deadline = loop->now + (req->archive_deadline > wall
		                             ? req->archive_deadline - wall : 1);
	struct stream const *s = req->s;
	if (s->principal) {
		job->principal = s->principal;
		job->session = 0;
		job->peer[0] = job->peer[1] = UINT64_MAX;
	} else {
		job->peer[0] = s->peer[0];
		job->peer[1] = s->peer[1];
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
	/* The front's and home's, and the post adds the message's. */
	atomic_store_explicit(&job->refs, 2, memory_order_relaxed);
	req->job = job;
	req->phase = PHASE_CALL;
	job_post(job, mcp->home, &job->to_home, &job->home_bits, JOB_SUBMIT);
	return 0;
}

static void mcp_req_turn_ready (struct mcp_req *req);

/* Escape as much of the client's string under way into the upstream request
 * as the step has the share for, and once it is all in, the draft after it:
 * 0 once all is written; EAGAIN while more is to come; or ENOMEM. */
static int
mcp_req_build_slice (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	if (req->next == req->splices)
		return 0;
	struct mcp_splice const *splices = req->history_splices ? req->history_splices
	                                                     : req->splice;
	struct json_object *str = splices[req->next].str;
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
 * it, after what askai adds to the prompt if anything. */
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
	free(req->history_splices);
	req->history_splices = nullptr;
	req->next = 0;
	mcp_req_turn_ready(req);
}

/* Open the reply as an event stream, which keep-alives keep open while the
 * upstream works.  The timer, for those and the notifications of progress,
 * is in the heap from now on, so setting it cannot fail. */
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
 * millisecond; returns its length, 0 if it cannot be told. */
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
 * into the task's record; it has no client to tell more. */
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
 * cancelled. */
static void
mcp_on_task_cancel (struct loop *loop,
                    struct msg  *msg)
{
	struct ledger_task *task = container_of(msg, struct ledger_task, cancel);
	struct mcp_req *req = loop->exit ? nullptr : loop_lookup(loop, task->who, HANDLE_REQ);
	ledger_task_put(task);
	if (!req)
		return;
	mcp_req_log_call(req, "cancelled by its client", (struct span){}, false);
	mcp_req_abort(req, ECANCELED);
}

/* This is the function of the message that roster_cancel() posts when a
 * client cancels a tool call with notifications/cancelled.  @p msg is the
 * member cancel of the call's record.  If the call still runs on @p loop
 * and its reply has not begun, the function ends the call with
 * mcp_req_finish(), as a hang-up does, and its end record says cancelled.
 * Before that, it answers a JSON request with the status 202 and no body.
 * An event stream ends without a response. */
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
	mcp_req_log_call(req, "cancelled by notifications/cancelled", (struct span){},
	                 false);
	req->withdrawn = true;
	mcp_req_finish(req);
}

/* Puts the request's tool call into the roster, so that a
 * notifications/cancelled of the same caller on any loop reaches the call.
 * The record has a new handle of the request, which mcp_req_leave() frees.
 * Returns 0, or ENOMEM. */
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
 * handle for the record's cancel.  On failure, returns nullptr with the
 * error in @p *e: that of ledger_open(), or ENOMEM if the request cannot
 * be named. */
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
 * make, which close the result and the reply.  From then on, the call runs
 * as the task or the job of the record @p task: its stream is the stand-in,
 * which writes the call's reply into the record.  If the answer cannot go,
 * the function ends the record and the call, and returns the error. */
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
	/* The request has its answer.  From now on a client can cancel the
	 * call only with tasks/cancel or a client's jobs.  The call opens no
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
 * call runs as from now on, as mcp_req_detach() describes. */
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
		    mcp_slot_offer(&req->self)->name, mcp_req_client(req), LEDGER_ID,
		    task->id, mcp_elapsed(req->head));
}

/* Whether @p id may name a request, or a progress token: a number, or a
 * string of at most MAX_ID bytes, as every reply or notification repeats
 * it. */
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
	return slot->req->mcp->parts[mcp_slot_offer(slot)->part];
}

void *
mcp_slot_instance (struct mcp_slot const *slot)
{
	return slot ? slot->inst : nullptr;
}

/* Returns the record of the loading of the plugin of @p slot's tool, or
 * nullptr if the plugin is built into askai. */
static struct plugin_loaded const *
mcp_slot_loaded (struct mcp_slot const *slot)
{
	return slot->req->mcp->st->plugins[mcp_slot_offer(slot)->part].loaded;
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

void **
mcp_slot_own (struct mcp_slot *slot)
{
	return slot ? &slot->own : nullptr;
}

/* @p slot's call waits no more: let go of its name, if it has one, and of
 * its step, if queued.  The function also releases the call's HTTP
 * request if the request is under way.  A reply that has come stays until
 * mcp_slot_drop(), so that drop() can still use it. */
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
               struct tool_text  text,
               uint32_t          flags)
{
	if (!slot || (!text.ptr && text.len))
		return EFAULT;
	if (slot->done || flags & ~(uint32_t)TOOL_FAILED)
		return EINVAL;
	size_t len = jb_utf8(text.ptr, text.len < TOOL_TELL_MAX ? text.len : TOOL_TELL_MAX);
	mcp_slot_wake(slot);
	/* The output is the text whole, not lent, nor from an offset. */
	slot->skip = 0;
	slot->lent = (struct span){};
	mcp_slot_result(slot, ESC(text.ptr, len), flags & TOOL_FAILED);
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

/* Whether a plugin may use @p slot: 0, EFAULT for no slot, or EINVAL for a
 * call that has ended. */
static int
mcp_slot_live (struct mcp_slot const *slot)
{
	return !slot ? EFAULT : slot->done ? EINVAL : 0;
}

int
mcp_slot_fail (struct mcp_slot *slot,
               int              e)
{
	int refused = mcp_slot_live(slot);
	if (refused)
		return refused;
	slot->done = true;
	mcp_req_fail(slot->req, e);
	return 0;
}

int
mcp_slot_offset (struct mcp_slot    *slot,
                 struct json_object *args)
{
	int e = mcp_slot_live(slot);
	if (e)
		return e;
	struct json_object *o = jb_get(args, "offset");
	int64_t n = !o ? 0
	          : json_object_is_type(o, json_type_int) ? json_object_get_int64(o) : -1;
	slot->skip = n > 0 ? (size_t)n : 0;
	if (n >= 0)
		return 0;
	mcp_slot_tell(slot, TOOL_TEXT("The offset argument must be a whole number of bytes, "
	                              "from 0."), TOOL_FAILED);
	return EINVAL;
}

int
mcp_slot_write (struct mcp_slot  *slot,
                struct tool_text  text,
                uint32_t          flags,
                size_t           *taken)
{
	if (!taken)
		return EFAULT;
	*taken = 0;
	if (!slot || (!text.ptr && text.len))
		return EFAULT;
	if (slot->done || flags & ~(uint32_t)TOOL_MORE)
		return EINVAL;
	struct dstr *answer = &slot->answer;
	size_t had = dstr_len(answer);
	if (text.len > TOOL_OUTPUT_MAX - had)
		return EFBIG;
	/* What is UTF-8 goes at once; from the first byte that breaks it, a
	 * chunk at a time, mended by jb_mend(), which with TOOL_MORE leaves a
	 * character the end cuts short to the next write.  All or nothing. */
	size_t i = jb_utf8(text.ptr, text.len);
	bool ok = dstr_cat_n(answer, text.ptr, i);
	for (bool last = i == text.len; ok && !last; ) {
		char chunk[4 << 10];
		size_t n = text.len - i < sizeof chunk ? text.len - i : sizeof chunk;
		last = i + n == text.len;
		memcpy(chunk, text.ptr + i, n);
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

struct tool_text
mcp_slot_answer (struct mcp_slot const *slot)
{
	return (struct tool_text){ dstr_get(&slot->answer), dstr_len(&slot->answer) };
}

void
mcp_slot_lend (struct mcp_slot  *slot,
               struct tool_text  text)
{
	slot->lent = (struct span){ (unsigned char *)(uintptr_t)text.ptr, text.len };
}

/* Its request parks, if the slot is its own; a turn's call keeps it among
 * the calls a drain ends. */
int
mcp_slot_park (struct mcp_slot *slot)
{
	int e = mcp_slot_live(slot);
	if (e)
		return e;
	struct mcp_req *req = slot->req;
	if (mcp_slot_offer(slot)->kind->flags & TOOL_STREAM && req->sse_ok && !req->sse)
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
	int e = mcp_slot_live(slot);
	if (e)
		return e;
	if (!mcp_slot_offer(slot)->kind->step) {
		(void)mcp_slot_tell(slot, TOOL_TEXT("The tool's kind has no step(), so the "
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
	int e = mcp_slot_live(slot);
	if (!e && !slot->call.bits)
		e = loop_handle(slot->req->mcp->loop, slot, HANDLE_SLOT, &slot->call);
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
 * come.  The step of the call is queued, unless the call has ended.  If the
 * call is gone, or the loop has stopped, the wait is freed. */
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
 * web_wait_create(). */
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
	if (!slot || !request || !request->url || (!request->body && request->len))
		return EFAULT;
	if (slot->done)
		return EINVAL;
	char const *url = request->url;
	char const *accept = request->accept;
	size_t url_len = strnlen(url, WEB_URL_MAX + 1);
	size_t accept_len = accept ? strnlen(accept, WEB_ACCEPT_MAX + 1) : 0;
	size_t scheme = !strncmp(url, "http://", 7) ? 7 : !strncmp(url, "https://", 8) ? 8 : 0;
	/* The authority ends at the first '/', '?' or '#'.  A user name or a
	 * password in it ends in '@'.  For a URL that is too long, authority
	 * is 0, and the URL is refused. */
	size_t authority = scheme && url_len <= WEB_URL_MAX
	                 ? strcspn(url + scheme, "/?#") : 0;
	if (!authority || memchr(url + scheme, '@', authority) ||
	    (accept && (!accept_len || accept_len > WEB_ACCEPT_MAX)) ||
	    request->len > WEB_SEND_MAX || request->max - 1 >= WEB_BODY_MAX ||
	    request->timeout - 1 >= WEB_TIMEOUT_MAX || request->keep > WEB_KEEP_MAX)
		return EINVAL;
	for (size_t i = 0; i < url_len; ++i)
		if ((unsigned char)url[i] <= ' ' || (unsigned char)url[i] > '~')
			return EINVAL;
	for (size_t i = 0; i < accept_len; ++i)
		if ((unsigned char)accept[i] < ' ' || (unsigned char)accept[i] > '~')
			return EINVAL;
	struct mcp_offer const *offer = mcp_slot_offer(slot);
	if (!offer->kind->step)
		return mcp_slot_ready(slot);
	if (slot->web && !web_wait_done(slot->web))
		return EBUSY;
	struct plugin_loaded const *loaded = mcp_slot_loaded(slot);
	struct web_ask ask = {
		.request = request,
		.plugin = loaded ? loaded->manifest.name : nullptr,
		.url = url_len,
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
	int e = mcp_slot_live(slot);
	if (e)
		return e;
	if (!slot->web)
		return ENOENT;
	if (!web_wait_done(slot->web))
		return EINPROGRESS;
	web_wait_response(slot->web, response, slot->req->mcp->loop->now);
	return 0;
}

static void mcp_slot_notes_run (struct mcp_slot *slot, uint16_t fetch, struct buf *in);
static char const *mcp_slot_notebook (struct mcp_slot const *slot);

/* Serialize the tools that the turns of @p obj's userdata, a request, may
 * call: their definitions as the upstream's API has them, each after a
 * comma, the first's left out, in the API's array. */
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
	int e = printbuf_memappend(pb, f->tools.ptr, (int)f->tools.len);
	size_t skip = 1;
	for (uint32_t id = 0; e >= 0 && id < st->n_offers; ++id) {
		struct dstr const *def = &st->offers[id].lent;
		if (!(req->lent >> id & 1))
			continue;
		e = printbuf_memappend(pb, dstr_get(def) + skip, (int)(dstr_len(def) - skip));
		skip = 0;
	}
	if (e >= 0)
		e = printbuf_memappend(pb, f->tools_end.ptr, (int)f->tools_end.len);
	return e < 0 ? -1 : 0;
}

/* Draft the upstream request with @p prompt and @p system, and write it up
 * to the first of them.  The client's strings, which may be long, are left
 * out of the draft, and mcp_req_build() escapes them in; a default system
 * prompt is ours, and goes in with the draft, as do the tools the turns may
 * call, serialized once for every loop. */
static int
mcp_req_draft (struct mcp_req     *req,
               struct json_object *prompt,
               struct json_object *system,
               struct json_object *history)
{
	struct mcp_static const *st = req->mcp->st;
	size_t n = history ? json_object_array_length(history) : 0;
	req->splice_max = (uint16_t)(n + ARRAY_SIZE(req->splice));
	if (n) {
		req->history_splices = malloc(req->splice_max * sizeof *req->history_splices);
		if (!req->history_splices)
			return ENOMEM;
		for (size_t k = 0; k < n; ++k)
			json_object_set_serializer(jb_get(jb_at(history, k), "content"),
			                           mcp_splice_mark, req, nullptr);
	}
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
	req->text = json_object_to_json_string_length(req->draft, JB_WIRE, &req->len);
	return req->text ? mcp_req_copy(req, 0) : ENOMEM;
}

/* Logs, with --verbose, the start of the ask call of @p req, whose prompt
 * is @p prompt: its client, the size of the prompt, the thread of its
 * notes, if any, and the start of the prompt. */
static void
mcp_req_log_ask (struct mcp_req const *req,
                 struct json_object   *prompt,
                 bool                  resume)
{
	if (log_threshold < LOG_VERBOSE)
		return;
	size_t len = (size_t)json_object_get_string_len(prompt);
	char text[PROMPT_LOG + 8];
	mcp_quote(text, sizeof text, json_object_get_string(prompt), len, PROMPT_LOG);
	char thread[NOTES_ID + 32] = "no thread";
	if (*req->thread)
		util_format(thread, sizeof thread, "thread %s (%s)", req->thread,
		            resume ? "resumed" : "new");
	LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " from %s: prompt %zu %s, %s, %s",
	    req->mcp->loop->id, req->asked, mcp_req_client(req), len, mcp_bytes(len), thread,
	    text);
}

static int mcp_req_background (struct mcp_req *req, uint32_t tool, bool own_tool);

/* Only visible user and assistant text can be supplied as chat history.
 * Tool calls, tool results and system messages remain server controlled. */
static bool
mcp_history_valid (struct json_object *history)
{
	if (!json_object_is_type(history, json_type_array))
		return false;
	size_t n = json_object_array_length(history);
	if (n > CHAT_HISTORY)
		return false;
	for (size_t k = 0; k < n; ++k) {
		struct json_object *message = jb_at(history, k);
		if (!json_object_is_type(message, json_type_object) ||
		    json_object_object_length(message) != 2 || !jb_text(jb_get(message, "content")))
			return false;
		size_t len = 0;
		char const *role = jb_str(jb_get(message, "role"), &len);
		if (!role || !((len == 4 && !memcmp(role, "user", 4)) ||
		               (len == 9 && !memcmp(role, "assistant", 9))))
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
	                   notes_writer(req->mcp->st->notes), notes_selector(req->repo))
	     ? 0 : ENOMEM;
}

/* Start a call that asks the upstream, with the arguments @p args, of the
 * class the request asks for, lending the turns the tools req->lent says,
 * if any: build its upstream request, then submit its job.  The notes the turns keep, if askai keeps
 * them, go in the thread the call goes on with, else in a new one. */
static void
mcp_req_ask (struct mcp_req     *req,
             struct json_object *args,
             bool                background)
{
	struct mcp_static const *st = req->mcp->st;
	struct notes const *notes = req->lent & st->lending.by[LEND_ALWAYS] ? st->notes
	                                                                     : nullptr;
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
	if (problem.ptr) {
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
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	req->s->ops->waiting(req->s);
	if (req->sse_ok && !req->detach_pending)
		mcp_req_stream(req);
	/* Every ask starts with this participant's session notes. */
	req->self.notebook[0] = '\0';
	bool resume = notes && notes_exists(notes);
	mcp_req_dispose(req);
	if (resume) {
		mcp_slot_notes_run(&req->self, FETCH_RESUME, nullptr);
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
 * has gone.  Once the loop has stopped, MCP is gone, and the task is only
 * let go of. */
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
	mcp_slot_tell(slot, (struct tool_text){ text, n }, 0);
}

/* Run the @p slices a test_task call in @p slot asks for as a task, which
 * the call waits for. */
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
		mcp_slot_tell(slot, TOOL_TEXT("The slices argument must be a whole number "
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
 * that asks the upstream as background work. */
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
		mcp_slot_tell(slot, TOOL_TEXT("Give slices or a prompt, not both."), TOOL_FAILED);
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

/* Append the @p n pieces at @p p to @p slot's answer: 0, or ENOMEM. */
static int
mcp_slot_say (struct mcp_slot        *slot,
              struct mcp_piece const *p,
              size_t                  n)
{
	for (size_t i = 0; i < n; ++i)
		if (!dstr_cat_n(&slot->answer, p[i].ptr, p[i].len))
			return ENOMEM;
	return 0;
}

static void mcp_req_calls_next (struct mcp_req *req);
static bool mcp_req_slice (struct mcp_req *req, struct jb_pool *pool,
                           struct span json);
static void mcp_req_turn_submit (struct mcp_req *req);
static void mcp_req_add_start (struct mcp_req *req);

/* Append the literal @p s to @p d. */
#define CAT(d, s) dstr_cat_n(d, s, sizeof s - 1)

/* Write piece @p q into the upload from its byte req->off on, as far as the
 * step's shares allow. */
static int
mcp_req_upload_piece (struct mcp_req         *req,
                      struct mcp_piece const *q)
{
	struct loop *loop = req->mcp->loop;
	enum loop_unit kind = q->text ? LOOP_ESCAPE : LOOP_COPY;
	while (req->off < q->len) {
		size_t left = q->len - req->off;
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
 * its byte req->off on, as far as the step's shares allow.  Returns 0 once
 * all are written; EAGAIN if the step ran out first; ENOMEM; or ENOBUFS if
 * the upload would pass UPLOAD_MAX, its context full. */
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
 * context is full; else out of memory, the call ends. */
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
 * mcp_lending_lent() computes them for the client's role. */
static tool_mask
mcp_req_lent (struct mcp_req const *req)
{
	return mcp_lending_lent(&req->mcp->st->lending, req->s->upstream);
}

/* The bytes of a tool's output that a turn takes, at most: an eighth of the
 * context, if known, as text takes some 4 bytes a token. */
static size_t
mcp_req_lend_max (struct mcp_req const *req)
{
	size_t eighth = (size_t)req->context / 2;
	return !req->context ? LEND_MAX : eighth < 2048 ? 2048
	       : eighth < LEND_MAX ? eighth : LEND_MAX;
}

/* The tokens that the upload, as written so far, takes: the last turn's
 * request and answer, as its usage said, and what was written after the
 * answer, at 4 bytes a token; or, unknown, all of it so. */
static uint64_t
mcp_req_tokens (struct mcp_req const *req)
{
	size_t len = buf_peek(&req->upload).len;
	return req->used ? req->used + (len > req->sent ? len - req->sent : 0) / 4 +
	                   req->filed / 4
	                 : (len + req->filed) / 4;
}

/* A turn's tool call, as its job's calls, and then req->calls, hold it: a
 * struct job_call, then its id, name, extra and arguments. */
struct mcp_call {
	struct span id;
	struct span name;
	struct span extra;
	struct span args;
	size_t      size;    /* bytes of all that */
	size_t      replay;  /* bytes of the job's replay before it */
};

/* The call at @p p. */
static struct mcp_call
mcp_call_at (unsigned char const *p)
{
	struct job_call c;
	memcpy(&c, p, sizeof c);
	unsigned char *id = (unsigned char *)(uintptr_t)(p + sizeof c);
	unsigned char *extra = id + c.id + c.name;
	return (struct mcp_call){
		.id = { id, c.id },
		.name = { id + c.id, c.name },
		.extra = { extra, c.extra },
		.args = { extra + c.extra, c.args },
		.size = job_call_size(&c),
		.replay = c.replay,
	};
}

/* The id of the turn's call @p k, @p c, in @p *id: its own, or, lacking
 * one, one made up in @p made, of CALL_ID bytes. */
static int
mcp_req_call_id (struct mcp_req const  *req,
                 struct mcp_call const *c,
                 uint32_t               k,
                 char                  *made,
                 struct mcp_piece      *id)
{
	if (c->id.len) {
		*id = ESC(c->id.ptr, c->id.len);
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
 * timer set, or what happened that they were not told yet. */
static bool
mcp_agenda_pending (struct mcp_agenda const *a)
{
	if (!a)
		return false;
	bool any = dstr_len(&a->notices) || dstr_len(&a->told);
	for (uint32_t k = 0; !any && k < a->n_jobs; ++k)
		any = !a->jobs[k].done;
	for (size_t i = 0; !any && i < TIMERS_MAX; ++i)
		any = a->timers[i].due;
	return any;
}

/* Whether the turns, which answered, doze until what their agenda has under
 * way happens, then go on: if the answer, streamed, has text, the turns may
 * take another, and the context has room. */
static bool
mcp_req_dozes (struct mcp_req const *req)
{
	struct job const *job = req->job;
	return job->status == JOB_TEXT && buf_peek(&job->result).len &&
	       req->turn < req->mcp->st->turns && !mcp_req_full(req) &&
	       mcp_agenda_pending(req->agenda);
}

/* A turn has ended, its job's answer in: an answer that calls no tool is the
 * call's answer, unless the turns doze after it, as are the last turn's;
 * else parse the arguments of the calls it makes, then write the turn into
 * the upload, then run them, or doze. */
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
 * parse goes on, or ENOMEM. */
static int
mcp_req_turn_arg (struct mcp_req *req,
                  struct span     args)
{
	if (!args.len) {
		req->args[req->part] = json_object_new_object();
		return req->args[req->part] ? 0 : ENOMEM;
	}
	if (!mcp_req_slice(req, &req->mcp->answers, args))
		return EAGAIN;
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
 * the turn. */
static void
mcp_req_turn_args (struct mcp_req *req)
{
	for (; req->part < req->n_calls; ++req->part) {
		struct mcp_call c = mcp_call_at(buf_peek(&req->job->calls[req->part]).ptr);
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
	struct ai_bytes end = ai_format(req->mcp->st->ep->api)->end;
	buf_cut(&req->upload, buf_peek(&req->upload).len - end.len, end.len);
	req->part = 0;
	req->replayed = 0;
	req->next = 0;
	req->off = 0;
	mcp_req_enter(req, PHASE_TURN);
}

/* The items of the job's replay that the turn has yet to write before its
 * byte @p to, as a piece: the first's comma left out if the list that they
 * go in has no item yet. */
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

/* The arguments of the call @p c, @p k in the turn, as @p f writes them,
 * into @p *p: "{}" if they were no object, as the upstream may parse them. */
static int
mcp_req_call_args (struct mcp_req const   *req,
                   struct ai_format const *f,
                   struct mcp_call const  *c,
                   uint32_t                k,
                   struct mcp_piece       *p)
{
	struct json_object *v = req->args[k];
	if (!(f->flags & AI_ARGS_RAW)) {
		*p = v && c->args.len ? ESC(c->args.ptr, c->args.len) : TEXT("{}");
		return 0;
	}
	size_t len = 0;
	char const *json = v ? json_object_to_json_string_length(v, JB_WIRE, &len)
	                     : nullptr;
	*p = json ? RAW(json, len) : FRAG("{}");
	return v && !json ? ENOMEM : 0;
}

/* Write the call @p c, req->part - 1 in the turn, after @p items, those of
 * the replay before it. */
static int
mcp_req_call_put (struct mcp_req         *req,
                  struct ai_format const *f,
                  struct mcp_call const  *c,
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
		BYTES(req->listed || items.len ? f->call_next : f->call),
		id,
		BYTES(f->name),
		ESC(c->name.ptr, c->name.len),
		BYTES(f->args),
		args,
		BYTES(f->args_end),
		c->extra.len ? BYTES(f->extra) : (struct mcp_piece){},
		RAW(c->extra.ptr, c->extra.len),
		BYTES(f->call_end),
	};
	e = mcp_req_upload(req, p, ARRAY_SIZE(p));
	if (!e)
		req->listed = true;
	return e;
}

/* Write the turn's call under way, after the items of the replay before
 * it; or only those, as the API's calls are in the replay. */
static int
mcp_req_turn_call (struct mcp_req         *req,
                   struct ai_format const *f)
{
	struct mcp_call c = mcp_call_at(buf_peek(&req->job->calls[req->part - 1u]).ptr);
	struct mcp_piece items = mcp_req_items(req, c.replay);
	int e = f->call.len ? mcp_req_call_put(req, f, &c, items)
	                    : mcp_req_upload(req, &items, 1);
	if (e)
		return e;
	req->listed = req->listed || items.len;
	req->replayed = c.replay;
	return 0;
}

/* Write part req->part of the turn into the upload: first its head, with
 * its text and thinking if the API writes them, then each tool call, then
 * the replay's items after the last and its end, an answer's if it calls
 * none. */
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
 * go of the job and run the first; or doze, if it calls none. */
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

/* Add a line for the tool call @p c, with the arguments @p args, to the
 * trace: its name and, as @p trace says, the first TRACE_ARGS bytes of its
 * arguments' JSON text, or its action; the outcome follows. */
static int
mcp_req_trace (struct mcp_req        *req,
               struct mcp_call const *c,
               struct json_object    *args,
               enum mcp_trace         trace)
{
	if (req->traced++ >= TRACE_MAX)
		return 0;
	char const *name = (char const *)c->name.ptr;
	if (!CAT(&req->trace, "- ") ||
	    !dstr_cat_n(&req->trace, name, jb_utf8(name, mcp_clip(name, c->name.len, 64))))
		return ENOMEM;
	switch (trace) {
	case TRACE_NAME:
		return 0;
	case TRACE_ACTION:
		size_t len = 0;
		char const *action = jb_str(jb_get(args, "action"), &len);
		return !action || (CAT(&req->trace, " ") &&
		                   dstr_cat_n(&req->trace, action, mcp_clip(action, len, 16)))
		       ? 0 : ENOMEM;
	default:
		char const *json = (char const *)c->args.ptr;
		size_t n = jb_utf8(json, mcp_clip(json, c->args.len, TRACE_ARGS));
		if (n && !(CAT(&req->trace, " ") && dstr_cat_n(&req->trace, json, n)))
			return ENOMEM;
		return n < c->args.len && !CAT(&req->trace, "...") ? ENOMEM : 0;
	}
}

/* End the trace's line for the call under way: whether it failed, or the
 * bytes of its output. */
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

/* The id of the tool of @p name, of @p len bytes, that askai offers,
 * compared byte for byte; TOOL_MAX if none, as for nullptr and 0.  The
 * tools that a role can grant have the ids below st->n_granted. */
static uint32_t
mcp_static_tool_of (struct mcp_static const *st,
                    char const              *name,
                    size_t                   len)
{
	for (uint32_t id = 0; id < st->n_offers; ++id) {
		struct mcp_offer const *o = &st->offers[id];
		if (o->len == len && !memcmp(o->name, name, len))
			return id;
	}
	return TOOL_MAX;
}

/* The id of the tool of @p name, of @p len bytes, that the turns may
 * call; TOOL_MAX if none. */
static uint32_t
mcp_req_tool_of (struct mcp_req const *req,
                 char const           *name,
                 size_t                len)
{
	uint32_t id = mcp_static_tool_of(req->mcp->st, name, len);
	return id < TOOL_MAX && req->lent >> id & 1 ? id : TOOL_MAX;
}

/* The words of the log for the arguments of a turn's tool call of @p req
 * that are not a JSON object, so that the call was not run: the output
 * token limit cut them short, or the model wrote another value. */
static struct tool_text
mcp_req_no_args (struct mcp_req const *req)
{
	return req->cut ? TOOL_TEXT("cut short by the output token limit")
	                : TOOL_TEXT("not a JSON object");
}

/* Logs, with --verbose, a turn's call of the tool @p name, which the turns
 * of @p req may not call, with its arguments @p args: askai offers the
 * tool, but not to this ask call, or askai offers no such tool.  The name
 * comes from the upstream, so the line has it as a JSON string. */
static void
mcp_req_log_refused (struct mcp_req const *req,
                     struct span           name,
                     struct json_object   *args)
{
	if (log_threshold < LOG_VERBOSE)
		return;
	struct mcp_static const *st = req->mcp->st;
	char const *str = (char const *)name.ptr;
	uint32_t known = mcp_static_tool_of(st, str, name.len);
	char quoted[NAME_LOG + 8];
	mcp_quote(quoted, sizeof quoted, str, name.len, NAME_LOG);
	char shown[SHOWN];
	mcp_args_shown(&st->offers[known], args, mcp_req_no_args(req), shown);
	LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " turn %" PRIu32 ": tool call %s "
	    "refused: %s; arguments %s", req->mcp->loop->id, req->asked, req->turn, quoted,
	    known < TOOL_MAX ? "not lent to this ask call" : "askai offers no such tool",
	    shown);
}

static void mcp_agenda_alarm (struct loop *loop, struct loop_timer *timer);

/* An agenda for the turns of @p req, whose jobs are its own slots; nullptr,
 * with ENOMEM in @p *e, if out of memory. */
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
 * error in @p *e, if it cannot be. */
static struct mcp_agenda *
mcp_req_agenda (struct mcp_req *req,
                int            *e)
{
	*e = 0;
	if (!req->agenda)
		req->agenda = mcp_agenda_create(req, e);
	return req->agenda;
}

/* Set @p a's alarm to the first that is due, a timer or the wait's end, if
 * any: 0, or ENOMEM. */
static int
mcp_agenda_arm (struct mcp_agenda *a)
{
	struct loop *loop = a->req->mcp->loop;
	uint64_t due = a->wait ? a->wait_end : UINT64_MAX;
	for (size_t i = 0; i < TIMERS_MAX; ++i)
		if (a->timers[i].due && a->timers[i].due < due)
			due = a->timers[i].due;
	if (due == UINT64_MAX) {
		loop_timer_stop(loop, &a->alarm);
		return 0;
	}
	return loop_timer_set(loop, &a->alarm, due);
}

/* End the wait that @p a's turn waits in: it says what happened, or that
 * nothing did in the seconds it took. */
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
 * the reply, before the answer that follows. */
static void
mcp_agenda_wake (struct mcp_agenda *a)
{
	struct mcp_req *req = a->req;
	bool ok = dstr_cat_n(&a->told, dstr_get(&a->notices), dstr_len(&a->notices)) &&
	          dstr_cat_n(&a->said, dstr_get(&a->told), dstr_len(&a->told)) &&
	          CAT(&a->said, "\n") &&
	          (req->turn + 1u < req->mcp->st->turns ||
	           CAT(&a->told, "[askai: your next turn is your last: answer then, "
	                         "without calling tools.]\n"));
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
 * happens next wakes them, or wake them now if something has. */
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
 * and a wait for it ends, or it wakes the turns, if they doze. */
static void
mcp_agenda_job_done (struct mcp_slot *slot)
{
	struct mcp_agenda *a = slot->req->agenda;
	uint32_t n = (uint32_t)(slot - a->jobs) + 1;
	if (!dstr_printf(&a->notices, "[askai: job %" PRIu32 ", %s, %s: call jobs with the "
	                 "action result and job %" PRIu32 " for its output.]\n", n,
	                 mcp_slot_offer(slot)->name, slot->failed ? "failed" : "finished",
	                 n)) {
		mcp_req_fail(slot->req, ENOMEM);
		return;
	}
	if (a->wait && (!a->wait_for || a->wait_for >> (n - 1) & 1))
		mcp_agenda_wait_end(a);
	else if (a->req->phase == PHASE_DOZE)
		mcp_agenda_wake(a);
}

/* Appends the notice that the timer @p t fired, with its note, to @p d.
 * Returns false if out of memory. */
static bool
mcp_notice_timer (struct dstr               *d,
                  struct ledger_timer const *t)
{
	return dstr_printf(d, "[askai: timer %" PRIu32 " fired%s%.*s.]\n", t->id,
	                   t->len ? ": " : "", (int)t->len, t->note);
}

/* @p timer's agenda's alarm: the timers due fire, which the next result
 * says, and a wait ends if one did, or if its time is up; one that fires
 * wakes the turns, if they doze. */
static void
mcp_agenda_alarm (struct loop       *loop,
                  struct loop_timer *timer)
{
	struct mcp_agenda *a = container_of(timer, struct mcp_agenda, alarm);
	bool fired = false;
	for (size_t i = 0; i < TIMERS_MAX; ++i) {
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

/* Starts the call of @p slot with the arguments @p args, and stores the
 * time of the start in the slot.
 *
 * A client can send any JSON value as the arguments.  If @p args is neither
 * nullptr nor an object, the call ends with an error text.  Otherwise the
 * function gets the instance of the tool's plugin for the user of the
 * request, if the plugin has instances, and then calls start() of the
 * tool's kind.  If instance_table_get() returns EBUSY, the call ends as
 * failed.  Any other error of instance_table_get() ends the whole request.
 *
 * No function of a plugin is running when this function is called. */
static void
mcp_slot_start (struct mcp_slot    *slot,
                struct json_object *args)
{
	slot->start = loop_clock();
	if (args && !json_object_is_type(args, json_type_object)) {
		mcp_slot_tell(slot, TOOL_TEXT("Tool arguments must be an object."), TOOL_FAILED);
		return;
	}
	struct mcp_req *req = slot->req;
	struct mcp *mcp = req->mcp;
	struct mcp_offer const *offer = mcp_slot_offer(slot);
	int e = instance_table_get(&mcp->instances, mcp->st->plugins[offer->part].plugin,
	                           req->principal, req->generation, &slot->inst);
	if (e == EBUSY)
		mcp_slot_result(slot, TEXT(busy), true);
	else if (e)
		mcp_req_fail(req, e);
	else
		offer->kind->start(slot, args);
}

/* The words for the kinds and the statuses of the records of the ledger,
 * which the answers of a client's jobs and wait use. */
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
 * ledger refuses for want of room. */
static char const no_room[] = "askai has no room for more jobs, timers or waits "
                              "now; try again later.";

/* Answers a client's call in @p slot that the ledger refused with @p e:
 * with @p busy for EBUSY, and with no_room for ENOSPC.  Any other error
 * fails the call. */
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
 * to @p max, else 0. */
static int64_t
mcp_seconds (struct json_object *args,
             int64_t             max)
{
	struct json_object *o = jb_get(args, "seconds");
	int64_t secs = json_object_is_type(o, json_type_int) ? json_object_get_int64(o) : 0;
	return secs >= 1 && secs <= max ? secs : 0;
}

/* Returns the whole seconds from @p from to @p to, or 0 if @p to is not
 * later. */
static uint64_t
mcp_seconds_since (uint64_t from,
                   uint64_t to)
{
	return to > from ? (to - from) / NS : 0;
}

/* Detach a client call into the ledger, with its result read through the
 * same tool when @p own_tool, else through jobs. */
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
	                       st->offers[tool].name, LEDGER_ID, task->id)
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

/* A client's call of background in @p slot, the request's own: start a
 * call of the tool that @p args names, with the arguments in @p args, as a
 * job.  The request answers the client with the id of the job at once, and
 * then runs the call as the job. */
static void
mcp_slot_client_background (struct mcp_slot    *slot,
                            struct json_object *args)
{
	struct mcp_req *req = slot->req;
	struct mcp_static const *st = req->mcp->st;
	size_t len = 0;
	char const *name = jb_str(jb_get(args, "tool"), &len);
	uint32_t id = mcp_static_tool_of(st, name, len);
	struct json_object *call = jb_get(args, "arguments");
	struct mcp_piece problem =
		id >= st->n_granted || !(req->s->tools >> id & 1) ||
		st->offers[id].kind->flags & KF_AGENDA
		? TEXT("The tool argument must name a tool of yours other than background, "
		       "jobs, wait and timer.")
		: call && !json_object_is_type(call, json_type_object)
		? TEXT("The arguments argument must be an object.")
		: (struct mcp_piece){};
	if (problem.ptr) {
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
		mcp_args_shown(&st->offers[id], call, TOOL_TEXT("none"), req->call_args);
	LOG(LOG_VERBOSE, "tool call background from %s became job %.*s, a call of %s",
	    mcp_req_client(req), LEDGER_ID, req->task->id, st->offers[id].name);
	/* Last, as the start of the job's call may end the request. */
	mcp_slot_start(slot, call);
}

/* Start the call that @p args asks for as a job in the background, in a
 * slot of the agenda's: of a tool the turns may call that may run so.  The
 * call in @p slot says the job's number at once.  A client's call is
 * mcp_slot_client_background()'s. */
static void
mcp_slot_background (struct mcp_slot    *slot,
                     struct json_object *args)
{
	if (slot->role == ROLE_OWN) {
		mcp_slot_client_background(slot, args);
		return;
	}
	struct mcp_req *req = slot->req;
	size_t len = 0;
	char const *name = jb_str(jb_get(args, "tool"), &len);
	struct json_object *call = jb_get(args, "arguments");
	uint32_t id = name ? mcp_req_tool_of(req, name, len) : TOOL_MAX;
	struct mcp_piece problem =
		!(req->mcp->st->offers[id].kind->flags & TOOL_DETACH)
		? TEXT("The tool argument must name a tool of yours that may run in the "
		       "background, such as git or man.")
		: call && !json_object_is_type(call, json_type_object)
		? TEXT("The arguments argument must be an object.")
		: (struct mcp_piece){};
	if (problem.ptr) {
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
	/* job->args is a deep copy of the arguments.  mcp_req_args_free()
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
 * and where it stands. */
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
		                          k + 1, mcp_slot_offer(job)->name,
		                          (int)mcp_clip(args, len, 100), args,
		                          len > 100 ? "..." : "", state))
			return ENOMEM;
	}
	return 0;
}

/* Give @p job's output, as the jobs tool's call in @p slot asks with
 * @p args: from its offset, once it has finished. */
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
 * names, for a client's call of jobs in @p slot.  If the owner of the call
 * has no such record, returns nullptr, and the call answers why. */
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
 * first, then the timers.  Returns 0, or ENOMEM. */
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
	uint64_t day = util_realtime();
	uint64_t now = loop_clock();
	bool ok = true;
	for (uint32_t i = 0; ok && i < look.n_tasks; ++i) {
		struct ledger_task const *t = look.tasks[i];
		enum ledger_status status = atomic_load_explicit(&t->status,
		                                                 memory_order_acquire);
		bool told = atomic_load_explicit(&t->told, memory_order_relaxed);
		ok = dstr_printf(d, "%s %.*s: %s: ", record_kinds[t->kind], LEDGER_ID, t->id,
		                 st->offers[t->tool].name) &&
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
 * which the record has, and which mcp_req_show() copies. */
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
	if (problem.ptr) {
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
 * jobs in @p slot. */
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
 * answer with the result of a job, or cancel a job or a timer. */
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
 * the one @p args names, or cancel it.  A client's call is
 * mcp_slot_client_jobs()'s. */
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
	if (problem.ptr) {
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
 * else that nothing happened.  Returns 0, or ENOMEM. */
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
		char const *tool = st->offers[t->tool].name;
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

/* Checks the client's wait @p w.  If ledger_tell() returns jobs that ended
 * or timers that fired, or the wait's time is up, the wait's call answers
 * with what happened.  Else the wait goes on: its alarm is set for its end,
 * or for the time of the owner's first timer, if that is sooner.  On the
 * first check, @p first, the call then parks. */
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
 * record of the wait's owner ends or the owner sets a timer.  If the wait's
 * call still runs on @p loop, its step checks the wait again. */
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
 * names or any, or a timer of the owner fires.  A job named that has ended,
 * an ended job that no one has read, or a timer that fired before the call,
 * ends the call at once. */
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
	for (size_t i = 0; !problem.ptr && i < n; ++i) {
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
		                               (int)mcp_clip(id, len, LEDGER_ID), id));
	}
	if (problem.ptr) {
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
 * queues. */
static void
mcp_slot_wait_step (struct mcp_slot *slot)
{
	if (slot->own && !slot->done)
		mcp_wait_check(slot->own, false);
}

/* Stops the alarm of the wait of a client's call of wait in @p slot,
 * removes the wait from the ledger and frees it. */
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
 * any, finishes, a timer fires, or its seconds are up.  What happened and
 * no result said yet ends it at once.  A client's call is
 * mcp_slot_client_wait()'s. */
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

/* Sets a timer of the turns of @p req that fires at @p due, with the
 * @p len bytes at @p note as its note, and stores its number in @p *id.
 * Returns 0, EBUSY while the turns have TIMERS_MAX timers, or ENOMEM. */
static int
mcp_agenda_timer (struct mcp_req *req,
                  uint64_t        due,
                  char const     *note,
                  size_t          len,
                  uint32_t       *id)
{
	int e;
	struct mcp_agenda *a = mcp_req_agenda(req, &e);
	if (!a)
		return e;
	size_t i = 0;
	while (i < TIMERS_MAX && a->timers[i].due)
		++i;
	if (i == TIMERS_MAX)
		return EBUSY;
	struct ledger_timer *t = &a->timers[i];
	t->due = due;
	t->id = *id = ++a->n_timers;
	t->len = (uint32_t)len;
	memcpy(t->note, note ? note : "", len);
	return mcp_agenda_arm(a);
}

/* The timer tool's call in @p slot: set a timer to fire in the seconds in
 * @p args, with its note: a timer of the turns, or for a client's call, a
 * timer of the call's owner in the ledger. */
static void
mcp_slot_timer (struct mcp_slot    *slot,
                struct json_object *args)
{
	static_assert(TIMER_MAX == 3600 && TIMER_NOTE == 200 && TIMERS_MAX == 8,
	              "the problems say");
	struct mcp_req *req = slot->req;
	int64_t secs = mcp_seconds(args, TIMER_MAX);
	size_t len = 0;
	struct json_object *o = jb_get(args, "note");
	char const *note = jb_str(o, &len);
	struct mcp_piece problem =
		!secs ? TEXT("The seconds argument must be a whole number from 1 to 3600.")
		: o && (!note || len > TIMER_NOTE || jb_utf8(note, len) < len)
		? TEXT("The note argument must be a string of at most 200 bytes.")
		: (struct mcp_piece){};
	if (problem.ptr) {
		mcp_slot_result(slot, problem, true);
		return;
	}
	uint64_t due = req->mcp->loop->now + (uint64_t)secs * NS;
	uint32_t id;
	int e = slot->role == ROLE_OWN
	      ? ledger_timer_set(req->mcp->st->ledger, mcp_req_owner(req), due, note, len, &id)
	      : mcp_agenda_timer(req, due, note, len, &id);
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
 * offer. */
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

/* Launch the turn's next tool call, in its slot, unless it must wait: a
 * call that keeps to itself until those before it are written, and those
 * after it until it is.  A call goes to the tool of its name that the turns
 * may call, whose kind starts it, writes its output into the slot's answer,
 * or lends it, and calls mcp_slot_done().  The step comes again after
 * it, as the call may end the request. */
static void
mcp_req_call_launch (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	uint32_t k = req->launched;
	if (k == req->n_calls)
		return;
	struct mcp_call c = mcp_call_at(buf_peek(&req->calls).ptr + req->launch_at);
	uint32_t id = mcp_req_tool_of(req, (char const *)c.name.ptr, c.name.len);
	struct mcp_offer const *tool = &mcp->st->offers[id];
	if (k > req->part &&
	    (tool->kind->flags | mcp_slot_offer(&req->slots[k - 1])->kind->flags) & KF_SERIAL)
		return;
	if (mcp->draining) {
		mcp_req_fail(req, ESHUTDOWN);
		return;
	}
	if (*req->archive && !(req->intents >> k & 1) && !req->saved_tools[k]) {
		struct json_object *intent = jb_object(
			"index", json_object_new_int64(k),
			"agenda", json_object_new_boolean((tool->kind->flags & KF_AGENDA) != 0),
			"name", json_object_new_string_len((char const *)c.name.ptr, (int)c.name.len),
			"arguments", req->args[k] ? json_object_get(req->args[k])
			                          : json_object_new_object(), nullptr);
		int e = mcp_req_archive(req, "tool-intent", intent, mcp_req_intent_saved);
		if (e) {
			mcp_req_fail(req, e);
			return;
		}
		loop_unready(&req->step);
		req->phase = PHASE_JOURNAL;
		return;
	}
	/* Its arguments go once its result is written, or the request ends. */
	struct json_object *args = req->args[k];
	struct mcp_slot *slot = &req->slots[k];
	slot->tool = (uint8_t)id;
	req->launch_at += c.size;
	req->launched = k + 1;
	hist_add(&mcp->counts->calls.lent[tool->metric], 1);
	mcp_req_notify_tool(req, tool->name);
	loop_ready_bulk(mcp->loop, &req->step);
	if (req->saved_tools[k]) {
		struct json_object *saved = req->saved_tools[k];
		size_t len = 0;
		char const *notebook = jb_str(jb_get(saved, "notebook"), &len);
		if (notebook) {
			if (notes_select(notebook, len, slot->notebook)) {
				mcp_req_fail(req, EILSEQ);
				return;
			}
			slot->fetch = FETCH_READ;
		}
		char const *text = jb_str(jb_get(saved, "text"), &len);
		slot->start = loop_clock();
		slot->skip = json_object_get_uint64(jb_get(saved, "skip"));
		mcp_slot_result(slot, ESC(text, len), json_object_get_boolean(jb_get(saved, "failed")));
		return;
	}
	if (id == TOOL_MAX)
		mcp_req_log_refused(req, c.name, args);
	if (!args) {
		/* mcp_slot_start() does not run for this call, so this line sets
		 * slot->start.  mcp_slot_done() logs the time since slot->start. */
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
 * written, submit the next turn, or, its context full, compact it first. */
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
 * if one is near. */
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
		window = mcp_clip((char const *)out.ptr + skip, len, cap);
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
 * its end; returns the note's length. */
static size_t
mcp_slot_note_window (struct mcp_slot *slot,
                      size_t           len,
                      bool             past)
{
	size_t end = slot->skip + slot->window;
	if (end < len) {
		if (mcp_slot_offer(slot)->kind->flags & TOOL_PAGED)
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

/* Handles the end of the call in @p slot, which is a call of a turn or a
 * job.  The call has produced its output or has failed.  For a call of a
 * turn, the function sets slot->skip and slot->window to the part of the
 * output that the next upstream request contains, and writes the note about
 * that part into slot->note.  Then it queues the step of the request.  The
 * step writes the results of the calls into the upstream request in the
 * order of the calls.  For a job, the function calls mcp_agenda_job_done(),
 * which adds a notice about the job to the agenda.
 *
 * With --verbose, the function logs one line with the ask call, the turn,
 * the name of the tool, whether the call failed, the time that the call
 * took, the size of the output in bytes, the arguments, as mcp_args_shown()
 * writes them, without the text of a note, and the text of a failure.  The
 * line never contains the output of a call that succeeds, which can be the
 * text of the upstream's notes.  A failed call is logged at the verbose
 * level, because a model that calls a tool with wrong arguments is not an
 * error of askai.  A call of a tool that the turns may not call is logged
 * by mcp_req_call_launch() instead.  A call that ends in another way, such
 * as a job that a jobs call cancels or a call that ends with its request,
 * ends in mcp_slot_drop() and has no line. */
static void
mcp_slot_done (struct mcp_slot *slot,
               bool             failed)
{
	/* The words of the log line for a call of a turn and for a job.  This
	 * function is not called for ROLE_OWN. */
	static char const *const label[] = {
		[ROLE_TURN] = "tool call",
		[ROLE_JOB]  = "background tool call",
	};
	struct mcp_req *req = slot->req;
	struct span out = mcp_slot_output(slot);
	if (log_threshold >= LOG_VERBOSE && slot->tool != TOOL_MAX) {
		struct mcp_offer const *offer = mcp_slot_offer(slot);
		bool job = slot->role == ROLE_JOB;
		/* A turn's call whose arguments are not a JSON object ends at once
		 * in mcp_req_call_launch(), so req->cut is still the value of its
		 * turn. */
		char shown[SHOWN];
		mcp_args_shown(offer, job ? slot->args : req->args[slot - req->slots],
		               job ? TOOL_TEXT("none") : mcp_req_no_args(req), shown);
		char text[ERROR_LOG + 8] = "";
		if (failed && out.len)
			mcp_quote(text, sizeof text, (char const *)out.ptr, out.len, ERROR_LOG);
		LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " turn %" PRIu32 ": %s %s %s "
		    "after %.1f s (%zu %s); arguments %s%s%s", req->mcp->loop->id, req->asked,
		    req->turn, label[slot->role], offer->name, failed ? "failed" : "answered",
		    mcp_elapsed(slot->start), out.len, mcp_bytes(out.len), shown,
		    *text ? "; error " : "", text);
	}
	slot->failed = failed;
	char const *notebook = !failed &&
	                      (slot->fetch & (FETCH_KIND | FETCH_COPY | FETCH_ATTACH)) == FETCH_READ
	                    ? notes_selector(mcp_slot_notebook(slot)) : nullptr;
	if (*req->archive && slot->role == ROLE_TURN && !req->saved_tools[slot - req->slots]) {
		struct span files = buf_peek(&req->attached);
		struct json_object *result = jb_object(
			"index", json_object_new_int64(slot - req->slots),
			"failed", json_object_new_boolean(failed),
			"skip", json_object_new_uint64(slot->skip),
			"branch", json_object_new_boolean(req->branch),
			"filing", json_object_new_boolean(req->filing),
			"filed", json_object_new_uint64(req->filed),
			"attached", json_object_new_string_len(files.len ? (char const *)files.ptr : "", (int)files.len),
			"text", json_object_new_string_len(out.len ? (char const *)out.ptr : "", (int)out.len),
			nullptr);
		if (notebook && jb_set(result, "notebook", json_object_new_string(notebook))) {
			json_object_put(result);
			result = nullptr;
		}
		int e = mcp_req_archive(req, "tool-result", result, nullptr);
		if (e)
			req->archive_error = e;
	}
	if (slot->role == ROLE_JOB) {
		mcp_agenda_job_done(slot);
		return;
	}
	bool past = slot->skip > out.len;
	mcp_slot_window(slot, out);
	slot->note_len = (uint16_t)mcp_slot_note_window(slot, out.len, past);
	if (notebook)
		slot->note_len += (uint16_t)util_format(slot->note + slot->note_len,
		                                        sizeof slot->note - slot->note_len,
		                                        "\n\n[askai: notebook %s]",
		                                        notebook);
	loop_ready_bulk(req->mcp->loop, &req->step);
}

/* Writes a notice about the context of @p req into the @p size bytes at
 * @p buf, and returns its length.  @p tokens is the number of tokens in the
 * context after the turn's results.  From two thirds of the context on, the
 * notice asks the turns to call compact, after each turn, if they have it.
 * Below two thirds,
 * if the turns have the notes tool, the notice reminds them to write notes,
 * once when the context is a quarter full and once when it is half full.
 * Otherwise there is no notice.  With --verbose, a notice is logged. */
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
	if (full ? !(req->lent & l->compact)
	         : quarters <= req->reminded || !(req->lent & l->by[LEND_ALWAYS]))
		return 0;
	LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " turn %" PRIu32 ": context "
	    "notice, %" PRIu64 " %% full, %" PRIu64 " of %" PRIu64 " tokens: %s",
	    req->mcp->loop->id, req->asked, req->turn, tokens * 100 / context, tokens,
	    context, full ? "a request to call compact" : "a reminder to write notes");
	if (full)
		return util_format(buf, size, "\n\n[askai: your context is %" PRIu64 "%% full, "
		                   "%" PRIu64 " of %" PRIu64 " tokens. Note what you need to "
		                   "keep, then call compact.]", tokens * 100 / context, tokens,
		                   context);
	req->reminded = quarters;
	return util_format(buf, size, "\n\n[askai: your context is %" PRIu64 "%% full, "
	                   "%" PRIu64 " of %" PRIu64 " tokens. Now is a good time to write "
	                   "notes of what you have learned. Compaction removes the "
	                   "conversation, but not the notes.]", tokens * 100 / context,
	                   tokens, context);
}

/* Write the result of the turn's call in @p slot, the next in order.  After
 * the turn's last result, askai adds the notice about the context, if any,
 * and a notice that the next turn is the last one, if it is. */
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
			n += util_format(slot->note + n, size - n, "\n\n[askai: your next turn "
			                 "is your last: answer then, without calling tools.]");
	}
	slot->note_len = (uint16_t)n;
	req->next = 0;
	req->off = 0;
	mcp_req_enter(req, PHASE_RESULT);
}

/* Write the result of the call @p c, the one under way, into the upload,
 * and after the turn's last one the files that the turn's calls attached
 * and the end of the messages. */
static int
mcp_req_result_put (struct mcp_req        *req,
                    struct mcp_call const *c)
{
	struct ai_format const *f = ai_format(req->mcp->st->ep->api);
	struct mcp_slot const *slot = &req->slots[req->part];
	struct span out = mcp_slot_output(slot);
	struct span attached = buf_peek(&req->attached);
	struct dstr const *told = req->agenda ? &req->agenda->told : nullptr;
	size_t told_len = told ? dstr_len(told) : 0;
	char made[CALL_ID];
	struct mcp_piece id;
	int e = mcp_req_call_id(req, c, req->part, made, &id);
	if (e)
		return e;
	/* A result that names its call gives the id of a call that had one,
	 * or of one that the turn wrote with an id made up. */
	struct mcp_piece key = id;
	struct mcp_piece by = {};
	struct mcp_piece also = {};
	if (f->flags & AI_RESULT_NAMED) {
		key = ESC(c->name.ptr, c->name.len);
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
		ESC("\n\n", told_len ? 2 : 0),
		ESC(told_len ? dstr_get(told) : nullptr, told_len),
		BYTES(f->result_end),
		RAW(attached.ptr, attached.len),
		BYTES(f->results_end),
		BYTES(f->end),
	};
	bool last = req->part + 1u == req->n_calls;
	return mcp_req_upload(req, p, last ? ARRAY_SIZE(p) : ARRAY_SIZE(p) - 3);
}

/* Write the result of the turn's call whose turn it is into the upload, a
 * slice a step, then its line of the trace; then let go of its output,
 * and go on with the calls. */
static void
mcp_req_result_write (struct mcp_req *req)
{
	struct mcp_call c = mcp_call_at(buf_peek(&req->calls).ptr + req->at);
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
	(void)mcp_req_trace(req, &c, req->args[req->part], mcp_slot_offer(slot)->trace);
	(void)mcp_req_trace_end(req, slot->failed, mcp_slot_output(slot).len);
	mcp_slot_drop(slot);
	slot->of = nullptr;
	dstr_clear(&slot->answer);
	slot->note_len = 0;
	slot->skip = slot->window = 0;
	if (req->agenda)
		dstr_clear(&req->agenda->told);
	req->at += c.size;
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
		if ((req->lent >> i & 1) &&
		    jb_push(lent, json_object_new_string(st->offers[i].name))) {
			json_object_put(lent);
			lent = nullptr;
		}
	struct span upload = buf_peek(&req->upload);
	uint64_t now = util_realtime();
	req->archive_deadline = st->ep->timeout_ms > 0
	                      ? now + (uint64_t)st->ep->timeout_ms * 1'000'000 : 0;
	struct json_object *state = jb_object(
		"version", json_object_new_int(1),
		"owner", json_object_new_uint64(mcp_req_owner(req)),
		"created", json_object_new_uint64(task ? task->created : now),
		"tool", json_object_new_string(st->offers[req->self.tool].name),
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
	bool cancel;
};

/* Restored jobs use today's permissions and identities; the saved mask is
 * expressed by tool names, because registry indices change with config. */
static tool_mask
mcp_archive_lent (struct mcp_req *req,
                  struct json_object *state)
{
	struct json_object *names = jb_get(state, "lent");
	tool_mask allowed = mcp_req_lent(req), saved = 0;
	size_t n = json_object_array_length(names);
	for (size_t k = 0; k < n; ++k) {
		size_t len = 0;
		char const *name = jb_str(json_object_array_get_idx(names, k), &len);
		uint32_t id = mcp_static_tool_of(req->mcp->st, name, len);
		if (id < TOOL_MAX)
			saved |= (tool_mask)1 << id;
	}
	return allowed & saved;
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
	bool cancel = wait->cancel;
	free(wait);
	wait = nullptr;
	if (!req)
		goto out;
	struct mcp_static const *st = req->mcp->st;
	size_t id_len = 0;
	char const *id = jb_str(jb_get(args, "job"), &id_len);
	struct ledger_task *found = ledger_find(st->ledger, mcp_req_owner(req), id, id_len);
	if (found) {
		ledger_task_put(found);
		(cancel ? mcp_slot_client_cancel : mcp_slot_client_result)(&req->self, args, true);
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
	char const *tool = jb_text(jb_get(state, "tool"));
	char const *problem = error ? "Cannot read the durable job journal."
	                     : !state || json_object_get_int(jb_get(state, "version")) != 1
	                     ? "No recoverable job of yours has that id."
	                     : json_object_get_uint64(jb_get(state, "owner")) != mcp_req_owner(req) ||
	                       !tool || strcmp(tool, st->offers[req->self.tool].name)
	                     ? "No job of yours from this tool has that id."
	                     : nullptr;
	if (problem)
		goto failed;
	uint64_t created = json_object_get_uint64(jb_get(state, "created"));
	if (end) {
		struct json_object *result = mcp_archive_json(end);
		size_t len = 0;
		char const *text = jb_str(jb_get(result, "result"), &len);
		int status = json_object_get_int(jb_get(result, "status"));
		struct ledger_task *task = nullptr;
		int e = EINVAL;
		if (text && status >= LEDGER_COMPLETED && status <= LEDGER_CANCELLED)
			task = ledger_restore(st->ledger, mcp_req_owner(req), LEDGER_JOB, req->self.tool,
			                       (struct span){ (unsigned char *)(uintptr_t)id, id_len },
			                       created, json_object_get_uint64(jb_get(result, "ended")),
			                       (enum ledger_status)status,
			                       (struct span){ (unsigned char *)(uintptr_t)text, len },
			                       nullptr, (struct handle){}, nullptr, &e);
		json_object_put(result);
		if (!task && e != EEXIST) {
			problem = "Cannot restore this job's saved result.";
			goto failed;
		}
		ledger_task_put(task);
		json_object_put(state);
		(cancel ? mcp_slot_client_cancel : mcp_slot_client_result)(&req->self, args, true);
		goto out;
	}
	if (json_object_get_int(jb_get(state, "api")) != st->ep->api ||
	    json_object_get_int(jb_get(state, "provider")) != st->ep->provider ||
	    json_object_get_uint64(jb_get(state, "endpoint")) != mcp_archive_endpoint(st)) {
		problem = "This job belongs to a different upstream endpoint, model or API.";
		goto failed;
	}
	if (json_object_get_boolean(jb_get(state, "agenda"))) {
		problem = "This interrupted job had background tools or timers. Their outcomes must be reconciled before continuing.";
		goto failed;
	}
	uint32_t intents = 0, results = 0;
	struct json_object *last_tool_result = nullptr;
	for (struct journal_record *r = checkpoint->next; r; r = r->next) {
		bool intent = !strcmp(r->kind, "tool-intent");
		if (!intent && strcmp(r->kind, "tool-result"))
			continue;
		struct json_object *obj = mcp_archive_json(r);
		int64_t k = json_object_get_int64(jb_get(obj, "index"));
		if (!obj || k < 0 || k >= JOB_CALLS ||
		    (!intent && !jb_text(jb_get(obj, "text")))) {
			json_object_put(obj);
			problem = "This job's tool journal is invalid; no calls were repeated.";
			goto failed;
		}
		if (intent) {
			if (json_object_get_boolean(jb_get(obj, "agenda"))) {
				json_object_put(obj);
				problem = "This interrupted turn started or used background tools or timers. Reconcile their outcomes before continuing.";
				goto failed;
			}
			intents |= UINT32_C(1) << k;
			json_object_put(obj);
		} else {
			results |= UINT32_C(1) << k;
			json_object_put(req->saved_tools[k]);
			req->saved_tools[k] = obj;
			last_tool_result = obj;
		}
	}
	if (intents & ~results) {
		problem = "A tool call was interrupted with an unknown outcome; reconcile its effects before continuing. AskAI has not repeated it.";
		goto failed;
	}
	/* Before the job is admitted again, a failure here leaves its
	 * checkpoint as it was, with no end record. */
	struct json_object *local = last_tool_result ? last_tool_result : state;
	size_t len = 0;
	char const *attached = jb_str(jb_get(local, "attached"), &len);
	if (attached && len &&
	    mcp_piece_write(&RAW(attached, len), 0, len, &req->attached, SIZE_MAX)) {
		problem = "Cannot restore this turn's saved attachments.";
		goto failed;
	}
	char const *request = jb_str(jb_get(state, "request"), &len);
	int64_t turn = json_object_get_int64(jb_get(state, "turn"));
	uint64_t prompt_at = json_object_get_uint64(jb_get(state, "prompt_at"));
	struct json_object *names = jb_get(state, "lent");
	if (!request || !len || turn < 1 || turn > UINT32_MAX || prompt_at > len ||
	    !json_object_is_type(names, json_type_array)) {
		problem = "This job's saved request is invalid; no provider request was sent.";
		goto failed;
	}
	int e = mcp_piece_write(&RAW(request, len), 0, len, &req->upload, SIZE_MAX);
	char const *trace = jb_str(jb_get(state, "trace"), &len);
	if (!e && trace && !dstr_cat_n(&req->trace, trace, len))
		e = ENOMEM;
	struct handle who = {};
	if (!e)
		e = loop_handle(loop, req, HANDLE_REQ, &who);
	struct ledger_task *task = !e
		? ledger_restore(st->ledger, mcp_req_owner(req), LEDGER_JOB, req->self.tool,
		                 (struct span){ (unsigned char *)(uintptr_t)id, id_len },
		                 created, 0, LEDGER_WORKING, (struct span){}, loop, who,
		                 mcp_on_task_cancel, &e) : nullptr;
	if (!task) {
		if (who.bits)
			loop_forget(loop, who);
		problem = "Cannot resume this job now; it may already be resuming on another request.";
		goto failed;
	}
	req->admitted = task;
	memcpy(req->archive, id, LEDGER_ID);
	req->turn = (uint32_t)turn - 1;
	req->prompt_at = prompt_at;
	req->archive_deadline = json_object_get_uint64(jb_get(state, "deadline"));
	req->lent = mcp_archive_lent(req, state);
	req->asks = (uint8_t)json_object_get_int(jb_get(state, "asks"));
	req->used = (uint32_t)json_object_get_int64(jb_get(state, "used"));
	req->context = (uint32_t)json_object_get_int64(jb_get(state, "context"));
	req->traced = (uint32_t)json_object_get_int64(jb_get(state, "traced"));
	req->reminded = (uint8_t)json_object_get_int(jb_get(state, "reminded"));
	req->forced = (uint8_t)json_object_get_int(jb_get(state, "forced"));
	req->branch = json_object_get_boolean(jb_get(local, "branch"));
	req->filing = json_object_get_boolean(jb_get(local, "filing"));
	req->filed = json_object_get_uint64(jb_get(local, "filed"));
	req->replay_filed = last_tool_result ? req->filed : 0;
	char const *thread = jb_str(jb_get(state, "thread"), &len);
	if (thread && notes_is_id(thread, len))
		memcpy(req->thread, thread, NOTES_ID);
	if (st->notes)
		notes_repo(st->notes, req->principal, NOTES_UPSTREAM, req->repo);
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
	if (cancel)
		ledger_task_cancel(req->task);
	e = mcp_req_submit(req);
	if (e)
		mcp_req_fail(req, e);
	goto out;
failed:
	json_object_put(state);
	mcp_slot_result(&req->self, ESC(problem, strlen(problem)), true);
out:
	json_object_put(args);
	journal_records_destroy(&records);
}

/* Return true once this poll is waiting for disk, or has answered an
 * admission error. Simultaneous polls converge at ledger_restore(). */
static bool
mcp_req_recover (struct mcp_req *req,
                 struct json_object *args,
                 bool cancel)
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
		wait->cancel = cancel;
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
 * the prompt, if anything. */
static void
mcp_req_turn_ready (struct mcp_req *req)
{
	if (dstr_len(&req->self.answer))
		mcp_req_add_start(req);
	else
		mcp_req_turn_submit(req);
}

/* Write what askai adds to the prompt, in the request's answer, at the end
 * of the prompt's text: the conversation after it goes. */
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

/* Write what askai adds to the prompt, in the request's answer, or the
 * message that wakes the turns, what the agenda tells, a slice a step, and
 * end the prompt or the message and the messages; then submit the turn. */
static void
mcp_req_add_write (struct mcp_req *req)
{
	struct ai_format const *f = ai_format(req->mcp->st->ep->api);
	bool wake = req->phase == PHASE_WAKE;
	struct dstr *text = wake ? &req->agenda->told : &req->self.answer;
	struct mcp_piece const p[] = {
		wake ? BYTES(f->user) : (struct mcp_piece){},
		ESC(dstr_get(text), dstr_len(text)),
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

/* Append @p text to what askai adds to the prompt, then word to go on with
 * the task, and write it. */
static void
mcp_req_go_on (struct mcp_req   *req,
               struct mcp_piece  text)
{
	struct mcp_piece const p[] = { text, TEXT("[askai: go on with the task.]") };
	int e = mcp_slot_say(&req->self, p, ARRAY_SIZE(p));
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	mcp_req_add_start(req);
}

/* git has exited and askai has read all of its output.  The function
 * queues the step of the slot, which handles the output. */
static void
mcp_on_notes (struct child *c)
{
	mcp_slot_ready(c->owner);
}

/* The command of git for each enum mcp_fetch.  FETCH_NEWEST reads a note
 * with NOTES_READ. */
static enum notes_cmd const mcp_fetch_cmds[] = {
	[FETCH_CREATE]  = NOTES_CREATE,
	[FETCH_CHECK]   = NOTES_CHECK,
	[FETCH_ADD]     = NOTES_ADD,
	[FETCH_LIST]    = NOTES_LIST,
	[FETCH_READ]    = NOTES_READ,
	[FETCH_RESUME]  = NOTES_LIST,
	[FETCH_COMPACT] = NOTES_LIST,
	[FETCH_FULL]    = NOTES_LIST,
};

/* Continues the ask call after the notes for the prompt are in the
 * request's answer.  If @p fetch has FETCH_RESUME, the call goes on with a
 * thread and builds its upstream request.  After a compaction, the function
 * adds the line that tells the upstream to go on with the task. */
static void
mcp_req_notes_then (struct mcp_req *req,
                    uint16_t        fetch)
{
	if ((fetch & FETCH_KIND) == FETCH_RESUME)
		mcp_req_enter(req, PHASE_BUILD);
	else
		mcp_req_go_on(req, TEXT("\n"));
}

/* Handles the error @p e of spawning git for slot->fetch.  A compaction
 * goes on without the notes, and an ask call goes on without the newest
 * note.  A call of the notes tool, and an ask call that goes on with a
 * thread, end with the error. */
static void
mcp_slot_notes_failed (struct mcp_slot *slot,
                       int              e)
{
	struct mcp_req *req = slot->req;
	if (e != EBUSY)
		LOG(LOG_WARN, "notes: cannot run git: %s", strerror(e));
	if (slot->fetch & FETCH_NEWEST) {
		e = mcp_slot_say(slot, &TEXT("[askai: the note could not be read.]\n"), 1);
		if (e)
			mcp_req_fail(req, e);
		else
			mcp_req_notes_then(req, slot->fetch);
		return;
	}
	if ((slot->fetch & FETCH_KIND) == FETCH_COMPACT ||
	    (slot->fetch & FETCH_KIND) == FETCH_FULL) {
		mcp_req_go_on(req, TEXT("\n\n[askai: your notes could not be read.]\n\n"));
		return;
	}
	buf_fini(&req->body);
	if (e == EBUSY) {
		mcp_slot_result(slot, TEXT("Too many commands are running; try again in "
		                           "a few seconds."), true);
		return;
	}
	char why[96];
	mcp_slot_result(slot, ESC(why, util_format(why, sizeof why, "The notes cannot "
	                                           "be used now: %s.", strerror(e))), true);
}

/* The side of the notebook that the call in @p slot adds notes to: the
 * client's for a client's call, else the upstream's. */
static enum notes_side
mcp_slot_side (struct mcp_slot const *slot)
{
	return slot->role == ROLE_OWN ? NOTES_CLIENT : NOTES_UPSTREAM;
}

/* Writes the opposite side's branch of this participant and principal. */
static void
mcp_slot_other (struct mcp_slot const *slot,
                char                   repo[NOTES_REPO])
{
	enum notes_side side = mcp_slot_side(slot) == NOTES_CLIENT ? NOTES_UPSTREAM
	                                                          : NOTES_CLIENT;
	notes_repo(slot->req->mcp->st->notes, slot->req->principal, side, repo);
}

/* A read's explicitly selected notebook, or the server-selected own branch. */
static char const *
mcp_slot_notebook (struct mcp_slot const *slot)
{
	return *slot->notebook ? slot->notebook : slot->req->repo;
}

/* Spawns Git in the shared repository and frees its input. FETCH_OTHER
 * selects the slot's read branch; writes always use the request's own branch. */
static void
mcp_slot_notes_run (struct mcp_slot *slot,
                    uint16_t         fetch,
                    struct buf      *in)
{
	struct mcp_req *req = slot->req;
	struct mcp *mcp = req->mcp;
	struct loop *loop = mcp->loop;
	char const *repo = fetch & FETCH_OTHER ? mcp_slot_notebook(slot) : req->repo;
	enum notes_cmd cmd = (fetch & ~FETCH_COPY) == FETCH_FORMAT ? NOTES_FORMAT
	                   : fetch & FETCH_BOOKS ? NOTES_BOOKS
	                   : fetch & FETCH_FILES ? NOTES_FILES
	                   : fetch & FETCH_NEWEST ? NOTES_READ
	                   : mcp_fetch_cmds[fetch & FETCH_KIND];
	/* Git removes its own lock files on SIGTERM, and notes_lock() removes
	 * one that a crash left.  The lock file of a live writer, which may be
	 * outside askai, stays. */
	bool writes = cmd == NOTES_CREATE || cmd == NOTES_ADD;
	int64_t age;
	int e = cmd == NOTES_ADD ? notes_lock(mcp->st->notes, req->repo, &age) : 0;
	if (e)
		LOG(LOG_WARN, "notes: cannot check the lock of %s: %s",
		    notes_selector(req->repo), strerror(e));
	char const *argv[NOTES_ARGV];
	if (fetch & FETCH_REVISION)
		notes_revision_argv(slot->own, mcp->st->notes, mcp_slot_notebook(slot),
		                    fetch & FETCH_DIFF, argv);
	else if (cmd == NOTES_LIST && slot->own)
		notes_revision_history_argv(slot->own, mcp->st->notes, repo, argv);
	else
		notes_argv(mcp->st->notes, repo, cmd, argv);
	struct child_cmd const c = {
		.file = "git",
		.argv = argv,
		.envp = notes_env(mcp->st->notes),
		.in = in ? buf_peek(in) : (struct span){},
		.max = NOTES_MAX,
		.deadline = loop->now + NOTES_NS,
		.grace = writes ? NOTES_GRACE : 0,
		/* For the commands that write, the output includes the errors. */
		.flags = writes ? CHILD_STDERR : 0
	};
	slot->fetch = fetch;
	slot->child = child_create(loop, &mcp->children, &c, mcp_on_notes, slot, &e);
	if (in)
		buf_fini(in);
	if (!slot->child) {
		mcp_slot_notes_failed(slot, e);
		return;
	}
	mcp_slot_park(slot);
}

static void
mcp_slot_notes_drop (struct mcp_slot *slot)
{
	struct notes_revision *revision = slot->own;
	notes_revision_destroy(&revision);
	slot->own = nullptr;
}

/* Returns true if git exited with status 0 before its deadline, and askai
 * did not cut its output. */
static bool
mcp_notes_ok (struct child const *c)
{
	return !c->cut && !c->late && c->code == CLD_EXITED && !c->status;
}

/* Returns true if git wrote a list of the notes.  A list that askai cut
 * has the newest notes. */
static bool
mcp_notes_listed (struct child const *c)
{
	return c->cut || mcp_notes_ok(c);
}

/* Returns true if git exited with status 128.  It does so if the
 * repository or the branch of the notes does not exist. */
static bool
mcp_notes_none (struct child const *c)
{
	return !c->late && c->code == CLD_EXITED && c->status == 128;
}

/* Destroys the child of @p slot and ends the call of the notes tool.  If
 * @p e is not 0, the request fails with @p e.  Else the call ends with its
 * answer, as a failed call if @p failed is true. */
static void
mcp_slot_notes_end (struct mcp_slot *slot,
                    int              e,
                    bool             failed)
{
	child_destroy(&slot->child);
	if (e) {
		mcp_req_fail(slot->req, e);
		return;
	}
	/* The offset of a list or a read does not apply to the answer of a
	 * failed call. */
	if (failed)
		slot->skip = 0;
	mcp_slot_respond(slot, failed);
}

/* Appends to the answer of @p slot why git read no notes.  Exit status 128
 * means that the notebook does not exist: the other side keeps no notes in
 * the thread, or the caller has added none to it, or there is no such
 * thread. */
static int
mcp_slot_notes_unread (struct mcp_slot *slot)
{
	if (!mcp_notes_none(slot->child))
		return mcp_slot_say(slot, &TEXT("The notes could not be read."), 1);
	return mcp_slot_say(slot, &TEXT("This notebook has no notes yet."), 1);
}

/* Both ref updates and the first git init can contend across instances. */
static bool
mcp_notes_raced (struct child const *child)
{
	struct span const raced[] = {
		SPAN("Not updating "), SPAN("not updating "), SPAN(" but expected "),
		SPAN("File exists"),
	};
	struct span out = buf_peek(&child->out);
	for (size_t i = 0; i < ARRAY_SIZE(raced) && out.len; ++i)
		if (memmem(out.ptr, out.len, raced[i].ptr, raced[i].len))
			return true;
	return false;
}

/* Appends to the answer of @p slot why git did not write the note: git took
 * too long, a writer that died or hangs left the notebook locked, which the
 * function logs, another call changed the notebook at the same time, or
 * another error, which the function logs too. */
static int
mcp_slot_notes_unadded (struct mcp_slot *slot)
{
	struct child const *c = slot->child;
	struct span out = buf_peek(&c->out);
	if (c->late)
		return mcp_slot_say(slot, &TEXT("Could not write the note: it took too "
		                                "long."), 1);
	struct notes const *notes = slot->req->mcp->st->notes;
	char const *repo = slot->req->repo;
	int64_t age = 0;
	if (out.len && memmem(out.ptr, out.len, "File exists", sizeof "File exists" - 1) &&
	    !notes_lock(notes, repo, &age) && age > NOTES_STALE) {
		LOG(LOG_WARN, "notes: %s/%s.lock is %" PRId64 " s old; remove it if no "
		    "git process uses the repository", notes_directory(notes), repo, age);
		char said[128];
		return mcp_slot_say(slot, &ESC(said, util_format(said, sizeof said,
		                    "Could not write the note: its notebook has been "
		                    "locked for %" PRId64 " s.", age)), 1);
	}
	if (mcp_notes_raced(c))
		return mcp_slot_say(slot, &TEXT("Could not write the note, because another "
		                                "call changed this notebook at the same "
		                                "time. Read its current state and retry."), 1);
	char const *why = (char const *)out.ptr;
	LOG(LOG_WARN, "notes: git could not write a note: %.*s",
	    (int)mcp_clip(why, out.len, 300), why);
	return mcp_slot_say(slot, &TEXT("Could not write the note."), 1);
}

/* Runs git to add the note whose file part is in the request's body.  The
 * new commit has the parent @p tip, or no parent if @p tip is empty. */
static void
mcp_slot_notes_write (struct mcp_slot *slot,
                      char const      *tip)
{
	struct mcp_req *req = slot->req;
	if (!*tip) {
		mcp_slot_notes_run(slot, FETCH_FORMAT | (slot->fetch & FETCH_COPY), nullptr);
		return;
	}
	struct buf in = {};
	int e = notes_add_input(&in, req->repo, mcp_slot_side(slot), tip, buf_peek(&req->body));
	buf_fini(&req->body);
	if (e) {
		buf_fini(&in);
		mcp_req_fail(req, e);
		return;
	}
	mcp_slot_notes_run(slot, FETCH_ADD | (slot->fetch & (FETCH_COPY | FETCH_REPLACE | FETCH_RESTORE)),
	                   &in);
}

/* An explicit zero parent makes the first commit a root even if another
 * writer creates the branch between our name check and fast-import. */
static void
mcp_slot_notes_format (struct mcp_slot *slot)
{
	struct span out = buf_peek(&slot->child->out);
	size_t size = out.len == 5 && !memcmp(out.ptr, "sha1\n", 5) ? 40
	            : out.len == 7 && !memcmp(out.ptr, "sha256\n", 7) ? 64 : 0;
	if (!mcp_notes_ok(slot->child) || !size) {
		buf_fini(&slot->req->body);
		mcp_slot_notes_end(slot, mcp_slot_say(slot, &TEXT("Cannot determine the "
		                                                "repository object format."), 1), true);
		return;
	}
	char zero[NOTES_TIP + 1];
	memset(zero, '0', size);
	zero[size] = '\0';
	child_destroy(&slot->child);
	mcp_slot_notes_write(slot, zero);
}

/* Handles the end of git init for an add.  The function adds the note, or
 * ends the call if git failed. */
static void
mcp_slot_notes_created (struct mcp_slot *slot)
{
	if (mcp_notes_ok(slot->child) ||
	    (mcp_notes_raced(slot->child) && notes_exists(slot->req->mcp->st->notes))) {
		child_destroy(&slot->child);
		mcp_slot_notes_write(slot, "");
		return;
	}
	buf_fini(&slot->req->body);
	mcp_slot_notes_end(slot, mcp_slot_notes_unadded(slot), true);
}

/* Handles the output of the check of the name of a new note.  The function
 * adds the note if no note and no directory of notes has the name, and if
 * no directory in the name is a note. */
static void
mcp_slot_notes_checked (struct mcp_slot *slot)
{
	struct child const *c = slot->child;
	char tip[NOTES_TIP + 1];
	int r = mcp_notes_ok(c) ? notes_checked(buf_peek(&c->out), tip) : EIO;
	bool replace = slot->fetch & FETCH_REPLACE;
	if (replace)
		r = r == EEXIST ? 0 : r ? r : ENOENT;
	if (slot->fetch & FETCH_RESTORE) {
		if (r == EEXIST)
			r = 0;
		if (!r && strcmp(tip, notes_revision_tip(slot->own))) {
			buf_fini(&slot->req->body);
			mcp_slot_notes_end(slot, mcp_slot_say(slot, &TEXT("The notebook changed during "
			                                                "restore. Read its current state "
			                                                "and retry."), 1), true);
			return;
		}
	}
	if (!r) {
		child_destroy(&slot->child);
		mcp_slot_notes_write(slot, tip);
		return;
	}
	buf_fini(&slot->req->body);
	int e = r == EEXIST ? mcp_slot_say(slot, &TEXT("A note of that name exists. Use "
	                                               "action replace to update it, or "
	                                               "add under another name."), 1)
	      : r == ENOENT ? mcp_slot_say(slot, &TEXT("No note has that name. Use action "
	                                                "add to create it."), 1)
	      : r == EISDIR ? mcp_slot_say(slot, &TEXT("That name is a directory of "
	                                               "notes. Add the note under "
	                                               "another name."), 1)
	      : r == ENOTDIR ? mcp_slot_say(slot, &TEXT("A directory in that name is a "
	                                                "note. Add the note under "
	                                                "another name."), 1)
	      : mcp_slot_notes_unread(slot);
	mcp_slot_notes_end(slot, e, true);
}

/* Reports the end of git fast-import and records that the own branch exists. */
static void
mcp_slot_notes_added (struct mcp_slot *slot)
{
	struct mcp_req *req = slot->req;
	bool ok = mcp_notes_ok(slot->child);
	req->branch |= ok;
	int e = !ok ? mcp_slot_notes_unadded(slot)
	      : slot->fetch & FETCH_RESTORE ? mcp_slot_say(slot, &TEXT("Restored in a new commit. "
	                                                            "Previous versions remain in history."), 1)
	      : slot->fetch & FETCH_COPY ? mcp_slot_say(slot, &TEXT("Copied into your "
	                                                         "notebook."), 1)
	      : slot->fetch & FETCH_REPLACE ? mcp_slot_say(slot, &TEXT("Replaced. Previous "
	                                                            "versions remain in history."), 1)
	      : mcp_slot_say(slot, &TEXT("Noted."), 1);
	mcp_slot_notes_end(slot, e, !ok);
}

/* Appends the list of the notes in @p out, the output of git for
 * NOTES_LIST, to the answer of @p slot, the oldest note first, and stores
 * the name of the newest note in @p *newest.  @p cut is true if askai cut
 * the output of git, which leaves the oldest notes out.  Returns 0 or ENOMEM. */
static int
mcp_slot_notes_listed (struct mcp_slot *slot,
                       struct span      out,
                       bool             cut,
                       struct span     *newest)
{
	struct dstr *d = &slot->answer;
	*newest = (struct span){};
	if (cut && !CAT(d, "[The oldest notes are not in this list.]\n"))
		return ENOMEM;
	struct notes_cursor cursor = { .end = out.len };
	struct notes_entry entry;
	bool history = (slot->fetch & FETCH_KIND) == FETCH_LIST;
	struct cstr_view filter = history && slot->own ? notes_revision_file(slot->own)
	                                             : (struct cstr_view){};
	uint32_t k = 0;
	while (notes_entry_before(out, &cursor, &entry)) {
		char const *name = (char const *)entry.name.ptr;
		/* Git also matches descendants if this path was once a directory. */
		if (filter.len && !cstr_view_equal(filter, (struct cstr_view){ name, entry.name.len }))
			continue;
		char when[sizeof "2026-10-14 12:34 UTC"];
		struct tm tm;
		time_t t = (time_t)entry.time;
		size_t w = gmtime_r(&t, &tm) ? strftime(when, sizeof when, "%Y-%m-%d %H:%M UTC",
		                                        &tm) : 0;
		if (!dstr_printf(d, "%" PRIu32 ". %.*s  ", ++k, (int)w, when) ||
		    !dstr_cat_n(d, name, jb_utf8(name, entry.name.len)) ||
		    !(entry.lines == NOTES_BINARY ? CAT(d, "  (binary)")
		      : dstr_printf(d, "  (%" PRIu64 " line%s)", entry.lines,
		                    entry.lines == 1 ? "" : "s")) ||
		    (history && !dstr_printf(d, "  commit %.*s", (int)entry.commit.len,
		                            (char const *)entry.commit.ptr)) || !CAT(d, "\n"))
			return ENOMEM;
		*newest = entry.name;
	}
	loop_work(slot->req->mcp->loop, LOOP_COPY, out.len);
	return 0;
}

/** Shows the selected notebook's current files or its change history. */
static void
mcp_slot_notes_got_list (struct mcp_slot *slot)
{
	struct child const *c = slot->child;
	bool ok = mcp_notes_listed(c);
	if (!ok) {
		mcp_slot_notes_end(slot, mcp_slot_notes_unread(slot), true);
		return;
	}
	bool files = slot->fetch & FETCH_FILES;
	struct dstr *d = &slot->answer;
	int e = dstr_printf(d, "Notebook %s — %s:\n\n",
	                    notes_selector(mcp_slot_notebook(slot)),
	                    files ? "current files (bytes, blob)" : "history, oldest first (lines added, commit)")
	      ? 0 : ENOMEM;
	struct span out = buf_peek(&c->out);
	if (!files) {
		struct span newest;
		e = e ? e : mcp_slot_notes_listed(slot, out, c->cut, &newest);
	} else {
		while (!e && out.len) {
			struct notes_file file;
			int r = notes_file_next(&out, &file);
			if (r) {
				if (!c->cut)
					e = mcp_slot_say(slot, &TEXT("Cannot decode the notebook file list."), 1);
				ok = c->cut;
				break;
			}
			char const *name = (char const *)file.name.ptr;
			if (!dstr_printf(d, "%.*s  (%" PRIu64 " bytes, %.*s)\n",
			                 (int)jb_utf8(name, file.name.len), name, file.bytes,
			                 (int)file.id.len, (char const *)file.id.ptr))
				e = ENOMEM;
		}
		if (!e && c->cut)
			e = mcp_slot_say(slot, &TEXT("[The file list exceeded the capture limit "
			                             "and is incomplete.]\n"), 1);
		loop_work(slot->req->mcp->loop, LOOP_COPY, buf_peek(&c->out).len);
	}
	mcp_slot_notes_end(slot, e, !ok);
}

/** Identifies the server-selected writer, including an unborn notebook. */
static int
mcp_slot_notes_identity (struct mcp_slot *slot)
{
	return dstr_printf(&slot->answer, "Self (writable): %s\n",
	                   notes_selector(slot->req->repo)) ? 0 : ENOMEM;
}

/* Lists discoverable branch selectors. Git resolves both loose and packed refs. */
static void
mcp_slot_notes_got_books (struct mcp_slot *slot)
{
	struct child const *c = slot->child;
	if (!mcp_notes_listed(c)) {
		mcp_slot_notes_end(slot, mcp_slot_notes_unread(slot), true);
		return;
	}
	struct span out = buf_peek(&c->out);
	int e = mcp_slot_say(slot, &TEXT("Session notebooks (use a selector as notebook):\n"), 1);
	if (!e)
		e = mcp_slot_notes_identity(slot);
	char const *own = notes_selector(slot->req->repo);
	size_t own_len = strlen(own);
	while (!e && out.len) {
		unsigned char const *end = memchr(out.ptr, '\n', out.len);
		if (!end)
			break;
		size_t len = (size_t)(end - out.ptr);
		char const *name = (char const *)out.ptr;
		char repo[NOTES_REPO];
		if ((len != own_len || memcmp(out.ptr, own, len)) &&
		    !notes_select(name, len, repo))
			e = dstr_printf(&slot->answer, "%.*s (read-only)\n",
			                (int)len, name)
			  ? 0 : ENOMEM;
		size_t n = len + 1;
		out.ptr += n;
		out.len -= n;
	}
	if (!e && c->cut)
		e = mcp_slot_say(slot, &TEXT("[The notebook list is incomplete.]\n"), 1);
	mcp_slot_notes_end(slot, e, false);
}

/* The marker of a byte that is not shown as text in a note's view. */
static char const view_open[] = "\xe2\x9f\xa6";   /* U+27E6 */
static char const view_close[] = "\xe2\x9f\xa7";  /* U+27E7 */

/* Whether the @p len bytes at @p s are UTF-8 text without a NUL, which a
 * note shows as it is, and not as its view (mcp_notes_view()). */
static bool
mcp_notes_text (char const *s,
                size_t      len)
{
	return !len || (jb_utf8(s, len) == len && !memchr(s, '\0', len));
}

/* Appends the @p n bytes at @p p of a view to @p d while the @p *size bytes
 * of the view before them are fewer than @p max, and counts them in
 * @p *size.  Returns 0 or ENOMEM. */
static int
mcp_notes_put (struct dstr *d,
               size_t      *size,
               size_t       max,
               char const  *p,
               size_t       n)
{
	bool put = *size < max;
	*size += n;
	return put && !dstr_cat_n(d, p, n) ? ENOMEM : 0;
}

/* Appends to @p d the view of a note of the @p len bytes at @p s that is not
 * UTF-8 text, or that has a NUL byte: a line that says how to read the
 * view, then the bytes as UTF-8 where they are valid UTF-8, each other byte
 * and each NUL as ⟦XX⟧, its value in hexadecimal, and each ⟦ of the text as
 * ⟦⟦.  The view tells every byte of the note, which stays as it is.  Only
 * the start of the view goes into @p d, up to the first piece that ends at
 * or past @p max bytes, and the length of the whole view goes into
 * @p *size, unless @p size is nullptr.  Returns 0 or ENOMEM. */
static int
mcp_notes_view (struct dstr *d,
                char const  *s,
                size_t       len,
                size_t       max,
                size_t      *size)
{
	static char const hex[] = "0123456789ABCDEF";
	size_t const open = sizeof view_open - 1;
	size_t const close = sizeof view_close - 1;
	char head[320];
	size_t w = 0;
	int e = mcp_notes_put(d, &w, max, head, util_format(head, sizeof head,
	        "[askai: the note is not UTF-8 text: %zu bytes.  Each byte that is not "
	        "part of UTF-8 text, and each NUL, is shown as %sXX%s, its value in "
	        "hexadecimal, and a %s of the text as %s%s.  The note itself is not "
	        "changed.]\n\n", len, view_open, view_close, view_open, view_open,
	        view_open));
	for (size_t i = 0; !e && i < len; ) {
		size_t run = jb_utf8(s + i, len - i);
		char const *nul = memchr(s + i, '\0', run);
		if (nul)
			run = (size_t)(nul - s) - i;
		/* The valid text, with each ⟦ in it doubled. */
		for (size_t at = i; !e && at < i + run; ) {
			char const *m = memmem(s + at, i + run - at, view_open, open);
			size_t n = m ? (size_t)(m - s) - at + open : i + run - at;
			e = mcp_notes_put(d, &w, max, s + at, n);
			e = e || !m ? e : mcp_notes_put(d, &w, max, view_open, open);
			at += n;
		}
		i += run;
		if (e || i == len)
			break;
		unsigned char b = (unsigned char)s[i++];
		char x[] = { hex[b >> 4], hex[b & 15] };
		e = mcp_notes_put(d, &w, max, view_open, open);
		e = e ? e : mcp_notes_put(d, &w, max, x, 2);
		e = e ? e : mcp_notes_put(d, &w, max, view_close, close);
	}
	if (size)
		*size = w;
	return e;
}

static void mcp_slot_notes_got_copy (struct mcp_slot *slot, struct span text);
static void mcp_slot_notes_got_attach (struct mcp_slot *slot, struct span text,
                                       struct span id);

/* Resolve and authorize every revision before reading any of its contents. */
static void
mcp_slot_notes_revision_step (struct mcp_slot *slot)
{
	if (!mcp_notes_ok(slot->child) ||
	    notes_revision_next(slot->own, buf_peek(&slot->child->out))) {
		mcp_slot_notes_end(slot, mcp_slot_say(slot, &TEXT("Cannot use that revision in this "
		                                                "notebook. Use a full commit ID from "
		                                                "its history; the notebook must exist "
		                                                "and the revision must belong to it."), 1), true);
		return;
	}
	child_destroy(&slot->child);
	struct buf in = {};
	int e = 0;
	if (notes_revision_ready(slot->own) && !(slot->fetch & FETCH_DIFF)) {
		struct cstr_view file = notes_revision_file(slot->own);
		e = notes_read_input(&in, notes_revision_from(slot->own), file.data, file.len);
	}
	if (e) {
		buf_fini(&in);
		mcp_req_fail(slot->req, e);
		return;
	}
	mcp_slot_notes_run(slot, slot->fetch, &in);
}

static void
mcp_slot_notes_got_diff (struct mcp_slot *slot)
{
	struct child const *c = slot->child;
	if (!mcp_notes_listed(c)) {
		mcp_slot_notes_end(slot, mcp_slot_notes_unread(slot), true);
		return;
	}
	struct cstr_view file = notes_revision_file(slot->own);
	struct span out = buf_peek(&c->out);
	struct dstr *d = &slot->answer;
	int e = dstr_printf(d, "Notebook %s — %.*s\nfrom %s\nto %s\n\n",
	                    notes_selector(mcp_slot_notebook(slot)), (int)file.len, file.data,
	                    notes_revision_from(slot->own), notes_revision_to(slot->own)) ? 0 : ENOMEM;
	char const *text = (char const *)out.ptr;
	if (!e)
		e = !out.len ? mcp_slot_say(slot, &TEXT("No differences.\n"), 1)
		  : mcp_notes_text(text, out.len) ? mcp_slot_say(slot, &ESC(text, out.len), 1)
		  : mcp_notes_view(d, text, out.len, SIZE_MAX, nullptr);
	if (!e && c->cut)
		e = mcp_slot_say(slot, &TEXT("\n[The diff exceeded the capture limit and is incomplete.]\n"), 1);
	loop_work(slot->req->mcp->loop, LOOP_COPY, out.len);
	mcp_slot_notes_end(slot, e, false);
}

/* Answers the read action with the output of git cat-file, or goes on with
 * the copy action or the attach action. */
static void
mcp_slot_notes_got_note (struct mcp_slot *slot)
{
	struct child const *c = slot->child;
	struct span text = {};
	struct span id = {};
	int r = !mcp_notes_ok(c) ? EIO
	      : slot->fetch & FETCH_RESTORE ? notes_read_full_out(buf_peek(&c->out), &text, &id)
	      : notes_read_out(buf_peek(&c->out), &text, &id);
	if (!r && (slot->fetch & FETCH_RESTORE) && text.len > NOTES_TEXT) {
		mcp_slot_notes_end(slot, mcp_slot_say(slot, &TEXT("That revision exceeds the 4 MiB "
		                                                "note size limit; it was not restored."), 1), true);
		return;
	}
	if (!r && slot->fetch & (FETCH_COPY | FETCH_RESTORE)) {
		mcp_slot_notes_got_copy(slot, text);
		return;
	}
	if (!r && slot->fetch & FETCH_ATTACH) {
		mcp_slot_notes_got_attach(slot, text, id);
		return;
	}
	char const *s = (char const *)text.ptr;
	int e = 0;
	if (!r && (slot->fetch & FETCH_REVISION)) {
		struct cstr_view file = notes_revision_file(slot->own);
		e = dstr_printf(&slot->answer, "Notebook %s — %.*s at %s:\n\n",
		                 notes_selector(mcp_slot_notebook(slot)), (int)file.len, file.data,
		                 notes_revision_from(slot->own)) ? 0 : ENOMEM;
	}
	e = e ? e : !r && !mcp_notes_text(s, text.len)
	      ? mcp_notes_view(&slot->answer, s, text.len, SIZE_MAX, nullptr)
	      : !r ? mcp_slot_say(slot, &ESC(s, text.len), 1)
	      : r == ENOENT ? mcp_slot_say(slot, &TEXT("No note has that name."), 1)
	      : r == EISDIR ? mcp_slot_say(slot, &TEXT("That name is a directory of "
	                                               "notes. list shows the notes in "
	                                               "it."), 1)
	      : mcp_slot_notes_unread(slot);
	mcp_slot_notes_end(slot, e, r);
}

/* Adds the file whose URI is the @p len bytes at @p uri, of @p bytes bytes,
 * to the files that the turn attached, as its API's fragments have them
 * (ai_format()).  The URI is printable ASCII (ai_uploaded()).  Returns 0 or
 * ENOMEM. */
static int
mcp_req_attach (struct mcp_req *req,
                char const     *uri,
                size_t          len,
                size_t          bytes)
{
	struct ai_format const *f = ai_format(req->mcp->st->ep->api);
	/* A turn has JOB_CALLS calls, so a few KiB of files at most. */
	struct span to;
	int e = buf_reserve(&req->attached, f->file.len + 6 * len + f->file_end.len,
	                    SIZE_MAX, &to);
	if (e)
		return ENOMEM;
	char *p = (char *)to.ptr;
	memcpy(p, f->file.ptr, f->file.len);
	size_t n = f->file.len + jb_escape(p + f->file.len, uri, len);
	memcpy(p + n, f->file_end.ptr, f->file_end.len);
	buf_commit(&req->attached, n + f->file_end.len);
	req->filed += bytes;
	req->filing = true;
	return 0;
}

/* The turns' files of a conversation that failed may be gone from the
 * upstream's store, so their notes are uploaded again. */
static void
mcp_req_unfile (struct mcp_req *req)
{
	if (req->filing)
		uploads_forget(req->mcp->st->uploads, mcp_req_owner(req));
}

/* Sends the @p len bytes at @p body to the upstream's file store, as the
 * HTTP request of @p slot's call: 0, or the error of mcp_slot_send(), such
 * as EAGAIN if askai sends too many requests at once. */
static int
mcp_slot_post_file (struct mcp_slot *slot,
                    char const      *body,
                    size_t           len)
{
	static char const accept[] = "application/json";
	struct mcp_static const *st = slot->req->mcp->st;
	struct web_request const request = {
		.url = st->files_url,
		.body = body,
		.len = len,
		.accept = accept,
		.max = 64 << 10,
		.timeout = WEB_TIMEOUT_MAX
	};
	struct web_ask ask = {
		.request = &request,
		.url = strlen(st->files_url),
		.accept = sizeof accept - 1,
		.lines = st->files_lines
	};
	return mcp_slot_send(slot, &ask);
}

/* Fails the attach action of @p slot, and returns EFBIG, if the context of
 * its ask call has no room for a file of @p bytes bytes, about a quarter as
 * many tokens; else returns 0.  An unknown context is not checked. */
static int
mcp_slot_notes_fits (struct mcp_slot *slot,
                     size_t           bytes)
{
	struct mcp_req *req = slot->req;
	size_t used = mcp_req_tokens(req);
	if (!req->context || used + bytes / 4 < req->context)
		return 0;
	dstr_clear(&slot->answer);
	bool said = dstr_printf(&slot->answer, "The note is too large to attach: its "
	                        "file of %zu bytes, about %zu tokens, does not fit the "
	                        "context of %" PRIu32 " tokens, of which about %zu are "
	                        "used. read shows it in parts.", bytes, bytes / 4,
	                        req->context, used);
	mcp_slot_notes_end(slot, said ? 0 : ENOMEM, true);
	return EFBIG;
}

/* Says in the answer of @p slot that the note of @p len bytes follows the
 * results as a file of @p bytes bytes, its view if @p view: 0 or ENOMEM. */
static int
mcp_slot_notes_attached (struct mcp_slot *slot,
                         size_t           len,
                         size_t           bytes,
                         bool             view)
{
	dstr_clear(&slot->answer);
	bool said = view ? dstr_printf(&slot->answer, "The note, %zu bytes, is not UTF-8 "
	                               "text, so its view, %zu bytes, about %zu tokens, "
	                               "in which each other byte is %sXX%s, follows the "
	                               "results of this turn as a file.", len, bytes,
	                               bytes / 4, view_open, view_close)
	                 : dstr_printf(&slot->answer, "The note, %zu bytes, about %zu "
	                               "tokens, follows the results of this turn as a "
	                               "file.", len, bytes / 4);
	return said ? 0 : ENOMEM;
}

/* Attaches @p text, the note whose object id is @p id, to the turn: the
 * file that askai uploaded for the note and its owner, if it lasts, or else
 * a new upload of the note, or of its view if it is not UTF-8 text, if the
 * context has room for it.  The length of the upload, the note's and the
 * object id stay in req->body until the upload's answer comes. */
static void
mcp_slot_notes_got_attach (struct mcp_slot *slot,
                           struct span      text,
                           struct span      id)
{
	struct mcp_req *req = slot->req;
	struct mcp_static const *st = req->mcp->st;
	char const *s = (char const *)text.ptr;
	bool view = !mcp_notes_text(s, text.len);
	char uri[UPLOADS_URI];
	size_t len = 0;
	size_t n = uploads_find(st->uploads, mcp_req_owner(req), (char const *)id.ptr,
	                        id.len, (int64_t)(util_realtime() / NS), uri, &len);
	if (n) {
		if (mcp_slot_notes_fits(slot, len))
			return;
		LOG(LOG_VERBOSE, "upstream file %.*s attached again", (int)n, uri);
		int e = mcp_req_attach(req, uri, n, len);
		mcp_slot_notes_end(slot, e ? e : mcp_slot_notes_attached(slot, text.len, len,
		                                                         view), false);
		return;
	}
	/* The length of a view is counted before any of it is made, so that
	 * a view longer than askai uploads is not made. */
	dstr_auto shown = DSTR_INIT;
	len = text.len;
	int e = view ? mcp_notes_view(&shown, s, text.len, 0, &len) : 0;
	if (!e && view && len <= NOTES_MAX)
		e = mcp_notes_view(&shown, s, text.len, SIZE_MAX, nullptr);
	char const *body = view ? dstr_get(&shown) : s;
	if (!e && len > NOTES_MAX) {
		dstr_clear(&slot->answer);
		e = dstr_printf(&slot->answer, "The note is too large to attach: its file "
		                "would have %zu bytes, and askai uploads %d MiB at most.",
		                len, NOTES_MAX >> 20) ? 0 : ENOMEM;
		mcp_slot_notes_end(slot, e, true);
		return;
	}
	if (!e && mcp_slot_notes_fits(slot, len))
		return;
	/* The table keeps no id that is too long, so none is kept then. */
	size_t const sizes[] = { len, text.len };
	size_t kept = id.len <= UPLOADS_ID ? id.len : 0;
	buf_fini(&req->body);
	struct span to;
	if (!e && !(e = buf_reserve(&req->body, sizeof sizes + kept,
	                            sizeof sizes + UPLOADS_ID, &to))) {
		memcpy(to.ptr, sizes, sizeof sizes);
		memcpy(to.ptr + sizeof sizes, id.ptr, kept);
		buf_commit(&req->body, sizeof sizes + kept);
	}
	e = e ? e : mcp_slot_post_file(slot, body, len);
	if (e == EAGAIN) {
		dstr_clear(&slot->answer);
		e = CAT(&slot->answer, "askai sends too many requests at once to upload the "
		        "note now. Try again later.") ? 0 : ENOMEM;
		mcp_slot_notes_end(slot, e, true);
		return;
	}
	if (e) {
		mcp_slot_notes_end(slot, e, true);
		return;
	}
	child_destroy(&slot->child);
	mcp_slot_park(slot);
}

/* Takes the answer to the upload of the attach action: puts the file into
 * the table of uploads and attaches it to the turn, or says why the upload
 * failed. */
static void
mcp_slot_notes_uploaded (struct mcp_slot *slot)
{
	struct mcp_req *req = slot->req;
	struct mcp_static const *st = req->mcp->st;
	struct web_response r;
	int e = mcp_slot_fetched(slot, &r);
	if (e == EINPROGRESS) {
		mcp_slot_park(slot);
		return;
	}
	size_t sizes[2] = {};
	struct span kept = buf_peek(&req->body);
	struct span id = {};
	if (kept.len >= sizeof sizes) {
		memcpy(sizes, kept.ptr, sizeof sizes);
		id = (struct span){ kept.ptr + sizeof sizes, kept.len - sizeof sizes };
	}
	dstr_auto uri = DSTR_INIT;
	dstr_auto why = DSTR_INIT;
	int64_t expires = 0;
	if (!e)
		e = r.e ? (dstr_cat(&why, r.error) ? EIO : ENOMEM)
		    : ai_uploaded(st->ep->api, r.status, r.body, r.len, &uri, &expires, &why);
	if (!e && dstr_len(&uri) > UPLOADS_URI)
		e = dstr_printf(&why, "The upstream named the file with a URI of more "
		                "than %d bytes.", UPLOADS_URI) ? EIO : ENOMEM;
	if (!e) {
		LOG(LOG_VERBOSE, "upstream file %s uploaded: %zu bytes", dstr_get(&uri),
		    sizes[0]);
		e = mcp_req_attach(req, dstr_get(&uri), dstr_len(&uri), sizes[0]);
		if (!e && expires)
			(void)uploads_keep(st->uploads, mcp_req_owner(req), (char const *)id.ptr,
			                   id.len, dstr_get(&uri), dstr_len(&uri), sizes[0],
			                   expires, (int64_t)(util_realtime() / NS));
		/* A view is longer than its note, by its first line at least. */
		e = e ? e : mcp_slot_notes_attached(slot, sizes[1], sizes[0],
		                                    sizes[0] != sizes[1]);
	}
	buf_fini(&req->body);
	if (e == EIO) {
		dstr_clear(&slot->answer);
		e = CAT(&slot->answer, "The upload of the note as a file failed: ") &&
		    dstr_cat_n(&slot->answer, dstr_get(&why), dstr_len(&why)) ? 0 : ENOMEM;
		mcp_slot_notes_end(slot, e, true);
		return;
	}
	mcp_slot_notes_end(slot, e, false);
}

/* Appends @p head and the list of the notes in the output of git to the
 * request's answer, which askai adds to the prompt.  Then the function
 * runs git to read the newest note.  If there are no notes, the ask call
 * continues. */
static void
mcp_slot_notes_prompt (struct mcp_slot  *slot,
                       struct mcp_piece  head)
{
	struct mcp_req *req = slot->req;
	struct child const *c = slot->child;
	struct span newest = {};
	struct buf in = {};
	int e = mcp_slot_say(slot, &head, 1);
	if (!e)
		e = mcp_slot_notes_listed(slot, buf_peek(&c->out), c->cut, &newest);
	char const *name = (char const *)newest.ptr;
	struct mcp_piece const p[] = {
		TEXT("\n[askai: the current text of your most recently written note, "),
		ESC(name, newest.len ? jb_utf8(name, newest.len) : 0),
		TEXT(":]\n\n"),
	};
	/* The name comes from git, and git reads the note only if the name is
	 * one that an add would take. */
	size_t skip = 0;
	bool named = newest.len && !notes_name(name, newest.len, &skip) && !skip;
	if (!e && named && !(e = mcp_slot_say(slot, p, ARRAY_SIZE(p))))
		e = notes_read_input(&in, req->repo, name, newest.len);
	if (!e && newest.len && !named)
		e = mcp_slot_say(slot, &TEXT("\n[askai: the newest note could not be "
		                             "read.]\n"), 1);
	child_destroy(&slot->child);
	if (e) {
		buf_fini(&in);
		mcp_req_fail(req, e);
		return;
	}
	if (!named) {
		mcp_req_notes_then(req, slot->fetch);
		return;
	}
	mcp_slot_notes_run(slot, slot->fetch | FETCH_NEWEST, &in);
}

/* Appends the newest note, which git read for the prompt, to the request's
 * answer, or else a line with the text that the note could not be read.  A
 * note longer than a tool's output in a turn (mcp_req_lend_max()) is cut
 * there, with a line that says how to read on.  Then the ask call
 * continues. */
static void
mcp_slot_notes_newest (struct mcp_slot *slot)
{
	struct mcp_req *req = slot->req;
	struct child const *c = slot->child;
	struct span text = {};
	int r = mcp_notes_ok(c) ? notes_read_out(buf_peek(&c->out), &text, nullptr) : EIO;
	char const *s = (char const *)text.ptr;
	size_t len = text.len;
	/* A note that is not UTF-8 text goes in as its view, of which only the
	 * start that the prompt takes is made. */
	size_t most = mcp_req_lend_max(req);
	size_t shown = len;
	dstr_auto view = DSTR_INIT;
	if (!r && !mcp_notes_text(s, text.len)) {
		if (mcp_notes_view(&view, s, text.len, most + 1, &len)) {
			child_destroy(&slot->child);
			mcp_req_fail(req, ENOMEM);
			return;
		}
		s = dstr_get(&view);
		shown = dstr_len(&view);
	}
	size_t n = mcp_clip(s, shown, most);
	char more[128];
	size_t m = n < len ? util_format(more, sizeof more, "\n[askai: bytes 0 to %zu of "
	                                 "%zu; read the note with \"offset\": %zu for the "
	                                 "rest.]", n, len, n) : 0;
	struct mcp_piece const p[] = {
		r ? TEXT("[askai: the note could not be read.]") : ESC(s, n),
		ESC(more, m),
		TEXT("\n"),
	};
	int e = mcp_slot_say(slot, p, n && s[n - 1] == '\n' && !m ? 1 : 3);
	child_destroy(&slot->child);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	mcp_req_notes_then(req, slot->fetch);
}

/* Handles the list of the notes of the thread that the ask call goes on
 * with.  The list goes into the prompt.  If git failed, the call ends with
 * an error. */
static void
mcp_slot_notes_resumed (struct mcp_slot *slot)
{
	struct mcp_req *req = slot->req;
	struct child *c = slot->child;
	if (!mcp_notes_listed(c)) {
		bool none = mcp_notes_none(c);
		child_destroy(&slot->child);
		if (none) {
			mcp_req_notes_then(req, FETCH_RESUME);
			return;
		}
		mcp_slot_result(slot, TEXT("Cannot read the session notes."), true);
		return;
	}
	req->branch = true;
	mcp_slot_notes_prompt(slot, TEXT("\n\n[askai: your session note history, "
	                                 "oldest first; repeated names are updates. "
	                                 "Use notes action list for current files and notebooks "
	                                 "to discover the other participants:]\n\n"));
}

/* Handles the list of the notes for a compaction.  The list goes into the
 * prompt, or else a line with the text that the notes could not be read. */
static void
mcp_slot_notes_compacted (struct mcp_slot *slot)
{
	struct mcp_req *req = slot->req;
	if (mcp_notes_listed(slot->child)) {
		mcp_slot_notes_prompt(slot, TEXT("\n\n[askai: your note history, oldest first; "
		                                 "repeated names are updates:]\n\n"));
		return;
	}
	child_destroy(&slot->child);
	int e = mcp_slot_say(slot, &TEXT("\n\n[askai: your notes could not be read.]\n"), 1);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	mcp_req_go_on(req, TEXT("\n"));
}

/* Handles the output of git for the command in slot->fetch. */
static void
mcp_slot_notes_out (struct mcp_slot *slot)
{
	static void (*const steps[]) (struct mcp_slot *slot) = {
		[FETCH_CREATE]  = mcp_slot_notes_created,
		[FETCH_CHECK]   = mcp_slot_notes_checked,
		[FETCH_ADD]     = mcp_slot_notes_added,
		[FETCH_LIST]    = mcp_slot_notes_got_list,
		[FETCH_READ]    = mcp_slot_notes_got_note,
		[FETCH_RESUME]  = mcp_slot_notes_resumed,
		[FETCH_COMPACT] = mcp_slot_notes_compacted,
		[FETCH_FULL]    = mcp_slot_notes_compacted,
	};
	mcp_slot_unwait(slot);
	/* The attach action's upload, which follows its read, has answered. */
	(slot->web ? mcp_slot_notes_uploaded
	 : (slot->fetch & FETCH_REVISION) && !notes_revision_ready(slot->own)
	 ? mcp_slot_notes_revision_step
	 : slot->fetch & FETCH_DIFF ? mcp_slot_notes_got_diff
	 : (slot->fetch & ~FETCH_COPY) == FETCH_FORMAT ? mcp_slot_notes_format
	 : slot->fetch & FETCH_BOOKS ? mcp_slot_notes_got_books
	 : slot->fetch & FETCH_NEWEST ? mcp_slot_notes_newest
	 : steps[slot->fetch & FETCH_KIND])(slot);
}

/* The message for a file argument that is not a valid name. */
static char const bad_name[] = "The file argument must be a file name of 1 to 255 "
                               "bytes, such as plan.md or findings/locks.md. It "
                               "cannot have control characters or empty parts, "
                               "and no part can be ., .., .git or git~1, in any "
                               "case and also with spaces or dots after it.";

/* Writes to @p in the file part of a note of the @p name_len bytes at
 * @p name whose text is the data of @p d, decoded. */
static int
mcp_notes_add_data (struct buf           *in,
                    char const           *name,
                    size_t                name_len,
                    struct datauri const *d)
{
	int e = notes_add_head(in, name, name_len, d->size);
	struct span s;
	if (!e)
		e = buf_reserve(in, d->size + 1, SIZE_MAX, &s);
	if (e)
		return e;
	datauri_decode(d, s.ptr);
	s.ptr[d->size] = '\n';
	buf_commit(in, d->size + 1);
	return 0;
}

/** Writes a versioned note on the caller's branch, initializing the shared
 * repository if needed. */
static void
mcp_slot_notes_add (struct mcp_slot    *slot,
                    struct json_object *args,
                    uint16_t            flags)
{
	static_assert(NOTES_TEXT == 4 << 20, "the problem says");
	struct mcp_req *req = slot->req;
	bool initialized = notes_exists(req->mcp->st->notes);
	size_t most = req->mcp->st->files_inline;
	char big[96];
	size_t len = 0;
	size_t name_len = 0;
	size_t skip = 0;
	size_t uri_len = 0;
	struct json_object *content = jb_get(args, "content");
	char const *text = jb_str(jb_get(args, "text"), &len);
	char const *name = jb_str(jb_get(args, "file"), &name_len);
	char const *uri = jb_str(content, &uri_len);
	struct datauri data = {};
	struct mcp_piece problem =
		!name || notes_name(name, name_len, &skip) ? TEXT(bad_name)
		: content && slot->role != ROLE_OWN
		? TEXT("Only a client hands askai a file as content.")
		: content && text ? TEXT("Give the text or the content of a note, not both.")
		: content && (!uri || datauri_parse(uri, uri_len, &data))
		? TEXT("The content argument must be a data: URI, which the client fills "
		       "in with a file that the user gave it.")
		: content && data.size > most
		? ESC(big, util_format(big, sizeof big, "The file has more than %zu bytes, "
		                       "which this server takes at most in one request.",
		                       most))
		: !content && (!text || !len || len > NOTES_TEXT || memchr(text, '\0', len))
		? TEXT("The text argument must be a string of 1 byte to 4 MiB, and no NUL.")
		: (struct mcp_piece){};
	if (problem.ptr) {
		mcp_slot_result(slot, problem, true);
		return;
	}
	if (!initialized && (flags & FETCH_REPLACE)) {
		mcp_slot_result(slot, TEXT("No note has that name. Use action add to create it."),
		                true);
		return;
	}
	name += skip;
	name_len -= skip;
	/* The file part of the note stays in the request's body while git
	 * checks the name or makes the repository.  A call that ended early
	 * can have left a file part there, so the body is cleared first. */
	buf_fini(&req->body);
	struct buf in = {};
	int e = content ? mcp_notes_add_data(&req->body, name, name_len, &data)
	                : notes_add_file(&req->body, name, name_len, text, len);
	if (content)
		loop_work(req->mcp->loop, LOOP_COPY, data.size);
	if (!e && initialized)
		e = notes_check_input(&in, req->repo, name, name_len);
	if (e) {
		buf_fini(&in);
		buf_fini(&req->body);
		mcp_req_fail(req, e);
		return;
	}
	mcp_slot_notes_run(slot, (initialized ? FETCH_CHECK : FETCH_CREATE) | flags, &in);
}

/* Copies bytes into a new own commit: a copy's destination waits in the
 * request body; a restore retains its source name and pinned notebook tip. */
static void
mcp_slot_notes_got_copy (struct mcp_slot *slot,
                         struct span      text)
{
	struct mcp_req *req = slot->req;
	bool initialized = notes_exists(req->mcp->st->notes);
	bool restore = slot->fetch & FETCH_RESTORE;
	char to[NOTES_NAME];
	struct span name = buf_peek(&req->body);
	if (restore) {
		struct cstr_view file = notes_revision_file(slot->own);
		name = (struct span){ (unsigned char *)(uintptr_t)file.data, file.len };
	}
	size_t len = name.len < sizeof to ? name.len : sizeof to;
	memcpy(to, name.ptr, len);
	buf_fini(&req->body);
	struct buf in = {};
	int e = notes_add_file(&req->body, to, len, (char const *)text.ptr, text.len);
	if (!e && initialized)
		e = notes_check_input(&in, req->repo, to, len);
	loop_work(req->mcp->loop, LOOP_COPY, text.len);
	child_destroy(&slot->child);
	if (e) {
		buf_fini(&in);
		buf_fini(&req->body);
		mcp_req_fail(req, e);
		return;
	}
	mcp_slot_notes_run(slot, (initialized ? FETCH_CHECK : FETCH_CREATE) |
	                         (restore ? FETCH_RESTORE : FETCH_COPY),
	                   &in);
}

/* Starts the read action for the file name in @p args, with @p fetch: the
 * fetch of read, of attach, or of copy, after which
 * mcp_slot_notes_got_copy() adds the note byte for byte to the caller's own
 * notebook, under the name that the as argument gives, or else under the
 * same name.  The text of a copied note does not pass through the caller. */
static void
mcp_slot_notes_read (struct mcp_slot    *slot,
                     struct json_object *args,
                     uint16_t            fetch)
{
	struct mcp_req *req = slot->req;
	size_t len = 0;
	size_t skip = 0;
	char const *name = jb_str(jb_get(args, "file"), &len);
	if (!name || notes_name(name, len, &skip)) {
		mcp_slot_result(slot, TEXT(bad_name), true);
		return;
	}
	int e = 0;
	if (fetch & FETCH_COPY) {
		struct json_object *as = jb_get(args, "as");
		size_t to_len = len;
		size_t to_skip = skip;
		char const *to = as ? jb_str(as, &to_len) : name;
		if (!to || notes_name(to, to_len, &to_skip)) {
			mcp_slot_result(slot, TEXT("The as argument must be a file name, as the "
			                           "file argument is."), true);
			return;
		}
		/* The name of the copy waits in the request's body while git reads
		 * the note.  A call that ended early can have left a file part
		 * there. */
		buf_fini(&req->body);
		e = mcp_piece_write(&RAW(to + to_skip, to_len - to_skip), 0, to_len - to_skip,
		                    &req->body, NOTES_NAME);
	}
	struct buf in = {};
	if (!e)
		e = notes_read_input(&in, mcp_slot_notebook(slot), name + skip, len - skip);
	if (e) {
		buf_fini(&in);
		buf_fini(&req->body);
		mcp_req_fail(req, e);
		return;
	}
	mcp_slot_notes_run(slot, fetch, &in);
}

/** Starts a revision-aware read, diff or restore, or a filtered history. */
static void
mcp_slot_notes_revision (struct mcp_slot    *slot,
                         struct json_object *args,
                         uint16_t            fetch)
{
	static char const *const keys[][2] = { { "revision" }, { "from", "to" } };
	bool diff = fetch & FETCH_DIFF;
	struct cstr_view revisions[2] = {};
	for (size_t i = 0; i < (diff ? 2 : 1); ++i) {
		struct json_object *arg = jb_get(args, keys[diff][i]);
		revisions[i].data = jb_str(arg, &revisions[i].len);
		if ((i == 0 && !arg && (fetch & (FETCH_DIFF | FETCH_RESTORE))) ||
		    (arg && !util_is_oid(revisions[i].data, revisions[i].len) &&
		     !cstr_view_equal(revisions[i], CSTR_VIEW("current")))) {
			mcp_slot_result(slot, TEXT("Use a full commit ID from action history, or current. "
			                           "diff requires from; restore requires revision."), true);
			return;
		}
	}
	struct cstr_view file = {};
	file.data = jb_str(jb_get(args, "file"), &file.len);
	int e = 0;
	slot->own = notes_revision_create(file, revisions[0], revisions[1], &e);
	if (!slot->own) {
		if (e == EINVAL)
			mcp_slot_result(slot, TEXT(bad_name), true);
		else
			mcp_req_fail(slot->req, e);
		return;
	}
	if ((fetch & FETCH_KIND) != FETCH_LIST)
		fetch |= FETCH_REVISION;
	mcp_slot_notes_run(slot, fetch | (*slot->notebook ? FETCH_OTHER : 0), nullptr);
}

/** Notes are shared by repository path; the server chooses every write branch. */
static void
mcp_slot_notes (struct mcp_slot    *slot,
                struct json_object *args)
{
	struct mcp_req *req = slot->req;
	struct notes const *notes = req->mcp->st->notes;
	bool uploads = slot->role != ROLE_OWN && req->mcp->st->uploads;
	size_t len = 0;
	char const *action = jb_str(jb_get(args, "action"), &len);
	size_t which = 0;
	while (which < ARRAY_SIZE(mcp_notes_actions) &&
	       (!action || len != mcp_notes_actions[which].len ||
	        memcmp(action, mcp_notes_actions[which].name, len)))
		++which;
	if (which == ARRAY_SIZE(mcp_notes_actions) ||
	    ((mcp_notes_actions[which].flags & NOTE_UPLOAD) && !uploads)) {
		dstr_auto why = DSTR_INIT;
		bool ok = CAT(&why, "The action must be one of:");
		for (size_t i = 0; ok && i < ARRAY_SIZE(mcp_notes_actions); ++i)
			if (uploads || !(mcp_notes_actions[i].flags & NOTE_UPLOAD))
				ok = dstr_printf(&why, " %s", mcp_notes_actions[i].name);
		if (!ok || !CAT(&why, "."))
			mcp_req_fail(req, ENOMEM);
		else
			mcp_slot_result(slot, DSTR(&why), true);
		return;
	}
	uint32_t flags = mcp_notes_actions[which].flags;
	uint16_t fetch = mcp_notes_actions[which].fetch;
	struct json_object *revision = jb_get(args, "revision");
	if ((revision && !(flags & NOTE_REVISION)) ||
	    ((jb_get(args, "from") || jb_get(args, "to")) && !(fetch & FETCH_DIFF))) {
		mcp_slot_result(slot, TEXT("revision applies to read and restore; from and to apply "
		                           "to diff. Use history to find commit IDs."), true);
		return;
	}
	if (slot->role == ROLE_OWN) {
		struct json_object *thread = jb_get(args, "thread");
		char const *id = jb_str(thread, &len);
		if (thread && (!id || !notes_is_id(id, len))) {
			mcp_slot_result(slot, TEXT("The optional thread must be 32 hexadecimal "
			                           "digits. It does not select the notes session."), true);
			return;
		}
		if (id)
			memcpy(req->thread, id, sizeof req->thread);
		notes_repo(notes, req->principal, NOTES_CLIENT, req->repo);
	}
	struct json_object *notebook = jb_get(args, "notebook");
	char const *selected = jb_str(notebook, &len);
	slot->notebook[0] = '\0';
	if (jb_is(notebook, "other") ||
	    (!notebook && (fetch & FETCH_COPY) && slot->role == ROLE_OWN)) {
		mcp_slot_other(slot, slot->notebook);
	} else if (notebook && !jb_is(notebook, "own")) {
		if (notes_select(selected, len, slot->notebook)) {
			mcp_slot_result(slot, TEXT("The notebook must be own or a selector returned "
			                           "by action notebooks (participant/principal/side)."), true);
			return;
		}
	}
	if ((fetch & FETCH_COPY) && !notebook && slot->role != ROLE_OWN) {
		mcp_slot_result(slot, TEXT("For copy, give the source notebook selector from "
		                           "action notebooks."), true);
		return;
	}
	if ((flags & NOTE_WRITE) && *slot->notebook && strcmp(slot->notebook, req->repo)) {
		mcp_slot_result(slot, TEXT("You can write notes only to your own notebook."), true);
		return;
	}
	if ((flags & NOTE_OFFSET) && mcp_slot_offset(slot, args))
		return;
	if (revision || (fetch & (FETCH_DIFF | FETCH_RESTORE)) ||
	    (fetch == FETCH_LIST && jb_get(args, "file"))) {
		mcp_slot_notes_revision(slot, args, fetch);
		return;
	}
	if (flags & NOTE_WRITE) {
		mcp_slot_notes_add(slot, args, fetch);
		return;
	}
	if (!notes_exists(notes)) {
		if (fetch == FETCH_BOOKS) {
			int e = mcp_slot_notes_identity(slot);
			if (e)
				mcp_req_fail(req, e);
			else
				mcp_slot_respond(slot, false);
		} else {
			mcp_slot_result(slot, TEXT("No notes have been added to this session yet."), false);
		}
		return;
	}
	if (flags & NOTE_FILE)
		mcp_slot_notes_read(slot, args, fetch | (*slot->notebook ? FETCH_OTHER : 0));
	else
		mcp_slot_notes_run(slot, fetch | (*slot->notebook ? FETCH_OTHER : 0), nullptr);
}

/* Do the work of @p step's slot, as its call's kind does. */
static void
mcp_slot_step (struct loop      *loop,
               struct loop_step *step)
{
	(void)loop;
	struct mcp_slot *slot = container_of(step, struct mcp_slot, step);
	mcp_slot_offer(slot)->kind->step(slot);
}

/* Compaction has left the summary, if any, in what askai adds to the
 * prompt: the notes go after it, read for @p fetch, if the thread has any. */
static void
mcp_req_compact_notes (struct mcp_req *req,
                       enum mcp_fetch  fetch)
{
	int e = mcp_req_notes_identity(req);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	if (req->branch) {
		mcp_slot_notes_run(&req->self, fetch, nullptr);
		return;
	}
	mcp_req_go_on(req, TEXT("\n\n"));
}

/* Compact the context, as the turn's compact call in @p slot asks with
 * @p args: what follows the prompt goes, and askai adds the summary and the
 * notes to it.  The turn's calls after this one do not run. */
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
	/* The trace names the call by its tool, the name it called. */
	struct mcp_offer const *o = mcp_slot_offer(slot);
	struct mcp_call const c = { .name = { (unsigned char *)(uintptr_t)o->name, o->len } };
	(void)mcp_req_trace(req, &c, args, o->trace);
	(void)mcp_req_trace_end(req, false, len);
	hist_add(&req->mcp->counts->calls.compacted[0], 1);
	LOG(LOG_VERBOSE, "ask call %" PRIu64 ".%" PRIu64 " turn %" PRIu32 ": compacted by "
	    "the upstream's compact call, summary %zu %s", req->mcp->loop->id, req->asked,
	    req->turn, len, mcp_bytes(len));
	req->reminded = 0;
	dstr_clear(&req->self.answer);
	struct mcp_piece const p[] = {
		TEXT("\n\n[askai: you compacted the context: the conversation since the "
		     "task is gone, your turns and the tools' results with it. Your "
		     "summary of it:]\n\n"),
		ESC(summary, jb_utf8(summary, len)),
	};
	int e = mcp_slot_say(&req->self, p, ARRAY_SIZE(p));
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
	mcp_req_compact_notes(req, FETCH_COMPACT);
}

/* The context is full, as the upstream said or as askai reckons: what
 * follows the prompt goes, and askai adds the notes to it, unless that was
 * done already since a turn went through. */
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
	int e = mcp_slot_say(&req->self, &TEXT("\n\n[askai: your context filled up, so the "
	                               "conversation since the task is gone, your turns "
	                               "and the tools' results with it.]"), 1);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	mcp_req_compact_notes(req, FETCH_FULL);
}

#undef CAT

/* Start an ask call in @p slot, the request's own, its turns lent the tools
 * that the client's role grants. */
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
		if (problem.ptr) {
			mcp_slot_result(slot, problem, true);
			return;
		}
		if (job) {
			req->promote_at = UINT64_MAX;
			mcp_req_arm(req);
			bool cancel = has_action && jb_is(action, "cancel");
			if (!mcp_req_recover(req, args, cancel))
				(cancel ? mcp_slot_client_cancel : mcp_slot_client_result)(slot, args, true);
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

/* Start a tool call: of the tool that the name in @p params names, compared
 * over its length, as the Mcp-Name header is, among those that a role may
 * grant, with the arguments and the progress token that @p params gives;
 * for a client that takes tasks, as @p tasks says, one that becomes a task
 * if it runs TASK_NS, unless its kind has KF_AGENDA. */
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
	size_t len = 0;
	char const *str = jb_str(jb_get(params, "name"), &len);
	uint32_t id = mcp_static_tool_of(st, str, len);
	/* One that the client's role may not call is not unknown, but not
	 * available to it. */
	if (id >= st->n_granted || !(req->s->tools >> id & 1)) {
		bool known = id < st->n_granted;
		/* An unknown name is the client's, so the log has it as a JSON
		 * string. */
		char quoted[NAME_LOG + 8] = "(none)";
		if (str && !known)
			mcp_quote(quoted, sizeof quoted, str, len, NAME_LOG);
		LOG(LOG_WARN, "tool call %s from %s refused: %s",
		    known ? st->offers[id].name : quoted, mcp_req_client(req),
		    known ? "the role does not grant it" : str ? "askai offers no such tool"
		                                               : "the call names no tool");
		struct mcp_piece head = known ? FRAG("Tool not available: ")
		                              : FRAG("Unknown tool: ");
		struct mcp_piece what = str ? ESC(str, mcp_clip(str, len, 100))
		                            : TEXT("(none)");
		char msg[32 + 6 * 100];
		memcpy(msg, head.ptr, head.len);
		size_t n = jb_escape(msg + head.len, what.ptr, what.len);
		mcp_req_error(req, 200, INVALID_PARAMS, RAW(msg, head.len + n), nullptr, 0);
		return;
	}
	struct json_object *stream = nullptr;
	bool has_stream = json_object_object_get_ex(jb_get(jb_get(params, "_meta"), "askai"),
	                                            "stream", &stream);
	if (has_stream && (!json_object_is_type(stream, json_type_boolean) ||
	                   (json_object_get_boolean(stream) &&
	                    (!req->sse_ok || st->offers[id].kind->start != mcp_slot_ask)))) {
		mcp_req_error(req, 200, INVALID_PARAMS,
		              FRAG("askai.stream must be boolean; true requires the ask tool "
		                   "and an SSE-capable client"), nullptr, 0);
		return;
	}
	if (has_stream && json_object_get_boolean(stream))
		req->asks |= JOB_STREAM;
	int e = mcp_req_enrol(req);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	struct json_object *token = jb_get(jb_get(params, "_meta"), "progressToken");
	req->token = mcp_id_valid(token) ? json_object_get(token) : nullptr;
	if (tasks && !(req->asks & JOB_STREAM) && !(st->offers[id].kind->flags & KF_AGENDA)) {
		req->promote_at = req->mcp->loop->now + TASK_NS;
		mcp_req_arm(req);
	}
	req->self.tool = (uint8_t)id;
	struct json_object *args = jb_get(params, "arguments");
	if (log_threshold >= LOG_VERBOSE)
		mcp_args_shown(&st->offers[id], args, TOOL_TEXT("none"), req->call_args);
	mcp_slot_start(&req->self, args);
}

static void
mcp_req_initialize (struct mcp_req     *req,
                    struct json_object *params)
{
	static char const head[] = JSON_FIELD "Mcp-Session-Id: ";
	size_t len = 0;
	char const *version = jb_str(jb_get(params, "protocolVersion"), &len);
	if (!mcp_is_legacy(version, len)) {
		version = versions[MODERN];
		len = sizeof versions[MODERN] - 1;
	}

	/* Never stored: the ID only scopes the request IDs a client cancels. */
	char fields[sizeof head - 1 + 32 + 2];
	struct span f = SPAN(JSON_FIELD);
	if (!util_random_hex(fields + sizeof head - 1, 16)) {
		memcpy(fields, head, sizeof head - 1);
		memcpy(fields + sizeof fields - 2, "\r\n", 2);
		f = (struct span){ (unsigned char *)fields, sizeof fields };
	}

	struct mcp_piece const p[] = {
		FRAG("{\"protocolVersion\":\""),
		RAW(version, len),
		DSTR(&req->mcp->st->init_tail),
		FRAG("}"),
	};
	mcp_req_result(req, p, ARRAY_SIZE(p), f);
}

/* Handles the notifications/cancelled in @p req.  The notification cancels
 * the calls of the same caller that have the request id in @p params, and
 * these calls can run on any loop.  roster_cancel() posts the message of
 * each such record to its call's loop, and mcp_on_withdraw() ends the call
 * there.  An id that no request can have names no call.  If memory runs
 * out, nothing is cancelled. */
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

/* List the tools that the client's role may call: the definitions, each
 * after a comma, of those it is granted, the first's comma left out. */
static void
mcp_req_tools_list (struct mcp_req *req)
{
	struct mcp_static const *st = req->mcp->st;
	/* One piece for each tool that a role can be granted, and three more. */
	struct mcp_piece p[TOOL_MAX + 3] = { FRAG("{\"tools\":[") };
	size_t n = 1;
	for (uint32_t id = 0; id < st->n_granted; ++id) {
		struct dstr const *def = &st->offers[id].listed;
		bool first = n == 1;
		if (req->s->tools >> id & 1)
			p[n++] = RAW(dstr_get(def) + first, dstr_len(def) - first);
	}
	p[n++] = req->modern ? DSTR(&st->list_tail) : FRAG("]}");
	p[n++] = FRAG("}");
	mcp_req_result(req, p, n, SPAN(JSON_FIELD));
}

/* Copy the result of the task that tasks/get shows into the reply, a slice
 * a step; then close it.  The record has the reply of the task's call:
 * '"result":{...}' or '"error":{...}', and a '}' that closes the object
 * around it.  A result in the record has no modern tail.  For a modern
 * reader of a result, the copy leaves out the record's last two bytes, and
 * the modern tail and a '}' follow the copy.  The reply of tasks/get ends
 * with one more '}'. */
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

/* Answer tasks/get with where @p task, a reference, stands: once its call
 * has ended, with its result or error, which mcp_req_show() copies. */
static void
mcp_req_task_show (struct mcp_req     *req,
                   struct ledger_task *task)
{
	static_assert(POLL_MS == 1000, "the reply says");
#define S(s) { s, sizeof s - 1, false }
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
	bool ended = status != LEDGER_WORKING;
	bool whole = ended && task->result;
	/* A tasks/get of a record whose call has ended marks the record read. */
	if (ended)
		atomic_store_explicit(&task->told, 1, memory_order_relaxed);
	char created[ISO_TIME];
	char updated[ISO_TIME];
	char ttl[24];
	/* Its end, and the time it stays after, once there is one to read. */
	uint64_t ran = ended && task->ended > task->created ? task->ended - task->created
	                                                    : 0;
	size_t n_ttl = ended ? util_format(ttl, sizeof ttl, "%" PRIu64,
	                                   ran / 1'000'000 + LEDGER_KEEP * 1000)
	                     : util_format(ttl, sizeof ttl, "null");
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
		RAW(updated, mcp_iso_time(updated, ended ? task->ended : task->created)),
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
 * takes tasks, as @p tasks says its request declares.  askai asks for no
 * input, so tasks/update has none to take. */
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
 * extension. */
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

/* Validate the per-request metadata of a modern request, @p method of
 * @p method_len bytes, then answer it. */
static void
mcp_req_modern (struct mcp_req     *req,
                struct json_object *params,
                char const         *method,
                size_t              method_len)
{
	struct stream *s = req->s;
	struct json_object *m = jb_get(params, "_meta");
	struct json_object *caps = jb_get(m, meta_caps);
	size_t len = 0;
	char const *version = jb_str(jb_get(m, meta_version), &len);
	if (!version || !json_object_is_type(caps, json_type_object)) {
		mcp_req_error(req, 400, INVALID_PARAMS,
		              FRAG("Request _meta must carry protocolVersion and "
		                   "clientCapabilities"), nullptr, 0);
		return;
	}
	if (!mcp_span_is(s->ops->field(s, F_VERSION), version, len)) {
		mcp_req_error(req, 400, HEADER_MISMATCH,
		              FRAG("MCP-Protocol-Version header does not match the "
		                   "request _meta"), nullptr, 0);
		return;
	}
	if (mcp_version_index(version, len) >= MODERN) {
		struct mcp_piece const data[] = {
			FRAG("{\"supported\":"),
			DSTR(&req->mcp->st->versions),
			FRAG(",\"requested\":\""),
			ESC(version, mcp_clip(version, len, MAX_ECHO)),
			FRAG("\"}"),
		};
		mcp_req_error(req, 400, UNSUPPORTED_VERSION,
		              FRAG("Unsupported protocol version"), data, ARRAY_SIZE(data));
		return;
	}
	if (!mcp_span_is(s->ops->field(s, F_METHOD), method, method_len)) {
		mcp_req_error(req, 400, HEADER_MISMATCH,
		              FRAG("Mcp-Method header does not match the request method"),
		              nullptr, 0);
		return;
	}
	/* Mcp-Name mirrors a tool call's tool, and a task method's task. */
	bool call = req->method == METHOD_TOOLS_CALL;
	bool task = req->method >= METHOD_TASKS_GET && req->method <= METHOD_TASKS_CANCEL;
	size_t name_len = 0;
	char const *name = jb_str(jb_get(params, call ? "name" : "taskId"), &name_len);
	if ((call || task) && name && !mcp_span_is(s->ops->field(s, F_NAME), name, name_len)) {
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

/* A request, of @p method, @p len bytes, and @p id: answer it in its era. */
static void
mcp_req_request (struct mcp_req     *req,
                 char const         *method,
                 size_t              len,
                 struct json_object *id)
{
	struct stream *s = req->s;
	struct json_object *params = jb_get(req->msg, "params");
	req->method = mcp_method_index(method, len);
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
	size_t version_len = 0;
	char const *version = jb_str(jb_get(jb_get(params, "_meta"), meta_version),
	                             &version_len);
	struct span header = s->ops->field(s, F_VERSION);
	req->modern = (version && !mcp_is_legacy(version, version_len)) ||
	              (header.ptr && !mcp_is_legacy((char const *)header.ptr, header.len));
	if (req->modern) {
		mcp_req_modern(req, params, method, len);
		return;
	}
	/* Without the header a legacy client speaks 2025-03-26, which has
	 * event streams; 2024-11-05 has none. */
	req->sse_ok = mcp_accepts_sse(s->ops->field(s, F_ACCEPT)) &&
	              !mcp_span_is(header, versions[PRE_STREAMS], sizeof versions[0] - 1);
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

/* Answer the message, or start the tool call it asks for; a tool call frees
 * what it does not keep of the message as it starts to wait. */
static void
mcp_req_message (struct mcp_req *req)
{
	static char const cancelled[] = "notifications/cancelled";
	struct json_object *msg = req->msg;
	size_t len = 0;
	char const *method = jb_str(jb_get(msg, "method"), &len);
	struct json_object *id;
	bool has_id = json_object_object_get_ex(msg, "id", &id);
	if (!jb_is(jb_get(msg, "jsonrpc"), "2.0")) {
		mcp_req_error(req, 400, INVALID_REQUEST, FRAG("Invalid Request"), nullptr, 0);
		return;
	}
	if (!method) {
		/* A response, as we send no requests, has an id, and a result or
		 * an error. */
		if (has_id && (jb_get(msg, "result") || jb_get(msg, "error")))
			mcp_req_accept(req);
		else
			mcp_req_error(req, 400, INVALID_REQUEST, FRAG("Invalid Request"),
			              nullptr, 0);
		return;
	}
	if (!has_id) {
		if (len == sizeof cancelled - 1 && !memcmp(method, cancelled, len))
			mcp_req_cancel(req, jb_get(msg, "params"));
		mcp_req_accept(req);
		return;
	}
	mcp_req_request(req, method, len, id);
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
 * @p pool; true once the parse is over, with its value in req->msg. */
static bool
mcp_req_slice (struct mcp_req *req,
               struct jb_pool *pool,
               struct span     json)
{
	struct loop *loop = req->mcp->loop;
	struct jb_parser *parser = &req->parser;
	uint64_t bytes = loop_room(loop, LOOP_PARSE);
	uint64_t values = loop_room(loop, LOOP_VALUES);
	if (bytes < 4 || !values)
		return false;
	size_t off = parser->off;
	uint32_t seen = parser->cut.values;
	size_t copied = parser->cut.copied;
	bool over = jb_parser_step(parser, pool, (char const *)json.ptr, json.len, bytes,
	                           (uint32_t)values);
	loop_work(loop, LOOP_PARSE, parser->off - off);
	loop_work(loop, LOOP_VALUES, parser->cut.values - seen);
	loop_work(loop, LOOP_COPY, parser->cut.copied - copied);
	if (over) {
		req->values = parser->cut.values;
		req->msg = jb_parser_take(parser);
		jb_parser_fini(parser, pool, &req->reaper);
	}
	return over;
}

/* Parse as much of the body as the step has the share for, the rest a slice
 * a bulk step; the message is handed on at once if one step parsed it all,
 * else in a step of its own. */
static void
mcp_req_parse (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	bool whole = !req->parser.off;
	if (!mcp_req_slice(req, &req->mcp->pool, buf_peek(&req->body))) {
		loop_ready_bulk(loop, &req->step);
		return;
	}
	buf_fini(&req->body);
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
 * response text and wire JSON are not transcript records. */
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
 * tool's output from it in a step of its own, as that copies the output. */
static void
mcp_req_answer (struct mcp_req *req)
{
	struct span json = buf_peek(&req->job->result);
	char const *text = json.len ? (char const *)json.ptr : "";
	size_t off = req->parser.off;
	bool over = mcp_req_slice(req, &req->mcp->answers, (struct span){
		(unsigned char *)(uintptr_t)text, json.len });
	size_t end = !over ? req->parser.off : req->msg ? json.len : off;
	if (jb_utf8(text + off, end - off) < end - off) {
		jb_parser_fini(&req->parser, &req->mcp->answers, &req->reaper);
		mcp_req_dispose(req);
		over = true;
	}
	if (over) {
		req->phase = PHASE_TEXT;
		int e = mcp_req_reasoning(req);
		if (e) {
			mcp_req_fail(req, e);
			return;
		}
		if (req->phase == PHASE_JOURNAL)
			return;
	}
	loop_ready_bulk(req->mcp->loop, &req->step);
}

/* Read the tool's output from the answer, then let go of that, and of the
 * job; a streamed answer's text, home read already. */
static void
mcp_req_text (struct mcp_req *req)
{
	struct job *job = req->job;
	struct span body = buf_peek(&job->result);
	/* Stopped with no text, its turns gave no answer. */
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
	if (failed)
		mcp_req_unfile(req);
	if (failed && mcp_req_refit(req, dstr_get(&req->self.answer), dstr_len(&req->self.answer))) {
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
	int e = metrics_render(&req->mcp->metrics, &req->off, &req->reply);
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
 * free, then the phase's work. */
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
 * the stream answered, if out of memory. */
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
		.s = s,
		.mcp = mcp,
		.head = loop_clock(),
		.principal = s->principal,
		.generation = instance_table_generation(&mcp->instances),
		.ping_at = UINT64_MAX,
		.promote_at = UINT64_MAX,
		.phase = phase,
		.method = method
	};
	/* The client's name is in the policy, which a reload can free after
	 * the head. */
	if (s->who.len)
		memcpy(req->client, s->who.ptr, s->who.len < sizeof req->client
		                                ? s->who.len : sizeof req->client - 1);
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
	if (mcp_span_is(method, "GET", sizeof "GET" - 1) && mcp_is_metrics(target)) {
		if (s->trusted)
			(void)mcp_begin(mcp, s, PHASE_METRICS, METHOD_METRICS);
		else
			mcp_http_error(s, 403, "Forbidden", SPAN(JSON_FIELD));
		return;
	}
	if (!mcp_span_is(method, "POST", sizeof "POST" - 1)) {
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
	/* A reply that has gone whole ends as it did (mcp_req_abort()). */
	if (req->phase != PHASE_END)
		mcp_req_log_call(req, "cancelled by a closed connection", (struct span){}, false);
	mcp_req_abort(req, ECANCELED);
}

/* The on_policy() function of the stream application.  The loop of @p ctx,
 * a struct mcp, has replaced its policy after a reload.  The instances that
 * exist were made for users as the old policy identified them.  After
 * instance_table_flush(), no call of a later request gets one of them. */
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
 * its share of requests allows. */
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
		.metrics = *metrics,
		.loop = loop,
		.home = home,
		.st = st,
		.counts = counts
	};
	list_init(&mcp->calls);
	list_init(&mcp->tasks);
	list_init(&mcp->freeing);
	list_init(&mcp->ending);
	list_init(&mcp->children);
	list_init(&mcp->spare.list);
	list_init(&mcp->jobs.list);
	*e = instance_table_init(&mcp->instances, loop);
	if (*e) {
		free(mcp);
		return nullptr;
	}
	for (uint32_t i = 0; i < st->n_plugins; ++i) {
		struct mcp_plugin const *p = &st->plugins[i];
		/* For a plugin without part_create(), the part is the plugin's
		 * service.  A plugin that askai loaded from a file has neither, so
		 * its part is nullptr. */
		mcp->parts[i] = p->service;
		if (!p->part_create)
			continue;
		int pe = 0;
		mcp->parts[i] = p->part_create(p->service, loop, &pe);
		if (mcp->parts[i])
			continue;
		*e = pe ? pe : ENOMEM;
		mcp_destroy_parts(mcp, i);
		instance_table_fini(&mcp->instances);
		free(mcp);
		return nullptr;
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
	 * failed.  The calls whose end record is still being written end here
	 * too, as no journal callback reaches them now.  Then only requests
	 * still freeing their message are left, and what the parts do, such as
	 * the man fetches.  The parts go once what they keep is given back. */
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
	child_destroy_all(&mcp->children);
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

/* A JSON string of the texts @p a, @p b and @p c, one after another, or
 * nullptr if askai runs out of memory. */
static struct json_object *
mcp_text3 (char const *a,
           char const *b,
           char const *c)
{
	dstr_auto d = DSTR_INIT;
	if (!dstr_cat(&d, a) || !dstr_cat(&d, b) || !dstr_cat(&d, c))
		return nullptr;
	return json_object_new_string_len(dstr_get(&d), (int)dstr_len(&d));
}

/* A property of @p type, as JSON Schema has one, that @p about describes. */
static struct json_object *
mcp_property (char const *type,
              char const *about)
{
	return jb_object("type", json_object_new_string(type),
	                 "description", json_object_new_string(about), nullptr);
}

/* The schema of an object of the @p properties, those @p required. */
static struct json_object *
mcp_schema (struct json_object *properties,
            struct json_object *required)
{
	return jb_object("type", json_object_new_string("object"),
	                 "properties", properties,
	                 "required", required,
	                 "additionalProperties", json_object_new_boolean(0),
	                 nullptr);
}

/* A JSON array of the strings @p words, which end in nullptr; nullptr if
 * out of memory. */
static struct json_object *
mcp_words (char const *const words[])
{
	struct json_object *arr = json_object_new_array();
	for (size_t i = 0; arr && words[i]; ++i)
		if (jb_push(arr, json_object_new_string(words[i]))) {
			json_object_put(arr);
			arr = nullptr;
		}
	return arr;
}

/* Returns the definition of the ask tool.  @p service is the mcp_static,
 * which has the tool's configured name and description.  The definition has
 * the argument "thread" if the turns get the notes tool, which is the only
 * tool in st->lending.by[LEND_ALWAYS].  They get it with --notes and
 * --upstream-tools. */
static struct json_object *
mcp_tool_definition (void const *service)
{
	struct mcp_static const *st = service;
	struct json_object *props = jb_object(
		"prompt", mcp_property("string",
			"The new question or task. Include the context it needs, or supply "
			"earlier user and assistant turns in messages for a continuing chat."),
		"system", mcp_property("string",
			"Optional system instructions for the other AI, such as a role "
			"or an answer format."),
		"messages", jb_object(
			"type", json_object_new_string("array"),
			"maxItems", json_object_new_int(CHAT_HISTORY),
			"description", json_object_new_string(
				"Earlier visible chat messages, in order, before the new prompt. "
				"Each has role user or assistant and plain string content. The "
				"client retains this history; thread does not retain messages."),
			"items", jb_object(
				"type", json_object_new_string("object"),
				"properties", jb_object(
					"role", jb_object("type", json_object_new_string("string"),
					                  "enum", mcp_words((char const *[]){
					                          "user", "assistant", nullptr }),
					                  nullptr),
					"content", jb_object("type", json_object_new_string("string"), nullptr),
					nullptr),
				"required", mcp_words((char const *[]){ "role", "content", nullptr }),
				"additionalProperties", json_object_new_boolean(0),
				nullptr),
			nullptr),
		"background", mcp_property("boolean",
			"False waits for the answer on this connection. True requires server "
			"background mode and returns a job id. Omission follows the server "
			"setting, unless live streaming is requested."),
		nullptr);
	if (st->lending.by[LEND_ALWAYS] && jb_set(props, "thread", mcp_property("string",
		    "Optional ask-chain id from an earlier answer: 32 hexadecimal "
		    "digits. Notes are shared by repository path and loaded at the "
		    "start of every ask, independently of this argument."))) {
		json_object_put(props);
		props = nullptr;
	}
	if (st->ep->background) {
		int e = jb_set(props, "job", mcp_property("string",
			"The job id returned by this tool. Give job instead of prompt, "
			"system or thread to check its status or read its completed result."));
		e |= jb_set(props, "action", jb_object(
			"type", json_object_new_string("string"),
			"enum", mcp_words((char const *[]){ "result", "cancel", nullptr }),
			"default", json_object_new_string("result"),
			"description", json_object_new_string(
				"With job: result checks its status or returns its completed "
				"answer; cancel stops the job. Omit action to use result."),
			nullptr));
		if (e) {
			json_object_put(props);
			props = nullptr;
		}
	}
	return jb_object(
		"name", json_object_new_string(st->tool->name),
		"title", json_object_new_string("Ask AI"),
		"description", mcp_text3(st->tool->description, st->ep->background
			? "\n\nWith prompt, start a job and return its id immediately. The job "
			  "keeps running after this connection closes. Later call this same "
			  "tool with job and no prompt to check its status or read its result; "
			  "action cancel stops it. Results are kept for up to an hour after "
			  "completion. Set background false to wait for the answer on this "
			  "connection."
			: "", ""),
		"inputSchema", mcp_schema(props, st->ep->background
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
				"slices", jb_object(
					"type", json_object_new_string("integer"),
					"minimum", json_object_new_int(1),
					"maximum", json_object_new_int(SPIN_MAX),
					nullptr),
				"prompt", jb_object(
					"type", json_object_new_string("string"),
					nullptr),
				nullptr),
			"additionalProperties", json_object_new_boolean(0),
			nullptr),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(1),
			"openWorldHint", json_object_new_boolean(0),
			nullptr),
		nullptr);
}

/* Returns the definition of the notes tool for the side @p side.  The
 * definition for the upstream has no thread argument, because a call of a
 * turn uses the thread of its ask call, and it has the action attach if
 * the upstream takes files, which @p st says (mcp_static_uploads()). */
static struct json_object *
mcp_notes_schema (enum notes_side          side,
                  struct mcp_static const *st)
{
	static_assert(NOTES_TEXT == 4 << 20 && NOTES_NAME == 255, "the descriptions say");
	static char const *const descriptions[] = {
		[NOTES_UPSTREAM] =
			"Your persistent notebook in this shared Git session. Add findings, "
			"decisions and plans as named files. replace can update your own "
			"existing files; Git preserves every previous version. All instances sharing the repository "
			"can read every notebook, but you can write only your own. Use action "
			"notebooks to discover the others, then list or read with that notebook "
			"selector. Your notes are included after compaction; read older notes "
			"when needed. Non-UTF-8 bytes are displayed as \xe2\x9f\xa6XX\xe2\x9f\xa7.",
		[NOTES_CLIENT] =
			"Your persistent notebook in this shared Git session. Notes are named "
			"files; add creates one, replace updates one while preserving history. All instances sharing the "
			"repository can read every notebook; only your own can be written. "
			"Use action notebooks to discover selectors, then list or read one. "
			"copy imports a note into your own notebook without changing its text. "
			"content keeps an attached user file. Threads do not partition notes. "
			"Non-UTF-8 bytes are displayed as \xe2\x9f\xa6XX\xe2\x9f\xa7.",
	};
	/* A turn attaches a note if its upstream takes files. */
	bool files = side == NOTES_UPSTREAM && st && st->uploads;
	size_t most = st ? st->files_inline : FILES_INLINE;
	struct json_object *actions = json_object_new_array();
	for (size_t i = 0; actions && i < ARRAY_SIZE(mcp_notes_actions); ++i) {
		if (!files && (mcp_notes_actions[i].flags & NOTE_UPLOAD))
			continue;
		if (jb_push(actions, json_object_new_string(mcp_notes_actions[i].name))) {
			json_object_put(actions);
			actions = nullptr;
		}
	}
	struct json_object *props = jb_object(
		"action", jb_object(
			"type", json_object_new_string("string"),
			"enum", actions,
			"description", mcp_text3(
				"add a note, replace an existing own note, list current files "
				"with their byte sizes and blob ids, show history oldest first "
				"with commit IDs (line counts are lines added by each change), read a note, "
				"diff two versions, or restore an old version in a new own commit. "
				"To recover lost text: history with file, diff with file and from, then "
				"read with revision for selected text or restore with revision for the whole file. "
				"Use copy to import a note into yours, or discover all session notebooks with "
				"notebooks.", files ? " attach gives a note as a file." : "", ""),
			nullptr),
		"file", jb_object(
			"type", json_object_new_string("string"),
			"description", mcp_text3(
				"For add, replace, restore, read, diff", files ? ", copy and attach" : " and copy",
				": the file name of the note, at most 255 bytes. It can have "
				"directories, such as findings/locks.md. Its extension, such "
				"as .md or .txt, shows the format of the text. Optional for history "
				"to show only changes to that exact file name."),
			nullptr),
		"revision", mcp_property("string",
			"For read: full commit ID from the selected notebook's history, "
			"or current (default). Required for restore, which copies the exact "
			"historical bytes into your own notebook in a new commit, preserving "
			"all intervening history. No Git revision expressions."),
		"from", mcp_property("string",
			"Required for diff: the older full commit ID from this notebook's history, "
			"or current. The diff compares that file at from against to."),
		"to", mcp_property("string",
			"For diff: full commit ID from the same notebook, or current (default). "
			"Both revisions are resolved against one notebook snapshot. Output names "
			"the resolved IDs; reuse them when continuing with offset."),
		"as", mcp_property("string",
			"For copy: the file name of the copy in your notebook. The "
			"default is the name of the note."),
		"text", mcp_property("string",
			"For add and replace: the text of the note, at most 4 MiB."),
		"offset", jb_object(
			"type", json_object_new_string("integer"),
			"minimum", json_object_new_int(0),
			"description", json_object_new_string(
				"For list, history, notebooks, read and diff: the byte of the output to start from, to "
				"read on past where it was cut."),
			nullptr),
		"notebook", jb_object(
			"type", json_object_new_string("string"),
			"description", mcp_text3(
				"For list, history, read, diff and copy", files ? ", and attach" : "",
				": own (default), or a participant/principal/side selector "
				"from action notebooks. That action identifies your writable "
				"notebook even before your first note. For copy, explicitly "
				"select the source. add, replace and restore write only your own notebook."),
			nullptr),
		nullptr);
	/* The client's program, not its model, fills content in with a file
	 * (the extension io.modelcontextprotocol/files). */
	if (side == NOTES_CLIENT && jb_set(props, "content", jb_object(
		    "type", json_object_new_string("string"),
		    "format", json_object_new_string("uri"),
		    "description", json_object_new_string(
			    "For add and replace, instead of text: a file that the user gave, which "
			    "your client attaches as it is. Do not write it yourself."),
		    "x-mcp-file", jb_object("maxSize", json_object_new_int64((int64_t)most),
		                            nullptr),
		    nullptr))) {
		json_object_put(props);
		props = nullptr;
	}
	if (side == NOTES_CLIENT && jb_set(props, "thread", mcp_property("string",
		    "Optional legacy ask-chain id (32 hexadecimal digits). Notes belong "
		    "to the configured shared repository, independent of this value."))) {
		json_object_put(props);
		props = nullptr;
	}
	dstr_auto description = DSTR_INIT;
	if (!dstr_printf(&description, "Participant: %s. %s "
	                 "Prefer small, controlled, incremental edits. Before using replace, "
	                 "read the complete current file; send the complete updated file "
	                 "while preserving unrelated passages, findings and evidence verbatim. "
	                 "If an edit would overhaul most of a document, prefer add with a "
	                 "new versioned filename, keep the previous file, and identify the "
	                 "predecessor and what changed in the new document. Preserve existing "
	                 "structure and useful detail across session resumes and model changes.%s",
	                 st && st->notes ? notes_writer(st->notes) : "unconfigured",
	                 descriptions[side], files
	                 ? " attach gives you a whole note as a file after the results "
	                   "of the turn, where read shows a large note in parts." : "")) {
		json_object_put(props);
		return nullptr;
	}
	return jb_object(
		"name", json_object_new_string("notes"),
		"description", json_object_new_string_len(dstr_get(&description),
		                                          (int)dstr_len(&description)),
		"inputSchema", mcp_schema(props, jb_list(json_object_new_string("action"))),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(0),
			"destructiveHint", json_object_new_boolean(0),
			"idempotentHint", json_object_new_boolean(0),
			"openWorldHint", json_object_new_boolean(0),
			nullptr),
		nullptr);
}

/* Returns the definition of the notes tool for clients. */
static struct json_object *
mcp_notes_definition (void const *service)
{
	struct mcp_static const *st = service;
	struct json_object *def = mcp_notes_schema(NOTES_CLIENT, st);
	if (!st || !st->notes)
		return def;
	/* The canonical common directory names the session, including attached
	 * worktrees. Cache its fingerprint with the tool instead of exposing the
	 * local path; it remains stable across instance and client restarts. */
	char const *directory = notes_directory(st->notes);
	unsigned char digest[SHA256_DIGEST_LENGTH];
	if (!SHA256((unsigned char const *)directory, strlen(directory), digest)) {
		json_object_put(def);
		return nullptr;
	}
	char session[2 * sizeof digest + 1];
	util_hex(session, digest, sizeof digest);
	session[sizeof session - 1] = '\0';
	/* Clients discover stored participants through notebooks. A new writer
	 * has no branch yet, so publish its identity with the authorized tool. */
	if (jb_set(def, "_meta", jb_object(
		    "askai", jb_object(
			    "session", json_object_new_string_len(session, sizeof session - 1),
			    "participant", json_object_new_string(notes_writer(st->notes)),
			    "notebooks", json_object_new_boolean(1),
			    nullptr),
		    nullptr))) {
		json_object_put(def);
		def = nullptr;
	}
	return def;
}

/* Returns the definition of the notes tool for the upstream. */
static struct json_object *
mcp_notes_upstream_definition (void const *service)
{
	return mcp_notes_schema(NOTES_UPSTREAM, service);
}

/* The compact tool's definition, which only turns are given. */
static struct json_object *
mcp_compact_definition (void const *service)
{
	(void)service;
	static_assert(SUMMARY_MAX == 4 << 20, "the description says");
	return jb_object(
		"name", json_object_new_string("compact"),
		"description", json_object_new_string(
			"Free your context: the conversation since the task, your turns and "
			"the tools' results, is replaced by your summary of it, and your "
			"notes are shown again. Note first what you must keep. Call it when "
			"askai says that the context fills up, or once you are done with a "
			"part of the work whose details you no longer need. Calls after it "
			"in the same turn do not run."),
		"inputSchema", mcp_schema(
			jb_object(
				"summary", mcp_property("string",
					"Where the task stands: what is done, what you found, "
					"and what you will do next; at most 4 MiB."),
				nullptr),
			jb_list(json_object_new_string("summary"))),
		nullptr);
}

/* The definition of background as the turns get it; nullptr if out of
 * memory. */
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
		"inputSchema", mcp_schema(
			jb_object(
				"tool", mcp_property("string", "The name of the tool to call, "
				                     "such as git or man."),
				"arguments", mcp_property("object",
					"Its arguments, as you would give them to it."),
				nullptr),
			jb_list(json_object_new_string("tool"))),
		nullptr);
}

/* The values of the argument action of jobs, an array; nullptr if out of
 * memory. */
static struct json_object *
mcp_jobs_actions (void)
{
	return mcp_words((char const *[]){ "list", "result", "cancel", nullptr });
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
		"inputSchema", mcp_schema(
			jb_object(
				"action", jb_object(
					"type", json_object_new_string("string"),
					"enum", mcp_jobs_actions(),
					"description", json_object_new_string(
						"list the jobs, read a job's result, or cancel it."),
					nullptr),
				"job", mcp_property("integer", "For result and cancel: the "
				                    "job's number."),
				"offset", mcp_property("integer", "For result: the byte of the "
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
		"inputSchema", mcp_schema(
			jb_object(
				"seconds", mcp_property("integer", "The longest to wait: 1 to "
				                        "600."),
				"jobs", jb_object(
					"type", json_object_new_string("array"),
					"items", jb_object("type", json_object_new_string("integer"),
					                   nullptr),
					"description", json_object_new_string(
						"The jobs to wait for; any if none."),
					nullptr),
				nullptr),
			jb_list(json_object_new_string("seconds"))),
		nullptr);
}

/* The definition of timer as the turns get it, likewise. */
static struct json_object *
mcp_timer_definition (void const *service)
{
	(void)service;
	static_assert(TIMER_MAX == 3600 && TIMER_NOTE == 200, "the description says");
	return jb_object(
		"name", json_object_new_string("timer"),
		"description", json_object_new_string(
			"Set a timer, and go on at once: when it fires, askai tells you, "
			"with a later tool result, by ending a wait, or in a message after "
			"your answer."),
		"inputSchema", mcp_schema(
			jb_object(
				"seconds", mcp_property("integer", "When it fires: in 1 to 3600 "
				                        "seconds."),
				"note", mcp_property("string", "What it is for, which askai "
				                     "says as it fires; at most 200 bytes."),
				nullptr),
			jb_list(json_object_new_string("seconds"))),
		nullptr);
}

/* Returns the definition of background as clients list it, with
 * --client-jobs.  @p service is the mcp_static, which has the name of the
 * ask tool.  Returns nullptr if out of memory. */
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
		"inputSchema", mcp_schema(
			jb_object(
				"tool", mcp_property("string", tool),
				"arguments", mcp_property("object", "The arguments for the tool, "
				                          "the same as in a direct call."),
				nullptr),
			jb_list(json_object_new_string("tool"))),
		nullptr);
}

/* Returns the definition of jobs as clients list it, with --client-jobs;
 * nullptr if out of memory. */
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
		"inputSchema", mcp_schema(
			jb_object(
				"action", jb_object(
					"type", json_object_new_string("string"),
					"enum", mcp_jobs_actions(),
					"description", json_object_new_string(
						"list lists your jobs and timers, result returns the "
						"result of a job, and cancel cancels a job or a timer."),
					nullptr),
				"job", mcp_property("string", "The id of the job for result and "
				                    "cancel, from the result of background."),
				"timer", mcp_property("integer", "The number of the timer for "
				                      "cancel, from the result of timer."),
				nullptr),
			jb_list(json_object_new_string("action"))),
		nullptr);
}

/* Returns the definition of wait as clients list it, with --client-jobs;
 * nullptr if out of memory. */
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
		"inputSchema", mcp_schema(
			jb_object(
				"seconds", mcp_property("integer", "The longest time to wait, "
				                        "from 1 to 600 seconds."),
				"jobs", jb_object(
					"type", json_object_new_string("array"),
					"items", jb_object("type", json_object_new_string("string"),
					                   nullptr),
					"description", json_object_new_string(
						"The ids of the jobs to wait for, at most 16; without "
						"ids, any job."),
					nullptr),
				nullptr),
			jb_list(json_object_new_string("seconds"))),
		nullptr);
}

/* Returns the definition of timer as clients list it, with --client-jobs;
 * nullptr if out of memory. */
static struct json_object *
mcp_timer_client_definition (void const *service)
{
	(void)service;
	static_assert(TIMER_MAX == 3600 && TIMER_NOTE == 200, "the description says");
	return jb_object(
		"name", json_object_new_string("timer"),
		"description", json_object_new_string(
			"Set a timer, and continue at once: the result contains the number of "
			"the timer. When the timer fires, a call of wait ends, and its result "
			"contains the number and the note of the timer."),
		"inputSchema", mcp_schema(
			jb_object(
				"seconds", mcp_property("integer", "The seconds until the timer "
				                        "fires, from 1 to 3600."),
				"note", mcp_property("string", "What the timer is for, at most 200 "
				                     "bytes; the result of a wait contains it when "
				                     "the timer fires."),
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
 * it. */
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
	if (jb_set(result, "ttlMs", json_object_new_int(CACHE_TTL_MS)) ||
	    jb_set(result, "cacheScope", json_object_new_string("public")) ||
	    jb_set(result, "resultType", json_object_new_string("complete")) ||
	    jb_set(result, "_meta", jb_object(meta_server, json_object_get(info),
	                                      nullptr))) {
		json_object_put(result);
		return nullptr;
	}
	return result;
}

/* Serialize the supported versions, pushed onto @p list, and with what
 * tells of askai, @p info, the server/discover result and the tails of
 * initialize's and modern results. */
static int
mcp_static_tails (struct mcp_static  *st,
                  struct json_object *info,
                  struct json_object *list)
{
	for (size_t i = 0; i < ARRAY_SIZE(versions); ++i) {
		int e = jb_push(list, json_object_new_string(versions[i]));
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
	/* tools/list lists the tools that the caller's role grants, so no cache
	 * may give one caller's list to another caller. */
	return dstr_printf(&st->list_tail, "],\"ttlMs\":%d,\"cacheScope\":\"private\"%s",
	                   CACHE_TTL_MS, dstr_get(&st->modern_tail)) ? 0 : ENOMEM;
}

/* What each kind of tool is: what its calls do, or the plugin whose one
 * tool it is; its definition as the turns get it, if that is not the
 * definition as clients list it; its name, but the ask tool's, which the
 * configuration names; the argument whose value is logged as its size; the
 * option that turns it on, for the log, or nullptr for compact and the test
 * aid, which the log leaves out while they are off; how the turns are lent it, but a
 * plugin's, which follows its kind's flags; its row in the turns' metrics;
 * and what the trace shows of its calls. */
struct mcp_row {
	struct tool_kind        kind;
	struct plugin const    *plugin;
	tool_definition_fn     *upstream;
	char const             *ptr;
	char const             *sized;
	char const             *option;
	uint32_t                len;
	uint8_t                 lend;    /* enum mcp_lend */
	uint8_t                 metric;  /* enum metrics_lent */
	uint8_t                 trace;   /* enum mcp_trace */
};

/* mcp_host_plugin is the plugin of askai's own kinds.  It has no instances
 * and no part_create().  Its service is the mcp_static, and mcp_create()
 * uses the service as the plugin's part on every loop.  It is the plugin
 * at index 0 of mcp_static.plugins. */
static struct plugin const mcp_host_plugin = {};

static struct mcp_row const mcp_rows[KINDS] = {
#define N(s) .ptr = s, .len = sizeof s - 1
	[KIND_ASK]        = { { .definition = mcp_tool_definition, .start = mcp_slot_ask,
	                        .step = mcp_slot_notes_out },
	                      .metric = LENT_NONE },
	[KIND_MAN]        = { .plugin = &manual_plugin, N("man"), .metric = LENT_MAN,
	                      .option = "--man" },
	[KIND_TASK]       = { { .definition = mcp_task_definition, .start = mcp_slot_task,
	                        .drop = mcp_slot_task_drop },
	                      N("test_task"), LEND_NEVER, LENT_NONE },
	[KIND_GIT]        = { .plugin = &git_plugin, N("git"), .metric = LENT_GIT,
	                      .option = "--git PATH" },
	/* The flag TOOL_LENT lets a role's "upstream" name the notes tool, which
	 * the turns always get. */
	[KIND_NOTES]      = { { .definition = mcp_notes_definition, .start = mcp_slot_notes,
	                        .step = mcp_slot_notes_out, .drop = mcp_slot_notes_drop,
	                        .flags = KF_SERIAL | TOOL_LENT | TOOL_PAGED },
	                      .upstream = mcp_notes_upstream_definition,
	                      N("notes"), LEND_ALWAYS, LENT_NOTES, TRACE_ACTION,
	                      .sized = "text", .option = "--notes DIR" },
	[KIND_COMPACT]    = { { .definition = mcp_compact_definition, .start = mcp_slot_compact,
	                        .flags = KF_SERIAL },
	                      N("compact"), LEND_WITH, LENT_COMPACT, TRACE_NAME,
	                      .sized = "summary" },
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

/* Serialize the replies that never change, of @p info, what tells of
 * askai, and @p list, an array for the supported versions: each tool's
 * definition, as tools/list lists it if a role may grant it, and as the
 * upstream's API has it if it is lent, the row's upstream definition if
 * the row has one; then mcp_static_tails()'s. */
static int
mcp_static_build (struct mcp_static  *st,
                  struct json_object *info,
                  struct json_object *list)
{
	for (uint32_t id = 0; id < st->n_offers; ++id) {
		struct mcp_offer *o = &st->offers[id];
		/* definition() of a plugin that askai loaded from a file gets the
		 * plugin's configuration. */
		struct mcp_plugin const *p = &st->plugins[o->part];
		void const *service = p->loaded ? p->loaded->config : p->service;
		struct json_object *def = o->kind->definition(service);
		int e = !def ? ENOMEM : id < st->n_granted
		      ? mcp_static_item(&o->listed, json_object_get(def)) : 0;
		struct json_object *lent = o->lend == LEND_NEVER ? nullptr
		                         : o->upstream ? o->upstream(service)
		                         : json_object_get(def);
		if (!e && o->lend != LEND_NEVER)
			e = lent ? mcp_static_item(&o->lent, ai_tool(st->ep->api, lent)) : ENOMEM;
		json_object_put(lent);
		json_object_put(def);
		if (e)
			return e;
	}
	return mcp_static_tails(st, info, list);
}

/* The attributes of a tool of a plugin that askai loaded from a file.  The
 * flags of its kind decide whether the turns are lent it.  The turns'
 * metrics count the calls of all such tools in one row.  The trace shows
 * the arguments of its calls. */
static struct mcp_row const mcp_loaded_row = { .metric = LENT_PLUGIN };

/* The row of the tool @p t of a registry: askai's own tool's, or that of
 * the tools of the plugins askai loaded. */
static struct mcp_row const *
mcp_row_of (struct registry_tool const *t)
{
	/* mcp_register() gave each of askai's own tools its row. */
	return t->data ? t->data : &mcp_loaded_row;
}

/* How the turns are lent the tool @p t of a registry, with
 * --upstream-tools: by its row, or for the kind of a plugin, built in or
 * loaded, which is not in a row, by its flags. */
static enum mcp_lend
mcp_lend_of (struct registry_tool const *t)
{
	struct mcp_row const *row = mcp_row_of(t);
	if (t->kind == &row->kind)
		return row->lend;
	return t->kind->flags & TOOL_LENT ? LEND_GRANTED : LEND_NEVER;
}

/* The tools of the registry of @p cfg that askai lends the turns, by how it
 * lends them: none without --upstream-tools, and only those that the
 * configuration file's member tools allows the upstream. */
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
		l.compact |= t->kind == &mcp_rows[KIND_COMPACT].kind ? bit : 0;
	}
	return l;
}

/* The kinds background, jobs, wait and timer, as bits. */
static constexpr uint32_t KINDS_AGENDA = 1u << KIND_BACKGROUND | 1u << KIND_JOBS |
                                         1u << KIND_WAIT | 1u << KIND_TIMER;

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
 * turns, as bits. */
static uint32_t
mcp_offered (struct config const *cfg)
{
	uint32_t turns = 1u << KIND_COMPACT | KINDS_AGENDA;
	return mcp_offered_clients(cfg) | (cfg->flags & CFG_UPSTREAM_TOOLS ? turns : 0);
}

int
mcp_register (struct registry     *reg,
              struct config const *cfg)
{
	uint32_t offered = mcp_offered(cfg);
	uint32_t granted = mcp_offered_clients(cfg);
	/* registry_add() inserts each tool that a role can grant before
	 * compact, which only the turns get. */
	for (enum mcp_kind k = 0; k < KIND_NONE; ++k) {
		if (!(offered >> k & 1))
			continue;
		struct mcp_row const *row = &mcp_rows[k];
		struct plugin const *plugin = &mcp_host_plugin;
		struct registry_tool tool = {
			.name = row->ptr,
			.kind = &row->kind,
			.data = row,
			.len  = row->len,
		};
		/* The configuration names the ask tool. */
		if (!tool.name) {
			tool.name = cfg->tool.name;
			tool.len = (uint32_t)strlen(tool.name);
		}
		/* The man tool and the git tool are kinds of plugins built in. */
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

/* The plugin of @p reg that askai loaded as @p plugin; nullptr for a
 * plugin built into askai. */
static struct plugin_loaded const *
mcp_loaded_of (struct registry const *reg,
               struct plugin const   *plugin)
{
	for (uint32_t i = 0; i < reg->n_loaded; ++i)
		if (reg->loaded[i]->plugin == plugin)
			return reg->loaded[i];
	return nullptr;
}

/* Whether the configuration file's member tools of @p cfg allows clients
 * the tool @p name of @p reg. */
static bool
mcp_allowed (struct registry const *reg,
             struct config const   *cfg,
             char const            *name)
{
	for (uint32_t id = 0; id < reg->n_granted; ++id)
		if (!strcmp(reg->tools[id].name, name))
			return cfg->tools_clients >> id & 1;
	return true;
}

/* Logs askai's own tools for the clients of @p cfg, each on or off, and for
 * a tool that is off, the option that turns it on, or "tools" if the
 * configuration file's member tools denies it to clients.  A tool without
 * such an option is not logged while it is off: compact, which only the
 * turns get, and the test aid. */
static void
mcp_report_own (struct registry const *reg,
                struct config const   *cfg)
{
	uint32_t on = mcp_offered_clients(cfg);
	int width = 0;
	for (enum mcp_kind k = 0; k < KIND_NONE; ++k) {
		size_t len = mcp_rows[k].ptr ? mcp_rows[k].len : strlen(cfg->tool.name);
		width = (int)len > width ? (int)len : width;
	}
	LOG_ALL(LOG_INFO, "built-in tools for clients:");
	for (enum mcp_kind k = 0; k < KIND_NONE; ++k) {
		struct mcp_row const *row = &mcp_rows[k];
		char const *name = row->ptr ? row->ptr : cfg->tool.name;
		if (on >> k & 1)
			LOG_ALL(LOG_INFO, "  %-*s  %s", width, name,
			        mcp_allowed(reg, cfg, name) ? "on" : "off (tools)");
		else if (row->option)
			LOG_ALL(LOG_INFO, "  %-*s  off (%s)", width, name, row->option);
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

/* Logs the plugin @p p of @p reg: its file, whether it got options, and its
 * tools, each that the configuration file's member tools of @p cfg denies
 * clients with "off (tools)". */
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
		        mcp_allowed(reg, cfg, name) ? "" : "  off (tools)");
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
	if (!reg->n_loaded)
		LOG_ALL(LOG_INFO, "plugins: none");
	for (uint32_t i = 0; i < reg->n_loaded; ++i)
		mcp_report_plugin(reg, cfg, reg->loaded[i]);
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

/* Returns the record of @p plugin for @p st: the plugin, its service and
 * its two part functions.  The service of askai's own kinds is @p st.  The
 * service of the man tool's plugin is @p manual, and the service of the git
 * tool's plugin is @p git.  Both of these have a part on every client loop.
 * A plugin that askai loaded from a file has no service and no part.  Its
 * record has the record of the loading from @p reg instead. */
static struct mcp_plugin
mcp_static_plugin (struct mcp_static     *st,
                   struct registry const *reg,
                   struct plugin const   *plugin,
                   struct manual         *manual,
                   struct git            *git)
{
	struct mcp_plugin const own[] = {
		{ .plugin = &mcp_host_plugin, .service = st },
		{ .plugin = &manual_plugin, .service = manual,
		  .part_create = manual_loop_create, .part_destroy = manual_loop_destroy },
		{ .plugin = &git_plugin, .service = git,
		  .part_create = git_loop_create, .part_destroy = git_loop_destroy },
	};
	for (size_t i = 0; i < ARRAY_SIZE(own); ++i)
		if (own[i].plugin == plugin)
			return own[i];
	return (struct mcp_plugin){ .plugin = plugin, .loaded = mcp_loaded_of(reg, plugin) };
}

/* Prepares the attach action of the notes tool, which the caller makes
 * only for turns that are lent tools, if @p st's upstream takes files
 * (ai_upload_url()) and the turns keep notes: the URL of the uploads, on
 * the host of the upstream's URL, so that the key goes to no other host,
 * their header lines, the upstream's and those of its API, and the table of
 * the files.  Returns 0, ENOMEM, or the error of uploads_create(). */
static int
mcp_static_uploads (struct mcp_static *st)
{
	struct ai_endpoint const *ep = st->ep;
	struct ai_upload const *up = ai_upload(ep->api);
	if (!up || !st->notes || !(st->files_url = ai_upload_url(ep)))
		return 0;
	size_t n = 0;
	size_t m = 0;
	while (ep->headers && ep->headers[n])
		++n;
	while (up->lines[m])
		++m;
	st->files_lines = malloc((n + m + 1) * sizeof *st->files_lines);
	if (!st->files_lines)
		return ENOMEM;
	for (size_t i = 0; i < n; ++i)
		st->files_lines[i] = ep->headers[i];
	for (size_t i = 0; i <= m; ++i)
		st->files_lines[n + i] = up->lines[i];
	int e = 0;
	st->uploads = uploads_create(&e);
	return e;
}

/* The bytes of a file that a client may hand askai inline with requests of
 * @p body bytes at most, base64 taking 4 bytes for 3. */
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
	bool lends = cfg->flags & CFG_UPSTREAM_TOOLS;
	struct registry const *reg = &cfg->registry;
	struct mcp_static *st = malloc(sizeof *st);
	if (!st) {
		*e = ENOMEM;
		return nullptr;
	}
	*st = (struct mcp_static){
		.ep = &cfg->ep,
		.tool = &cfg->tool,
		.offers[TOOL_MAX] = {
			.kind = &mcp_rows[KIND_NONE].kind,
			.name = mcp_rows[KIND_NONE].ptr,
			.len = mcp_rows[KIND_NONE].len,
			.metric = mcp_rows[KIND_NONE].metric
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
		.notes = notes,
		.journal = journal,
		.ledger = ledger,
		.roster = roster,
		.web = web,
		.files_inline = mcp_files_inline(cfg->http.limits.body),
		.seed = loop_clock(),
		.lim = cfg->mcp,
		.turns = cfg->turns
	};
	for (uint32_t i = 0; i < reg->n_plugins; ++i)
		st->plugins[i] = mcp_static_plugin(st, reg, reg->plugins[i], manual, git);
	/* A zeroed dstr is no empty string (dstr.h). */
	for (uint32_t id = 0; id < reg->n_tools; ++id) {
		struct registry_tool const *t = &reg->tools[id];
		struct mcp_row const *row = mcp_row_of(t);
		struct mcp_offer *o = &st->offers[id];
		/* The turns are lent tools with --upstream-tools. */
		*o = (struct mcp_offer){
			.listed = DSTR_INIT,
			.lent = DSTR_INIT,
			.kind = t->kind,
			.name = t->name,
			.sized = row->sized,
			.upstream = row->upstream,
			.len = t->len,
			.part = (uint8_t)t->part,
			.lend = lends ? mcp_lend_of(t) : LEND_NEVER,
			.metric = row->metric,
			.trace = row->trace
		};
	}
	st->lending = mcp_lending(cfg);
	if (journal)
		st->seed = journal_seed(journal);
	else
		(void)getrandom(&st->seed, sizeof st->seed, GRND_NONBLOCK);
	if (lends && (*e = mcp_static_uploads(st))) {
		mcp_static_destroy(&st);
		return nullptr;
	}

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
