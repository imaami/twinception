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

/*
 * Give freed memory back to the system once the loop is quiet.  glibc keeps
 * what small allocations free, and a large message freed leaves much of it.
 * malloc_trim() returns it all, at about 55 us per MB, in one call that no
 * step can share, so it waits for a turn with nothing else to do.
 */
enum { TRIM_S = 5 };

struct trim {
	struct loop_timer timer;
	struct loop_step  step;
};

static void
trim_step (struct loop      *loop,
           struct loop_step *step)
{
	(void)loop;
	(void)step;
	malloc_trim(0);
}

static void
trim_timer (struct loop       *loop,
            struct loop_timer *timer)
{
	if (loop_quiet(loop))
		loop_ready_bulk(loop, &container_of(timer, struct trim, timer)->step);
	(void)loop_timer_set(loop, timer, loop->now + TRIM_S * 1'000'000'000ULL);
}

/* What serves on the event loop's thread. */
struct app {
	struct trim              trim;
	struct mcp_static const *st;
	struct httpd_cfg const  *hcfg;
	struct http             *http;
	struct mcp              *mcp;
	struct httpd            *httpd;
};

static int
app_start (void            *ctx,
           struct loop     *loop,
           struct blockers *blockers)
{
	struct app *app = ctx;
	app->trim = (struct trim){
		.timer.fn = trim_timer,
		.step.fn = trim_step
	};
	int e = loop_timer_set(loop, &app->trim.timer, loop->now + TRIM_S * 1'000'000'000ULL);
	if (!e && !(e = http_init(&app->http, loop))) {
		http_resolver(app->http, blockers);
		e = mcp_init(&app->mcp, loop, app->http, app->st, &(struct metrics_src){
			.loop = loop->metrics,
			.http = http_metrics(app->http),
			.blockers = blockers_metrics(blockers)
		});
	}
	if (e)
		LOG(LOG_ERROR, "%s", strerror(e));
	else
		e = httpd_init(&app->httpd, loop, mcp_app(app->mcp), app->hcfg);
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
	loop_unready(&app->trim.step);
	loop_timer_stop(loop, &app->trim.timer);
}

/* Serve until shut down; returns the exit status. */
static int
serve (struct ai_endpoint const *ep,
       struct mcp_tool const    *tool,
       struct mcp_limits const  *mlim,
       struct httpd_cfg const   *hcfg,
       struct brainstem_cfg     *bcfg)
{
	/* Hand large buffers back to the system when freed: after the first such
	 * free, glibc would raise its mmap threshold and keep the next ones. */
	mallopt(M_MMAP_THRESHOLD, 128 << 10);

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
		OPT_GRACE, OPT_WATCHDOG, OPT_TEST_STALL
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
