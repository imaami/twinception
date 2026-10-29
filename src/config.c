/** @file askai's configuration (config.h): the command line, and the
 *  configuration file, whose objects' members are applied as the long
 *  options of their names are, and the members of the objects of the
 *  built-in tools in its member tools as their options, before the command
 *  line's. */
#define _GNU_SOURCE /* vasprintf() */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <json-c/json.h>
#include <json-c/json_visit.h>
#include <libgen.h>
#include <limits.h>
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
	[AI_CHAT]         = "chat",
	[AI_RESPONSES]    = "responses",
	[AI_MESSAGES]     = "messages",
	[AI_GENERATE]     = "generate",
	[AI_INTERACTIONS] = "interactions",
};

/* Each API's header line prefix for the API key. */
static char const *const auths[] = {
	[AI_CHAT]         = "Authorization: Bearer ",
	[AI_RESPONSES]    = "Authorization: Bearer ",
	[AI_MESSAGES]     = "x-api-key: ",
	[AI_GENERATE]     = "x-goog-api-key: ",
	[AI_INTERACTIONS] = "x-goog-api-key: ",
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
		}, { "OPENAI_API_KEY" }, AI_RESPONSES
	},
	[AI_ANTHROPIC] = {
		{
			[AI_CHAT]     = "https://api.anthropic.com/v1/chat/completions",
			[AI_MESSAGES] = "https://api.anthropic.com/v1/messages",
		}, { "ANTHROPIC_API_KEY" }, AI_MESSAGES
	},
	[AI_GOOGLE] = {
		{
			[AI_CHAT]         = "https://generativelanguage.googleapis.com/v1beta/"
			                    "openai/chat/completions",
			[AI_GENERATE]     = "https://generativelanguage.googleapis.com/v1beta/"
			                    "models/",
			[AI_INTERACTIONS] = "https://generativelanguage.googleapis.com/v1beta/"
			                    "interactions",
		}, { "GEMINI_API_KEY", "GOOGLE_API_KEY" }, AI_INTERACTIONS
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
	        "  -c, --config FILE         read options from FILE before the command\n"
	        "                            line: a JSON object without comments.  Its\n"
	        "                            objects upstream, service, testing and\n"
	        "                            debugging contain the options of the\n"
	        "                            sections below, named as the long options.\n"
	        "                            Its member tools can contain the objects of\n"
	        "                            the built-in tools, whose members the\n"
	        "                            section Built-in tools names.  true sets an\n"
	        "                            option without argument, and an array lists\n"
	        "                            the values of a repeatable option.  The\n"
	        "                            command line overrides the file, but adds\n"
	        "                            to its header, allow-origin and git lists.\n"
	        "                            Only the last --config is read.  See\n"
	        "                            doc/configuration.md and\n"
	        "                            doc/askai.example.json.\n"
	        "  -h, --help                show this help\n"
	        "\n"
	        "Session (top-level configuration members):\n"
	        "      --participant NAME    stable writer identity, 1 to 64 characters\n"
	        "                            from [A-Za-z0-9_-]; default: tool-name.  Use\n"
	        "                            distinct names for collaborating instances\n"
	        "      --transcripts PATH    separate git repository for reasoning text,\n"
	        "                            with private job recovery state; default:\n"
	        "                            <notes>.transcripts.git with --notes, else\n"
	        "                            $XDG_STATE_HOME/askai/NAME/transcripts.git\n"
	        "                            with --background, using $HOME/.local/state\n"
	        "                            if XDG_STATE_HOME is unset\n"
	        "\n"
	        "Upstream AI (\"upstream\"):\n"
	        "  -p, --provider NAME       llama (llama-server at 127.0.0.1:9931),\n"
	        "                            openai, anthropic, google, deepinfra or\n"
	        "                            openrouter; the provider sets the defaults\n"
	        "                            of --api, --url and --key-env\n"
	        "      --api NAME            chat (chat completions), responses,\n"
	        "                            messages (Anthropic), generate (Gemini) or\n"
	        "                            interactions (Gemini Interactions)\n"
	        "                            (default: the API of the provider, else\n"
	        "                            chat)\n"
	        "  -u, --url URL             endpoint URL, http:// or https:// (default:\n"
	        "                            the URL of the provider; required without\n"
	        "                            --provider)\n"
	        "  -m, --model NAME          model to ask (required at the URL of a\n"
	        "                            provider other than llama)\n"
	        "  -k, --key-env VAR         environment variable that contains the API\n"
	        "                            key (default: at the URL of a provider, the\n"
	        "                            usual variable of the provider, which llama\n"
	        "                            may leave unset; else ASKAI_API_KEY, if set)\n"
	        "  -H, --header LINE         extra request header, Name: value\n"
	        "                            (repeatable)\n"
	        "  -s, --system TEXT         default system prompt\n"
	        "  -t, --temperature N       sampling temperature, a number from 0\n"
	        "                            (default: none sent)\n"
	        "  -n, --max-tokens N        output token limit (default: none sent, but\n"
	        "                            4096 with the messages API)\n", argv0);
	fprintf(out,
	        "  -x, --extra JSON          JSON object that askai merges into each\n"
	        "                            request body, recursively; a null member\n"
	        "                            removes that key\n"
	        "      --background          return ask calls immediately as jobs, and\n"
	        "                            keep their upstream streams alive after\n"
	        "                            clients disconnect; request native background\n"
	        "                            execution with OpenAI Responses and Gemini\n"
	        "                            Interactions (default off)\n"
	        "  -T, --timeout SECONDS     time limit on each upstream call: first on\n"
	        "                            the wait for an upstream slot, then again on\n"
	        "                            the call itself (default 600, 0 = none)\n"
	        "      --chunk-timeout SECONDS\n"
	        "                            longest pause in an answer once it has\n"
	        "                            begun (default 60, 0 = none)\n"
	        "      --slots N             upstream calls at once (default: total_slots\n"
	        "                            from the /props of llama-server, else 4)\n"
	        "      --max-calls N         upstream calls queued or running, at most\n"
	        "                            (default 256), and at most 8 queued for one\n"
	        "                            identity, one Unix user, or else one session\n"
	        "                            or client address; an ask call past those\n"
	        "                            fails at once with an error to try again\n"
	        "      --upstream-tools      offer tools to the upstream in ask calls:\n"
	        "                            man, git and the plugin tools that allow it,\n"
	        "                            as the role of the client allows, and notes\n"
	        "                            with --notes; with any of those also\n"
	        "                            compact, wait and timer, and beside a tool\n"
	        "                            that can run in the background also\n"
	        "                            background and jobs; an ask call runs the\n"
	        "                            tool calls of each turn and sends the next\n"
	        "                            upstream request until an answer ends the\n"
	        "                            call\n"
	        "      --max-turns N         upstream calls that an ask call makes at\n"
	        "                            most, with --upstream-tools (default 32)\n"
	        "      --context TOKENS      context size of the upstream, used with\n"
	        "                            --upstream-tools: a tool result in a turn is\n"
	        "                            at most an eighth of it; at a quarter and at\n"
	        "                            half, the upstream is reminded to write\n"
	        "                            notes, with --notes; from two thirds on, it\n"
	        "                            is asked to call compact; and when it is\n"
	        "                            full, askai compacts the conversation\n"
	        "                            (default: n_ctx from the /props of\n"
	        "                            llama-server, else unknown; 0 = the default)\n");
	fprintf(out,
	        "      --tool-name NAME      name of the ask tool, 1 to 128 characters\n"
	        "                            from A-Z, a-z, 0-9, _, . and - (default\n"
	        "                            ask_ai)\n"
	        "      --tool-description TEXT\n"
	        "                            description of the ask tool (default: a text\n"
	        "                            built from --model, --upstream-tools and\n"
	        "                            --notes)\n"
	        "\n"
	        "MCP server (\"service\"):\n"
	        "  -l, --listen ADDR         [host:]port, where host is 127.0.0.1 if it\n"
	        "                            is left out; unix:PATH for a Unix socket; or\n"
	        "                            tls:[host:]port for TLS (repeatable;\n"
	        "                            default 127.0.0.1:8090)\n"
	        "      --tls-cert FILE       certificate chain for tls: listeners, in\n"
	        "                            PEM: the certificate of the server first,\n"
	        "                            then the intermediate CA certificates\n"
	        "      --tls-key FILE        private key of that certificate, in PEM,\n"
	        "                            unencrypted: ECDSA on P-256, P-384 or P-521,\n"
	        "                            or Ed25519; others must not be able to read\n"
	        "                            FILE\n"
	        "      --tls-allow-rsa       allow an RSA key too\n"
	        "  -o, --allow-origin URL    allow browser requests whose Origin header\n"
	        "                            is exactly URL, such as https://example.com\n"
	        "                            (repeatable; '*' allows any; loopback\n"
	        "                            Origins are always allowed)\n"
	        "      --loops N             event loops that serve clients (default 0:\n"
	        "                            one fewer than the CPUs askai may use, at\n"
	        "                            least 1, at most 255)\n"
	        "      --grace SECONDS       time to finish the requests under way after\n"
	        "                            SIGINT, SIGTERM or SIGHUP (default 5)\n"
	        "      --memory-high BYTES   resident memory from which memory pressure\n"
	        "                            is high (default: a sixteenth of RAM, or of\n"
	        "                            the lowest cgroup limit, at least 256M)\n"
	        "\n"
	        "Limits on clients (\"service\"; BYTES and RATE may end in K, M or G for\n"
	        "KiB, MiB or GiB; 0 = none where noted):\n"
	        "      --max-head BYTES      request line and headers (default 16K,\n"
	        "                            at most 65535)\n"
	        "      --max-fields N        header fields (default 64)\n"
	        "      --head-timeout SECONDS\n"
	        "                            time limit for a TLS handshake, and for a\n"
	        "                            request line and headers, each from its\n"
	        "                            first byte (default 10, 0 = none)\n"
	        "      --max-body BYTES      request body, at most --body-budget\n"
	        "                            (default 8M)\n"
	        "      --min-body-rate RATE  bytes per second that a request body must\n"
	        "                            average from the end of its headers, once\n"
	        "                            --body-grace has passed (default 4K,\n"
	        "                            0 = none)\n"
	        "      --body-grace SECONDS  time after the headers before\n"
	        "                            --min-body-rate applies (default 5)\n"
	        "      --body-budget BYTES   request bodies in memory for all clients\n"
	        "                            together (default 256M)\n"
	        "      --client-budget BYTES\n"
	        "                            the part of --body-budget for one client\n"
	        "                            address (default: a quarter of it, at least\n"
	        "                            --max-body)\n"
	        "      --max-depth N         JSON nesting in a request (default 32)\n"
	        "      --max-values N        JSON values in a request, keys included\n"
	        "                            (default 1024; json-c uses up to 0.7 KiB for\n"
	        "                            each value while it parses)\n"
	        "      --max-members N       members of one JSON object in a request\n"
	        "                            (default 1024)\n"
	        "      --sse-keepalive SECONDS\n"
	        "                            time between the comment lines that askai\n"
	        "                            writes on an idle event stream (default 15,\n"
	        "                            0 = none)\n");
	fprintf(out,
	        "      --max-client-conns N  connections per client address: an IPv4\n"
	        "                            address, an IPv6 /64, or the user of a Unix\n"
	        "                            socket client (default 256, 0 = none)\n"
	        "      --idle-timeout SECONDS\n"
	        "                            time a connection may stay open with no\n"
	        "                            request under way, before its first request\n"
	        "                            and between requests (default 30, 0 = none)\n"
	        "      --output-pause BYTES  pending output at which no more input is\n"
	        "                            read from a connection, and a response is\n"
	        "                            paused until at most half of this is pending\n"
	        "                            (default 256K, at most --max-output)\n"
	        "      --max-output BYTES    close a connection whose pending output a\n"
	        "                            response would take past this, unless it\n"
	        "                            is all that is pending (default 8M)\n"
	        "      --send-timeout SECONDS\n"
	        "                            longest stall of pending output\n"
	        "                            (default 30, 0 = none)\n"
	        "\n");
	fprintf(out,
	        "Built-in tools (\"tools\", each in the member in parentheses):\n"
	        "      --man                 also offer the tool man, which returns a\n"
	        "                            page of the manual as man(1) renders it, in\n"
	        "                            plain UTF-8 text; askai renders each page\n"
	        "                            once and stores it (tools.man.enabled)\n"
	        "      --git PATH            also offer the tool git: read-only git\n"
	        "                            commands in the repository at PATH, the top\n"
	        "                            of a work tree or a bare repository, named\n"
	        "                            by the last component of PATH less .git\n"
	        "                            (repeatable; tools.git.repositories)\n"
	        "      --git-timeout SECONDS\n"
	        "                            deadline of a git call, at which git gets\n"
	        "                            SIGTERM, and SIGKILL one second later\n"
	        "                            (default 30; tools.git.timeout)\n"
	        "      --notes PATH          shared session's git repository, created\n"
	        "                            bare if absent.  Each participant\n"
	        "                            writes its own branches and reads all notes;\n"
	        "                            offer notes to clients as their role\n"
	        "                            allows, and to the upstream with\n"
	        "                            --upstream-tools (tools.notes.directory)\n"
	        "      --client-jobs         also offer clients the tools background,\n"
	        "                            jobs, wait and timer, as their role allows:\n"
	        "                            background runs a call of another tool as a\n"
	        "                            job, whose result askai stores for an hour;\n"
	        "                            timer sets a timer; wait waits for jobs and\n"
	        "                            timers (tools.jobs.clients)\n"
	        "\n"
	        "Test aids (\"testing\"):\n"
	        "      --test-man-recheck SECONDS\n"
	        "                            once the lookup of the file of a page (man\n"
	        "                            -w) is older than this, the next man call\n"
	        "                            for the page repeats the lookup in the\n"
	        "                            background (default 600, 0 = on each call)\n"
	        "      --test-stall MS       SIGUSR2 stops each event loop in one turn\n"
	        "                            for this long\n"
	        "      --test-pressure FILE  memory pressure is at least the level in\n"
	        "                            FILE, 0 (none), 1 (high) or 2 (critical),\n"
	        "                            read each second\n"
	        "      --test-fifo           upstream calls wait in one queue for each\n"
	        "                            class, first in first out, and without the\n"
	        "                            limit of 8 queued calls\n"
	        "      --test-task           also offer the tool test_task: CPU work in\n"
	        "                            250 us slices, run as a task on the client\n"
	        "                            loops; or, given a prompt, an upstream call\n"
	        "                            run as background work\n"
	        "\n"
	        "Debugging (\"debugging\"):\n"
	        "  -v, --verbose             log each tool call of a client as it ends,\n"
	        "                            with its arguments, and each ask call as\n"
	        "                            it starts; each upstream turn, each tool\n"
	        "                            call of a turn or of a background job,\n"
	        "                            each compaction and each notice about the\n"
	        "                            context; each web request of a plugin; and\n"
	        "                            each man page that askai renders\n");
	fprintf(out,
	        "      --watchdog MS         log a turn of an event loop longer than\n"
	        "                            this (default 20, 0 = none)\n"
	        "\n"
	        "The members roles, identities, unix-groups and anonymous of the\n"
	        "configuration file define who may connect and what each client may do\n"
	        "(see README.md).  With any of them, askai ignores ASKAI_TOKEN.  Without\n"
	        "them, every client that askai accepts may call every tool.  If\n"
	        "ASKAI_TOKEN is set and not empty, askai accepts only clients that send\n"
	        "'Authorization: Bearer TOKEN' with that value as TOKEN, and these may\n"
	        "request GET /metrics.  Otherwise askai accepts every client, and only\n"
	        "clients on a Unix socket, or on loopback with no Host header or the Host\n"
	        "localhost, 127.0.0.1 or [::1], may request GET /metrics, which returns\n"
	        "metrics in the Prometheus text format.\n"
	        "The member plugins of the configuration file is an array of the\n"
	        "plugins that askai loads as it starts.  An element is the absolute\n"
	        "path of a plugin, or an object whose member path is that path and\n"
	        "whose member options is an object of the plugin's options (see\n"
	        "README.md).\n"
	        "The member tools of the configuration file allows or denies the tools\n"
	        "other than the ask tool, to clients and to the upstream: true or false\n"
	        "for all, a list of the tools it allows, an object with a list allow or a\n"
	        "list deny, or an object whose members client and upstream each have one\n"
	        "of those forms (see doc/configuration.md).  As an object, it also has\n"
	        "the objects man, git, notes and jobs of the built-in tools.\n"
	        "SIGUSR1 prints to stderr, for each event loop, the times of its turns and\n"
	        "steps, the lateness of its timers and the most work that one step did\n"
	        "since the previous SIGUSR1.  The lines name the loops: loop1 to loopN\n"
	        "serve clients, and home accepts them and runs the upstream calls.\n"
	        "With --config or a tls: listener, SIGHUP reloads the configuration file\n"
	        "and the TLS certificate and key.  A reload applies only the members\n"
	        "roles, identities, unix-groups and anonymous and the TLS files.  Other\n"
	        "options need a restart.  A failed reload changes nothing.\n"
	        "Without --config or a tls: listener, SIGHUP stops askai as SIGTERM does.\n"
	        "The first SIGINT or SIGTERM ends the tool calls under way with an error,\n"
	        "finishes the other requests and exits 0.  During that shutdown, a second\n"
	        "such signal, the end of the grace period (--grace) or an event loop stuck\n"
	        "in one turn for 1 s ends askai at once with exit status %d.  SIGINT,\n"
	        "SIGTERM and SIGHUP stay ignored if askai starts with them ignored, except\n"
	        "SIGHUP for a reload.\n"
	        "Invalid options or environment exit 2.  A failure to start or to serve\n"
	        "exits 1, also a --listen host name that cannot be resolved.\n"
	        "\n",
	        BRAINSTEM_FORCED);
}

/* @p str, a count from @p min to @p max in decimal digits, in @p *value; for
 * a count of @p bytes, the digits may end in K, M or G for KiB, MiB or GiB.
 * EINVAL if it is not one. */
static int
config_parse_count (char const *str,
                    uint64_t    min,
                    uint64_t    max,
                    bool        bytes,
                    uint64_t   *value)
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
		char const *unit = bytes ? strchr(units, *end) : nullptr;
		if (!unit || end[1])
			return EINVAL;
		shift = 10 * (unsigned)(unit - units + 1);
	}
	if (n > max >> shift || n << shift < min)
		return EINVAL;
	*value = (uint64_t)n << shift;
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

static bool
config_participant_valid (char const *name)
{
	return util_word(name, strnlen(name, UTIL_WORD_MAX + 1), UTIL_WORD_MAX);
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
	OPT_MAX_TURNS, OPT_CONTEXT, OPT_API, OPT_CLIENT_JOBS, OPT_BACKGROUND,
	OPT_PARTICIPANT, OPT_TRANSCRIPTS
};

static char const shorts[] = "c:p:u:m:k:H:s:t:n:x:T:l:o:vh";

/* The objects of the configuration file that contain the options, and
 * TOOLS for the options that an object of a built-in tool in the member
 * tools contains (tool_members[]). */
enum config_group : uint8_t {
	GROUP_NONE, GROUP_UPSTREAM, GROUP_SERVICE, GROUP_TESTING, GROUP_DEBUGGING,
	GROUP_TOOLS, GROUP_ROOT
};

static char const *const group_names[] = {
	[GROUP_UPSTREAM]  = "upstream",
	[GROUP_SERVICE]   = "service",
	[GROUP_TESTING]   = "testing",
	[GROUP_DEBUGGING] = "debugging",
};

/* Each option: the object of the configuration file that contains it, NONE
 * for the two that the file cannot set; its long name; whether it takes an
 * argument; and its short form or OPT_ value. */
#define CONFIG_OPTIONS(X)                                                         \
        X(ROOT,      "participant",      required_argument, OPT_PARTICIPANT)      \
        X(ROOT,      "transcripts",      required_argument, OPT_TRANSCRIPTS)      \
        X(UPSTREAM,  "provider",         required_argument, 'p')                  \
        X(UPSTREAM,  "api",              required_argument, OPT_API)              \
        X(UPSTREAM,  "url",              required_argument, 'u')                  \
        X(UPSTREAM,  "model",            required_argument, 'm')                  \
        X(UPSTREAM,  "key-env",          required_argument, 'k')                  \
        X(UPSTREAM,  "header",           required_argument, 'H')                  \
        X(UPSTREAM,  "system",           required_argument, 's')                  \
        X(UPSTREAM,  "temperature",      required_argument, 't')                  \
        X(UPSTREAM,  "max-tokens",       required_argument, 'n')                  \
        X(UPSTREAM,  "extra",            required_argument, 'x')                  \
        X(UPSTREAM,  "background",       no_argument,       OPT_BACKGROUND)       \
        X(UPSTREAM,  "timeout",          required_argument, 'T')                  \
        X(UPSTREAM,  "chunk-timeout",    required_argument, OPT_CHUNK_TIMEOUT)    \
        X(UPSTREAM,  "slots",            required_argument, OPT_SLOTS)            \
        X(UPSTREAM,  "max-calls",        required_argument, OPT_MAX_CALLS)        \
        X(UPSTREAM,  "upstream-tools",   no_argument,       OPT_UPSTREAM_TOOLS)   \
        X(TOOLS,     "notes",            required_argument, OPT_NOTES)            \
        X(UPSTREAM,  "max-turns",        required_argument, OPT_MAX_TURNS)        \
        X(UPSTREAM,  "context",          required_argument, OPT_CONTEXT)          \
        X(UPSTREAM,  "tool-name",        required_argument, OPT_TOOL_NAME)        \
        X(UPSTREAM,  "tool-description", required_argument, OPT_TOOL_DESCRIPTION) \
        X(SERVICE,   "listen",           required_argument, 'l')                  \
        X(SERVICE,   "tls-cert",         required_argument, OPT_TLS_CERT)         \
        X(SERVICE,   "tls-key",          required_argument, OPT_TLS_KEY)          \
        X(SERVICE,   "tls-allow-rsa",    no_argument,       OPT_TLS_ALLOW_RSA)    \
        X(SERVICE,   "allow-origin",     required_argument, 'o')                  \
        X(TOOLS,     "man",              no_argument,       OPT_MAN)              \
        X(TOOLS,     "git",              required_argument, OPT_GIT)              \
        X(TOOLS,     "git-timeout",      required_argument, OPT_GIT_TIMEOUT)      \
        X(TOOLS,     "client-jobs",      no_argument,       OPT_CLIENT_JOBS)      \
        X(SERVICE,   "loops",            required_argument, OPT_LOOPS)            \
        X(SERVICE,   "grace",            required_argument, OPT_GRACE)            \
        X(SERVICE,   "memory-high",      required_argument, OPT_MEMORY_HIGH)      \
        X(SERVICE,   "max-head",         required_argument, OPT_MAX_HEAD)         \
        X(SERVICE,   "max-fields",       required_argument, OPT_MAX_FIELDS)       \
        X(SERVICE,   "head-timeout",     required_argument, OPT_HEAD_TIMEOUT)     \
        X(SERVICE,   "max-body",         required_argument, OPT_MAX_BODY)         \
        X(SERVICE,   "min-body-rate",    required_argument, OPT_MIN_BODY_RATE)    \
        X(SERVICE,   "body-grace",       required_argument, OPT_BODY_GRACE)       \
        X(SERVICE,   "body-budget",      required_argument, OPT_BODY_BUDGET)      \
        X(SERVICE,   "client-budget",    required_argument, OPT_CLIENT_BUDGET)    \
        X(SERVICE,   "max-depth",        required_argument, OPT_MAX_DEPTH)        \
        X(SERVICE,   "max-values",       required_argument, OPT_MAX_VALUES)       \
        X(SERVICE,   "max-members",      required_argument, OPT_MAX_MEMBERS)      \
        X(SERVICE,   "sse-keepalive",    required_argument, OPT_SSE_KEEPALIVE)    \
        X(SERVICE,   "max-client-conns", required_argument, OPT_MAX_CLIENT_CONNS) \
        X(SERVICE,   "idle-timeout",     required_argument, OPT_IDLE_TIMEOUT)     \
        X(SERVICE,   "output-pause",     required_argument, OPT_OUTPUT_PAUSE)     \
        X(SERVICE,   "max-output",       required_argument, OPT_MAX_OUTPUT)       \
        X(SERVICE,   "send-timeout",     required_argument, OPT_SEND_TIMEOUT)     \
        X(TESTING,   "test-man-recheck", required_argument, OPT_TEST_MAN_RECHECK) \
        X(TESTING,   "test-stall",       required_argument, OPT_TEST_STALL)       \
        X(TESTING,   "test-pressure",    required_argument, OPT_TEST_PRESSURE)    \
        X(TESTING,   "test-fifo",        no_argument,       OPT_TEST_FIFO)        \
        X(TESTING,   "test-task",        no_argument,       OPT_TEST_TASK)        \
        X(DEBUGGING, "verbose",          no_argument,       'v')                  \
        X(DEBUGGING, "watchdog",         required_argument, OPT_WATCHDOG)         \
        X(NONE,      "config",           required_argument, 'c')                  \
        X(NONE,      "help",             no_argument,       'h')

#define CONFIG_OPTION(group, name, arg, val) { name, arg, nullptr, val },
static struct option const options[] = {
	CONFIG_OPTIONS(CONFIG_OPTION)
	{ nullptr, 0, nullptr, 0 }
};
#undef CONFIG_OPTION

/* The object that contains each option of options[]. */
#define CONFIG_GROUP(group, name, arg, val) GROUP_##group,
static enum config_group const groups[] = { CONFIG_OPTIONS(CONFIG_GROUP) };
#undef CONFIG_GROUP
#undef CONFIG_OPTIONS

/* The member of the configuration file that sets each option of the group
 * TOOLS: the object of its built-in tool in the member tools, and its name
 * in that object. */
static struct {
	char const *tool;
	char const *name;
	int         val;
} const tool_members[] = {
	{ "man",   "enabled",      OPT_MAN },
	{ "git",   "repositories", OPT_GIT },
	{ "git",   "timeout",      OPT_GIT_TIMEOUT },
	{ "notes", "directory",    OPT_NOTES },
	{ "jobs",  "clients",      OPT_CLIENT_JOBS },
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
	char what[1024];
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
		fprintf(stderr, "%s%s%s\n", where, colon, what);
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

/* How each numeric option is taken: the member of struct config at @p at,
 * of @p width bytes, takes a count from @p min to @p max, in bytes if
 * @p bytes, times @p scale; the option also sets the CFG_ flags @p flag. */
static struct config_number {
	uint64_t min;
	uint64_t max;
	uint64_t scale;
	uint32_t flag;
	int      opt;
	uint16_t at;
	uint8_t  width;
	bool     bytes;
} const numbers[] = {
#define TAKE(opt, member, min, max, scale, bytes, flag)                     \
        { min, max, scale, flag, opt, offsetof(struct config, member),          \
          sizeof ((struct config *)nullptr)->member, bytes }
#define N(opt, member, min, max) TAKE(opt, member, min, max, 1, false, 0)
#define B(opt, member, min, max) TAKE(opt, member, min, max, 1, true, 0)
	B(OPT_MAX_HEAD,          http.limits.head,          1, UINT16_MAX),
	N(OPT_MAX_FIELDS,        http.limits.fields,        0, UINT32_MAX),
	N(OPT_HEAD_TIMEOUT,      http.limits.head_time,     0, UINT32_MAX),
	TAKE(OPT_MAX_BODY,       http.limits.body,          0, SIZE_MAX / 2, 1, true,
	     CFG_BODY_GIVEN),
	B(OPT_MIN_BODY_RATE,     http.limits.body_rate,     0, UINT32_MAX),
	N(OPT_BODY_GRACE,        http.limits.body_grace,    0, UINT32_MAX),
	B(OPT_BODY_BUDGET,       http.limits.bodies,        0, SIZE_MAX / 2),
	B(OPT_CLIENT_BUDGET,     http.limits.client_bodies, 1, SIZE_MAX / 2),
	N(OPT_MAX_CLIENT_CONNS,  http.limits.clients,       0, UINT32_MAX),
	N(OPT_IDLE_TIMEOUT,      http.limits.idle_time,     0, UINT32_MAX),
	B(OPT_OUTPUT_PAUSE,      http.limits.out_pause,     1, SIZE_MAX / 2),
	B(OPT_MAX_OUTPUT,        http.limits.out_max,       1, SIZE_MAX / 2),
	N(OPT_SEND_TIMEOUT,      http.limits.write_time,    0, UINT32_MAX),
	N(OPT_MAX_DEPTH,         mcp.depth,                 1, INT32_MAX - 1),
	N(OPT_MAX_VALUES,        mcp.values,                1, UINT32_MAX),
	N(OPT_MAX_MEMBERS,       mcp.members,               1, UINT32_MAX - 1),
	N(OPT_SSE_KEEPALIVE,     mcp.keepalive,             0, UINT32_MAX / 2),
	TAKE(OPT_GRACE,          brainstem.grace_ms,        1, 3600, 1000, false, 0),
	N(OPT_WATCHDOG,          brainstem.watchdog_ms,     0, 60'000),
	N(OPT_TEST_STALL,        brainstem.stall_ms,        1, 3'600'000),
	B(OPT_MEMORY_HIGH,       brainstem.memory_high,     1, SIZE_MAX / 2),
	N(OPT_MAX_CALLS,         calls,                     1, 1'000'000),
	N(OPT_TEST_MAN_RECHECK,  man_recheck,               0, 86'400),
	TAKE(OPT_GIT_TIMEOUT,    git_timeout,               1, 3600, NS, false, 0),
	N(OPT_MAX_TURNS,         turns,                     1, 1000),
	/* As many as a handle's loop field names, home being 0. */
	N(OPT_LOOPS,             loops,                     0, 255),
	N(OPT_SLOTS,             ep.slots,                  1, UINT16_MAX),
	N('n',                   ep.max_tokens,             1, INT32_MAX),
	TAKE('T',                ep.timeout_ms,             0, INT32_MAX / 1000, 1000, false, 0),
	TAKE(OPT_CHUNK_TIMEOUT,  ep.chunk_ms,               0, INT32_MAX / 1000, 1000, false, 0),
	N(OPT_CONTEXT,           ep.context,                0, INT32_MAX),
#undef B
#undef N
#undef TAKE
};

/* Apply the numeric option @p opt with @p arg, given in the configuration
 * file @p where, or on the command line if that is nullptr; returns 0, or 2,
 * having said why if @p opt is one. */
static int
config_apply_number (struct config *cfg,
                     int            opt,
                     char const    *arg,
                     char const    *where)
{
	struct config_number const *n = numbers;
	while (n < numbers + ARRAY_SIZE(numbers) && n->opt != opt)
		++n;
	if (n == numbers + ARRAY_SIZE(numbers))
		return 2;
	uint64_t value;
	if (config_parse_count(arg, n->min, n->max, n->bytes, &value))
		return config_complain(cfg, where, "invalid --%s: %s",
		                       config_option(opt)->name, arg);
	/* Every member is an unsigned or a nonnegative count of 32 or 64 bits. */
	value *= n->scale;
	unsigned char *member = (unsigned char *)cfg + n->at;
	switch (n->width) {
	case sizeof (uint32_t): {
		uint32_t narrow = (uint32_t)value;
		memcpy(member, &narrow, sizeof narrow);
		break;
	}
	case sizeof (uint64_t):
		memcpy(member, &value, sizeof value);
		break;
	}
	cfg->flags |= n->flag;
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
	case OPT_CLIENT_JOBS: cfg->flags |= CFG_CLIENT_JOBS; break;
	case OPT_BACKGROUND: ep->background = 1; break;
	case OPT_NOTES: cfg->notes = arg; break;
	case OPT_PARTICIPANT:
		if (!config_participant_valid(arg))
			return config_complain(cfg, where, "--participant: use 1-64 of "
			                       "[A-Za-z0-9_-]: %s", arg);
		cfg->participant = arg;
		break;
	case OPT_TRANSCRIPTS: cfg->transcripts = arg; break;
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
	case 'x':
		return config_apply_extra(cfg, arg, where);
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
	int e = util_read_all(fd, text, (size_t)st.st_size, -1, &len);
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

/* Parses the @p len bytes at @p text, the configuration file, into
 * cfg->file.  The file must be one JSON object in strict JSON, so that tools
 * such as jq can read it.  With JSON_TOKENER_STRICT, json-c reports comments
 * and trailing commas as errors.  Only blanks may follow the object.  Returns
 * 0, or the exit status after a message about the error. */
static int
config_json (struct config *cfg,
             char const    *text,
             size_t         len)
{
	struct json_tokener *tok = json_tokener_new();
	if (!tok)
		return config_oom(cfg);
	json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
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

/* Whether option @p name takes effect on a reload: TLS's certificate, key
 * and key types. */
static bool
config_reloadable (char const *name)
{
	return !strncmp(name, "tls-", sizeof "tls-" - 1);
}

/* The value of option @p o, which the file can set, in @p cfg's
 * configuration file, if it has one; nullptr if none. */
static struct json_object *
config_file_value (struct config const *cfg,
                   struct option const *o)
{
	enum config_group g = groups[o - options];
	struct json_object *group = g == GROUP_ROOT ? cfg->file
	                           : json_object_object_get(cfg->file, group_names[g]);
	return json_object_object_get(group, o->name);
}

/* Whether @p cfg's configuration file and @p from's, if either has one, set
 * the same plugins, tools and options, but the options that take effect on
 * a reload. */
static bool
config_same_options (struct config const *cfg,
                     struct config const *from)
{
	/* The options of the group TOOLS are in the member tools, which is
	 * compared whole. */
	for (struct option const *o = options; o->name; ++o)
		if (groups[o - options] && groups[o - options] != GROUP_TOOLS &&
		    !config_reloadable(o->name) &&
		    !json_object_equal(config_file_value(cfg, o), config_file_value(from, o)))
			return false;
	return json_object_equal(json_object_object_get(cfg->file, "plugins"),
	                         json_object_object_get(from->file, "plugins")) &&
	       json_object_equal(json_object_object_get(cfg->file, "tools"),
	                         json_object_object_get(from->file, "tools"));
}

/* The values of the members of @p obj, if it is an object: the elements of
 * an array, else 1 for each member. */
static size_t
config_values (struct json_object *obj)
{
	size_t n = 0;
	struct json_object_iter it;
	if (json_object_is_type(obj, json_type_object))
		json_object_object_foreachC(obj, it)
			n += json_object_is_type(it.val, json_type_array)
			   ? json_object_array_length(it.val) : 1;
	return n;
}

/* Make room for the lists of headers, origins, listeners and repositories,
 * in one block, and point ep.headers, http.origins and http.listen at their
 * lists: each has room for every argument, every value of an option in
 * the configuration file, in its four objects of options and in the
 * objects of the tools in its member tools, and the three headers askai
 * adds, and ends in nullptr.  Returns 0, or 1 if out of memory, having said
 * so. */
static int
config_lists (struct config *cfg)
{
	size_t room = (size_t)cfg->argc + 4;
	for (size_t g = 1; g < ARRAY_SIZE(group_names); ++g)
		room += config_values(json_object_object_get(cfg->file, group_names[g]));
	struct json_object *tools = json_object_object_get(cfg->file, "tools");
	struct json_object_iter it;
	if (json_object_is_type(tools, json_type_object))
		json_object_object_foreachC(tools, it)
			room += config_values(it.val);
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

/* The option named @p name that the configuration file can set; nullptr
 * if there is none. */
static struct option const *
config_named (char const *name)
{
	for (struct option const *o = options; o->name; ++o)
		if (groups[o - options] && !strcmp(o->name, name))
			return o;
	return nullptr;
}

/* The member of the configuration file's member tools that sets option
 * @p o of the group TOOLS. */
static size_t
config_tool_member (struct option const *o)
{
	size_t i = 0;
	while (i + 1 < ARRAY_SIZE(tool_members) && tool_members[i].val != o->val)
		++i;
	return i;
}

/* Say that the configuration file has option @p o outside the object that
 * contains it; returns 2. */
static int
config_misplaced (struct config const *cfg,
                  struct option const *o)
{
	if (groups[o - options] == GROUP_ROOT)
		return config_complain(cfg, cfg->path, "%s must be at the top level", o->name);
	if (groups[o - options] == GROUP_TOOLS) {
		size_t i = config_tool_member(o);
		return config_complain(cfg, cfg->path, "%s must be tools.%s.%s", o->name,
		                       tool_members[i].tool, tool_members[i].name);
	}
	return config_complain(cfg, cfg->path, "%s must be in %s", o->name,
	                       group_names[groups[o - options]]);
}

/* Whether @p name names the object of a built-in tool in the member tools
 * of the configuration file. */
static bool
config_is_tool (char const *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(tool_members); ++i)
		if (!strcmp(name, tool_members[i].tool))
			return true;
	return false;
}

/* Apply the members of @p obj, the object of the built-in tool @p tool in
 * the configuration file's member tools, as the options that
 * tool_members[] names. */
static int
config_apply_tool (struct config      *cfg,
                   char const         *tool,
                   struct json_object *obj)
{
	if (!json_object_is_type(obj, json_type_object))
		return config_complain(cfg, cfg->path, "tools.%s must be an object", tool);
	struct json_object_iter it;
	json_object_object_foreachC(obj, it) {
		size_t i = 0;
		while (i < ARRAY_SIZE(tool_members) && (strcmp(tool_members[i].tool, tool) ||
		                                        strcmp(tool_members[i].name, it.key)))
			++i;
		if (i == ARRAY_SIZE(tool_members))
			return config_complain(cfg, cfg->path, "tools.%s: unknown member %s",
			                       tool, it.key);
		struct option const *o = options;
		while (o->val != tool_members[i].val)
			++o;
		int status = config_apply_member(cfg, o, it.val);
		if (status)
			return status;
	}
	return 0;
}

/* Apply the objects of the built-in tools in the configuration file's
 * member tools, if it is an object; config_allow() reads the rest of it. */
static int
config_apply_tools (struct config *cfg)
{
	struct json_object *tools = json_object_object_get(cfg->file, "tools");
	struct json_object_iter it;
	if (json_object_is_type(tools, json_type_object))
		json_object_object_foreachC(tools, it) {
			int status = config_is_tool(it.key)
			           ? config_apply_tool(cfg, it.key, it.val) : 0;
			if (status)
				return status;
		}
	return 0;
}

/* Apply the members of @p obj, the configuration file's object of group
 * @p g, as the options of their names. */
static int
config_apply_group (struct config      *cfg,
                    enum config_group   g,
                    struct json_object *obj)
{
	if (!json_object_is_type(obj, json_type_object))
		return config_complain(cfg, cfg->path, "%s must be an object",
		                       group_names[g]);
	struct json_object_iter it;
	json_object_object_foreachC(obj, it) {
		struct option const *o = config_named(it.key);
		if (!o)
			return config_complain(cfg, cfg->path, "%s: unknown member %s",
			                       group_names[g], it.key);
		int status = groups[o - options] != g ? config_misplaced(cfg, o)
		                                      : config_apply_member(cfg, o, it.val);
		if (status)
			return status;
	}
	return 0;
}

/* Apply the options in the configuration file's objects upstream, service,
 * testing and debugging.  The policy's members, "plugins" and "tools" are
 * not options: policy_create(), config_registry(), config_apply_tools()
 * and config_allow() read them. */
static int
config_apply_file (struct config *cfg)
{
	struct json_object_iter it;
	json_object_object_foreachC(cfg->file, it) {
		if (config_of_policy(it.key) || !strcmp(it.key, "plugins") ||
		    !strcmp(it.key, "tools"))
			continue;
		size_t g = config_find(group_names, ARRAY_SIZE(group_names), it.key);
		struct option const *o = config_named(it.key);
		int status = g ? config_apply_group(cfg, (enum config_group)g, it.val)
		           : o && groups[o - options] == GROUP_ROOT
		             ? config_apply_member(cfg, o, it.val)
		           : o ? config_misplaced(cfg, o)
		               : config_complain(cfg, cfg->path, "unknown member %s", it.key);
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
	status = config_apply_file(cfg);
	return status ? status : config_apply_tools(cfg);
}

/* Check the limits on clients against each other.  A body limit that
 * --max-body did not give shrinks to the budgets for the bodies, so that a
 * configuration that lowers a budget does not stop askai. */
static int
config_check (struct config *cfg)
{
	struct httpd_limits *lim = &cfg->http.limits;
	size_t const budgets[] = {
		lim->bodies, lim->client_bodies ? lim->client_bodies : SIZE_MAX
	};
	static char const *const names[] = { "--body-budget", "--client-budget" };
	for (size_t i = 0; i < ARRAY_SIZE(budgets); ++i) {
		if (lim->body <= budgets[i])
			continue;
		if (cfg->flags & CFG_BODY_GIVEN)
			return config_complain(cfg, nullptr, "--max-body exceeds %s", names[i]);
		lim->body = budgets[i];
	}
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

/* A repository is the session; a participant keeps the same writer identity
 * across restarts.  Resolve the transcript location once, before any service
 * opens it.  The journal validates and creates its parent directories. */
static int
config_session (struct config *cfg)
{
	if (!cfg->participant)
		cfg->participant = cfg->tool.name;
	if (!config_participant_valid(cfg->participant) &&
	    (cfg->notes || cfg->transcripts || cfg->ep.background))
		return config_complain(cfg, nullptr, "--participant: use 1-64 of "
		                       "[A-Za-z0-9_-]; set it explicitly if --tool-name "
		                       "does not fit: %s", cfg->participant);
	if (cfg->transcripts)
		return *cfg->transcripts ? 0
		       : config_complain(cfg, nullptr, "--transcripts: empty path");
	if (cfg->notes) {
		size_t n = strlen(cfg->notes);
		while (n > 1 && cfg->notes[n - 1] == '/')
			--n;
		if (n > INT_MAX)
			return config_complain(cfg, nullptr, "--notes: path too long");
		cfg->transcripts_owned = config_aprintf("%.*s.transcripts.git", (int)n,
		                                        cfg->notes);
	} else if (cfg->ep.background) {
		char const *state = getenv("XDG_STATE_HOME");
		if (state && *state) {
			if (*state != '/')
				return config_complain(cfg, nullptr, "XDG_STATE_HOME must be "
				                       "absolute, or set --transcripts explicitly");
			cfg->transcripts_owned = config_aprintf("%s/askai/%s/transcripts.git",
			                                        state, cfg->participant);
		} else {
			char const *home = getenv("HOME");
			if (!home || *home != '/')
				return config_complain(cfg, nullptr, "--background needs "
				                       "--transcripts, XDG_STATE_HOME or an absolute HOME");
			cfg->transcripts_owned = config_aprintf("%s/.local/state/askai/%s/transcripts.git",
			                                        home, cfg->participant);
		}
	} else {
		return 0;
	}
	if (!cfg->transcripts_owned)
		return config_oom(cfg);
	cfg->transcripts = cfg->transcripts_owned;
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

/* Work out the upstream's API key, URL and headers. */
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
	return config_headers(cfg, has_auth ? nullptr : key);
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
	struct json_object_iter it;
	json_object_object_foreachC(cfg->file, it)
		if (config_of_policy(it.key)) {
			(void)config_complain(cfg, nullptr, "note: ASKAI_TOKEN is ignored, "
			                      "as %s names who may connect", cfg->path);
			return;
		}
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

/* Loads the plugin of @p v, an element of the configuration file's
 * "plugins", and adds its tools to the registry.  The element is the
 * absolute path of the plugin, or an object.  The object's member "path" is
 * that path, and its member "options", if it has one, is an object of the
 * plugin's options.  The object has no other member. */
static int
config_plugin (struct config      *cfg,
               struct json_object *v)
{
	struct json_object *file = v;
	struct json_object *options = nullptr;
	if (json_object_is_type(v, json_type_object)) {
		struct json_object_iter it;
		bool has_path = false;
		bool has_options = false;
		json_object_object_foreachC(v, it) {
			if (!strcmp(it.key, "path")) {
				file = it.val;
				has_path = true;
			} else if (!strcmp(it.key, "options")) {
				options = it.val;
				has_options = true;
			} else {
				return config_complain(cfg, cfg->path, "plugins: unknown "
				                       "member of a plugin: %s", it.key);
			}
		}
		if (!has_path)
			return config_complain(cfg, cfg->path, "plugins: a plugin has no "
			                       "member path");
		if (has_options && !json_object_is_type(options, json_type_object))
			return config_complain(cfg, cfg->path, "plugins: the member "
			                       "options of a plugin is not an object");
	}
	char const *path = json_object_get_string(file);
	if (!json_object_is_type(file, json_type_string) || *path != '/')
		return config_complain(cfg, cfg->path, "plugins: not an absolute "
		                       "path: %s", path ? path : "null");
	int status = config_check_plugin(cfg, path);
	if (status)
		return status;
	char why[256];
	int e = registry_load(&cfg->registry, path, options, why, sizeof why);
	if (e == ENOMEM)
		return config_oom(cfg);
	if (e)
		return config_complain(cfg, path, "%s", why);
	return 0;
}

/* Fills the registry with the tools that askai offers: its own, then those
 * of the plugins that the configuration file lists in "plugins", loaded in
 * the order of the array.  A reload adds only askai's own
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

/* The configuration whose tools the roles of @p cfg name: the running one
 * on a reload, else @p cfg.  The names of all its tools go into @p tools,
 * by id, and the mask of the tools that a role can grant and askai can lend
 * to the upstream goes into @p *upstream. */
static struct config const *
config_tools (struct config const *cfg,
              char const          *tools[static TOOL_MAX],
              tool_mask           *upstream)
{
	struct config const *run = cfg->running ? cfg->running : cfg;
	struct registry const *reg = &run->registry;
	/* The tools that a role can grant have the first ids of the registry.
	 * With --upstream-tools, askai can offer such a tool to the upstream if
	 * the tool's kind has the flag TOOL_LENT. */
	bool lends = run->flags & CFG_UPSTREAM_TOOLS;
	*upstream = 0;
	for (uint32_t id = 0; id < reg->n_tools; ++id) {
		tools[id] = reg->tools[id].name;
		if (lends && id < reg->n_granted && reg->tools[id].kind->flags & TOOL_LENT)
			*upstream |= (tool_mask)1 << id;
	}
	return run;
}

/* Say that @p v, in the member tools of the configuration file, is no
 * name of a tool of the @p n tools @p tools that it may name: all but the
 * ask tool, the first.  Returns 2. */
static int
config_allow_none (struct config const *cfg,
                   char const          *where,
                   char const *const    tools[],
                   size_t               n,
                   struct json_object  *v)
{
	char const *name = json_object_get_string(v);
	if (json_object_is_type(v, json_type_string) && !strcmp(name, tools[0]))
		return config_complain(cfg, cfg->path, "%s: %s is the ask tool, which is "
		                       "always on", where, name);
	char names[1024];
	size_t len = 0;
	for (size_t id = 1; id < n; ++id)
		len += util_format(names + len, sizeof names - len, "%s%s", len ? ", " : "",
		                   tools[id]);
	return config_complain(cfg, cfg->path, "%s: askai has no tool %s; the tools "
	                       "are %s", where, json_object_to_json_string(v),
	                       len ? names : "none");
}

/* The mask, in @p *mask, of the tools that @p list names, an array of names
 * of the @p n tools @p tools, the ask tool aside. */
static int
config_allow_names (struct config const *cfg,
                    char const          *where,
                    char const *const    tools[],
                    size_t               n,
                    struct json_object  *list,
                    tool_mask           *mask)
{
	if (!json_object_is_type(list, json_type_array))
		return config_complain(cfg, cfg->path, "%s: not a list of tools", where);
	*mask = 0;
	size_t len = json_object_array_length(list);
	for (size_t i = 0; i < len; ++i) {
		struct json_object *v = json_object_array_get_idx(list, i);
		size_t id = json_object_is_type(v, json_type_string)
		          ? config_find(tools, n, json_object_get_string(v)) : 0;
		if (!id)
			return config_allow_none(cfg, where, tools, n, v);
		*mask |= (tool_mask)1 << id;
	}
	return 0;
}

/* The mask, in @p *mask, of the tools that @p spec allows: all for true,
 * none for false, those of a list, those of the member allow, or all but
 * those of the member deny.  The ask tool is not counted.  With @p top,
 * @p spec is the member tools itself, whose objects of the built-in tools
 * are skipped (config_apply_tools()). */
static int
config_allow_form (struct config const *cfg,
                   char const          *where,
                   char const *const    tools[],
                   size_t               n,
                   struct json_object  *spec,
                   bool                 top,
                   tool_mask           *mask)
{
	if (json_object_is_type(spec, json_type_boolean)) {
		*mask = json_object_get_boolean(spec) ? ~(tool_mask)0 : 0;
		return 0;
	}
	if (json_object_is_type(spec, json_type_array))
		return config_allow_names(cfg, where, tools, n, spec, mask);
	if (!json_object_is_type(spec, json_type_object))
		return config_complain(cfg, cfg->path, "%s: not true, false, a list or "
		                       "an object", where);
	struct json_object_iter it;
	json_object_object_foreachC(spec, it)
		if (strcmp(it.key, "allow") && strcmp(it.key, "deny") &&
		    !(top && config_is_tool(it.key)))
			return config_complain(cfg, cfg->path, "%s: unknown member %s",
			                       where, it.key);
	struct json_object *allow;
	struct json_object *deny;
	bool allows = json_object_object_get_ex(spec, "allow", &allow);
	if (!json_object_object_get_ex(spec, "deny", &deny)) {
		if (allows)
			return config_allow_names(cfg, where, tools, n, allow, mask);
		*mask = ~(tool_mask)0;
		return 0;
	}
	if (allows)
		return config_complain(cfg, cfg->path, "%s: allow and deny together",
		                       where);
	int status = config_allow_names(cfg, where, tools, n, deny, mask);
	*mask = ~*mask;
	return status;
}

/* As config_allow_form(), and the mask must not have background without
 * jobs, with which the output of a job is read. */
static int
config_allow_spec (struct config const *cfg,
                   char const          *where,
                   char const *const    tools[],
                   size_t               n,
                   struct json_object  *spec,
                   bool                 top,
                   tool_mask           *mask)
{
	int status = config_allow_form(cfg, where, tools, n, spec, top, mask);
	size_t background = config_find(tools, n, "background");
	size_t jobs = config_find(tools, n, "jobs");
	if (!status && background && *mask >> background & 1 && !(*mask >> jobs & 1))
		status = config_complain(cfg, cfg->path, "%s: background needs jobs", where);
	return status;
}

/* Check that the member tools @p v, an object with the member client or
 * upstream, has no other member but the objects of the built-in tools. */
static int
config_allow_parts (struct config const *cfg,
                    struct json_object  *v)
{
	struct json_object_iter it;
	json_object_object_foreachC(v, it) {
		if (!strcmp(it.key, "client") || !strcmp(it.key, "upstream") ||
		    config_is_tool(it.key))
			continue;
		if (!strcmp(it.key, "allow") || !strcmp(it.key, "deny"))
			return config_complain(cfg, cfg->path, "tools: allow and deny go "
			                       "into client and upstream");
		return config_complain(cfg, cfg->path, "tools: unknown member %s", it.key);
	}
	return 0;
}

/* The tools that the member tools of the configuration file allows clients
 * and the upstream, of the tools that config_tools() names.  Without the
 * member, it allows all.  It is one form of config_allow_spec() for both,
 * or an object with the members client and upstream, either of which may be
 * missing, each one of those forms for its own. */
static int
config_allow (struct config *cfg)
{
	struct json_object *v;
	if (!json_object_object_get_ex(cfg->file, "tools", &v))
		return 0;
	char const *tools[TOOL_MAX];
	tool_mask lendable;
	size_t n = config_tools(cfg, tools, &lendable)->registry.n_tools;
	struct json_object *client;
	struct json_object *upstream;
	bool clients = json_object_object_get_ex(v, "client", &client);
	bool lends = json_object_object_get_ex(v, "upstream", &upstream);
	int status = 0;
	if (!clients && !lends) {
		status = config_allow_spec(cfg, "tools", tools, n, v, true,
		                           &cfg->tools_clients);
		cfg->tools_upstream = cfg->tools_clients;
	} else {
		status = config_allow_parts(cfg, v);
		if (!status && clients)
			status = config_allow_spec(cfg, "tools: client", tools, n, client,
			                           false, &cfg->tools_clients);
		if (!status && lends)
			status = config_allow_spec(cfg, "tools: upstream", tools, n, upstream,
			                           false, &cfg->tools_upstream);
	}
	/* mcp_register() adds the ask tool first, with the id 0. */
	cfg->tools_clients |= 1;
	return status;
}

/* The ask tool's description, unless one is given: with the sentences
 * about tools and notes if the turns of a role that grants every tool get
 * any tool and the tool notes. */
static int
config_description (struct config *cfg)
{
	if (cfg->tool.description)
		return 0;
	char const *tools[TOOL_MAX];
	tool_mask lendable;
	struct config const *run = config_tools(cfg, tools, &lendable);
	tool_mask lent = mcp_lent(run, ~(tool_mask)0, ~(tool_mask)0);
	/* config_find() returns 0, the ask tool's id, for none. */
	size_t notes = config_find(tools, run->registry.n_tools, "notes");
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
	                                  lent ? " It may use some of this server's tools "
	                                         "itself, and its answer lists the calls it "
	                                         "made." : "",
	                                  notes && lent >> notes & 1
	                                  ? " It keeps notes in the shared session repository. "
	                                    "Each call loads its notes and can read the other "
	                                    "participants' notes." : "");
	if (!cfg->description)
		return config_oom(cfg);
	cfg->tool.description = cfg->description;
	return 0;
}

/* Make the policy, who may connect and what they may do, of the tools from
 * config_tools() that a role can grant, and that the member tools of the
 * running configuration allows clients. */
static int
config_policy (struct config *cfg)
{
	char const *tools[TOOL_MAX];
	tool_mask upstream;
	struct config const *run = config_tools(cfg, tools, &upstream);
	char why[1024];
	int e;
	cfg->policy = policy_create(cfg->file, tools, run->registry.n_granted,
	                            run->tools_clients, upstream, why, sizeof why, &e);
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
	status = config_session(cfg);
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
	status = config_allow(cfg);
	if (status)
		return status;
	status = config_description(cfg);
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
		.tools_clients = ~(tool_mask)0,
		.tools_upstream = ~(tool_mask)0,
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

/* Log @p url as the item @p name, without its user, password and query,
 * which can have secrets. */
static void
config_log_url (char const *name,
                char const *url)
{
	int scheme = 0;
	char const *rest = util_url_shown(url, &scheme);
	LOG_ALL(LOG_INFO, "  %-9s %.*s%.*s", name, scheme, url, (int)strcspn(rest, "?#"), rest);
}

/* Log where the turns' attach action uploads the notes, which needs an API
 * that takes files (ai_upload()) at a URL of its API (ai_upload_url()),
 * --notes and --upstream-tools. */
static void
config_log_files (struct config const *cfg)
{
	char *url = ai_upload_url(&cfg->ep);
	if (!ai_upload(cfg->ep.api))
		LOG_ALL(LOG_INFO, "  files     off (the API takes none)");
	else if (!url)
		LOG_ALL(LOG_INFO, "  files     off (the URL has no /v1beta/)");
	else if (!cfg->notes || !(cfg->flags & CFG_UPSTREAM_TOOLS))
		LOG_ALL(LOG_INFO, "  files     off (--notes and --upstream-tools)");
	else
		config_log_url("files", url);
	free(url);
}

void
config_log (struct config const *cfg)
{
	if (!cfg)
		return;
	struct ai_endpoint const *ep = &cfg->ep;
	LOG_ALL(LOG_INFO, "upstream:");
	LOG_ALL(LOG_INFO, "  provider  %s", ep->provider ? provider_names[ep->provider] : "none");
	LOG_ALL(LOG_INFO, "  API       %s", api_names[ep->api]);
	LOG_ALL(LOG_INFO, "  model     %s", ep->model ? ep->model : "none");
	LOG_ALL(LOG_INFO, "  background %s", !ep->background ? "off"
	        : ai_background(ep) ? "on (upstream and local)" : "on (local)");
	/* Home asks /props of a server that may be llama-server, and Gemini's
	 * API about its model, but not when told the slots and lending the
	 * upstream no tools (home_create()). */
	char *model = ai_props(ep) ? nullptr : ai_model_url(ep);
	bool asked = !ep->slots || cfg->flags & CFG_UPSTREAM_TOOLS;
	if (ep->context > 0)
		LOG_ALL(LOG_INFO, "  context   %ld tokens", ep->context);
	else if (ai_props(ep) && asked)
		LOG_ALL(LOG_INFO, "  context   from llama-server");
	else if (model && asked)
		LOG_ALL(LOG_INFO, "  context   from the model");
	else
		LOG_ALL(LOG_INFO, "  context   unknown");
	free(model);
	config_log_url("URL", ep->url);
	config_log_files(cfg);
	if (cfg->notes || cfg->transcripts)
		LOG_ALL(LOG_INFO, "participant: %s", cfg->participant);
	if (cfg->transcripts)
		LOG_ALL(LOG_INFO, "transcripts repository: %s", cfg->transcripts);
	mcp_report(&cfg->registry, cfg);
	config_log_policy(cfg);
}

void
config_log_policy (struct config const *cfg)
{
	if (!cfg)
		return;
	char const *tools[TOOL_MAX];
	tool_mask upstream;
	struct config const *run = config_tools(cfg, tools, &upstream);
	policy_log(cfg->policy, cfg->file, tools, run->registry.n_tools,
	           run->registry.n_granted, run->flags & CFG_UPSTREAM_TOOLS ? mcp_lent : nullptr,
	           run);
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
	free(cfg->transcripts_owned);
	free(cfg->headers);
	*cfg = (struct config){};
}
