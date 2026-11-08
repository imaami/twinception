/** @file config.c
 *
 * askai's configuration (config.h): the command line, and the
 * configuration file, whose objects' members are applied as the long
 * options of their names are, and the members of the objects of the
 * built-in tools in its member tools as their options, before the command
 * line's.
 */
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
#include <sys/stat.h>
#include <unistd.h>

#include "ai.h"
#include "brainstem.h"
#include "buf.h"
#include "code.h"
#include "config.h"
#include "dstr.h"
#include "git.h"
#include "httpd.h"
#include "jb.h"
#include "log.h"
#include "mcp.h"
#include "policy.h"
#include "tls.h"
#include "util.h"


#define CONFIG_API_NAME(id, name, auth) [AI_##id] = CSTR_VIEW_INIT(name),
static struct cstr_view const api_names[] = { AI_API_LIST(CONFIG_API_NAME) };
#undef CONFIG_API_NAME

/* Each API's header line prefix for the API key. */
#define CONFIG_API_AUTH(id, name, auth) [AI_##id] = auth,
static char const *const auths[] = { AI_API_LIST(CONFIG_API_AUTH) };
#undef CONFIG_API_AUTH

static struct cstr_view const provider_names[] = {
	[AI_LLAMA]      = CSTR_VIEW_INIT("llama"),
	[AI_OPENAI]     = CSTR_VIEW_INIT("openai"),
	[AI_ANTHROPIC]  = CSTR_VIEW_INIT("anthropic"),
	[AI_GOOGLE]     = CSTR_VIEW_INIT("google"),
	[AI_DEEPINFRA]  = CSTR_VIEW_INIT("deepinfra"),
	[AI_OPENROUTER] = CSTR_VIEW_INIT("openrouter"),
};

/* What each provider has: its URL for each API it serves, Gemini's
 * generateContent's followed by the model and ":streamGenerateContent"; the
 * variables tried for its key, at its own URLs only; and its default API.
 * No provider's are those of a URL that --url gives.
 */
static struct {
	char const *url[array_size(api_names)];
	char const *keys[2];
	uint64_t    api;  /* enum ai_api, widened over the padding */
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

/* The objects of the configuration file that contain the options, and
 * TOOLS for the options that an object of a built-in tool in the member
 * tools contains.
 */
enum config_group : uint8_t {
	GROUP_NONE, GROUP_UPSTREAM, GROUP_SERVICE, GROUP_TESTING, GROUP_DEBUGGING,
	GROUP_TOOLS, GROUP_ROOT
};

static struct cstr_view const group_names[] = {
	[GROUP_UPSTREAM]  = CSTR_VIEW_INIT("upstream"),
	[GROUP_SERVICE]   = CSTR_VIEW_INIT("service"),
	[GROUP_TESTING]   = CSTR_VIEW_INIT("testing"),
	[GROUP_DEBUGGING] = CSTR_VIEW_INIT("debugging"),
};

/* How an option takes its argument: the has_arg of getopt_long() in the
 * bits ARG_HAS, and the bits above them.
 */
enum config_arg : uint8_t {
	ARG_NONE        = no_argument,
	ARG_ONE         = required_argument,
	ARG_HAS         = 3,
	ARG_LIST        = 1u << 2,  /* a member of the configuration file can
	                               list the values in an array */
	ARG_RELOAD      = 1u << 3,  /* the option takes effect on a reload */
	ARG_MANY        = ARG_ONE | ARG_LIST,
	ARG_ONE_RELOAD  = ARG_ONE | ARG_RELOAD,
	ARG_NONE_RELOAD = ARG_NONE | ARG_RELOAD,
};

/* A line feed and the indentation of the help in the usage text. */
#define NL "\n                            "

/* The options, in the order of the usage text: X(kind, ID, name, short,
 * place, argument, ARG, help, ...). kind is the way the value is parsed
 * and stored, ID the constant ROW_ID of enum config_row, name the long
 * name and the name of the member in the configuration file, and short the
 * short form, or nothing. place is the object of the configuration file
 * that contains the option: NONE for config and help, and
 * TOOLS("TOOL.NAME") for the member NAME of the object TOOL in the member
 * tools. argument is ARG_ of enum config_arg, and ARG and help are the
 * name of the argument and the help in the usage text. The columns after
 * the help depend on the kind:
 * - count, bytes: the member of struct config, the least and the most
 *   value, and an optional CFG_ flag that the option sets. The digits of
 *   bytes may end in K, M or G for KiB, MiB or GiB.
 * - seconds: the member, the least and the most value, and the units of
 *   the member in a second.
 * - text: the member, which is set to point to the argument.
 * - flag: the CFG_ flag that the option sets.
 * - custom: none. config_apply() applies the option.
 * H(text) is a heading of the usage text.
 */
#define CONFIG_OPTION_LIST(X, H)                                                                        \
        X(custom,  CONFIG,           "config",           'c', NONE, ONE, "FILE",                        \
          "read options from FILE before the command" NL                                                \
          "line: a JSON object without comments.  Its" NL                                               \
          "objects upstream, service, testing and" NL                                                   \
          "debugging contain the options of the" NL                                                     \
          "sections below, named as the long options." NL                                               \
          "Its member tools can contain the objects of" NL                                              \
          "the built-in tools, whose members the" NL                                                    \
          "section Built-in tools names.  true sets an" NL                                              \
          "option without argument, and an array lists" NL                                              \
          "the values of a repeatable option.  The" NL                                                  \
          "command line overrides the file, but adds" NL                                                \
          "to its header, allow-origin, git and code" NL                                                \
          "lists.  Only the last --config is read.  See" NL                                             \
          "doc/configuration.md and" NL                                                                 \
          "doc/askai.example.json.")                                                                    \
        X(custom,  HELP,             "help",             'h', NONE, NONE, "",                           \
          "show this help")                                                                             \
        H("Session (top-level configuration members)")                                                  \
        X(custom,  PARTICIPANT,      "participant",      ,    ROOT, ONE, "NAME",                        \
          "stable writer identity, 1 to 64 characters" NL                                               \
          "from [A-Za-z0-9_-]; default: tool-name.  Use" NL                                             \
          "distinct names for collaborating instances")                                                 \
        X(text,    TRANSCRIPTS,      "transcripts",      ,    ROOT, ONE, "PATH",                        \
          "separate git repository for reasoning text," NL                                              \
          "with private job recovery state; default:" NL                                                \
          "<notes>.transcripts.git with --notes, else" NL                                               \
          "$XDG_STATE_HOME/askai/NAME/transcripts.git" NL                                               \
          "with --background, using $HOME/.local/state" NL                                              \
          "if XDG_STATE_HOME is not absolute",                                                          \
          transcripts)                                                                                  \
        H("Upstream AI (\"upstream\")")                                                                 \
        X(custom,  PROVIDER,         "provider",         'p', UPSTREAM, ONE, "NAME",                    \
          "llama (llama-server at 127.0.0.1:9931)," NL                                                  \
          "openai, anthropic, google, deepinfra or" NL                                                  \
          "openrouter; the provider sets the defaults" NL                                               \
          "of --api, --url and --key-env")                                                              \
        X(custom,  API,              "api",              ,    UPSTREAM, ONE, "NAME",                    \
          "chat (chat completions), responses," NL                                                      \
          "messages (Anthropic), generate (Gemini) or" NL                                               \
          "interactions (Gemini Interactions)" NL                                                       \
          "(default: the API of the provider, else" NL                                                  \
          "chat)")                                                                                      \
        X(custom,  URL,              "url",              'u', UPSTREAM, ONE, "URL",                     \
          "endpoint URL, http:// or https:// (default:" NL                                              \
          "the URL of the provider; required without" NL                                                \
          "--provider)")                                                                                \
        X(text,    MODEL,            "model",            'm', UPSTREAM, ONE, "NAME",                    \
          "model to ask (required at the URL of a" NL                                                   \
          "provider other than llama)",                                                                 \
          ep.model)                                                                                     \
        X(text,    KEY_ENV,          "key-env",          'k', UPSTREAM, ONE, "VAR",                     \
          "environment variable that contains the API" NL                                               \
          "key (default: at the URL of a provider, the" NL                                              \
          "usual variable of the provider, which llama" NL                                              \
          "may leave unset; else ASKAI_API_KEY, if set)",                                               \
          key_env)                                                                                      \
        X(custom,  HEADER,           "header",           'H', UPSTREAM, MANY, "LINE",                   \
          "extra request header, Name: value" NL                                                        \
          "(repeatable)")                                                                               \
        X(text,    SYSTEM,           "system",           's', UPSTREAM, ONE, "TEXT",                    \
          "default system prompt",                                                                      \
          ep.system)                                                                                    \
        X(custom,  TEMPERATURE,      "temperature",      't', UPSTREAM, ONE, "N",                       \
          "sampling temperature, a number from 0" NL                                                    \
          "(default: none sent)")                                                                       \
        X(count,   MAX_TOKENS,       "max-tokens",       'n', UPSTREAM, ONE, "N",                       \
          "output token limit (default: none sent, but" NL                                              \
          "4096 with the messages API)",                                                                \
          ep.max_tokens, 1, INT32_MAX)                                                                  \
        X(custom,  EXTRA,            "extra",            'x', UPSTREAM, ONE, "JSON",                    \
          "JSON object that askai merges into each" NL                                                  \
          "request body, recursively; a null member" NL                                                 \
          "removes that key")                                                                           \
        X(custom,  BACKGROUND,       "background",       ,    UPSTREAM, NONE, "",                       \
          "return ask calls immediately as jobs, and" NL                                                \
          "keep their upstream streams alive after" NL                                                  \
          "clients disconnect; request native background" NL                                            \
          "execution with OpenAI Responses and Gemini" NL                                               \
          "Interactions (default off)")                                                                 \
        X(seconds, TIMEOUT,          "timeout",          'T', UPSTREAM, ONE, "SECONDS",                 \
          "time limit on each upstream call: first on" NL                                               \
          "the wait for an upstream slot, then again on" NL                                             \
          "the call itself (default 600, 0 = none)",                                                    \
          ep.timeout_ms, 0, INT32_MAX / 1000, 1000)                                                     \
        X(seconds, CHUNK_TIMEOUT,    "chunk-timeout",    ,    UPSTREAM, ONE, "SECONDS",                 \
          "longest pause in an answer once it has" NL                                                   \
          "begun (default 60, 0 = none)",                                                               \
          ep.chunk_ms, 0, INT32_MAX / 1000, 1000)                                                       \
        X(count,   SLOTS,            "slots",            ,    UPSTREAM, ONE, "N",                       \
          "upstream calls at once (default: total_slots" NL                                             \
          "from the /props of llama-server, else 4)",                                                   \
          ep.slots, 1, UINT16_MAX)                                                                      \
        X(count,   MAX_CALLS,        "max-calls",        ,    UPSTREAM, ONE, "N",                       \
          "upstream calls queued or running, at most" NL                                                \
          "(default 256), and at most 8 queued for one" NL                                              \
          "identity, one Unix user, or else one session" NL                                             \
          "or client address; an ask call past those" NL                                                \
          "fails at once with an error to try again",                                                   \
          calls, 1, 1'000'000)                                                                          \
        X(flag,    UPSTREAM_TOOLS,   "upstream-tools",   ,    UPSTREAM, NONE, "",                       \
          "offer tools to the upstream in ask calls:" NL                                                \
          "man, git and the plugin tools that allow it," NL                                             \
          "as the role of the client allows, and notes" NL                                              \
          "with --notes; with any of those also" NL                                                     \
          "compact, findings, jobs, wait and timer," NL                                                 \
          "and beside a tool that can run in the" NL                                                    \
          "background also background; an ask call" NL                                                  \
          "runs the tool calls of each turn and sends" NL                                               \
          "the next upstream request until an answer" NL                                                \
          "ends the call",                                                                              \
          CFG_UPSTREAM_TOOLS)                                                                           \
        X(count,   MAX_TURNS,        "max-turns",        ,    UPSTREAM, ONE, "N",                       \
          "upstream calls that an ask call makes at" NL                                                 \
          "most, with --upstream-tools (default 32)",                                                   \
          turns, 1, 1000)                                                                               \
        X(count,   CONTEXT,          "context",          ,    UPSTREAM, ONE, "TOKENS",                  \
          "context size of the upstream, used with" NL                                                  \
          "--upstream-tools: a tool result in a turn is" NL                                             \
          "at most an eighth of it; at a quarter and at" NL                                             \
          "half, the upstream is reminded to write" NL                                                  \
          "notes, with --notes; from two thirds on, it" NL                                              \
          "is asked to call compact; and when it is" NL                                                 \
          "full, askai compacts the conversation" NL                                                    \
          "(default: n_ctx from the /props of" NL                                                       \
          "llama-server, else unknown; 0 = the default)",                                               \
          ep.context, 0, INT32_MAX)                                                                     \
        X(custom,  TOOL_NAME,        "tool-name",        ,    UPSTREAM, ONE, "NAME",                    \
          "name of the ask tool, 1 to 128 characters" NL                                                \
          "from A-Z, a-z, 0-9, _, . and - (default" NL                                                  \
          "ask_ai)")                                                                                    \
        X(text,    TOOL_DESCRIPTION, "tool-description", ,    UPSTREAM, ONE, "TEXT",                    \
          "description of the ask tool (default: a text" NL                                             \
          "built from --model, --upstream-tools and" NL                                                 \
          "--notes)",                                                                                   \
          tool.description)                                                                             \
        H("MCP server (\"service\")")                                                                   \
        X(custom,  LISTEN,           "listen",           'l', SERVICE, MANY, "ADDR",                    \
          "[host:]port, where host is 127.0.0.1 if it" NL                                               \
          "is left out; unix:PATH for a Unix socket; or" NL                                             \
          "tls:[host:]port for TLS (repeatable;" NL                                                     \
          "default 127.0.0.1:8090)")                                                                    \
        X(text,    TLS_CERT,         "tls-cert",         ,    SERVICE, ONE_RELOAD, "FILE",              \
          "certificate chain for tls: listeners, in" NL                                                 \
          "PEM: the certificate of the server first," NL                                                \
          "then the intermediate CA certificates",                                                      \
          tls_cert)                                                                                     \
        X(text,    TLS_KEY,          "tls-key",          ,    SERVICE, ONE_RELOAD, "FILE",              \
          "private key of that certificate, in PEM," NL                                                 \
          "unencrypted: ECDSA on P-256, P-384 or P-521," NL                                             \
          "or Ed25519; others must not be able to read" NL                                              \
          "FILE",                                                                                       \
          tls_key)                                                                                      \
        X(flag,    TLS_ALLOW_RSA,    "tls-allow-rsa",    ,    SERVICE, NONE_RELOAD, "",                 \
          "allow an RSA key too",                                                                       \
          CFG_TLS_RSA)                                                                                  \
        X(custom,  ALLOW_ORIGIN,     "allow-origin",     'o', SERVICE, MANY, "URL",                     \
          "allow browser requests whose Origin header" NL                                               \
          "is exactly URL, such as https://example.com" NL                                              \
          "(repeatable; '*' allows any; loopback" NL                                                    \
          "Origins are always allowed)")                                                                \
        X(count,   LOOPS,            "loops",            ,    SERVICE, ONE, "N",                        \
          "event loops that serve clients (default 0:" NL                                               \
          "one fewer than the CPUs askai may use, at" NL                                                \
          "least 1, at most 255)",                                                                      \
          loops, 0, 255) /* as many as a handle's loop field names, home being 0 */                     \
        X(seconds, GRACE,            "grace",            ,    SERVICE, ONE, "SECONDS",                  \
          "time to finish the requests under way after" NL                                              \
          "SIGINT, SIGTERM or SIGHUP (default 5)",                                                      \
          brainstem.grace_ms, 1, 3600, 1000)                                                            \
        X(bytes,   MEMORY_HIGH,      "memory-high",      ,    SERVICE, ONE, "BYTES",                    \
          "resident memory from which memory pressure" NL                                               \
          "is high (default: a sixteenth of RAM, or of" NL                                              \
          "the lowest cgroup limit, at least 256M)",                                                    \
          brainstem.memory_high, 1, SIZE_MAX / 2)                                                       \
        H("Limits on clients (\"service\"; BYTES and RATE may end in K, M or G for\n"                   \
          "KiB, MiB or GiB; 0 = none where noted)")                                                     \
        X(bytes,   MAX_HEAD,         "max-head",         ,    SERVICE, ONE, "BYTES",                    \
          "request line and headers (default 16K," NL                                                   \
          "at most 65535)",                                                                             \
          http.limits.head, 1, UINT16_MAX)                                                              \
        X(count,   MAX_FIELDS,       "max-fields",       ,    SERVICE, ONE, "N",                        \
          "header fields (default 64)",                                                                 \
          http.limits.fields, 0, UINT32_MAX)                                                            \
        X(count,   HEAD_TIMEOUT,     "head-timeout",     ,    SERVICE, ONE, "SECONDS",                  \
          "time limit for a TLS handshake, and for a" NL                                                \
          "request line and headers, each from its" NL                                                  \
          "first byte (default 10, 0 = none)",                                                          \
          http.limits.head_time, 0, UINT32_MAX)                                                         \
        X(bytes,   MAX_BODY,         "max-body",         ,    SERVICE, ONE, "BYTES",                    \
          "request body, at most --body-budget" NL                                                      \
          "(default 8M)",                                                                               \
          http.limits.body, 0, SIZE_MAX / 2, CFG_BODY_GIVEN)                                            \
        X(bytes,   MIN_BODY_RATE,    "min-body-rate",    ,    SERVICE, ONE, "RATE",                     \
          "bytes per second that a request body must" NL                                                \
          "average from the end of its headers, once" NL                                                \
          "--body-grace has passed (default 4K," NL                                                     \
          "0 = none)",                                                                                  \
          http.limits.body_rate, 0, UINT32_MAX)                                                         \
        X(count,   BODY_GRACE,       "body-grace",       ,    SERVICE, ONE, "SECONDS",                  \
          "time after the headers before" NL                                                            \
          "--min-body-rate applies (default 5)",                                                        \
          http.limits.body_grace, 0, UINT32_MAX)                                                        \
        X(bytes,   BODY_BUDGET,      "body-budget",      ,    SERVICE, ONE, "BYTES",                    \
          "request bodies in memory for all clients" NL                                                 \
          "together (default 256M)",                                                                    \
          http.limits.bodies, 0, SIZE_MAX / 2)                                                          \
        X(bytes,   CLIENT_BUDGET,    "client-budget",    ,    SERVICE, ONE, "BYTES",                    \
          "the part of --body-budget for one client" NL                                                 \
          "address (default: a quarter of it, at least" NL                                              \
          "--max-body)",                                                                                \
          http.limits.client_bodies, 1, SIZE_MAX / 2)                                                   \
        X(count,   MAX_DEPTH,        "max-depth",        ,    SERVICE, ONE, "N",                        \
          "JSON nesting in a request (default 32)",                                                     \
          mcp.depth, 1, INT32_MAX - 1)                                                                  \
        X(count,   MAX_VALUES,       "max-values",       ,    SERVICE, ONE, "N",                        \
          "JSON values in a request, keys included" NL                                                  \
          "(default 1024; json-c uses up to 0.7 KiB for" NL                                             \
          "each value while it parses)",                                                                \
          mcp.values, 1, UINT32_MAX)                                                                    \
        X(count,   MAX_MEMBERS,      "max-members",      ,    SERVICE, ONE, "N",                        \
          "members of one JSON object in a request" NL                                                  \
          "(default 1024)",                                                                             \
          mcp.members, 1, UINT32_MAX - 1)                                                               \
        X(count,   SSE_KEEPALIVE,    "sse-keepalive",    ,    SERVICE, ONE, "SECONDS",                  \
          "time between the comment lines that askai" NL                                                \
          "writes on an idle event stream (default 15," NL                                              \
          "0 = none)",                                                                                  \
          mcp.keepalive, 0, UINT32_MAX / 2)                                                             \
        X(count,   MAX_CLIENT_CONNS, "max-client-conns", ,    SERVICE, ONE, "N",                        \
          "connections per client address: an IPv4" NL                                                  \
          "address, an IPv6 /64, or the user of a Unix" NL                                              \
          "socket client (default 256, 0 = none)",                                                      \
          http.limits.clients, 0, UINT32_MAX)                                                           \
        X(count,   IDLE_TIMEOUT,     "idle-timeout",     ,    SERVICE, ONE, "SECONDS",                  \
          "time a connection may stay open with no" NL                                                  \
          "request under way, before its first request" NL                                              \
          "and between requests (default 30, 0 = none)",                                                \
          http.limits.idle_time, 0, UINT32_MAX)                                                         \
        X(bytes,   OUTPUT_PAUSE,     "output-pause",     ,    SERVICE, ONE, "BYTES",                    \
          "pending output at which no more input is" NL                                                 \
          "read from a connection, and a response is" NL                                                \
          "paused until at most half of this is pending" NL                                             \
          "(default 256K, at most --max-output)",                                                       \
          http.limits.out_pause, 1, SIZE_MAX / 2)                                                       \
        X(bytes,   MAX_OUTPUT,       "max-output",       ,    SERVICE, ONE, "BYTES",                    \
          "close a connection whose pending output a" NL                                                \
          "response would take past this, unless it" NL                                                 \
          "is all that is pending (default 8M)",                                                        \
          http.limits.out_max, 1, SIZE_MAX / 2)                                                         \
        X(count,   SEND_TIMEOUT,     "send-timeout",     ,    SERVICE, ONE, "SECONDS",                  \
          "longest stall of pending output" NL                                                          \
          "(default 30, 0 = none)",                                                                     \
          http.limits.write_time, 0, UINT32_MAX)                                                        \
        H("Built-in tools (\"tools\", each in the member in parentheses)")                              \
        X(flag,    MAN,              "man",              ,    TOOLS("man.enabled"), NONE, "",           \
          "also offer the tool man, which returns a" NL                                                 \
          "page of the manual as man(1) renders it, in" NL                                              \
          "plain UTF-8 text; askai renders each page" NL                                                \
          "once and stores it (tools.man.enabled)",                                                     \
          CFG_MAN)                                                                                      \
        X(custom,  GIT,              "git",              ,    TOOLS("git.repositories"), MANY, "PATH",  \
          "also offer the tool git: read-only git" NL                                                   \
          "commands in the repository at PATH, the top" NL                                              \
          "of a work tree or a bare repository, named" NL                                               \
          "by the last component of PATH less .git" NL                                                  \
          "(repeatable; tools.git.repositories)")                                                       \
        X(seconds, GIT_TIMEOUT,      "git-timeout",      ,    TOOLS("git.timeout"), ONE, "SECONDS",     \
          "deadline of a git call, at which git gets" NL                                                \
          "SIGTERM, and SIGKILL one second later" NL                                                    \
          "(default 30; tools.git.timeout)",                                                            \
          git_timeout, 1, 3600, NS)                                                                     \
        X(custom,  CODE,             "code",             ,                                              \
          TOOLS("code.compile-commands"), MANY, "REPO=FILE",                                            \
          "also offer the tool code, which runs" NL                                                     \
          "clang on the committed C code of the" NL                                                     \
          "git repository REPO, a name or absolute" NL                                                  \
          "path of --git, with the flags of the" NL                                                     \
          "compile commands in FILE, relative to" NL                                                    \
          "REPO; askai runs askai-code for each call" NL                                                \
          "(repeatable; tools.code.compile-commands)")                                                  \
        X(flag,    CODE_UNCONFINED,  "code-unconfined",  ,    TOOLS("code.unconfined"), NONE, "",       \
          "run askai-code without Landlock and" NL                                                      \
          "seccomp, which the tool code needs" NL                                                       \
          "otherwise (tools.code.unconfined)",                                                          \
          CFG_UNCONFINED)                                                                               \
        X(text,    NOTES,            "notes",            ,    TOOLS("notes.directory"), ONE, "PATH",    \
          "shared session's git repository, created" NL                                                 \
          "bare if absent.  Each participant" NL                                                        \
          "writes its own branches and reads all notes;" NL                                             \
          "offer notes to clients as their role" NL                                                     \
          "allows, and to the upstream with" NL                                                         \
          "--upstream-tools (tools.notes.directory)",                                                   \
          notes)                                                                                        \
        X(flag,    CLIENT_JOBS,      "client-jobs",      ,    TOOLS("jobs.clients"), NONE, "",          \
          "also offer clients the tools background," NL                                                 \
          "jobs, wait and timer, as their role allows:" NL                                              \
          "background runs a call of another tool as a" NL                                              \
          "job, whose result askai stores for up to an" NL                                              \
          "hour; timer sets a timer; wait waits for" NL                                                 \
          "jobs and timers (tools.jobs.clients)",                                                       \
          CFG_CLIENT_JOBS)                                                                              \
        H("Test aids (\"testing\")")                                                                    \
        X(count,   TEST_MAN_RECHECK, "test-man-recheck", ,    TESTING, ONE, "SECONDS",                  \
          "once the lookup of the file of a page (man" NL                                               \
          "-w) is older than this, the next man call" NL                                                \
          "for the page repeats the lookup in the" NL                                                   \
          "background (default 600, 0 = on each call)",                                                 \
          man_recheck, 0, 86'400)                                                                       \
        X(count,   TEST_STALL,       "test-stall",       ,    TESTING, ONE, "MS",                       \
          "SIGUSR2 stops each event loop in one turn" NL                                                \
          "for this long",                                                                              \
          brainstem.stall_ms, 1, 3'600'000)                                                             \
        X(text,    TEST_PRESSURE,    "test-pressure",    ,    TESTING, ONE, "FILE",                     \
          "memory pressure is at least the level in" NL                                                 \
          "FILE, 0 (none), 1 (high) or 2 (critical)," NL                                                \
          "read each second",                                                                           \
          brainstem.pressure)                                                                           \
        X(flag,    TEST_FIFO,        "test-fifo",        ,    TESTING, NONE, "",                        \
          "upstream calls wait in one queue for each" NL                                                \
          "class, first in first out, and without the" NL                                               \
          "limit of 8 queued calls",                                                                    \
          CFG_TEST_FIFO)                                                                                \
        X(flag,    TEST_TASK,        "test-task",        ,    TESTING, NONE, "",                        \
          "also offer the tool test_task: CPU work in" NL                                               \
          "250 us slices, run as a task on the client" NL                                               \
          "loops; or, given a prompt, an upstream call" NL                                              \
          "run as background work",                                                                     \
          CFG_TEST_TASK)                                                                                \
        H("Debugging (\"debugging\")")                                                                  \
        X(flag,    VERBOSE,          "verbose",          'v', DEBUGGING, NONE, "",                      \
          "log each tool call of a client as it ends," NL                                               \
          "with its arguments, and each ask call as" NL                                                 \
          "it starts; each upstream turn, each tool" NL                                                 \
          "call of a turn or of a background job," NL                                                   \
          "each compaction and each notice about the" NL                                                \
          "context; each web request of a plugin; and" NL                                               \
          "each man page that askai renders",                                                           \
          CFG_VERBOSE)                                                                                  \
        X(count,   WATCHDOG,         "watchdog",         ,    DEBUGGING, ONE, "MS",                     \
          "log a turn of an event loop longer than" NL                                                  \
          "this (default 20, 0 = none)",                                                                \
          brainstem.watchdog_ms, 0, 60'000)

/* A heading of the usage text, which the other expansions leave out. */
#define CONFIG_NO_HEADING(text)

/* The rows of CONFIG_OPTION_LIST, in its order. */
enum config_row : uint8_t {
#define CONFIG_ROW(kind, id, ...) ROW_##id,
	CONFIG_OPTION_LIST(CONFIG_ROW, CONFIG_NO_HEADING)
#undef CONFIG_ROW
};

/* The first of the arguments. */
#define CONFIG_FIRST(x, ...) x

/* The options for getopt_long(). The value of an option is its short form,
 * or else 256 plus its row.
 */
static struct option const options[] = {
#define CONFIG_OPTION(kind, id, name, s, place, arg, ...) \
        { name, ARG_##arg & ARG_HAS, nullptr, CONFIG_VALUE(id, s) },
#define CONFIG_VALUE(id, ...) CONFIG_FIRST(__VA_ARGS__ __VA_OPT__(,) 256 + ROW_##id,)
	CONFIG_OPTION_LIST(CONFIG_OPTION, CONFIG_NO_HEADING)
#undef CONFIG_VALUE
#undef CONFIG_OPTION
	{ nullptr, 0, nullptr, 0 }
};

/* The short forms for getopt_long(), each followed by ':' if the option
 * requires an argument.
 */
static char const shorts[] = {
#define CONFIG_SHORT(kind, id, name, s, place, arg, ...) CONFIG_SHORT_(arg, s)
#define CONFIG_SHORT_(arg, ...) __VA_OPT__(__VA_ARGS__, CONFIG_COLON_##arg)
#define CONFIG_COLON_NONE
#define CONFIG_COLON_ONE ':',
#define CONFIG_COLON_MANY ':',
	CONFIG_OPTION_LIST(CONFIG_SHORT, CONFIG_NO_HEADING)
#undef CONFIG_COLON_MANY
#undef CONFIG_COLON_ONE
#undef CONFIG_COLON_NONE
#undef CONFIG_SHORT_
#undef CONFIG_SHORT
	'\0'
};

/* Print the usage text to @p out. Each option is printed by one fprintf()
 * whose arguments are the short form, if there is one, and the padding
 * after the head of the option. The help starts in column 28 after a head
 * shorter than 27 bytes, and on the next line after a longer head.
 */
static void
config_usage (struct config const *cfg,
              FILE                *out)
{
	#define CONFIG_HELP(kind, id, name, s, place, arg, meta, help, ...) \
	        CONFIG_HELP_(name " " meta, help, s)
	#define CONFIG_HELP_(head, help, ...)                                         \
	        fprintf(out, CONFIG_FIRST(__VA_OPT__("  -%c, --",) "      --",) head  \
	                "%s" help "\n", __VA_ARGS__ __VA_OPT__(,)                     \
	                CONFIG_PAD(sizeof "      --" head - 1));
	#define CONFIG_PAD(n) ((n) < 27 ? &"                            "[n] \
	                                : "\n                            ")
	#define CONFIG_HEADING(text) fputs("\n" text ":\n", out);
	fprintf(out, "Usage: %s [options]\n"
	        "MCP server (Streamable HTTP) with a tool that asks another AI.\n"
	        "\n", cfg->argv[0]);
	CONFIG_OPTION_LIST(CONFIG_HELP, CONFIG_HEADING)
	#undef CONFIG_HEADING
	#undef CONFIG_PAD
	#undef CONFIG_HELP_
	#undef CONFIG_HELP
	fprintf(out,
	        "\n"
	        "The members roles, identities, unix-groups and anonymous of the\n"
	        "configuration file define who may connect and what each client may do\n"
	        "(see README.md).  With any of them, askai ignores ASKAI_TOKEN.  Without\n"
	        "them, every client that askai accepts may call every tool.  If\n"
	        "ASKAI_TOKEN is set and not empty, askai accepts only clients that send\n"
	        "'Authorization: Bearer TOKEN' with that value as TOKEN, and these may\n"
	        "request GET /metrics.  Otherwise askai accepts every client but one\n"
	        "that sends a token on a Unix socket, and only clients on a Unix\n"
	        "socket, or on loopback with no Host header or the Host localhost,\n"
	        "127.0.0.1 or [::1], may request GET /metrics, which returns metrics in\n"
	        "the Prometheus text format.\n"
	        "Under every policy, askai refuses a client on a Unix socket whose uid\n"
	        "is in a range of /etc/subuid, is 4294967295 or is the kernel's\n"
	        "overflow uid (see doc/configuration.md).\n"
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
	        "the objects man, git, code, notes and jobs of the built-in tools.\n"
	        "SIGUSR1 prints to stderr, for each event loop, the times of its turns and\n"
	        "steps, the lateness of its timers and the most work that one step did\n"
	        "since the previous SIGUSR1.  The lines name the loops: loop1 to loopN\n"
	        "serve clients, and home accepts them and runs the upstream calls.\n"
	        "With --config or a tls: listener, SIGHUP reloads the configuration file\n"
	        "and the TLS certificate and key.  A reload applies only the members\n"
	        "roles, identities, unix-groups and anonymous and the TLS files.  It\n"
	        "also reads /etc/subuid and the kernel's overflow ids again, and\n"
	        "/etc/subgid if unix-groups names a group.  Other options need a\n"
	        "restart.  A failed reload changes nothing.\n"
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

#undef CONFIG_FIRST
#undef NL

/* Whether a line of cfg->headers begins, in any case, with @p prefix up to
 * its colon.
 */
static bool
config_has_header (struct config const *cfg,
                   char const          *prefix)
{
	struct cstr_view const name = { prefix, strcspn(prefix, ":") + 1 };
	for (char const **line = cfg->headers; *line; ++line)
		if (cstr_view_starts_case(cstr_view(*line), name))
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
	size_t n = strspn(name, UTIL_ALNUM "_.-");
	return n && n <= 128 && !name[n];
}

/* Bytes of a configuration file, at most. */
enum { FILE_MAX = 1 << 20 };

/* Say what is wrong, as of the configuration file @p where unless that is
 * nullptr: on stderr, or in the log on a reload. Returns 2, the exit status
 * for an invalid option or environment.
 */
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
	if (cfg->flags & CFG_LOGGED)
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

/* The row of the option whose value is @p opt, or the row of the
 * terminator of options[] if there is none.
 */
static size_t
config_row (int opt)
{
	if (opt >= 256)
		return (size_t)opt - 256;
	size_t row = 0;
	while (options[row].name && options[row].val != opt)
		++row;
	return row;
}

/* Append @p item to the nullptr-terminated @p list of @p *n items, which
 * has room for it (config_lists()).
 */
static void
config_push (char const **list,
             size_t       *n,
             char const   *item)
{
	list[(*n)++] = item;
	list[*n] = nullptr;
}

int
config_aprintf (char       **str,
                char const  *fmt,
                ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vasprintf(str, fmt, ap);
	va_end(ap);
	if (n >= 0)
		return 0;
	*str = nullptr;
	return ENOMEM;
}

/* Check a number in the object of --extra, as a json_c_visit() callback.
 * The function returns JSON_C_VISIT_RETURN_ERROR for a double that is NaN
 * or infinite. For a finite double, it resets the serializer to the
 * default serializer of json-c, which prints the value of the double. The
 * parser of json-c sets for each double a serializer that prints the text
 * of the number as the parser read it, and json-c reads some numbers that
 * JSON does not allow, such as "1.".
 */
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
 * nullptr if @p arg is from the command line.
 */
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
 * object whose numbers are finite. @p where is the path of the
 * configuration file that contains @p arg, or nullptr if @p arg is from the
 * command line.
 */
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

/* How an option of rows[] takes its argument. */
enum config_take : uint8_t {
	TAKE_BYTES    = 1u << 0,  /* the digits may end in K, M or G for KiB,
	                             MiB or GiB */
	TAKE_POSITIVE = 1u << 1,  /* the count is at least 1 */
	TAKE_TEXT     = 1u << 2,  /* the member points to the argument */
	TAKE_CUSTOM   = 1u << 3,  /* config_apply() applies the option */
};

/* Each option, by its row: its place in the configuration file, its ARG_
 * flags, and what it stores. The member of struct config at @p at, of
 * @p width bytes, is set to point to the argument (TAKE_TEXT), or to a
 * count up to @p max, parsed as the bits of @p take say, times @p scale.
 * The option also sets the CFG_ flags @p flag. An option without a member
 * only sets the flags.
 */
static struct config_option {
	char const *member;  /* "TOOL.NAME" of the member tools, in the group
	                        TOOLS */
	uint64_t    max;
	uint32_t    scale;
	uint32_t    flag;
	uint32_t    at;
	uint8_t     width;
	uint8_t     take;    /* enum config_take */
	uint8_t     group;   /* enum config_group */
	uint8_t     arg;     /* enum config_arg */
} const rows[] = {
#define CONFIG_ROW(kind, id, name, s, place, argument, meta, help, ...)          \
        [ROW_##id] = { CONFIG_STORE_##kind(__VA_ARGS__), .group = GROUP_##place, \
                       .arg = ARG_##argument },
/* The place TOOLS("TOOL.NAME"): the group TOOLS, and the member. */
#define GROUP_TOOLS(m) GROUP_TOOLS, .member = m
#define CONFIG_AT(m) .at = offsetof(struct config, m), \
                     .width = sizeof ((struct config *)nullptr)->m
#define CONFIG_STORE_custom() .take = TAKE_CUSTOM
#define CONFIG_STORE_text(m) CONFIG_AT(m), .take = TAKE_TEXT
#define CONFIG_STORE_flag(bit) .flag = bit
#define CONFIG_STORE_count(m, min, max_, ...)                                  \
        .max = max_, .scale = 1, .flag = 0 __VA_OPT__(| __VA_ARGS__),          \
        CONFIG_AT(m), .take = (min) ? TAKE_POSITIVE : 0
#define CONFIG_STORE_bytes(m, min, max_, ...)                                  \
        .max = max_, .scale = 1, .flag = 0 __VA_OPT__(| __VA_ARGS__),          \
        CONFIG_AT(m), .take = TAKE_BYTES | ((min) ? TAKE_POSITIVE : 0)
#define CONFIG_STORE_seconds(m, min, max_, scale_)                             \
        .max = max_, .scale = scale_, CONFIG_AT(m), .take = (min) ? TAKE_POSITIVE : 0
	CONFIG_OPTION_LIST(CONFIG_ROW, CONFIG_NO_HEADING)
#undef CONFIG_STORE_seconds
#undef CONFIG_STORE_bytes
#undef CONFIG_STORE_count
#undef CONFIG_STORE_flag
#undef CONFIG_STORE_text
#undef CONFIG_STORE_custom
#undef CONFIG_AT
#undef GROUP_TOOLS
#undef CONFIG_ROW
};

#undef CONFIG_NO_HEADING
#undef CONFIG_OPTION_LIST

/* @p str, a count as @p n takes it, in decimal digits, in @p *value. EINVAL
 * if it is not one.
 */
static int
config_parse_count (char const                 *str,
                    struct config_option const *n,
                    uint64_t                   *value)
{
	static char const units[] = "KMG";
	uint64_t count;
	size_t const d = util_digits(str, strlen(str), UINT64_MAX, &count);
	if (!d)
		return EINVAL;
	unsigned shift = 0;
	if (str[d]) {
		char const *unit = n->take & TAKE_BYTES ? strchr(units, str[d]) : nullptr;
		if (!unit || str[d + 1])
			return EINVAL;
		shift = 10 * (unsigned)(unit - units + 1);
	}
	if (count > n->max >> shift || (!count && n->take & TAKE_POSITIVE))
		return EINVAL;
	*value = count << shift;
	return 0;
}

/* Apply the option of the row @p row of rows[] with @p arg, given in the
 * configuration file @p where, or on the command line if that is nullptr;
 * returns 0, or 2, having said why if the option has a value.
 */
static int
config_apply_store (struct config *cfg,
                    size_t         row,
                    char const    *arg,
                    char const    *where)
{
	if (row >= array_size(rows) || rows[row].take & TAKE_CUSTOM)
		return 2;
	struct config_option const *n = &rows[row];
	unsigned char *member = (unsigned char *)cfg + n->at;
	if (n->take & TAKE_TEXT) {
		memcpy(member, &arg, sizeof arg);
		return 0;
	}
	uint64_t value = 0;
	if (n->width && config_parse_count(arg, n, &value))
		return config_complain(cfg, where, "invalid --%s: %s", options[row].name, arg);
	/* Every member is an unsigned or a nonnegative count of 32 or 64 bits. */
	value *= n->scale;
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

/* Apply the option of the row @p row with @p arg, given in the
 * configuration file @p where, or on the command line if that is nullptr;
 * returns 0, or 2, having said why.
 */
static int
config_apply (struct config *cfg,
              size_t         row,
              char const    *arg,
              char const    *where)
{
	struct ai_endpoint *ep = &cfg->ep;
	switch (row) {
	case ROW_PROVIDER: {
		size_t i = 1 + cstr_view_index(provider_names + 1, array_size(provider_names) - 1,
		                               cstr_view(arg));
		if (i == array_size(provider_names))
			return config_complain(cfg, where, "unknown provider: %s", arg);
		ep->provider = (enum ai_provider)i;
		break;
	}
	case ROW_API: {
		size_t i = 1 + cstr_view_index(api_names + 1, array_size(api_names) - 1,
		                               cstr_view(arg));
		if (i == array_size(api_names))
			return config_complain(cfg, where, "unknown API: %s", arg);
		ep->api = (enum ai_api)i;
		break;
	}
	case ROW_URL:
		if (!util_url_scheme(cstr_view(arg)))
			return config_complain(cfg, where, "not an http(s) URL: %s", arg);
		ep->url = arg;
		break;
	case ROW_LISTEN:
		/* The command line's listeners replace the file's. */
		if (!where && cfg->flags & CFG_LISTEN_FILE)
			cfg->n_listens = 0;
		cfg->flags = (cfg->flags & ~CFG_LISTEN_FILE) | (where ? CFG_LISTEN_FILE : 0);
		config_push(cfg->listens, &cfg->n_listens, arg);
		break;
	case ROW_BACKGROUND: ep->background = 1; break;
	case ROW_PARTICIPANT:
		if (!util_word_len(arg, UTIL_WORD_MAX))
			return config_complain(cfg, where, "--participant: use 1-64 of "
			                       "[A-Za-z0-9_-]: %s", arg);
		cfg->participant = arg;
		break;
	case ROW_ALLOW_ORIGIN:
		config_push(cfg->origins, &cfg->n_origins, arg);
		break;
	case ROW_HEADER:
		if (!config_header_valid(arg))
			return config_complain(cfg, where, "invalid header line: %s", arg);
		config_push(cfg->headers, &cfg->n_headers, arg);
		break;
	case ROW_TEMPERATURE:
		return config_apply_temperature(cfg, arg, where);
	case ROW_EXTRA:
		return config_apply_extra(cfg, arg, where);
	case ROW_TOOL_NAME:
		if (!config_tool_name_valid(arg))
			return config_complain(cfg, where, "tool names are 1-128 of "
			                       "[A-Za-z0-9_.-]: %s", arg);
		cfg->tool.name = arg;
		break;
	case ROW_GIT:
		config_push(cfg->gits, &cfg->n_gits, arg);
		break;
	case ROW_CODE:
		config_push(cfg->codes, &cfg->n_codes, arg);
		break;
	default:
		return config_apply_store(cfg, row, arg, where);
	}
	return 0;
}

/* Say that @p val, a member of the configuration file or an element of one,
 * is no value of option @p o; returns 2.
 */
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
 * of those.
 */
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
 * argument config_value_arg() finds.
 */
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
		return config_apply(cfg, (size_t)(o - options), nullptr, cfg->path);
	}
	char const *arg = config_value_arg(o, val);
	if (!arg)
		return config_invalid(cfg, o, val);
	return config_apply(cfg, (size_t)(o - options), arg, cfg->path);
}

/* Apply @p val, a member of the configuration file, as option @p o: a value
 * config_apply_value() takes, or an array of them for a repeatable option.
 */
static int
config_apply_member (struct config       *cfg,
                     struct option const *o,
                     struct json_object  *val)
{
	if (!(rows[o - options].arg & ARG_LIST) || !json_object_is_type(val, json_type_array))
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
 * them (the sticky bit, as on /tmp).
 */
static bool
config_safe (struct stat const *st)
{
	return (!st->st_uid || st->st_uid == geteuid()) &&
	       (!(st->st_mode & (S_IWGRP | S_IWOTH)) ||
	        (st->st_mode & (S_IFMT | S_ISVTX)) == (S_IFDIR | S_ISVTX));
}

/* Whether every directory above @p real, a path that realpath() made, is
 * config_safe(); false if that cannot be told. The function cuts @p real
 * short as it goes up.
 */
static bool
config_safe_dirs (char *real)
{
	bool ok = true;
	struct stat st;
	/* Each but the root, from the nearest; the root last. */
	for (char *slash = strrchr(real, '/'); ok && slash != real;
	     slash = strrchr(real, '/')) {
		*slash = '\0';
		ok = !stat(real, &st) && config_safe(&st);
	}
	return ok && !stat("/", &st) && config_safe(&st);
}

/* Whether the file at @p path, whose status is @p st, and every directory
 * above it are config_safe(); false if that cannot be told.
 */
static bool
config_safe_path (char const        *path,
                  struct stat const *st)
{
	char *real = config_safe(st) ? realpath(path, nullptr) : nullptr;
	bool const ok = real && config_safe_dirs(real);
	free(real);
	return ok;
}

/* The reason to refuse a file that config_safe_path() does not accept. */
static char const config_unsafe[] = "others than root and askai's user may change it";

/* As config_slurp() says, the file at @p path being open as @p fd. */
static int
config_slurp_fd (struct config const *cfg,
                 int                  fd,
                 char const          *path,
                 mode_t               hide,
                 struct buf          *text)
{
	struct stat st;
	if (fstat(fd, &st))
		return config_complain(cfg, nullptr, "cannot open %s: %s", path,
		                       strerror(errno));
	if (!S_ISREG(st.st_mode))
		return config_complain(cfg, path, "not a regular file");
	if (!config_safe_path(path, &st))
		return config_complain(cfg, path, "%s", config_unsafe);
	if (st.st_mode & hide)
		return config_complain(cfg, path, "others may read it");
	/* The storage of the size that fstat() gave never moves, so no copy
	 * of a secret is left in storage given up, and a file that grows
	 * meanwhile is refused. */
	size_t const size = (size_t)st.st_size;
	int e = buf_read(text, fd, size, size < FILE_MAX ? size : FILE_MAX);
	if (!e)
		return 0;
	buf_wipe(text);
	buf_fini(text);
	static char const *const too_long[] = { "changed while askai read it",
	                                        "larger than 1 MiB" };
	return e == ENOBUFS ? config_complain(cfg, path, "%s", too_long[size > FILE_MAX])
	     : e == ENOMEM ? config_oom(cfg) : config_complain(cfg, path, "%s", strerror(e));
}

/* Read the file at @p path whole into @p text, an empty buffer: a regular
 * file of FILE_MAX bytes at most that only root and askai's user may
 * change, nor replace, and that has none of the permission bits @p hide,
 * such as S_IROTH for a secret. On an error, the bytes read are wiped and
 * the storage of @p text given up. Returns 0, or the exit status, having
 * said why.
 */
static int
config_slurp (struct config const *cfg,
              char const          *path,
              mode_t               hide,
              struct buf          *text)
{
	/* Not blocking, a FIFO opens at once, and fails the check. */
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return config_complain(cfg, nullptr, "cannot open %s: %s", path,
		                       strerror(errno));
	int status = config_slurp_fd(cfg, fd, path, hide, text);
	util_close(&fd);
	return status;
}

/* A view of the bytes that config_slurp() read into @p text, whose data is
 * not nullptr even if there are none: BIO_new_mem_buf() refuses nullptr.
 */
static struct cstr_view
config_text (struct buf const *text)
{
	struct span const s = buf_peek(text);
	return (struct cstr_view){ s.len ? (char const *)s.ptr : "", s.len };
}

/* Say that the configuration file's @p text is not as config_json() says,
 * @p what being wrong at byte @p end; returns 2.
 */
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
 * cfg->file. The file must be one JSON object in strict JSON, so that tools
 * such as jq can read it. With JSON_TOKENER_STRICT, json-c reports comments
 * and trailing commas as errors. Only blanks may follow the object. Returns
 * 0, or the exit status after a message about the error.
 */
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
	while (end < len && cstr_view_space(text[end]))
		++end;
	if (end < len)
		return config_json_fail(cfg, text, end, "not one JSON object");
	return 0;
}

/* The value of option @p o, which the file can set, in @p cfg's
 * configuration file, if it has one; nullptr if none.
 */
static struct json_object *
config_file_value (struct config const *cfg,
                   struct option const *o)
{
	enum config_group const g = rows[o - options].group;
	struct json_object *group = g == GROUP_ROOT ? cfg->file
	                           : json_object_object_get(cfg->file, group_names[g].data);
	return json_object_object_get(group, o->name);
}

/* Whether @p cfg's configuration file and @p from's, if either has one, set
 * the same plugins, tools and options, but the options that take effect on
 * a reload.
 */
static bool
config_same_options (struct config const *cfg,
                     struct config const *from)
{
	/* The options of the group TOOLS are in the member tools, which is
	 * compared whole. */
	for (size_t i = 0; i < array_size(rows); ++i)
		if (rows[i].group && rows[i].group != GROUP_TOOLS &&
		    !(rows[i].arg & ARG_RELOAD) &&
		    !json_object_equal(config_file_value(cfg, &options[i]),
		                       config_file_value(from, &options[i])))
			return false;
	return json_object_equal(json_object_object_get(cfg->file, "plugins"),
	                         json_object_object_get(from->file, "plugins")) &&
	       json_object_equal(json_object_object_get(cfg->file, "tools"),
	                         json_object_object_get(from->file, "tools"));
}

/* The values of the members of @p obj, if it is an object: the elements of
 * an array, else 1 for each member.
 */
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

/* Make room for the lists of headers, origins, listeners, repositories and
 * compile commands, in one block, and point ep.headers, http.origins and
 * http.listen at their lists: each has room for every argument, every
 * value of an option in the configuration file, in its four objects of
 * options and in the objects of the tools in its member tools, and the
 * three headers askai adds, and ends in nullptr. Returns 0, or 1 if out of
 * memory, having said so.
 */
static int
config_lists (struct config *cfg)
{
	size_t room = (size_t)cfg->argc + 4;
	for (size_t g = 1; g < array_size(group_names); ++g)
		room += config_values(json_object_object_get(cfg->file, group_names[g].data));
	struct json_object *tools = json_object_object_get(cfg->file, "tools");
	struct json_object_iter it;
	if (json_object_is_type(tools, json_type_object))
		json_object_object_foreachC(tools, it)
			room += config_values(it.val);
	char const **block = malloc(6 * room * sizeof *block);
	if (!block)
		return config_oom(cfg);
	cfg->headers = block;
	cfg->origins = block + room;
	cfg->listens = block + 2 * room;
	cfg->gits = block + 3 * room;
	cfg->codes = block + 4 * room;
	cfg->code_repos = block + 5 * room;
	*cfg->headers = *cfg->origins = *cfg->listens = *cfg->gits = *cfg->codes = nullptr;
	cfg->ep.headers = cfg->headers;
	cfg->http.origins = cfg->origins;
	cfg->http.listen = cfg->listens;
	return 0;
}

/* The option named @p name that the configuration file can set; nullptr
 * if there is none.
 */
static struct option const *
config_named (char const *name)
{
	for (struct option const *o = options; o->name; ++o)
		if (rows[o - options].group && !strcmp(o->name, name))
			return o;
	return nullptr;
}

/* Say that the configuration file has option @p o outside the object that
 * contains it; returns 2.
 */
static int
config_misplaced (struct config const *cfg,
                  struct option const *o)
{
	enum config_group const g = rows[o - options].group;
	switch (g) {
	case GROUP_ROOT:
		return config_complain(cfg, cfg->path, "%s must be at the top level", o->name);
	case GROUP_TOOLS:
		return config_complain(cfg, cfg->path, "%s must be tools.%s", o->name,
		                       rows[o - options].member);
	default:
		return config_complain(cfg, cfg->path, "%s must be in %s", o->name,
		                       group_names[g].data);
	}
}

/* The option whose member of the configuration file's member tools is
 * @p key in the object of the built-in tool @p tool, or for a nullptr
 * @p key the first option of the tool; nullptr if there is none, and so
 * for a @p tool that is no built-in tool.
 */
static struct option const *
config_tool_option (char const *tool,
                    char const *key)
{
	size_t const len = strlen(tool);
	for (size_t i = 0; i < array_size(rows); ++i) {
		char const *m = rows[i].member;
		if (m && !strncmp(m, tool, len) && m[len] == '.' &&
		    (!key || !strcmp(m + len + 1, key)))
			return &options[i];
	}
	return nullptr;
}

/* Apply the members of @p obj, the object of the built-in tool @p tool in
 * the configuration file's member tools, as the options of the group TOOLS
 * that they are.
 */
static int
config_apply_tool (struct config      *cfg,
                   char const         *tool,
                   struct json_object *obj)
{
	if (!json_object_is_type(obj, json_type_object))
		return config_complain(cfg, cfg->path, "tools.%s must be an object", tool);
	struct json_object_iter it;
	json_object_object_foreachC(obj, it) {
		struct option const *o = config_tool_option(tool, it.key);
		int status = o ? config_apply_member(cfg, o, it.val)
		               : config_complain(cfg, cfg->path, "tools.%s: unknown member %s",
		                                 tool, it.key);
		if (status)
			return status;
	}
	return 0;
}

/* Apply the objects of the built-in tools in the configuration file's
 * member tools, if it is an object; config_allow() reads the rest of it.
 */
static int
config_apply_tools (struct config *cfg)
{
	struct json_object *tools = json_object_object_get(cfg->file, "tools");
	if (!json_object_is_type(tools, json_type_object))
		return 0;
	struct json_object_iter it;
	json_object_object_foreachC(tools, it) {
		int status = config_tool_option(it.key, nullptr)
		           ? config_apply_tool(cfg, it.key, it.val) : 0;
		if (status)
			return status;
	}
	return 0;
}

/* Apply the members of @p obj, the configuration file's object of group
 * @p g, as the options of their names.
 */
static int
config_apply_group (struct config      *cfg,
                    enum config_group   g,
                    struct json_object *obj)
{
	if (!json_object_is_type(obj, json_type_object))
		return config_complain(cfg, cfg->path, "%s must be an object",
		                       group_names[g].data);
	struct json_object_iter it;
	json_object_object_foreachC(obj, it) {
		struct option const *o = config_named(it.key);
		if (!o)
			return config_complain(cfg, cfg->path, "%s: unknown member %s",
			                       group_names[g].data, it.key);
		int status = rows[o - options].group != g ? config_misplaced(cfg, o)
		                                          : config_apply_member(cfg, o, it.val);
		if (status)
			return status;
	}
	return 0;
}

/* Apply the options in the configuration file's objects upstream, service,
 * testing and debugging. The policy's members, "plugins" and "tools" are
 * not options: policy_create(), config_registry(), config_apply_tools()
 * and config_allow() read them.
 */
static int
config_apply_file (struct config *cfg)
{
	struct json_object_iter it;
	json_object_object_foreachC(cfg->file, it) {
		if (policy_member(it.key) || !strcmp(it.key, "plugins") ||
		    !strcmp(it.key, "tools"))
			continue;
		size_t g = 1 + cstr_view_index(group_names + 1, array_size(group_names) - 1,
		                               cstr_view(it.key));
		struct option const *o = config_named(it.key);
		int status = g < array_size(group_names)
		             ? config_apply_group(cfg, (enum config_group)g, it.val)
		           : o && rows[o - options].group == GROUP_ROOT
		             ? config_apply_member(cfg, o, it.val)
		           : o ? config_misplaced(cfg, o)
		               : config_complain(cfg, cfg->path, "unknown member %s", it.key);
		if (status)
			return status;
	}
	return 0;
}

/* Read the configuration file, which config_slurp() reads, as config_json()
 * says, make room for the lists, and apply the file's options.
 */
static int
config_file (struct config *cfg)
{
	struct buf text = {};
	int status = config_slurp(cfg, cfg->path, 0, &text);
	if (status)
		return status;
	struct cstr_view const json = config_text(&text);
	status = config_json(cfg, json.data, json.len);
	buf_fini(&text);
	status = status ? status : config_lists(cfg);
	status = status ? status : config_apply_file(cfg);
	return status ? status : config_apply_tools(cfg);
}

/* Check the limits on clients against each other. A body limit that
 * --max-body did not give is lowered to the budgets for the bodies, so that
 * a configuration that lowers a budget does not stop askai.
 */
static int
config_fit_limits (struct config *cfg)
{
	struct httpd_limits *lim = &cfg->http.limits;
	size_t const budgets[] = {
		lim->bodies, lim->client_bodies ? lim->client_bodies : SIZE_MAX
	};
	static char const *const names[] = { "--body-budget", "--client-budget" };
	for (size_t i = 0; i < array_size(budgets); ++i) {
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
 * finds no repository around it.
 */
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

/* Check each repository of --git. git_name() must return a name for it
 * that is not empty and differs from the names of the repositories before
 * it, and config_git_repo() must return true for its path.
 */
static int
config_check_gits (struct config *cfg)
{
	for (size_t i = 0; i < cfg->n_gits; ++i) {
		char const *path = cfg->gits[i];
		struct cstr_view const name = git_name(path);
		if (!name.len)
			return config_complain(cfg, nullptr, "--git: %s names no repository: "
			                       "its last component, less a .git suffix, must "
			                       "be 1 to 64 of [A-Za-z0-9_.-], not beginning "
			                       "with a dot", path);
		if (!config_git_repo(path))
			return config_complain(cfg, nullptr, "--git: not a git repository: %s",
			                       path);
		size_t j = 0;
		while (j < i && !cstr_view_equal(git_name(cfg->gits[j]), name))
			++j;
		if (j < i)
			return config_complain(cfg, nullptr, "--git: two repositories "
			                       "named %.*s", (int)name.len, name.data);
	}
	return 0;
}

/* Check that each value of --code is REPO=FILE with one '=', a nonempty
 * FILE, and as REPO the name of a --git repository or, if it begins with
 * '/', the path of one as --git gives it. No two values may be for the
 * same repository. Then point codes[i] at the FILE part of each value,
 * store the path of its repository in code_repos[i], and run askai-code
 * --check (code_check()). If the check fails, the code tool is off and
 * cfg->code_check contains the reason.
 */
static int
config_check_codes (struct config *cfg)
{
	for (size_t i = 0; i < cfg->n_codes; ++i) {
		char const *code = cfg->codes[i];
		char const *file = strchr(code, '=');
		struct cstr_view const repo = { code, file ? (size_t)(file - code) : 0 };
		bool const path = *code == '/';
		size_t g = 0;
		while (g < cfg->n_gits &&
		       !cstr_view_equal(path ? cstr_view(cfg->gits[g]) : git_name(cfg->gits[g]), repo))
			++g;
		if (g == cfg->n_gits || !file[1] || strchr(file + 1, '='))
			return config_complain(cfg, nullptr, "--code: REPO=FILE needs the name or "
			                       "the path of a --git repository, one '=' and a "
			                       "file: %s", code);
		size_t j = 0;
		while (j < i && cfg->code_repos[j] != cfg->gits[g])
			++j;
		if (j < i) {
			struct cstr_view const name = git_name(cfg->gits[g]);
			return config_complain(cfg, nullptr, "--code: two compile commands for "
			                       "%.*s", (int)name.len, name.data);
		}
		cfg->code_repos[i] = cfg->gits[g];
		cfg->codes[i] = file + 1;
	}
	return cfg->n_codes && code_check(cfg) ? config_oom(cfg) : 0;
}

/* Settle the upstream's API, unless --api chose it: its provider's default.
 * A provider must serve it; with none, --url must say where it is.
 */
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
	                       provider_names[ep->provider].data, api_names[ep->api].data);
}

/* Check that the upstream may call tools, if it is to. */
static int
config_check_upstream (struct config *cfg)
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
 * notes, which does not exist yet, in the directory above it.
 */
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
	else if (!S_ISDIR(st.st_mode) || !config_safe_path(dir, &st))
		status = config_complain(cfg, nullptr, "--notes: %s: others than root "
		                         "and askai's user may change the directory it "
		                         "would be made in", cfg->notes);
	free(copy);
	return status;
}

/* Checks the directory of the notes, if --notes names one. Only root and
 * askai's user may be able to change it, or to make it if it does not
 * exist.
 */
static int
config_check_notes (struct config *cfg)
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
	if (!config_safe_path(path, &st))
		return config_complain(cfg, nullptr, "--notes: %s: %s", path, config_unsafe);
	return 0;
}

/* A repository is the session; a participant keeps the same writer identity
 * across restarts. Resolve the transcript location once, before any service
 * opens it. The journal validates and creates its parent directories.
 */
static int
config_session (struct config *cfg)
{
	if (!cfg->participant)
		cfg->participant = cfg->tool.name;
	if (!util_word_len(cfg->participant, UTIL_WORD_MAX) &&
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
		if (config_aprintf(&cfg->transcripts_owned, "%.*s.transcripts.git", (int)n,
		                   cfg->notes))
			return config_oom(cfg);
	} else if (cfg->ep.background) {
		char const *base;
		char const *rest;
		if (util_state_path(&base, &rest))
			return config_complain(cfg, nullptr, "--background needs --transcripts, "
			                       "an absolute XDG_STATE_HOME or an absolute HOME");
		if (config_aprintf(&cfg->transcripts_owned, "%s%s/%s/transcripts.git", base,
		                   rest, cfg->participant))
			return config_oom(cfg);
	} else {
		return 0;
	}
	cfg->transcripts = cfg->transcripts_owned;
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
	*p_key = key;
	return 0;
}

/*
 * The API key, in @p *p_key, unless a header gives one, as @p has_auth says:
 * from the first of the usual variables that is set, the provider's at its
 * own URL, which llama-server may do without, else ASKAI_API_KEY, which a
 * URL that --url gives may do without. A provider's variables are only read
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
	for (size_t i = 0; i < array_size(providers[0].keys) && vars[i]; ++i) {
		char const *key = getenv(vars[i]);
		if (key && *key) {
			*p_key = key;
			return 0;
		}
	}
	if (ai_llama_like(p))
		return 0;
	return config_complain(cfg, nullptr, "set %s or pass --key-env", vars[0]);
}

/* Point the upstream at the provider's own URL for its API: for Gemini's
 * generateContent, at the model's method that streams, in SSE.
 */
static int
config_url_own (struct config *cfg)
{
	struct ai_endpoint *ep = &cfg->ep;
	if (!ep->model && !ai_llama_like(ep->provider))
		return config_complain(cfg, nullptr, "--model is required for %s's "
		                       "own API", provider_names[ep->provider].data);
	ep->url = providers[ep->provider].url[ep->api];
	if (ep->api != AI_GENERATE)
		return 0;
	if (config_aprintf(&cfg->url, "%s%s:streamGenerateContent?alt=sse", ep->url,
	                   ep->model))
		return config_oom(cfg);
	ep->url = cfg->url;
	return 0;
}

/* Gemini streams from :streamGenerateContent, in SSE with alt=sse, whatever
 * alt the URL that --url gives had.
 */
static int
config_url_generate (struct config *cfg)
{
	if (cfg->ep.api != AI_GENERATE)
		return 0;
	static char const method[] = ":generateContent";
	size_t const m = sizeof method - 1;
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
	char const *alt = strstr(query, "?alt=");
	alt = alt ? alt : strstr(query, "&alt=");
	if (alt) {
		char const *value = alt + sizeof "?alt=" - 1;
		if (config_aprintf(&cfg->url, "%.*s%s%.*salt=sse%s", (int)path, url, stream,
		                   (int)(alt + 1 - query), query, value + strcspn(value, "&")))
			return config_oom(cfg);
	} else if (config_aprintf(&cfg->url, "%.*s%s%s%salt=sse", (int)path, url, stream,
	                          query, *query ? "&" : "?")) {
		return config_oom(cfg);
	}
	cfg->ep.url = cfg->url;
	return 0;
}

/* Add the headers that askai sends unless they are given: the API key's
 * with @p key, unless that is nullptr, anthropic-version for Messages, and
 * User-Agent.
 */
static int
config_headers (struct config *cfg,
                char const    *key)
{
	if (key) {
		if (config_aprintf(&cfg->auth, "%s%s", auths[cfg->ep.api], key))
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
	if (key && strpbrk(key, "\r\n"))
		return config_complain(cfg, nullptr, "the API key contains a line break");
	status = cfg->ep.url ? config_url_generate(cfg) : config_url_own(cfg);
	return status ? status : config_headers(cfg, has_auth ? nullptr : key);
}

/* Make the TLS context from the certificate chain @p chain and from the
 * key in the file of --tls-key, which only its owner and its group may
 * read. The key is wiped once read.
 */
static int
config_tls_key (struct config    *cfg,
                struct cstr_view  chain)
{
	struct buf key = {};
	int status = config_slurp(cfg, cfg->tls_key, S_IROTH, &key);
	if (status)
		return status;
	struct cstr_view const pem = config_text(&key);
	char why[256];
	int e;
	cfg->http.tls = tls_ctx_create(cfg, chain.data, chain.len, pem.data, pem.len, why,
	                               sizeof why, &e);
	buf_wipe(&key);
	buf_fini(&key);
	if (e == ENOMEM)
		return config_oom(cfg);
	if (e)
		return config_complain(cfg, nullptr, "TLS: %s", why);
	return 0;
}

/* Make the TLS listeners' context from the certificate chain's file and its
 * key's, the chain wiped once read too, as one file may hold the two.
 */
static int
config_tls (struct config *cfg)
{
	if (!cfg->tls_cert || !cfg->tls_key)
		return config_complain(cfg, nullptr, "a tls: listener needs --tls-cert "
		                       "and --tls-key");
	struct buf chain = {};
	int status = config_slurp(cfg, cfg->tls_cert, 0, &chain);
	if (status)
		return status;
	status = config_tls_key(cfg, config_text(&chain));
	buf_wipe(&chain);
	buf_fini(&chain);
	return status;
}

/* Add the listen address 127.0.0.1:8090 if cfg->listens is empty. Refuse a
 * listen address that httpd_listen_addr() cannot parse. If an address begins with
 * "tls:", make the TLS context.
 */
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
			return config_complain(cfg, cfg->flags & CFG_LISTEN_FILE ? cfg->path : nullptr,
			                       "invalid --listen: %s: %s", cfg->listens[i],
			                       strerror(e));
		tls |= a.tls;
	}
	return tls ? config_tls(cfg) : 0;
}

/* Note that ASKAI_TOKEN goes unused if the configuration file names who may
 * connect, but on a reload.
 */
static void
config_note_token (struct config const *cfg)
{
	char const *token = getenv("ASKAI_TOKEN");
	if (cfg->flags & CFG_LOGGED || !token || !*token || !cfg->file)
		return;
	struct json_object_iter it;
	json_object_object_foreachC(cfg->file, it)
		if (policy_member(it.key)) {
			(void)config_complain(cfg, nullptr, "note: ASKAI_TOKEN is ignored, "
			                      "as %s names who may connect", cfg->path);
			return;
		}
}

/* Checks the plugin file at @p path. Only root and askai's user may change
 * the file and the directories above it. The path must be the file's real
 * path, so it has no symbolic link. Then nobody else can replace the file
 * between this check and the dlopen() in plugin.c.
 */
static int
config_check_plugin (struct config const *cfg,
                     char const          *path)
{
	char *real = realpath(path, nullptr);
	struct stat st;
	int status = !real || stat(real, &st)
	           ? config_complain(cfg, path, "cannot open it: %s", strerror(errno))
	           : strcmp(real, path)
	           ? config_complain(cfg, path, "not the file's real path, which is %s", real)
	           : !config_safe(&st) || !config_safe_dirs(real)
	           ? config_complain(cfg, path, "%s", config_unsafe)
	           : 0;
	free(real);
	return status;
}

/* Loads the plugin of @p v, an element of the configuration file's
 * "plugins", and adds its tools to the registry. The element is the
 * absolute path of the plugin, or an object. The object's member "path" is
 * that path, and its member "options", if it has one, is an object of the
 * plugin's options. The object has no other member.
 */
static int
config_plugin (struct config      *cfg,
               struct json_object *v)
{
	struct json_object *file = v;
	struct json_object *options = nullptr;
	if (json_object_is_type(v, json_type_object)) {
		static struct cstr_view const names[] = {
			CSTR_VIEW_INIT("path"), CSTR_VIEW_INIT("options")
		};
		struct json_object *of[array_size(names)] = {};
		uint64_t seen;
		struct cstr_view other;
		if (jb_args(v, names, array_size(names), UINT64_MAX, of, &seen, &other))
			return config_complain(cfg, cfg->path, "plugins: unknown member of a "
			                       "plugin: %.*s", (int)other.len, other.data);
		if (!(seen & 1))
			return config_complain(cfg, cfg->path, "plugins: a plugin has no "
			                       "member path");
		if (seen & 2 && !json_object_is_type(of[1], json_type_object))
			return config_complain(cfg, cfg->path, "plugins: the member "
			                       "options of a plugin is not an object");
		file = of[0];
		options = of[1];
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
 * the order of the array. A reload adds only askai's own
 * tools, so that it refuses the same --tool-name as a start does. A reload
 * loads no plugin, and config_policy() reads the registry of cfg->running.
 */
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
 * on a reload, else @p cfg.
 */
static struct config const *
config_run (struct config const *cfg)
{
	return cfg->running ? cfg->running : cfg;
}

/* The bit of the tool named @p name in a tool mask of @p reg, or 0 if
 * there is no such tool.
 */
static tool_mask
config_tool_bit (struct registry const *reg,
                 struct cstr_view       name)
{
	uint32_t id = registry_find(reg, name, TOOL_MAX);
	return id < TOOL_MAX ? (tool_mask)1 << id : 0;
}

/* Say that @p v, in the member tools of the configuration file, is no
 * name of a tool of @p reg that it may name: all but the ask tool, the
 * first. Returns 2.
 */
static int
config_allow_none (struct config const   *cfg,
                   char const            *where,
                   struct registry const *reg,
                   struct json_object    *v)
{
	return config_complain(cfg, cfg->path, "%s: askai has no tool %s; the tools "
	                       "are %s", where, json_object_to_json_string(v),
	                       registry_names(reg, ~(tool_mask)1).data);
}

/* The mask, in @p *mask, of the tools of @p reg that @p list names, an
 * array of names, the ask tool aside.
 */
static int
config_allow_names (struct config const   *cfg,
                    char const            *where,
                    struct registry const *reg,
                    struct json_object    *list,
                    tool_mask             *mask)
{
	if (!json_object_is_type(list, json_type_array))
		return config_complain(cfg, cfg->path, "%s: not a list of tools", where);
	*mask = 0;
	size_t len = json_object_array_length(list);
	for (size_t i = 0; i < len; ++i) {
		struct json_object *v = json_object_array_get_idx(list, i);
		uint32_t id = registry_find(reg, jb_view(v), reg->n_tools);
		if (id == reg->n_tools)
			return config_allow_none(cfg, where, reg, v);
		if (!id)
			return config_complain(cfg, cfg->path, "%s: %s is the ask tool, which "
			                       "is always on", where, reg->tools[0].name.data);
		*mask |= (tool_mask)1 << id;
	}
	return 0;
}

/* The mask, in @p *mask, of the tools that @p spec allows: all for true,
 * none for false, those of a list, those of the member allow, or all but
 * those of the member deny. The ask tool is not counted. With @p top,
 * @p spec is the member tools itself, whose objects of the built-in tools
 * are skipped (config_apply_tools()).
 */
static int
config_allow_form (struct config const   *cfg,
                   char const            *where,
                   struct registry const *reg,
                   struct json_object    *spec,
                   bool                   top,
                   tool_mask             *mask)
{
	if (json_object_is_type(spec, json_type_boolean)) {
		*mask = json_object_get_boolean(spec) ? ~(tool_mask)0 : 0;
		return 0;
	}
	if (json_object_is_type(spec, json_type_array))
		return config_allow_names(cfg, where, reg, spec, mask);
	if (!json_object_is_type(spec, json_type_object))
		return config_complain(cfg, cfg->path, "%s: not true, false, a list or "
		                       "an object", where);
	struct json_object_iter it;
	json_object_object_foreachC(spec, it)
		if (strcmp(it.key, "allow") && strcmp(it.key, "deny") &&
		    !(top && config_tool_option(it.key, nullptr)))
			return config_complain(cfg, cfg->path, "%s: unknown member %s",
			                       where, it.key);
	struct json_object *allow;
	struct json_object *deny;
	bool allows = json_object_object_get_ex(spec, "allow", &allow);
	if (!json_object_object_get_ex(spec, "deny", &deny)) {
		if (allows)
			return config_allow_names(cfg, where, reg, allow, mask);
		*mask = ~(tool_mask)0;
		return 0;
	}
	if (allows)
		return config_complain(cfg, cfg->path, "%s: allow and deny together",
		                       where);
	int status = config_allow_names(cfg, where, reg, deny, mask);
	*mask = ~*mask;
	return status;
}

/* As config_allow_form(), and the mask must not have background without
 * jobs, with which the output of a job is read.
 */
static int
config_allow_spec (struct config const   *cfg,
                   char const            *where,
                   struct registry const *reg,
                   struct json_object    *spec,
                   bool                   top,
                   tool_mask             *mask)
{
	int status = config_allow_form(cfg, where, reg, spec, top, mask);
	tool_mask background = config_tool_bit(reg, CSTR_VIEW("background"));
	tool_mask jobs = config_tool_bit(reg, CSTR_VIEW("jobs"));
	if (!status && *mask & background && !(*mask & jobs))
		status = config_complain(cfg, cfg->path, "%s: background needs jobs", where);
	return status;
}

/* Check that the member tools @p v, an object with the member client or
 * upstream, has no other member but the objects of the built-in tools.
 */
static int
config_allow_parts (struct config const *cfg,
                    struct json_object  *v)
{
	struct json_object_iter it;
	json_object_object_foreachC(v, it) {
		if (!strcmp(it.key, "client") || !strcmp(it.key, "upstream") ||
		    config_tool_option(it.key, nullptr))
			continue;
		if (!strcmp(it.key, "allow") || !strcmp(it.key, "deny"))
			return config_complain(cfg, cfg->path, "tools: allow and deny go "
			                       "into client and upstream");
		return config_complain(cfg, cfg->path, "tools: unknown member %s", it.key);
	}
	return 0;
}

/* The tools that the member tools of the configuration file allows clients
 * and the upstream, of the tools of config_run(). Without the
 * member, it allows all. It is one form of config_allow_spec() for both,
 * or an object with the members client and upstream, either of which may be
 * missing, each one of those forms for its own.
 */
static int
config_allow (struct config *cfg)
{
	struct json_object *v;
	if (!json_object_object_get_ex(cfg->file, "tools", &v))
		return 0;
	struct registry const *reg = &config_run(cfg)->registry;
	struct json_object *client;
	struct json_object *upstream;
	bool const clients = json_object_object_get_ex(v, "client", &client);
	bool const lends = json_object_object_get_ex(v, "upstream", &upstream);
	int status = clients || lends
	           ? config_allow_parts(cfg, v)
	           : config_allow_spec(cfg, "tools", reg, v, true, &cfg->tools_clients);
	/* With one form, the upstream gets the clients' tools. With the member
	 * client or upstream, it gets all tools unless the member upstream is
	 * given, because the clients' tools are still all tools here. */
	cfg->tools_upstream = cfg->tools_clients;
	if (!status && clients)
		status = config_allow_spec(cfg, "tools: client", reg, client, false,
		                           &cfg->tools_clients);
	if (!status && lends)
		status = config_allow_spec(cfg, "tools: upstream", reg, upstream, false,
		                           &cfg->tools_upstream);
	/* mcp_register() adds the ask tool first, with the id 0. */
	cfg->tools_clients |= 1;
	return status;
}

/* The ask tool's description, unless one is given: with the sentences
 * about tools and notes if the turns of a role that grants every tool get
 * any tool and the tool notes.
 */
static int
config_description (struct config *cfg)
{
	if (cfg->tool.description)
		return 0;
	struct config const *run = config_run(cfg);
	tool_mask lent = mcp_lent(run, ~(tool_mask)0, ~(tool_mask)0);
	tool_mask notes = config_tool_bit(&run->registry, CSTR_VIEW("notes"));
	char const *who = "another AI model";
	char const *model = "";
	if (cfg->ep.model) {
		who = "the AI model ";
		model = cfg->ep.model;
	}
	if (config_aprintf(&cfg->description, "Ask %s%s a question or give it a task, and "
	                   "get its answer. It cannot see this conversation, your files or "
	                   "your tools, so write a complete, self-contained prompt.%s%s Use "
	                   "it for a second opinion, for hard problems, or for knowledge "
	                   "you lack.", who, model,
	                   lent ? " It may use some of this server's tools itself, and its "
	                          "answer lists the calls it made." : "",
	                   lent & notes
	                   ? " It keeps notes in the shared session repository. Each call "
	                     "loads its notes and can read the other participants' notes."
	                   : ""))
		return config_oom(cfg);
	cfg->tool.description = cfg->description;
	return 0;
}

/* Make the policy of who may connect and what they may do. A role can
 * grant the first run->registry.n_granted tools of config_run(), and
 * clients receive only the tools that the member "tools" of the running
 * configuration allows. With --upstream-tools, askai can offer such a tool
 * to the upstream if TOOL_LENT is set in the flags of the tool's kind.
 */
static int
config_policy (struct config *cfg)
{
	struct config const *run = config_run(cfg);
	struct registry const *reg = &run->registry;
	tool_mask upstream = 0;
	if (run->flags & CFG_UPSTREAM_TOOLS)
		for (uint32_t id = 0; id < reg->n_granted; ++id)
			upstream |= reg->tools[id].kind->flags & TOOL_LENT ? (tool_mask)1 << id : 0;
	char why[1024];
	int e;
	cfg->http.policy = policy_create(cfg->file, reg, run->tools_clients, upstream, why,
	                                 sizeof why, &e);
	if (e == ENOMEM)
		return config_oom(cfg);
	/* EINVAL means that the configuration file is wrong. Another error
	 * comes from a file that the policy reads, such as /etc/subuid, or is
	 * EIO from HMAC-SHA-256. The message contains the name of that file
	 * or the cause, so it does not begin with the path of the
	 * configuration file. */
	if (e)
		return config_complain(cfg, e == EINVAL ? cfg->path : nullptr, "%s", why);
	config_note_token(cfg);
	return 0;
}

/* Check the options together, and work out what they leave to askai. */
static int
config_finish (struct config *cfg)
{
	static int (*const steps[]) (struct config *cfg) = {
		config_fit_limits, config_check_gits, config_check_codes, config_api,
		config_check_upstream, config_check_notes, config_session, config_upstream,
		config_listens, config_registry, config_allow, config_description, config_policy,
	};
	for (size_t i = 0; i < array_size(steps); ++i) {
		int status = steps[i](cfg);
		if (status)
			return status;
	}
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
	if (!(cfg->flags & CFG_LOGGED))
		config_usage(cfg, stderr);
	return 2;
}

/* Apply the command line's options, but --config, over the file's. */
static int
config_argv (struct config *cfg)
{
	opterr = !(cfg->flags & CFG_LOGGED);
	optind = 0;
	for (int opt; (opt = getopt_long(cfg->argc, cfg->argv, shorts, options,
	                                 nullptr)) >= 0; ) {
		if (opt == '?')
			return config_misuse(cfg);
		if (opt == 'c')
			continue;
		int status = config_apply(cfg, config_row(opt), optarg, nullptr);
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
		.git_timeout = 30 * NS
	};
}

/* Read the command line and the file it names into @p cfg, as config()
 * says.
 */
static int
config_parse (struct config *cfg)
{
	/* The configuration file goes first, so that the command line overrides
	 * it; help goes before either. Each pass begins getopt afresh. */
	opterr = 0;
	optind = 0;
	for (int opt; (opt = getopt_long(cfg->argc, cfg->argv, shorts, options,
	                                 nullptr)) >= 0; ) {
		if (opt == 'h' && !(cfg->flags & CFG_LOGGED)) {
			config_usage(cfg, stdout);
			return 0;
		}
		if (opt == 'c')
			cfg->path = optarg;
	}
	int status = cfg->path ? config_file(cfg) : config_lists(cfg);
	status = status ? status : config_argv(cfg);
	status = status ? status : config_finish(cfg);
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
	cfg->flags |= CFG_LOGGED;
	cfg->running = from;
	int status = config_parse(cfg);
	if (status < 0 && !config_same_options(cfg, from))
		cfg->flags |= CFG_RESTART;
	return status;
}

/* Log @p url as the item @p name, without its user, password and query,
 * which can have secrets.
 */
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
 * --notes and --upstream-tools.
 */
static void
config_log_files (struct config const *cfg)
{
	dstr_auto url = DSTR_INIT;
	if (!ai_upload(cfg->ep.api))
		LOG_ALL(LOG_INFO, "  files     off (the API takes none)");
	else if (ai_upload_url(&cfg->ep, &url))
		LOG_ALL(LOG_INFO, "  files     off (the URL has no /v1beta/)");
	else if (!cfg->notes || !(cfg->flags & CFG_UPSTREAM_TOOLS))
		LOG_ALL(LOG_INFO, "  files     off (--notes and --upstream-tools)");
	else
		config_log_url("files", dstr_get(&url));
}

void
config_log (struct config const *cfg)
{
	if (!cfg)
		return;
	struct ai_endpoint const *ep = &cfg->ep;
	LOG_ALL(LOG_INFO, "upstream:");
	LOG_ALL(LOG_INFO, "  provider  %s",
	        ep->provider ? provider_names[ep->provider].data : "none");
	LOG_ALL(LOG_INFO, "  API       %s", api_names[ep->api].data);
	LOG_ALL(LOG_INFO, "  model     %s", ep->model ? ep->model : "none");
	LOG_ALL(LOG_INFO, "  background %s", !ep->background ? "off"
	        : ai_background(ep) ? "on (upstream and local)" : "on (local)");
	/* Home asks /props of a server that may be llama-server, and Gemini's
	 * API about its model, but not when told the slots and lending the
	 * upstream no tools (home_create()). */
	bool const props = ai_props(ep);
	dstr_auto model = DSTR_INIT;
	bool const gemini = !props && !ai_model_url(ep, &model);
	bool asked = !ep->slots || cfg->flags & CFG_UPSTREAM_TOOLS;
	if (ep->context > 0)
		LOG_ALL(LOG_INFO, "  context   %ld tokens", ep->context);
	else if (props && asked)
		LOG_ALL(LOG_INFO, "  context   from llama-server");
	else if (gemini && asked)
		LOG_ALL(LOG_INFO, "  context   from the model");
	else
		LOG_ALL(LOG_INFO, "  context   unknown");
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
	struct config const *run = config_run(cfg);
	policy_log(cfg->http.policy, cfg->file, &run->registry,
	           run->flags & CFG_UPSTREAM_TOOLS ? mcp_lent : nullptr, run);
}

void
config_fini (struct config *cfg)
{
	json_object_put(cfg->ep.extra);
	json_object_put(cfg->ep.temperature);
	json_object_put(cfg->file);
	policy_put(cfg->http.policy);
	tls_ctx_put(cfg->http.tls);
	registry_fini(&cfg->registry);
	free(cfg->url);
	free(cfg->auth);
	free(cfg->description);
	free(cfg->code_helper);
	free(cfg->code_cache);
	free(cfg->code_check);
	free(cfg->transcripts_owned);
	free(cfg->headers);
	*cfg = (struct config){};
}
