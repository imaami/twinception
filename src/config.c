/** @file askai's configuration (config.h): the command line, and the
 *  configuration file, whose members are applied as the long options of
 *  their names are, before the command line's. */
#define _GNU_SOURCE /* vasprintf() */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <json-c/json.h>
#include <json-c/json_visit.h>
#include <libgen.h>
#include <math.h>
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


static char const *const api_names[] = {
	[AI_CHAT]      = "chat",
	[AI_RESPONSES] = "responses",
	[AI_MESSAGES]  = "messages",
	[AI_GENERATE]  = "generate",
};

/* Each API's header line prefix for the API key. */
static char const *const auths[] = {
	[AI_CHAT]      = "Authorization: Bearer ",
	[AI_RESPONSES] = "Authorization: Bearer ",
	[AI_MESSAGES]  = "x-api-key: ",
	[AI_GENERATE]  = "x-goog-api-key: ",
};

static char const *const provider_names[] = {
	[AI_LLAMA]      = "llama",
	[AI_OPENAI]     = "openai",
	[AI_ANTHROPIC]  = "anthropic",
	[AI_GOOGLE]     = "google",
	[AI_DEEPINFRA]  = "deepinfra",
	[AI_OPENROUTER] = "openrouter",
};

/* What each provider has: its URL for each API it serves, Gemini's
 * generateContent's followed by the model and ":streamGenerateContent"; the
 * variables tried for its key, at its own URLs only; and its default API.
 * No provider's are those of a URL that --url gives. */
static struct {
	char const  *url[ARRAY_SIZE(api_names)];
	char const  *keys[2];
	enum ai_api  api;
} const providers[] = {
	[AI_NO_PROVIDER] = { .keys = { "ASKAI_API_KEY" }, .api = AI_CHAT },
	[AI_LLAMA] = {
		{
			[AI_CHAT]      = "http://127.0.0.1:9931/v1/chat/completions",
			[AI_RESPONSES] = "http://127.0.0.1:9931/v1/responses",
			[AI_MESSAGES]  = "http://127.0.0.1:9931/v1/messages",
		}, { "LLAMA_API_KEY" }, AI_CHAT
	},
	[AI_OPENAI] = {
		{
			[AI_CHAT]      = "https://api.openai.com/v1/chat/completions",
			[AI_RESPONSES] = "https://api.openai.com/v1/responses",
		}, { "OPENAI_API_KEY" }, AI_CHAT
	},
	[AI_ANTHROPIC] = {
		{
			[AI_CHAT]     = "https://api.anthropic.com/v1/chat/completions",
			[AI_MESSAGES] = "https://api.anthropic.com/v1/messages",
		}, { "ANTHROPIC_API_KEY" }, AI_MESSAGES
	},
	[AI_GOOGLE] = {
		{
			[AI_CHAT]     = "https://generativelanguage.googleapis.com/v1beta/"
			                "openai/chat/completions",
			[AI_GENERATE] = "https://generativelanguage.googleapis.com/v1beta/"
			                "models/",
		}, { "GEMINI_API_KEY", "GOOGLE_API_KEY" }, AI_GENERATE
	},
	[AI_DEEPINFRA] = {
		{
			[AI_CHAT] = "https://api.deepinfra.com/v1/openai/chat/completions",
		}, { "DEEPINFRA_API_KEY", "DEEPINFRA_TOKEN" }, AI_CHAT
	},
	[AI_OPENROUTER] = {
		{
			[AI_CHAT] = "https://openrouter.ai/api/v1/chat/completions",
		}, { "OPENROUTER_API_KEY" }, AI_CHAT
	},
};

static void
config_usage (struct config const *cfg,
              FILE                *out)
{
	char const *argv0 = cfg->argv[0];
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
	        "  -p, --provider NAME       llama (llama-server at 127.0.0.1:9931),\n"
	        "                            openai, anthropic, google, deepinfra or\n"
	        "                            openrouter, whose API, URL and key\n"
	        "                            variable are the defaults\n"
	        "      --api NAME            chat (completions), responses, messages\n"
	        "                            (Anthropic's) or generate (Gemini's);\n"
	        "                            default: the provider's, else chat\n"
	        "  -u, --url URL             endpoint URL (default: the provider's)\n"
	        "  -m, --model NAME          model to ask (required at a provider's\n"
	        "                            own URL, but llama's)\n"
	        "  -k, --key-env VAR         environment variable holding the API key\n"
	        "                            (default: at a provider's own URL its\n"
	        "                            usual one, which llama may lack; else\n"
	        "                            ASKAI_API_KEY, if set)\n"
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
	        "      --upstream-tools      give the upstream tools too: man, git and\n"
	        "                            the plugins' tools that allow it, as the\n"
	        "                            client's role allows, notes with --notes,\n"
	        "                            and compact; an ask call then runs them\n"
	        "                            and asks on until it answers\n"
	        "      --notes DIR           the directory of the notebooks of the\n"
	        "                            upstream and of the clients, made if it\n"
	        "                            does not exist\n"
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
	        "The configuration file's \"plugins\" member lists the plugins that\n"
	        "askai loads as it starts: an array of absolute paths (see\n"
	        "README.md).\n"
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

/* @p str, a decimal number from @p min to @p max, in @p *value; EINVAL if
 * it is not one. */
static int
config_parse_number (char const *str,
                     long long   min,
                     long long   max,
                     long long  *value)
{
	char *end;
	/* strtoll() leaves errno as it was on success. */
	errno = 0;
	long long n = strtoll(str, &end, 10);
	if (errno || end == str || *end || n < min || n > max)
		return EINVAL;
	*value = n;
	return 0;
}

/* As config_parse_number(), into a long. */
static int
config_parse_long (char const *str,
                   long        min,
                   long        max,
                   long       *value)
{
	long long n;
	int e = config_parse_number(str, min, max, &n);
	if (!e)
		*value = (long)n;
	return e;
}

/* As config_parse_number(), into a uint32_t. */
static int
config_parse_u32 (char const *str,
                  uint32_t    min,
                  uint32_t    max,
                  uint32_t   *value)
{
	long long n;
	int e = config_parse_number(str, min, max, &n);
	if (!e)
		*value = (uint32_t)n;
	return e;
}

/* A byte count, which may end in K, M or G for KiB, MiB or GiB. */
static int
config_parse_size (char const *str,
                   size_t      min,
                   size_t      max,
                   size_t     *value)
{
	static char const units[] = "KMG";
	if (*str < '0' || *str > '9')
		return EINVAL;
	char *end;
	/* strtoull() leaves errno as it was on success. */
	errno = 0;
	unsigned long long n = strtoull(str, &end, 10);
	if (errno)
		return EINVAL;
	unsigned shift = 0;
	if (*end) {
		char const *unit = strchr(units, *end);
		if (!unit || end[1])
			return EINVAL;
		shift = 10 * (unsigned)(unit - units + 1);
	}
	if (n > max >> shift || n << shift < min)
		return EINVAL;
	*value = (size_t)n << shift;
	return 0;
}

/* Whether a header line named like @p prefix's name is among cfg->headers. */
static bool
config_has_header (struct config const *cfg,
                   char const          *prefix)
{
	size_t n = strcspn(prefix, ":");
	for (char const **line = cfg->headers; *line; ++line)
		if (!strncasecmp(*line, prefix, n) && (*line)[n] == ':')
			return true;
	return false;
}

static bool
config_header_valid (char const *line)
{
	size_t n = strcspn(line, ": \t\r\n");
	return n && line[n] == ':' && !strpbrk(line, "\r\n");
}

static bool
config_tool_name_valid (char const *name)
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
	OPT_MAX_TURNS, OPT_CONTEXT, OPT_API
};

static char const shorts[] = "c:p:u:m:k:H:s:t:n:x:T:l:o:vh";

static struct option const options[] = {
	{ "config",           required_argument, nullptr, 'c' },
	{ "provider",         required_argument, nullptr, 'p' },
	{ "api",              required_argument, nullptr, OPT_API },
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
 * nullptr: on stderr, or in the log on a reload.  Returns 2, the exit status
 * for an invalid option or environment. */
[[gnu::format(printf, 3, 4)]]
static int
config_complain (struct config const *cfg,
                 char const          *where,
                 char const          *fmt,
                 ...)
{
	char what[512];
	va_list ap;
	va_start(ap, fmt);
	util_vformat(what, sizeof what, fmt, ap);
	va_end(ap);
	char const *colon = ": ";
	if (!where)
		where = colon = "";
	if (cfg->logged)
		LOG(LOG_ERROR, "%s%s%s", where, colon, what);
	else
		fprintf(stderr, "askai: %s%s%s\n", where, colon, what);
	return 2;
}

/* Say that askai is out of memory; returns 1, the exit status for it. */
static int
config_oom (struct config const *cfg)
{
	(void)config_complain(cfg, nullptr, "out of memory");
	return 1;
}

/* The option whose value is @p opt. */
static struct option const *
config_option (int opt)
{
	struct option const *o = options;
	while (o->name && o->val != opt)
		++o;
	return o;
}

/* Append @p item to the nullptr-terminated @p list of @p *n items, which
 * has room for it (config_lists()). */
static void
config_push (char const **list,
             size_t       *n,
             char const   *item)
{
	list[(*n)++] = item;
	list[*n] = nullptr;
}

/* A string of its own, printed as @p fmt says; nullptr if out of memory. */
[[gnu::format(printf, 1, 2)]]
static char *
config_aprintf (char const *fmt,
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

/* The index of @p name among the @p n @p names, which begin at 1; 0 if it
 * is none of them. */
static size_t
config_find (char const *const *names,
             size_t             n,
             char const        *name)
{
	size_t i = n;
	while (--i && strcmp(names[i], name))
		;
	return i;
}

/* Check a number in the object of --extra, as a json_c_visit() callback.
 * The function returns JSON_C_VISIT_RETURN_ERROR for a double that is NaN
 * or infinite.  For a finite double, it resets the serializer to the
 * default serializer of json-c, which prints the value of the double.  The
 * parser of json-c sets for each double a serializer that prints the text
 * of the number as the parser read it, and json-c reads some numbers that
 * JSON does not allow, such as "1.". */
static int
config_extra_number (struct json_object *val,
                      int,
                      struct json_object *,
                      char const *,
                      size_t *,
                      void *)
{
	if (!json_object_is_type(val, json_type_double))
		return JSON_C_VISIT_RETURN_CONTINUE;
	if (!isfinite(json_object_get_double(val)))
		return JSON_C_VISIT_RETURN_ERROR;
	json_object_set_serializer(val, nullptr, nullptr, nullptr);
	return JSON_C_VISIT_RETURN_CONTINUE;
}

/* Set the sampling temperature to @p arg, a finite number not below 0.
 * @p where is the path of the configuration file that contains @p arg, or
 * nullptr if @p arg is from the command line. */
static int
config_apply_temperature (struct config *cfg,
                          char const    *arg,
                          char const    *where)
{
	struct json_object *t = jb_parse(arg, strlen(arg), JSON_TOKENER_STRICT, 0);
	enum json_type type = json_object_get_type(t);
	double d = json_object_get_double(t);
	/* !(d >= 0) is true for a negative number and for NaN. */
	if ((type != json_type_double && type != json_type_int) || !(d >= 0) || isinf(d)) {
		json_object_put(t);
		return config_complain(cfg, where, "invalid temperature: %s", arg);
	}
	/* The default serializer prints the value of the number, not the text
	 * that json-c read (config_extra_number()). */
	json_object_set_serializer(t, nullptr, nullptr, nullptr);
	json_object_put(cfg->ep.temperature);
	cfg->ep.temperature = t;
	return 0;
}

/* Set the object that is merged into each request body to @p arg, a JSON
 * object whose numbers are finite.  @p where is the path of the
 * configuration file that contains @p arg, or nullptr if @p arg is from the
 * command line. */
static int
config_apply_extra (struct config *cfg,
                    char const    *arg,
                    char const    *where)
{
	struct json_object *x = jb_parse(arg, strlen(arg), JSON_TOKENER_STRICT, 0);
	if (!json_object_is_type(x, json_type_object)) {
		json_object_put(x);
		return config_complain(cfg, where, "--extra needs a JSON object: %s", arg);
	}
	if (json_c_visit(x, 0, config_extra_number, nullptr)) {
		json_object_put(x);
		return config_complain(cfg, where, "--extra contains NaN, an infinity "
		                       "or a number outside the range of a double: %s", arg);
	}
	json_object_put(cfg->ep.extra);
	cfg->ep.extra = x;
	return 0;
}

/* Apply option @p opt, which takes a number, with @p arg, as config_apply()
 * says. */
static int
config_apply_number (struct config *cfg,
                     int            opt,
                     char const    *arg,
                     char const    *where)
{
	struct ai_endpoint *ep = &cfg->ep;
	struct httpd_limits *lim = &cfg->http.limits;
	struct mcp_limits *mlim = &cfg->mcp;
	struct brainstem_cfg *bs = &cfg->brainstem;
	size_t size = 0;
	uint32_t n = 0;
	int e;
	switch (opt) {
	case OPT_MAX_HEAD:
		e = config_parse_size(arg, 1, UINT16_MAX, &size);
		lim->head = (uint32_t)size;
		break;
	case OPT_MAX_FIELDS:
		e = config_parse_u32(arg, 0, UINT32_MAX, &lim->fields);
		break;
	case OPT_HEAD_TIMEOUT:
		e = config_parse_u32(arg, 0, UINT32_MAX, &lim->head_time);
		break;
	case OPT_MAX_BODY:
		e = config_parse_size(arg, 0, SIZE_MAX / 2, &lim->body);
		break;
	case OPT_MIN_BODY_RATE:
		e = config_parse_size(arg, 0, UINT32_MAX, &size);
		lim->body_rate = (uint32_t)size;
		break;
	case OPT_BODY_GRACE:
		e = config_parse_u32(arg, 0, UINT32_MAX, &lim->body_grace);
		break;
	case OPT_BODY_BUDGET:
		e = config_parse_size(arg, 0, SIZE_MAX / 2, &lim->bodies);
		break;
	case OPT_CLIENT_BUDGET:
		e = config_parse_size(arg, 1, SIZE_MAX / 2, &lim->client_bodies);
		break;
	case OPT_MAX_DEPTH:
		e = config_parse_u32(arg, 1, INT32_MAX - 1, &mlim->depth);
		break;
	case OPT_MAX_VALUES:
		e = config_parse_u32(arg, 1, UINT32_MAX, &mlim->values);
		break;
	case OPT_MAX_MEMBERS:
		e = config_parse_u32(arg, 1, UINT32_MAX - 1, &mlim->members);
		break;
	case OPT_SSE_KEEPALIVE:
		e = config_parse_u32(arg, 0, UINT32_MAX / 2, &mlim->keepalive);
		break;
	case OPT_MAX_CLIENT_CONNS:
		e = config_parse_u32(arg, 0, UINT32_MAX, &lim->clients);
		break;
	case OPT_IDLE_TIMEOUT:
		e = config_parse_u32(arg, 0, UINT32_MAX, &lim->idle_time);
		break;
	case OPT_OUTPUT_PAUSE:
		e = config_parse_size(arg, 1, SIZE_MAX / 2, &lim->out_pause);
		break;
	case OPT_MAX_OUTPUT:
		e = config_parse_size(arg, 1, SIZE_MAX / 2, &lim->out_max);
		break;
	case OPT_SEND_TIMEOUT:
		e = config_parse_u32(arg, 0, UINT32_MAX, &lim->write_time);
		break;
	case OPT_GRACE:
		e = config_parse_u32(arg, 1, 3600, &n);
		bs->grace_ms = n * 1000;
		break;
	case OPT_WATCHDOG:
		e = config_parse_u32(arg, 0, 60'000, &bs->watchdog_ms);
		break;
	case OPT_TEST_STALL:
		e = config_parse_u32(arg, 1, 3'600'000, &n);
		bs->stall_ms = n;
		break;
	case OPT_MEMORY_HIGH:
		e = config_parse_size(arg, 1, SIZE_MAX / 2, &size);
		bs->memory_high = size;
		break;
	case OPT_MAX_CALLS:
		e = config_parse_u32(arg, 1, 1'000'000, &cfg->calls);
		break;
	case OPT_SLOTS:
		e = config_parse_u32(arg, 1, UINT16_MAX, &ep->slots);
		break;
	case OPT_CHUNK_TIMEOUT:
		e = config_parse_long(arg, 0, INT32_MAX / 1000, &ep->chunk_ms);
		ep->chunk_ms *= 1000;
		break;
	case OPT_TEST_MAN_RECHECK:
		e = config_parse_u32(arg, 0, 86'400, &cfg->man_recheck);
		break;
	case OPT_GIT_TIMEOUT:
		e = config_parse_u32(arg, 1, 3600, &n);
		cfg->git_timeout = n * 1'000'000'000ULL;
		break;
	case OPT_MAX_TURNS:
		e = config_parse_u32(arg, 1, 1000, &cfg->turns);
		break;
	case OPT_CONTEXT:
		e = config_parse_long(arg, 0, INT32_MAX, &ep->context);
		break;
	case OPT_LOOPS:
		/* As many as a handle's loop field names, home being 0. */
		e = config_parse_u32(arg, 0, 255, &cfg->loops);
		break;
	default:
		return 2;
	}
	if (e)
		return config_complain(cfg, where, "invalid --%s: %s",
		                       config_option(opt)->name, arg);
	return 0;
}

/* Apply option @p opt with @p arg, given in the configuration file @p where,
 * or on the command line if that is nullptr; returns 0, or 2, having said
 * why. */
static int
config_apply (struct config *cfg,
              int            opt,
              char const    *arg,
              char const    *where)
{
	struct ai_endpoint *ep = &cfg->ep;
	switch (opt) {
	case 'p': {
		size_t i = config_find(provider_names, ARRAY_SIZE(provider_names), arg);
		if (!i)
			return config_complain(cfg, where, "unknown provider: %s", arg);
		ep->provider = (enum ai_provider)i;
		break;
	}
	case OPT_API: {
		size_t i = config_find(api_names, ARRAY_SIZE(api_names), arg);
		if (!i)
			return config_complain(cfg, where, "unknown API: %s", arg);
		ep->api = (enum ai_api)i;
		break;
	}
	case 'u':
		if (strncmp(arg, "http://", sizeof "http://" - 1) &&
		    strncmp(arg, "https://", sizeof "https://" - 1))
			return config_complain(cfg, where, "not an http(s) URL: %s", arg);
		ep->url = arg;
		break;
	case 'm': ep->model = arg; break;
	case 'k': cfg->key_env = arg; break;
	case 's': ep->system = arg; break;
	case 'l':
		/* The command line's listeners replace the file's. */
		if (!where && cfg->listen_file)
			cfg->n_listens = 0;
		cfg->listen_file = where != nullptr;
		config_push(cfg->listens, &cfg->n_listens, arg);
		break;
	case 'v': cfg->flags |= CFG_VERBOSE; break;
	case OPT_TOOL_DESCRIPTION: cfg->tool.description = arg; break;
	case OPT_TLS_CERT: cfg->tls_cert = arg; break;
	case OPT_TLS_KEY: cfg->tls_key = arg; break;
	case OPT_TLS_ALLOW_RSA: cfg->flags |= CFG_TLS_RSA; break;
	case OPT_UPSTREAM_TOOLS: cfg->flags |= CFG_UPSTREAM_TOOLS; break;
	case OPT_NOTES: cfg->notes = arg; break;
	case 'o':
		config_push(cfg->origins, &cfg->n_origins, arg);
		break;
	case 'H':
		if (!config_header_valid(arg))
			return config_complain(cfg, where, "invalid header line: %s", arg);
		config_push(cfg->headers, &cfg->n_headers, arg);
		break;
	case 't':
		return config_apply_temperature(cfg, arg, where);
	case 'n':
		if (config_parse_long(arg, 1, INT32_MAX, &ep->max_tokens))
			return config_complain(cfg, where, "invalid max token count: %s", arg);
		break;
	case 'x':
		return config_apply_extra(cfg, arg, where);
	case 'T':
		if (config_parse_long(arg, 0, INT32_MAX / 1000, &ep->timeout_ms))
			return config_complain(cfg, where, "invalid timeout: %s", arg);
		ep->timeout_ms *= 1000;
		break;
	case OPT_TOOL_NAME:
		if (!config_tool_name_valid(arg))
			return config_complain(cfg, where, "tool names are 1-128 of "
			                       "[A-Za-z0-9_.-]: %s", arg);
		cfg->tool.name = arg;
		break;
	case OPT_TEST_PRESSURE:
		cfg->brainstem.pressure = arg;
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
	case OPT_GIT:
		config_push(cfg->gits, &cfg->n_gits, arg);
		break;
	default:
		return config_apply_number(cfg, opt, arg, where);
	}
	return 0;
}

/* Say that @p val, a member of the configuration file or an element of one,
 * is no value of option @p o; returns 2. */
static int
config_invalid (struct config const *cfg,
                struct option const *o,
                struct json_object  *val)
{
	char const *text = json_object_to_json_string_ext(val, JSON_C_TO_STRING_PLAIN);
	return config_complain(cfg, cfg->path, "invalid --%s: %s", o->name, text);
}

/* The argument that @p val gives option @p o, which takes one: a string, or
 * a number or, for --extra, an object, as JSON text; nullptr if it is none
 * of those. */
static char const *
config_value_arg (struct option const *o,
                  struct json_object  *val)
{
	switch (json_object_get_type(val)) {
	case json_type_string:
		return json_object_get_string(val);
	case json_type_object:
		if (o->val != 'x')
			return nullptr;
		[[fallthrough]];
	case json_type_int:
	case json_type_double:
		return json_object_to_json_string_ext(val, JSON_C_TO_STRING_PLAIN);
	default:
		return nullptr;
	}
}

/* Apply @p val, a member of the configuration file or an element of one, as
 * option @p o: true for an option that takes no argument, else the
 * argument config_value_arg() finds. */
static int
config_apply_value (struct config       *cfg,
                    struct option const *o,
                    struct json_object  *val)
{
	if (o->has_arg == no_argument) {
		if (!json_object_is_type(val, json_type_boolean))
			return config_invalid(cfg, o, val);
		if (!json_object_get_boolean(val))
			return 0;
		return config_apply(cfg, o->val, nullptr, cfg->path);
	}
	char const *arg = config_value_arg(o, val);
	if (!arg)
		return config_invalid(cfg, o, val);
	return config_apply(cfg, o->val, arg, cfg->path);
}

/* Apply @p val, a member of the configuration file, as option @p o: a value
 * config_apply_value() takes, or an array of them for a repeatable option. */
static int
config_apply_member (struct config       *cfg,
                     struct option const *o,
                     struct json_object  *val)
{
	bool many = o->val == 'H' || o->val == 'o' || o->val == 'l' ||
	            o->val == OPT_GIT;
	if (!many || !json_object_is_type(val, json_type_array))
		return config_apply_value(cfg, o, val);
	size_t n = json_object_array_length(val);
	for (size_t i = 0; i < n; ++i) {
		struct json_object *v = json_object_array_get_idx(val, i);
		/* Not of arrays, as config_lists() counts them. */
		int status = json_object_is_type(v, json_type_array)
		           ? config_invalid(cfg, o, val)
		           : config_apply_value(cfg, o, v);
		if (status)
			return status;
	}
	return 0;
}

/* Whether @p st's file only root and askai's user may change: for a
 * directory, others may add entries to it if only their owners may remove
 * them (the sticky bit, as on /tmp). */
static bool
config_safe (struct stat const *st)
{
	return (!st->st_uid || st->st_uid == geteuid()) &&
	       (!(st->st_mode & (S_IWGRP | S_IWOTH)) ||
	        (S_ISDIR(st->st_mode) && st->st_mode & S_ISVTX));
}

/* Whether every directory above @p path is config_safe(); false if that
 * cannot be told. */
static bool
config_safe_dirs (char const *path)
{
	char *real = realpath(path, nullptr);
	if (!real)
		return false;
	bool ok = true;
	struct stat st;
	/* Each but the root, from the nearest; the root last. */
	for (char *slash = strrchr(real, '/'); ok && slash != real;
	     slash = strrchr(real, '/')) {
		*slash = '\0';
		ok = !stat(real, &st) && config_safe(&st);
	}
	free(real);
	return ok && !stat("/", &st) && config_safe(&st);
}

/* Read up to @p size bytes of the file open as @p fd into @p buf, as many as
 * there are, their count into @p *p_len; 0, or why a read failed. */
static int
config_read (int     fd,
             char   *buf,
             size_t  size,
             size_t *p_len)
{
	*p_len = 0;
	while (*p_len < size) {
		ssize_t n = read(fd, buf + *p_len, size - *p_len);
		if (n > 0)
			*p_len += (size_t)n;
		else if (!n)
			return 0;
		else if (errno != EINTR)
			return errno;
	}
	return 0;
}

/* As config_slurp() says, the file at @p path being open as @p fd. */
static int
config_slurp_fd (struct config const *cfg,
                 int                  fd,
                 char const          *path,
                 mode_t               hide,
                 char               **p_text,
                 size_t              *p_len)
{
	struct stat st;
	if (fstat(fd, &st))
		return config_complain(cfg, nullptr, "cannot open %s: %s", path,
		                       strerror(errno));
	if (!S_ISREG(st.st_mode))
		return config_complain(cfg, path, "not a regular file");
	if (st.st_size > FILE_MAX)
		return config_complain(cfg, path, "larger than 1 MiB");
	if (!config_safe(&st) || !config_safe_dirs(path))
		return config_complain(cfg, path, "others than root and askai's user "
		                       "may change it");
	if (st.st_mode & hide)
		return config_complain(cfg, path, "others may read it");
	char *text = malloc((size_t)st.st_size + 1);
	if (!text)
		return config_oom(cfg);
	size_t len;
	int e = config_read(fd, text, (size_t)st.st_size, &len);
	if (e) {
		explicit_bzero(text, len);
		free(text);
		return config_complain(cfg, path, "%s", strerror(e));
	}
	text[len] = '\0';
	*p_text = text;
	*p_len = len;
	return 0;
}

/* Read the file at @p path whole into @p *p_text, NUL-terminated, its
 * length into @p *p_len: a regular file of FILE_MAX bytes at most that only
 * root and askai's user may change, nor replace, and that has none of the
 * permission bits @p hide, such as S_IROTH for a secret, which is wiped
 * from memory freed here.  Returns 0, or the exit status, having said why. */
static int
config_slurp (struct config const *cfg,
              char const          *path,
              mode_t               hide,
              char               **p_text,
              size_t              *p_len)
{
	/* Not blocking, a FIFO opens at once, and fails the check. */
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return config_complain(cfg, nullptr, "cannot open %s: %s", path,
		                       strerror(errno));
	int status = config_slurp_fd(cfg, fd, path, hide, p_text, p_len);
	close(fd);
	return status;
}

/* Say that the configuration file's @p text is not as config_json() says,
 * @p what being wrong at byte @p end; returns 2. */
static int
config_json_fail (struct config const *cfg,
                  char const          *text,
                  size_t               end,
                  char const          *what)
{
	size_t line = 1;
	size_t column = 1;
	for (size_t i = 0; i < end; ++i, ++column)
		if (text[i] == '\n') {
			++line;
			column = 0;
		}
	return config_complain(cfg, cfg->path, "%zu:%zu: %s", line, column, what);
}

/* Parse the configuration file's @p len bytes of @p text into cfg->file:
 * a JSON object, which may hold comments, and blanks after it at most.
 * Returns 0, or the exit status, having said why. */
static int
config_json (struct config *cfg,
             char const    *text,
             size_t         len)
{
	struct json_tokener *tok = json_tokener_new();
	if (!tok)
		return config_oom(cfg);
	/* config_fini() puts what this gives, whatever the status. */
	cfg->file = json_tokener_parse_ex(tok, text, (int)len);
	size_t end = json_tokener_get_parse_end(tok);
	enum json_tokener_error e = json_tokener_get_error(tok);
	json_tokener_free(tok);
	if (!cfg->file)
		return config_json_fail(cfg, text, end, json_tokener_error_desc(e));
	if (!json_object_is_type(cfg->file, json_type_object))
		return config_json_fail(cfg, text, 0, "not one JSON object");
	end += strspn(text + end, " \t\r\n");
	if (end < len)
		return config_json_fail(cfg, text, end, "not one JSON object");
	return 0;
}

/* Whether the configuration file's member @p key is of its policy, who may
 * connect and what they may do (src/policy.h), rather than an option. */
static bool
config_of_policy (char const *key)
{
	return !strcmp(key, "roles") || !strcmp(key, "identities") ||
	       !strcmp(key, "unix-groups") || !strcmp(key, "anonymous");
}

/* Whether the configuration file's member @p key takes effect on a reload:
 * the policy's, and TLS's certificate, key and key types. */
static bool
config_reloadable (char const *key)
{
	return config_of_policy(key) || !strncmp(key, "tls-", sizeof "tls-" - 1);
}

/* The members of @p cfg's configuration file, if it has one, but those
 * @p but is true of. */
static size_t
config_members_but (struct config const *cfg,
                    bool               (*but) (char const *key))
{
	size_t n = 0;
	struct json_object_iter it;
	if (cfg->file)
		json_object_object_foreachC(cfg->file, it)
			n += !but(it.key);
	return n;
}

/* Whether @p cfg's configuration file and @p from's, if either has one, set
 * the same options, those a reload takes aside. */
static bool
config_same_options (struct config const *cfg,
                     struct config const *from)
{
	if (config_members_but(cfg, config_reloadable) !=
	    config_members_but(from, config_reloadable))
		return false;
	if (!from->file)
		return true;
	struct json_object_iter it;
	json_object_object_foreachC(from->file, it) {
		struct json_object *v;
		if (!config_reloadable(it.key) &&
		    (!json_object_object_get_ex(cfg->file, it.key, &v) ||
		     !json_object_equal(it.val, v)))
			return false;
	}
	return true;
}

/* Make room for the lists of headers, origins, listeners and repositories,
 * in one block, and point ep.headers, http.origins and http.listen at their
 * lists: each may hold every argument, every string of the configuration
 * file and the three headers askai adds, and ends in nullptr.  Returns 0, or
 * 1 if out of memory, having said so. */
static int
config_lists (struct config *cfg)
{
	size_t room = (size_t)cfg->argc + 4;
	struct json_object_iter it;
	if (cfg->file)
		json_object_object_foreachC(cfg->file, it)
			room += json_object_is_type(it.val, json_type_array)
			      ? json_object_array_length(it.val) : 1;
	char const **block = malloc(4 * room * sizeof *block);
	if (!block)
		return config_oom(cfg);
	cfg->headers = block;
	cfg->origins = block + room;
	cfg->listens = block + 2 * room;
	cfg->gits = block + 3 * room;
	*cfg->headers = *cfg->origins = *cfg->listens = *cfg->gits = nullptr;
	cfg->ep.headers = cfg->headers;
	cfg->http.origins = cfg->origins;
	cfg->http.listen = cfg->listens;
	return 0;
}

/* Apply the configuration file's members but its policy's as the options of
 * their names.  The member "plugins" is not an option either:
 * config_registry() reads it. */
static int
config_apply_file (struct config *cfg)
{
	struct json_object_iter it;
	json_object_object_foreachC(cfg->file, it) {
		if (config_of_policy(it.key) || !strcmp(it.key, "plugins"))
			continue;
		struct option const *o = options;
		while (o->name && strcmp(o->name, it.key))
			++o;
		if (!o->name || o->val == 'c' || o->val == 'h')
			return config_complain(cfg, cfg->path, "unknown option: %s", it.key);
		int status = config_apply_member(cfg, o, it.val);
		if (status)
			return status;
	}
	return 0;
}

/* Read the configuration file, which config_slurp() reads, as config_json()
 * says, make room for the lists, and apply the file's options. */
static int
config_file (struct config *cfg)
{
	char *text;
	size_t len;
	int status = config_slurp(cfg, cfg->path, 0, &text, &len);
	if (status)
		return status;
	status = config_json(cfg, text, len);
	free(text);
	if (status)
		return status;
	status = config_lists(cfg);
	if (status)
		return status;
	return config_apply_file(cfg);
}

/* Check the limits on clients against each other. */
static int
config_check (struct config const *cfg)
{
	struct httpd_limits const *lim = &cfg->http.limits;
	if (lim->body > lim->bodies)
		return config_complain(cfg, nullptr, "--max-body exceeds --body-budget");
	if (lim->client_bodies && lim->body > lim->client_bodies)
		return config_complain(cfg, nullptr, "--max-body exceeds --client-budget");
	if (lim->out_pause > lim->out_max)
		return config_complain(cfg, nullptr, "--output-pause exceeds --max-output");
	return 0;
}

/* Whether @p path is a work tree's top or a git directory, so that git
 * finds no repository around it. */
static bool
config_git_repo (char const *path)
{
	int fd = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return false;
	struct stat st;
	bool repo = !fstatat(fd, ".git", &st, 0) ||
	            (!fstatat(fd, "HEAD", &st, 0) && !fstatat(fd, "objects", &st, 0));
	close(fd);
	return repo;
}

/* Check that repository @p i is one config_git_repo() is true of, named
 * apart from those before it. */
static int
config_check_git (struct config const *cfg,
                  size_t               i)
{
	char const *path = cfg->gits[i];
	size_t len;
	char const *name = git_name(path, &len);
	if (!name)
		return config_complain(cfg, nullptr, "--git: %s names no repository: "
		                       "its last component, less a .git suffix, must "
		                       "be 1 to 64 of [A-Za-z0-9_.-], not beginning "
		                       "with a dot", path);
	if (!config_git_repo(path))
		return config_complain(cfg, nullptr, "--git: not a git repository: %s",
		                       path);
	for (size_t j = 0; j < i; ++j) {
		size_t other;
		char const *o = git_name(cfg->gits[j], &other);
		if (other == len && !memcmp(o, name, len))
			return config_complain(cfg, nullptr, "--git: two repositories "
			                       "named %.*s", (int)len, name);
	}
	return 0;
}

static int
config_check_gits (struct config const *cfg)
{
	for (size_t i = 0; i < cfg->n_gits; ++i) {
		int status = config_check_git(cfg, i);
		if (status)
			return status;
	}
	return 0;
}

/* Settle the upstream's API, unless --api chose it: its provider's default.
 * A provider must serve it; with none, --url must say where it is. */
static int
config_api (struct config *cfg)
{
	struct ai_endpoint *ep = &cfg->ep;
	if (!ep->api)
		ep->api = providers[ep->provider].api;
	if (!ep->provider)
		return ep->url ? 0 : config_complain(cfg, nullptr, "--provider or --url "
		                                     "is required");
	if (providers[ep->provider].url[ep->api])
		return 0;
	return config_complain(cfg, nullptr, "%s has no %s API",
	                       provider_names[ep->provider], api_names[ep->api]);
}

/* Check that the upstream may call tools, if it is to. */
static int
config_check_upstream (struct config const *cfg)
{
	if (!(cfg->flags & CFG_UPSTREAM_TOOLS))
		return 0;
	/* The turns follow the conversation. */
	char const *key = ai_format(cfg->ep.api)->conversation;
	if (json_object_object_get_ex(cfg->ep.extra, key, nullptr))
		return config_complain(cfg, nullptr, "--extra may not change %s with "
		                       "--upstream-tools", key);
	return 0;
}

/* Checks that only root and askai's user can make the directory of the
 * notes, which does not exist yet, in the directory above it. */
static int
config_check_notes_parent (struct config const *cfg)
{
	char *copy = strdup(cfg->notes);
	if (!copy)
		return config_oom(cfg);
	char const *dir = dirname(copy);
	struct stat st;
	int status = 0;
	if (stat(dir, &st))
		status = config_complain(cfg, nullptr, "--notes: %s: %s", cfg->notes,
		                         strerror(errno));
	else if (!S_ISDIR(st.st_mode) || !config_safe(&st) || !config_safe_dirs(dir))
		status = config_complain(cfg, nullptr, "--notes: %s: others than root "
		                         "and askai's user may change the directory it "
		                         "would be made in", cfg->notes);
	free(copy);
	return status;
}

/* Checks the directory of the notes, if --notes names one.  Only root and
 * askai's user may be able to change it, or to make it if it does not
 * exist. */
static int
config_check_notes (struct config const *cfg)
{
	char const *path = cfg->notes;
	if (!path)
		return 0;
	struct stat st;
	if (stat(path, &st))
		return errno == ENOENT ? config_check_notes_parent(cfg)
		                       : config_complain(cfg, nullptr, "--notes: %s: %s",
		                                         path, strerror(errno));
	if (!S_ISDIR(st.st_mode))
		return config_complain(cfg, nullptr, "--notes: %s: not a directory",
		                       path);
	if (!config_safe(&st) || !config_safe_dirs(path))
		return config_complain(cfg, nullptr, "--notes: %s: others than root "
		                       "and askai's user may change it", path);
	return 0;
}

/* Take @p key as the API key, in @p *p_key, unless it has a line break. */
static int
config_key_take (struct config const *cfg,
                 char const          *key,
                 char const         **p_key)
{
	if (strpbrk(key, "\r\n"))
		return config_complain(cfg, nullptr, "the API key contains a line "
		                       "break");
	*p_key = key;
	return 0;
}

/* The API key, in @p *p_key, from the variable --key-env names. */
static int
config_key_env (struct config const *cfg,
                char const         **p_key)
{
	char const *key = getenv(cfg->key_env);
	if (!key || !*key)
		return config_complain(cfg, nullptr, "%s is not set", cfg->key_env);
	return config_key_take(cfg, key, p_key);
}

/*
 * The API key, in @p *p_key, unless a header gives one, as @p has_auth says:
 * from the first of the usual variables that is set, the provider's at its
 * own URL, which llama-server may do without, else ASKAI_API_KEY, which a
 * URL that --url gives may do without.  A provider's variables are only read
 * for its own URL, so pointing --url elsewhere never leaks its key.
 */
static int
config_key_usual (struct config const *cfg,
                  bool                 has_auth,
                  char const         **p_key)
{
	if (has_auth)
		return 0;
	enum ai_provider p = cfg->ep.url ? AI_NO_PROVIDER : cfg->ep.provider;
	char const *const *vars = providers[p].keys;
	for (size_t i = 0; i < ARRAY_SIZE(providers[0].keys) && vars[i]; ++i) {
		char const *key = getenv(vars[i]);
		if (key && *key)
			return config_key_take(cfg, key, p_key);
	}
	if (ai_llama_like(p))
		return 0;
	return config_complain(cfg, nullptr, "set %s or pass --key-env", vars[0]);
}

/* Point the upstream at the provider's own URL for its API: for Gemini's
 * generateContent, at the model's method that streams, in SSE. */
static int
config_url_own (struct config *cfg)
{
	struct ai_endpoint *ep = &cfg->ep;
	if (!ep->model && !ai_llama_like(ep->provider))
		return config_complain(cfg, nullptr, "--model is required for %s's "
		                       "own API", provider_names[ep->provider]);
	ep->url = providers[ep->provider].url[ep->api];
	if (ep->api != AI_GENERATE)
		return 0;
	cfg->url = config_aprintf("%s%s:streamGenerateContent?alt=sse", ep->url,
	                          ep->model);
	if (!cfg->url)
		return config_oom(cfg);
	ep->url = cfg->url;
	return 0;
}

/* The '?' or '&' that begins the alt parameter of @p query, a URL's query
 * or ""; nullptr if it has none. */
static char const *
config_url_alt (char const *query)
{
	char const *alt = strstr(query, "?alt=");
	return alt ? alt : strstr(query, "&alt=");
}

/* Gemini streams from :streamGenerateContent, in SSE with alt=sse, whatever
 * alt the URL that --url gives had. */
static int
config_url_generate (struct config *cfg)
{
	static char const method[] = ":generateContent";
	size_t m = sizeof method - 1;
	if (cfg->ep.api != AI_GENERATE)
		return 0;
	char const *url = cfg->ep.url;
	char const *query = url + strcspn(url, "?");
	/* The path, less its method if that is the one that does not stream. */
	size_t path = (size_t)(query - url);
	char const *stream = "";
	if (path >= m && !memcmp(query - m, method, m)) {
		path -= m;
		stream = ":streamGenerateContent";
	}
	/* The query up to its alt's value, and what follows that; or all of it,
	 * and alt. */
	char const *alt = config_url_alt(query);
	if (alt) {
		char const *value = alt + sizeof "?alt=" - 1;
		cfg->url = config_aprintf("%.*s%s%.*salt=sse%s", (int)path, url, stream,
		                          (int)(alt + 1 - query), query,
		                          value + strcspn(value, "&"));
	} else {
		cfg->url = config_aprintf("%.*s%s%s%salt=sse", (int)path, url, stream,
		                          query, *query ? "&" : "?");
	}
	if (!cfg->url)
		return config_oom(cfg);
	cfg->ep.url = cfg->url;
	return 0;
}

/* Add the headers that askai sends unless they are given: the API key's
 * with @p key, unless that is nullptr, anthropic-version for Messages, and
 * User-Agent. */
static int
config_headers (struct config *cfg,
                char const    *key)
{
	if (key) {
		cfg->auth = config_aprintf("%s%s", auths[cfg->ep.api], key);
		if (!cfg->auth)
			return config_oom(cfg);
		config_push(cfg->headers, &cfg->n_headers, cfg->auth);
	}
	if (cfg->ep.api == AI_MESSAGES &&
	    !config_has_header(cfg, "anthropic-version:"))
		config_push(cfg->headers, &cfg->n_headers, "anthropic-version: 2023-06-01");
	if (!config_has_header(cfg, "user-agent:"))
		config_push(cfg->headers, &cfg->n_headers, "User-Agent: askai/0.1");
	return 0;
}

/* The tool's description, unless one is given. */
static int
config_description (struct config *cfg)
{
	if (cfg->tool.description)
		return 0;
	char const *who = "another AI model";
	char const *model = "";
	if (cfg->ep.model) {
		who = "the AI model ";
		model = cfg->ep.model;
	}
	cfg->description = config_aprintf("Ask %s%s a question or give it a task, and get "
	                                  "its answer. It cannot see this conversation, your "
	                                  "files or your tools, so write a complete, "
	                                  "self-contained prompt.%s%s Use it for a second "
	                                  "opinion, for hard problems, or for knowledge you "
	                                  "lack.", who, model,
	                                  cfg->flags & CFG_UPSTREAM_TOOLS
	                                  ? " It may use some of this server's tools "
	                                    "itself, and its answer lists the calls it "
	                                    "made." : "",
	                                  cfg->flags & CFG_UPSTREAM_TOOLS && cfg->notes
	                                  ? " It keeps notes in a thread, which a later "
	                                    "call may go on with." : "");
	if (!cfg->description)
		return config_oom(cfg);
	cfg->tool.description = cfg->description;
	return 0;
}

/* Work out the upstream's API key, URL and headers, and the tool's
 * description. */
static int
config_upstream (struct config *cfg)
{
	bool has_auth = config_has_header(cfg, auths[cfg->ep.api]);
	char const *key = nullptr;
	int status = cfg->key_env ? config_key_env(cfg, &key)
	                          : config_key_usual(cfg, has_auth, &key);
	if (status)
		return status;
	status = cfg->ep.url ? config_url_generate(cfg) : config_url_own(cfg);
	if (status)
		return status;
	status = config_headers(cfg, has_auth ? nullptr : key);
	if (status)
		return status;
	return config_description(cfg);
}

/* Make TLS's context of the certificate chain of @p chain_len bytes at
 * @p chain and the key that --tls-key's file holds, which none but its owner
 * and its group may read, wiped once read. */
static int
config_tls_key (struct config *cfg,
                char const    *chain,
                size_t         chain_len)
{
	char *key;
	size_t len;
	int status = config_slurp(cfg, cfg->tls_key, S_IROTH, &key, &len);
	if (status)
		return status;
	char why[256];
	int e;
	cfg->tls = tls_ctx_create(cfg, chain, chain_len, key, len, why, sizeof why, &e);
	explicit_bzero(key, len);
	free(key);
	if (e == ENOMEM)
		return config_oom(cfg);
	if (e)
		return config_complain(cfg, nullptr, "TLS: %s", why);
	cfg->http.tls = cfg->tls;
	return 0;
}

/* Make the TLS listeners' context from the certificate chain's file and its
 * key's, the chain wiped once read too, as one file may hold the two. */
static int
config_tls (struct config *cfg)
{
	if (!cfg->tls_cert || !cfg->tls_key)
		return config_complain(cfg, nullptr, "a tls: listener needs --tls-cert "
		                       "and --tls-key");
	char *chain;
	size_t len;
	int status = config_slurp(cfg, cfg->tls_cert, 0, &chain, &len);
	if (status)
		return status;
	status = config_tls_key(cfg, chain, len);
	explicit_bzero(chain, len);
	free(chain);
	return status;
}

/* Add the listen address 127.0.0.1:8090 if cfg->listens is empty.  Refuse a
 * listen address that httpd_listen_addr() cannot parse.  If an address begins with
 * "tls:", make the TLS context. */
static int
config_listens (struct config *cfg)
{
	if (!cfg->n_listens)
		config_push(cfg->listens, &cfg->n_listens, "127.0.0.1:8090");
	bool tls = false;
	for (size_t i = 0; i < cfg->n_listens; ++i) {
		int e;
		struct httpd_listen_addr const a = httpd_listen_addr(cfg->listens[i], &e);
		if (e)
			return config_complain(cfg, cfg->listen_file ? cfg->path : nullptr,
			                       "invalid --listen: %s: %s", cfg->listens[i],
			                       strerror(e));
		tls |= a.tls;
	}
	return tls ? config_tls(cfg) : 0;
}

/* Note that ASKAI_TOKEN goes unused if the configuration file names who may
 * connect, but on a reload. */
static void
config_note_token (struct config const *cfg)
{
	char const *token = getenv("ASKAI_TOKEN");
	if (cfg->logged || !token || !*token || !cfg->file)
		return;
	if (config_members_but(cfg, config_of_policy) <
	    (size_t)json_object_object_length(cfg->file))
		(void)config_complain(cfg, nullptr, "note: ASKAI_TOKEN is ignored, as "
		                      "%s names who may connect", cfg->path);
}

/* Checks the plugin file at @p path.  Only root and askai's user may change
 * the file and the directories above it.  The path must be the file's real
 * path, so it has no symbolic link.  Then nobody else can replace the file
 * between this check and the dlopen() in plugin.c. */
static int
config_check_plugin (struct config const *cfg,
                     char const          *path)
{
	char *real = realpath(path, nullptr);
	if (!real)
		return config_complain(cfg, path, "cannot open it: %s", strerror(errno));
	int status = strcmp(real, path)
	           ? config_complain(cfg, path, "not the file's real path, which "
	                             "is %s", real)
	           : 0;
	free(real);
	if (status)
		return status;
	struct stat st;
	if (stat(path, &st))
		return config_complain(cfg, path, "cannot open it: %s", strerror(errno));
	if (!config_safe(&st) || !config_safe_dirs(path))
		return config_complain(cfg, path, "others than root and askai's user "
		                       "may change it");
	return 0;
}

/* Loads the plugin whose path is @p v, an element of the configuration
 * file's "plugins", and adds its tools to the registry. */
static int
config_plugin (struct config      *cfg,
               struct json_object *v)
{
	char const *path = json_object_get_string(v);
	if (!json_object_is_type(v, json_type_string) || *path != '/')
		return config_complain(cfg, cfg->path, "plugins: not an absolute "
		                       "path: %s", path ? path : "null");
	int status = config_check_plugin(cfg, path);
	if (status)
		return status;
	char why[256];
	int e = registry_load(&cfg->registry, path, why, sizeof why);
	if (e == ENOMEM)
		return config_oom(cfg);
	if (e)
		return config_complain(cfg, path, "%s", why);
	return 0;
}

/* Fills the registry with the tools that askai offers: its own, then those
 * of the plugins that the configuration file lists in "plugins", an array
 * of absolute paths, loaded in that order.  A reload adds only askai's own
 * tools, so that it refuses the same --tool-name as a start does.  A reload
 * loads no plugin, and config_policy() reads the registry of cfg->running. */
static int
config_registry (struct config *cfg)
{
	/* The names of askai's own tools are fixed and distinct, except the
	 * ask tool's. */
	int e = mcp_register(&cfg->registry, cfg);
	if (e)
		return config_complain(cfg, nullptr, "--tool-name: askai has another "
		                       "tool named %s", cfg->tool.name);
	struct json_object *list;
	if (cfg->running || !json_object_object_get_ex(cfg->file, "plugins", &list))
		return 0;
	if (!json_object_is_type(list, json_type_array))
		return config_complain(cfg, cfg->path, "plugins: not an array");
	size_t n = json_object_array_length(list);
	for (size_t i = 0; i < n; ++i) {
		int status = config_plugin(cfg, json_object_array_get_idx(list, i));
		if (status)
			return status;
	}
	return 0;
}

/* Make the policy, who may connect and what they may do: the roles name
 * the tools that askai runs, which on a reload are the running
 * configuration's. */
static int
config_policy (struct config *cfg)
{
	struct registry const *reg = cfg->running ? &cfg->running->registry
	                                          : &cfg->registry;
	/* The tools that a role can grant have the first ids of the registry.
	 * With --upstream-tools, askai can offer such a tool to the upstream if
	 * the tool's kind has the flag TOOL_LENT. */
	bool lends = (cfg->running ? cfg->running : cfg)->flags & CFG_UPSTREAM_TOOLS;
	char const *tools[TOOL_MAX];
	tool_mask upstream = 0;
	for (uint32_t id = 0; id < reg->n_granted; ++id) {
		tools[id] = reg->tools[id].name;
		if (lends && reg->tools[id].kind->flags & TOOL_LENT)
			upstream |= (tool_mask)1 << id;
	}
	char why[256];
	int e;
	cfg->policy = policy_create(cfg->file, tools, reg->n_granted, upstream, why,
	                            sizeof why, &e);
	if (e == ENOMEM)
		return config_oom(cfg);
	if (e)
		return config_complain(cfg, cfg->path, "%s", why);
	cfg->http.policy = cfg->policy;
	config_note_token(cfg);
	return 0;
}

/* Check the options together, and work out what they leave to askai. */
static int
config_finish (struct config *cfg)
{
	int status = config_check(cfg);
	if (status)
		return status;
	status = config_check_gits(cfg);
	if (status)
		return status;
	status = config_api(cfg);
	if (status)
		return status;
	status = config_check_upstream(cfg);
	if (status)
		return status;
	status = config_check_notes(cfg);
	if (status)
		return status;
	status = config_upstream(cfg);
	if (status)
		return status;
	status = config_listens(cfg);
	if (status)
		return status;
	status = config_registry(cfg);
	if (status)
		return status;
	status = config_policy(cfg);
	if (status)
		return status;
	/* One CPU fewer than askai may use is left to home and the threads that
	 * mostly sleep (design section 1). */
	size_t loops = cfg->loops;
	if (!loops) {
		size_t cpus = brainstem_cpus() - 1;
		loops = cpus < 1 ? 1 : cpus < 255 ? cpus : 255;
	}
	cfg->brainstem.loops = loops;
	return 0;
}

/* Show the usage on stderr, but on a reload; returns 2. */
static int
config_misuse (struct config const *cfg)
{
	if (!cfg->logged)
		config_usage(cfg, stderr);
	return 2;
}

/* Apply the command line's options, but --config, over the file's. */
static int
config_argv (struct config *cfg)
{
	opterr = !cfg->logged;
	optind = 0;
	for (int opt; (opt = getopt_long(cfg->argc, cfg->argv, shorts, options,
	                                 nullptr)) >= 0; ) {
		if (opt == '?')
			return config_misuse(cfg);
		if (opt == 'c')
			continue;
		int status = config_apply(cfg, opt, optarg, nullptr);
		if (status)
			return status;
	}
	return optind == cfg->argc ? 0 : config_misuse(cfg);
}

/* The defaults, for the command line of @p argc arguments at @p argv. */
static struct config
config_defaults (int   argc,
                 char *argv[])
{
	return (struct config){
		.ep = {
			.timeout_ms = 600'000,
			.chunk_ms = 60'000,
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
		.git_timeout = 30'000'000'000
	};
}

/* Read the command line and the file it names into @p cfg, as config()
 * says. */
static int
config_parse (struct config *cfg)
{
	/* The configuration file goes first, so that the command line overrides
	 * it; help goes before either.  Each pass begins getopt afresh. */
	opterr = 0;
	optind = 0;
	for (int opt; (opt = getopt_long(cfg->argc, cfg->argv, shorts, options,
	                                 nullptr)) >= 0; ) {
		if (opt == 'h' && !cfg->logged) {
			config_usage(cfg, stdout);
			return 0;
		}
		if (opt == 'c')
			cfg->path = optarg;
	}
	int status = cfg->path ? config_file(cfg) : config_lists(cfg);
	if (status)
		return status;
	status = config_argv(cfg);
	if (status)
		return status;
	status = config_finish(cfg);
	return status ? status : -1;
}

struct config
config (int   argc,
        char *argv[],
        int  *status)
{
	struct config cfg = config_defaults(argc, argv);
	*status = config_parse(&cfg);
	return cfg;
}

int
config_reload (struct config       *cfg,
               struct config const *from)
{
	*cfg = config_defaults(from->argc, from->argv);
	cfg->logged = true;
	cfg->running = from;
	int status = config_parse(cfg);
	cfg->restart = status < 0 && !config_same_options(cfg, from);
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
	registry_fini(&cfg->registry);
	free(cfg->url);
	free(cfg->auth);
	free(cfg->description);
	free(cfg->headers);
	*cfg = (struct config){};
}
