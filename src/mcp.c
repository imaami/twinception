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
 * cancels the call in every era; legacy clients may also cancel it with
 * notifications/cancelled, on any connection of any loop, which home finds
 * the call's job by (src/job.h).
 *
 * A man call, when askai offers the man tool, takes its page from the
 * manual (src/manual.h), which renders each page once and keeps it: at once
 * if the manual has it, else once man has, which the call waits for, named
 * by a handle as for a job's completion.  It replies with the page, or with
 * why there is none.  A call that goes leaves the page to the manual.
 *
 * A git call, when askai lists repositories, runs git (src/git.h) as a
 * child (src/child.h) on the call's loop, and replies with its output, its
 * errors included, made UTF-8 text a share a step; a call that goes kills
 * it.
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
#include "jb.h"
#include "job.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "manual.h"
#include "mcp.h"
#include "metrics.h"
#include "notes.h"
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
	TRACE_MAX    = 48,        /* tool calls the trace tells of */
	TRACE_ARGS   = 200,       /* bytes of a call's arguments it tells */
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

/** Newest first; the first MODERN revisions carry their version in _meta. */
static char const *const versions[] = {
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

struct mcp_static {
	struct ai_endpoint const *ep;
	char const               *tool;
	struct dstr               lent[4];      /**< the tools a turn may call, by
	                                             man and git, bits 0 and 1, that
	                                             the role may call: the request's
	                                             tools; empty: none */
	struct dstr               list[1 << MCP_TOOLS][2];  /**< tools/list results
	                                                       for each set of the
	                                                       tools a role may
	                                                       list: legacy, modern */
	struct dstr               discover;     /**< the server/discover result */
	struct dstr               init_tail;    /**< an initialize result after the
	                                             version */
	struct dstr               modern_tail;  /**< closes a modern result */
	struct dstr               versions;     /**< the supported versions, an array */
	struct manual            *manual;       /**< the man tool's, if askai
	                                             offers it */
	struct task_pool         *tasks;        /**< test_task's, if askai offers
	                                             it */
	struct git               *git;          /**< git's, if askai lists
	                                             repositories */
	struct notes             *notes;        /**< the notes', with --notes */
	uint64_t                  seed;         /**< of the calls' keys' hash */
	struct mcp_limits         lim;
	uint64_t                  turns;        /**< upstream calls an ask call
	                                             makes at most; widened over
	                                             the padding */
};

/** Where a request stands. */
enum mcp_phase {
	PHASE_BODY,      /* taking the body in */
	PHASE_PARSE,     /* parsing it, a slice a step */
	PHASE_DISPATCH,  /* handing the message on, in a step of its own */
	PHASE_BUILD,     /* writing the upstream request, a slice a step */
	PHASE_CALL,      /* waiting for the upstream */
	PHASE_MAN,       /* waiting for a man call's page */
	PHASE_TASK,      /* waiting for a test_task call's task */
	PHASE_GIT,       /* waiting for a git call's git */
	PHASE_GIT_TEXT,  /* taking its output, a slice a step */
	PHASE_ANSWER,    /* parsing its answer, a slice a step */
	PHASE_TEXT,      /* taking the tool's output from that, in a step of its own */
	PHASE_ARGS,      /* parsing a turn's tool calls' arguments, a slice a step */
	PHASE_TURN,      /* writing the turn into the upstream request, likewise */
	PHASE_NOTES,     /* waiting for a git that writes or reads notes */
	PHASE_NOTES_OUT, /* taking a read's notes, in a step of its own */
	PHASE_RESULT,    /* writing a tool call's result into the request */
	PHASE_ADD,       /* writing what askai adds to the prompt into it */
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

/** A request, from its stream's head to its end or cancellation. */
struct mcp_req {
	struct list          hook;       /* on mcp->calls while a tool call runs,
	                                    then on mcp->freeing, then spare */
	struct loop_step     step;       /* the phase's work, and keep-alives */
	struct loop_timer    timer;      /* the next keep-alive */
	struct buf           body;       /* the request body */
	struct buf           upload;     /* the upstream request, until its job
	                                    takes it */
	struct buf           reply;      /* our response body, until sent */
	struct jb_parser     parser;     /* of the body, then of the answer */
	struct jb_reaper     reaper;     /* what is freed a slice a step */
	struct timespec      start;
	struct mcp_splice    splice[2];  /* prompt and system, in draft order */
	struct dstr          answer;     /* the tool's output; its storage stays
	                                    with the request for reuse */
	struct dstr          trace;      /* the tool calls the turns made, for the
	                                    client; likewise */
	struct buf           calls;      /* a turn's tool calls, as its job had
	                                    them, while they run */
	struct stream       *s;          /* nullptr once it is gone */
	struct mcp          *mcp;
	struct job          *job;        /* the tool call's, once submitted, until
	                                    let go of */
	struct manual_page  *page;       /* a man call's, until let go of */
	struct task         *task;       /* a test_task call's, while it waits */
	struct child        *child;      /* a git call's git, until its output is
	                                    taken */
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
	                                          if none or no object */
	char const          *text;       /* the draft serialized */
	size_t               len;
	size_t               off;        /* bytes of the next splice written, of
	                                    the answer escaped, or metrics done */
	size_t               values;     /* of msg */
	size_t               prompt_at;  /* where the prompt's text ends in the
	                                    upload: what askai adds goes there */
	size_t               sent;       /* bytes of the upload as a turn went */
	size_t               at;         /* where the call under way is in calls */
	size_t               skip;       /* the byte of a tool's output that its
	                                    window, given to a turn, starts at */
	size_t               window;     /* and the bytes of that */
	uint64_t             head;       /* loop_clock() at the request's head */
	uint64_t             ping_at;    /* when the next keep-alive is due */
	uint64_t             noted;      /* when the last notification went */
	uint64_t             base;       /* what the progress was as the turn
	                                    began */
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
	uint16_t             splices;
	uint16_t             next;       /* the splice to write next, or piece */
	uint16_t             part;       /* the part of the turn being written,
	                                    or its call under way */
	uint16_t             n_calls;    /* the turn's tool calls */
	uint16_t             bad;        /* bits of those whose arguments are no
	                                    JSON object, one a call */
	uint8_t              phase;      /* enum mcp_phase */
	uint8_t              method;     /* enum metrics_method */
	uint8_t              asks;       /* enum job_ask: JOB_BACKGROUND for
	                                    test_task's prompt */
	uint8_t              lent;       /* 1 + the index in st->lent of the
	                                    tools its turns may call; 0: none */
	uint8_t              tool;       /* enum metrics_lent: the call under
	                                    way's tool */
	uint8_t              fetch;      /* enum mcp_fetch: what the notes git is
	                                    for */
	uint8_t              forced;     /* compactions askai made since a turn
	                                    went through */
	bool                 modern;
	bool                 sse_ok;     /* the client takes an event stream */
	bool                 sse;        /* the response is one */
	bool                 failed;     /* the answer reports an error */
	bool                 tick;       /* a keep-alive or notification may be
	                                    due */
	bool                 progress;   /* the job's progress moved on */
	bool                 nomem;      /* building the reply ran out of memory */
	bool                 blocked;    /* waiting for on_writable() */
	bool                 tooling;    /* a tool runs for a turn */
	bool                 branch;     /* the thread has notes */
	bool                 stopped;    /* the turns ran out with tools called */
	bool                 cut;        /* the turn's answer reached the output
	                                    token limit */
	char                 thread[NOTES_ID + 1];  /* the notes' thread's id */
	char                 ref[NOTES_REF];        /* and its ref */
	char                 note[382];  /* what askai adds to a tool's output,
	                                    sized to end the request unpadded */
};

struct mcp {
	struct list              calls;     /* requests with a tool call under way */
	struct list              freeing;   /* requests freeing their message */
	struct manual_loop       man;       /* the man calls' fetches */
	struct list              children;  /* the git calls' */
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
	uint64_t                 draining;  /* bool, widened over the padding */
};

/* The thread's MCP: the handler of jobs' completions finds it here. */
static thread_local struct mcp *self;

/* Tool output for a call refused for want of room (design section 10). */
static char const busy[] = "Too many tool calls are under way; try again in a "
                           "few seconds.";

/* A run of reply bytes. */
struct mcp_frag {
	char const *ptr;
	size_t      len;
};

#define FRAG(s) ((struct mcp_frag){ s, sizeof s - 1 })
#define TEXT(d) ((struct mcp_frag){ dstr_get(d), dstr_len(d) })

static bool
mcp_span_is (struct span  s,
             char const  *str)
{
	size_t len = strlen(str);
	return s.len == len && !memcmp(s.ptr, str, len);
}

/* Whether field @p f equals @p str, both being absent alike. */
static bool
mcp_same (struct span  f,
          char const  *str)
{
	return !str ? !f.ptr : f.ptr && mcp_span_is(f, str);
}

/* Index into versions[], ARRAY_SIZE(versions) if @p v is unknown or absent. */
static size_t
mcp_version_index (char const *v,
                   size_t      len)
{
	size_t i = 0;
	while (v && i < ARRAY_SIZE(versions) &&
	       (strlen(versions[i]) != len || memcmp(versions[i], v, len)))
		++i;
	return v ? i : ARRAY_SIZE(versions);
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

/* The enum metrics_method of @p method. */
static uint8_t
mcp_method_index (char const *method)
{
	static char const *const names[METHOD_N] = {
		[METHOD_INITIALIZE] = "initialize",
		[METHOD_PING]       = "ping",
		[METHOD_TOOLS_LIST] = "tools/list",
		[METHOD_TOOLS_CALL] = "tools/call",
		[METHOD_DISCOVER]   = "server/discover",
	};
	uint8_t i = METHOD_N;
	while (--i && (!names[i] || strcmp(method, names[i])))
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

static double
mcp_elapsed (struct timespec const *start)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - start->tv_sec) +
	       (double)(now.tv_nsec - start->tv_nsec) / 1e9;
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

/* The bytes a finished request keeps for reuse. */
static uint64_t
mcp_req_kept (struct mcp_req const *req)
{
	return sizeof *req + dstr_get_block_count(&req->answer) * DSTR_BLK_SIZE +
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
			*req = (struct mcp_req){ .answer = DSTR_INIT, .trace = DSTR_INIT };
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
	dstr_cleanup(&req->answer);
	dstr_cleanup(&req->trace);
	free(req);
}

static void mcp_on_job (struct loop *loop, struct msg *msg);

/* A job for a tool call: the one let go of last if there is one; nullptr if
 * out of memory. */
static struct job *
mcp_jobs_take (struct mcp *mcp)
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

/* Keep @p job, whose last reference has gone, for reuse; once the loop has
 * stopped, with no MCP left, free it. */
static void
mcp_jobs_shelve (struct mcp *mcp,
                 struct job *job)
{
	buf_fini(&job->request);
	buf_fini(&job->key);
	if (mcp) {
		shelf_put(&mcp->jobs, &job->spare);
		hist_add(&mcp->counts->kept.jobs, 1);
	} else {
		buf_fini(&job->result);
		buf_fini(&job->calls);
		buf_fini(&job->thought);
		free(job);
	}
}

/* Let go of the request's job, if it has one: cancel it while the request
 * waits for it, freeing the request's handle, so that its completion finds
 * no request; else tell home the result has been read.  A man call lets go
 * of its page, or of its handle while it waits for one, likewise; a
 * test_task call of its task, which it cancels. */
static void
mcp_req_drop (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct job *job = req->job;
	bool waits = req->call.bits;
	manual_page_put(req->page);
	req->page = nullptr;
	if (waits) {
		loop_forget(mcp->loop, req->call);
		req->call = (struct handle){};
	}
	if (req->task) {
		task_drop(req->task);
		req->task = nullptr;
	}
	child_destroy(&req->child);
	if (!job)
		return;
	req->job = nullptr;
	if (waits)
		hist_add(&mcp->counts->calls.cancelled, 1);
	job_post(job, mcp->home, &job->to_home, &job->home_bits, JOB_CANCEL);
	/* A job that waits keeps the front's reference for its completion. */
	if (!waits && job_put(job, 1))
		mcp_jobs_shelve(mcp, job);
}

/* Have the request wait among the calls under way, which a drain ends, if it
 * is not among them yet. */
static void
mcp_req_wait (struct mcp_req *req)
{
	if (!req->hook.next)
		list_append(&req->mcp->calls, &req->hook);
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

/* The reply is queued, or the stream gone: let go of everything at once but
 * the values still to free, and end once those are freed. */
static void
mcp_req_finish (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	if (req->hook.next) {
		list_del(&req->hook);
		req->hook = (struct list){};
	}
	mcp_req_drop(req);
	loop_timer_stop(mcp->loop, &req->timer);
	jb_parser_fini(&req->parser, req->phase == PHASE_ANSWER || req->phase == PHASE_ARGS
	                             ? &mcp->answers : &mcp->pool,
	               &req->reaper);
	mcp_req_dispose(req);
	for (size_t i = 0; i < ARRAY_SIZE(req->args); ++i) {
		if (req->args[i])
			jb_reaper_take(&req->reaper, req->args[i]);
		req->args[i] = nullptr;
	}
	json_object_put(req->draft);
	json_object_put(req->id);
	json_object_put(req->token);
	req->draft = req->id = req->token = req->prompt = nullptr;
	jb_scan_reset(&req->scan);
	buf_fini(&req->body);
	buf_fini(&req->upload);
	buf_fini(&req->reply);
	buf_fini(&req->calls);
	dstr_clear(&req->answer);
	dstr_clear(&req->trace);
	if (!req->reaper.n) {
		mcp_req_end(req);
		return;
	}
	req->phase = PHASE_FREE;
	list_append(&mcp->freeing, &req->hook);
	loop_ready_bulk(mcp->loop, &req->step);
}

/* Answer a request MCP takes no part in with an HTTP error, the stream
 * having no request state. */
static void
mcp_http_error (struct stream *s,
                uint16_t       status,
                char const    *message,
                char const    *fields)
{
	char head[160];
	char body[160];
	int h = snprintf(head, sizeof head, "Content-Type: application/json\r\n%s",
	                 fields);
	int b = snprintf(body, sizeof body, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":"
	                 "-32600,\"message\":\"%s\"}}", message);
	if (!s->ops->head(s, status, (struct span){ (unsigned char *)head, (size_t)h },
	                  (size_t)b))
		(void)s->ops->data(s, (struct span){ (unsigned char *)body, (size_t)b });
	s->ops->end(s, 0);
}

/* The same for a request with state, which goes. */
static void
mcp_req_http_error (struct mcp_req *req,
                    uint16_t        status,
                    char const     *message)
{
	mcp_http_error(req->s, status, message, "");
	req->s = nullptr;
	mcp_req_finish(req);
}

/* Queue @p bytes of the response; false if the stream can carry no more.
 * Once the transport asks to wait, the request waits for on_writable(). */
static bool
mcp_req_write (struct mcp_req *req,
               struct span     bytes)
{
	int e = req->s->ops->data(req->s, bytes);
	req->blocked |= e == EAGAIN;
	return e != EPIPE;
}

/* Send as much of the reply as the step has the share for, the rest a piece
 * a bulk step, each once the transport has room for it; then finish. */
static void
mcp_req_send (struct mcp_req *req)
{
	if (req->blocked)
		return;
	struct span reply = buf_peek(&req->reply);
	size_t room = loop_room(req->mcp->loop, LOOP_SEND);
	size_t n = reply.len < room ? reply.len : room;
	if (n && !mcp_req_write(req, (struct span){ reply.ptr, n })) {
		req->error = EPIPE;
		mcp_req_finish(req);
		return;
	}
	buf_consume(&req->reply, n);
	if (n == reply.len)
		mcp_req_finish(req);
	else if (!req->blocked)
		loop_ready_bulk(req->mcp->loop, &req->step);
}

/* Begin the reply body: on an event stream, an event. */
static void
mcp_req_open (struct mcp_req *req)
{
	buf_consume(&req->reply, buf_peek(&req->reply).len);
	req->nomem = false;
	if (req->sse) {
		struct span s;
		req->nomem = buf_reserve(&req->reply, 6, SIZE_MAX, &s);
		if (!req->nomem) {
			memcpy(s.ptr, "data: ", 6);
			buf_commit(&req->reply, 6);
		}
	}
}

static void
mcp_req_put (struct mcp_req        *req,
             struct mcp_frag const *f,
             size_t                 n)
{
	for (size_t i = 0; i < n && !req->nomem; ++i) {
		struct span s;
		if (!f[i].len)
			continue;
		req->nomem = buf_reserve(&req->reply, f[i].len, SIZE_MAX, &s);
		if (!req->nomem) {
			memcpy(s.ptr, f[i].ptr, f[i].len);
			buf_commit(&req->reply, f[i].len);
		}
	}
}

/* Escape up to @p len bytes of @p src into the reply. */
static void
mcp_req_escape (struct mcp_req *req,
                char const     *src,
                size_t          len)
{
	struct span s;
	if (req->nomem || !len)
		return;
	req->nomem = buf_reserve(&req->reply, 6 * len, SIZE_MAX, &s);
	if (!req->nomem)
		buf_commit(&req->reply, jb_escape((char *)s.ptr, src, len));
}

/* The reply's start: '{"jsonrpc":"2.0","id":ID,"result":', or "error". */
static void
mcp_req_envelope (struct mcp_req *req,
                  bool            error)
{
	size_t len = 0;
	char const *id = req->id ? json_object_to_json_string_length(req->id, JB_WIRE,
	                                                             &len)
	                         : nullptr;
	struct mcp_frag const f[] = {
		FRAG("{\"jsonrpc\":\"2.0\","),
		id ? FRAG("\"id\":") : FRAG(""),
		{ id, len },
		id ? FRAG(",") : FRAG(""),
		error ? FRAG("\"error\":") : FRAG("\"result\":"),
	};
	req->nomem |= req->id && !id;
	mcp_req_put(req, f, ARRAY_SIZE(f));
}

/* Send the reply, as JSON with @p status or as the stream's last event, then
 * finish the request. */
static void
mcp_req_close (struct mcp_req *req,
               uint16_t        status,
               char const     *session)
{
	struct stream *s = req->s;
	if (req->sse)
		mcp_req_put(req, &FRAG("\n\n"), 1);
	req->error = req->nomem ? ENOMEM : 0;
	if (!req->error && !req->sse) {
		char fields[128];
		int n = snprintf(fields, sizeof fields, "Content-Type: application/json\r\n"
		                 "%s%s%s", session ? "Mcp-Session-Id: " : "",
		                 session ? session : "", session ? "\r\n" : "");
		req->error = s->ops->head(s, status, (struct span){ (unsigned char *)fields,
		                                                    (size_t)n },
		                          buf_peek(&req->reply).len);
	}
	if (req->error) {
		mcp_req_finish(req);
		return;
	}
	req->phase = PHASE_SEND;
	mcp_req_send(req);
}

/* A JSON-RPC error; @p message must be JSON string content already. */
static void
mcp_req_error (struct mcp_req        *req,
               uint16_t               status,
               int                    code,
               char const            *message,
               struct mcp_frag const *data,
               size_t                 n_data)
{
	char num[16];
	int n = snprintf(num, sizeof num, "%d", code);
	mcp_req_open(req);
	mcp_req_envelope(req, true);
	struct mcp_frag const f[] = {
		FRAG("{\"code\":"),
		{ num, (size_t)n },
		FRAG(",\"message\":\""),
		{ message, strlen(message) },
		FRAG("\""),
	};
	mcp_req_put(req, f, ARRAY_SIZE(f));
	if (n_data) {
		mcp_req_put(req, &FRAG(",\"data\":"), 1);
		mcp_req_put(req, data, n_data);
	}
	mcp_req_put(req, &FRAG("}}"), 1);
	mcp_req_close(req, status, nullptr);
}

/* A result made of @p n fragments. */
static void
mcp_req_result (struct mcp_req        *req,
                struct mcp_frag const *result,
                size_t                 n,
                char const            *session)
{
	mcp_req_open(req);
	mcp_req_envelope(req, false);
	mcp_req_put(req, result, n);
	mcp_req_put(req, &FRAG("}"), 1);
	mcp_req_close(req, 200, session);
}

/* Begin a tool result: its text follows, escaped. */
static void
mcp_req_tool_open (struct mcp_req *req)
{
	mcp_req_open(req);
	mcp_req_envelope(req, false);
	mcp_req_put(req, &FRAG("{\"content\":[{\"type\":\"text\",\"text\":\""), 1);
}

/* End a tool result: after its text, for an ask call whose turns called
 * tools, or kept notes, what they called and where the notes are, as a
 * text of its own. */
static void
mcp_req_tool_close (struct mcp_req *req)
{
	struct mcp_static const *st = req->mcp->st;
	mcp_req_put(req, &FRAG("\"}"), 1);
	if (dstr_len(&req->trace) || req->branch || req->stopped) {
		char head[160];
		char tail[256];
		int h = req->traced
		      ? snprintf(head, sizeof head, "The other AI made %" PRIu32 " tool "
		                 "call%s in %" PRIu32 " turn%s:\n", req->traced,
		                 req->traced > 1 ? "s" : "", req->turn,
		                 req->turn > 1 ? "s" : "")
		      : snprintf(head, sizeof head, "The other AI called no tools.\n");
		int t = 0;
		if (req->traced > TRACE_MAX)
			t = snprintf(tail, sizeof tail, "- and %" PRIu32 " more\n",
			             req->traced - TRACE_MAX);
		if (req->stopped)
			t += snprintf(tail + t, sizeof tail - (size_t)t, "It was stopped after "
			              "%" PRIu32 " turns, still calling tools.\n", req->turn);
		if (req->branch)
			t += snprintf(tail + t, sizeof tail - (size_t)t, "It kept notes in the "
			              "thread %s: to go on with this work, give that as the "
			              "thread argument.", req->thread);
		mcp_req_put(req, &FRAG(",{\"type\":\"text\",\"text\":\""), 1);
		mcp_req_escape(req, head, (size_t)h);
		mcp_req_escape(req, dstr_get(&req->trace), dstr_len(&req->trace));
		mcp_req_escape(req, tail, (size_t)t);
		mcp_req_put(req, &FRAG("\"}"), 1);
	}
	struct mcp_frag const f[] = {
		FRAG("]"),
		req->failed ? FRAG(",\"isError\":true") : FRAG(""),
		req->modern ? TEXT(&st->modern_tail) : FRAG("}"),
		FRAG("}"),
	};
	mcp_req_put(req, f, ARRAY_SIZE(f));
	mcp_req_close(req, 200, nullptr);
}

static void mcp_req_tool_done (struct mcp_req *req, bool ok, bool failed);

/* A tool result of @p text, short and ours; for a turn's tool call, its
 * output, which goes to the turn. */
static void
mcp_req_tool_result (struct mcp_req *req,
                     char const     *text,
                     bool            failed)
{
	if (req->tooling) {
		dstr_clear(&req->answer);
		mcp_req_tool_done(req, dstr_cat(&req->answer, text), failed);
		return;
	}
	req->failed = failed;
	mcp_req_tool_open(req);
	mcp_req_escape(req, text, strlen(text));
	mcp_req_tool_close(req);
}

/* The tool's output: a streamed answer's text, in the job's result while
 * the request holds the job, a man page while it holds the page, else what
 * the request read or built. */
static struct span
mcp_req_output (struct mcp_req const *req)
{
	return req->job ? buf_peek(&req->job->result)
	     : req->page ? (struct span){ (unsigned char *)req->page->text,
	                                  req->page->len }
	     : (struct span){ (unsigned char *)(uintptr_t)dstr_get(&req->answer),
	                      dstr_len(&req->answer) };
}

/* Escape as much of the output into the reply as the step has the share
 * for; the rest goes on in bulk steps.  Then let go of the job, or the
 * page, if the output was its. */
static void
mcp_req_reply (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	struct span text = mcp_req_output(req);
	size_t len = text.len - req->off;
	size_t room = loop_room(loop, LOOP_ESCAPE);
	size_t n = len < room ? len : room;
	mcp_req_escape(req, (char const *)text.ptr + req->off, n);
	loop_work(loop, LOOP_ESCAPE, n);
	req->off += n;
	if (n < len && !req->nomem) {
		loop_ready_bulk(loop, &req->step);
		return;
	}
	mcp_req_drop(req);
	mcp_req_tool_close(req);
}

/* Reply with the tool's output, unless building that ran out of memory
 * (@p ok false); it goes out a slice at a time. */
static void
mcp_req_respond (struct mcp_req *req,
                 bool            ok,
                 bool            failed)
{
	if (req->tooling) {
		mcp_req_tool_done(req, ok, failed);
		return;
	}
	struct span text = mcp_req_output(req);
	char const *str = (char const *)text.ptr;
	if (ok && failed)
		LOG(LOG_WARN, "tool call failed after %.1f s: %.*s", mcp_elapsed(&req->start),
		    (int)mcp_clip(str, text.len, 300), str);
	else if (ok)
		LOG(LOG_VERBOSE, "tool call answered after %.1f s (%zu bytes)",
		    mcp_elapsed(&req->start), text.len);
	if (!ok) {
		mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return;
	}

	loop_timer_stop(req->mcp->loop, &req->timer);
	/* From the offset that a man or git call asked for, a character's
	 * start; an ask call's answer whole. */
	size_t off = req->lent ? 0 : req->skip;
	if (off > text.len) {
		char why[96];
		snprintf(why, sizeof why, "The output has %zu bytes, fewer than the offset.",
		         text.len);
		mcp_req_tool_result(req, why, true);
		return;
	}
	while (off < text.len && (text.ptr[off] & 0xc0) == 0x80)
		++off;
	req->failed = failed;
	req->phase = PHASE_REPLY;
	req->off = off;
	mcp_req_tool_open(req);
	mcp_req_reply(req);
}

/* Write a notification of progress @p value, saying @p text, which needs
 * no escaping, on the call's event stream. */
static void
mcp_req_notify (struct mcp_req *req,
                uint64_t        value,
                char const     *text)
{
	char num[24];
	int n = snprintf(num, sizeof num, "%" PRIu64, value);
	size_t len = 0;
	char const *token = json_object_to_json_string_length(req->token, JB_WIRE, &len);
	struct mcp_frag const f[] = {
		FRAG("data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\","
		     "\"params\":{\"progressToken\":"),
		{ token, len },
		FRAG(",\"progress\":"),
		{ num, (size_t)n },
		FRAG(",\"message\":\""),
		{ text, strlen(text) },
		FRAG("\"}}\n\n"),
	};
	req->nomem = !token;
	mcp_req_put(req, f, ARRAY_SIZE(f));
	if (!req->nomem)
		(void)mcp_req_write(req, buf_peek(&req->reply));
	buf_consume(&req->reply, buf_peek(&req->reply).len);
	req->nomem = false;
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
	uint64_t value = req->base + (place ? req->place - place
	                                    : (uint64_t)req->place + tokens);
	req->progress = false;
	if (value < req->next_note)
		return;
	char text[64];
	int m = req->lent ? snprintf(text, sizeof text, "turn %" PRIu32 ", ", req->turn)
	                  : 0;
	if (place)
		snprintf(text + m, sizeof text - (size_t)m, "queued, position %" PRIu32,
		         place);
	else
		snprintf(text + m, sizeof text - (size_t)m, "%s, %" PRIu32 " tokens",
		         progress & JOB_THINKING ? "thinking" : "generating", tokens);
	mcp_req_notify(req, value, text);
}

/* Notify the client that a turn calls @p tool, a tool askai names, unless
 * the call has no progressToken, or output the client has yet to read holds
 * the notification back. */
static void
mcp_req_notify_tool (struct mcp_req *req,
                     char const     *tool)
{
	char text[64];
	snprintf(text, sizeof text, "turn %" PRIu32 ", calling %s", req->turn, tool);
	if (req->token && req->sse && !req->blocked)
		mcp_req_notify(req, req->next_note, text);
}

/* While a call waits: a keep-alive, if one is due, and a notification of its
 * progress, if that moved on, the last went PROGRESS_NS ago and the client
 * has read what it was sent; then the timer for the next of them. */
static void
mcp_req_tick (struct mcp_req *req)
{
	struct loop *loop = req->mcp->loop;
	uint32_t keepalive = req->mcp->st->lim.keepalive;
	if (keepalive && req->ping_at <= loop->now) {
		if (!req->blocked)
			(void)mcp_req_write(req, (struct span){ (unsigned char *)":\n\n", 3 });
		req->ping_at = loop->now + keepalive * NS;
	}
	req->progress &= req->phase == PHASE_CALL;
	if (req->progress && req->noted + PROGRESS_NS <= loop->now && !req->blocked)
		mcp_req_progress(req);
	/* Held back, it waits for on_writable(). */
	uint64_t next = req->progress && !req->blocked ? req->noted + PROGRESS_NS
	                                               : UINT64_MAX;
	(void)loop_timer_set(loop, &req->timer, next < req->ping_at ? next : req->ping_at);
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

/* Whether @p why, a failed turn's reason, says that the request did not
 * fit the upstream's context: llama-server's "exceeds the available
 * context size", OpenAI's "maximum context length", others' "context
 * window". */
static bool
mcp_overflowed (char const *why,
                size_t      len)
{
#define W(s) { s, sizeof s - 1 }
	static char const word[] = "context ";
	static struct {
		char const *s;
		size_t      n;
	} const what[] = { W("size"), W("length"), W("window") };
#undef W
	for (size_t i = 0; i + sizeof word - 1 <= len; ++i) {
		if (strncasecmp(why + i, word, sizeof word - 1))
			continue;
		char const *rest = why + i + sizeof word - 1;
		size_t left = len - i - (sizeof word - 1);
		for (size_t k = 0; k < ARRAY_SIZE(what); ++k)
			if (left >= what[k].n && !strncasecmp(rest, what[k].s, what[k].n))
				return true;
	}
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
	return req->lent && req->turn > 1 && !req->forced && mcp_overflowed(why, len);
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
	struct mcp *mcp = loop->exit ? nullptr : self;
	if (bits & JOB_RETURN) {
		mcp_jobs_shelve(mcp, job);
		return;
	}
	if (!(bits & JOB_DONE)) {
		struct mcp_req *req = mcp ? loop_lookup(loop, job->call) : nullptr;
		if (req)
			mcp_req_progressed(req);
		if (job_put(job, 1))
			mcp_jobs_shelve(mcp, job);
		return;
	}
	/* Home is done with the upstream request and the key: a request
	 * whose turns go on keeps the request, its conversation. */
	struct mcp_req *req = mcp ? loop_lookup(loop, job->call) : nullptr;
	if (req && req->lent) {
		req->upload = job->request;
		job->request = (struct buf){};
	}
	buf_fini(&job->request);
	buf_fini(&job->key);
	if (!req) {
		if (mcp)
			hist_add(&mcp->counts->calls.dropped, 1);
		if (job_put(job, 2))
			mcp_jobs_shelve(mcp, job);
		return;
	}
	/* The message's reference; the request keeps the front's. */
	(void)job_put(job, 1);
	loop_forget(loop, req->call);
	req->call = (struct handle){};
	list_del(&req->hook);
	req->hook = (struct list){};
	enum job_status status = job->status;
	if (status == JOB_TEXT && req->lent) {
		mcp_req_turn_done(req);
		return;
	}
	/* A failed call's result says why, unless it never started. */
	struct span why = buf_peek(&job->result);
	if (status == JOB_FAILED && mcp_req_refit(req, (char const *)why.ptr, why.len)) {
		mcp_req_drop(req);
		mcp_req_compact_full(req);
		return;
	}
	if (status == JOB_ANSWERED || status == JOB_TEXT) {
		req->phase = status == JOB_TEXT ? PHASE_TEXT : PHASE_ANSWER;
		loop_ready_bulk(loop, &req->step);
		return;
	}
	if (status == JOB_WITHDRAWN) {
		/* Its client gave up on it: the stream ends quietly. */
		if (!req->sse)
			(void)req->s->ops->head(req->s, 202, (struct span){}, 0);
		LOG(LOG_VERBOSE, "tool call cancelled after %.1f s", mcp_elapsed(&req->start));
		mcp_req_finish(req);
		return;
	}
	struct dstr const *text =
		status == JOB_BUSY ? dstr_cat_n(&req->answer, busy, sizeof busy - 1)
		: why.len ? dstr_cat_n(&req->answer, (char const *)why.ptr, why.len)
		: nullptr;
	mcp_req_drop(req);
	if (status == JOB_CANCELLED)
		mcp_req_error(req, 503, INTERNAL_ERROR, "Server is shutting down", nullptr, 0);
	else if (status == JOB_FAILED && !why.len)
		mcp_req_error(req, 500, INTERNAL_ERROR, "Cannot start the upstream request",
		              nullptr, 0);
	else
		mcp_req_respond(req, text, true);
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
 * or to its end; false if out of memory. */
static bool
mcp_req_copy (struct mcp_req *req,
              size_t          k)
{
	size_t from = k ? req->splice[k - 1].at : 0;
	size_t len = (k < req->splices ? req->splice[k].at : req->len) - from;
	struct span s;
	if (!len)
		return true;
	if (buf_reserve(&req->upload, len, SIZE_MAX, &s))
		return false;
	memcpy(s.ptr, req->text + from, len);
	buf_commit(&req->upload, len);
	return true;
}

/* Write the key a legacy notifications/cancelled names a call by into
 * @p job: the session field, if present, and a line break, which no field
 * holds, then the JSON of the request id @p id; and its hash, seeded so
 * that no client can pick keys that crowd one of home's buckets.  False if
 * out of memory. */
static bool
mcp_key (struct job         *job,
         uint64_t            seed,
         struct span         session,
         struct json_object *id)
{
	size_t len = 0;
	char const *json = json_object_to_json_string_length(id, JB_WIRE, &len);
	size_t head = session.ptr ? session.len + 1 : 0;
	struct span s;
	if (!json || buf_reserve(&job->key, head + len, SIZE_MAX, &s))
		return false;
	if (head) {
		memcpy(s.ptr, session.ptr, session.len);
		s.ptr[session.len] = '\n';
	}
	memcpy(s.ptr + head, json, len);
	buf_commit(&job->key, head + len);
	job->hash = util_hash(seed, s.ptr, head + len);
	job->session = (uint32_t)head;
	return true;
}

/* Hand the upstream request to a job, which the request names itself to for
 * the job's completion, with the call's key and principal: its client's
 * identity, else its session, else its client's address; and submit the
 * job to home.  An identity's jobs are one principal's, whatever their
 * sessions and addresses: home tells them by their hash and a peer no
 * client has (UINT64_MAX twice; a Unix socket's user's is UINT64_MAX and
 * its uid). */
static void
mcp_req_submit (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct loop *loop = mcp->loop;
	struct stream *s = req->s;
	uint64_t seed = mcp->st->seed;
	struct job *job = mcp_jobs_take(mcp);
	if (!job || !mcp_key(job, seed, s->ops->field(s, F_SESSION), req->id) ||
	    loop_handle(loop, req, &req->call)) {
		if (job)
			mcp_jobs_shelve(mcp, job);
		mcp_req_error(req, 500, INTERNAL_ERROR, "Cannot start the upstream request",
		              nullptr, 0);
		return;
	}
	long ms = mcp->st->ep->timeout_ms;
	job->request = req->upload;
	req->upload = (struct buf){};
	job->call = req->call;
	job->deadline = ms > 0 ? loop->now + (uint64_t)ms * 1'000'000 : 0;
	job->peer[0] = s->peer[0];
	job->peer[1] = s->peer[1];
	job->principal = s->principal ? s->principal
	               : job->session ? util_hash(seed, buf_peek(&job->key).ptr,
	                                          job->session)
	               : util_hash(~seed, job->peer, sizeof job->peer);
	if (s->principal) {
		job->session = 0;
		job->peer[0] = job->peer[1] = UINT64_MAX;
	}
	job->asks = (uint8_t)(req->asks | (req->token && req->sse ? JOB_NOTIFY : 0) |
	                      (req->lent ? JOB_TOOLS : 0) |
	                      (req->lent && req->turn ? JOB_AGAIN : 0));
	req->sent = buf_peek(&job->request).len;
	req->base = req->next_note;
	req->place = 0;
	++req->turn;
	if (req->lent)
		hist_add(&mcp->counts->calls.turns, 1);
	/* The front's and home's, and the post adds the message's. */
	atomic_store_explicit(&job->refs, 2, memory_order_relaxed);
	req->job = job;
	req->phase = PHASE_CALL;
	job_post(job, mcp->home, &job->to_home, &job->home_bits, JOB_SUBMIT);
}

static void mcp_req_turn_ready (struct mcp_req *req);

/* Escape as much of a client's string into the upstream request as the step
 * has the share for, with the draft that follows it; once all is written,
 * submit it, after what askai adds to the prompt if anything. */
static void
mcp_req_build (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct loop *loop = mcp->loop;
	bool ok = true;
	if (req->next < req->splices) {
		struct json_object *str = req->splice[req->next].str;
		size_t len = (size_t)json_object_get_string_len(str) - req->off;
		size_t room = loop_room(loop, LOOP_ESCAPE);
		size_t n = len < room ? len : room;
		struct span s = {};
		ok = !n || !buf_reserve(&req->upload, 6 * n, SIZE_MAX, &s);
		if (n && ok) {
			buf_commit(&req->upload, jb_escape((char *)s.ptr,
			                                   json_object_get_string(str) +
			                                   req->off, n));
			loop_work(loop, LOOP_ESCAPE, n);
			req->off += n;
		}
		if (ok && n == len) {
			if (str == req->prompt)
				req->prompt_at = buf_peek(&req->upload).len;
			req->off = 0;
			ok = mcp_req_copy(req, ++req->next);
		}
	}
	if (ok && req->next < req->splices) {
		loop_ready_bulk(loop, &req->step);
		return;
	}

	json_object_put(req->draft);
	req->draft = req->prompt = nullptr;
	req->next = 0;
	if (ok)
		mcp_req_turn_ready(req);
	else
		mcp_req_error(req, 500, INTERNAL_ERROR, "Cannot start the upstream request",
		              nullptr, 0);
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
	req->sse = !s->ops->head(s, 200, (struct span){ (unsigned char *)fields,
	                                                sizeof fields - 1 }, STREAM_OPEN);
	uint32_t keepalive = mcp->st->lim.keepalive;
	req->ping_at = keepalive ? mcp->loop->now + keepalive * NS : UINT64_MAX;
	if (req->sse)
		(void)loop_timer_set(mcp->loop, &req->timer, req->ping_at);
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

/* The page a man call waited for is in, or man failed: reply with the page,
 * or why there is none, unless the call has gone.  Once the loop has
 * stopped, MCP is gone, and the post is only freed. */
static void
mcp_on_page (struct loop *loop,
             struct msg  *msg)
{
	struct manual_wait *w = container_of(msg, struct manual_wait, msg);
	struct mcp_req *req = loop->exit ? nullptr : loop_lookup(loop, w->who);
	if (req) {
		loop_forget(loop, req->call);
		req->call = (struct handle){};
		list_del(&req->hook);
		req->hook = (struct list){};
		req->page = w->page;
		w->page = nullptr;
		if (req->page) {
			mcp_req_respond(req, true, false);
		} else {
			LOG(LOG_VERBOSE, "man call failed after %.1f s: %s",
			    mcp_elapsed(&req->start), w->why);
			mcp_req_tool_result(req, w->why, true);
		}
	}
	manual_wait_destroy(&w);
}

/* Take the byte of a tool's output that @p args asks it to start from, its
 * offset, into req->skip; what is wrong with it, or nullptr. */
static char const *
mcp_req_offset (struct mcp_req     *req,
                struct json_object *args)
{
	struct json_object *o = jb_get(args, "offset");
	int64_t n = json_object_is_type(o, json_type_int) ? json_object_get_int64(o) : -1;
	req->skip = o && n > 0 ? (size_t)n : 0;
	return o && n < 0 ? "The offset argument must be a whole number of bytes, from 0."
	                  : nullptr;
}

/* Whether @p s, of @p len bytes, is 1 to @p max of the characters of @p set,
 * the first not '-', which man would take for an option. */
static bool
mcp_man_word (char const *s,
              size_t      len,
              size_t      max,
              char const *set)
{
	return s && len && len <= max && *s != '-' && strspn(s, set) == len;
}

/* Start a man call for the page that @p args names, of the section it
 * names if any: reply with it if the manual has it, else wait for it; false
 * if the request is finished instead. */
static bool
mcp_req_man (struct mcp_req     *req,
             struct json_object *args)
{
#define ALNUM "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
	struct mcp *mcp = req->mcp;
	struct loop *loop = mcp->loop;
	struct json_object *name = jb_get(args, "name");
	struct json_object *section = jb_get(args, "section");
	char const *page = jb_text(name);
	size_t page_len = page ? (size_t)json_object_get_string_len(name) : 0;
	/* A small model may give a section as a number. */
	int64_t number = json_object_is_type(section, json_type_int)
	               ? json_object_get_int64(section) : -1;
	char digit[2] = { number >= 0 && number <= 9 ? (char)('0' + number) : 0 };
	char const *sect = *digit ? digit : jb_text(section);
	size_t len = *digit ? 1 : sect ? (size_t)json_object_get_string_len(section) : 0;
	char const *problem =
		args && !json_object_is_type(args, json_type_object)
		? "Tool arguments must be an object."
		: !mcp_man_word(page, page_len, MANUAL_NAME, ALNUM "_.:+@-")
		? "The name argument must name a manual page: 1 to 128 letters, digits "
		  "and _.:+@- characters, the first not -."
		: section && !(sect && (!len || mcp_man_word(sect, len, MANUAL_SECTION, ALNUM)))
		? "The section argument must name a section, such as 1, 3 or 3p: 1 to "
		  "16 letters and digits."
		: mcp_req_offset(req, args);
#undef ALNUM
	static_assert(MANUAL_NAME == 128 && MANUAL_SECTION == 16, "the problems say");
	if (problem) {
		mcp_req_tool_result(req, problem, true);
		return false;
	}

	if (!req->tooling)
		clock_gettime(CLOCK_MONOTONIC, &req->start);
	struct manual_page *found = nullptr;
	int e = loop_handle(loop, req, &req->call);
	if (!e)
		e = manual_find(&mcp->man, page, page_len, sect, len, req->call, mcp_on_page,
		                &found);
	if (e || found) {
		loop_forget(loop, req->call);
		req->call = (struct handle){};
	}
	if (e) {
		mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return false;
	}
	if (found) {
		req->page = found;
		mcp_req_respond(req, true, false);
		return false;
	}
	mcp_req_wait(req);
	req->s->ops->waiting(req->s);
	req->phase = PHASE_MAN;
	return true;
}

static bool mcp_req_notes_fetch (struct mcp_req *req, enum mcp_fetch fetch,
                                 char const *search);

/* Serialize the tools a turn may call: the text @p obj's userdata points
 * at, as it is. */
static int
mcp_lent_text (struct json_object *obj,
               struct printbuf    *pb,
               int                 level,
               int                 flags)
{
	(void)level;
	(void)flags;
	struct dstr const *text = json_object_get_userdata(obj);
	return printbuf_memappend(pb, dstr_get(text), (int)dstr_len(text)) < 0 ? -1 : 0;
}

/* Start a call that asks the upstream, of the class the request asks for,
 * lending the turns the tools req->lent says, if any: build its upstream
 * request, then submit its job; false if the request is finished
 * instead. */
static bool
mcp_req_ask (struct mcp_req     *req,
             struct json_object *params)
{
	struct mcp *mcp = req->mcp;
	struct mcp_static const *st = mcp->st;
	/* Argument problems go back to the model so it can correct itself. */
	struct json_object *args = jb_get(params, "arguments");
	struct json_object *prompt = jb_get(args, "prompt");
	struct json_object *system = jb_get(args, "system");
	struct json_object *thread = jb_get(args, "thread");
	char const *id = jb_text(thread);
	bool notes = req->lent && st->notes;
	char const *problem =
		args && !json_object_is_type(args, json_type_object)
		? "Tool arguments must be an object."
		: !jb_text(prompt) || !*jb_text(prompt)
		? "The prompt argument must be a non-empty string."
		: system && !jb_text(system) ? "The system argument must be a string."
		: thread && !notes
		? "No notes are kept for you here, so there is no thread to go on with."
		: thread && !(id && notes_is_id(id, (size_t)json_object_get_string_len(thread)))
		? "The thread argument must be a thread's id, as an earlier answer gave it: "
		  "32 hexadecimal digits."
		: nullptr;
	if (problem) {
		mcp_req_tool_result(req, problem, true);
		return false;
	}

	clock_gettime(CLOCK_MONOTONIC, &req->start);
	mcp_req_wait(req);
	struct json_object *token = jb_get(jb_get(params, "_meta"), "progressToken");
	req->token = mcp_id_valid(token) ? json_object_get(token) : nullptr;
	if (notes && id) {
		memcpy(req->thread, id, sizeof req->thread);
	} else if (notes && notes_id(req->thread)) {
		mcp_req_error(req, 500, INTERNAL_ERROR, "Cannot make a thread's id", nullptr, 0);
		return false;
	}
	if (notes)
		notes_ref(req->s->principal, req->thread, req->ref);

	/* The client's strings, which may be long, are left out of the draft
	 * request, and mcp_req_build() escapes them in; a default system prompt
	 * is ours, and goes in with the draft. */
	char const *sys = system ? jb_text(system) : st->ep->system;
	struct json_object *instructions = !sys || !*sys ? nullptr
	                                 : system ? json_object_get(system)
	                                 : json_object_new_string(sys);
	json_object_set_serializer(prompt, mcp_splice_mark, req, nullptr);
	if (system)
		json_object_set_serializer(system, mcp_splice_mark, req, nullptr);
	/* The tools the turns may call, serialized once for every loop. */
	struct json_object *tools = req->lent ? json_object_new_array() : nullptr;
	if (tools)
		json_object_set_serializer(tools, mcp_lent_text,
		                           (void *)(uintptr_t)&st->lent[req->lent - 1],
		                           nullptr);
	if ((!sys || !*sys || instructions) && (!req->lent || tools))
		req->draft = ai_request(st->ep, instructions, prompt, tools);
	json_object_put(instructions);
	json_object_put(tools);
	req->prompt = prompt;
	req->text = req->draft ? json_object_to_json_string_length(req->draft, JB_WIRE,
	                                                           &req->len)
	                       : nullptr;
	if (!req->text || !mcp_req_copy(req, 0)) {
		mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return false;
	}
	req->s->ops->waiting(req->s);
	if (req->sse_ok)
		mcp_req_stream(req);
	/* Going on with a thread, its notes go with the prompt. */
	if (thread)
		return mcp_req_notes_fetch(req, FETCH_RESUME, nullptr);
	req->phase = PHASE_BUILD;
	loop_ready_bulk(mcp->loop, &req->step);
	return true;
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
	struct mcp_req *req = loop->exit ? nullptr : loop_lookup(loop, s->call);
	if (!req) {
		task_put(task, 1);
		return;
	}
	loop_forget(loop, req->call);
	req->call = (struct handle){};
	list_del(&req->hook);
	req->hook = (struct list){};
	req->task = nullptr;
	char text[96];
	snprintf(text, sizeof text, "Ran %" PRIu64 " slices, and moved %" PRIu64
	         " times between loops.", s->slices, task->moves);
	task_put(task, 2);
	mcp_req_tool_result(req, text, false);
}

/* Start a test_task call: a task of the slices its arguments in @p params
 * ask for, which the call waits for; or, with a prompt instead, a call that
 * asks the upstream as background work.  False if the request is finished
 * instead. */
static bool
mcp_req_task (struct mcp_req     *req,
              struct json_object *params)
{
	static_assert(SPIN_MAX == 100'000, "the problem says");
	struct mcp *mcp = req->mcp;
	struct loop *loop = mcp->loop;
	struct json_object *args = jb_get(params, "arguments");
	struct json_object *slices = jb_get(args, "slices");
	bool prompt = jb_get(args, "prompt");
	int64_t n = json_object_is_type(slices, json_type_int)
	          ? json_object_get_int64(slices) : 0;
	char const *problem =
		args && !json_object_is_type(args, json_type_object)
		? "Tool arguments must be an object."
		: prompt && slices ? "Give slices or a prompt, not both."
		: !prompt && (n < 1 || n > SPIN_MAX)
		? "The slices argument must be a whole number from 1 to 100000."
		: nullptr;
	if (problem) {
		mcp_req_tool_result(req, problem, true);
		return false;
	}
	if (prompt) {
		req->asks = JOB_BACKGROUND;
		return mcp_req_ask(req, params);
	}
	struct mcp_spin *s = malloc(sizeof *s);
	if (!s || loop_handle(loop, req, &req->call)) {
		free(s);
		mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return false;
	}
	task_init(&s->task, mcp->st->tasks, loop, mcp_spin_slice, mcp_spin_back);
	s->left = (uint64_t)n;
	s->slices = 0;
	s->x = loop->now | 1;
	s->call = req->call;
	clock_gettime(CLOCK_MONOTONIC, &req->start);
	req->task = &s->task;
	list_append(&mcp->calls, &req->hook);
	req->s->ops->waiting(req->s);
	req->phase = PHASE_TASK;
	task_submit(&s->task, loop);
	return true;
}

/* git's output is all in: take it a slice a step. */
static void
mcp_on_git (struct child *c)
{
	struct mcp_req *req = c->owner;
	req->phase = PHASE_GIT_TEXT;
	req->off = 0;
	loop_ready_bulk(req->mcp->loop, &req->step);
}

/* Take a slice of a git call's output into its answer, made UTF-8 text; once
 * all is in, say how git ended, if it failed, and reply. */
static void
mcp_req_git_text (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct loop *loop = mcp->loop;
	struct child *c = req->child;
	struct span out = buf_peek(&c->out);
	size_t left = out.len - req->off;
	size_t room = loop_room(loop, LOOP_COPY);
	size_t n = left < room ? left : room;
	if (n) {
		char *p = (char *)out.ptr + req->off;
		n = jb_mend(p, n, n == left);
		if (!dstr_cat_n(&req->answer, p, n)) {
			mcp_req_drop(req);
			list_del(&req->hook);
			req->hook = (struct list){};
			mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
			return;
		}
	}
	loop_work(loop, LOOP_COPY, n);
	req->off += n;
	if (req->off < out.len) {
		loop_ready_bulk(loop, &req->step);
		return;
	}

	/* An exit status of 1 with nothing written, as grep's finding none, is
	 * no failure. */
	static_assert(GIT_MAX == 2 << 20, "the note says 2 MiB");
	bool empty = !dstr_len(&req->answer);
	bool exited = c->code == CLD_EXITED;
	bool failed = c->late || (!exited && !c->cut) ||
	              (exited && c->status && !(c->status == 1 && empty));
	char note[96] = "";
	if (c->late)
		snprintf(note, sizeof note, "[git took longer than %.0f s, and was "
		         "stopped.]", (double)git_timeout(mcp->st->git) / 1e9);
	else if (c->cut)
		snprintf(note, sizeof note, "[The output goes on past the 2 MiB shown.]");
	else if (!exited)
		snprintf(note, sizeof note, "[git was killed by signal %d.]", c->status);
	else if (c->status)
		snprintf(note, sizeof note, "[git exited with status %d.]", c->status);
	else if (empty)
		snprintf(note, sizeof note, "[No output.]");
	child_destroy(&req->child);
	list_del(&req->hook);
	req->hook = (struct list){};
	/* A blank line between the output and the note. */
	char const *gap = empty ? "" : dstr_get(&req->answer)[dstr_len(&req->answer) - 1]
	                               == '\n' ? "\n" : "\n\n";
	if (*note && !(dstr_cat(&req->answer, gap) && dstr_cat(&req->answer, note))) {
		mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return;
	}
	mcp_req_respond(req, true, failed);
}

/* Start a git call: the command @p args asks for, in the repository it names;
 * false if the request is finished instead. */
static bool
mcp_req_git (struct mcp_req     *req,
             struct json_object *args)
{
	struct mcp *mcp = req->mcp;
	struct loop *loop = mcp->loop;
	struct git const *git = mcp->st->git;
	char const *argv[GIT_ARGV];
	char why[GIT_WHY];
	char const *problem = git_command(git, args, argv, why) ? why
	                    : mcp_req_offset(req, args);
	if (problem) {
		mcp_req_tool_result(req, problem, true);
		return false;
	}
	if (!req->tooling)
		clock_gettime(CLOCK_MONOTONIC, &req->start);
	struct child_cmd const cmd = {
		.file = "git",
		.argv = argv,
		.envp = git_env(git),
		.max = GIT_MAX,
		.deadline = loop->now + git_timeout(git),
		.grace = GIT_GRACE_NS,
		.flags = CHILD_STDERR
	};
	int e = 0;
	req->child = child_create(loop, &mcp->children, &cmd, mcp_on_git, req, &e);
	if (e == EBUSY) {
		mcp_req_tool_result(req, "Too many commands are running; try again in a few "
		                    "seconds.", true);
		return false;
	}
	if (e) {
		snprintf(why, sizeof why, "Cannot run git: %s.", strerror(e));
		LOG(LOG_WARN, "%s", why);
		mcp_req_tool_result(req, why, true);
		return false;
	}
	mcp_req_wait(req);
	req->s->ops->waiting(req->s);
	if (req->sse_ok && !req->sse)
		mcp_req_stream(req);
	req->phase = PHASE_GIT;
	return true;
}

static void mcp_req_tool_start (struct mcp_req *req);
static bool mcp_req_slice (struct mcp_req *req, struct jb_pool *pool,
                           struct span json);
static void mcp_req_notes (struct mcp_req *req, struct json_object *args);
static void mcp_req_compact (struct mcp_req *req, struct json_object *args);
static void mcp_req_turn_submit (struct mcp_req *req);
static void mcp_req_add_start (struct mcp_req *req);
static void mcp_on_notes (struct child *c);

static_assert(JOB_CALLS <= 16, "a turn's calls are bits of bad");

/* Append the literal @p s to @p d. */
#define CAT(d, s) dstr_cat_n(d, s, sizeof s - 1)

/* @p val's text and its length in @p *len, if it is a string; else
 * nullptr. */
static char const *
mcp_str (struct json_object *val,
         size_t             *len)
{
	char const *str = jb_text(val);
	*len = str ? (size_t)json_object_get_string_len(val) : 0;
	return str;
}

/* Where the last byte @p c is among the @p n at @p p, or nullptr. */
static unsigned char const *
mcp_last_of (unsigned char const *p,
             int                  c,
             size_t               n)
{
	while (n--)
		if (p[n] == (unsigned char)c)
			return p + n;
	return nullptr;
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

/* A piece of the upstream request's JSON text: bytes as they are, or text
 * escaped into a string. */
struct mcp_piece {
	char const *ptr;
	size_t      len;
	uint64_t    text;  /* bool, widened over the padding */
};

#define BYTES(b) ((struct mcp_piece){ (b).ptr, (b).len, false })
#define TEXT_OF(p, n) ((struct mcp_piece){ (char const *)(p), (n), true })

/* Write the @p n pieces at @p p into the upload, from piece req->next and
 * its byte req->off on, as far as the step's shares allow.  Returns 0 once
 * all are written; EAGAIN if the step ran out first; ENOMEM; or ENOBUFS if
 * the upload would pass UPLOAD_MAX, its context full. */
static int
mcp_req_upload (struct mcp_req         *req,
                struct mcp_piece const *p,
                size_t                  n)
{
	struct loop *loop = req->mcp->loop;
	for (; req->next < n; ++req->next, req->off = 0) {
		struct mcp_piece const *q = &p[req->next];
		while (req->off < q->len) {
			enum loop_unit kind = q->text ? LOOP_ESCAPE : LOOP_COPY;
			size_t left = q->len - req->off;
			size_t room = loop_room(loop, kind);
			size_t k = left < room ? left : room;
			if (!k)
				return EAGAIN;
			struct span s;
			int e = buf_reserve(&req->upload, q->text ? 6 * k : k, UPLOAD_MAX, &s);
			if (e)
				return e;
			if (q->text) {
				buf_commit(&req->upload, jb_escape((char *)s.ptr, q->ptr + req->off, k));
			} else {
				memcpy(s.ptr, q->ptr + req->off, k);
				buf_commit(&req->upload, k);
			}
			loop_work(loop, kind, k);
			req->off += k;
		}
	}
	req->next = 0;
	return 0;
}

/* The upload could not be written, as @p e says: out of memory, the call
 * ends; past UPLOAD_MAX, its context is full. */
static void
mcp_req_upload_failed (struct mcp_req *req,
                       int             e)
{
	if (e == ENOBUFS) {
		mcp_req_compact_full(req);
		return;
	}
	mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
}

/* The tools of @p req's client's role that its turns may call: 1 + the
 * index of their definitions in st->lent; 0 if none. */
static uint8_t
mcp_req_lent (struct mcp_req const *req)
{
	struct mcp_static const *st = req->mcp->st;
	uint32_t tools = req->s->tools;
	size_t i = (tools & MCP_TOOL_MAN && st->manual ? 1u : 0) |
	           (tools & MCP_TOOL_GIT && st->git ? 2u : 0);
	return dstr_len(&st->lent[i]) ? (uint8_t)(i + 1) : 0;
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

/* Call @p k of the turn, the struct job_call at @p p, and its id, name and
 * arguments in @p id, @p name and @p args; an id it lacks made up in
 * @p made, of 24 bytes. */
static struct job_call
mcp_req_call_at (struct mcp_req const *req,
                 unsigned char const  *p,
                 uint32_t              k,
                 struct span          *id,
                 struct span          *name,
                 struct span          *args,
                 char                 *made)
{
	struct job_call c;
	memcpy(&c, p, sizeof c);
	*id = (struct span){ (unsigned char *)(uintptr_t)(p + sizeof c), c.id };
	*name = (struct span){ id->ptr + c.id, c.name };
	*args = (struct span){ name->ptr + c.name, c.args };
	if (!c.id) {
		int n = snprintf(made, 24, "call_%" PRIu32 "_%" PRIu32, req->turn, k);
		*id = (struct span){ (unsigned char *)made, (size_t)n };
	}
	return c;
}

/* A turn has ended, its job's answer in: an answer that calls no tool is the
 * call's answer, as are the last turn's, else parse the arguments of the
 * calls it makes, then write the turn into the upload, then run them. */
static void
mcp_req_turn_done (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct job *job = req->job;
	struct span calls = buf_peek(&job->calls);
	long told = mcp->st->ep->context;
	req->used = job->used;
	req->context = told > 0 ? (uint32_t)told : job->context;
	req->cut = ai_cut(mcp->st->ep->protocol, job->reason);
	req->forced = 0;
	uint16_t n = 0;
	for (size_t off = 0; off < calls.len; ++n) {
		struct job_call c;
		memcpy(&c, calls.ptr + off, sizeof c);
		off += sizeof c + c.id + c.name + c.args;
	}
	if (calls.len > CALLS_MAX) {
		mcp_req_tool_result(req, "The other AI's tool calls in one turn took more than "
		                    "1 MiB, so none was run.", true);
		return;
	}
	req->stopped = n && req->turn >= mcp->st->turns;
	if (req->stopped)
		hist_add(&mcp->counts->calls.stopped, 1);
	if (!n || req->stopped) {
		req->phase = PHASE_TEXT;
		loop_ready_bulk(mcp->loop, &req->step);
		return;
	}
	req->n_calls = n;
	req->part = 0;
	req->at = 0;
	req->bad = 0;
	req->phase = PHASE_ARGS;
	loop_ready_bulk(mcp->loop, &req->step);
}

/* Parse the arguments of the turn's tool calls, a slice a step, each into
 * req->args, or its bit in bad if they are no JSON object; then write the
 * turn. */
static void
mcp_req_turn_args (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct loop *loop = mcp->loop;
	struct span all = buf_peek(&req->job->calls);
	for (; req->part < req->n_calls; ++req->part) {
		struct span id, name, args;
		char made[24];
		struct job_call c = mcp_req_call_at(req, all.ptr + req->at, req->part, &id, &name,
		                                    &args, made);
		struct json_object *v = nullptr;
		if (!args.len) {
			v = json_object_new_object();
			if (!v) {
				mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr,
				              0);
				return;
			}
		} else if (!mcp_req_slice(req, &mcp->answers, args)) {
			loop_ready_bulk(loop, &req->step);
			return;
		} else {
			v = req->msg;
			req->msg = nullptr;
		}
		if (v && !json_object_is_type(v, json_type_object)) {
			jb_reaper_take(&req->reaper, v);
			v = nullptr;
		}
		req->args[req->part] = v;
		req->bad |= (uint16_t)(!v << req->part);
		req->at += sizeof c + c.id + c.name + c.args;
	}
	/* The turn goes after the request's messages, which it ends no more. */
	struct ai_bytes end = ai_format(mcp->st->ep->protocol)->end;
	struct span up = buf_peek(&req->upload);
	buf_cut(&req->upload, up.len - end.len, end.len);
	req->part = 0;
	req->at = 0;
	req->next = 0;
	req->off = 0;
	req->phase = PHASE_TURN;
	loop_ready_bulk(loop, &req->step);
}

/* Write the turn into the upload, a slice a step: its text and thinking,
 * then each tool call, its arguments "{}" if they were no object, as the
 * upstream may parse them; then keep the calls, let go of the job and run
 * the first. */
static void
mcp_req_turn_write (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct ai_format const *f = ai_format(mcp->st->ep->protocol);
	struct job *job = req->job;
	struct span all = buf_peek(&job->calls);
	int e = 0;
	for (; !e && req->part <= req->n_calls + 1u; ++req->part) {
		if (!req->part) {
			struct span text = buf_peek(&job->result);
			struct span thought = buf_peek(&job->thought);
			struct mcp_piece const p[] = {
				BYTES(f->turn),
				TEXT_OF(text.ptr, text.len),
				thought.len ? BYTES(f->thought) : (struct mcp_piece){},
				TEXT_OF(thought.ptr, thought.len),
				BYTES(f->calls),
			};
			e = mcp_req_upload(req, p, ARRAY_SIZE(p));
		} else if (req->part <= req->n_calls) {
			uint32_t k = req->part - 1u;
			struct span id, name, args;
			char made[24];
			struct job_call c = mcp_req_call_at(req, all.ptr + req->at, k, &id, &name,
			                                    &args, made);
			bool bad = req->bad & 1u << k;
			struct mcp_piece const p[] = {
				BYTES(k ? f->call_next : f->call),
				TEXT_OF(id.ptr, id.len),
				BYTES(f->name),
				TEXT_OF(name.ptr, name.len),
				BYTES(f->args),
				bad || !args.len ? TEXT_OF("{}", 2) : TEXT_OF(args.ptr, args.len),
				BYTES(f->call_end),
			};
			e = mcp_req_upload(req, p, ARRAY_SIZE(p));
			if (!e)
				req->at += sizeof c + c.id + c.name + c.args;
		} else {
			e = mcp_req_upload(req, &BYTES(f->turn_end), 1);
		}
	}
	if (e == EAGAIN) {
		--req->part;
		loop_ready_bulk(mcp->loop, &req->step);
		return;
	}
	if (e) {
		mcp_req_upload_failed(req, e);
		return;
	}
	struct span s;
	if (buf_reserve(&req->calls, all.len, CALLS_MAX, &s)) {
		mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return;
	}
	memcpy(s.ptr, all.ptr, all.len);
	buf_commit(&req->calls, all.len);
	loop_work(mcp->loop, LOOP_COPY, all.len);
	/* The usage counts the turn: what is written after it counts on. */
	req->sent = buf_peek(&req->upload).len;
	mcp_req_drop(req);
	req->part = 0;
	req->at = 0;
	mcp_req_tool_start(req);
}

/* Add a line for a tool call to the trace, @p name its name and @p text
 * its arguments' JSON text, @p args parsed, but what notes and compact are
 * given, which are the upstream's own; the outcome follows. */
static void
mcp_req_trace (struct mcp_req     *req,
               struct span         name,
               struct span         text,
               struct json_object *args,
               enum metrics_lent   tool)
{
	if (req->traced++ >= TRACE_MAX)
		return;
	size_t act = 0;
	char const *action = tool == LENT_NOTES ? mcp_str(jb_get(args, "action"), &act)
	                                        : nullptr;
	char const *json = tool == LENT_NOTES || tool == LENT_COMPACT
	                 ? nullptr : (char const *)text.ptr;
	size_t len = json ? jb_utf8(json, mcp_clip(json, text.len, TRACE_ARGS)) : 0;
	size_t n = mcp_clip((char const *)name.ptr, name.len, 64);
	bool ok = CAT(&req->trace, "- ") &&
	          dstr_cat_n(&req->trace, (char const *)name.ptr, jb_utf8((char const *)
	                                                                   name.ptr, n)) &&
	          (!action || (CAT(&req->trace, " ") &&
	                       dstr_cat_n(&req->trace, action, mcp_clip(action, act, 16)))) &&
	          (!len || (CAT(&req->trace, " ") && dstr_cat_n(&req->trace, json, len))) &&
	          (!json || len == text.len || CAT(&req->trace, "..."));
	(void)ok;
}

/* End the trace's line for the call under way: whether it failed, or the
 * bytes of its output. */
static void
mcp_req_trace_end (struct mcp_req *req,
                   bool            failed,
                   size_t          bytes)
{
	if (req->traced > TRACE_MAX)
		return;
	if (failed)
		(void)CAT(&req->trace, ": failed\n");
	else
		(void)dstr_printf(&req->trace, ": %zu bytes\n", bytes);
}

/* Run the turn's tool call req->part, unless all have run: then submit the
 * next turn, or, its context full, compact it first.  A call goes to the
 * tool of its name that the turns may call, which writes its output into
 * the request's answer, or gives a page, and calls mcp_req_tool_done(). */
static void
mcp_req_tool_start (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct mcp_static const *st = mcp->st;
	if (req->part == req->n_calls) {
		buf_fini(&req->calls);
		if (req->context && req->used && mcp_req_tokens(req) >= req->context)
			mcp_req_compact_full(req);
		else
			mcp_req_turn_submit(req);
		return;
	}
	if (mcp->draining) {
		mcp_req_error(req, 503, INTERNAL_ERROR, "Server is shutting down", nullptr, 0);
		return;
	}
	struct span id, name, args;
	char made[24];
	(void)mcp_req_call_at(req, buf_peek(&req->calls).ptr + req->at, req->part, &id, &name,
	                      &args, made);
	uint32_t lent = req->lent - 1u;
#define IS(s) (name.len == sizeof s - 1 && !memcmp(name.ptr, s, sizeof s - 1))
	enum metrics_lent tool = IS("man") && lent & 1 ? LENT_MAN
	               : IS("git") && lent & 2 ? LENT_GIT
	               : IS("notes") && st->notes ? LENT_NOTES
	               : IS("compact") ? LENT_COMPACT
	               : LENT_NONE;
#undef IS
	static char const *const names[] = {
		[LENT_NONE] = "an unknown tool", [LENT_MAN] = "man", [LENT_GIT] = "git",
		[LENT_NOTES] = "notes", [LENT_COMPACT] = "compact",
	};
	/* Its arguments go once its result is written, or the request ends. */
	struct json_object *a = req->args[req->part];
	req->tool = (uint8_t)tool;
	hist_add(&mcp->counts->calls.lent[tool], 1);
	req->skip = 0;
	req->tooling = true;
	dstr_clear(&req->answer);
	mcp_req_trace(req, name, args, a, tool);
	mcp_req_notify_tool(req, names[tool]);
	if (req->bad & 1u << req->part && req->cut)
		mcp_req_tool_result(req, "Your answer reached the output token limit, which cut "
		                    "these arguments short, so the call was not run.", true);
	else if (req->bad & 1u << req->part)
		mcp_req_tool_result(req, "Your arguments were not a JSON object, so the call "
		                    "was not run.", true);
	else if (tool == LENT_MAN)
		(void)mcp_req_man(req, a);
	else if (tool == LENT_GIT)
		(void)mcp_req_git(req, a);
	else if (tool == LENT_NOTES)
		mcp_req_notes(req, a);
	else if (tool == LENT_COMPACT)
		mcp_req_compact(req, a);
	else
		mcp_req_tool_result(req, "No tool of that name is yours to call.", true);
}

/* The call under way has its output, its window and what askai adds after
 * it: for the turn's last call, what the context holds, if it fills up,
 * and that the next turn is the last, if it is. */
static void
mcp_req_tool_done (struct mcp_req *req,
                   bool            ok,
                   bool            failed)
{
	struct mcp *mcp = req->mcp;
	req->tooling = false;
	req->note[0] = '\0';
	if (!ok) {
		mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return;
	}
	struct span out = mcp_req_output(req);
	mcp_req_trace_end(req, failed, out.len);
	/* From a character's start, up to a line's end if one is near. */
	size_t cap = mcp_req_lend_max(req);
	bool past = req->skip > out.len;
	size_t skip = past ? out.len : req->skip;
	while (skip < out.len && (out.ptr[skip] & 0xc0) == 0x80)
		++skip;
	size_t len = out.len - skip;
	size_t window = len <= cap ? len : mcp_clip((char const *)out.ptr + skip, len, cap);
	if (window < len) {
		size_t from = window - window / 4;
		unsigned char const *nl = mcp_last_of(out.ptr + skip + from, '\n', window - from);
		window = nl ? (size_t)(nl - out.ptr - skip) + 1 : window;
	}
	req->skip = skip;
	req->window = window;
	int n = 0;
	size_t room = sizeof req->note;
	bool pages = req->tool == LENT_MAN || req->tool == LENT_GIT;
	if (skip + window < out.len && pages)
		n = snprintf(req->note, room, "\n\n[Bytes %zu to %zu of %zu: for the rest, "
		             "call again with \"offset\": %zu.]", skip, skip + window,
		             out.len, skip + window);
	else if (skip + window < out.len)
		n = snprintf(req->note, room, "\n\n[Bytes %zu to %zu of %zu: the rest is "
		             "cut.]", skip, skip + window, out.len);
	else if (past)
		n = snprintf(req->note, room, "[The output has %zu bytes, fewer than the "
		             "offset.]", out.len);
	else if (skip)
		n = snprintf(req->note, room, "\n\n[Bytes %zu to %zu of %zu: its end.]", skip,
		             out.len, out.len);
	if (req->part + 1u == req->n_calls) {
		uint64_t tokens = mcp_req_tokens(req) + (window + (size_t)n) / 4;
		if (req->context && tokens * 4 >= (uint64_t)req->context * 3 &&
		    (size_t)n < room)
			n += snprintf(req->note + n, room - (size_t)n, "\n\n[askai: your context "
			              "is %" PRIu64 "%% full, %" PRIu64 " of %" PRIu32 " tokens. "
			              "Note what you need to keep, then call compact.]",
			              tokens * 100 / req->context, tokens, req->context);
		if (req->turn + 1u == mcp->st->turns && (size_t)n < room)
			snprintf(req->note + n, room - (size_t)n, "\n\n[askai: your next turn is "
			         "your last: answer then, without calling tools.]");
	}
	req->phase = PHASE_RESULT;
	req->next = 0;
	req->off = 0;
	loop_ready_bulk(mcp->loop, &req->step);
}

/* Write the result of the call under way into the upload, a slice a step,
 * and after the turn's last one the end of the messages; then let go of
 * its output, and run the next call. */
static void
mcp_req_result_write (struct mcp_req *req)
{
	struct mcp *mcp = req->mcp;
	struct ai_format const *f = ai_format(mcp->st->ep->protocol);
	struct span out = mcp_req_output(req);
	struct span id, name, args;
	char made[24];
	struct job_call c = mcp_req_call_at(req, buf_peek(&req->calls).ptr + req->at, req->part,
	                                    &id, &name, &args, made);
	bool last = req->part + 1u == req->n_calls;
	struct mcp_piece const p[] = {
		BYTES(f->result),
		TEXT_OF(id.ptr, id.len),
		BYTES(f->output),
		TEXT_OF(out.len ? out.ptr + req->skip : nullptr, req->window),
		TEXT_OF(req->note, strlen(req->note)),
		BYTES(f->result_end),
		last ? BYTES(f->end) : (struct mcp_piece){},
	};
	int e = mcp_req_upload(req, p, ARRAY_SIZE(p));
	if (e == EAGAIN) {
		loop_ready_bulk(mcp->loop, &req->step);
		return;
	}
	if (e) {
		mcp_req_upload_failed(req, e);
		return;
	}
	manual_page_put(req->page);
	req->page = nullptr;
	dstr_clear(&req->answer);
	req->note[0] = '\0';
	req->skip = req->window = 0;
	req->at += sizeof c + c.id + c.name + c.args;
	mcp_req_args_free(req, req->part, req->part + 1u);
	++req->part;
	mcp_req_tool_start(req);
}

/* Submit the next turn, its upload whole; or, draining, end the call. */
static void
mcp_req_turn_submit (struct mcp_req *req)
{
	if (req->mcp->draining) {
		mcp_req_error(req, 503, INTERNAL_ERROR, "Server is shutting down", nullptr, 0);
		return;
	}
	mcp_req_wait(req);
	mcp_req_submit(req);
}

/* The first turn's upload is written: submit it, after what askai adds to
 * the prompt, if anything. */
static void
mcp_req_turn_ready (struct mcp_req *req)
{
	if (dstr_len(&req->answer))
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
	req->phase = PHASE_ADD;
	loop_ready_bulk(req->mcp->loop, &req->step);
}

/* Write what askai adds to the prompt a slice a step, and end the prompt
 * and the messages; then submit the turn. */
static void
mcp_req_add_write (struct mcp_req *req)
{
	struct ai_format const *f = ai_format(req->mcp->st->ep->protocol);
	struct mcp_piece const p[] = {
		TEXT_OF(dstr_get(&req->answer), dstr_len(&req->answer)),
		BYTES(f->prompt_end),
		BYTES(f->end),
	};
	int e = mcp_req_upload(req, p, ARRAY_SIZE(p));
	if (e == EAGAIN) {
		loop_ready_bulk(req->mcp->loop, &req->step);
		return;
	}
	if (e) {
		mcp_req_error(req, 500, INTERNAL_ERROR, e == ENOBUFS
		              ? "The task and its notes do not fit the upstream request"
		              : "Out of memory", nullptr, 0);
		return;
	}
	dstr_clear(&req->answer);
	mcp_req_turn_submit(req);
}

/* Spawn the git of @p argv, its input @p in, for the notes, for what
 * req->fetch says: 0, or why not. */
static int
mcp_req_notes_spawn (struct mcp_req *req,
                     char const     *argv[],
                     struct span     in)
{
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
	req->child = child_create(loop, &mcp->children, &cmd, mcp_on_notes, req, &e);
	if (!req->child)
		return e;
	mcp_req_wait(req);
	req->phase = PHASE_NOTES;
	return 0;
}

/* The notes' git could not be spawned, as @p e says: a compaction goes on
 * without the notes; the notes tool's call, or a thread gone on with, ends
 * with why. */
static void
mcp_req_notes_failed (struct mcp_req *req,
                      int             e)
{
	if (e != EBUSY)
		LOG(LOG_WARN, "notes: cannot run git: %s", strerror(e));
	if (req->fetch == FETCH_COMPACT || req->fetch == FETCH_FULL) {
		if (CAT(&req->answer, "\n\n[askai: your notes could not be read.]\n\n"
		        "[askai: go on with the task.]"))
			mcp_req_add_start(req);
		else
			mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return;
	}
	char why[96];
	snprintf(why, sizeof why, e == EBUSY ? "Too many commands are running; try again "
	         "in a few seconds." : "Cannot run git: %s.", strerror(e));
	dstr_clear(&req->answer);
	mcp_req_tool_result(req, why, true);
}

/* Read the notes of the request's thread for @p fetch, those that hold
 * @p search if not nullptr; false if the request is finished instead. */
static bool
mcp_req_notes_fetch (struct mcp_req *req,
                     enum mcp_fetch  fetch,
                     char const     *search)
{
	char const *argv[NOTES_ARGV];
	notes_read(req->mcp->st->notes, req->ref, search, argv);
	req->fetch = (uint8_t)fetch;
	int e = mcp_req_notes_spawn(req, argv, (struct span){});
	if (e)
		mcp_req_notes_failed(req, e);
	return !e || fetch == FETCH_COMPACT || fetch == FETCH_FULL;
}

/* The notes tool's call: write a note, or read them back. */
static void
mcp_req_notes (struct mcp_req     *req,
               struct json_object *args)
{
	static_assert(NOTES_TEXT == 8 << 10 && NOTES_SEARCH == 1 << 10,
	              "the problems say");
	struct json_object *action = jb_get(args, "action");
	struct json_object *text = jb_get(args, "text");
	struct json_object *search = jb_get(args, "search");
	bool write = jb_is(action, "write");
	size_t len = 0;
	char const *note = mcp_str(text, &len);
	size_t find_len = 0;
	char const *find = mcp_str(search, &find_len);
	char const *problem =
		!write && !jb_is(action, "read") ? "The action argument must be write or read."
		: write && (!note || !len || len > NOTES_TEXT || memchr(note, '\0', len))
		? "A note must be a string of 1 to 8192 bytes, and no NUL."
		: search && (write || !find || !find_len || find_len > NOTES_SEARCH ||
		             strcspn(find, "\n") < find_len)
		? "The search argument, which only read takes, must be a line of 1 to "
		  "1024 bytes."
		: nullptr;
	if (problem) {
		mcp_req_tool_result(req, problem, true);
		return;
	}
	if (!write && !req->branch) {
		mcp_req_tool_result(req, "You have written no notes yet.", false);
		return;
	}
	if (!write) {
		(void)mcp_req_notes_fetch(req, FETCH_READ, find);
		return;
	}
	char const *argv[NOTES_ARGV];
	notes_write(req->mcp->st->notes, argv);
	if (notes_input(&req->body, req->ref, note, len, req->branch)) {
		mcp_req_tool_done(req, false, true);
		return;
	}
	req->fetch = FETCH_WRITE;
	int e = mcp_req_notes_spawn(req, argv, buf_peek(&req->body));
	buf_fini(&req->body);
	if (e)
		mcp_req_notes_failed(req, e);
}

/* The notes' git has ended, its output all in: take it in a step of its
 * own. */
static void
mcp_on_notes (struct child *c)
{
	struct mcp_req *req = c->owner;
	req->phase = PHASE_NOTES_OUT;
	loop_ready_bulk(req->mcp->loop, &req->step);
}

/* Add the newest notes of @p out, git log's, newest first and each ending
 * in a NUL, that fit the bytes a tool's output may take, at least one, to
 * the request's answer, oldest first; false if out of memory.  Only those
 * are looked at: the rest are only counted, @p cut saying that git's
 * output was cut, so that some went uncounted. */
static bool
mcp_req_notes_render (struct mcp_req *req,
                      struct span     out,
                      bool            cut)
{
	/* What a rendered note adds to its text, and the line before them. */
	enum { EACH = 8, HEAD = 64 };
	size_t budget = mcp_req_lend_max(req);
	struct span shown[NOTES_READ];
	uint32_t n = 0;
	size_t at = 0;
	size_t rendered = HEAD;
	while (at < out.len && n < NOTES_READ) {
		unsigned char const *nul = memchr(out.ptr + at, '\0', out.len - at);
		size_t end = nul ? (size_t)(nul - out.ptr) : out.len;
		rendered += end - at + EACH;
		if (n && rendered > budget)
			break;
		shown[n++] = (struct span){ out.ptr + at, end - at };
		at = nul ? end + 1 : end;
	}
	uint32_t more = 0;
	while (at < out.len) {
		unsigned char const *nul = memchr(out.ptr + at, '\0', out.len - at);
		at = nul ? (size_t)(nul - out.ptr) + 1 : out.len;
		++more;
	}
	loop_work(req->mcp->loop, LOOP_COPY, out.len);
	bool ok = true;
	if (more)
		ok = dstr_printf(&req->answer, "[%" PRIu32 "%s older note%s not shown: "
		                 "search for %s.]\n\n", more, cut ? " or more" : "",
		                 more > 1 || cut ? "s are" : " is",
		                 more > 1 || cut ? "them" : "it");
	else if (cut)
		ok = CAT(&req->answer, "[Older notes are not shown: search for them.]\n\n");
	for (uint32_t k = n; ok && k--; ) {
		char const *note = (char const *)shown[k].ptr;
		ok = dstr_printf(&req->answer, "%" PRIu32 ". ", n - k) &&
		     dstr_cat_n(&req->answer, note, jb_utf8(note, shown[k].len)) &&
		     (k ? CAT(&req->answer, "\n\n") : CAT(&req->answer, "\n"));
	}
	return ok;
}

/* Take what the notes' git said, for what it ran for: a note written, or the
 * notes read, for the notes tool, for the prompt of a thread gone on with,
 * or for a compaction. */
static void
mcp_req_notes_out (struct mcp_req *req)
{
	struct child *c = req->child;
	struct span out = buf_peek(&c->out);
	bool ok = c->cut || (!c->late && c->code == CLD_EXITED && !c->status);
	/* Exit status 128 with fatal: bad revision: no such branch. */
	bool none = !c->late && c->code == CLD_EXITED && c->status == 128;
	mcp_req_unwait(req);
	enum mcp_fetch fetch = req->fetch;
	bool mem = true;
	if (fetch == FETCH_WRITE) {
		req->branch |= ok;
		if (ok) {
			mem = CAT(&req->answer, "Noted.");
		} else {
			size_t n = jb_utf8((char const *)out.ptr, mcp_clip((char const *)out.ptr,
			                                              out.len, 300));
			mem = c->late ? CAT(&req->answer, "Could not write the note: git took "
			                    "too long.") != nullptr
			    : CAT(&req->answer, "Could not write the note: ") &&
			      dstr_cat_n(&req->answer, (char const *)out.ptr, n);
		}
		child_destroy(&req->child);
		mcp_req_tool_done(req, mem, !ok);
		return;
	}
	if (fetch == FETCH_RESUME && !ok) {
		child_destroy(&req->child);
		char why[96 + NOTES_ID];
		snprintf(why, sizeof why, none ? "Unknown thread: %s." : "Cannot read the "
		         "notes of the thread %s.", req->thread);
		mcp_req_drop(req);
		dstr_clear(&req->answer);
		mcp_req_tool_result(req, why, true);
		return;
	}
	if (fetch == FETCH_RESUME) {
		req->branch = true;
		mem = CAT(&req->answer, "\n\n[askai: the notes you kept in earlier calls "
		          "on this thread, oldest first:]\n\n");
	} else if (fetch != FETCH_READ) {
		mem = ok ? CAT(&req->answer, "\n\n[askai: your notes, oldest first:]\n\n")
		         : CAT(&req->answer, "\n\n[askai: your notes could not be read.]\n");
	}
	if (mem && ok && !out.len && fetch == FETCH_READ)
		mem = CAT(&req->answer, "No note holds that text.");
	if (mem && ok)
		mem = mcp_req_notes_render(req, out, c->cut);
	if (mem && fetch == FETCH_READ && !ok)
		mem = none ? CAT(&req->answer, "You have written no notes yet.")
		           : CAT(&req->answer, "Your notes could not be read.");
	if (mem && fetch != FETCH_READ && fetch != FETCH_RESUME)
		mem = CAT(&req->answer, "\n[askai: go on with the task.]");
	child_destroy(&req->child);
	if (fetch == FETCH_READ) {
		mcp_req_tool_done(req, mem, !ok && !none);
	} else if (!mem) {
		mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
	} else if (fetch == FETCH_RESUME) {
		req->phase = PHASE_BUILD;
		loop_ready_bulk(req->mcp->loop, &req->step);
	} else {
		mcp_req_add_start(req);
	}
}

/* Compact the context, as the turn's compact call asks: what follows the
 * prompt goes, and askai adds the summary and the notes to it.  The turn's
 * calls after this one do not run. */
static void
mcp_req_compact (struct mcp_req     *req,
                 struct json_object *args)
{
	static_assert(SUMMARY_MAX == 16 << 10, "the problem says");
	size_t len = 0;
	char const *summary = mcp_str(jb_get(args, "summary"), &len);
	if (!summary || !len || len > SUMMARY_MAX) {
		mcp_req_tool_result(req, "The summary argument must be a string of 1 to 16384 "
		                    "bytes.", true);
		return;
	}
	req->tooling = false;
	mcp_req_trace_end(req, false, len);
	hist_add(&req->mcp->counts->calls.compacted[0], 1);
	dstr_clear(&req->answer);
	bool ok = CAT(&req->answer, "\n\n[askai: you compacted the context: the "
	              "conversation since the task is gone, your turns and the tools' "
	              "results with it. Your summary of it:]\n\n") &&
	          dstr_cat_n(&req->answer, summary, jb_utf8(summary, len));
	mcp_req_args_free(req, req->part, ARRAY_SIZE(req->args));
	buf_fini(&req->calls);
	if (!ok) {
		mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return;
	}
	if (req->mcp->draining) {
		mcp_req_error(req, 503, INTERNAL_ERROR, "Server is shutting down", nullptr, 0);
		return;
	}
	if (req->branch) {
		(void)mcp_req_notes_fetch(req, FETCH_COMPACT, nullptr);
		return;
	}
	if (!CAT(&req->answer, "\n\n[askai: go on with the task.]")) {
		mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return;
	}
	mcp_req_add_start(req);
}

/* The context is full, as the upstream said or as askai reckons: what
 * follows the prompt goes, and askai adds the notes to it, unless that was
 * done already since a turn went through. */
static void
mcp_req_compact_full (struct mcp_req *req)
{
	if (req->mcp->draining) {
		mcp_req_error(req, 503, INTERNAL_ERROR, "Server is shutting down", nullptr, 0);
		return;
	}
	if (req->forced++) {
		mcp_req_tool_result(req, "The upstream's context cannot hold the task and the "
		                    "notes kept for it.", true);
		return;
	}
	hist_add(&req->mcp->counts->calls.compacted[1], 1);
	mcp_req_drop(req);
	mcp_req_args_free(req, 0, ARRAY_SIZE(req->args));
	buf_fini(&req->calls);
	dstr_clear(&req->answer);
	req->tooling = false;
	if (!CAT(&req->answer, "\n\n[askai: your context filled up, so the "
	         "conversation since the task is gone, your turns and the tools' "
	         "results with it.]")) {
		mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return;
	}
	if (req->branch) {
		(void)mcp_req_notes_fetch(req, FETCH_FULL, nullptr);
		return;
	}
	if (!CAT(&req->answer, "\n\n[askai: go on with the task.]")) {
		mcp_req_error(req, 500, INTERNAL_ERROR, "Out of memory", nullptr, 0);
		return;
	}
	mcp_req_add_start(req);
}

#undef CAT
#undef TEXT_OF
#undef BYTES

/* Start a tool call; false if the request is finished instead. */
static bool
mcp_req_tools_call (struct mcp_req     *req,
                    struct json_object *params)
{
	struct mcp *mcp = req->mcp;
	struct mcp_static const *st = mcp->st;
	if (mcp->draining) {
		mcp_req_error(req, 503, INTERNAL_ERROR, "Server is shutting down", nullptr, 0);
		return false;
	}
	char const *name = jb_text(jb_get(params, "name"));
	uint32_t tool = !name ? 0 : !strcmp(name, st->tool) ? MCP_TOOL_ASK
	              : st->manual && !strcmp(name, "man") ? MCP_TOOL_MAN
	              : st->tasks && !strcmp(name, "test_task") ? MCP_TOOL_TASK
	              : st->git && !strcmp(name, "git") ? MCP_TOOL_GIT : 0;
	/* One that the client's role may not call is not unknown, but not
	 * available to it. */
	if (!(tool & req->s->tools)) {
		static char const unknown[] = "Unknown tool: ";
		static char const denied[] = "Tool not available: ";
		size_t at = tool ? sizeof denied - 1 : sizeof unknown - 1;
		char msg[32 + 6 * 100];
		memcpy(msg, tool ? denied : unknown, at);
		size_t len = name ? (size_t)json_object_get_string_len(jb_get(params, "name"))
		                  : 0;
		size_t n = name ? jb_escape(msg + at, name, mcp_clip(name, len, 100))
		                : (size_t)sprintf(msg + at, "(none)");
		msg[at + n] = '\0';
		mcp_req_error(req, 200, INVALID_PARAMS, msg, nullptr, 0);
		return false;
	}
	if (tool == MCP_TOOL_MAN)
		return mcp_req_man(req, jb_get(params, "arguments"));
	if (tool == MCP_TOOL_TASK)
		return mcp_req_task(req, params);
	if (tool == MCP_TOOL_GIT)
		return mcp_req_git(req, jb_get(params, "arguments"));
	req->lent = mcp_req_lent(req);
	return mcp_req_ask(req, params);
}

static void
mcp_req_initialize (struct mcp_req     *req,
                    struct json_object *params)
{
	struct json_object *v = jb_get(params, "protocolVersion");
	char const *version = jb_text(v);
	if (!mcp_is_legacy(version, version ? (size_t)json_object_get_string_len(v) : 0))
		version = versions[MODERN];

	/* Never stored: the ID only scopes the request IDs a client cancels. */
	static char const hex[] = "0123456789abcdef";
	unsigned char rnd[16];
	char session[2 * sizeof rnd + 1] = "";
	if (getrandom(rnd, sizeof rnd, 0) == (ssize_t)sizeof rnd) {
		for (size_t i = 0; i < sizeof rnd; ++i) {
			session[2 * i] = hex[rnd[i] >> 4];
			session[2 * i + 1] = hex[rnd[i] & 15];
		}
	}

	struct mcp_frag const f[] = {
		FRAG("{\"protocolVersion\":\""),
		{ version, strlen(version) },
		TEXT(&req->mcp->st->init_tail),
	};
	mcp_req_result(req, f, ARRAY_SIZE(f), *session ? session : nullptr);
}

/* A legacy client gave up on a call, which may wait on any loop: hand its
 * key to home in a carrier job, and home ends the call's job, whose
 * completion ends the call's stream quietly.  An id no request may have
 * names no call, and out of memory nothing does. */
static void
mcp_cancel (struct mcp         *mcp,
            struct span         session,
            struct json_object *params)
{
	struct json_object *id = jb_get(params, "requestId");
	struct job *job = mcp_id_valid(id) ? mcp_jobs_take(mcp) : nullptr;
	if (!job)
		return;
	if (!mcp_key(job, mcp->st->seed, session, id)) {
		mcp_jobs_shelve(mcp, job);
		return;
	}
	/* Home's, and the post adds the message's. */
	atomic_store_explicit(&job->refs, 1, memory_order_relaxed);
	job_post(job, mcp->home, &job->to_home, &job->home_bits, JOB_WITHDRAW);
}

/* Validate the per-request metadata of a modern request. */
static bool
mcp_req_modern (struct mcp_req     *req,
                char const         *method,
                struct json_object *params)
{
	struct stream *s = req->s;
	struct json_object *m = jb_get(params, "_meta");
	struct json_object *v = jb_get(m, meta_version);
	char const *version = jb_text(v);
	if (!version || !json_object_is_type(jb_get(m, meta_caps), json_type_object)) {
		mcp_req_error(req, 400, INVALID_PARAMS, "Request _meta must carry "
		              "protocolVersion and clientCapabilities", nullptr, 0);
		return false;
	}
	if (!mcp_same(s->ops->field(s, F_VERSION), version)) {
		mcp_req_error(req, 400, HEADER_MISMATCH, "MCP-Protocol-Version header "
		              "does not match the request _meta", nullptr, 0);
		return false;
	}
	size_t len = (size_t)json_object_get_string_len(v);
	if (mcp_version_index(version, len) >= MODERN) {
		char echo[6 * MAX_ECHO];
		size_t n = jb_escape(echo, version, mcp_clip(version, len, MAX_ECHO));
		struct mcp_frag const data[] = {
			FRAG("{\"supported\":"),
			TEXT(&req->mcp->st->versions),
			FRAG(",\"requested\":\""),
			{ echo, n },
			FRAG("\"}"),
		};
		mcp_req_error(req, 400, UNSUPPORTED_VERSION, "Unsupported protocol version",
		              data, ARRAY_SIZE(data));
		return false;
	}
	if (!mcp_same(s->ops->field(s, F_METHOD), method)) {
		mcp_req_error(req, 400, HEADER_MISMATCH,
		              "Mcp-Method header does not match the request method", nullptr, 0);
		return false;
	}
	char const *name = jb_text(jb_get(params, "name"));
	if (!strcmp(method, "tools/call") && name &&
	    !mcp_same(s->ops->field(s, F_NAME), name)) {
		mcp_req_error(req, 400, HEADER_MISMATCH,
		              "Mcp-Name header does not match the tool name", nullptr, 0);
		return false;
	}
	return true;
}

/* Answer the message, or start the tool call it asks for; false if the
 * request is finished. */
static bool
mcp_req_message (struct mcp_req *req)
{
	struct json_object *msg = req->msg;
	struct mcp *mcp = req->mcp;
	struct mcp_static const *st = mcp->st;
	struct stream *s = req->s;
	char const *method = jb_text(jb_get(msg, "method"));
	struct json_object *params = jb_get(msg, "params");
	struct json_object *id;
	bool has_id = json_object_object_get_ex(msg, "id", &id);

	if (!jb_is(jb_get(msg, "jsonrpc"), "2.0") ||
	    (!method && !(has_id && (jb_get(msg, "result") || jb_get(msg, "error"))))) {
		mcp_req_error(req, 400, INVALID_REQUEST, "Invalid Request", nullptr, 0);
		return false;
	}
	req->method = !method || !has_id ? METHOD_NOTIFY : mcp_method_index(method);
	if (!method || !has_id) {
		/* Responses (we send no requests) and notifications. */
		if (method && !strcmp(method, "notifications/cancelled"))
			mcp_cancel(mcp, s->ops->field(s, F_SESSION), params);
		(void)s->ops->head(s, 202, (struct span){}, 0);
		mcp_req_finish(req);
		return false;
	}
	if (!mcp_id_valid(id)) {
		mcp_req_error(req, 400, INVALID_REQUEST, "Invalid request id", nullptr, 0);
		return false;
	}
	req->id = json_object_get(id);
	if (params && !json_object_is_type(params, json_type_object)) {
		mcp_req_error(req, 400, INVALID_PARAMS, "Params must be an object", nullptr, 0);
		return false;
	}

	struct json_object *v = jb_get(jb_get(params, "_meta"), meta_version);
	char const *version = jb_text(v);
	struct span header = s->ops->field(s, F_VERSION);
	req->modern = (version &&
	               !mcp_is_legacy(version, (size_t)json_object_get_string_len(v))) ||
	              (header.ptr && !mcp_is_legacy((char const *)header.ptr, header.len));
	if (req->modern && !mcp_req_modern(req, method, params))
		return false;
	/* Without the header a legacy client speaks 2025-03-26, which has
	 * event streams; 2024-11-05 has none. */
	req->sse_ok = mcp_accepts_sse(s->ops->field(s, F_ACCEPT)) &&
	              (req->modern || !mcp_same(header, pre_streams));

	if (!strcmp(method, "tools/call"))
		return mcp_req_tools_call(req, params);
	if (!strcmp(method, "tools/list"))
		mcp_req_result(req, &TEXT(&st->list[s->tools & ((1 << MCP_TOOLS) - 1)]
		                                 [req->modern]), 1, nullptr);
	else if (req->modern && !strcmp(method, "server/discover"))
		mcp_req_result(req, &TEXT(&st->discover), 1, nullptr);
	else if (!req->modern && !strcmp(method, "initialize"))
		mcp_req_initialize(req, params);
	else if (!req->modern && !strcmp(method, "ping"))
		mcp_req_result(req, &FRAG("{}"), 1, nullptr);
	else
		/* Legacy clients take a 404 for an expired session, so keep 200. */
		mcp_req_error(req, req->modern ? 404 : 200, METHOD_NOT_FOUND,
		              "Method not found", nullptr, 0);
	return false;
}

#undef TEXT
#undef FRAG

/* Hand the parsed message on; a tool call frees what it does not keep of it
 * while it runs. */
static void
mcp_req_dispatch (struct mcp_req *req)
{
	if (!req->msg)
		mcp_req_error(req, 400, PARSE_ERROR, "Parse error", nullptr, 0);
	else if (mcp_req_message(req))
		mcp_req_dispose(req);
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
	/* Stopped with no text, its turns gave no answer. */
	if (req->job->status == JOB_TEXT) {
		mcp_req_respond(req, true, req->stopped && !buf_peek(&req->job->result).len);
		return;
	}
	struct span body = buf_peek(&req->job->result);
	bool failed = true;
	bool ok = !ai_answer(req->mcp->st->ep->protocol, req->job->http, req->msg,
	                     body.len ? (char const *)body.ptr : "", body.len,
	                     &req->answer, &failed);
	loop_work(req->mcp->loop, LOOP_COPY, dstr_len(&req->answer));
	mcp_req_dispose(req);
	mcp_req_drop(req);
	if (ok && failed && mcp_req_refit(req, dstr_get(&req->answer), dstr_len(&req->answer))) {
		mcp_req_compact_full(req);
		return;
	}
	mcp_req_respond(req, ok, failed);
}

/* Render a share of the metrics into the reply; once all are in, send it. */
static void
mcp_req_metrics (struct mcp_req *req)
{
	static char const fields[] = "Content-Type: text/plain; version=0.0.4; "
	                             "charset=utf-8\r\n";
	struct stream *s = req->s;
	int e = metrics_render(&req->mcp->metrics, &req->off, &req->reply);
	if (e == EAGAIN) {
		loop_ready_bulk(req->mcp->loop, &req->step);
		return;
	}
	req->error = e ? e : s->ops->head(s, 200, (struct span){
		(unsigned char *)(uintptr_t)fields, sizeof fields - 1
	}, buf_peek(&req->reply).len);
	if (req->error) {
		mcp_req_finish(req);
		return;
	}
	req->phase = PHASE_SEND;
	mcp_req_send(req);
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
	case PHASE_GIT_TEXT:
		mcp_req_git_text(req);
		break;
	case PHASE_ARGS:
		mcp_req_turn_args(req);
		break;
	case PHASE_TURN:
		mcp_req_turn_write(req);
		break;
	case PHASE_NOTES_OUT:
		mcp_req_notes_out(req);
		break;
	case PHASE_RESULT:
		mcp_req_result_write(req);
		break;
	case PHASE_ADD:
		mcp_req_add_write(req);
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

static void
mcp_on_head (void          *ctx,
             struct stream *s,
             struct span    method,
             struct span    target,
             size_t         length)
{
	struct mcp *mcp = ctx;
	bool metrics = mcp_span_is(method, "GET") && mcp_is_metrics(target);
	if (!metrics && !mcp_span_is(method, "POST")) {
		/* No standalone event stream (GET) and no sessions to DELETE. */
		mcp_http_error(s, 405, "Method Not Allowed", "Allow: POST, OPTIONS\r\n");
		return;
	}
	if (metrics && !s->trusted) {
		mcp_http_error(s, 403, "Forbidden", "");
		return;
	}
	if (!metrics && !mcp_is_json(s->ops->field(s, F_TYPE))) {
		mcp_http_error(s, 415, "Unsupported Media Type", "");
		return;
	}
	struct mcp_req *req = mcp_spare_take(mcp);
	if (!req) {
		mcp_http_error(s, 500, "Out of memory", "");
		return;
	}
	*req = (struct mcp_req){
		.step.fn = mcp_req_step,
		.timer.fn = mcp_req_timer,
		.reaper = req->reaper,
		.scan = req->scan,
		.answer = req->answer,
		.trace = req->trace,
		.s = s,
		.mcp = mcp,
		.head = loop_clock(),
		.phase = metrics ? PHASE_METRICS : PHASE_BODY,
		.method = metrics ? METHOD_METRICS : METHOD_OTHER
	};
	s->user = req;
	/* A body of known length is taken in without growing. */
	if (!metrics && length && length != STREAM_OPEN)
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
	struct span room = {};
	if (bytes.len && buf_reserve(&req->body, bytes.len, SIZE_MAX, &room)) {
		mcp_req_http_error(req, 500, "Out of memory");
		return;
	}
	if (bytes.len) {
		memcpy(room.ptr, bytes.ptr, bytes.len);
		buf_commit(&req->body, bytes.len);
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
		LOG(LOG_VERBOSE, "tool call cancelled after %.1f s", mcp_elapsed(&req->start));
	req->s = nullptr;
	mcp_req_finish(req);
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
		struct mcp_req *req = container_of(mcp->calls.next, struct mcp_req, hook);
		list_del(&req->hook);
		req->hook = (struct list){};
		mcp_req_drop(req);
		loop_timer_stop(loop, &req->timer);
		mcp_req_error(req, 503, INTERNAL_ERROR, "Server is shutting down", nullptr, 0);
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
	struct mcp *mcp = malloc(sizeof *mcp);
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
			.on_cancel = mcp_on_cancel
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
	list_init(&mcp->freeing);
	manual_loop_init(&mcp->man, st->manual, loop);
	list_init(&mcp->children);
	list_init(&mcp->spare.list);
	list_init(&mcp->jobs.list);
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

	/* The transport has cancelled every stream by now, so only requests
	 * still freeing their message are left, and the man fetches. */
	while (!list_is_empty(&mcp->freeing))
		mcp_req_end(container_of(mcp->freeing.next, struct mcp_req, hook));
	manual_loop_fini(&mcp->man);
	child_destroy_all(&mcp->children);
	while (mcp_relieve(mcp, BUF_CRITICAL, SIZE_MAX))
		;
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
	return done;
}

void
mcp_drain (struct mcp *mcp)
{
	mcp->draining = true;
	loop_ready_bulk(mcp->loop, &mcp->drain);
}

/* The ask tool's definition; with @p notes, with the thread argument. */
static struct json_object *
mcp_tool_definition (struct mcp_tool const *tool,
                     bool                   notes)
{
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
	if (notes && jb_set(props, "thread", jb_object(
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
		"name", json_object_new_string(tool->name),
		"title", json_object_new_string("Ask AI"),
		"description", json_object_new_string(tool->description),
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
mcp_man_definition (void)
{
	static_assert(MANUAL_MAX == 2 << 20, "the description says 2 MiB");
	return jb_object(
		"name", json_object_new_string("man"),
		"title", json_object_new_string("Manual page"),
		"description", json_object_new_string(
			"Read one of the server's Unix manual pages, as `man [section] name` "
			"shows it, in plain text; a long page is cut, and offset reads on "
			"past the cut. Use it to check the exact interface of a system "
			"call, library function, command, file format or convention. Pages "
			"are kept once rendered, so reading one again is cheap."),
		"inputSchema", jb_object(
			"type", json_object_new_string("object"),
			"properties", jb_object(
				"name", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"The page's name, such as printf, pthread_create, epoll "
						"or bash."),
					nullptr),
				"section", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"Optional section: 1 commands, 2 system calls, 3 library "
						"functions, 3p POSIX, 4 devices, 5 file formats, 7 "
						"overviews and conventions, 8 administration. Without "
						"it, the first page man finds in its usual order."),
					nullptr),
				"offset", jb_object(
					"type", json_object_new_string("integer"),
					"minimum", json_object_new_int(0),
					"description", json_object_new_string(
						"The byte of the page to start from, to read on past "
						"where it was cut."),
					nullptr),
				nullptr),
			"required", jb_list(json_object_new_string("name")),
			"additionalProperties", json_object_new_boolean(0),
			nullptr),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(1),
			"openWorldHint", json_object_new_boolean(0),
			nullptr),
		nullptr);
}

static struct json_object *
mcp_task_definition (void)
{
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
mcp_notes_definition (void)
{
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
mcp_compact_definition (void)
{
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

/* @p def, an MCP tool's definition, as chat completions' tools have one;
 * nullptr if out of memory. */
static struct json_object *
mcp_lend (struct json_object *def)
{
	return jb_object(
		"type", json_object_new_string("function"),
		"function", jb_object(
			"name", json_object_get(jb_get(def, "name")),
			"description", json_object_get(jb_get(def, "description")),
			"parameters", json_object_get(jb_get(def, "inputSchema")),
			nullptr),
		nullptr);
}

/* Serialize @p val into @p text, then free it; false if out of memory. */
static bool
mcp_serialize (struct dstr        *text,
               struct json_object *val)
{
	size_t len = 0;
	char const *json = val ? json_object_to_json_string_length(val, JB_WIRE, &len)
	                       : nullptr;
	bool ok = json && dstr_cat_n(text, json, len);
	json_object_put(val);
	return ok;
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

struct mcp_static *
mcp_static_create (struct config const *cfg,
                   struct manual       *manual,
                   struct task_pool    *tasks,
                   struct git          *git,
                   struct notes        *notes,
                   int                 *e)
{
	struct mcp_tool const *tool = &cfg->tool;
	bool lends = cfg->flags & CFG_UPSTREAM_TOOLS && ai_format(cfg->ep.protocol);
	struct mcp_static *st = malloc(sizeof *st);
	if (!st) {
		*e = ENOMEM;
		return nullptr;
	}
	*st = (struct mcp_static){
		.ep = &cfg->ep,
		.tool = tool->name,
		.discover = DSTR_INIT,
		.init_tail = DSTR_INIT,
		.modern_tail = DSTR_INIT,
		.versions = DSTR_INIT,
		.manual = manual,
		.tasks = tasks,
		.git = git,
		.notes = lends ? notes : nullptr,
		.seed = loop_clock(),
		.lim = cfg->mcp,
		.turns = cfg->turns
	};
	/* A zeroed dstr is no empty string (dstr.h). */
	for (size_t i = 0; i < ARRAY_SIZE(st->list); ++i)
		st->list[i][0] = st->list[i][1] = (struct dstr)DSTR_INIT;
	for (size_t i = 0; i < ARRAY_SIZE(st->lent); ++i)
		st->lent[i] = (struct dstr)DSTR_INIT;
	(void)getrandom(&st->seed, sizeof st->seed, GRND_NONBLOCK);

	/* Built with json-c, which escapes as replies need, then kept as text. */
	struct json_object *info = jb_object("name", json_object_new_string("askai"),
	                                     "title", json_object_new_string("Ask AI"),
	                                     "version", json_object_new_string("0.1.0"),
	                                     nullptr);
	struct json_object *defs[MCP_TOOLS] = {
		mcp_tool_definition(tool, st->notes),
		manual ? mcp_man_definition() : nullptr,
		tasks ? mcp_task_definition() : nullptr,
		git ? git_definition(git) : nullptr
	};
	static_assert(MCP_TOOL_MAN == 1u << 1 && MCP_TOOL_TASK == 1u << 2 &&
	              MCP_TOOL_GIT == 1u << 3);
	struct json_object *list = json_object_new_array();
	bool ok = info && defs[0] && (!manual || defs[1]) && (!tasks || defs[2]) &&
	          (!git || defs[3]);
	for (size_t i = 0; ok && i < ARRAY_SIZE(versions); ++i)
		ok = !jb_push(list, json_object_new_string(versions[i]));
	/* What a turn may call, by whether the role may call man and git: those,
	 * the notes, and, with any of those, compact. */
	struct json_object *own[2] = {
		lends && st->notes ? mcp_notes_definition() : nullptr,
		lends ? mcp_compact_definition() : nullptr,
	};
	ok = ok && (!lends || (own[1] && (!st->notes || own[0])));
	for (size_t mask = 0; ok && lends && mask < ARRAY_SIZE(st->lent); ++mask) {
		struct json_object *lent[] = {
			mask & 1 ? defs[1] : nullptr, mask & 2 ? defs[3] : nullptr, own[0]
		};
		struct json_object *tools = json_object_new_array();
		bool some = false;
		for (size_t i = 0; tools && i < ARRAY_SIZE(lent); ++i) {
			some |= lent[i] != nullptr;
			if (lent[i] && jb_push(tools, mcp_lend(lent[i])))
				tools = nullptr;
		}
		ok = tools && (!some || !jb_push(tools, mcp_lend(own[1])));
		if (ok && some) {
			size_t len = 0;
			char const *json = json_object_to_json_string_length(tools, JB_WIRE, &len);
			ok = json && dstr_cat_n(&st->lent[mask], json, len);
		}
		json_object_put(tools);
	}
	json_object_put(own[0]);
	json_object_put(own[1]);
	/* A role lists the tools it may call, of those askai offers. */
	for (size_t mask = 0; ok && mask < ARRAY_SIZE(st->list); ++mask) {
		struct json_object *tools = json_object_new_array();
		for (size_t i = 0; tools && i < MCP_TOOLS; ++i)
			if (mask & 1u << i && defs[i] &&
			    jb_push(tools, json_object_get(defs[i])))
				tools = nullptr;
		ok = tools &&
		     mcp_serialize(&st->list[mask][0], jb_object("tools", json_object_get(tools),
		                                                 nullptr)) &&
		     mcp_serialize(&st->list[mask][1], mcp_modern_list(jb_object(
		             "tools", json_object_get(tools), nullptr), info));
		json_object_put(tools);
	}
	struct dstr info_text = DSTR_INIT;
	ok = ok && mcp_serialize(&info_text, json_object_get(info)) &&
	     mcp_serialize(&st->versions, json_object_get(list)) &&
	     mcp_serialize(&st->discover, mcp_modern_list(jb_object(
	             "supportedVersions", json_object_get(list),
	             "capabilities", jb_object("tools", jb_object(nullptr), nullptr),
	             nullptr), info)) &&
	     dstr_printf(&st->init_tail, "\",\"capabilities\":{\"tools\":{}},"
	                 "\"serverInfo\":%s}", dstr_get(&info_text)) &&
	     dstr_printf(&st->modern_tail, ",\"resultType\":\"complete\",\"_meta\":"
	                 "{\"%s\":%s}}", meta_server, dstr_get(&info_text));
	dstr_cleanup(&info_text);
	json_object_put(list);
	for (size_t i = 0; i < MCP_TOOLS; ++i)
		json_object_put(defs[i]);
	json_object_put(info);
	if (!ok) {
		mcp_static_destroy(&st);
		*e = ENOMEM;
	}
	return st;
}

void
mcp_static_destroy (struct mcp_static **p_static)
{
	struct mcp_static *st = *p_static;
	if (!st)
		return;
	*p_static = nullptr;
	for (size_t i = 0; i < ARRAY_SIZE(st->list); ++i)
		for (size_t j = 0; j < ARRAY_SIZE(st->list[i]); ++j)
			dstr_cleanup(&st->list[i][j]);
	for (size_t i = 0; i < ARRAY_SIZE(st->lent); ++i)
		dstr_cleanup(&st->lent[i]);
	dstr_cleanup(&st->discover);
	dstr_cleanup(&st->init_tail);
	dstr_cleanup(&st->modern_tail);
	dstr_cleanup(&st->versions);
	free(st);
}
