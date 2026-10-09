/** @file askai: an MCP server whose one tool asks another AI. */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "ai.h"
#include "dstr.h"
#include "http.h"
#include "httpd.h"
#include "jb.h"
#include "loop.h"
#include "mcp.h"
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
	        "  -h, --help                show this help\n"
	        "\n"
	        "If ASKAI_TOKEN is set, clients must send 'Authorization: Bearer <it>'.\n",
	        argv0);
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

static int
on_signal (struct loop     *loop,
           struct loop_ref *ref,
           uint32_t         events)
{
	(void)events;
	struct signalfd_siginfo info;
	while (read(ref->fd, &info, sizeof info) < 0 && errno == EINTR) {}
	loop_exit(loop);
	return 0;
}

static int
serve (struct ai_endpoint const *ep,
       struct mcp_tool const    *tool,
       struct httpd_cfg const   *hcfg,
       bool                      verbose)
{
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigprocmask(SIG_BLOCK, &mask, nullptr);
	/* libcurl leaves SIGPIPE to us because CURLOPT_NOSIGNAL is set. */
	signal(SIGPIPE, SIG_IGN);

	struct loop loop;
	struct loop_ref sig_ref = { .fd = -1 };
	struct http *http = nullptr;
	struct mcp *mcp = nullptr;
	struct httpd *httpd = nullptr;
	int e = loop_init(&loop);
	int sfd = e ? -1 : signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (!e && sfd < 0)
		e = errno;
	if (!e)
		e = loop_watch(&loop, &sig_ref, sfd, EPOLLIN, on_signal, nullptr);
	if (!e)
		e = http_init(&http, &loop);
	if (!e)
		e = mcp_init(&mcp, http, ep, tool, verbose);
	if (e)
		fprintf(stderr, "askai: %s\n", strerror(e));
	else if (!(e = httpd_init(&httpd, &loop, mcp, hcfg)) &&
	         (e = loop_exec(&loop)))
		fprintf(stderr, "askai: %s\n", strerror(e));

	httpd_fini(&httpd);
	mcp_fini(&mcp);
	http_fini(&http);
	loop_unwatch(&loop, &sig_ref);
	if (sfd >= 0)
		close(sfd);
	loop_fini(&loop);
	return e;
}

int
main (int   argc,
      char *argv[])
{
	enum { OPT_TOOL_NAME = 0x100, OPT_TOOL_DESCRIPTION };
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
		{ "verbose",          no_argument,       nullptr, 'v' },
		{ "help",             no_argument,       nullptr, 'h' },
		{ nullptr, 0, nullptr, 0 }
	};

	struct ai_endpoint ep = {
		.timeout_ms = 600'000,
		.protocol = AI_OPENAI
	};
	struct mcp_tool tool = { .name = "ask_ai" };
	struct httpd_cfg hcfg = { .listen = "127.0.0.1:8090" };
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

	for (int opt; (opt = getopt_long(argc, argv, "p:u:m:k:H:s:t:n:x:T:l:o:vh",
	                                 options, nullptr)) >= 0; ) {
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
			                                 JSON_TOKENER_STRICT);
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
			                                 JSON_TOKENER_STRICT);
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
		case 'h':
			usage(stdout, argv[0]);
			status = 0;
			goto out;
		default:
			usage(stderr, argv[0]);
			goto out;
		}
	}
	if (optind != argc) {
		usage(stderr, argv[0]);
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
	status = serve(&ep, &tool, &hcfg, verbose) ? 1 : 0;
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
