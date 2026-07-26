/** @file Command-line option parsing and process entry point. */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

static void
usage (FILE       *out,
       char const *argv0)
{
	fprintf(out,
	        "Usage: %s [options]\n"
	        "  -a, --a-url URL          model A chat-completions URL\n"
	        "  -b, --b-url URL          model B chat-completions URL\n"
	        "  -A, --a-model NAME       model A name/alias\n"
	        "  -B, --b-model NAME       model B name/alias\n"
	        "  -s, --system TEXT        common system message\n"
	        "  -p, --prompt TEXT        run one turn and exit\n"
	        "  -t, --temperature N      sampling temperature\n"
	        "  -n, --max-tokens N       max output tokens per model\n"
	        "  -H, --history MODE       split, shared-a, or shared-b\n"
	        "  -d, --debug              print both reasoning traces\n"
	        "  -P, --no-template-check  skip llama.cpp template validation\n"
	        "  -h, --help               show this help\n"
	        "\nInteractive command: :quit\n",
	        argv0);
}

static int
parse_history_mode (char const            *str,
                    enum app_history_mode *mode)
{
	if (!strcmp(str, "split"))
		*mode = APP_HISTORY_SPLIT;
	else if (!strcmp(str, "shared-a"))
		*mode = APP_HISTORY_SHARED_A;
	else if (!strcmp(str, "shared-b"))
		*mode = APP_HISTORY_SHARED_B;
	else
		return EINVAL;
	return 0;
}

int
main (int   argc,
      char *argv[])
{
	struct app_cfg cfg = {
		.url = {
			"http://127.0.0.1:8080/v1/chat/completions",
			"http://127.0.0.1:8081/v1/chat/completions"
		},
		.temperature = -1,
		.max_tokens = -1,
		.history_mode = APP_HISTORY_SPLIT,
		.template_check = 1
	};

	static struct option const options[] = {
		{ "a-url",             required_argument, nullptr, 'a' },
		{ "b-url",             required_argument, nullptr, 'b' },
		{ "a-model",           required_argument, nullptr, 'A' },
		{ "b-model",           required_argument, nullptr, 'B' },
		{ "system",            required_argument, nullptr, 's' },
		{ "prompt",            required_argument, nullptr, 'p' },
		{ "temperature",       required_argument, nullptr, 't' },
		{ "max-tokens",        required_argument, nullptr, 'n' },
		{ "history",           required_argument, nullptr, 'H' },
		{ "debug",             no_argument,       nullptr, 'd' },
		{ "no-template-check", no_argument,       nullptr, 'P' },
		{ "help",              no_argument,       nullptr, 'h' },
		{ nullptr, 0, nullptr, 0 }
	};

	for (;;) {
		int opt = getopt_long(argc, argv, "a:b:A:B:s:p:t:n:H:dPh",
		                      options, nullptr);
		if (opt < 0)
			break;

		switch (opt) {
		case 'a': cfg.url[0] = optarg; break;
		case 'b': cfg.url[1] = optarg; break;
		case 'A': cfg.model[0] = optarg; break;
		case 'B': cfg.model[1] = optarg; break;
		case 's': cfg.system = optarg; break;
		case 'p': cfg.prompt = optarg; break;
		case 'd': cfg.debug = 1; break;
		case 'P': cfg.template_check = 0; break;
		case 'H':
			if (parse_history_mode(optarg, &cfg.history_mode)) {
				fprintf(stderr, "invalid history mode: %s\n", optarg);
				return 2;
			}
			break;
		case 't': {
			char *end;
			errno = 0;
			cfg.temperature = strtod(optarg, &end);
			if (errno || *end || cfg.temperature < 0) {
				fprintf(stderr, "invalid temperature: %s\n", optarg);
				return 2;
			}
			break;
		}
		case 'n': {
			char *end;
			errno = 0;
			cfg.max_tokens = strtol(optarg, &end, 10);
			if (errno || *end || cfg.max_tokens <= 0) {
				fprintf(stderr, "invalid max token count: %s\n", optarg);
				return 2;
			}
			break;
		}
		case 'h':
			usage(stdout, argv[0]);
			return 0;
		default:
			usage(stderr, argv[0]);
			return 2;
		}
	}

	if (optind != argc) {
		usage(stderr, argv[0]);
		return 2;
	}

	int e = app_run(&cfg);
	if (e) {
		fprintf(stderr, "%s: %s\n", argv[0], strerror(e));
		return 1;
	}
	return 0;
}
