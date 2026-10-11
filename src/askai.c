/** @file askai: an MCP server whose tool asks another AI, and whose other,
 *  optional one reads the manual. */
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>

#include "ai.h"
#include "blocker.h"
#include "brainstem.h"
#include "buf.h"
#include "config.h"
#include "dstr.h"
#include "home.h"
#include "http.h"
#include "httpd.h"
#include "jb.h"
#include "log.h"
#include "loop.h"
#include "manual.h"
#include "mcp.h"
#include "metrics.h"
#include "util.h"

static struct {
	char const *name;
	char const *url;      /* gemini: the model and ":streamGenerateContent"
	                         follow */
	char const *auth;     /* header line prefix for the API key */
	char const *keys[2];  /* variables tried for the provider's own endpoint */
} const protocols[] = {
	[AI_OPENAI] = {
		"openai", "https://api.openai.com/v1/chat/completions",
		"Authorization: Bearer ", { "OPENAI_API_KEY" }
	},
	[AI_RESPONSES] = {
		"responses", "https://api.openai.com/v1/responses",
		"Authorization: Bearer ", { "OPENAI_API_KEY" }
	},
	[AI_ANTHROPIC] = {
		"anthropic", "https://api.anthropic.com/v1/messages",
		"x-api-key: ", { "ANTHROPIC_API_KEY" }
	},
	[AI_GEMINI] = {
		"gemini", "https://generativelanguage.googleapis.com/v1beta/models/",
		"x-goog-api-key: ", { "GEMINI_API_KEY", "GOOGLE_API_KEY" }
	},
};

static void
usage (FILE       *out,
       char const *argv0)
{
	fprintf(out,
	        "Usage: %s [options]\n"
	        "MCP server (Streamable HTTP) with a tool that asks another AI.\n"
	        "\n"
	        "Upstream AI:\n"
	        "  -p, --protocol NAME       openai (chat completions), responses,\n"
	        "                            anthropic or gemini (default openai)\n"
	        "  -u, --url URL             endpoint URL (default: the provider's API)\n"
	        "  -m, --model NAME          model to ask\n"
	        "  -k, --key-env VAR         environment variable holding the API key;\n"
	        "                            without -u the provider's usual one is used\n"
	        "  -H, --header LINE         extra request header (repeatable)\n"
	        "  -s, --system TEXT         default system prompt\n"
	        "  -t, --temperature N       sampling temperature\n"
	        "  -n, --max-tokens N        output token limit\n"
	        "  -x, --extra JSON          object deep-merged into each request body;\n"
	        "                            null members delete keys\n"
	        "  -T, --timeout SECONDS     a tool call's wait for an upstream slot, and\n"
	        "                            then its upstream call's (default 600,\n"
	        "                            0 = none)\n"
	        "      --chunk-timeout SECONDS\n"
	        "                            longest pause in an answer once it has\n"
	        "                            begun (default 60, 0 = none)\n"
	        "      --slots N             upstream calls at once (default: what\n"
	        "                            llama-server's /props says, else 4)\n"
	        "      --max-calls N         tool calls under way at most (default 256),\n"
	        "                            8 of them queued per session or client\n"
	        "                            address; a tool call past those is answered\n"
	        "                            at once as an error that says to try again\n"
	        "\n"
	        "MCP server:\n"
	        "  -l, --listen ADDR         [host:]port (default 127.0.0.1:8090)\n"
	        "  -o, --allow-origin URL    allow a non-loopback browser Origin\n"
	        "                            (repeatable; '*' allows any)\n"
	        "      --tool-name NAME      tool name (default ask_ai)\n"
	        "      --tool-description TEXT\n"
	        "      --man                 also offer the tool man, which gives a page\n"
	        "                            of the manual as man(1) renders it, in\n"
	        "                            plain UTF-8 text; each page is rendered\n"
	        "                            once and kept\n"
	        "      --test-man-recheck SECONDS\n"
	        "                            test aid: how long man's finding of a\n"
	        "                            page's file holds (default 600)\n"
	        "  -v, --verbose             log every tool call\n"
	        "      --loops N             event loops that serve clients (default 0:\n"
	        "                            one fewer than the CPUs askai may use, at\n"
	        "                            least 1, at most 255)\n"
	        "      --grace SECONDS       to finish what is under way on SIGINT,\n"
	        "                            SIGTERM or SIGHUP (default 5)\n"
	        "      --watchdog MS         log a turn of an event loop longer than\n"
	        "                            this (default 20, 0 = none)\n"
	        "      --test-stall MS       test aid: SIGUSR2 holds each event loop in\n"
	        "                            one turn this long\n"
	        "      --memory-high BYTES   resident memory from which memory pressure\n"
	        "                            is high (default: a sixteenth of RAM, or of\n"
	        "                            the lowest cgroup limit, at least 256M)\n"
	        "      --test-pressure FILE  test aid: memory pressure is at least the\n"
	        "                            level FILE holds, 0 (none), 1 (high) or 2\n"
	        "                            (critical), read each second\n"
	        "      --test-fifo           test aid: tool calls wait for the upstream\n"
	        "                            in one queue, first in first out\n"
	        "  -h, --help                show this help\n"
	        "\n", argv0);
	fprintf(out,
	        "Limits on clients (BYTES may end in K, M or G; 0 = none where noted):\n"
	        "      --max-head BYTES      request line and headers (default 16K,\n"
	        "                            at most 65535)\n"
	        "      --max-fields N        header fields (default 64)\n"
	        "      --head-timeout SECONDS\n"
	        "                            time to send those in (default 10, 0 = none)\n"
	        "      --max-body BYTES      request body (default 1M)\n"
	        "      --min-body-rate RATE  bytes per second a body must average from\n"
	        "                            its start (default 4K, 0 = none),\n"
	        "      --body-grace SECONDS  once this has passed (default 5)\n"
	        "      --body-budget BYTES   bodies held for all clients (default 256M)\n"
	        "      --client-budget BYTES\n"
	        "                            of those, held for one client address\n"
	        "                            (default: a quarter, at least --max-body)\n"
	        "      --max-depth N         JSON nesting in a request (default 32)\n"
	        "      --max-values N        JSON values in a request, keys included\n"
	        "                            (default 1024; json-c takes up to 0.7 KiB\n"
	        "                            for each while it holds them)\n"
	        "      --max-members N       members of one JSON object in a request\n"
	        "                            (default 1024)\n"
	        "      --sse-keepalive SECONDS\n"
	        "                            between comments that keep an event stream\n"
	        "                            open (default 15, 0 = none)\n"
	        "      --max-client-conns N  connections per client address\n"
	        "                            (default 256, 0 = none)\n"
	        "      --idle-timeout SECONDS\n"
	        "                            keep-alive idle time (default 30, 0 = none)\n"
	        "      --output-pause BYTES  pending output that holds back a connection's\n"
	        "                            requests until it drains (default 256K)\n"
	        "      --max-output BYTES    close a connection whose pending output a\n"
	        "                            response would take past this, unless it\n"
	        "                            is all that is pending (default 1M)\n"
	        "      --send-timeout SECONDS\n"
	        "                            longest stall of pending output\n"
	        "                            (default 30, 0 = none)\n"
	        "\n"
	        "If ASKAI_TOKEN is set, clients must send 'Authorization: Bearer <it>'.\n"
	        "GET /metrics gives metrics in Prometheus's text format to clients\n"
	        "on loopback that name it as localhost, 127.0.0.1 or [::1], or with\n"
	        "the token when one is set.\n"
	        "SIGUSR1 prints each event loop's turn and step times, its timers'\n"
	        "lateness and the most work a step did, since the previous SIGUSR1,\n"
	        "to stderr, in lines that name it: loop1 to loopN serve clients,\n"
	        "home accepts them and runs the upstream calls.  The first SIGINT,\n"
	        "SIGTERM or SIGHUP, unless askai started with it ignored, ends tool\n"
	        "calls with an error, finishes other requests and exits 0; a second,\n"
	        "or a grace period run out, or an event loop stuck, exits at once\n"
	        "with %d.\n"
	        "Invalid options or environment exit 2, and a failure to start or\n"
	        "to serve 1.\n",
	        BRAINSTEM_FORCED);
}

static bool
parse_long (char const *str,
            long        min,
            long        max,
            long       *value)
{
	char *end;
	errno = 0;
	long n = strtol(str, &end, 10);
	if (errno || end == str || *end || n < min || n > max)
		return false;
	*value = n;
	return true;
}

static bool
parse_u32 (char const *str,
           uint32_t    min,
           uint32_t    max,
           uint32_t   *value)
{
	long n;
	if (!parse_long(str, min, max, &n))
		return false;
	*value = (uint32_t)n;
	return true;
}

/* A byte count, which may end in K, M or G for KiB, MiB or GiB. */
static bool
parse_size (char const *str,
            size_t      min,
            size_t      max,
            size_t     *value)
{
	static char const units[] = "KMG";
	char *end;
	errno = 0;
	unsigned long long n = strtoull(str, &end, 10);
	char const *unit = *end ? strchr(units, *end) : nullptr;
	if (errno || *str < '0' || *str > '9' || (*end && (!unit || end[1])))
		return false;
	unsigned shift = unit ? 10 * (unsigned)(unit - units + 1) : 0;
	if (n > max >> shift || n << shift < min)
		return false;
	*value = (size_t)n << shift;
	return true;
}

/* Whether a header line named like @p prefix's name is among @p lines. */
static bool
has_header (char const *const *lines,
            char const        *prefix)
{
	size_t n = strcspn(prefix, ":");
	for (; *lines; ++lines)
		if (!strncasecmp(*lines, prefix, n) && (*lines)[n] == ':')
			return true;
	return false;
}

static bool
valid_header (char const *line)
{
	size_t n = strcspn(line, ": \t\r\n");
	return n && line[n] == ':' && !strpbrk(line, "\r\n");
}

static bool
valid_tool_name (char const *name)
{
	size_t n = strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	                        "0123456789_.-");
	return n && n <= 128 && !name[n];
}

/* The phases of a relief of memory pressure. */
enum { RELIEVE_KEPT, RELIEVE_STASH, RELIEVE_END, RELIEVED };

/* glibc's tuning (see serve()): blocks from this size up get mappings of
 * their own, and where nothing limits the address space, heaps keep this
 * much of their tops; and a free of this much trims the top of its arena's
 * heap. */
enum { MMAP_MIN = 32 << 20, TOP_PAD = 64 << 20, TOP_FREE = 64 << 10 };

/* The heaps' top pad: glibc's default, or TOP_PAD (see serve()). */
static int top_pad = 128 << 10;

/* Blocks of TOP_FREE, one from each loop's arena that relieved memory
 * pressure, chained through their first bytes, for the trim to free. */
static _Atomic(void *) tops;

/* Free @p block under a low threshold and pad (glibc), which gives back the
 * top of the arena it came from, as only such a free does.  The settings are
 * the process's, lowered for this free alone, and only one thread at a time
 * does it (brainstem_relieved()). */
static void
top_trim (void *block)
{
	mallopt(M_TOP_PAD, 0);
	mallopt(M_TRIM_THRESHOLD, 0);
	free(block);
	mallopt(M_TRIM_THRESHOLD, -1);
	mallopt(M_TOP_PAD, top_pad);
}

/* Whether the kernel commits memory strictly (vm.overcommit_memory 2). */
static bool
strict_commit (void)
{
	FILE *f = fopen("/proc/sys/vm/overcommit_memory", "re");
	int mode = f ? fgetc(f) : EOF;
	if (f)
		fclose(f);
	return mode == '2';
}

/* What a loop thread keeps for reuse, and its reliefs of memory pressure
 * (design section 5); first in each loop's app, which it stands for. */
struct keep {
	struct buf_stash    stash;
	struct loop_step    relief;
	struct loop_step    trim;      /* the trim, on the last loop of a relief */
	struct loop_timer   age;       /* the windows of reuse */
	struct mcp         *mcp;       /* what a client loop keeps besides */
	struct httpd_home  *door;      /* what home keeps besides */
	struct home        *home;      /* and its scheduler */
	struct blockers    *blockers;  /* where the walk goes */
	void               *tops;      /* the trim's blocks yet to free */
	uint64_t            released;  /* the stash's count as the relief began */
	uint64_t            trim_ns;   /* the trim's so far */
	uint64_t            phase;     /* the relief's, widened over the padding */
};

/* The trim's walk of the arenas, on a blocker, for the keep of the loop that
 * trims, and its time, which the blocker writes: here, as one trim at a time
 * walks, and a blocker still busy at shutdown is left to the process's exit,
 * after the loops' keeps are gone. */
static struct {
	struct blocker_job  job;
	struct keep        *keep;
	uint64_t            ns;
} walker;

/* What runs on home: the jobs (src/home.h), and the door, which accepts the
 * clients and places them on the client loops (src/httpd.h). */
struct home_app {
	struct keep               keep;
	struct http_metrics       http;  /* here, as the client loops render it */
	struct home_metrics       jobs;  /* likewise */
	struct ai_endpoint const *ep;
	struct httpd_cfg const   *hcfg;
	struct httpd_hub         *hub;
	struct metrics_src       *metrics;
	struct config const      *cfg;
	struct loop              *loop;  /* home's: the client loops' jobs go there */
};

/* What runs on a client loop: MCP over HTTP. */
struct app {
	struct keep              keep;
	struct metrics_mcp       counts;  /* MCP's, which any loop renders */
	struct home_app const   *home;
	struct mcp_static const *st;
	struct httpd_cfg const  *hcfg;
	struct httpd_hub        *hub;
	struct metrics_src      *metrics;
	struct httpd            *httpd;
};

/*
 * Relieve memory pressure, in bulk steps (design section 5): free the
 * requests and jobs, or connections, kept for reuse, then the stash's
 * storage, as much of each a step as its share of release work allows and
 * as much in all as the level says; then leave a block for the trim, which
 * the last loop of all to end its share runs, in a step of its own, as that
 * one call cannot be split.  The share ends early if the pressure clears.
 */
static void
relief_step (struct loop      *loop,
             struct loop_step *step)
{
	struct keep *keep = container_of(step, struct keep, relief);
	enum buf_pressure level = atomic_load_explicit(&buf_pressure, memory_order_relaxed);
	size_t room = loop_room(loop, LOOP_RELEASE);
	size_t n;
	switch (level || keep->phase == RELIEVED ? keep->phase : RELIEVE_END) {
	case RELIEVE_KEPT:
		n = keep->mcp ? mcp_relieve(keep->mcp, level, room)
		  : keep->door ? httpd_home_relieve(keep->door, level, room) : 0;
		if (!n && keep->home)
			n = home_relieve(keep->home, level, room);
		break;
	case RELIEVE_STASH:
		n = buf_stash_release(&keep->stash, level, room);
		break;
	case RELIEVE_END: {
		keep->phase = RELIEVED;
		void *block = malloc(TOP_FREE);
		void *head = atomic_load_explicit(&tops, memory_order_relaxed);
		if (block) {
			do memcpy(block, &head, sizeof head);
			while (!atomic_compare_exchange_weak(&tops, &head, block));
		}
		if (brainstem_relieved(hist_get(&keep->stash.metrics.released) -
		                       keep->released)) {
			keep->tops = atomic_exchange(&tops, nullptr);
			keep->trim_ns = 0;
			loop_ready_bulk(loop, &keep->trim);
		}
		return;
	}
	default:
		return;
	}
	loop_work(loop, LOOP_RELEASE, n);
	keep->phase += !n;
	loop_ready_bulk(loop, step);
}

/* Walk every arena with malloc_trim() (glibc), which gives back the free
 * pages within their heaps and the top of the main arena's, whatever the
 * settings: on a blocker, as it takes each arena's lock in turn, so that a
 * loop that allocates meanwhile waits only for its own arena's part, where
 * on a loop the walk of them all would be one step. */
static void
walk (struct blocker_job *job)
{
	(void)job;
	uint64_t start = loop_clock();
	malloc_trim(0);
	walker.ns = loop_clock() - start;
}

/* The trim is over: count it, and let the next relief begin. */
static void
trim_end (struct keep *keep)
{
	struct buf_stash_metrics *m = &keep->stash.metrics;
	hist_add(&m->trims, 1);
	hist_add(&m->trim_ns, keep->trim_ns);
	uint64_t bytes = brainstem_trimmed();
	/* A relief that found little to do is routine while pressure lasts. */
	LOG(bytes || keep->trim_ns >= 1'000'000 ? LOG_INFO : LOG_VERBOSE, "memory "
	    "pressure relieved: %.1f MiB of buffers released, then a trim of %.1f ms",
	    (double)bytes / (1 << 20), (double)keep->trim_ns / 1e6);
}

static void
walk_done (struct loop *loop,
           struct msg  *msg)
{
	(void)msg;
	if (loop->exit)
		return;
	walker.keep->trim_ns += walker.job.error ? 0 : walker.ns;
	trim_end(walker.keep);
}

/* The last loop of a relief trims the allocator for every loop: a bulk step
 * for each loop's block, which gives back that arena's top, then the walk of
 * them all on a blocker, or here if none takes it.  The pressure cleared,
 * the blocks are only freed. */
static void
trim_step (struct loop      *loop,
           struct loop_step *step)
{
	struct keep *keep = container_of(step, struct keep, trim);
	bool on = atomic_load_explicit(&buf_pressure, memory_order_relaxed);
	void *block = keep->tops;
	if (block) {
		memcpy(&keep->tops, block, sizeof keep->tops);
		uint64_t start = loop_clock();
		if (on)
			top_trim(block);
		else
			free(block);
		keep->trim_ns += loop_clock() - start;
		loop_ready_bulk(loop, step);
	} else if (!on) {
		(void)brainstem_trimmed();
	} else {
		walker.job = (struct blocker_job){
			.done.fn = walk_done,
			.work = walk,
			.loop = loop
		};
		walker.keep = keep;
		if (blockers_submit(keep->blockers, &walker.job)) {
			walk(&walker.job);
			keep->trim_ns += walker.ns;
			trim_end(keep);
		}
	}
}

/* Begin a window of reuse, over which a relief at high pressure finds what
 * went unused: as long as the brainstem waits between reliefs. */
static void
keep_age (struct loop       *loop,
          struct loop_timer *timer)
{
	struct keep *keep = container_of(timer, struct keep, age);
	buf_stash_age(&keep->stash);
	if (keep->mcp)
		mcp_age(keep->mcp);
	if (keep->door)
		httpd_home_age(keep->door);
	if (keep->home)
		home_age(keep->home);
	(void)loop_timer_set(loop, timer, loop->now + BRAINSTEM_RELIEF_MS * 1'000'000ULL);
}

/* The loop's app, whose keep comes first. */
static void
keep_relieve (void        *ctx,
              struct loop *loop)
{
	struct keep *keep = ctx;
	keep->phase = RELIEVE_KEPT;
	keep->released = hist_get(&keep->stash.metrics.released);
	hist_add(&keep->stash.metrics.reliefs, 1);
	loop_ready_bulk(loop, &keep->relief);
}

/* Give the calling thread @p keep's stash, and begin its windows of reuse;
 * a trim walks the arenas on @p blockers. */
static int
keep_start (struct keep     *keep,
            struct loop     *loop,
            struct blockers *blockers)
{
	keep->blockers = blockers;
	keep->relief.fn = relief_step;
	keep->trim.fn = trim_step;
	keep->age.fn = keep_age;
	keep->phase = RELIEVED;
	buf_stash_attach(&keep->stash);
	return loop_timer_set(loop, &keep->age,
	                      loop->now + BRAINSTEM_RELIEF_MS * 1'000'000ULL);
}

/* Give back all that @p keep's stash keeps, and the blocks of a trim cut
 * short, and leave the thread without. */
static void
keep_stop (struct keep *keep,
           struct loop *loop)
{
	for (void *block = keep->tops; block; block = keep->tops) {
		memcpy(&keep->tops, block, sizeof keep->tops);
		free(block);
	}
	loop_unready(&keep->relief);
	loop_unready(&keep->trim);
	loop_timer_stop(loop, &keep->age);
	(void)buf_stash_release(&keep->stash, BUF_CRITICAL, SIZE_MAX);
	buf_stash_attach(nullptr);
}

static int
home_app_start (void            *ctx,
                struct loop     *loop,
                struct blockers *blockers)
{
	struct home_app *app = ctx;
	struct metrics_src *metrics = app->metrics;
	struct metrics_loop *mine = &metrics->loops[metrics->n_loops - 1];
	app->loop = loop;
	snprintf(mine->name, sizeof mine->name, "home");
	mine->loop = loop->metrics;
	mine->stash = &app->keep.stash.metrics;
	metrics->blockers = blockers_metrics(blockers);
	int e = keep_start(&app->keep, loop, blockers);
	if (!e)
		e = httpd_home_init(&app->keep.door, loop, app->hub, app->hcfg,
		                    &app->jobs.in_flight);
	if (!e)
		e = home_init(&app->keep.home, loop, blockers, app->keep.door, app->ep,
		              app->cfg, &app->jobs, &app->http);
	if (e)
		LOG(LOG_ERROR, "cannot start home: %s", strerror(e));
	return e;
}

static void
home_app_open (void        *ctx,
               struct loop *loop)
{
	(void)loop;
	httpd_home_open(((struct home_app *)ctx)->keep.door);
}

static void
home_app_drain (void        *ctx,
                struct loop *loop,
                uint64_t     deadline)
{
	(void)loop;
	(void)deadline;
	struct home_app *app = ctx;
	httpd_home_drain(app->keep.door);
	home_drain(app->keep.home);
}

static void
home_app_alone (void        *ctx,
                struct loop *loop)
{
	(void)loop;
	home_alone(((struct home_app *)ctx)->keep.home);
}

static void
home_app_stop (void        *ctx,
               struct loop *loop)
{
	struct home_app *app = ctx;
	home_fini(&app->keep.home);
	httpd_home_fini(&app->keep.door);
	keep_stop(&app->keep, loop);
}

static int
app_start (void            *ctx,
           struct loop     *loop,
           struct blockers *blockers)
{
	struct app *app = ctx;
	struct metrics_loop *mine = &app->metrics->loops[loop->id - 1];
	snprintf(mine->name, sizeof mine->name, "%" PRIu64, loop->id);
	mine->loop = loop->metrics;
	mine->stash = &app->keep.stash.metrics;
	mine->kept = &app->counts.kept;
	mine->calls = &app->counts.calls;
	mine->methods = app->counts.methods;
	mine->conns = httpd_hub_metrics(app->hub, loop->id - 1);
	int e = keep_start(&app->keep, loop, blockers);
	if (!e)
		e = mcp_init(&app->keep.mcp, loop, app->home->loop, app->st, app->metrics,
		             &app->counts);
	if (e)
		LOG(LOG_ERROR, "cannot start client loop %" PRIu64 ": %s", loop->id,
		    strerror(e));
	else
		e = httpd_init(&app->httpd, loop, app->hub, loop->id - 1,
		               mcp_app(app->keep.mcp), app->hcfg);
	return e;
}

static void
app_drain (void        *ctx,
           struct loop *loop,
           uint64_t     deadline)
{
	(void)loop;
	struct app *app = ctx;
	httpd_drain(app->httpd, deadline);
	mcp_drain(app->keep.mcp);
}

static void
app_stop (void        *ctx,
          struct loop *loop)
{
	struct app *app = ctx;
	httpd_fini(&app->httpd);
	mcp_fini(&app->keep.mcp);
	keep_stop(&app->keep, loop);
}

/* Serve until shut down, as @p cfg says; returns the exit status. */
static int
serve (struct ai_endpoint const *ep,
       struct mcp_tool const    *tool,
       struct mcp_limits const  *mlim,
       struct httpd_cfg const   *hcfg,
       struct brainstem_cfg     *bcfg,
       struct config const      *cfg)
{
	/* Keep what is freed for reuse, and give memory back to the system only
	 * under memory pressure (design section 5).  glibc would otherwise map
	 * and unmap each block over 128 KiB of its own, give back the top of a
	 * heap as it is freed and a thread's heap once all of it is, and grow a
	 * thread's heap a page and an mprotect() at a time.  Blocks of up to
	 * 32 MiB, the most the threshold takes and the largest stash class, come
	 * from the heaps.  A top pad as large as a heap keeps a thread's heaps,
	 * and makes each writable whole as glibc creates it; as that counts
	 * against RLIMIT_DATA, RLIMIT_AS and a strict commit limit, under any of
	 * those the pad stays glibc's. */
	mallopt(M_MMAP_THRESHOLD, MMAP_MIN);
	mallopt(M_TRIM_THRESHOLD, -1);
	struct rlimit data;
	struct rlimit space;
	if (!getrlimit(RLIMIT_DATA, &data) && data.rlim_cur == RLIM_INFINITY &&
	    !getrlimit(RLIMIT_AS, &space) && space.rlim_cur == RLIM_INFINITY &&
	    !strict_commit())
		top_pad = TOP_PAD;
	mallopt(M_TOP_PAD, top_pad);

	/* Only descriptors limit the connections: take all the hard limit allows. */
	struct rlimit nofile;
	if (!getrlimit(RLIMIT_NOFILE, &nofile) && nofile.rlim_cur < nofile.rlim_max) {
		nofile.rlim_cur = nofile.rlim_max;
		(void)setrlimit(RLIMIT_NOFILE, &nofile);
	}

	/* What every loop shares is made before any starts: the immutable, and
	 * what home's door shares with the client loops' servers.  Each loop
	 * sets its metrics as it starts, and home opens its door only once all
	 * have, so that no request renders them before. */
	size_t n = bcfg->loops;
	struct manual *manual = nullptr;
	struct mcp_static *st = nullptr;
	struct httpd_hub *hub = nullptr;
	struct app *apps = calloc(n, sizeof *apps);
	struct metrics_loop *loops = calloc(n + 1, sizeof *loops);
	int e = !apps || !loops ? ENOMEM : 0;
	if (!e && cfg->flags & CFG_MAN)
		e = manual_new(&manual, cfg->man_recheck);
	if (!e)
		e = mcp_static_new(&st, ep, tool, manual, mlim);
	if (!e)
		e = httpd_hub_new(&hub, n);
	struct metrics_src metrics = {
		.loops = loops,
		.man = manual ? manual_metrics(manual) : nullptr,
		.n_loops = n + 1
	};
	struct home_app home = {
		.ep = ep,
		.hcfg = hcfg,
		.hub = hub,
		.metrics = &metrics,
		.cfg = cfg
	};
	metrics.http = &home.http;
	metrics.jobs = &home.jobs;
	for (size_t i = 0; !e && i < n; ++i) {
		struct app *app = &apps[i];
		buf_stash_init(&app->keep.stash);
		metrics_mcp_init(&app->counts);
		app->home = &home;
		app->st = st;
		app->hcfg = hcfg;
		app->hub = hub;
		app->metrics = &metrics;
	}
	bcfg->home = (struct brainstem_app){
		.ctx = &home,
		.start = home_app_start,
		.open = home_app_open,
		.drain = home_app_drain,
		.relieve = keep_relieve,
		.alone = home_app_alone,
		.stop = home_app_stop
	};
	bcfg->client = (struct brainstem_app){
		.ctx = apps,
		.stride = sizeof *apps,
		.start = app_start,
		.drain = app_drain,
		.relieve = keep_relieve,
		.stop = app_stop
	};
	int status = e ? 1 : brainstem_run(bcfg);
	if (e)
		fprintf(stderr, "askai: %s\n", strerror(e));
	httpd_hub_free(&hub);
	mcp_static_free(&st);
	manual_free(&manual);
	free(loops);
	free(apps);
	return status;
}

int
main (int   argc,
      char *argv[])
{
	enum {
		OPT_TOOL_NAME = 0x100, OPT_TOOL_DESCRIPTION, OPT_MAX_HEAD, OPT_MAX_FIELDS,
		OPT_HEAD_TIMEOUT, OPT_MAX_BODY, OPT_MIN_BODY_RATE, OPT_BODY_GRACE,
		OPT_BODY_BUDGET, OPT_CLIENT_BUDGET, OPT_MAX_DEPTH, OPT_MAX_VALUES,
		OPT_MAX_MEMBERS, OPT_SSE_KEEPALIVE, OPT_MAX_CLIENT_CONNS,
		OPT_IDLE_TIMEOUT, OPT_OUTPUT_PAUSE, OPT_MAX_OUTPUT, OPT_SEND_TIMEOUT,
		OPT_GRACE, OPT_WATCHDOG, OPT_TEST_STALL, OPT_MEMORY_HIGH, OPT_TEST_PRESSURE,
		OPT_MAX_CALLS, OPT_LOOPS, OPT_SLOTS, OPT_CHUNK_TIMEOUT, OPT_TEST_FIFO,
		OPT_MAN, OPT_TEST_MAN_RECHECK
	};
	static struct option const options[] = {
		{ "protocol",         required_argument, nullptr, 'p' },
		{ "url",              required_argument, nullptr, 'u' },
		{ "model",            required_argument, nullptr, 'm' },
		{ "key-env",          required_argument, nullptr, 'k' },
		{ "header",           required_argument, nullptr, 'H' },
		{ "system",           required_argument, nullptr, 's' },
		{ "temperature",      required_argument, nullptr, 't' },
		{ "max-tokens",       required_argument, nullptr, 'n' },
		{ "extra",            required_argument, nullptr, 'x' },
		{ "timeout",          required_argument, nullptr, 'T' },
		{ "chunk-timeout",    required_argument, nullptr, OPT_CHUNK_TIMEOUT },
		{ "slots",            required_argument, nullptr, OPT_SLOTS },
		{ "max-calls",        required_argument, nullptr, OPT_MAX_CALLS },
		{ "listen",           required_argument, nullptr, 'l' },
		{ "allow-origin",     required_argument, nullptr, 'o' },
		{ "tool-name",        required_argument, nullptr, OPT_TOOL_NAME },
		{ "tool-description", required_argument, nullptr, OPT_TOOL_DESCRIPTION },
		{ "man",              no_argument,       nullptr, OPT_MAN },
		{ "test-man-recheck", required_argument, nullptr, OPT_TEST_MAN_RECHECK },
		{ "max-head",         required_argument, nullptr, OPT_MAX_HEAD },
		{ "max-fields",       required_argument, nullptr, OPT_MAX_FIELDS },
		{ "head-timeout",     required_argument, nullptr, OPT_HEAD_TIMEOUT },
		{ "max-body",         required_argument, nullptr, OPT_MAX_BODY },
		{ "min-body-rate",    required_argument, nullptr, OPT_MIN_BODY_RATE },
		{ "body-grace",       required_argument, nullptr, OPT_BODY_GRACE },
		{ "body-budget",      required_argument, nullptr, OPT_BODY_BUDGET },
		{ "client-budget",    required_argument, nullptr, OPT_CLIENT_BUDGET },
		{ "max-depth",        required_argument, nullptr, OPT_MAX_DEPTH },
		{ "max-values",       required_argument, nullptr, OPT_MAX_VALUES },
		{ "max-members",      required_argument, nullptr, OPT_MAX_MEMBERS },
		{ "sse-keepalive",    required_argument, nullptr, OPT_SSE_KEEPALIVE },
		{ "max-client-conns", required_argument, nullptr, OPT_MAX_CLIENT_CONNS },
		{ "idle-timeout",     required_argument, nullptr, OPT_IDLE_TIMEOUT },
		{ "output-pause",     required_argument, nullptr, OPT_OUTPUT_PAUSE },
		{ "max-output",       required_argument, nullptr, OPT_MAX_OUTPUT },
		{ "send-timeout",     required_argument, nullptr, OPT_SEND_TIMEOUT },
		{ "loops",            required_argument, nullptr, OPT_LOOPS },
		{ "grace",            required_argument, nullptr, OPT_GRACE },
		{ "watchdog",         required_argument, nullptr, OPT_WATCHDOG },
		{ "test-stall",       required_argument, nullptr, OPT_TEST_STALL },
		{ "memory-high",      required_argument, nullptr, OPT_MEMORY_HIGH },
		{ "test-pressure",    required_argument, nullptr, OPT_TEST_PRESSURE },
		{ "test-fifo",        no_argument,       nullptr, OPT_TEST_FIFO },
		{ "verbose",          no_argument,       nullptr, 'v' },
		{ "help",             no_argument,       nullptr, 'h' },
		{ nullptr, 0, nullptr, 0 }
	};

	struct ai_endpoint ep = {
		.timeout_ms = 600'000,
		.chunk_ms = 60'000,
		.protocol = AI_OPENAI
	};
	struct mcp_tool tool = { .name = "ask_ai" };
	struct httpd_cfg hcfg = {
		.limits = httpd_limits(),
		.listen = "127.0.0.1:8090"
	};
	struct httpd_limits *lim = &hcfg.limits;
	struct mcp_limits mlim = mcp_limits();
	struct brainstem_cfg bcfg = {
		.grace_ms = 5'000,
		.watchdog_ms = 20
	};
	dstr_auto url = DSTR_INIT;
	dstr_auto auth = DSTR_INIT;
	dstr_auto description = DSTR_INIT;
	struct config cfg = {
		.calls = 256,
		.man_recheck = 600
	};
	char const *key_env = nullptr;
	uint32_t loops = 0;
	bool verbose = false;
	int status = 2;

	/* Repeatable options are bounded by argc; headers get 3 spare slots. */
	char const **headers = calloc((size_t)argc + 3, sizeof *headers);
	char const **origins = calloc((size_t)argc, sizeof *origins);
	size_t n_headers = 0;
	size_t n_origins = 0;
	if (!headers || !origins) {
		fputs("askai: out of memory\n", stderr);
		goto out;
	}

	int idx = 0;
	for (int opt; (opt = getopt_long(argc, argv, "p:u:m:k:H:s:t:n:x:T:l:o:vh",
	                                 options, &idx)) >= 0; ) {
		size_t size = 0;
		bool ok = true;
		switch (opt) {
		case 'p': {
			size_t i = 0;
			while (i < ARRAY_SIZE(protocols) && strcmp(protocols[i].name, optarg))
				++i;
			if (i == ARRAY_SIZE(protocols)) {
				fprintf(stderr, "askai: unknown protocol: %s\n", optarg);
				goto out;
			}
			ep.protocol = (enum ai_protocol)i;
			break;
		}
		case 'u':
			if (strncmp(optarg, "http://", sizeof "http://" - 1) &&
			    strncmp(optarg, "https://", sizeof "https://" - 1)) {
				fprintf(stderr, "askai: not an http(s) URL: %s\n", optarg);
				goto out;
			}
			ep.url = optarg;
			break;
		case 'm': ep.model = optarg; break;
		case 'k': key_env = optarg; break;
		case 's': ep.system = optarg; break;
		case 'l': hcfg.listen = optarg; break;
		case 'o': origins[n_origins++] = optarg; break;
		case 'v': verbose = true; break;
		case OPT_TOOL_DESCRIPTION: tool.description = optarg; break;
		case 'H':
			if (!valid_header(optarg)) {
				fprintf(stderr, "askai: invalid header line: %s\n", optarg);
				goto out;
			}
			headers[n_headers++] = optarg;
			break;
		case 't': {
			struct json_object *t = jb_parse(optarg, strlen(optarg),
			                                 JSON_TOKENER_STRICT, 0);
			if ((!json_object_is_type(t, json_type_double) &&
			     !json_object_is_type(t, json_type_int)) ||
			    json_object_get_double(t) < 0) {
				json_object_put(t);
				fprintf(stderr, "askai: invalid temperature: %s\n", optarg);
				goto out;
			}
			json_object_put(ep.temperature);
			ep.temperature = t;
			break;
		}
		case 'n':
			if (!parse_long(optarg, 1, INT32_MAX, &ep.max_tokens)) {
				fprintf(stderr, "askai: invalid max token count: %s\n", optarg);
				goto out;
			}
			break;
		case 'x': {
			struct json_object *x = jb_parse(optarg, strlen(optarg),
			                                 JSON_TOKENER_STRICT, 0);
			if (!json_object_is_type(x, json_type_object)) {
				json_object_put(x);
				fprintf(stderr, "askai: --extra needs a JSON object: %s\n", optarg);
				goto out;
			}
			json_object_put(ep.extra);
			ep.extra = x;
			break;
		}
		case 'T':
			if (!parse_long(optarg, 0, INT32_MAX / 1000, &ep.timeout_ms)) {
				fprintf(stderr, "askai: invalid timeout: %s\n", optarg);
				goto out;
			}
			ep.timeout_ms *= 1000;
			break;
		case OPT_TOOL_NAME:
			if (!valid_tool_name(optarg)) {
				fprintf(stderr, "askai: tool names are 1-128 of "
				        "[A-Za-z0-9_.-]: %s\n", optarg);
				goto out;
			}
			tool.name = optarg;
			break;
		case OPT_MAX_HEAD:
			ok = parse_size(optarg, 1, UINT16_MAX, &size);
			lim->head = (uint32_t)size;
			break;
		case OPT_MAX_FIELDS:
			ok = parse_u32(optarg, 0, UINT32_MAX, &lim->fields);
			break;
		case OPT_HEAD_TIMEOUT:
			ok = parse_u32(optarg, 0, UINT32_MAX, &lim->head_time);
			break;
		case OPT_MAX_BODY:
			ok = parse_size(optarg, 0, SIZE_MAX / 2, &lim->body);
			break;
		case OPT_MIN_BODY_RATE:
			ok = parse_size(optarg, 0, UINT32_MAX, &size);
			lim->body_rate = (uint32_t)size;
			break;
		case OPT_BODY_GRACE:
			ok = parse_u32(optarg, 0, UINT32_MAX, &lim->body_grace);
			break;
		case OPT_BODY_BUDGET:
			ok = parse_size(optarg, 0, SIZE_MAX / 2, &lim->bodies);
			break;
		case OPT_CLIENT_BUDGET:
			ok = parse_size(optarg, 1, SIZE_MAX / 2, &lim->client_bodies);
			break;
		case OPT_MAX_DEPTH:
			ok = parse_u32(optarg, 1, INT32_MAX - 1, &mlim.depth);
			break;
		case OPT_MAX_VALUES:
			ok = parse_u32(optarg, 1, UINT32_MAX, &mlim.values);
			break;
		case OPT_MAX_MEMBERS:
			ok = parse_u32(optarg, 1, UINT32_MAX - 1, &mlim.members);
			break;
		case OPT_SSE_KEEPALIVE:
			ok = parse_u32(optarg, 0, UINT32_MAX / 2, &mlim.keepalive);
			break;
		case OPT_MAX_CLIENT_CONNS:
			ok = parse_u32(optarg, 0, UINT32_MAX, &lim->clients);
			break;
		case OPT_IDLE_TIMEOUT:
			ok = parse_u32(optarg, 0, UINT32_MAX, &lim->idle_time);
			break;
		case OPT_OUTPUT_PAUSE:
			ok = parse_size(optarg, 1, SIZE_MAX / 2, &lim->out_pause);
			break;
		case OPT_MAX_OUTPUT:
			ok = parse_size(optarg, 1, SIZE_MAX / 2, &lim->out_max);
			break;
		case OPT_SEND_TIMEOUT:
			ok = parse_u32(optarg, 0, UINT32_MAX, &lim->write_time);
			break;
		case OPT_GRACE: {
			uint32_t seconds = 0;
			ok = parse_u32(optarg, 1, 3600, &seconds);
			bcfg.grace_ms = seconds * 1000;
			break;
		}
		case OPT_WATCHDOG:
			ok = parse_u32(optarg, 0, 60'000, &bcfg.watchdog_ms);
			break;
		case OPT_TEST_STALL: {
			uint32_t ms = 0;
			ok = parse_u32(optarg, 1, 3'600'000, &ms);
			bcfg.stall_ms = ms;
			break;
		}
		case OPT_MEMORY_HIGH:
			ok = parse_size(optarg, 1, SIZE_MAX / 2, &size);
			bcfg.memory_high = size;
			break;
		case OPT_TEST_PRESSURE:
			bcfg.pressure = optarg;
			break;
		case OPT_MAX_CALLS:
			ok = parse_u32(optarg, 1, 1'000'000, &cfg.calls);
			break;
		case OPT_SLOTS:
			ok = parse_u32(optarg, 1, UINT16_MAX, &ep.slots);
			break;
		case OPT_CHUNK_TIMEOUT:
			ok = parse_long(optarg, 0, INT32_MAX / 1000, &ep.chunk_ms);
			ep.chunk_ms *= 1000;
			break;
		case OPT_TEST_FIFO:
			cfg.flags |= CFG_TEST_FIFO;
			break;
		case OPT_MAN:
			cfg.flags |= CFG_MAN;
			break;
		case OPT_TEST_MAN_RECHECK:
			ok = parse_u32(optarg, 0, 86'400, &cfg.man_recheck);
			break;
		case OPT_LOOPS:
			/* As many as a handle's loop field names, home being 0. */
			ok = parse_u32(optarg, 0, 255, &loops);
			break;
		case 'h':
			usage(stdout, argv[0]);
			status = 0;
			goto out;
		default:
			usage(stderr, argv[0]);
			goto out;
		}
		if (!ok) {
			fprintf(stderr, "askai: invalid --%s: %s\n", options[idx].name, optarg);
			goto out;
		}
	}
	if (optind != argc) {
		usage(stderr, argv[0]);
		goto out;
	}
	if (lim->body > lim->bodies) {
		fputs("askai: --max-body exceeds --body-budget\n", stderr);
		goto out;
	}
	if (lim->client_bodies && lim->body > lim->client_bodies) {
		fputs("askai: --max-body exceeds --client-budget\n", stderr);
		goto out;
	}
	if (lim->out_pause > lim->out_max) {
		fputs("askai: --output-pause exceeds --max-output\n", stderr);
		goto out;
	}
	if (cfg.flags & CFG_MAN && !strcmp(tool.name, "man")) {
		fputs("askai: --man needs a --tool-name other than man\n", stderr);
		goto out;
	}

	/*
	 * A provider's usual key variable is only read for its own endpoint, so
	 * pointing --url elsewhere never leaks that key; use --key-env there.
	 */
	char const *prefix = protocols[ep.protocol].auth;
	char const *key = nullptr;
	if (key_env) {
		key = getenv(key_env);
		if (!key || !*key) {
			fprintf(stderr, "askai: %s is not set\n", key_env);
			goto out;
		}
	} else if (!ep.url && !has_header(headers, prefix)) {
		for (size_t i = 0; !key && i < ARRAY_SIZE(protocols[0].keys); ++i) {
			char const *var = protocols[ep.protocol].keys[i];
			key = var ? getenv(var) : nullptr;
			key = key && *key ? key : nullptr;
		}
		if (!key) {
			fprintf(stderr, "askai: set %s or pass --key-env\n",
			        protocols[ep.protocol].keys[0]);
			goto out;
		}
	}
	if (key && strpbrk(key, "\r\n")) {
		fputs("askai: the API key contains a line break\n", stderr);
		goto out;
	}

	if (!ep.url) {
		if (!ep.model) {
			fprintf(stderr, "askai: --model is required for %s's own API\n",
			        protocols[ep.protocol].name);
			goto out;
		}
		bool gemini = ep.protocol == AI_GEMINI;
		if (!dstr_printf(&url, "%s%s%s", protocols[ep.protocol].url,
		                 gemini ? ep.model : "",
		                 gemini ? ":streamGenerateContent?alt=sse" : ""))
			goto oom;
		ep.url = dstr_get(&url);
	} else if (ep.protocol == AI_GEMINI) {
		/* Gemini streams from :streamGenerateContent, in SSE with alt=sse,
		 * whatever alt the URL had. */
		static char const method[] = ":generateContent";
		size_t m = sizeof method - 1;
		size_t len = strlen(ep.url);
		char const *query = strchr(ep.url, '?');
		size_t path = query ? (size_t)(query - ep.url) : len;
		bool plain = path >= m && !memcmp(ep.url + path - m, method, m);
		char const *alt = query ? strstr(query, "?alt=") : nullptr;
		if (query && !alt)
			alt = strstr(query, "&alt=");
		/* The query up to its alt's value, and what follows that. */
		size_t head = alt ? (size_t)(alt - ep.url) + 1 : len;
		char const *tail = alt ? alt + 5 + strcspn(alt + 5, "&") : "";
		if (!dstr_printf(&url, "%.*s%s%.*s%salt=sse%s",
		                 (int)(path - (plain ? m : 0)), ep.url,
		                 plain ? ":streamGenerateContent" : "",
		                 (int)(head - path), ep.url + path,
		                 alt ? "" : query ? "&" : "?", tail))
			goto oom;
		ep.url = dstr_get(&url);
	}

	if (key && !has_header(headers, prefix)) {
		if (!dstr_printf(&auth, "%s%s", prefix, key))
			goto oom;
		headers[n_headers++] = dstr_get(&auth);
	}
	if (ep.protocol == AI_ANTHROPIC && !has_header(headers, "anthropic-version:"))
		headers[n_headers++] = "anthropic-version: 2023-06-01";
	if (!has_header(headers, "user-agent:"))
		headers[n_headers++] = "User-Agent: askai/0.1";
	ep.headers = headers;

	if (!tool.description) {
		if (!dstr_printf(&description,
		                 "Ask %s%s a question or give it a task, and get its "
		                 "answer. It cannot see this conversation, your files or "
		                 "your tools, so write a complete, self-contained prompt. "
		                 "Use it for a second opinion, for hard problems, or for "
		                 "knowledge you lack.",
		                 ep.model ? "the AI model " : "another AI model",
		                 ep.model ? ep.model : ""))
			goto oom;
		tool.description = dstr_get(&description);
	}

	hcfg.origins = origins;
	char const *token = getenv("ASKAI_TOKEN");
	hcfg.token = token && *token ? token : nullptr;
	log_threshold = verbose ? LOG_VERBOSE : LOG_INFO;
	/* One CPU fewer than askai may use is left to home and the threads that
	 * mostly sleep (design section 1). */
	size_t cpus = brainstem_cpus() - 1;
	bcfg.loops = loops ? loops : cpus < 1 ? 1 : cpus < 255 ? cpus : 255;
	status = serve(&ep, &tool, &mlim, &hcfg, &bcfg, &cfg);
	goto out;

oom:
	fputs("askai: out of memory\n", stderr);
	status = 1;
out:
	json_object_put(ep.extra);
	json_object_put(ep.temperature);
	free(origins);
	free(headers);
	return status;
}
