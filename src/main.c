/** @file Command-line option parsing and process entry point. */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "util.h"

/* The options without a letter. */
enum {
	MAIN_A_PROVIDER = 0x100,
	MAIN_B_PROVIDER
};

static void
main_usage (FILE       *out,
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

static char const *const main_history_modes[] = {
	[APP_HISTORY_SPLIT]    = "split",
	[APP_HISTORY_SHARED_A] = "shared-a",
	[APP_HISTORY_SHARED_B] = "shared-b"
};

static char const *const main_providers[] = {
	[APP_PROVIDER_LLAMA]     = "llama",
	[APP_PROVIDER_DEEPINFRA] = "deepinfra"
};

/* The index of @p str among the @p n @p names, @p n if it is none of them. */
static size_t
main_lookup (char const *const names[],
             size_t             n,
             char const        *str)
{
	size_t i = 0;
	while (i < n && strcmp(str, names[i]))
		++i;
	return i;
}

static int
main_long (char const *str,
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
main_u32 (char const *str,
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

static int
main_temperature (char const *str,
                  double     *value)
{
	char *end;
	errno = 0;
	double t = strtod(str, &end);
	if (errno || end == str || *end || t < 0)
		return EINVAL;
	*value = t;
	return 0;
}

/* @p e, an error of @p arg as @p what, said unless it is 0. */
static int
main_invalid (int         e,
              char const *what,
              char const *arg)
{
	if (e)
		fprintf(stderr, "invalid %s: %s\n", what, arg);
	return e;
}

/* Take option @p opt, with its argument @p arg, into @p cfg: EINVAL, said,
 * if the argument is not valid, and EINVAL if @p opt is no option. */
static int
main_option (struct app_cfg *cfg,
             int             opt,
             char const     *arg)
{
	static size_t const modes = ARRAY_SIZE(main_history_modes);
	static size_t const providers = ARRAY_SIZE(main_providers);
	switch (opt) {
	case 'a': cfg->url[0] = arg; return 0;
	case 'b': cfg->url[1] = arg; return 0;
	case 'A': cfg->model[0] = arg; return 0;
	case 'B': cfg->model[1] = arg; return 0;
	case 's': cfg->system = arg; return 0;
	case 'p': cfg->prompt = arg; return 0;
	case 'd': cfg->debug = 1; return 0;
	case 'P': cfg->template_check = 0; return 0;
	case MAIN_A_PROVIDER: {
		size_t i = main_lookup(main_providers, providers, arg);
		cfg->provider[0] = (enum app_provider)i;
		return main_invalid(i < providers ? 0 : EINVAL, "model A provider", arg);
	}
	case MAIN_B_PROVIDER: {
		size_t i = main_lookup(main_providers, providers, arg);
		cfg->provider[1] = (enum app_provider)i;
		return main_invalid(i < providers ? 0 : EINVAL, "model B provider", arg);
	}
	case 'H': {
		size_t i = main_lookup(main_history_modes, modes, arg);
		cfg->history_mode = (enum app_history_mode)i;
		return main_invalid(i < modes ? 0 : EINVAL, "history mode", arg);
	}
	case 'q':
		return main_invalid(main_u32(arg, &cfg->rapid_quantum),
		                    "rapid quantum", arg);
	case 'R':
		return main_invalid(main_u32(arg, &cfg->rapid_budget),
		                    "rapid budget", arg);
	case 'C':
		return main_invalid(main_long(arg, &cfg->crosstalk_rounds, -1),
		                    "crosstalk round count", arg);
	case 't':
		return main_invalid(main_temperature(arg, &cfg->temperature),
		                    "temperature", arg);
	case 'n':
		return main_invalid(main_long(arg, &cfg->max_tokens, 1),
		                    "max token count", arg);
	default:
		return EINVAL;
	}
}

/* Fill in model @p i's URL, and the model of DeepInfra, as its provider has
 * them: EINVAL, said, if rapid mode cannot use that model. */
static int
main_model (struct app_cfg *cfg,
            size_t          i)
{
	static char const *const url[][2] = {
		[APP_PROVIDER_LLAMA] = {
			"http://127.0.0.1:8080/v1/chat/completions",
			"http://127.0.0.1:8081/v1/chat/completions"
		},
		[APP_PROVIDER_DEEPINFRA] = {
			"https://api.deepinfra.com/v1/openai/chat/completions",
			"https://api.deepinfra.com/v1/openai/chat/completions"
		}
	};
	if (!cfg->url[i])
		cfg->url[i] = url[cfg->provider[i]][i];
	if (cfg->provider[i] != APP_PROVIDER_DEEPINFRA)
		return 0;

	if (!cfg->model[i])
		cfg->model[i] = "Qwen/Qwen3.6-35B-A3B";
	if (!cfg->rapid_quantum ||
	    !strncmp(cfg->model[i], "Qwen/", sizeof "Qwen/" - 1) ||
	    !strncmp(cfg->model[i], "qwen/", sizeof "qwen/" - 1))
		return 0;

	fprintf(stderr,
	        "DeepInfra model %c must currently be a Qwen reasoning model "
	        "in rapid mode; raw reasoning re-injection is not defined for %s\n",
	        (int)('A' + i), cfg->model[i]);
	return EINVAL;
}

/* The DeepInfra API key, from the first of the variables that has one:
 * EINVAL, said, if neither has. */
static int
main_key (struct app_cfg *cfg)
{
	static char const *const names[] = {
		"DEEPINFRA_API_KEY",
		"DEEPINFRA_TOKEN"
	};
	for (size_t i = 0; i < ARRAY_SIZE(names); ++i) {
		char const *key = getenv(names[i]);
		if (key && *key) {
			cfg->deepinfra_key = key;
			return 0;
		}
	}
	fputs("DEEPINFRA_API_KEY or DEEPINFRA_TOKEN is required for "
	      "DeepInfra providers\n", stderr);
	return EINVAL;
}

/* Fill in what @p cfg was not given: EINVAL, said, if what it was given does
 * not go together. */
static int
main_complete (struct app_cfg *cfg)
{
	if (cfg->rapid_budget) {
		if (!cfg->rapid_quantum) {
			fputs("--rapid-budget requires --rapid\n", stderr);
			return EINVAL;
		}
	} else if (cfg->rapid_quantum) {
		cfg->rapid_budget = 512;
	}

	for (size_t i = 0; i < ARRAY_SIZE(cfg->provider); ++i) {
		int e = main_model(cfg, i);
		if (e)
			return e;
	}
	if (cfg->provider[0] == APP_PROVIDER_DEEPINFRA ||
	    cfg->provider[1] == APP_PROVIDER_DEEPINFRA)
		return main_key(cfg);
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

	static struct option const options[] = {
		{ "a-url",             required_argument, nullptr, 'a' },
		{ "b-url",             required_argument, nullptr, 'b' },
		{ "a-model",           required_argument, nullptr, 'A' },
		{ "b-model",           required_argument, nullptr, 'B' },
		{ "a-provider",        required_argument, nullptr, MAIN_A_PROVIDER },
		{ "b-provider",        required_argument, nullptr, MAIN_B_PROVIDER },
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
		case 'h':
			main_usage(stdout, argv[0]);
			return 0;
		case '?':
			main_usage(stderr, argv[0]);
			return 2;
		}
		if (main_option(&cfg, opt, optarg))
			return 2;
	}

	if (optind != argc) {
		main_usage(stderr, argv[0]);
		return 2;
	}
	if (main_complete(&cfg))
		return 2;

	/* libcurl leaves SIGPIPE to us because CURLOPT_NOSIGNAL is set. */
	(void)signal(SIGPIPE, SIG_IGN);
	int e = app_run(&cfg);
	if (e) {
		fprintf(stderr, "%s: %s\n", argv[0], strerror(e));
		return 1;
	}
	return 0;
}
