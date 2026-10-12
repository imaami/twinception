/** @file askai's configuration (config.h): the command line, and the
 *  configuration file, whose members are applied as the long options of
 *  their names are, before the command line's. */
#define _GNU_SOURCE /* vasprintf() */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ai.h"
#include "brainstem.h"
#include "config.h"
#include "git.h"
#include "httpd.h"
#include "jb.h"
#include "log.h"
#include "mcp.h"
#include "policy.h"
#include "tls.h"
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
	        "  -c, --config FILE         read options from FILE too: a JSON object\n"
	        "                            whose members are named as the long\n"
	        "                            options are, which the command line\n"
	        "                            overrides (see README.md)\n"
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
	        "      --upstream-tools      give the upstream tools too: those the\n"
	        "                            client may call but the ask tool, notes\n"
	        "                            with --notes, and compact; an ask call then\n"
	        "                            runs them and asks on until it answers\n"
	        "                            (needs --protocol openai)\n"
	        "      --notes DIR           the git repository where the upstream keeps\n"
	        "                            its notes, made bare if it does not exist\n"
	        "      --max-turns N         upstream calls an ask call makes at most\n"
	        "                            with --upstream-tools (default 32)\n"
	        "      --context TOKENS      the upstream's context, for compacting it\n"
	        "                            (default: what llama-server's /props says)\n"
	        "\n", argv0);
	fprintf(out,
	        "MCP server:\n"
	        "  -l, --listen ADDR         [host:]port, unix:PATH for a Unix socket, or\n"
	        "                            tls:[host:]port for TLS (repeatable;\n"
	        "                            default 127.0.0.1:8090)\n"
	        "      --tls-cert FILE       TLS's certificate chain, in PEM: the\n"
	        "                            server's, then those it is trusted by\n"
	        "      --tls-key FILE        its private key, in PEM, unencrypted: ECDSA\n"
	        "                            on P-256, P-384 or P-521, or Ed25519\n"
	        "      --tls-allow-rsa       allow an RSA key too\n"
	        "  -o, --allow-origin URL    allow a non-loopback browser Origin\n"
	        "                            (repeatable; '*' allows any)\n"
	        "      --tool-name NAME      tool name (default ask_ai)\n"
	        "      --tool-description TEXT\n"
	        "      --man                 also offer the tool man, which gives a page\n"
	        "                            of the manual as man(1) renders it, in\n"
	        "                            plain UTF-8 text; each page is rendered\n"
	        "                            once and kept\n"
	        "      --git PATH            also offer the tool git: read-only git\n"
	        "                            commands in the repository at PATH, which\n"
	        "                            its directory's name names (repeatable)\n"
	        "      --git-timeout SECONDS\n"
	        "                            a git call's deadline, at which git gets\n"
	        "                            SIGTERM, and SIGKILL a second later\n"
	        "                            (default 30)\n"
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
	        "      --test-task           test aid: also offer the tool test_task,\n"
	        "                            CPU work in 250 us slices, run as a task\n"
	        "                            on the client loops\n"
	        "  -h, --help                show this help\n"
	        "\n");
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
	        "Who may connect, and what they may do, the configuration file says\n"
	        "(roles, identities, unix-groups, anonymous; see README.md).  Without\n"
	        "those: if ASKAI_TOKEN is set, clients must send 'Authorization:\n"
	        "Bearer <it>'; else clients on loopback that name it as localhost,\n"
	        "127.0.0.1 or [::1], or on a Unix socket, may do everything, others\n"
	        "call the tools.  GET /metrics gives metrics in Prometheus's text\n"
	        "format to the clients who may see them.\n"
	        "SIGUSR1 prints each event loop's turn and step times, its timers'\n"
	        "lateness and the most work a step did, since the previous SIGUSR1,\n"
	        "to stderr, in lines that name it: loop1 to loopN serve clients,\n"
	        "home accepts them and runs the upstream calls.  With --config or\n"
	        "TLS, SIGHUP reloads who may connect and what they may do, and the\n"
	        "TLS certificate and key.  The first SIGINT or SIGTERM, or SIGHUP\n"
	        "without either, unless askai started with it ignored, ends tool\n"
	        "calls with an error, finishes other requests and exits 0; a\n"
	        "second, or a grace period run out, or an event loop stuck, exits\n"
	        "at once with %d.\n"
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
	for (; lines && *lines; ++lines)
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

/* The options without a short form. */
enum {
	OPT_TOOL_NAME = 0x100, OPT_TOOL_DESCRIPTION, OPT_MAX_HEAD, OPT_MAX_FIELDS,
	OPT_HEAD_TIMEOUT, OPT_MAX_BODY, OPT_MIN_BODY_RATE, OPT_BODY_GRACE,
	OPT_BODY_BUDGET, OPT_CLIENT_BUDGET, OPT_MAX_DEPTH, OPT_MAX_VALUES,
	OPT_MAX_MEMBERS, OPT_SSE_KEEPALIVE, OPT_MAX_CLIENT_CONNS,
	OPT_IDLE_TIMEOUT, OPT_OUTPUT_PAUSE, OPT_MAX_OUTPUT, OPT_SEND_TIMEOUT,
	OPT_GRACE, OPT_WATCHDOG, OPT_TEST_STALL, OPT_MEMORY_HIGH, OPT_TEST_PRESSURE,
	OPT_MAX_CALLS, OPT_LOOPS, OPT_SLOTS, OPT_CHUNK_TIMEOUT, OPT_TEST_FIFO,
	OPT_MAN, OPT_TEST_MAN_RECHECK, OPT_TEST_TASK, OPT_GIT, OPT_GIT_TIMEOUT,
	OPT_TLS_CERT, OPT_TLS_KEY, OPT_TLS_ALLOW_RSA, OPT_UPSTREAM_TOOLS, OPT_NOTES,
	OPT_MAX_TURNS, OPT_CONTEXT
};

static char const shorts[] = "c:p:u:m:k:H:s:t:n:x:T:l:o:vh";

static struct option const options[] = {
	{ "config",           required_argument, nullptr, 'c' },
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
	{ "upstream-tools",   no_argument,       nullptr, OPT_UPSTREAM_TOOLS },
	{ "notes",            required_argument, nullptr, OPT_NOTES },
	{ "max-turns",        required_argument, nullptr, OPT_MAX_TURNS },
	{ "context",          required_argument, nullptr, OPT_CONTEXT },
	{ "listen",           required_argument, nullptr, 'l' },
	{ "allow-origin",     required_argument, nullptr, 'o' },
	{ "tls-cert",         required_argument, nullptr, OPT_TLS_CERT },
	{ "tls-key",          required_argument, nullptr, OPT_TLS_KEY },
	{ "tls-allow-rsa",    no_argument,       nullptr, OPT_TLS_ALLOW_RSA },
	{ "tool-name",        required_argument, nullptr, OPT_TOOL_NAME },
	{ "tool-description", required_argument, nullptr, OPT_TOOL_DESCRIPTION },
	{ "man",              no_argument,       nullptr, OPT_MAN },
	{ "test-man-recheck", required_argument, nullptr, OPT_TEST_MAN_RECHECK },
	{ "git",              required_argument, nullptr, OPT_GIT },
	{ "git-timeout",      required_argument, nullptr, OPT_GIT_TIMEOUT },
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
	{ "test-task",        no_argument,       nullptr, OPT_TEST_TASK },
	{ "verbose",          no_argument,       nullptr, 'v' },
	{ "help",             no_argument,       nullptr, 'h' },
	{ nullptr, 0, nullptr, 0 }
};

/* Bytes of a configuration file, at most. */
enum { FILE_MAX = 1 << 20 };

/* Say what is wrong, as of the configuration file @p where unless that is
 * nullptr: on stderr, or in the log on a reload. */
[[gnu::format(printf, 3, 4)]]
static void
complain (struct config const *cfg,
          char const          *where,
          char const          *fmt,
          ...)
{
	char what[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(what, sizeof what, fmt, ap);
	va_end(ap);
	if (cfg->logged)
		LOG(LOG_ERROR, "%s%s%s", where ? where : "", where ? ": " : "", what);
	else
		fprintf(stderr, "askai: %s%s%s\n", where ? where : "", where ? ": " : "",
		        what);
}

/* The option whose value is @p opt. */
static struct option const *
option_of (int opt)
{
	struct option const *o = options;
	while (o->name && o->val != opt)
		++o;
	return o;
}

/* Append @p item to the nullptr-terminated @p list of @p *n items, which
 * has room for it (lists()). */
static void
push (char const **list,
      size_t       *n,
      char const   *item)
{
	list[(*n)++] = item;
	list[*n] = nullptr;
}

/* A string of its own, printed as @p fmt says; nullptr if out of memory. */
[[gnu::format(printf, 1, 2)]]
static char *
print (char const *fmt,
       ...)
{
	char *str;
	va_list ap;
	va_start(ap, fmt);
	if (vasprintf(&str, fmt, ap) < 0)
		str = nullptr;
	va_end(ap);
	return str;
}

/* Apply option @p opt with @p arg, given in the configuration file @p where,
 * or on the command line if that is nullptr; returns 0, or 2, having said
 * why. */
static int
apply (struct config *cfg,
       int            opt,
       char const    *arg,
       char const    *where)
{
	struct ai_endpoint *ep = &cfg->ep;
	struct httpd_limits *lim = &cfg->http.limits;
	struct mcp_limits *mlim = &cfg->mcp;
	size_t size = 0;
	bool ok = true;
	switch (opt) {
	case 'p': {
		size_t i = 0;
		while (i < ARRAY_SIZE(protocols) && strcmp(protocols[i].name, arg))
			++i;
		if (i == ARRAY_SIZE(protocols)) {
			complain(cfg, where, "unknown protocol: %s", arg);
			return 2;
		}
		ep->protocol = (enum ai_protocol)i;
		break;
	}
	case 'u':
		if (strncmp(arg, "http://", sizeof "http://" - 1) &&
		    strncmp(arg, "https://", sizeof "https://" - 1)) {
			complain(cfg, where, "not an http(s) URL: %s", arg);
			return 2;
		}
		ep->url = arg;
		break;
	case 'm': ep->model = arg; break;
	case 'k': cfg->key_env = arg; break;
	case 's': ep->system = arg; break;
	case 'l':
		/* The command line's listeners replace the file's. */
		if (!where && cfg->listen_file) {
			cfg->listen_file = false;
			cfg->n_listens = 0;
		}
		push(cfg->listens, &cfg->n_listens, arg);
		cfg->listen_file |= where != nullptr;
		break;
	case 'v': cfg->flags |= CFG_VERBOSE; break;
	case OPT_TOOL_DESCRIPTION: cfg->tool.description = arg; break;
	case OPT_TLS_CERT: cfg->tls_cert = arg; break;
	case OPT_TLS_KEY: cfg->tls_key = arg; break;
	case OPT_TLS_ALLOW_RSA: cfg->flags |= CFG_TLS_RSA; break;
	case OPT_UPSTREAM_TOOLS: cfg->flags |= CFG_UPSTREAM_TOOLS; break;
	case OPT_NOTES: cfg->notes = arg; break;
	case 'o':
		push(cfg->origins, &cfg->n_origins, arg);
		break;
	case 'H':
		if (!valid_header(arg)) {
			complain(cfg, where, "invalid header line: %s", arg);
			return 2;
		}
		push(cfg->headers, &cfg->n_headers, arg);
		break;
	case 't': {
		struct json_object *t = jb_parse(arg, strlen(arg), JSON_TOKENER_STRICT, 0);
		if ((!json_object_is_type(t, json_type_double) &&
		     !json_object_is_type(t, json_type_int)) ||
		    json_object_get_double(t) < 0) {
			json_object_put(t);
			complain(cfg, where, "invalid temperature: %s", arg);
			return 2;
		}
		json_object_put(ep->temperature);
		ep->temperature = t;
		break;
	}
	case 'n':
		if (!parse_long(arg, 1, INT32_MAX, &ep->max_tokens)) {
			complain(cfg, where, "invalid max token count: %s", arg);
			return 2;
		}
		break;
	case 'x': {
		struct json_object *x = jb_parse(arg, strlen(arg), JSON_TOKENER_STRICT, 0);
		if (!json_object_is_type(x, json_type_object)) {
			json_object_put(x);
			complain(cfg, where, "--extra needs a JSON object: %s", arg);
			return 2;
		}
		json_object_put(ep->extra);
		ep->extra = x;
		break;
	}
	case 'T':
		if (!parse_long(arg, 0, INT32_MAX / 1000, &ep->timeout_ms)) {
			complain(cfg, where, "invalid timeout: %s", arg);
			return 2;
		}
		ep->timeout_ms *= 1000;
		break;
	case OPT_TOOL_NAME:
		if (!valid_tool_name(arg)) {
			complain(cfg, where, "tool names are 1-128 of [A-Za-z0-9_.-]: %s", arg);
			return 2;
		}
		cfg->tool.name = arg;
		break;
	case OPT_MAX_HEAD:
		ok = parse_size(arg, 1, UINT16_MAX, &size);
		lim->head = (uint32_t)size;
		break;
	case OPT_MAX_FIELDS:
		ok = parse_u32(arg, 0, UINT32_MAX, &lim->fields);
		break;
	case OPT_HEAD_TIMEOUT:
		ok = parse_u32(arg, 0, UINT32_MAX, &lim->head_time);
		break;
	case OPT_MAX_BODY:
		ok = parse_size(arg, 0, SIZE_MAX / 2, &lim->body);
		break;
	case OPT_MIN_BODY_RATE:
		ok = parse_size(arg, 0, UINT32_MAX, &size);
		lim->body_rate = (uint32_t)size;
		break;
	case OPT_BODY_GRACE:
		ok = parse_u32(arg, 0, UINT32_MAX, &lim->body_grace);
		break;
	case OPT_BODY_BUDGET:
		ok = parse_size(arg, 0, SIZE_MAX / 2, &lim->bodies);
		break;
	case OPT_CLIENT_BUDGET:
		ok = parse_size(arg, 1, SIZE_MAX / 2, &lim->client_bodies);
		break;
	case OPT_MAX_DEPTH:
		ok = parse_u32(arg, 1, INT32_MAX - 1, &mlim->depth);
		break;
	case OPT_MAX_VALUES:
		ok = parse_u32(arg, 1, UINT32_MAX, &mlim->values);
		break;
	case OPT_MAX_MEMBERS:
		ok = parse_u32(arg, 1, UINT32_MAX - 1, &mlim->members);
		break;
	case OPT_SSE_KEEPALIVE:
		ok = parse_u32(arg, 0, UINT32_MAX / 2, &mlim->keepalive);
		break;
	case OPT_MAX_CLIENT_CONNS:
		ok = parse_u32(arg, 0, UINT32_MAX, &lim->clients);
		break;
	case OPT_IDLE_TIMEOUT:
		ok = parse_u32(arg, 0, UINT32_MAX, &lim->idle_time);
		break;
	case OPT_OUTPUT_PAUSE:
		ok = parse_size(arg, 1, SIZE_MAX / 2, &lim->out_pause);
		break;
	case OPT_MAX_OUTPUT:
		ok = parse_size(arg, 1, SIZE_MAX / 2, &lim->out_max);
		break;
	case OPT_SEND_TIMEOUT:
		ok = parse_u32(arg, 0, UINT32_MAX, &lim->write_time);
		break;
	case OPT_GRACE: {
		uint32_t seconds = 0;
		ok = parse_u32(arg, 1, 3600, &seconds);
		cfg->brainstem.grace_ms = seconds * 1000;
		break;
	}
	case OPT_WATCHDOG:
		ok = parse_u32(arg, 0, 60'000, &cfg->brainstem.watchdog_ms);
		break;
	case OPT_TEST_STALL: {
		uint32_t ms = 0;
		ok = parse_u32(arg, 1, 3'600'000, &ms);
		cfg->brainstem.stall_ms = ms;
		break;
	}
	case OPT_MEMORY_HIGH:
		ok = parse_size(arg, 1, SIZE_MAX / 2, &size);
		cfg->brainstem.memory_high = size;
		break;
	case OPT_TEST_PRESSURE:
		cfg->brainstem.pressure = arg;
		break;
	case OPT_MAX_CALLS:
		ok = parse_u32(arg, 1, 1'000'000, &cfg->calls);
		break;
	case OPT_SLOTS:
		ok = parse_u32(arg, 1, UINT16_MAX, &ep->slots);
		break;
	case OPT_CHUNK_TIMEOUT:
		ok = parse_long(arg, 0, INT32_MAX / 1000, &ep->chunk_ms);
		ep->chunk_ms *= 1000;
		break;
	case OPT_TEST_FIFO:
		cfg->flags |= CFG_TEST_FIFO;
		break;
	case OPT_TEST_TASK:
		cfg->flags |= CFG_TEST_TASK;
		break;
	case OPT_MAN:
		cfg->flags |= CFG_MAN;
		break;
	case OPT_TEST_MAN_RECHECK:
		ok = parse_u32(arg, 0, 86'400, &cfg->man_recheck);
		break;
	case OPT_GIT:
		push(cfg->gits, &cfg->n_gits, arg);
		break;
	case OPT_GIT_TIMEOUT: {
		uint32_t seconds = 0;
		ok = parse_u32(arg, 1, 3600, &seconds);
		cfg->git_timeout = seconds * 1'000'000'000ULL;
		break;
	}
	case OPT_MAX_TURNS:
		ok = parse_u32(arg, 1, 1000, &cfg->turns);
		break;
	case OPT_CONTEXT:
		ok = parse_long(arg, 0, INT32_MAX, &ep->context);
		break;
	case OPT_LOOPS:
		/* As many as a handle's loop field names, home being 0. */
		ok = parse_u32(arg, 0, 255, &cfg->loops);
		break;
	default:
		return 2;
	}
	if (!ok) {
		complain(cfg, where, "invalid --%s: %s", option_of(opt)->name, arg);
		return 2;
	}
	return 0;
}

/* Apply @p val, a member of the configuration file, as option @p o: a
 * string, or a number or, for --extra, an object, as its argument; true for
 * an option that takes none; an array of strings for a repeatable one. */
static int
apply_value (struct config        *cfg,
             struct option const  *o,
             struct json_object   *val)
{
	char const *where = cfg->path;
	bool flag = o->has_arg == no_argument;
	bool many = o->val == 'H' || o->val == 'o' || o->val == 'l' || o->val == OPT_GIT;
	enum json_type type = json_object_get_type(val);
	if (type == json_type_boolean && flag)
		return json_object_get_boolean(val) ? apply(cfg, o->val, nullptr, where) : 0;
	if (type == json_type_string && !flag)
		return apply(cfg, o->val, json_object_get_string(val), where);
	if ((type == json_type_int || type == json_type_double ||
	     (type == json_type_object && o->val == 'x')) && !flag)
		return apply(cfg, o->val, json_object_to_json_string_ext(
		             val, JSON_C_TO_STRING_PLAIN), where);
	if (type == json_type_array && many) {
		size_t n = json_object_array_length(val);
		size_t i = 0;
		int e = 0;
		/* Not of arrays, as lists() counts them. */
		for (struct json_object *v; !e && i < n &&
		     !json_object_is_type(v = json_object_array_get_idx(val, i),
		                          json_type_array); ++i)
			e = apply_value(cfg, o, v);
		if (e || i == n)
			return e;
	}
	complain(cfg, where, "invalid --%s: %s", o->name,
	         json_object_to_json_string_ext(val, JSON_C_TO_STRING_PLAIN));
	return 2;
}

/* Whether @p st's file only root and askai's user may change: for a
 * directory, others may add entries to it if only their owners may remove
 * them (the sticky bit, as on /tmp). */
static bool
safe (struct stat const *st)
{
	return (!st->st_uid || st->st_uid == geteuid()) &&
	       (!(st->st_mode & (S_IWGRP | S_IWOTH)) ||
	        (S_ISDIR(st->st_mode) && st->st_mode & S_ISVTX));
}

/* Whether every directory above @p path is safe(). */
static bool
safe_dirs (char const *path)
{
	char real[PATH_MAX];
	bool ok = realpath(path, real);
	for (char *slash = ok ? strrchr(real, '/') : nullptr; ok && slash;
	     slash = strrchr(real, '/')) {
		*slash = '\0';
		struct stat st;
		ok = !stat(slash == real ? "/" : real, &st) && safe(&st);
		if (slash == real)
			break;
	}
	return ok;
}

/* Read the file at @p path whole into @p *p_text, NUL-terminated, its
 * length into @p *p_len: a regular file of FILE_MAX bytes at most that only
 * root and askai's user may change, nor replace, and that has none of the
 * permission bits @p hide, such as S_IROTH for a secret, which is wiped
 * from memory freed here.  Returns 0, or the exit status, having said why. */
static int
slurp (struct config const *cfg,
       char const          *path,
       mode_t               hide,
       char               **p_text,
       size_t              *p_len)
{
	/* Not blocking, a FIFO opens at once, and fails the check. */
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	struct stat st;
	if (fd < 0 || fstat(fd, &st)) {
		complain(cfg, nullptr, "cannot open %s: %s", path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return 2;
	}
	char const *why = !S_ISREG(st.st_mode) ? "not a regular file"
	                : st.st_size > FILE_MAX ? "larger than 1 MiB"
	                : !safe(&st) || !safe_dirs(path)
	                ? "others than root and askai's user may change it"
	                : st.st_mode & hide ? "others may read it"
	                : nullptr;
	size_t len = 0;
	char *text = why ? nullptr : malloc((size_t)st.st_size + 1);
	if (text) {
		for (ssize_t n; (n = read(fd, text + len, (size_t)st.st_size - len)) != 0; ) {
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0) {
				why = strerror(errno);
				break;
			}
			len += (size_t)n;
			if (len == (size_t)st.st_size)
				break;
		}
	}
	close(fd);
	if (why || !text) {
		if (text)
			explicit_bzero(text, len);
		free(text);
		complain(cfg, why ? path : nullptr, "%s", why ? why : "out of memory");
		return why ? 2 : 1;
	}
	text[len] = '\0';
	*p_text = text;
	*p_len = len;
	return 0;
}

/* Read the configuration file into cfg->file: a JSON object, which may hold
 * comments, in a file slurp() reads.  Returns 0, or the exit status, having
 * said why. */
static int
load (struct config *cfg)
{
	char const *path = cfg->path;
	char *text;
	size_t len;
	int status = slurp(cfg, path, 0, &text, &len);
	if (status)
		return status;

	struct json_tokener *tok = json_tokener_new();
	struct json_object *file = tok ? json_tokener_parse_ex(tok, text, (int)len)
	                               : nullptr;
	size_t end = tok ? json_tokener_get_parse_end(tok) : 0;
	enum json_tokener_error e = tok ? json_tokener_get_error(tok)
	                                : json_tokener_error_memory;
	json_tokener_free(tok);
	/* What follows the object may only be blank. */
	size_t rest = end + strspn(text + end, " \t\r\n");
	if (file && (!json_object_is_type(file, json_type_object) || rest < len)) {
		e = json_tokener_error_parse_unexpected;
		end = json_object_is_type(file, json_type_object) ? rest : 0;
	}
	if (!file || e != json_tokener_success) {
		size_t line = 1;
		size_t column = 1;
		for (size_t i = 0; i < end && i < len; ++i, ++column)
			if (text[i] == '\n') {
				++line;
				column = 0;
			}
		complain(cfg, path, "%zu:%zu: %s", line, column, file && e ==
		         json_tokener_error_parse_unexpected ? "not one JSON object"
		         : json_tokener_error_desc(e));
		json_object_put(file);
		free(text);
		return 2;
	}
	free(text);
	cfg->file = file;
	return 0;
}

/* Whether the configuration file's member @p key is of its policy, who may
 * connect and what they may do (src/policy.h), rather than an option. */
static bool
of_policy (char const *key)
{
	return !strcmp(key, "roles") || !strcmp(key, "identities") ||
	       !strcmp(key, "unix-groups") || !strcmp(key, "anonymous");
}

/* Whether the configuration file's member @p key takes effect on a reload:
 * the policy's, and TLS's certificate, key and key types. */
static bool
reloadable (char const *key)
{
	return of_policy(key) || !strncmp(key, "tls-", sizeof "tls-" - 1);
}

/* The members of @p file, a configuration file's object or nullptr, but
 * those @p but is true of. */
static size_t
members_but (struct json_object *file,
             bool              (*but) (char const *key))
{
	size_t n = 0;
	struct json_object_iter it;
	if (file)
		json_object_object_foreachC(file, it)
			n += !but(it.key);
	return n;
}

/* Whether @p a and @p b, configuration files' objects or nullptr, set the
 * same options, those a reload takes aside. */
static bool
same_options (struct json_object *a,
              struct json_object *b)
{
	if (members_but(a, reloadable) != members_but(b, reloadable))
		return false;
	struct json_object_iter it;
	if (a)
		json_object_object_foreachC(a, it) {
			struct json_object *v;
			if (!reloadable(it.key) &&
			    (!json_object_object_get_ex(b, it.key, &v) ||
			     !json_object_equal(it.val, v)))
				return false;
		}
	return true;
}

/* Make room for the lists of headers, origins, listeners and repositories,
 * in one block: each may hold every argument, every string of the
 * configuration file and the three headers askai adds, and ends in nullptr.
 * False if out of memory. */
static bool
lists (struct config *cfg)
{
	size_t room = (size_t)cfg->argc + 4;
	struct json_object_iter it;
	if (cfg->file)
		json_object_object_foreachC(cfg->file, it)
			room += json_object_is_type(it.val, json_type_array)
			      ? json_object_array_length(it.val) : 1;
	char const **block = malloc(4 * room * sizeof *block);
	if (!block)
		return false;
	cfg->headers = block;
	cfg->origins = block + room;
	cfg->listens = block + 2 * room;
	cfg->gits = block + 3 * room;
	*cfg->headers = *cfg->origins = *cfg->listens = *cfg->gits = nullptr;
	return true;
}

/* Apply the configuration file's members as the options of their names. */
static int
apply_file (struct config *cfg)
{
	struct json_object_iter it;
	json_object_object_foreachC(cfg->file, it) {
		struct option const *o = options;
		while (o->name && strcmp(o->name, it.key))
			++o;
		if (of_policy(it.key))
			continue;
		if (!o->name || o->val == 'c' || o->val == 'h') {
			complain(cfg, cfg->path, "unknown option: %s", it.key);
			return 2;
		}
		int e = apply_value(cfg, o, it.val);
		if (e)
			return e;
	}
	return 0;
}

/* Whether the directory @p dir has @p entry. */
static bool
has (char const *dir,
     char const *entry)
{
	char path[PATH_MAX];
	struct stat st;
	int n = snprintf(path, sizeof path, "%s/%s", dir, entry);
	return n > 0 && (size_t)n < sizeof path && !stat(path, &st);
}

/* Whether the notes repository's path is a directory that only root and
 * askai's user may change, or else names one that the directory it would
 * be made in lets only them make; false, having said why, if not. */
static bool
notes_place (struct config const *cfg)
{
	char const *path = cfg->notes;
	struct stat st;
	char const *why = nullptr;
	if (!stat(path, &st)) {
		why = !S_ISDIR(st.st_mode) ? "not a directory"
		    : !safe(&st) || !safe_dirs(path)
		    ? "others than root and askai's user may change it" : nullptr;
	} else if (errno == ENOENT) {
		/* The directory it would be made in, its trailing slashes aside. */
		char dir[PATH_MAX];
		size_t end = strlen(path);
		while (end > 1 && path[end - 1] == '/')
			--end;
		while (end && path[end - 1] != '/')
			--end;
		while (end > 1 && path[end - 1] == '/')
			--end;
		int n = snprintf(dir, sizeof dir, "%.*s", end ? (int)end : 1, end ? path : ".");
		why = n < 0 || (size_t)n >= sizeof dir ? strerror(ENAMETOOLONG)
		    : stat(dir, &st) ? strerror(errno)
		    : !S_ISDIR(st.st_mode) || !safe(&st) || !safe_dirs(dir)
		    ? "others than root and askai's user may change the directory it "
		      "would be made in" : nullptr;
	} else {
		why = strerror(errno);
	}
	if (why)
		complain(cfg, nullptr, "--notes: %s: %s", path, why);
	return !why;
}

/* The TLS listeners' context, from the certificate chain's file and its
 * key's, which none but its owner and its group may read, both wiped once
 * read, as one file may hold the two; 0, or the exit status, having said
 * why. */
static int
tls_context (struct config *cfg)
{
	if (!cfg->tls_cert || !cfg->tls_key) {
		complain(cfg, nullptr, "a tls: listener needs --tls-cert and --tls-key");
		return 2;
	}
	char *chain;
	size_t chain_len;
	int e = slurp(cfg, cfg->tls_cert, 0, &chain, &chain_len);
	if (e)
		return e;
	char *key;
	size_t key_len;
	e = slurp(cfg, cfg->tls_key, S_IROTH, &key, &key_len);
	if (e) {
		explicit_bzero(chain, chain_len);
		free(chain);
		return e;
	}
	char why[256];
	cfg->tls = tls_ctx_create(cfg, chain, chain_len, key, key_len, why, sizeof why, &e);
	explicit_bzero(key, key_len);
	explicit_bzero(chain, chain_len);
	free(key);
	free(chain);
	if (e == ENOMEM) {
		complain(cfg, nullptr, "out of memory");
		return 1;
	}
	if (e) {
		complain(cfg, nullptr, "TLS: %s", why);
		return 2;
	}
	cfg->http.tls = cfg->tls;
	return 0;
}

/* Check the options together, and work out what they leave to askai. */
static int
finish (struct config *cfg)
{
	struct ai_endpoint *ep = &cfg->ep;
	struct httpd_limits const *lim = &cfg->http.limits;
	if (lim->body > lim->bodies) {
		complain(cfg, nullptr, "--max-body exceeds --body-budget");
		return 2;
	}
	if (lim->client_bodies && lim->body > lim->client_bodies) {
		complain(cfg, nullptr, "--max-body exceeds --client-budget");
		return 2;
	}
	if (lim->out_pause > lim->out_max) {
		complain(cfg, nullptr, "--output-pause exceeds --max-output");
		return 2;
	}
	if (cfg->flags & CFG_MAN && !strcmp(cfg->tool.name, "man")) {
		complain(cfg, nullptr, "--man needs a --tool-name other than man");
		return 2;
	}
	if (cfg->flags & CFG_TEST_TASK && !strcmp(cfg->tool.name, "test_task")) {
		complain(cfg, nullptr, "--test-task needs a --tool-name other than test_task");
		return 2;
	}
	if (cfg->n_gits && !strcmp(cfg->tool.name, "git")) {
		complain(cfg, nullptr, "--git needs a --tool-name other than git");
		return 2;
	}
	/* Each repository a work tree's top or a git directory, so that git
	 * finds none around it, and each named apart. */
	for (size_t i = 0; i < cfg->n_gits; ++i) {
		char const *path = cfg->gits[i];
		size_t len;
		char const *name = git_name(path, &len);
		if (!name) {
			complain(cfg, nullptr, "--git: %s names no repository: its last "
			         "component, less a .git suffix, must be 1 to 64 of "
			         "[A-Za-z0-9_.-], not beginning with a dot", path);
			return 2;
		}
		if (!has(path, ".git") && !(has(path, "HEAD") && has(path, "objects"))) {
			complain(cfg, nullptr, "--git: not a git repository: %s", path);
			return 2;
		}
		for (size_t j = 0; j < i; ++j) {
			size_t other;
			char const *o = git_name(cfg->gits[j], &other);
			if (other == len && !memcmp(o, name, len)) {
				complain(cfg, nullptr, "--git: two repositories named %.*s",
				         (int)len, name);
				return 2;
			}
		}
	}

	if (cfg->flags & CFG_UPSTREAM_TOOLS && ep->protocol != AI_OPENAI) {
		complain(cfg, nullptr, "--upstream-tools needs --protocol openai");
		return 2;
	}
	/* The turns follow the messages. */
	if (cfg->flags & CFG_UPSTREAM_TOOLS &&
	    json_object_object_get_ex(ep->extra, "messages", nullptr)) {
		complain(cfg, nullptr, "--extra may not change messages with "
		         "--upstream-tools");
		return 2;
	}
	if (cfg->notes && !(cfg->flags & CFG_UPSTREAM_TOOLS)) {
		complain(cfg, nullptr, "--notes needs --upstream-tools");
		return 2;
	}
	if (cfg->notes && !notes_place(cfg))
		return 2;

	/*
	 * A provider's usual key variable is only read for its own endpoint, so
	 * pointing --url elsewhere never leaks that key; use --key-env there.
	 */
	char const *prefix = protocols[ep->protocol].auth;
	char const *key = nullptr;
	if (cfg->key_env) {
		key = getenv(cfg->key_env);
		if (!key || !*key) {
			complain(cfg, nullptr, "%s is not set", cfg->key_env);
			return 2;
		}
	} else if (!ep->url && !has_header(cfg->headers, prefix)) {
		for (size_t i = 0; !key && i < ARRAY_SIZE(protocols[0].keys); ++i) {
			char const *var = protocols[ep->protocol].keys[i];
			key = var ? getenv(var) : nullptr;
			key = key && *key ? key : nullptr;
		}
		if (!key) {
			complain(cfg, nullptr, "set %s or pass --key-env",
			         protocols[ep->protocol].keys[0]);
			return 2;
		}
	}
	if (key && strpbrk(key, "\r\n")) {
		complain(cfg, nullptr, "the API key contains a line break");
		return 2;
	}

	if (!ep->url) {
		if (!ep->model) {
			complain(cfg, nullptr, "--model is required for %s's own API",
			         protocols[ep->protocol].name);
			return 2;
		}
		bool gemini = ep->protocol == AI_GEMINI;
		if (!(cfg->url = print("%s%s%s", protocols[ep->protocol].url,
		                       gemini ? ep->model : "",
		                       gemini ? ":streamGenerateContent?alt=sse" : "")))
			goto oom;
		ep->url = cfg->url;
	} else if (ep->protocol == AI_GEMINI) {
		/* Gemini streams from :streamGenerateContent, in SSE with alt=sse,
		 * whatever alt the URL had. */
		static char const method[] = ":generateContent";
		size_t m = sizeof method - 1;
		size_t len = strlen(ep->url);
		char const *query = strchr(ep->url, '?');
		size_t path = query ? (size_t)(query - ep->url) : len;
		bool plain = path >= m && !memcmp(ep->url + path - m, method, m);
		char const *alt = query ? strstr(query, "?alt=") : nullptr;
		if (query && !alt)
			alt = strstr(query, "&alt=");
		/* The query up to its alt's value, and what follows that. */
		size_t head = alt ? (size_t)(alt - ep->url) + 1 : len;
		char const *tail = alt ? alt + 5 + strcspn(alt + 5, "&") : "";
		if (!(cfg->url = print("%.*s%s%.*s%salt=sse%s",
		                       (int)(path - (plain ? m : 0)), ep->url,
		                       plain ? ":streamGenerateContent" : "",
		                       (int)(head - path), ep->url + path,
		                       alt ? "" : query ? "&" : "?", tail)))
			goto oom;
		ep->url = cfg->url;
	}

	if (key && !has_header(cfg->headers, prefix)) {
		if (!(cfg->auth = print("%s%s", prefix, key)))
			goto oom;
		push(cfg->headers, &cfg->n_headers, cfg->auth);
	}
	if (ep->protocol == AI_ANTHROPIC && !has_header(cfg->headers, "anthropic-version:"))
		push(cfg->headers, &cfg->n_headers, "anthropic-version: 2023-06-01");
	if (!has_header(cfg->headers, "user-agent:"))
		push(cfg->headers, &cfg->n_headers, "User-Agent: askai/0.1");
	ep->headers = cfg->headers;

	if (!cfg->tool.description) {
		bool lends = cfg->flags & CFG_UPSTREAM_TOOLS;
		cfg->description = print("Ask %s%s a question or give it a task, and get "
		                         "its answer. It cannot see this conversation, your "
		                         "files or your tools, so write a complete, "
		                         "self-contained prompt.%s%s Use it for a second "
		                         "opinion, for hard problems, or for knowledge you "
		                         "lack.",
		                         ep->model ? "the AI model " : "another AI model",
		                         ep->model ? ep->model : "",
		                         lends ? " It may use some of this server's tools "
		                                 "itself, and its answer lists the calls it "
		                                 "made." : "",
		                         cfg->notes ? " It keeps notes in a thread, which "
		                                      "a later call may go on with." : "");
		if (!(cfg->tool.description = cfg->description))
			goto oom;
	}

	cfg->http.origins = cfg->origins;
	if (!cfg->n_listens)
		push(cfg->listens, &cfg->n_listens, "127.0.0.1:8090");
	cfg->http.listen = cfg->listens;
	bool tls = false;
	for (size_t i = 0; i < cfg->n_listens; ++i) {
		char const *addr = httpd_tls(cfg->listens[i]);
		if (addr && !strncmp(addr, "unix:", sizeof "unix:" - 1)) {
			complain(cfg, nullptr, "TLS over a Unix socket: %s", cfg->listens[i]);
			return 2;
		}
		tls |= addr != nullptr;
	}
	int e = tls ? tls_context(cfg) : 0;
	if (e)
		return e;
	char const *tools[MCP_TOOLS] = {
		[0] = cfg->tool.name,
		[1] = cfg->flags & CFG_MAN ? "man" : nullptr,
		[2] = cfg->flags & CFG_TEST_TASK ? "test_task" : nullptr,
		[3] = cfg->n_gits ? "git" : nullptr
	};
	static_assert(MCP_TOOL_ASK == 1u << 0 && MCP_TOOL_MAN == 1u << 1 &&
	              MCP_TOOL_TASK == 1u << 2 && MCP_TOOL_GIT == 1u << 3);
	char why[256];
	e = policy_new(&cfg->policy, cfg->file, tools, MCP_TOOLS, why, sizeof why);
	if (e == ENOMEM)
		goto oom;
	if (e) {
		complain(cfg, cfg->path, "%s", why);
		return 2;
	}
	cfg->http.policy = cfg->policy;
	char const *token = getenv("ASKAI_TOKEN");
	bool own = !cfg->file ||
	           members_but(cfg->file, of_policy) ==
	           (size_t)json_object_object_length(cfg->file);
	if (token && *token && !own && !cfg->logged)
		complain(cfg, nullptr, "note: ASKAI_TOKEN is ignored, as %s names who may "
		         "connect", cfg->path);
	/* One CPU fewer than askai may use is left to home and the threads that
	 * mostly sleep (design section 1). */
	size_t cpus = brainstem_cpus() - 1;
	cfg->brainstem.loops = cfg->loops ? cfg->loops
	                     : cpus < 1 ? 1 : cpus < 255 ? cpus : 255;
	return -1;

oom:
	complain(cfg, nullptr, "out of memory");
	return 1;
}

/* Read the command line and the file it names into @p cfg, saying what is
 * wrong in the log if @p logged, as config() says. */
static int
parse (struct config *cfg,
       int            argc,
       char          *argv[],
       bool           logged)
{
	*cfg = (struct config){
		.ep = {
			.timeout_ms = 600'000,
			.chunk_ms = 60'000,
			.protocol = AI_OPENAI
		},
		.tool = { .name = "ask_ai" },
		.mcp = mcp_limits(),
		.http = { .limits = httpd_limits() },
		.brainstem = {
			.grace_ms = 5'000,
			.watchdog_ms = 20
		},
		.argv = argv,
		.argc = argc,
		.calls = 256,
		.man_recheck = 600,
		.turns = 32,
		.git_timeout = 30'000'000'000,
		.logged = logged
	};

	/* The configuration file goes first, so that the command line overrides
	 * it; help goes before either.  Each pass begins getopt afresh. */
	opterr = 0;
	optind = 0;
	for (int opt; (opt = getopt_long(argc, argv, shorts, options, nullptr)) >= 0; ) {
		if (opt == 'h' && !logged) {
			usage(stdout, argv[0]);
			return 0;
		}
		if (opt == 'c')
			cfg->path = optarg;
	}
	opterr = !logged;
	optind = 0;
	int e = cfg->path ? load(cfg) : 0;
	if (!e && !lists(cfg)) {
		complain(cfg, nullptr, "out of memory");
		return 1;
	}
	if (!e && cfg->file)
		e = apply_file(cfg);
	for (int opt; !e && (opt = getopt_long(argc, argv, shorts, options, nullptr)) >= 0; ) {
		if (opt == '?') {
			if (!logged)
				usage(stderr, argv[0]);
			return 2;
		}
		if (opt != 'c')
			e = apply(cfg, opt, optarg, nullptr);
	}
	if (!e && optind != argc) {
		if (!logged)
			usage(stderr, argv[0]);
		return 2;
	}
	return e ? e : finish(cfg);
}

struct config
config (int   argc,
        char *argv[],
        int  *status)
{
	struct config cfg;
	*status = parse(&cfg, argc, argv, false);
	return cfg;
}

int
config_reload (struct config       *cfg,
               struct config const *from)
{
	int status = parse(cfg, from->argc, from->argv, true);
	cfg->restart = status < 0 && !same_options(from->file, cfg->file);
	return status;
}

void
config_fini (struct config *cfg)
{
	json_object_put(cfg->ep.extra);
	json_object_put(cfg->ep.temperature);
	json_object_put(cfg->file);
	policy_put(cfg->policy);
	tls_ctx_put(cfg->tls);
	free(cfg->url);
	free(cfg->auth);
	free(cfg->description);
	free(cfg->headers);
}
