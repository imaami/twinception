/** @file Command-line option parsing and process entry point. */
#include <errno.h>
#include <getopt.h>
#include <stdint.h>
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
	        "      --a-provider NAME    llama or deepinfra (default llama)\n"
	        "      --b-provider NAME    llama or deepinfra (default llama)\n"
	        "  -s, --system TEXT        common system message\n"
	        "  -p, --prompt TEXT        run one seeded conversation and exit\n"
	        "  -t, --temperature N      sampling temperature\n"
	        "  -n, --max-tokens N       output cap; final-answer cap in rapid mode\n"
	        "  -H, --history MODE       split, shared-a, or shared-b\n"
	        "  -q, --rapid N            causal thought-swap quantum, in tokens\n"
	        "  -R, --rapid-budget N     rapid reasoning budget/model (default 512)\n"
	        "  -C, --crosstalk N        autonomous answer cross-feed rounds (-1 forever)\n"
	        "  -d, --debug              print reasoning and rapid-swap quanta\n"
	        "  -P, --no-template-check  skip llama.cpp template validation\n"
	        "  -h, --help               show this help\n"
	        "\nInteractive commands: :stop, :quit\n",
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

static int
parse_provider (char const        *str,
                enum app_provider *provider)
{
	if (!strcmp(str, "llama"))
		*provider = APP_PROVIDER_LLAMA;
	else if (!strcmp(str, "deepinfra"))
		*provider = APP_PROVIDER_DEEPINFRA;
	else
		return EINVAL;
	return 0;
}

static int
parse_long (char const *str,
            long       *value,
            long        min)
{
	char *end;
	errno = 0;
	long n = strtol(str, &end, 10);
	if (errno || end == str || *end || n < min)
		return EINVAL;
	*value = n;
	return 0;
}

static int
parse_u32 (char const *str,
           uint32_t   *value)
{
	char *end;
	errno = 0;
	unsigned long n = strtoul(str, &end, 10);
	if (errno || *end || !n || n > UINT32_MAX)
		return EINVAL;
	*value = (uint32_t)n;
	return 0;
}

int
main (int   argc,
      char *argv[])
{
	struct app_cfg cfg = {
		.temperature = -1,
		.max_tokens = -1,
		.history_mode = APP_HISTORY_SPLIT,
		.template_check = 1
	};
	unsigned url_set = 0;
	enum { OPT_A_PROVIDER = 0x100, OPT_B_PROVIDER };

	static struct option const options[] = {
		{ "a-url",             required_argument, nullptr, 'a' },
		{ "b-url",             required_argument, nullptr, 'b' },
		{ "a-model",           required_argument, nullptr, 'A' },
		{ "b-model",           required_argument, nullptr, 'B' },
		{ "a-provider",        required_argument, nullptr, OPT_A_PROVIDER },
		{ "b-provider",        required_argument, nullptr, OPT_B_PROVIDER },
		{ "system",            required_argument, nullptr, 's' },
		{ "prompt",            required_argument, nullptr, 'p' },
		{ "temperature",       required_argument, nullptr, 't' },
		{ "max-tokens",        required_argument, nullptr, 'n' },
		{ "history",           required_argument, nullptr, 'H' },
		{ "rapid",             required_argument, nullptr, 'q' },
		{ "rapid-budget",      required_argument, nullptr, 'R' },
		{ "crosstalk",         required_argument, nullptr, 'C' },
		{ "debug",             no_argument,       nullptr, 'd' },
		{ "no-template-check", no_argument,       nullptr, 'P' },
		{ "help",              no_argument,       nullptr, 'h' },
		{ nullptr, 0, nullptr, 0 }
	};

	for (;;) {
		int opt = getopt_long(argc, argv, "a:b:A:B:s:p:t:n:H:q:R:C:dPh",
		                      options, nullptr);
		if (opt < 0)
			break;

		switch (opt) {
		case 'a': cfg.url[0] = optarg; url_set |= 1u; break;
		case 'b': cfg.url[1] = optarg; url_set |= 2u; break;
		case 'A': cfg.model[0] = optarg; break;
		case 'B': cfg.model[1] = optarg; break;
		case 's': cfg.system = optarg; break;
		case 'p': cfg.prompt = optarg; break;
		case 'd': cfg.debug = 1; break;
		case 'P': cfg.template_check = 0; break;
		case OPT_A_PROVIDER:
			if (parse_provider(optarg, &cfg.provider[0])) {
				fprintf(stderr, "invalid model A provider: %s\n", optarg);
				return 2;
			}
			break;
		case OPT_B_PROVIDER:
			if (parse_provider(optarg, &cfg.provider[1])) {
				fprintf(stderr, "invalid model B provider: %s\n", optarg);
				return 2;
			}
			break;
		case 'H':
			if (parse_history_mode(optarg, &cfg.history_mode)) {
				fprintf(stderr, "invalid history mode: %s\n", optarg);
				return 2;
			}
			break;
		case 'q':
			if (parse_u32(optarg, &cfg.rapid_quantum)) {
				fprintf(stderr, "invalid rapid quantum: %s\n", optarg);
				return 2;
			}
			break;
		case 'R':
			if (parse_u32(optarg, &cfg.rapid_budget)) {
				fprintf(stderr, "invalid rapid budget: %s\n", optarg);
				return 2;
			}
			break;
		case 'C':
			if (parse_long(optarg, &cfg.crosstalk_rounds, -1)) {
				fprintf(stderr, "invalid crosstalk round count: %s\n", optarg);
				return 2;
			}
			break;
		case 't': {
			char *end;
			errno = 0;
			cfg.temperature = strtod(optarg, &end);
			if (errno || end == optarg || *end || cfg.temperature < 0) {
				fprintf(stderr, "invalid temperature: %s\n", optarg);
				return 2;
			}
			break;
		}
		case 'n':
			if (parse_long(optarg, &cfg.max_tokens, 1)) {
				fprintf(stderr, "invalid max token count: %s\n", optarg);
				return 2;
			}
			break;
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

	if (cfg.rapid_budget && !cfg.rapid_quantum) {
		fputs("--rapid-budget requires --rapid\n", stderr);
		return 2;
	}
	if (cfg.rapid_quantum && !cfg.rapid_budget)
		cfg.rapid_budget = 512;

	static char const *llama_url[] = {
		"http://127.0.0.1:8080/v1/chat/completions",
		"http://127.0.0.1:8081/v1/chat/completions"
	};
	for (size_t i = 0; i < 2; ++i) {
		if (!(url_set & (1u << i)))
			cfg.url[i] = cfg.provider[i] == APP_PROVIDER_DEEPINFRA
				? "https://api.deepinfra.com/v1/openai/chat/completions"
				: llama_url[i];
		if (cfg.provider[i] == APP_PROVIDER_DEEPINFRA && !cfg.model[i])
			cfg.model[i] = "Qwen/Qwen3.6-35B-A3B";
		if (cfg.provider[i] == APP_PROVIDER_DEEPINFRA && cfg.rapid_quantum &&
		    strncmp(cfg.model[i], "Qwen/", sizeof "Qwen/" - 1) &&
		    strncmp(cfg.model[i], "qwen/", sizeof "qwen/" - 1)) {
			fprintf(stderr,
			        "DeepInfra model %c must currently be a Qwen reasoning model "
			        "in rapid mode; raw reasoning re-injection is not defined for %s\n",
			        (int)('A' + i), cfg.model[i]);
			return 2;
		}
	}
	if (cfg.provider[0] == APP_PROVIDER_DEEPINFRA ||
	    cfg.provider[1] == APP_PROVIDER_DEEPINFRA) {
		cfg.deepinfra_key = getenv("DEEPINFRA_API_KEY");
		if (!cfg.deepinfra_key || !*cfg.deepinfra_key)
			cfg.deepinfra_key = getenv("DEEPINFRA_TOKEN");
		if (!cfg.deepinfra_key || !*cfg.deepinfra_key) {
			fputs("DEEPINFRA_API_KEY or DEEPINFRA_TOKEN is required for "
			      "DeepInfra providers\n", stderr);
			return 2;
		}
	}

	int e = app_run(&cfg);
	if (e) {
		fprintf(stderr, "%s: %s\n", argv[0], strerror(e));
		return 1;
	}
	return 0;
}
