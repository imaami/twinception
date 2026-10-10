/** @file askai: an MCP server whose one tool asks another AI. */
#include <errno.h>
#include <getopt.h>
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
#include "dstr.h"
#include "http.h"
#include "httpd.h"
#include "jb.h"
#include "log.h"
#include "loop.h"
#include "mcp.h"
#include "metrics.h"
#include "util.h"

static struct {
	char const *name;
	char const *url;      /* gemini: the model and ":generateContent" follow */
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
	        "MCP server (Streamable HTTP) with one tool that asks another AI.\n"
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
	        "  -T, --timeout SECONDS     upstream timeout (default 600, 0 = none)\n"
	        "\n"
	        "MCP server:\n"
	        "  -l, --listen ADDR         [host:]port (default 127.0.0.1:8090)\n"
	        "  -o, --allow-origin URL    allow a non-loopback browser Origin\n"
	        "                            (repeatable; '*' allows any)\n"
	        "      --tool-name NAME      tool name (default ask_ai)\n"
	        "      --tool-description TEXT\n"
	        "  -v, --verbose             log every tool call\n"
	        "      --grace SECONDS       to finish what is under way on SIGINT,\n"
	        "                            SIGTERM or SIGHUP (default 5)\n"
	        "      --watchdog MS         log a turn of the event loop longer than\n"
	        "                            this (default 20, 0 = none)\n"
	        "      --test-stall MS       test aid: SIGUSR2 holds the event loop in\n"
	        "                            one turn this long\n"
	        "      --memory-high BYTES   resident memory from which memory pressure\n"
	        "                            is high (default: a sixteenth of RAM, or of\n"
	        "                            the lowest cgroup limit, at least 256M)\n"
	        "      --test-pressure FILE  test aid: memory pressure is at least the\n"
	        "                            level FILE holds, 0 (none), 1 (high) or 2\n"
	        "                            (critical), read each second\n"
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
	        "SIGUSR1 prints the event loop's turn and step times, its timers'\n"
	        "lateness and the most work a step did, since the previous SIGUSR1,\n"
	        "to stderr.  The first SIGINT, SIGTERM or SIGHUP, unless askai\n"
	        "started with it ignored, ends tool calls with an error, finishes\n"
	        "other requests and exits 0; a second, or a grace period run out, or\n"
	        "an event loop stuck, exits at once with %d.  Invalid options or\n"
	        "environment exit 2, and a failure to start or to serve 1.\n",
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
enum { RELIEVE_MCP, RELIEVE_STASH, RELIEVE_TRIM, RELIEVED };

/* glibc's tuning (see serve()): blocks from this size up get mappings of
 * their own, and where nothing limits the address space, heaps keep this
 * much of their tops. */
enum { MMAP_MIN = 32 << 20, TOP_PAD = 64 << 20 };

/* The heaps' top pad: glibc's default, or TOP_PAD (see serve()). */
static int top_pad = 128 << 10;

/*
 * Give what the allocator keeps free back to the system (glibc): malloc_trim()
 * gives back the free pages within every arena's heaps and the top of the main
 * arena's, and freeing 64 KiB under a low threshold and pad the top of this
 * thread's arena, which malloc_trim() leaves.
 */
static void
trim (void)
{
	mallopt(M_TOP_PAD, 0);
	mallopt(M_TRIM_THRESHOLD, 0);
	void *volatile block = malloc(64 << 10);
	free(block);
	malloc_trim(0);
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

/* What serves on the event loop's thread. */
struct app {
	struct buf_stash         stash;
	struct loop_step         relief;
	struct loop_timer        age;       /* the windows of reuse */
	struct mcp_static const *st;
	struct httpd_cfg const  *hcfg;
	struct http             *http;
	struct mcp              *mcp;
	struct httpd            *httpd;
	uint64_t                 released;  /* the stash's count as the relief
	                                       began */
	uint64_t                 phase;     /* the relief's, widened over the
	                                       padding */
};

/*
 * Relieve memory pressure, in bulk steps (design section 5): free the
 * requests kept for reuse, then the stash's storage, as much of each a step
 * as its share of release work allows and as much in all as the level says;
 * then have the allocator give what is free back to the system, in a step of
 * its own, as that one call cannot be split.  It stops if the pressure clears.
 */
static void
relief_step (struct loop      *loop,
             struct loop_step *step)
{
	struct app *app = container_of(step, struct app, relief);
	struct buf_stash_metrics *m = &app->stash.metrics;
	enum buf_pressure level = atomic_load_explicit(&buf_pressure, memory_order_relaxed);
	size_t room = loop_room(loop, LOOP_RELEASE);
	size_t n;
	switch (level ? app->phase : RELIEVED) {
	case RELIEVE_MCP:
		n = mcp_relieve(app->mcp, level, room);
		break;
	case RELIEVE_STASH:
		n = buf_stash_release(&app->stash, level, room);
		break;
	case RELIEVE_TRIM: {
		uint64_t start = loop_clock();
		trim();
		uint64_t ns = loop_clock() - start;
		uint64_t bytes = hist_get(&m->released) - app->released;
		hist_add(&m->trims, 1);
		hist_add(&m->trim_ns, ns);
		app->phase = RELIEVED;
		/* A relief that found little to do is routine while pressure lasts. */
		LOG(bytes || ns >= 1'000'000 ? LOG_INFO : LOG_VERBOSE, "memory pressure "
		    "relieved: %.1f MiB of buffers released, then a trim of %.1f ms",
		    (double)bytes / (1 << 20), (double)ns / 1e6);
		return;
	}
	default:
		return;
	}
	loop_work(loop, LOOP_RELEASE, n);
	app->phase += !n;
	loop_ready_bulk(loop, step);
}

/* Begin a window of reuse, over which a relief at high pressure finds what
 * went unused: as long as the brainstem waits between reliefs. */
static void
app_age (struct loop       *loop,
         struct loop_timer *timer)
{
	struct app *app = container_of(timer, struct app, age);
	buf_stash_age(&app->stash);
	mcp_age(app->mcp);
	(void)loop_timer_set(loop, timer, loop->now + BRAINSTEM_RELIEF_MS * 1'000'000ULL);
}

static void
app_relieve (void        *ctx,
             struct loop *loop)
{
	struct app *app = ctx;
	app->phase = RELIEVE_MCP;
	app->released = hist_get(&app->stash.metrics.released);
	hist_add(&app->stash.metrics.reliefs, 1);
	loop_ready_bulk(loop, &app->relief);
}

static int
app_start (void            *ctx,
           struct loop     *loop,
           struct blockers *blockers)
{
	struct app *app = ctx;
	app->relief.fn = relief_step;
	app->age.fn = app_age;
	app->phase = RELIEVED;
	buf_stash_attach(&app->stash);
	int e = http_init(&app->http, loop);
	if (!e) {
		http_resolver(app->http, blockers);
		e = mcp_init(&app->mcp, loop, app->http, app->st, &(struct metrics_src){
			.loop = loop->metrics,
			.http = http_metrics(app->http),
			.blockers = blockers_metrics(blockers),
			.stash = &app->stash.metrics
		});
	}
	if (e)
		LOG(LOG_ERROR, "%s", strerror(e));
	else if (!(e = httpd_init(&app->httpd, loop, mcp_app(app->mcp), app->hcfg)))
		e = loop_timer_set(loop, &app->age,
		                   loop->now + BRAINSTEM_RELIEF_MS * 1'000'000ULL);
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
	mcp_drain(app->mcp);
}

static void
app_stop (void        *ctx,
          struct loop *loop)
{
	struct app *app = ctx;
	httpd_fini(&app->httpd);
	mcp_fini(&app->mcp);
	http_fini(&app->http);
	loop_unready(&app->relief);
	loop_timer_stop(loop, &app->age);
	(void)buf_stash_release(&app->stash, BUF_CRITICAL, SIZE_MAX);
	buf_stash_attach(nullptr);
}

/* Serve until shut down; returns the exit status. */
static int
serve (struct ai_endpoint const *ep,
       struct mcp_tool const    *tool,
       struct mcp_limits const  *mlim,
       struct httpd_cfg const   *hcfg,
       struct brainstem_cfg     *bcfg)
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

	/* What every loop shares is made before any starts. */
	struct mcp_static *st = nullptr;
	int e = mcp_static_new(&st, ep, tool, mlim);
	if (e) {
		fprintf(stderr, "askai: %s\n", strerror(e));
		return 1;
	}
	struct app app = {
		.st = st,
		.hcfg = hcfg
	};
	bcfg->app = (struct brainstem_app){
		.ctx = &app,
		.start = app_start,
		.drain = app_drain,
		.relieve = app_relieve,
		.stop = app_stop
	};
	int status = brainstem_run(bcfg);
	mcp_static_free(&st);
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
		OPT_GRACE, OPT_WATCHDOG, OPT_TEST_STALL, OPT_MEMORY_HIGH, OPT_TEST_PRESSURE
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
		{ "listen",           required_argument, nullptr, 'l' },
		{ "allow-origin",     required_argument, nullptr, 'o' },
		{ "tool-name",        required_argument, nullptr, OPT_TOOL_NAME },
		{ "tool-description", required_argument, nullptr, OPT_TOOL_DESCRIPTION },
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
		{ "grace",            required_argument, nullptr, OPT_GRACE },
		{ "watchdog",         required_argument, nullptr, OPT_WATCHDOG },
		{ "test-stall",       required_argument, nullptr, OPT_TEST_STALL },
		{ "memory-high",      required_argument, nullptr, OPT_MEMORY_HIGH },
		{ "test-pressure",    required_argument, nullptr, OPT_TEST_PRESSURE },
		{ "verbose",          no_argument,       nullptr, 'v' },
		{ "help",             no_argument,       nullptr, 'h' },
		{ nullptr, 0, nullptr, 0 }
	};

	struct ai_endpoint ep = {
		.timeout_ms = 600'000,
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
	char const *key_env = nullptr;
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
		                 gemini ? ep.model : "", gemini ? ":generateContent" : ""))
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
	status = serve(&ep, &tool, &mlim, &hcfg, &bcfg);
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
