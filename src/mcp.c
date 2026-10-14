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
 * text, so that the request only escapes that into the reply.  Closing the
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
 * GET /metrics answers a trusted client (src/stream.h) with the metrics,
 * rendered a share a step (src/metrics.h).  Draining, tool calls that wait
 * for the upstream, and those yet to start, end with an error, which on an
 * event stream is its last event.
 */
#define _GNU_SOURCE /* memrchr() */
#include <errno.h>
#include <inttypes.h>
#include <json-c/printbuf.h>
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
#include "dstr.h"
#include "git.h"
#include "instance.h"
#include "jb.h"
#include "job.h"
#include "ledger.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "manual.h"
#include "mcp.h"
#include "metrics.h"
#include "notes.h"
#include "plugin.h"
#include "registry.h"
#include "roster.h"
#include "shelf.h"
#include "stream.h"
#include "task.h"
#include "util.h"

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
	CALLS_MAX    = 1 << 20,   /* bytes of a turn's tool calls */
	LEND_MAX     = 64 << 10,  /* bytes of a tool's output that a turn takes,
	                             at most */
	SUMMARY_MAX  = 16 << 10,  /* bytes of compact's summary */
	JOBS_MAX     = 8,         /* jobs that an ask call's turns may start */
	TIMERS_MAX   = 8,         /* timers they may have set at once */
	WAIT_MAX     = 600,       /* seconds that a wait may take */
	TIMER_MAX    = 3600,      /* seconds that a timer may be set to */
	TIMER_NOTE   = 200,       /* bytes of a timer's note */
	TRACE_MAX    = 48,        /* tool calls the trace tells of */
	TRACE_ARGS   = 200,       /* bytes of a call's arguments it tells */
	CALL_ID      = sizeof "call_4294967295_4294967295",  /* bytes of a
	                                                        made-up call id */
	POLL_MS      = 1000,      /* between a task's polls, as its client is asked */
	ISO_TIME     = sizeof "2026-10-13T12:34:56.789Z",
};

/* What a git of the notes is for. */
enum mcp_fetch {
	FETCH_WRITE,    /* the notes tool's write */
	FETCH_READ,     /* its read */
	FETCH_RESUME,   /* the notes of a thread the call goes on with */
	FETCH_COMPACT,  /* compact's */
	FETCH_FULL,     /* a compaction of a context that filled up */
};

static constexpr uint64_t NS = 1'000'000'000;
static constexpr uint64_t PROGRESS_NS = 100'000'000;  /* between a call's
                                                         notifications */
static constexpr uint64_t TASK_NS = 1'000'000'000;  /* a call's run before it
                                                       becomes a task, for a
                                                       client that takes them */

/** Newest first; the first MODERN revisions carry their version in _meta. */
static char const versions[][sizeof "2026-07-28"] = {
	"2026-07-28",
	"2025-11-25",
	"2025-06-18",
	"2025-03-26",
	"2024-11-05",
};
enum { MODERN = 1 };

/** The revision before Streamable HTTP, whose clients take no event stream. */
static char const pre_streams[] = "2024-11-05";

static char const meta_version[] = "io.modelcontextprotocol/protocolVersion";
static char const meta_caps[] = "io.modelcontextprotocol/clientCapabilities";
static char const meta_server[] = "io.modelcontextprotocol/serverInfo";
static char const meta_tasks[] = "io.modelcontextprotocol/tasks";

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

/* The kinds of tool that askai may offer, each its row of mcp_rows, in the
 * order of their ids: those that a role may grant, which clients list and
 * call, before KIND_NOTES; from notes on, those only turns are given; then
 * that of a tool it does not offer. */
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

/* A tool that askai offers: its definition as tools/list lists it, if a
 * role may grant it, and as the upstream's API has it, if it is lent, each
 * after a comma; its kind; its name; its plugin's part; how the turns are
 * lent it; its row in the turns' metrics; and what the trace tells of its
 * calls. */
struct mcp_offer {
	struct dstr             listed;
	struct dstr             lent;
	struct tool_kind const *kind;
	char const             *name;
	uint32_t                len;
	uint8_t                 part;    /* its plugin's, in plugins[] and a
	                                    loop's parts[] */
	uint8_t                 lend;    /* enum mcp_lend */
	uint8_t                 metric;  /* enum metrics_lent */
	uint8_t                 trace;   /* enum mcp_trace */
};

/* A plugin whose tools askai offers.  A built-in plugin can have a service,
 * the object that every loop shares, and a part of it on each client loop.
 * A plugin that askai loaded from a file has neither. */
struct mcp_plugin {
	struct plugin const *plugin;
	void                *service;
	mcp_part_create_fn  *part_create;
	mcp_part_destroy_fn *part_destroy;
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
	tool_mask                 lends;          /**< lent as a role grants them */
	tool_mask                 always;         /**< lent whenever the turns are
	                                               lent anything */
	tool_mask                 with;           /**< lent beside those */
	tool_mask                 detach;         /**< those that may run in the
	                                               background */
	tool_mask                 detached;       /**< lent beside one of those */
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
	struct ledger            *ledger;       /**< the tasks' */
	struct roster            *roster;       /**< the tool calls that run and
	                                             are not tasks */
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
	size_t              skip;      /* the byte of the output that its window,
	                                  given to a turn, starts at */
	size_t              window;    /* and the bytes of that */
	uint64_t            start;     /* loop_clock() as the call began */
	struct handle       call;      /* names it to the post that completes
	                                  it, as mcp_slot_name() makes it */
	uint16_t            note_len;  /* bytes of note */
	uint8_t             tool;      /* its call's tool, by its offer's id;
	                                  TOOL_MAX: none */
	uint8_t             fetch;     /* enum mcp_fetch: what its notes' git is
	                                  for */
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
	struct json_object *args;      /* a job's call's arguments, which the
	                                  list of jobs shows */
	struct mcp_slot    *of;        /* the job whose output it gives, if it
	                                  gives a job's */
	char                note[360]; /* The text that askai adds to the
	                                  call's output.  With 360 bytes the
	                                  slot is 512 bytes and has no padding
	                                  at its end. */
};

static_assert(TOOL_MAX <= UINT8_MAX, "a slot's tool holds TOOL_MAX");

struct mcp_agenda;

static void mcp_agenda_destroy (struct mcp_agenda **p_agenda);

/** A request, from its stream's head to its end or cancellation. */
struct mcp_req {
	struct list          hook;       /* on mcp->calls while a tool call runs,
	                                    then on mcp->freeing, then spare */
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
	struct mcp_slot      self;       /* its own call: its answer, the tool
	                                    that a client calls, and the notes'
	                                    git it reads */
	struct dstr          trace;      /* the tool calls the turns made, for the
	                                    client; likewise */
	struct buf           calls;      /* a turn's tool calls, as its job had
	                                    them, while they run */
	struct mcp_slot     *slots;      /* those calls', JOB_CALLS of them, from
	                                    its first turn with calls to its end */
	struct mcp_agenda   *agenda;     /* its turns' jobs and timers, from the
	                                    first to its end */
	struct ledger_task  *task;       /* the task the call runs as, once it is
	                                    one */
	struct ledger_task  *shown;      /* the task whose result tasks/get
	                                    copies, while it does */
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
	size_t               at;         /* where the call whose result is written
	                                    next is in calls */
	size_t               launch_at;  /* and the call launched next */
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
	uint16_t             next;       /* the splice to write next, or piece */
	uint16_t             part;       /* the part of the turn being written,
	                                    or its call under way */
	uint16_t             n_calls;    /* the turn's tool calls */
	uint8_t              phase;      /* enum mcp_phase */
	uint8_t              method;     /* enum metrics_method */
	uint8_t              asks;       /* enum job_ask: JOB_BACKGROUND for
	                                    test_task's prompt */
	uint8_t              forced;     /* compactions askai made since a turn
	                                    went through */
	bool                 modern;
	bool                 sse_ok;     /* the client takes an event stream */
	bool                 sse;        /* the response is one */
	bool                 failed;     /* the answer reports an error */
	bool                 tick;       /* a keep-alive or notification may be
	                                    due */
	bool                 progress;   /* the job's progress moved on */
	bool                 blocked;    /* waiting for on_writable() */
	bool                 listed;     /* the list that the turn's items go in
	                                    has one */
	bool                 branch;     /* the thread has notes */
	bool                 stopped;    /* the turns ran out with tools called */
	bool                 cut;        /* the turn's answer reached the output
	                                    token limit */
	char                 thread[NOTES_ID + 1];  /* the notes' thread's id */
	char                 ref[NOTES_REF];        /* and its ref */
};

/* A timer that a turn set: when it fires, its number, and its note. */
struct mcp_timer {
	uint64_t due;               /* loop time; 0: none set */
	uint32_t id;
	uint32_t len;               /* bytes of note */
	char     note[TIMER_NOTE];
};

/* What an ask call's turns have under way besides themselves: the jobs
 * they started in the background, the timers they set, a wait call that
 * waits for those, what happened that the next result or message says, and
 * the answers they gave before what happened woke them. */
struct mcp_agenda {
	struct mcp_slot   jobs[JOBS_MAX];      /* by number - 1 */
	struct mcp_timer  timers[TIMERS_MAX];
	struct loop_timer alarm;     /* the first due: a timer, or the wait's end */
	struct dstr       notices;   /* what happened, which none said yet */
	struct dstr       told;      /* what the result or message being written
	                                says */
	struct dstr       said;      /* those answers, each with what woke the
	                                turns after it, which the reply begins
	                                with */
	struct mcp_req   *req;
	struct mcp_slot  *wait;      /* the wait call's slot, while it waits */
	uint64_t          wait_end;  /* when it waits no more */
	uint32_t          wait_for;  /* bits of the jobs it waits for; 0: any */
	uint32_t          n_jobs;
	uint32_t          n_timers;  /* timers ever set, their numbers */
	uint32_t          waited;    /* seconds the wait may take */
};

struct mcp {
	struct list              calls;     /* requests with a tool call under way */
	struct list              tasks;     /* requests whose call is a task */
	struct list              freeing;   /* requests freeing their message */
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
	buf_fini(&job->request);
	buf_fini(&job->key);
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
		req->slots[k] = (struct mcp_slot){
			.step.fn = mcp_slot_step,
			.answer = DSTR_INIT,
			.req = req,
			.tool = TOOL_MAX,
			.role = ROLE_TURN
		};
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

/* Drops the call in @p slot.  If the call's own word is set, the call
 * counts as ended first, so the call API ignores an attempt of the kind's
 * drop() to end the call.  Then the function calls the kind's drop(), or it
 * logs a warning if the kind has no drop().  A request drops its own slot
 * between two turns too, and its own slot has no own word, so that slot
 * does not count as ended then.  Last, the function puts the call's
 * instance back (instance_table_put()), clears the lent output, destroys
 * the child of the call's notes, removes the call's step from the loop's
 * queue and frees the call's handle. */
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
	instance_table_put(&slot->req->mcp->instances, &slot->inst);
	slot->lent = (struct span){};
	child_destroy(&slot->child);
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
	job_post(job, mcp->home, &job->to_home, &job->home_bits, JOB_CANCEL);
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
	if (!req->hook.next)
		list_append(&mcp->calls, &req->hook);
	if (mcp->draining)
		loop_ready_bulk(mcp->loop, &mcp->drain);
}

/* Take the request off the calls under way, if it is among them. */
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

/* The call of a task ends: cancelled as its client asked, failed as
 * req->error says or its reply, in the task's record, is an error or not
 * whole, else completed.  Publish that, and let go of the task. */
static void
mcp_req_task_end (struct mcp_req *req)
{
	static char const erred[] = "\"error\":";
	struct ledger_task *task = req->task;
	size_t n = sizeof erred - 1;
	bool failed = !task->result || task->len != task->size ||
	              (task->len >= n && !memcmp(task->result, erred, n));
	enum ledger_status status = req->error == ECANCELED ? LEDGER_CANCELLED
	                          : req->error || failed ? LEDGER_FAILED
	                          : LEDGER_COMPLETED;
	ledger_end(req->mcp->st->ledger, task, status);
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
static void
mcp_req_finish (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	mcp_req_leave(req);
	if (req->task)
		mcp_req_task_end(req);
	ledger_task_put(req->shown);
	req->shown = nullptr;
	mcp_req_unwait(req);
	mcp_req_drop(req);
	loop_timer_stop(mcp->loop, &req->timer);
	jb_parser_fini(&req->parser, req->phase == PHASE_ANSWER || req->phase == PHASE_ARGS
	                             ? &mcp->answers : &mcp->pool,
	               &req->reaper);
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
	dstr_clear(&req->self.answer);
	dstr_clear(&req->trace);
	for (size_t k = 0; req->slots && k < JOB_CALLS; ++k)
		dstr_cleanup(&req->slots[k].answer);
	free(req->slots);
	req->slots = nullptr;
	mcp_agenda_destroy(&req->agenda);
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
	req->error = e;
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

/* Queue @p bytes of the response: 0, or EPIPE once the stream can carry no
 * more.  Once the transport asks to wait, the request waits for
 * on_writable(). */
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
 * '{"jsonrpc":"2.0","id":ID,"result":', or "error", without the id if the
 * request has none; for a task's call only '"result":', or "error", which
 * tasks/get writes into its own result. */
static int
mcp_req_open (struct mcp_req *req,
              bool            error)
{
	if (req->task) {
		struct mcp_piece const key = error ? FRAG("\"error\":") : FRAG("\"result\":");
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
		error ? FRAG("\"error\":") : FRAG("\"result\":"),
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
	int e = mcp_req_open(req, true);
	if (e)
		return e;
	e = mcp_req_put(req, p, ARRAY_SIZE(p));
	if (e)
		return e;
	e = mcp_req_put(req, data, n_data);
	return e ? e : mcp_req_put(req, &FRAG("}}"), 1);
}

/* Reply with a JSON-RPC error, as mcp_req_error_put() writes it. */
static void
mcp_req_error (struct mcp_req         *req,
               uint16_t                status,
               int                     code,
               struct mcp_piece        message,
               struct mcp_piece const *data,
               size_t                  n_data)
{
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
	int e = mcp_req_open(req, false);
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
	int e = mcp_req_open(req, false);
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
		n += util_format(tail + n, size - n, "It kept notes in the thread %s: to "
		                 "go on with this work, give that as the thread "
		                 "argument.", req->thread);
	return n;
}

/* Close the result's text; then, for an ask call whose turns called tools,
 * or kept notes, what they called and where the notes are, as a text of its
 * own. */
static int
mcp_req_trace_put (struct mcp_req *req)
{
	static char const none[] = "The other AI called no tools.\n";
	if (!dstr_len(&req->trace) && !req->branch && !req->stopped)
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

/* End a tool result, its trace after its text, and send it. */
static void
mcp_req_tool_close (struct mcp_req *req)
{
	struct mcp_static const *st = req->mcp->st;
	struct mcp_piece const p[] = {
		FRAG("]"),
		req->failed ? FRAG(",\"isError\":true") : FRAG(""),
		req->modern ? DSTR(&st->modern_tail) : FRAG("}"),
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
		dstr_clear(&slot->answer);
		if (!dstr_cat_n(&slot->answer, text.ptr, text.len)) {
			mcp_req_fail(req, ENOMEM);
			return;
		}
		mcp_slot_done(slot, failed);
		return;
	}
	req->failed = failed;
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
	char const *str = (char const *)text.ptr;
	if (failed)
		LOG(LOG_WARN, "tool call failed after %.1f s: %.*s", mcp_elapsed(slot->start),
		    (int)mcp_clip(str, text.len, 300), str);
	else
		LOG(LOG_VERBOSE, "tool call answered after %.1f s (%zu bytes)",
		    mcp_elapsed(slot->start), text.len);
	loop_timer_stop(req->mcp->loop, &req->timer);
	size_t off = req->lent ? 0 : slot->skip;
	if (off > text.len) {
		char why[96];
		size_t n = util_format(why, sizeof why, "The output has %zu bytes, fewer "
		                       "than the offset.", text.len);
		mcp_slot_result(slot, ESC(why, n), true);
		return;
	}
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
 * @p text, which need no escaping, into the reply. */
static int
mcp_req_notify_put (struct mcp_req *req,
                    uint64_t        value,
                    char const     *text,
                    size_t          len)
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
		FRAG("\"}}\n\n"),
	};
	return mcp_req_put(req, p, ARRAY_SIZE(p));
}

/* Send a notification of progress @p value, saying the @p len bytes of
 * @p text, on the call's event stream. */
static void
mcp_req_notify (struct mcp_req *req,
                uint64_t        value,
                char const     *text,
                size_t          len)
{
	if (!mcp_req_notify_put(req, value, text, len))
		(void)mcp_req_write(req, buf_peek(&req->reply));
	buf_consume(&req->reply, buf_peek(&req->reply).len);
	req->noted = req->mcp->loop->now;
	req->next_note = value + 1;
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
	mcp_req_notify(req, value, text, n);
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
	mcp_req_notify(req, req->next_note, text, n);
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

static struct ledger_task *mcp_req_task_open (struct mcp_req *req);
static void mcp_req_detach (struct mcp_req *req, struct ledger_task *task);

/* While a call waits: its becoming a task, if that is due; else a
 * keep-alive, if one is due, and a notification of its progress, if that
 * moved on, the last went PROGRESS_NS ago and the client has read what it
 * was sent; then the timer for the next of them. */
static void
mcp_req_tick (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	uint32_t keepalive = req->mcp->st->lim.keepalive;
	if (req->promote_at <= loop->now && !req->blocked) {
		req->promote_at = UINT64_MAX;
		struct ledger_task *task = mcp_req_task_open(req);
		if (task) {
			mcp_req_detach(req, task);
			return;
		}
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
 * window". */
static bool
mcp_overflowed (char const *why,
                size_t      len)
{
	static char const word[] = "context ";
	size_t n = sizeof word - 1;
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
		buf_fini(&job->request);
		buf_fini(&job->key);
		buf_fini(&job->result);
		buf_fini(&job->calls);
		buf_fini(&job->thought);
		buf_fini(&job->replay);
		free(job);
		return;
	}
	if (bits & JOB_RETURN) {
		mcp_shelve_job(self, job);
		return;
	}
	struct mcp_req *req = loop_lookup(loop, job->call);
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
	buf_fini(&job->request);
	buf_fini(&job->key);
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
	if (req->splices == ARRAY_SIZE(req->splice) ||
	    printbuf_strappend(pb, "\"\"") < 0)
		return -1;
	req->splice[req->splices++] = (struct mcp_splice){ str, (size_t)pb->bpos - 1 };
	return 0;
}

/* Write the draft from where splice @p k - 1 goes to where splice @p k goes,
 * or to its end. */
static int
mcp_req_copy (struct mcp_req *req,
              size_t          k)
{
	size_t from = k ? req->splice[k - 1].at : 0;
	size_t len = (k < req->splices ? req->splice[k].at : req->len) - from;
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

/* Write the call's key into @p job, and name the request by a handle for
 * the job's completion. */
static int
mcp_req_name (struct mcp_req *req,
              struct job     *job)
{
	struct stream *s = req->s;
	int e = mcp_key(job, s->ops->field(s, F_SESSION));
	return e ? e : loop_handle(req->mcp->loop, req, &req->call);
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
	if (req->lent) {
		asks |= JOB_TOOLS;
		hist_add(&mcp->counts->calls.turns, 1);
	}
	job->asks = asks;
	req->sent = buf_peek(&job->request).len;
	req->base = req->next_note;
	req->place = 0;
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

/* A client asked to cancel the task whose cancel @p msg is: end its call,
 * if it still runs here, as a hang-up would, the task cancelled. */
static void
mcp_on_task_cancel (struct loop *loop,
                    struct msg  *msg)
{
	struct ledger_task *task = container_of(msg, struct ledger_task, cancel);
	struct mcp_req *req = loop->exit ? nullptr : loop_lookup(loop, task->who);
	ledger_task_put(task);
	if (!req)
		return;
	LOG(LOG_VERBOSE, "task cancelled after %.1f s", mcp_elapsed(req->head));
	mcp_req_abort(req, ECANCELED);
}

/* This is the function of the message that roster_cancel() posts when a
 * client cancels a tool call with notifications/cancelled.  @p msg is the
 * member cancel of the call's record.  If the call still runs on @p loop
 * and its reply has not begun, the function ends the call with
 * mcp_req_finish(), as a hang-up does.  Before that, it answers a JSON
 * request with the status 202 and no body.  An event stream ends without a
 * response. */
static void
mcp_on_withdraw (struct loop *loop,
                 struct msg  *msg)
{
	struct roster_call *call = container_of(msg, struct roster_call, cancel);
	struct mcp_req *req = loop->exit ? nullptr : loop_lookup(loop, call->who);
	roster_call_put(call);
	if (!req || req->phase >= PHASE_REPLY)
		return;
	if (!req->sse)
		(void)req->s->ops->head(req->s, 202, (struct span){}, 0);
	LOG(LOG_VERBOSE, "tool call cancelled after %.1f s", mcp_elapsed(req->self.start));
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
		e = loop_handle(mcp->loop, req, &who);
	if (!e) {
		req->rostered = roster_enter(mcp->st->roster, buf_peek(&key), hash, mcp->loop,
		                             who, mcp_on_withdraw, &e);
		if (e)
			loop_forget(mcp->loop, who);
	}
	buf_fini(&key);
	return e;
}

/* The record of the task the call becomes, in the ledger for any loop to
 * find; nullptr if the ledger has no room, or the call cannot be named, as
 * the call then goes on as it is. */
static struct ledger_task *
mcp_req_task_open (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct handle who;
	int e = loop_handle(mcp->loop, req, &who);
	struct ledger_task *task = e ? nullptr
	                         : ledger_open(mcp->st->ledger, req->s->principal, mcp->loop,
	                                       who, mcp_on_task_cancel, &e);
	if (task)
		return task;
	if (who.bits)
		loop_forget(mcp->loop, who);
	LOG(LOG_VERBOSE, "tool call kept from being a task: %s", strerror(e));
	return nullptr;
}

/* Answer the call's client with @p task, which the call runs as from now
 * on, its stream the stand-in, which writes its reply into the task's
 * record; or, if the answer cannot go, end the task and the call. */
static void
mcp_req_detach (struct mcp_req     *req,
                struct ledger_task *task)
{
	static_assert(POLL_MS == 1000, "the reply says");
	struct mcp *mcp = req->mcp;
	struct stream *s = req->s;
	char at[ISO_TIME];
	size_t n = mcp_iso_time(at, task->created);
	struct mcp_piece const p[] = {
		FRAG("{\"resultType\":\"task\","),
		DSTR(&mcp->st->server_meta),
		FRAG(",\"taskId\":\""),
		RAW(task->id, LEDGER_ID),
		FRAG("\",\"status\":\"working\",\"createdAt\":\""),
		RAW(at, n),
		FRAG("\",\"lastUpdatedAt\":\""),
		RAW(at, n),
		FRAG("\",\"ttlMs\":null,\"pollIntervalMs\":1000}}"),
	};
	int e = mcp_req_open(req, false);
	e = e ? e : mcp_req_put(req, p, ARRAY_SIZE(p));
	e = e ? e : req->sse ? mcp_req_put(req, &FRAG("\n\n"), 1)
	          : s->ops->head(s, 200, SPAN(JSON_FIELD), buf_peek(&req->reply).len);
	e = e ? e : mcp_req_write(req, buf_peek(&req->reply));
	buf_consume(&req->reply, buf_peek(&req->reply).len);
	if (e) {
		ledger_end(mcp->st->ledger, task, LEDGER_CANCELLED);
		loop_forget(mcp->loop, task->who);
		ledger_task_put(task);
		mcp_req_abort(req, e);
		return;
	}
	/* The request has its answer, which is the task.  From now on a client
	 * can cancel the call only with tasks/cancel. */
	mcp_req_leave(req);
	req->stand_in = *s;
	req->stand_in.ops = &stand_in_ops;
	s->ops->end(s, 0);
	req->s = &req->stand_in;
	req->task = task;
	list_append(&mcp->tasks, &req->task_hook);
	req->sse = req->blocked = req->progress = false;
	req->ping_at = UINT64_MAX;
	json_object_put(req->token);
	req->token = nullptr;
	loop_timer_stop(mcp->loop, &req->timer);
	LOG(LOG_VERBOSE, "tool call became task %.*s after %.1f s", LEDGER_ID, task->id,
	    mcp_elapsed(req->head));
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
	return slot->inst;
}

struct loop *
mcp_slot_loop (struct mcp_slot const *slot)
{
	return slot->req->mcp->loop;
}

uint64_t
mcp_slot_principal (struct mcp_slot const *slot)
{
	return slot->req->principal;
}

void **
mcp_slot_own (struct mcp_slot *slot)
{
	return &slot->own;
}

/* @p slot's call waits no more: let go of its name, if it has one, and of
 * its step, if queued. */
static void
mcp_slot_wake (struct mcp_slot *slot)
{
	if (slot->call.bits)
		loop_forget(slot->req->mcp->loop, slot->call);
	slot->call = (struct handle){};
	loop_unready(&slot->step);
	mcp_slot_unwait(slot);
}

void
mcp_slot_tell (struct mcp_slot  *slot,
               struct tool_text  text,
               uint32_t          flags)
{
	if (slot->done)
		return;
	size_t len = jb_utf8(text.ptr, text.len < TOOL_TELL_MAX ? text.len : TOOL_TELL_MAX);
	mcp_slot_wake(slot);
	/* The output is the text whole, not lent, nor from an offset. */
	slot->skip = 0;
	slot->lent = (struct span){};
	mcp_slot_result(slot, ESC(text.ptr, len), flags & TOOL_FAILED);
}

void
mcp_slot_finish (struct mcp_slot *slot,
                 uint32_t         flags)
{
	if (slot->done)
		return;
	mcp_slot_wake(slot);
	mcp_slot_respond(slot, flags & TOOL_FAILED);
}

void
mcp_slot_fail (struct mcp_slot *slot,
               int              e)
{
	if (slot->done)
		return;
	slot->done = true;
	mcp_req_fail(slot->req, e);
}

int
mcp_slot_offset (struct mcp_slot    *slot,
                 struct json_object *args)
{
	if (slot->done)
		return EINVAL;
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
	*taken = 0;
	if (slot->done)
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
void
mcp_slot_park (struct mcp_slot *slot)
{
	if (slot->done)
		return;
	struct mcp_req *req = slot->req;
	if (mcp_slot_offer(slot)->kind->flags & TOOL_STREAM && req->sse_ok && !req->sse)
		mcp_req_stream(req);
	if (slot == &req->self)
		mcp_req_park(req);
	else
		mcp_req_wait(req);
}

void
mcp_slot_ready (struct mcp_slot *slot)
{
	if (slot->done)
		return;
	if (!mcp_slot_offer(slot)->kind->step) {
		mcp_slot_tell(slot, TOOL_TEXT("The tool's kind has no step(), so the call "
		                              "cannot go on."), TOOL_FAILED);
		return;
	}
	loop_ready_bulk(slot->req->mcp->loop, &slot->step);
}

int
mcp_slot_name (struct mcp_slot *slot,
               struct handle   *who)
{
	if (slot->done) {
		*who = (struct handle){};
		return EINVAL;
	}
	int e = slot->call.bits ? 0 : loop_handle(slot->req->mcp->loop, slot, &slot->call);
	*who = slot->call;
	return e;
}

struct mcp_slot *
mcp_slot_named (struct loop const *loop,
                struct handle      who)
{
	return loop->exit ? nullptr : loop_lookup(loop, who);
}

static void mcp_slot_notes_fetch (struct mcp_slot *slot, enum mcp_fetch fetch,
                                  char const *search);

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
               struct json_object *system)
{
	struct mcp_static const *st = req->mcp->st;
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
	req->draft = ai_request(st->ep, instructions, prompt, tools);
	json_object_put(instructions);
	json_object_put(tools);
	req->prompt = prompt;
	if (!req->draft)
		return ENOMEM;
	req->text = json_object_to_json_string_length(req->draft, JB_WIRE, &req->len);
	return req->text ? mcp_req_copy(req, 0) : ENOMEM;
}

/* Start a call that asks the upstream, with the arguments @p args, of the
 * class the request asks for, lending the turns the tools req->lent says,
 * if any: build its upstream request, then submit its job.  The notes the turns keep, if askai keeps
 * them, go in the thread the call goes on with, else in a new one. */
static void
mcp_req_ask (struct mcp_req     *req,
             struct json_object *args)
{
	struct notes const *notes = req->lent ? req->mcp->st->notes : nullptr;
	/* Argument problems go back to the model so it can correct itself. */
	struct json_object *prompt = jb_get(args, "prompt");
	struct json_object *system = jb_get(args, "system");
	struct json_object *thread = jb_get(args, "thread");
	char const *text = jb_text(prompt);
	size_t len = 0;
	char const *id = jb_str(thread, &len);
	struct mcp_piece problem =
		!text || !*text ? TEXT("The prompt argument must be a non-empty string.")
		: system && !jb_text(system) ? TEXT("The system argument must be a string.")
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
	if (id) {
		memcpy(req->thread, id, sizeof req->thread);
		notes_ref(req->s->principal, req->thread, req->ref);
	} else if (notes) {
		if (notes_id(req->thread)) {
			mcp_req_error(req, 500, INTERNAL_ERROR, FRAG("Cannot make a thread's id"),
			              nullptr, 0);
			return;
		}
		notes_ref(req->s->principal, req->thread, req->ref);
	}
	mcp_req_wait(req);
	int e = mcp_req_draft(req, prompt, system);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	req->s->ops->waiting(req->s);
	if (req->sse_ok)
		mcp_req_stream(req);
	/* The message, which holds the thread's id, goes now: going on with a
	 * thread, its notes go with the prompt. */
	bool resume = id;
	mcp_req_dispose(req);
	if (resume) {
		mcp_slot_notes_fetch(&req->self, FETCH_RESUME, nullptr);
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
	mcp_req_ask(slot->req, args);
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

/* The tools that @p req's turns may call: those lent as the client's role
 * grants them, and those lent always; and if any, those lent beside them,
 * and if any may run in the background, those lent beside those. */
static tool_mask
mcp_req_lent (struct mcp_req const *req)
{
	struct mcp_static const *st = req->mcp->st;
	tool_mask lent = (req->s->tools & st->lends) | st->always;
	tool_mask more = lent & st->detach ? st->detached : 0;
	return lent ? lent | st->with | more : 0;
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
	return req->used ? req->used + (len > req->sent ? len - req->sent : 0) / 4
	                 : len / 4;
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
	struct span calls = buf_peek(&job->calls);
	long told = mcp->st->ep->context;
	req->used = job->used;
	req->context = told > 0 ? (uint32_t)told : job->context;
	req->cut = ai_cut(mcp->st->ep->api, job->reason);
	req->forced = 0;
	if (calls.len > CALLS_MAX) {
		mcp_slot_result(&req->self, TEXT("The other AI's tool calls in one turn took "
		                              "more than 1 MiB, so none was run."), true);
		return;
	}
	uint16_t n = 0;
	for (size_t off = 0; off < calls.len; ++n)
		off += mcp_call_at(calls.ptr + off).size;
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
	req->at = 0;
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
	struct span all = buf_peek(&req->job->calls);
	for (; req->part < req->n_calls; ++req->part) {
		struct mcp_call c = mcp_call_at(all.ptr + req->at);
		int e = mcp_req_turn_arg(req, c.args);
		if (e == EAGAIN) {
			loop_ready_bulk(req->mcp->loop, &req->step);
			return;
		}
		if (e) {
			mcp_req_fail(req, e);
			return;
		}
		req->at += c.size;
	}
	/* The turn goes after the request's messages, which it ends no more. */
	struct ai_bytes end = ai_format(req->mcp->st->ep->api)->end;
	buf_cut(&req->upload, buf_peek(&req->upload).len - end.len, end.len);
	req->part = 0;
	req->at = 0;
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
	struct mcp_call c = mcp_call_at(buf_peek(&req->job->calls).ptr + req->at);
	struct mcp_piece items = mcp_req_items(req, c.replay);
	int e = f->call.len ? mcp_req_call_put(req, f, &c, items)
	                    : mcp_req_upload(req, &items, 1);
	if (e)
		return e;
	req->listed = req->listed || items.len;
	req->replayed = c.replay;
	req->at += c.size;
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
	struct span all = buf_peek(&req->job->calls);
	int e = mcp_piece_write(&RAW(all.ptr, all.len), 0, all.len, &req->calls,
	                        CALLS_MAX);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	loop_work(loop, LOOP_COPY, all.len);
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

/* The id of the tool of @p name, of @p len bytes, that the turns may
 * call; TOOL_MAX if none. */
static uint32_t
mcp_req_tool_of (struct mcp_req const *req,
                 char const           *name,
                 size_t                len)
{
	struct mcp_static const *st = req->mcp->st;
	for (uint32_t id = 0; id < st->n_offers; ++id) {
		struct mcp_offer const *o = &st->offers[id];
		if (req->lent >> id & 1 && o->len == len && !memcmp(o->name, name, len))
			return id;
	}
	return TOOL_MAX;
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
		a->jobs[k] = (struct mcp_slot){
			.step.fn = mcp_slot_step,
			.answer = DSTR_INIT,
			.req = req,
			.tool = TOOL_MAX,
			.role = ROLE_JOB
		};
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
		struct mcp_timer *t = &a->timers[i];
		if (!t->due || t->due > loop->now)
			continue;
		t->due = 0;
		fired = true;
		if (!dstr_printf(&a->notices, "[askai: timer %" PRIu32 " fired%s%.*s.]\n", t->id,
		                 t->len ? ": " : "", (int)t->len, t->note)) {
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

/* Start the call that @p args asks for as a job in the background, in a
 * slot of the agenda's: of a tool the turns may call that may run so.  The
 * call in @p slot says the job's number at once. */
static void
mcp_slot_background (struct mcp_slot    *slot,
                     struct json_object *args)
{
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
	job->args = call ? json_object_get(call) : json_object_new_object();
	if (!job->args) {
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

/* The jobs tool's call in @p slot: list the jobs, or give the output of
 * the one @p args names, or cancel it. */
static void
mcp_slot_jobs (struct mcp_slot    *slot,
               struct json_object *args)
{
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

/* The wait tool's call in @p slot: until a job of those @p args names, or
 * any, finishes, a timer fires, or its seconds are up.  What happened and
 * no result said yet ends it at once. */
static void
mcp_slot_wait (struct mcp_slot    *slot,
               struct json_object *args)
{
	static_assert(WAIT_MAX == 600 && JOBS_MAX <= 32, "the problem says");
	struct mcp_req *req = slot->req;
	struct json_object *o = jb_get(args, "seconds");
	int64_t secs = json_object_is_type(o, json_type_int) ? json_object_get_int64(o) : 0;
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
	struct mcp_piece problem =
		secs < 1 || secs > WAIT_MAX
		? TEXT("The seconds argument must be a whole number from 1 to 600.")
		: !listed ? TEXT("The jobs argument must list the numbers of jobs you "
		                 "started.")
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

/* The timer tool's call in @p slot: set a timer to fire in the seconds
 * that @p args gives, with its note. */
static void
mcp_slot_timer (struct mcp_slot    *slot,
                struct json_object *args)
{
	static_assert(TIMER_MAX == 3600 && TIMER_NOTE == 200, "the problem says");
	struct mcp_req *req = slot->req;
	struct json_object *o = jb_get(args, "seconds");
	int64_t secs = json_object_is_type(o, json_type_int) ? json_object_get_int64(o) : 0;
	size_t len = 0;
	struct json_object *note = jb_get(args, "note");
	char const *text = jb_str(note, &len);
	struct mcp_piece problem =
		secs < 1 || secs > TIMER_MAX
		? TEXT("The seconds argument must be a whole number from 1 to 3600.")
		: note && (!text || len > TIMER_NOTE || jb_utf8(text, len) < len)
		? TEXT("The note argument must be a string of at most 200 bytes.")
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
	size_t i = 0;
	while (i < TIMERS_MAX && a->timers[i].due)
		++i;
	if (i == TIMERS_MAX) {
		mcp_slot_result(slot, TEXT("You have 8 timers set, the most at once."), true);
		return;
	}
	struct mcp_timer *t = &a->timers[i];
	t->due = req->mcp->loop->now + (uint64_t)secs * NS;
	t->id = ++a->n_timers;
	t->len = (uint32_t)len;
	memcpy(t->note, text ? text : "", len);
	e = mcp_agenda_arm(a);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	char said[64];
	mcp_slot_result(slot, ESC(said, util_format(said, sizeof said, "Timer %" PRIu32
	                                            " will fire in %" PRId64 " s.", t->id,
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
	if (k == req->n_calls ||
	    (k > req->part && mcp_slot_offer(&req->slots[k - 1])->kind->flags & KF_SERIAL))
		return;
	struct mcp_call c = mcp_call_at(buf_peek(&req->calls).ptr + req->launch_at);
	uint32_t id = mcp_req_tool_of(req, (char const *)c.name.ptr, c.name.len);
	struct mcp_offer const *tool = &mcp->st->offers[id];
	if (tool->kind->flags & KF_SERIAL && k > req->part)
		return;
	if (mcp->draining) {
		mcp_req_fail(req, ESHUTDOWN);
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
	if (!args) {
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

/* The turn's call in @p slot has its output: its window, and the note on
 * it; the request writes its result once those before it are.  A job's
 * says so in the next result. */
static void
mcp_slot_done (struct mcp_slot *slot,
               bool             failed)
{
	if (slot->role == ROLE_JOB) {
		slot->failed = failed;
		slot->done = true;
		mcp_agenda_job_done(slot);
		return;
	}
	struct span out = mcp_slot_output(slot);
	bool past = slot->skip > out.len;
	mcp_slot_window(slot, out);
	slot->note_len = (uint16_t)mcp_slot_note_window(slot, out.len, past);
	slot->failed = failed;
	slot->done = true;
	loop_ready_bulk(slot->req->mcp->loop, &slot->req->step);
}

/* Write the result of the turn's call in @p slot, the next in order: after
 * the turn's last, askai adds what the context holds, if it fills up, and
 * that the next turn is the last, if it is. */
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
		if (req->context && tokens * 4 >= (uint64_t)req->context * 3)
			n += util_format(slot->note + n, size - n, "\n\n[askai: your context is "
			                 "%" PRIu64 "%% full, %" PRIu64 " of %" PRIu32 " tokens. "
			                 "Note what you need to keep, then call compact.]",
			                 tokens * 100 / req->context, tokens, req->context);
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
 * and after the turn's last one the end of the messages. */
static int
mcp_req_result_put (struct mcp_req        *req,
                    struct mcp_call const *c)
{
	struct ai_format const *f = ai_format(req->mcp->st->ep->api);
	struct mcp_slot const *slot = &req->slots[req->part];
	struct span out = mcp_slot_output(slot);
	struct dstr const *told = req->agenda ? &req->agenda->told : nullptr;
	size_t told_len = told ? dstr_len(told) : 0;
	char made[CALL_ID];
	struct mcp_piece id;
	int e = mcp_req_call_id(req, c, req->part, made, &id);
	if (e)
		return e;
	/* A result that names its call gives the id of a call that had one. */
	struct mcp_piece key = id;
	struct mcp_piece by = {};
	struct mcp_piece also = {};
	if (f->flags & AI_RESULT_NAMED) {
		key = ESC(c->name.ptr, c->name.len);
		if (c->id.len) {
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
		BYTES(f->results_end),
		BYTES(f->end),
	};
	bool last = req->part + 1u == req->n_calls;
	return mcp_req_upload(req, p, last ? ARRAY_SIZE(p) : ARRAY_SIZE(p) - 2);
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

/* Submit the next turn, its upload whole; or, draining, end the call. */
static void
mcp_req_turn_submit (struct mcp_req *req)
{
	if (req->mcp->draining) {
		mcp_req_fail(req, ESHUTDOWN);
		return;
	}
	mcp_req_wait(req);
	if (mcp_req_submit(req))
		mcp_req_error(req, 500, INTERNAL_ERROR,
		              FRAG("Cannot start the upstream request"), nullptr, 0);
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

/* The notes' git has ended, its output all in: take it in steps of its
 * slot's own. */
static void
mcp_on_notes (struct child *c)
{
	mcp_slot_ready(c->owner);
}

/* Spawn the git of @p argv, its input @p in, for the notes, for what
 * req->self.fetch says: 0, or why not. */
static int
mcp_slot_notes_spawn (struct mcp_slot *slot,
                      char const      *argv[],
                      struct span      in)
{
	struct mcp_req *req = slot->req;
	struct mcp *mcp = req->mcp;
	struct loop *loop = mcp->loop;
	struct child_cmd const cmd = {
		.file = "git",
		.argv = argv,
		.envp = notes_env(mcp->st->notes),
		.in = in,
		.max = NOTES_MAX,
		.deadline = loop->now + NOTES_NS,
		.flags = CHILD_STDERR
	};
	int e = 0;
	slot->child = child_create(loop, &mcp->children, &cmd, mcp_on_notes, slot, &e);
	if (!slot->child)
		return e;
	mcp_slot_park(slot);
	return 0;
}

/* The notes' git could not be spawned, as @p e says: a compaction goes on
 * without the notes; the notes tool's call, or a thread gone on with, ends
 * with why. */
static void
mcp_slot_notes_failed (struct mcp_slot *slot,
                       int              e)
{
	struct mcp_req *req = slot->req;
	if (slot->fetch == FETCH_COMPACT || slot->fetch == FETCH_FULL) {
		if (e != EBUSY)
			LOG(LOG_WARN, "notes: cannot run git: %s", strerror(e));
		mcp_req_go_on(req, TEXT("\n\n[askai: your notes could not be read.]\n\n"));
		return;
	}
	if (e == EBUSY) {
		mcp_slot_result(slot, TEXT("Too many commands are running; try again in "
		                              "a few seconds."), true);
		return;
	}
	LOG(LOG_WARN, "notes: cannot run git: %s", strerror(e));
	char why[96];
	mcp_slot_result(slot, ESC(why, util_format(why, sizeof why, "Cannot run git: "
	                                              "%s.", strerror(e))), true);
}

/* Read the notes of the request's thread for @p fetch, those that hold
 * @p search if not nullptr. */
static void
mcp_slot_notes_fetch (struct mcp_slot *slot,
                      enum mcp_fetch   fetch,
                      char const      *search)
{
	struct mcp_req *req = slot->req;
	char const *argv[NOTES_ARGV];
	notes_read(req->mcp->st->notes, req->ref, search, argv);
	slot->fetch = (uint8_t)fetch;
	int e = mcp_slot_notes_spawn(slot, argv, (struct span){});
	if (e)
		mcp_slot_notes_failed(slot, e);
}

/* What is wrong with the search argument, for write, which takes none, or
 * for read. */
static char const bad_search[] = "The search argument, which only read takes, "
                                 "must be a line of 1 to 1024 bytes.";

/* The notes tool's write: a note of the text that @p args gives. */
static void
mcp_slot_notes_write (struct mcp_slot    *slot,
                      struct json_object *args)
{
	struct mcp_req *req = slot->req;
	static_assert(NOTES_TEXT == 8 << 10, "the problem says");
	size_t len = 0;
	char const *note = jb_str(jb_get(args, "text"), &len);
	struct mcp_piece problem =
		!note || !len || len > NOTES_TEXT || memchr(note, '\0', len)
		? TEXT("A note must be a string of 1 to 8192 bytes, and no NUL.")
		: jb_get(args, "search") ? TEXT(bad_search)
		: (struct mcp_piece){};
	if (problem.ptr) {
		mcp_slot_result(slot, problem, true);
		return;
	}
	char const *argv[NOTES_ARGV];
	notes_write(req->mcp->st->notes, argv);
	int e = notes_input(&req->body, req->ref, note, len, req->branch);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	slot->fetch = FETCH_WRITE;
	e = mcp_slot_notes_spawn(slot, argv, buf_peek(&req->body));
	buf_fini(&req->body);
	if (e)
		mcp_slot_notes_failed(slot, e);
}

/* The notes tool's read: the notes, those that hold the text that @p args
 * searches for, if any. */
static void
mcp_slot_notes_read (struct mcp_slot    *slot,
                     struct json_object *args)
{
	struct mcp_req *req = slot->req;
	static_assert(NOTES_SEARCH == 1 << 10, "the problem says");
	struct json_object *search = jb_get(args, "search");
	size_t len = 0;
	char const *find = jb_str(search, &len);
	if (search && (!find || !len || len > NOTES_SEARCH || strcspn(find, "\n") < len)) {
		mcp_slot_result(slot, TEXT(bad_search), true);
		return;
	}
	if (!req->branch) {
		mcp_slot_result(slot, TEXT("You have written no notes yet."), false);
		return;
	}
	mcp_slot_notes_fetch(slot, FETCH_READ, find);
}

/* The notes tool's call: write a note, or read them back. */
static void
mcp_slot_notes (struct mcp_slot    *slot,
                struct json_object *args)
{
	struct json_object *action = jb_get(args, "action");
	if (jb_is(action, "write")) {
		mcp_slot_notes_write(slot, args);
		return;
	}
	if (!jb_is(action, "read")) {
		mcp_slot_result(slot, TEXT("The action argument must be write or read."),
		                    true);
		return;
	}
	mcp_slot_notes_read(slot, args);
}

/* Whether the notes' git @p c gave what it was run for: its output cut is
 * the newest notes all the same. */
static bool
mcp_notes_ok (struct child const *c)
{
	return c->cut || (!c->late && c->code == CLD_EXITED && !c->status);
}

/* Whether it failed for want of the thread's branch: exit status 128, with
 * fatal: bad revision. */
static bool
mcp_notes_none (struct child const *c)
{
	return !c->late && c->code == CLD_EXITED && c->status == 128;
}

/* The end of the next note at @p at in @p out, git log's, each ending in a
 * NUL: that NUL, or the output's end. */
static size_t
mcp_note_end (struct span out,
              size_t      at)
{
	unsigned char const *nul = memchr(out.ptr + at, '\0', out.len - at);
	return nul ? (size_t)(nul - out.ptr) : out.len;
}

/* Add the newest notes of @p out, git log's, newest first and each ending
 * in a NUL, that fit the bytes a tool's output may take, at least one, to
 * the request's answer, oldest first.  Only those are looked at: the rest
 * are only counted, @p cut saying that git's output was cut, so that some
 * went uncounted. */
static int
mcp_slot_notes_render (struct mcp_slot *slot,
                       struct span      out,
                       bool             cut)
{
	struct mcp_req *req = slot->req;
	/* What a rendered note adds to its text, and the line before them. */
	enum { EACH = 8, HEAD = 64 };
	size_t budget = mcp_req_lend_max(req);
	struct span shown[NOTES_READ];
	uint32_t n = 0;
	size_t at = 0;
	size_t rendered = HEAD;
	for (; at < out.len && n < NOTES_READ; ++n) {
		size_t end = mcp_note_end(out, at);
		rendered += end - at + EACH;
		if (n && rendered > budget)
			break;
		shown[n] = (struct span){ out.ptr + at, end - at };
		at = end + 1;
	}
	uint32_t more = 0;
	for (; at < out.len; ++more)
		at = mcp_note_end(out, at) + 1;
	loop_work(req->mcp->loop, LOOP_COPY, out.len);
	/* How many older notes are not shown, and some uncounted if cut. */
	if (more) {
		if (!dstr_printf(&slot->answer, "[%" PRIu32 "%s older note%s.]\n\n", more,
		                 cut ? " or more" : "", more > 1 || cut
		                 ? "s are not shown: search for them"
		                 : " is not shown: search for it"))
			return ENOMEM;
	} else if (cut && !CAT(&slot->answer, "[Older notes are not shown: search for "
	                       "them.]\n\n")) {
		return ENOMEM;
	}
	for (uint32_t k = n; k--; ) {
		char const *note = (char const *)shown[k].ptr;
		if (!dstr_printf(&slot->answer, "%" PRIu32 ". ", n - k) ||
		    !dstr_cat_n(&slot->answer, note, jb_utf8(note, shown[k].len)) ||
		    !(k ? CAT(&slot->answer, "\n\n") : CAT(&slot->answer, "\n")))
			return ENOMEM;
	}
	return 0;
}

/* Add @p head, then the notes that the notes' git read, to the request's
 * answer. */
static int
mcp_slot_notes_list (struct mcp_slot  *slot,
                     struct mcp_piece  head)
{
	struct child const *c = slot->child;
	int e = mcp_slot_say(slot, &head, 1);
	return e ? e : mcp_slot_notes_render(slot, buf_peek(&c->out), c->cut);
}

/* Say whether the notes' git wrote the note, as @p ok says, and if not,
 * why. */
static int
mcp_slot_notes_said (struct mcp_slot *slot,
                     bool             ok)
{
	struct child const *c = slot->child;
	if (ok)
		return mcp_slot_say(slot, &TEXT("Noted."), 1);
	if (c->late)
		return mcp_slot_say(slot, &TEXT("Could not write the note: git took too "
		                              "long."), 1);
	struct span out = buf_peek(&c->out);
	char const *why = (char const *)out.ptr;
	struct mcp_piece const p[] = {
		TEXT("Could not write the note: "),
		ESC(why, jb_utf8(why, mcp_clip(why, out.len, 300))),
	};
	return mcp_slot_say(slot, p, ARRAY_SIZE(p));
}

/* The notes' git wrote a note for the notes tool, or failed to. */
static void
mcp_slot_notes_written (struct mcp_slot *slot)
{
	struct mcp_req *req = slot->req;
	bool ok = mcp_notes_ok(slot->child);
	int e = mcp_slot_notes_said(slot, ok);
	req->branch |= ok;
	child_destroy(&slot->child);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	mcp_slot_done(slot, !ok);
}

/* The notes' git read the notes for the notes tool, or failed to: show
 * them, or say why there are none: none holds the text searched for, none
 * was written, or they could not be read, which fails the call. */
static void
mcp_slot_notes_got (struct mcp_slot *slot)
{
	struct mcp_req *req = slot->req;
	struct child *c = slot->child;
	struct span out = buf_peek(&c->out);
	bool failed = false;
	int e;
	if (mcp_notes_ok(c)) {
		e = out.len ? mcp_slot_notes_render(slot, out, c->cut)
		            : mcp_slot_say(slot, &TEXT("No note holds that text."), 1);
	} else if (mcp_notes_none(c)) {
		e = mcp_slot_say(slot, &TEXT("You have written no notes yet."), 1);
	} else {
		failed = true;
		e = mcp_slot_say(slot, &TEXT("Your notes could not be read."), 1);
	}
	child_destroy(&slot->child);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	mcp_slot_done(slot, failed);
}

/* The notes' git read the notes of the thread the call goes on with, which
 * go with the prompt, or failed to, which ends the call. */
static void
mcp_slot_notes_resumed (struct mcp_slot *slot)
{
	struct mcp_req *req = slot->req;
	struct child *c = slot->child;
	if (!mcp_notes_ok(c)) {
		char why[96 + NOTES_ID];
		size_t n = mcp_notes_none(c)
		         ? util_format(why, sizeof why, "Unknown thread: %s.", req->thread)
		         : util_format(why, sizeof why, "Cannot read the notes of the "
		                       "thread %s.", req->thread);
		child_destroy(&slot->child);
		mcp_slot_result(slot, ESC(why, n), true);
		return;
	}
	req->branch = true;
	int e = mcp_slot_notes_list(slot, TEXT("\n\n[askai: the notes you kept in earlier "
	                                     "calls on this thread, oldest first:]\n\n"));
	child_destroy(&slot->child);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	mcp_req_enter(req, PHASE_BUILD);
}

/* The notes' git read the notes for a compaction, which go with the prompt,
 * or failed to. */
static void
mcp_slot_notes_compacted (struct mcp_slot *slot)
{
	struct mcp_req *req = slot->req;
	int e = mcp_notes_ok(slot->child)
	      ? mcp_slot_notes_list(slot, TEXT("\n\n[askai: your notes, oldest first:]\n\n"))
	      : mcp_slot_say(slot, &TEXT("\n\n[askai: your notes could not be read.]\n"), 1);
	child_destroy(&slot->child);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	mcp_req_go_on(req, TEXT("\n"));
}

/* Take what the notes' git said, for what it ran for. */
static void
mcp_slot_notes_out (struct mcp_slot *slot)
{
	mcp_slot_unwait(slot);
	switch (slot->fetch) {
	case FETCH_WRITE:
		mcp_slot_notes_written(slot);
		return;
	case FETCH_READ:
		mcp_slot_notes_got(slot);
		return;
	case FETCH_RESUME:
		mcp_slot_notes_resumed(slot);
		return;
	default:
		mcp_slot_notes_compacted(slot);
	}
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
	if (req->branch) {
		mcp_slot_notes_fetch(&req->self, fetch, nullptr);
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
	static_assert(SUMMARY_MAX == 16 << 10, "the problem says");
	struct mcp_req *req = slot->req;
	size_t len = 0;
	char const *summary = jb_str(jb_get(args, "summary"), &len);
	if (!summary || !len || len > SUMMARY_MAX) {
		mcp_slot_result(slot, TEXT("The summary argument must be a string of 1 to "
		                           "16384 bytes."), true);
		return;
	}
	/* The trace names the call by its tool, the name it called. */
	struct mcp_offer const *o = mcp_slot_offer(slot);
	struct mcp_call const c = { .name = { (unsigned char *)(uintptr_t)o->name, o->len } };
	(void)mcp_req_trace(req, &c, args, o->trace);
	(void)mcp_req_trace_end(req, false, len);
	hist_add(&req->mcp->counts->calls.compacted[0], 1);
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
	req->lent = mcp_req_lent(req);
	mcp_req_ask(req, args);
}

/* Start a tool call: of the tool that the name in @p params names, compared
 * over its length, as the Mcp-Name header is, among those that a role may
 * grant, with the arguments and the progress token that @p params gives;
 * for a client that takes tasks, as @p tasks says, one that becomes a task
 * if it runs TASK_NS. */
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
	uint32_t id = 0;
	while (str && id < st->n_granted &&
	       (st->offers[id].len != len || memcmp(st->offers[id].name, str, len)))
		++id;
	/* One that the client's role may not call is not unknown, but not
	 * available to it. */
	if (!str || id == st->n_granted || !(req->s->tools >> id & 1)) {
		struct mcp_piece head = str && id < st->n_granted
		                      ? FRAG("Tool not available: ") : FRAG("Unknown tool: ");
		struct mcp_piece what = str ? ESC(str, mcp_clip(str, len, 100))
		                            : TEXT("(none)");
		char msg[32 + 6 * 100];
		memcpy(msg, head.ptr, head.len);
		size_t n = jb_escape(msg + head.len, what.ptr, what.len);
		mcp_req_error(req, 200, INVALID_PARAMS, RAW(msg, head.len + n), nullptr, 0);
		return;
	}
	int e = mcp_req_enrol(req);
	if (e) {
		mcp_req_fail(req, e);
		return;
	}
	struct json_object *token = jb_get(jb_get(params, "_meta"), "progressToken");
	req->token = mcp_id_valid(token) ? json_object_get(token) : nullptr;
	if (tasks) {
		req->promote_at = req->mcp->loop->now + TASK_NS;
		mcp_req_arm(req);
	}
	req->self.tool = (uint8_t)id;
	mcp_slot_start(&req->self, jb_get(params, "arguments"));
}

static void
mcp_req_initialize (struct mcp_req     *req,
                    struct json_object *params)
{
	static char const head[] = JSON_FIELD "Mcp-Session-Id: ";
	static char const hex[] = "0123456789abcdef";
	size_t len = 0;
	char const *version = jb_str(jb_get(params, "protocolVersion"), &len);
	if (!mcp_is_legacy(version, len)) {
		version = versions[MODERN];
		len = sizeof versions[MODERN] - 1;
	}

	/* Never stored: the ID only scopes the request IDs a client cancels. */
	unsigned char rnd[16];
	char fields[sizeof head - 1 + 2 * sizeof rnd + 2];
	struct span f = SPAN(JSON_FIELD);
	if (getrandom(rnd, sizeof rnd, 0) == (ssize_t)sizeof rnd) {
		char *id = fields + sizeof head - 1;
		for (size_t i = 0; i < sizeof rnd; ++i) {
			id[2 * i] = hex[rnd[i] >> 4];
			id[2 * i + 1] = hex[rnd[i] & 15];
		}
		memcpy(fields, head, sizeof head - 1);
		memcpy(id + 2 * sizeof rnd, "\r\n", 2);
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
 * a step; then close it. */
static void
mcp_req_show (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	struct ledger_task *task = req->shown;
	size_t left = task->len - req->off;
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
	mcp_req_close(req, 200, SPAN(JSON_FIELD), e ? e : mcp_req_put(req, &FRAG("}"), 1));
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
	int e = mcp_req_open(req, false);
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
 * taskId names, which only its own principal finds; for a client that
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
	struct ledger_task *task = id ? ledger_find(st->ledger, req->s->principal, id, len)
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
	              !mcp_span_is(header, pre_streams, sizeof pre_streams - 1);
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
	if (over)
		req->phase = PHASE_TEXT;
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

/* A keep-alive, if one is due before the reply, a slice of what is left to
 * free, then the phase's work. */
static void
mcp_req_step (struct loop      *loop,
              struct loop_step *step)
{
	struct mcp_req *req = container_of(step, struct mcp_req, step);
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
	switch (req->phase) {
	case PHASE_PARSE:
		mcp_req_parse(req);
		break;
	case PHASE_DISPATCH:
		mcp_req_dispatch(req);
		break;
	case PHASE_BUILD:
		mcp_req_build(req);
		break;
	case PHASE_ANSWER:
		mcp_req_answer(req);
		break;
	case PHASE_TEXT:
		mcp_req_text(req);
		break;
	case PHASE_ARGS:
		mcp_req_turn_args(req);
		break;
	case PHASE_TURN:
		mcp_req_turn_write(req);
		break;
	case PHASE_CALLS:
		mcp_req_calls_next(req);
		break;
	case PHASE_RESULT:
		mcp_req_result_write(req);
		break;
	case PHASE_ADD:
	case PHASE_WAKE:
		mcp_req_add_write(req);
		break;
	case PHASE_SHOW:
		mcp_req_show(req);
		break;
	case PHASE_REPLY:
		mcp_req_reply(req);
		break;
	case PHASE_SEND:
		mcp_req_send(req);
		break;
	case PHASE_METRICS:
		mcp_req_metrics(req);
		break;
	default:
		break;
	}
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
	if (req->call.bits)
		LOG(LOG_VERBOSE, "tool call cancelled after %.1f s", mcp_elapsed(req->self.start));
	req->s = nullptr;
	mcp_req_finish(req);
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

	/* The transport has cancelled every stream by now, but for the
	 * stand-ins of the calls that are tasks, which end here, their tasks
	 * failed.  Then only requests still freeing their message are left,
	 * and what the parts do, such as the man fetches.  The parts go once
	 * what they keep is given back. */
	while (!list_is_empty(&mcp->tasks))
		mcp_req_abort(container_of(mcp->tasks.next, struct mcp_req, task_hook),
		              ESHUTDOWN);
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

/* Returns the definition of the ask tool.  @p service is the mcp_static,
 * which has the tool's configured name and description.  The definition has
 * the argument "thread" if askai has a notes repository (--notes). */
static struct json_object *
mcp_tool_definition (void const *service)
{
	struct mcp_static const *st = service;
	struct json_object *props = jb_object(
		"prompt", jb_object(
			"type", json_object_new_string("string"),
			"description", json_object_new_string(
				"The complete, self-contained question or task. The other AI "
				"cannot see this conversation, so include all the context it "
				"needs."),
			nullptr),
		"system", jb_object(
			"type", json_object_new_string("string"),
			"description", json_object_new_string(
				"Optional system instructions for the other AI, such as a role "
				"or an answer format."),
			nullptr),
		nullptr);
	if (st->notes && jb_set(props, "thread", jb_object(
		    "type", json_object_new_string("string"),
		    "description", json_object_new_string(
			    "Optional: the thread that an earlier answer named, to go on with "
			    "that work. The other AI keeps notes in a thread as it works, and "
			    "starts with them."),
		    nullptr))) {
		json_object_put(props);
		props = nullptr;
	}
	return jb_object(
		"name", json_object_new_string(st->tool->name),
		"title", json_object_new_string("Ask AI"),
		"description", json_object_new_string(st->tool->description),
		"inputSchema", jb_object(
			"type", json_object_new_string("object"),
			"properties", props,
			"required", jb_list(json_object_new_string("prompt")),
			"additionalProperties", json_object_new_boolean(0),
			nullptr),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(1),
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

/* The notes tool's definition, which only turns are given. */
static struct json_object *
mcp_notes_definition (void const *service)
{
	(void)service;
	static_assert(NOTES_TEXT == 8 << 10 && NOTES_READ == 256, "the description says");
	struct json_object *actions = jb_list(json_object_new_string("write"));
	if (jb_push(actions, json_object_new_string("read"))) {
		json_object_put(actions);
		actions = nullptr;
	}
	return jb_object(
		"name", json_object_new_string("notes"),
		"description", json_object_new_string(
			"Your notebook for this task, which outlasts your context: write "
			"down what you find, what you decide, and what is done and left "
			"to do, a short note at a time, as you go. When the context fills "
			"up, the conversation is compacted to the task, a summary and these "
			"notes; read them back when you need them. They are yours: the "
			"client sees that you keep them, not what they say."),
		"inputSchema", jb_object(
			"type", json_object_new_string("object"),
			"properties", jb_object(
				"action", jb_object(
					"type", json_object_new_string("string"),
					"enum", actions,
					"description", json_object_new_string(
						"write a note, or read the newest 256."),
					nullptr),
				"text", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"For write: the note, short and self-contained, at "
						"most 8 KiB."),
					nullptr),
				"search", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"For read: only the notes that hold this text, "
						"regardless of case."),
					nullptr),
				nullptr),
			"required", jb_list(json_object_new_string("action")),
			"additionalProperties", json_object_new_boolean(0),
			nullptr),
		nullptr);
}

/* The compact tool's definition, which only turns are given. */
static struct json_object *
mcp_compact_definition (void const *service)
{
	(void)service;
	static_assert(SUMMARY_MAX == 16 << 10, "the description says");
	return jb_object(
		"name", json_object_new_string("compact"),
		"description", json_object_new_string(
			"Free your context: the conversation since the task, your turns and "
			"the tools' results, is replaced by your summary of it, and your "
			"notes are shown again. Note first what you must keep. Call it when "
			"askai says that the context fills up, or once you are done with a "
			"part of the work whose details you no longer need. Calls after it "
			"in the same turn do not run."),
		"inputSchema", jb_object(
			"type", json_object_new_string("object"),
			"properties", jb_object(
				"summary", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"Where the task stands: what is done, what you found, "
						"and what you will do next; at most 16 KiB."),
					nullptr),
				nullptr),
			"required", jb_list(json_object_new_string("summary")),
			"additionalProperties", json_object_new_boolean(0),
			nullptr),
		nullptr);
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

/* The definition of background, which only turns are given; nullptr if
 * out of memory. */
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
				"arguments", jb_object(
					"type", json_object_new_string("object"),
					"description", json_object_new_string(
						"Its arguments, as you would give them to it."),
					nullptr),
				nullptr),
			jb_list(json_object_new_string("tool"))),
		nullptr);
}

/* The definition of jobs, likewise. */
static struct json_object *
mcp_jobs_definition (void const *service)
{
	(void)service;
	struct json_object *actions = jb_list(json_object_new_string("list"));
	if (jb_push(actions, json_object_new_string("result")) ||
	    jb_push(actions, json_object_new_string("cancel"))) {
		json_object_put(actions);
		actions = nullptr;
	}
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
					"enum", actions,
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

/* The definition of wait, likewise. */
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

/* The definition of timer, likewise. */
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
			"extensions", jb_object(meta_tasks, jb_object(nullptr), nullptr),
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
	return dstr_printf(&st->list_tail, "],\"ttlMs\":%d,\"cacheScope\":\"public\"%s",
	                   CACHE_TTL_MS, dstr_get(&st->modern_tail)) ? 0 : ENOMEM;
}

/* What each kind of tool is: what its calls do, or the plugin whose one
 * tool it is; its name, but the ask tool's, which the configuration names;
 * how the turns are lent it, but a plugin's, which its kind's flags say;
 * its row in the turns' metrics; and what the trace tells of its calls. */
struct mcp_row {
	struct tool_kind        kind;
	struct plugin const    *plugin;
	char const             *ptr;
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
	[KIND_MAN]        = { .plugin = &manual_plugin, N("man"), .metric = LENT_MAN },
	[KIND_TASK]       = { { .definition = mcp_task_definition, .start = mcp_slot_task,
	                        .drop = mcp_slot_task_drop },
	                      N("test_task"), LEND_NEVER, LENT_NONE },
	[KIND_GIT]        = { .plugin = &git_plugin, N("git"), .metric = LENT_GIT },
	[KIND_NOTES]      = { { .definition = mcp_notes_definition, .start = mcp_slot_notes,
	                        .step = mcp_slot_notes_out, .flags = KF_SERIAL },
	                      N("notes"), LEND_ALWAYS, LENT_NOTES, TRACE_ACTION },
	[KIND_COMPACT]    = { { .definition = mcp_compact_definition, .start = mcp_slot_compact,
	                        .flags = KF_SERIAL },
	                      N("compact"), LEND_WITH, LENT_COMPACT, TRACE_NAME },
	[KIND_BACKGROUND] = { { .definition = mcp_background_definition,
	                        .start = mcp_slot_background },
	                      N("background"), LEND_DETACH, LENT_BACKGROUND },
	[KIND_JOBS]       = { { .definition = mcp_jobs_definition, .start = mcp_slot_jobs,
	                        .flags = TOOL_PAGED | KF_SERIAL },
	                      N("jobs"), LEND_DETACH, LENT_JOBS },
	[KIND_WAIT]       = { { .definition = mcp_wait_definition, .start = mcp_slot_wait,
	                        .flags = KF_SERIAL },
	                      N("wait"), LEND_WITH, LENT_WAIT },
	[KIND_TIMER]      = { { .definition = mcp_timer_definition, .start = mcp_slot_timer },
	                      N("timer"), LEND_WITH, LENT_TIMER },
	[KIND_NONE]       = { { .start = mcp_slot_none },
	                      N("an unknown tool"), LEND_NEVER, LENT_NONE },
#undef N
};

/* Serialize the replies that never change, of @p info, what tells of
 * askai, and @p list, an array for the supported versions: each tool's
 * definition, as tools/list lists it if a role may grant it, and as the
 * upstream's API has it if it is lent; then mcp_static_tails()'s. */
static int
mcp_static_build (struct mcp_static  *st,
                  struct json_object *info,
                  struct json_object *list)
{
	for (uint32_t id = 0; id < st->n_offers; ++id) {
		struct mcp_offer *o = &st->offers[id];
		struct json_object *def = o->kind->definition(st->plugins[o->part].service);
		tool_mask bit = (tool_mask)1 << id;
		st->lends |= o->lend == LEND_GRANTED ? bit : 0;
		st->always |= o->lend == LEND_ALWAYS ? bit : 0;
		st->with |= o->lend == LEND_WITH ? bit : 0;
		st->detached |= o->lend == LEND_DETACH ? bit : 0;
		st->detach |= o->kind->flags & TOOL_DETACH ? bit : 0;
		int e = !def ? ENOMEM : id < st->n_granted
		      ? mcp_static_item(&o->listed, json_object_get(def)) : 0;
		if (!e && o->lend != LEND_NEVER)
			e = mcp_static_item(&o->lent, ai_tool(st->ep->api, def));
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

int
mcp_register (struct registry     *reg,
              struct config const *cfg)
{
	bool lends = cfg->flags & CFG_UPSTREAM_TOOLS;
	uint32_t turns = 1u << KIND_COMPACT | 1u << KIND_BACKGROUND | 1u << KIND_JOBS |
	                 1u << KIND_WAIT | 1u << KIND_TIMER;
	uint32_t offered = 1u << KIND_ASK | (cfg->flags & CFG_MAN ? 1u << KIND_MAN : 0) |
	                   (cfg->flags & CFG_TEST_TASK ? 1u << KIND_TASK : 0) |
	                   (cfg->n_gits ? 1u << KIND_GIT : 0) |
	                   (lends && cfg->notes ? 1u << KIND_NOTES : 0) |
	                   (lends ? turns : 0);
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
		                     k < KIND_NOTES ? REGISTRY_GRANTED : 0);
		if (e)
			return e;
	}
	return 0;
}

/* Returns the record of @p plugin for @p st: the plugin, its service and
 * its two part functions.  The service of askai's own kinds is @p st.  The
 * service of the man tool's plugin is @p manual, and the service of the git
 * tool's plugin is @p git.  Both of these have a part on every client loop.
 * A plugin that askai loaded from a file has no service and no part. */
static struct mcp_plugin
mcp_static_plugin (struct mcp_static   *st,
                   struct plugin const *plugin,
                   struct manual       *manual,
                   struct git          *git)
{
	struct mcp_plugin const own[] = {
		{ .plugin = &mcp_host_plugin, .service = st },
		{ &manual_plugin, manual, manual_loop_create, manual_loop_destroy },
		{ &git_plugin, git, git_loop_create, git_loop_destroy },
	};
	for (size_t i = 0; i < ARRAY_SIZE(own); ++i)
		if (own[i].plugin == plugin)
			return own[i];
	return (struct mcp_plugin){ .plugin = plugin };
}

struct mcp_static *
mcp_static_create (struct config const *cfg,
                   struct manual       *manual,
                   struct task_pool    *tasks,
                   struct git          *git,
                   struct notes        *notes,
                   struct ledger       *ledger,
                   struct roster       *roster,
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
		.notes = lends ? notes : nullptr,
		.ledger = ledger,
		.roster = roster,
		.seed = loop_clock(),
		.lim = cfg->mcp,
		.turns = cfg->turns
	};
	for (uint32_t i = 0; i < reg->n_plugins; ++i)
		st->plugins[i] = mcp_static_plugin(st, reg->plugins[i], manual, git);
	/* A zeroed dstr is no empty string (dstr.h). */
	for (uint32_t id = 0; id < reg->n_tools; ++id) {
		struct registry_tool const *t = &reg->tools[id];
		/* mcp_register() gave each of askai's own tools its row. */
		struct mcp_row const *row = t->data ? t->data : &mcp_loaded_row;
		struct mcp_offer *o = &st->offers[id];
		*o = (struct mcp_offer){
			.listed = DSTR_INIT,
			.lent = DSTR_INIT,
			.kind = t->kind,
			.name = t->name,
			.len = t->len,
			.part = (uint8_t)t->part,
			.lend = row->lend,
			.metric = row->metric,
			.trace = row->trace
		};
		/* The kind of a plugin, built in or loaded, is not in a row.  Its
		 * flags decide whether the turns are lent the tool. */
		if (t->kind != &row->kind)
			o->lend = o->kind->flags & TOOL_LENT ? LEND_GRANTED : LEND_NEVER;
		/* The turns are lent tools with --upstream-tools. */
		if (!lends)
			o->lend = LEND_NEVER;
	}
	(void)getrandom(&st->seed, sizeof st->seed, GRND_NONBLOCK);

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
	free(st);
}
