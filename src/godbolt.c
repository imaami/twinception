/** @file The godbolt plugin.  Its tools use a Compiler Explorer service,
 *  https://godbolt.org unless the option "url" names another one.
 *
 * godbolt_catalog lists the languages of the service, the compilers of one
 * language, or the numbers of compilers of each kind, family or
 * architecture.  godbolt_compile compiles source code once with each of
 * one or more compilers and each of one or more argument strings.  Its
 * output has a record for each compilation, with the exit code, the
 * assembly and the messages of the compiler.  godbolt_preprocess,
 * godbolt_macros and godbolt_targets compile in the same way.  Their
 * records have the preprocessed source, the predefined macros, or facts
 * about the target that follow from the predefined macros
 * (src/godbolt_probe.h).  The flags of the compiler's driver depend on the
 * kind of the compiler, which the list of compilers has.  README.md
 * describes the tools and the option.
 *
 * A call sends its HTTP requests through askai with mcp_slot_fetch(), one
 * at a time.  askai can store a response for the seconds of the keep of
 * the request, so the plugin stores nothing between calls.  Each call has
 * a struct godbolt, to which the call's own word points.  The plugin has
 * no instances and no mutable static variable.
 *
 * A step of a call does no more work than the bounds of the loop allow
 * (src/loop_export.h).  A list of compilers is a JSON array, and the
 * plugin parses it one element at a time, so it never has a tree of the
 * whole list.  The plugin parses a response to a compilation in slices
 * into one tree, writes it in slices, and frees it in slices.  One step
 * parses the output of the probe of godbolt_targets, which is usually less
 * than 100 KiB, but can be as long as a response.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "godbolt_probe.h"
#include "plugin.h"

PLUGIN_MANIFEST(.name = "godbolt",
                .tools = { "godbolt_catalog", "godbolt_compile", "godbolt_preprocess",
                           "godbolt_macros", "godbolt_targets" });

enum {
	GODBOLT_URL_MAX      = 255,       /* bytes of the option url */
	GODBOLT_IDS          = 16,        /* compiler ids of a call */
	GODBOLT_ID_MAX       = 128,       /* bytes of an id */
	GODBOLT_ARGS         = 8,         /* argument strings of a call */
	GODBOLT_ARG_MAX      = 8 << 10,   /* bytes of an argument string */
	GODBOLT_JOBS         = 32,        /* compilations of a call */
	GODBOLT_LANGUAGE_MAX = 64,        /* bytes of a language id */
	GODBOLT_SOURCE_MAX   = 1 << 20,   /* bytes of the source */
	GODBOLT_MATCH_MAX    = 256,       /* bytes of the argument match */
	GODBOLT_WORDS        = 8,         /* words of the argument match */
	GODBOLT_VALUE_MAX    = 128,       /* bytes of a kind, a family or an
	                                     architecture */
	GODBOLT_LIMIT        = 100,       /* the default of the argument limit */
	GODBOLT_LIMIT_MAX    = 2000,      /* the largest limit */
	GODBOLT_FULL         = 14 << 20,  /* bytes of output after which the
	                                     call sends no compilation */
	GODBOLT_REASON_MAX   = 200,       /* bytes of an error text */
	GODBOLT_ATTEMPTS     = 3,         /* attempts of a request */
	GODBOLT_RETRY_MAX    = 30,        /* seconds of the longest wait before
	                                     an attempt */
	GODBOLT_VALUE_BYTES  = 8,         /* bytes of JSON text for each value,
	                                     an estimate */
	GODBOLT_ELEMENT_MAX  = 4 << 10,   /* bytes of an element of a list */
	GODBOLT_TAGS         = 64,        /* family tags of a compiler */
	GODBOLT_KEYS         = 256,       /* different keys of the counts of
	                                     the catalog */
	GODBOLT_TAIL_MAX     = 6 * GODBOLT_ARG_MAX + 1024,  /* bytes of a body
	                                                       after the source */
};

/* The fields of the list of compilers that the plugin requests.  Each call
 * requests the same fields, so askai stores one list for each language. */
#define GODBOLT_FIELDS \
	"id,name,compilerType,compilerCategories,semver,instructionSet,releaseTrack"

/* The bytes of a compiler id, and of a language id. */
static char const godbolt_id_bytes[] =
	"+-.0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
static char const godbolt_language_bytes[] =
	"+-0123456789_abcdefghijklmnopqrstuvwxyz";

/* The plugin's configuration.  configure() stores it, and mcp_slot_config()
 * returns it.  It is plain data without pointers. */
struct godbolt_config {
	uint32_t len;                       /* the length of url in bytes */
	char     url[GODBOLT_URL_MAX + 1];  /* the base URL without a trailing
	                                       '/', NUL-terminated */
};

/* The configuration without options.  definition() uses it if askai passes
 * no configuration. */
static struct godbolt_config const godbolt_defaults = {
	.len = sizeof "https://godbolt.org" - 1,
	.url = "https://godbolt.org"
};

/* The tools, in the order of the manifest. */
enum godbolt_tool : uint8_t {
	GODBOLT_CATALOG,
	GODBOLT_COMPILE,
	GODBOLT_PREPROCESS,
	GODBOLT_MACROS,
	GODBOLT_TARGETS,
	GODBOLT_TOOLS
};

/* The kinds of driver of a compiler.  The list of compilers has the kind of
 * each compiler as its compilerType.  GODBOLT_GNU is every kind but
 * win32-vc and clang-cl. */
enum godbolt_driver : uint8_t {
	GODBOLT_GNU,
	GODBOLT_MSVC,
	GODBOLT_CLANG_CL,
	GODBOLT_DRIVERS
};

/* Where the main text of a record is in the response to a compilation. */
enum godbolt_from : uint32_t {
	GODBOLT_FROM_ASM,     /* in asm, before stdout and stderr */
	GODBOLT_FROM_PP,      /* in ppOutput.output, before stdout and stderr */
	GODBOLT_FROM_STDOUT,  /* in stdout, before stderr */
	GODBOLT_FROM_NONE,    /* none, because the compilation is not sent */
};

/* The sections of a record. */
enum godbolt_sec : uint8_t {
	GODBOLT_SEC_ASM,
	GODBOLT_SEC_STDOUT,
	GODBOLT_SEC_STDERR,
	GODBOLT_SEC_PP,
	GODBOLT_SEC_MACROS,
};

/* The kinds of request that the plugin sends. */
enum godbolt_req : uint8_t {
	GODBOLT_GET_LANGUAGES,  /* GET /api/languages */
	GODBOLT_GET_COMPILERS,  /* GET /api/compilers/LANGUAGE?fields=... */
	GODBOLT_POST_COMPILE,   /* POST /api/compiler/ID/compile */
};

/* The failures after which the plugin sends a request again, besides the
 * statuses 429, 502 and 503. */
enum godbolt_again : uint32_t {
	GODBOLT_AGAIN_504      = 1u << 0,
	GODBOLT_AGAIN_EIO      = 1u << 1,
	GODBOLT_AGAIN_TIMEDOUT = 1u << 2,
};

/* The limits of a kind of request, and the failures after which the plugin
 * sends it again. */
struct godbolt_rule {
	uint32_t max;      /* bytes of the response body */
	uint32_t timeout;  /* seconds */
	uint32_t keep;     /* seconds, but godbolt_request() sets the keep of
	                      a compilation */
	uint32_t again;    /* enum godbolt_again */
};

/* The plugin does not send a compilation again after 504 or a timeout,
 * because Compiler Explorer can still be compiling it. */
static struct godbolt_rule const godbolt_rules[] = {
	[GODBOLT_GET_LANGUAGES] = { 1 << 20, 30, 86400, GODBOLT_AGAIN_504 | GODBOLT_AGAIN_EIO |
	                                                GODBOLT_AGAIN_TIMEDOUT },
	[GODBOLT_GET_COMPILERS] = { 4 << 20, 30, 3600, GODBOLT_AGAIN_504 | GODBOLT_AGAIN_EIO |
	                                               GODBOLT_AGAIN_TIMEDOUT },
	[GODBOLT_POST_COMPILE]  = { 4 << 20, 60, 300, 0 },
};

/* What the next step of a call does. */
enum godbolt_phase : uint8_t {
	GODBOLT_LIST,    /* reads the response to the request of a list */
	GODBOLT_ROWS,    /* parses the list, and writes rows or reads the
	                    kinds of the compilers */
	GODBOLT_ESCAPE,  /* escapes the source into the body of the requests */
	GODBOLT_JOB,     /* sends the request of the next compilation */
	GODBOLT_ANSWER,  /* reads the response to a compilation */
	GODBOLT_PARSE,   /* parses the response to a compilation */
	GODBOLT_WRITE,   /* writes the record of a compilation */
	GODBOLT_FREE,    /* frees the parsed response to a compilation */
};

/* The state of the call's request. */
enum godbolt_wait : uint8_t {
	GODBOLT_IDLE,     /* no request is under way */
	GODBOLT_FETCH,    /* the request is under way */
	GODBOLT_TIMER,    /* the timer sends the request again later */
	GODBOLT_REFUSED,  /* askai refused the request that the timer sent,
	                     and call->why has the reason */
};

/* The values that the functions of a phase return. */
enum godbolt_go {
	GODBOLT_GO,     /* the next step goes on */
	GODBOLT_WAIT,   /* a request is under way, or the timer is set */
	GODBOLT_BAD,    /* the request failed, and call->why has the reason */
	GODBOLT_ENDED,  /* the function ended the call, so the call's memory
	                   may be gone */
};

/* The states of the parse of a JSON array, one element at a time. */
enum godbolt_array : uint8_t {
	GODBOLT_LIST_OPEN,   /* before the '[' */
	GODBOLT_LIST_FIRST,  /* after the '[', before an element or the ']' */
	GODBOLT_LIST_VALUE,  /* inside an element */
	GODBOLT_LIST_NEXT,   /* after an element, before a ',' or the ']' */
	GODBOLT_LIST_COMMA,  /* after a ',', before an element */
	GODBOLT_LIST_END,    /* after the ']' */
	GODBOLT_LIST_BAD,    /* the text is not a JSON array */
};

/* The initializer of a struct tool_text in a static table, where the
 * compound literal of TOOL_TEXT() is not a constant. */
#define GODBOLT_TEXT(s) { "" s, sizeof s - 1 }

/* The actions of godbolt_catalog. */
enum godbolt_action : uint8_t {
	GODBOLT_LANGUAGES,
	GODBOLT_COMPILERS,
	GODBOLT_KINDS,
	GODBOLT_FAMILIES,
	GODBOLT_ARCHITECTURES,
	GODBOLT_ACTIONS
};

static struct tool_text const godbolt_actions[] = {
	[GODBOLT_LANGUAGES]     = GODBOLT_TEXT("languages"),
	[GODBOLT_COMPILERS]     = GODBOLT_TEXT("compilers"),
	[GODBOLT_KINDS]         = GODBOLT_TEXT("kinds"),
	[GODBOLT_FAMILIES]      = GODBOLT_TEXT("families"),
	[GODBOLT_ARCHITECTURES] = GODBOLT_TEXT("architectures"),
};

/* The singular of the noun of each action. */
static char const *const godbolt_singulars[] = {
	[GODBOLT_LANGUAGES]     = "language",
	[GODBOLT_COMPILERS]     = "compiler",
	[GODBOLT_KINDS]         = "kind",
	[GODBOLT_FAMILIES]      = "family",
	[GODBOLT_ARCHITECTURES] = "architecture",
};

/* The first line of the output of each action. */
static struct tool_text const godbolt_headers[] = {
	[GODBOLT_LANGUAGES]     = GODBOLT_TEXT("id\tname\n"),
	[GODBOLT_COMPILERS]     = GODBOLT_TEXT("id\tname\tkind\tversion\tarchitecture\t"
	                                    "track\tfamilies\n"),
	[GODBOLT_KINDS]         = GODBOLT_TEXT("kind\tcompilers\n"),
	[GODBOLT_FAMILIES]      = GODBOLT_TEXT("family\tcompilers\n"),
	[GODBOLT_ARCHITECTURES] = GODBOLT_TEXT("architecture\tcompilers\n"),
};

/* The ends of the compilations, which the last line of the output
 * counts. */
enum godbolt_outcome : uint8_t {
	GODBOLT_ZERO,    /* exit code 0 */
	GODBOLT_OTHER,   /* another exit code, or a timeout */
	GODBOLT_FAILED,  /* a failed request */
	GODBOLT_UNSENT,  /* no request */
	GODBOLT_COUNTS
};

/* The arguments of the tools.  Bit i of a mask of struct godbolt_params
 * stands for the argument godbolt_arg_names[i]. */
enum godbolt_arg : uint8_t {
	GODBOLT_ARG_ACTION,
	GODBOLT_ARG_LANGUAGE,
	GODBOLT_ARG_IDS,
	GODBOLT_ARG_MATCH,
	GODBOLT_ARG_KIND,
	GODBOLT_ARG_FAMILY,
	GODBOLT_ARG_ARCHITECTURE,
	GODBOLT_ARG_LIMIT,
	GODBOLT_ARG_OFFSET,
	GODBOLT_ARG_COMPILERS,
	GODBOLT_ARG_ARGS,
	GODBOLT_ARG_SOURCE,
	GODBOLT_ARG_FILTERS,
	GODBOLT_ARG_HEADERS,
	GODBOLT_ARG_COUNT
};

static char const *const godbolt_arg_names[] = {
	"action", "language", "ids", "match", "kind", "family", "architecture",
	"limit", "offset", "compilers", "args", "source", "filters", "headers"
};

static_assert(sizeof godbolt_arg_names / sizeof *godbolt_arg_names == GODBOLT_ARG_COUNT);

/* The reason of the refusal of each argument. */
static struct tool_text const godbolt_arg_rules[] = {
	[GODBOLT_ARG_ACTION] = GODBOLT_TEXT(
		"action must be languages, compilers, kinds, families or architectures."),
	[GODBOLT_ARG_LANGUAGE] = GODBOLT_TEXT(
		"language must have 1 to 64 bytes of a-z, 0-9, +, - and _."),
	[GODBOLT_ARG_IDS] = GODBOLT_TEXT(
		"ids must be an array of 1 to 16 different ids. An id has 1 to 128 "
		"bytes of A-Z, a-z, 0-9, +, -, . and _, and it is not . or .."),
	[GODBOLT_ARG_MATCH] = GODBOLT_TEXT(
		"match must be a string of at most 256 bytes with at most 8 words."),
	[GODBOLT_ARG_KIND] = GODBOLT_TEXT(
		"kind must be a string of 1 to 128 bytes without a NUL byte."),
	[GODBOLT_ARG_FAMILY] = GODBOLT_TEXT(
		"family must be a string of 1 to 128 bytes without a NUL byte."),
	[GODBOLT_ARG_ARCHITECTURE] = GODBOLT_TEXT(
		"architecture must be a string of 1 to 128 bytes without a NUL byte."),
	[GODBOLT_ARG_LIMIT] = GODBOLT_TEXT(
		"limit must be a whole number from 1 to 2000."),
	[GODBOLT_ARG_COMPILERS] = GODBOLT_TEXT(
		"compilers must be an array of 1 to 16 different compiler ids. A "
		"compiler id has 1 to 128 bytes of A-Z, a-z, 0-9, +, -, . and _, and "
		"it is not . or .."),
	[GODBOLT_ARG_ARGS] = GODBOLT_TEXT(
		"args must be an array of 1 to 8 strings. Each string has at most "
		"8192 bytes and no NUL byte."),
	[GODBOLT_ARG_SOURCE] = GODBOLT_TEXT(
		"source must be a string of at most 1 MiB without a NUL byte."),
	[GODBOLT_ARG_FILTERS] = GODBOLT_TEXT(
		"filters must be an object whose members are among intel, demangle, "
		"directives, commentOnly, labels, libraryCode and trim. Each member "
		"is true or false."),
	[GODBOLT_ARG_HEADERS] = GODBOLT_TEXT(
		"headers must be true or false."),
};

#define GODBOLT_BIT(arg) (1u << GODBOLT_ARG_##arg)

/* The arguments of a tool. */
struct godbolt_params {
	uint32_t args;   /* bit i is set if the tool has the argument
	                    godbolt_arg_names[i] */
	uint32_t needs;  /* bit i is set if the argument godbolt_arg_names[i] is
	                    required */
};

static struct godbolt_params const godbolt_params[] = {
	[GODBOLT_CATALOG] = {
		.args = GODBOLT_BIT(ACTION) | GODBOLT_BIT(LANGUAGE) | GODBOLT_BIT(IDS) |
		        GODBOLT_BIT(MATCH) | GODBOLT_BIT(KIND) | GODBOLT_BIT(FAMILY) |
		        GODBOLT_BIT(ARCHITECTURE) | GODBOLT_BIT(LIMIT) | GODBOLT_BIT(OFFSET),
		.needs = GODBOLT_BIT(ACTION)
	},
	[GODBOLT_COMPILE] = {
		.args = GODBOLT_BIT(COMPILERS) | GODBOLT_BIT(ARGS) | GODBOLT_BIT(LANGUAGE) |
		        GODBOLT_BIT(SOURCE) | GODBOLT_BIT(FILTERS) | GODBOLT_BIT(OFFSET),
		.needs = GODBOLT_BIT(COMPILERS) | GODBOLT_BIT(SOURCE)
	},
	[GODBOLT_PREPROCESS] = {
		.args = GODBOLT_BIT(COMPILERS) | GODBOLT_BIT(ARGS) | GODBOLT_BIT(LANGUAGE) |
		        GODBOLT_BIT(SOURCE) | GODBOLT_BIT(OFFSET),
		.needs = GODBOLT_BIT(COMPILERS) | GODBOLT_BIT(SOURCE)
	},
	[GODBOLT_MACROS] = {
		.args = GODBOLT_BIT(COMPILERS) | GODBOLT_BIT(ARGS) | GODBOLT_BIT(LANGUAGE) |
		        GODBOLT_BIT(SOURCE) | GODBOLT_BIT(OFFSET),
		.needs = GODBOLT_BIT(COMPILERS)
	},
	[GODBOLT_TARGETS] = {
		.args = GODBOLT_BIT(COMPILERS) | GODBOLT_BIT(ARGS) | GODBOLT_BIT(LANGUAGE) |
		        GODBOLT_BIT(HEADERS) | GODBOLT_BIT(OFFSET),
		.needs = GODBOLT_BIT(COMPILERS)
	},
};

#undef GODBOLT_BIT

/* The filters of a compilation, in the order of the request body.  A
 * filter whose bit is 0 is always false.  The argument filters can set
 * each of the others. */
static struct {
	struct tool_text name;
	uint8_t          bit;
} const godbolt_filters[] = {
	{ GODBOLT_TEXT("binary"), 0 },
	{ GODBOLT_TEXT("binaryObject"), 0 },
	{ GODBOLT_TEXT("commentOnly"), 1u << 0 },
	{ GODBOLT_TEXT("debugCalls"), 0 },
	{ GODBOLT_TEXT("demangle"), 1u << 1 },
	{ GODBOLT_TEXT("directives"), 1u << 2 },
	{ GODBOLT_TEXT("execute"), 0 },
	{ GODBOLT_TEXT("intel"), 1u << 3 },
	{ GODBOLT_TEXT("labels"), 1u << 4 },
	{ GODBOLT_TEXT("libraryCode"), 1u << 5 },
	{ GODBOLT_TEXT("trim"), 1u << 6 },
};

enum { GODBOLT_FILTERS = sizeof godbolt_filters / sizeof *godbolt_filters };

/* The filters that the Compiler Explorer site sets by default:
 * commentOnly, demangle, directives, intel and labels. */
static constexpr uint8_t GODBOLT_SITE_FILTERS = 0x1f;

/* The first line of each section of a record. */
static struct tool_text const godbolt_sections[] = {
	[GODBOLT_SEC_ASM]    = GODBOLT_TEXT("--- asm ---\n"),
	[GODBOLT_SEC_STDOUT] = GODBOLT_TEXT("--- stdout ---\n"),
	[GODBOLT_SEC_STDERR] = GODBOLT_TEXT("--- stderr ---\n"),
	[GODBOLT_SEC_PP]     = GODBOLT_TEXT("--- pp ---\n"),
	[GODBOLT_SEC_MACROS] = GODBOLT_TEXT("--- macros ---\n"),
};

/* The largest number of sections of a record. */
enum { GODBOLT_SECTIONS = 3 };

/* How a tool compiles with a kind of driver. */
struct godbolt_recipe {
	struct tool_text flags;    /* the arguments before the argument string */
	struct tool_text options;  /* the member compilerOptions of the body */
	uint32_t         from;     /* enum godbolt_from */
	uint32_t         windows;  /* 1 if /TC comes before the flags, or /TP
	                              for a language other than c */
};

#define GODBOLT_PP \
	GODBOLT_TEXT("{\"producePp\":{\"filter-headers\":false,\"clang-format\":false}," \
	             "\"skipAsm\":true}")
#define GODBOLT_SKIP_ASM GODBOLT_TEXT("{\"skipAsm\":true}")

/* The recipes of the tools for each kind of driver.  These are the recipes
 * of godbolt-c with one change: godbolt-c refused a kind of compiler
 * without a recipe, and here such a kind gets the recipe of GCC.  Compiler
 * Explorer itself appends -E for the preprocessed view of any C or C++
 * compiler.  An MSVC compiler cannot output its predefined macros. */
static struct godbolt_recipe const godbolt_recipes[GODBOLT_TOOLS][GODBOLT_DRIVERS] = {
	[GODBOLT_COMPILE] = {
		[GODBOLT_GNU]      = { GODBOLT_TEXT(""), GODBOLT_TEXT("{}"), GODBOLT_FROM_ASM, 0 },
		[GODBOLT_MSVC]     = { GODBOLT_TEXT(""), GODBOLT_TEXT("{}"), GODBOLT_FROM_ASM, 0 },
		[GODBOLT_CLANG_CL] = { GODBOLT_TEXT(""), GODBOLT_TEXT("{}"), GODBOLT_FROM_ASM, 0 },
	},
	[GODBOLT_PREPROCESS] = {
		[GODBOLT_GNU]      = { GODBOLT_TEXT("-E"), GODBOLT_PP, GODBOLT_FROM_PP, 0 },
		[GODBOLT_MSVC]     = { GODBOLT_TEXT("/EP"), GODBOLT_SKIP_ASM, GODBOLT_FROM_STDOUT, 1 },
		[GODBOLT_CLANG_CL] = { GODBOLT_TEXT("/EP"), GODBOLT_SKIP_ASM, GODBOLT_FROM_STDOUT, 1 },
	},
	[GODBOLT_MACROS] = {
		[GODBOLT_GNU]      = { GODBOLT_TEXT("-E -dM"), GODBOLT_PP, GODBOLT_FROM_PP, 0 },
		[GODBOLT_MSVC]     = { GODBOLT_TEXT(""), GODBOLT_TEXT(""), GODBOLT_FROM_NONE, 0 },
		[GODBOLT_CLANG_CL] = { GODBOLT_TEXT("/E /clang:-dM"), GODBOLT_SKIP_ASM,
		                       GODBOLT_FROM_STDOUT, 1 },
	},
	[GODBOLT_TARGETS] = {
		[GODBOLT_GNU]      = { GODBOLT_TEXT("-E"), GODBOLT_PP, GODBOLT_FROM_PP, 0 },
		[GODBOLT_MSVC]     = { GODBOLT_TEXT("/EP"), GODBOLT_SKIP_ASM, GODBOLT_FROM_STDOUT, 1 },
		[GODBOLT_CLANG_CL] = { GODBOLT_TEXT("/EP"), GODBOLT_SKIP_ASM, GODBOLT_FROM_STDOUT, 1 },
	},
};

#undef GODBOLT_SKIP_ASM
#undef GODBOLT_PP
#undef GODBOLT_TEXT

/* The arguments of a call after the checks.  The texts point into the
 * call's JSON arguments. */
struct godbolt_args {
	struct tool_text ids[GODBOLT_IDS];
	struct tool_text args[GODBOLT_ARGS];
	struct tool_text words[GODBOLT_WORDS];
	struct tool_text language;
	struct tool_text source;
	struct tool_text kind;
	struct tool_text family;
	struct tool_text architecture;
	struct tool_text why;       /* the reason of a refusal */
	uint32_t         limit;
	uint8_t          n_ids;
	uint8_t          n_args;
	uint8_t          n_words;
	uint8_t          action;    /* enum godbolt_action */
	uint8_t          filters;   /* bits of godbolt_filters[] */
	uint8_t          headers;   /* the argument headers */
	char             text[96];  /* the bytes of why, unless why is a
	                               constant */
};

/* A call of a tool.  The call's own word points to it, and drop() frees
 * it.  The texts of the call's arguments are copies in blob, at the
 * end of the same allocation. */
struct godbolt {
	struct loop_timer            timer;     /* the first member, so that the
	                                           timer function can cast the
	                                           timer to the call */
	struct web_request           req;       /* the request under way, or the
	                                           request that the timer sends */
	struct godbolt_probe         probe;     /* the parsed output of the probe
	                                           of godbolt_targets */
	struct mcp_slot             *slot;
	struct loop                 *loop;
	struct godbolt_config const *cfg;
	struct json_tokener         *tok;       /* the parse of a response */
	struct json_object          *doc;       /* the response to a compilation,
	                                           or the counts of the catalog */
	char                        *body;      /* the body of the request of a
	                                           compilation */
	struct json_object          *secs[GODBOLT_SECTIONS];  /* the texts of
	                                                         the sections */
	struct tool_text             ids[GODBOLT_IDS];
	struct tool_text             args[GODBOLT_ARGS];
	struct tool_text             words[GODBOLT_WORDS];   /* in lower case */
	struct tool_text             language;
	struct tool_text             source;
	struct tool_text             kind;
	struct tool_text             family;
	struct tool_text             architecture;
	size_t                       cap;       /* bytes of body */
	size_t                       head;      /* bytes of body that each
	                                           compilation sends unchanged */
	size_t                       len;       /* bytes of the body of the
	                                           request */
	size_t                       off;       /* bytes of the source escaped,
	                                           or of the response parsed */
	size_t                       start;     /* the offset in the list of the
	                                           element being parsed */
	size_t                       written;   /* bytes of output */
	size_t                       arg_at;    /* the offset in body of the
	                                           escaped argument string */
	size_t                       arg_len;   /* its length */
	size_t                       byte;      /* bytes of the text of the
	                                           section written */
	size_t                       item;      /* lines of the section
	                                           written */
	uint32_t                     limit;     /* the argument limit */
	uint32_t                     total;     /* elements of the list with an
	                                           id */
	uint32_t                     matching;  /* elements that the filters
	                                           select */
	uint32_t                     listed;    /* rows written */
	uint32_t                     malformed; /* elements that the plugin
	                                           left out */
	uint32_t                     attempt;   /* attempts of the request */
	uint32_t                     why_len;   /* bytes of why */
	uint32_t                     counts[GODBOLT_COUNTS];
	int                          e;         /* the error number of the first
	                                           failure of the output, after
	                                           which the call writes
	                                           nothing */
	uint16_t                     found;     /* bit i is set if ids[i] is in
	                                           the list */
	uint8_t                      drivers[GODBOLT_IDS];  /* enum
	                                                       godbolt_driver of
	                                                       ids[i] */
	uint8_t                      sec_of[GODBOLT_SECTIONS];  /* enum
	                                                           godbolt_sec of
	                                                           secs[i] */
	uint8_t                      n_sec;     /* the sections of the record */
	uint8_t                      tool;      /* enum godbolt_tool */
	uint8_t                      action;    /* enum godbolt_action */
	uint8_t                      phase;     /* enum godbolt_phase */
	uint8_t                      wait;      /* enum godbolt_wait */
	uint8_t                      kind_of;   /* enum godbolt_req */
	uint8_t                      list;      /* enum godbolt_array */
	uint8_t                      n_ids;
	uint8_t                      n_args;
	uint8_t                      n_words;
	uint8_t                      job;       /* the compilation */
	uint8_t                      filters;   /* bits of godbolt_filters[] */
	uint8_t                      paged;     /* the call has an offset */
	uint8_t                      sec;       /* the section being written */
	uint8_t                      opened;    /* the section has its first
	                                           line */
	uint8_t                      headed;    /* the record has its first
	                                           line */
	uint8_t                      marked;    /* the section has a line that
	                                           marks a cut */
	uint8_t                      cut;       /* the record has a note on a
	                                           cut */
	uint8_t                      truncated; /* the response has truncated
	                                           true */
	uint8_t                      full;      /* the output has GODBOLT_FULL
	                                           bytes */
	uint8_t                      headers;   /* the argument headers */
	uint8_t                      probed;    /* probe has the facts of the
	                                           record */
	uint8_t                      no_pp;     /* the response has no text of
	                                           the preprocessor */
	char                         why[GODBOLT_REASON_MAX + 120];
	char                         url[1024];
	char                         blob[];
};

static_assert(GODBOLT_URL_MAX + sizeof "/api/compiler/" + 3 * GODBOLT_ID_MAX +
              sizeof "/compile" <= sizeof ((struct godbolt *)nullptr)->url);
static_assert(GODBOLT_URL_MAX + sizeof "/api/compilers/" + 3 * GODBOLT_LANGUAGE_MAX +
              sizeof "?fields=" GODBOLT_FIELDS <= sizeof ((struct godbolt *)nullptr)->url);

/* Returns the lower-case form of the ASCII letter @p c, or @p c. */
static char
godbolt_lower (char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c | 0x20) : c;
}

/* Returns whether the texts @p a and @p b have the same bytes. */
static bool
godbolt_same (struct tool_text a,
              struct tool_text b)
{
	return a.len == b.len && (!a.len || !memcmp(a.ptr, b.ptr, a.len));
}

/* Returns whether @p text contains @p word in any letter case.  @p word is
 * in lower case and not empty. */
static bool
godbolt_has (struct tool_text text,
             struct tool_text word)
{
	for (size_t i = 0; i + word.len <= text.len; ++i) {
		size_t k = 0;
		while (k < word.len && godbolt_lower(text.ptr[i + k]) == word.ptr[k])
			++k;
		if (k == word.len)
			return true;
	}
	return false;
}

/* Returns @p t, or @p otherwise if @p t is empty. */
static struct tool_text
godbolt_or (struct tool_text t,
            struct tool_text otherwise)
{
	return t.len ? t : otherwise;
}

/* Returns the string member @p name of @p obj, or an empty text whose ptr
 * is nullptr if @p obj has no such member.  The text is NUL-terminated. */
static struct tool_text
godbolt_member (struct json_object *obj,
                char const         *name)
{
	struct json_object *v = nullptr;
	if (!json_object_object_get_ex(obj, name, &v) ||
	    !json_object_is_type(v, json_type_string))
		return (struct tool_text){};
	return (struct tool_text){ json_object_get_string(v),
	                           (size_t)json_object_get_string_len(v) };
}

/* Returns @p n, or less if the first @p n of the @p len bytes at @p s would
 * end inside a UTF-8 character, so that a cut after the returned length
 * does not split a character.  The cut moves back by at most 3 bytes. */
static size_t
godbolt_cut (char const *s,
             size_t      len,
             size_t      n)
{
	n = n < len ? n : len;
	for (size_t k = 0; k < 3 && n && n < len && ((unsigned char)s[n] & 0xc0) == 0x80; ++k)
		--n;
	return n;
}

/* Returns "s" if @p n is not 1, for the plural of a noun after a count, and
 * "" if it is. */
static char const *
godbolt_s (uint64_t n)
{
	return n == 1 ? "" : "s";
}

/* Copies @p t to @p d, and returns the end of the copy. */
static char *
godbolt_cat (char             *d,
             struct tool_text  t)
{
	if (t.len)
		memcpy(d, t.ptr, t.len);
	return d + t.len;
}

/* Writes @p t to @p d, with each '+' as %2B.  '+' is the only byte of an
 * id that needs an escape in the path of a URL.  Returns the end of the
 * text in @p d. */
static char *
godbolt_cat_path (char             *d,
                  struct tool_text  t)
{
	for (size_t i = 0; i < t.len; ++i) {
		if (t.ptr[i] == '+')
			d = godbolt_cat(d, TOOL_TEXT("%2B"));
		else
			*d++ = t.ptr[i];
	}
	return d;
}

/* Writes the @p n bytes at @p s as the inside of a JSON string to @p d,
 * which has room for 6 * @p n bytes.  A quote, a backslash and a control
 * byte get an escape.  Returns the end of the text in @p d. */
static char *
godbolt_escape (char       *d,
                char const *s,
                size_t      n)
{
	static char const hex[] = "0123456789abcdef";
	static char const letter[0x20] = {
		['\b'] = 'b', ['\t'] = 't', ['\n'] = 'n', ['\f'] = 'f', ['\r'] = 'r'
	};
	for (size_t i = 0; i < n; ++i) {
		unsigned char const c = (unsigned char)s[i];
		if (c >= 0x20 && c != '"' && c != '\\') {
			*d++ = (char)c;
			continue;
		}
		*d++ = '\\';
		if (c >= 0x20 || letter[c]) {
			*d++ = c >= 0x20 ? (char)c : letter[c];
			continue;
		}
		d = godbolt_cat(d, TOOL_TEXT("u00"));
		*d++ = hex[c >> 4];
		*d++ = hex[c & 15];
	}
	return d;
}

/* Writes the 11 filters of a compilation as a JSON object to @p d.  The
 * filters that @p bits has are true.  Returns the end of the text in
 * @p d. */
static char *
godbolt_cat_filters (char    *d,
                     uint8_t  bits)
{
	char sep = '{';
	for (size_t i = 0; i < GODBOLT_FILTERS; ++i) {
		*d++ = sep;
		*d++ = '"';
		d = godbolt_cat(d, godbolt_filters[i].name);
		d = godbolt_cat(d, bits & godbolt_filters[i].bit ? TOOL_TEXT("\":true")
		                                                 : TOOL_TEXT("\":false"));
		sep = ',';
	}
	*d++ = '}';
	return d;
}

/* Appends @p t to the call's output, unless an earlier write failed.
 * Returns 0, or call->e, the error number of the call's first failed
 * write. */
static int
godbolt_put (struct godbolt   *call,
             struct tool_text  t)
{
	size_t taken = 0;
	if (!call->e && t.len)
		call->e = mcp_slot_write(call->slot, t, 0, &taken);
	call->written += taken;
	return call->e;
}

/* Appends the text that @p fmt and its arguments format to the call's
 * output.  The text has at most 511 bytes.  Returns 0, or call->e. */
[[gnu::format(printf, 2, 3)]]
static int
godbolt_putf (struct godbolt *call,
              char const     *fmt,
              ...)
{
	char text[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	if (n >= 0)
		return godbolt_put(call, (struct tool_text){
			text, n < (int)sizeof text ? (size_t)n : sizeof text - 1 });
	call->e = call->e ? call->e : EINVAL;
	return call->e;
}

/* Stores the text that @p fmt and its arguments format in call->why.  Each
 * control byte of the text becomes a space. */
[[gnu::format(printf, 2, 3)]]
static void
godbolt_why (struct godbolt *call,
             char const     *fmt,
             ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(call->why, sizeof call->why, fmt, ap);
	va_end(ap);
	call->why_len = n < 0 ? 0 : n < (int)sizeof call->why ? (uint32_t)n
	                                                       : sizeof call->why - 1;
	for (uint32_t i = 0; i < call->why_len; ++i)
		if ((unsigned char)call->why[i] < 0x20 || call->why[i] == 0x7f)
			call->why[i] = ' ';
}

/* Ends the call as failed with the text that @p fmt and its arguments
 * format.  Returns GODBOLT_ENDED. */
[[gnu::format(printf, 2, 3)]]
static enum godbolt_go
godbolt_refuse (struct godbolt *call,
                char const     *fmt,
                ...)
{
	char text[TOOL_TELL_MAX];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	if (n < 0)
		mcp_slot_tell(call->slot, TOOL_TEXT("The plugin could not format the reason of "
		                                    "the failure."), TOOL_FAILED);
	else
		mcp_slot_tell(call->slot, (struct tool_text){
			text, n < (int)sizeof text ? (size_t)n : sizeof text - 1 }, TOOL_FAILED);
	return GODBOLT_ENDED;
}

/* Ends the call after the first failure of its output, whose error number
 * is call->e.  After ENOMEM or ESHUTDOWN, the call ends with the error
 * number.  After any other error, for example EFBIG when the output has
 * reached its largest length, the call fails with the output that it has.
 * Returns GODBOLT_ENDED. */
static enum godbolt_go
godbolt_fail (struct godbolt *call)
{
	if (call->e == ENOMEM || call->e == ESHUTDOWN)
		mcp_slot_fail(call->slot, call->e);
	else
		mcp_slot_finish(call->slot, TOOL_FAILED);
	return GODBOLT_ENDED;
}

/* Sets the call's request to a request of the kind @p kind, with the URL in
 * call->url and the @p len bytes of the body at @p body.  A compilation of
 * a call without offset has the keep 1, so that it gets no response that
 * askai received more than a second earlier.  A compilation of a call with
 * offset has the keep 300, so that it gets the response that the same call
 * without offset received, while askai still stores it. */
static void
godbolt_request (struct godbolt   *call,
                 enum godbolt_req  kind,
                 char const       *body,
                 size_t            len)
{
	struct godbolt_rule const *rule = &godbolt_rules[kind];
	call->req = (struct web_request){
		.url = call->url,
		.body = body,
		.len = len,
		.accept = "application/json",
		.max = rule->max,
		.timeout = rule->timeout,
		.keep = kind == GODBOLT_POST_COMPILE && !call->paged ? 1 : rule->keep
	};
	call->kind_of = kind;
	call->attempt = 0;
}

/* Sets the call's timer to send the request again in @p seconds.  Returns
 * GODBOLT_WAIT.  If the timer cannot be set, it returns GODBOLT_BAD with
 * the reason in call->why, or GODBOLT_ENDED after ENOMEM. */
static enum godbolt_go
godbolt_later (struct godbolt *call,
               uint32_t        seconds)
{
	int const e = loop_timer_set(call->loop, &call->timer,
	                             loop_now(call->loop) + seconds * UINT64_C(1'000'000'000));
	if (e == ENOMEM) {
		mcp_slot_fail(call->slot, ENOMEM);
		return GODBOLT_ENDED;
	}
	if (e) {
		godbolt_why(call, "askai could not set the timer to send the request again");
		return GODBOLT_BAD;
	}
	call->wait = GODBOLT_TIMER;
	return GODBOLT_WAIT;
}

/* Sends the call's request.  Returns GODBOLT_WAIT if the request is under
 * way or the timer sends it later, GODBOLT_BAD with the reason in
 * call->why if askai refused it, or GODBOLT_ENDED. */
static enum godbolt_go
godbolt_send (struct godbolt *call)
{
	int e = mcp_slot_fetch(call->slot, &call->req);
	++call->attempt;
	if (!e) {
		call->wait = GODBOLT_FETCH;
		return GODBOLT_WAIT;
	}
	if (e == EAGAIN && call->attempt < GODBOLT_ATTEMPTS)
		return godbolt_later(call, 1);
	if (e == ENOMEM) {
		mcp_slot_fail(call->slot, ENOMEM);
		return GODBOLT_ENDED;
	}
	godbolt_why(call, "%s", e == EAGAIN ? "askai sends too many requests at once"
	                                    : "askai refused the request");
	return GODBOLT_BAD;
}

/* The timer function of a call.  It sends the call's request again.  If
 * askai refuses the request, the next step of the call reads the reason in
 * call->why. */
static void
godbolt_fired (struct loop       *loop,
               struct loop_timer *timer)
{
	(void)loop;
	struct godbolt *call = (struct godbolt *)timer;
	if (!call || !call->slot)
		return;
	call->wait = GODBOLT_IDLE;
	if (godbolt_send(call) == GODBOLT_BAD) {
		call->wait = GODBOLT_REFUSED;
		(void)mcp_slot_ready(call->slot);
	}
}

/* Stores in call->why the reason of the response @p r, whose status is an
 * error.  The reason is the member message of a JSON object, or else the
 * first line of the body, and it has at most GODBOLT_REASON_MAX bytes.  A
 * compilation with an unknown compiler id gets 404, and its reason names
 * godbolt_catalog. */
static void
godbolt_reason (struct godbolt            *call,
                struct web_response const *r)
{
	if (call->kind_of == GODBOLT_POST_COMPILE && r->status == 404) {
		godbolt_why(call, "HTTP 404: Compiler Explorer has no compiler with this "
		            "id. godbolt_catalog lists the ids.");
		return;
	}
	struct tool_text text = { r->body, r->body ? r->len : 0 };
	struct json_object *doc = nullptr;
	if (text.len <= LOOP_STEP_PARSE) {
		json_tokener_reset(call->tok);
		doc = json_tokener_parse_ex(call->tok, text.ptr ? text.ptr : "", (int)text.len);
		loop_work(call->loop, LOOP_PARSE, text.len);
		text = godbolt_or(godbolt_member(doc, "message"), text);
	}
	char const *nl = text.len ? memchr(text.ptr, '\n', text.len) : nullptr;
	size_t n = nl ? (size_t)(nl - text.ptr) : text.len;
	n = godbolt_cut(text.ptr, text.len, n < GODBOLT_REASON_MAX ? n : GODBOLT_REASON_MAX);
	godbolt_why(call, "HTTP %" PRIu32 "%s%.*s", r->status, n ? ": " : "", (int)n,
	            n ? text.ptr : "");
	json_object_put(doc);
}

/* Reads the result of the call's request into @p r.  Returns GODBOLT_GO
 * for a whole response with a status from 200 to 299.  Returns GODBOLT_WAIT
 * if the request is under way, or if the function set the timer to send it
 * again.  Returns GODBOLT_BAD with the reason in call->why if the request
 * failed, or GODBOLT_ENDED if the function ended the call. */
static enum godbolt_go
godbolt_result (struct godbolt      *call,
                struct web_response *r)
{
	*r = (struct web_response){};
	if (call->wait == GODBOLT_REFUSED) {
		call->wait = GODBOLT_IDLE;
		return GODBOLT_BAD;
	}
	int e = mcp_slot_fetched(call->slot, r);
	if (e == EINPROGRESS)
		return GODBOLT_WAIT;
	call->wait = GODBOLT_IDLE;
	if (e || (!r->body && r->len) || !r->error) {
		godbolt_why(call, "askai has no response to the request");
		return GODBOLT_BAD;
	}
	if (r->e == ESHUTDOWN || r->e == ENOMEM) {
		mcp_slot_fail(call->slot, r->e);
		return GODBOLT_ENDED;
	}
	uint32_t const again = godbolt_rules[call->kind_of].again;
	bool const retry = r->e ? (r->e == EIO && again & GODBOLT_AGAIN_EIO) ||
	                          (r->e == ETIMEDOUT && again & GODBOLT_AGAIN_TIMEDOUT)
	                        : r->status == 429 || r->status == 502 || r->status == 503 ||
	                          (r->status == 504 && again & GODBOLT_AGAIN_504);
	if (retry && r->retry > GODBOLT_RETRY_MAX) {
		godbolt_why(call, "HTTP %" PRIu32 ", and Retry-After is %" PRIu32 " s",
		            r->status, r->retry);
		return GODBOLT_BAD;
	}
	if (retry && call->attempt < GODBOLT_ATTEMPTS) {
		uint32_t const wait = 1u << (call->attempt - 1);
		return godbolt_later(call, r->retry > wait ? r->retry : wait);
	}
	size_t const max = call->req.max;
	if (r->e == EFBIG)
		godbolt_why(call, "the response is larger than %zu %s",
		            max >= 1 << 20 ? max >> 20 : max >> 10, max >= 1 << 20 ? "MiB" : "KiB");
	else if (r->e)
		godbolt_why(call, "the request failed: %.*s",
		            (int)godbolt_cut(r->error, strnlen(r->error, 201), 200), r->error);
	else if (r->status < 200 || r->status > 299)
		godbolt_reason(call, r);
	else
		return GODBOLT_GO;
	return GODBOLT_BAD;
}

/* Returns the state of the parse of a JSON array after the byte @p c,
 * which is outside an element, in the state @p s.  For a byte that begins
 * an element, the function returns GODBOLT_LIST_VALUE, and the parse of the
 * element begins with that byte. */
static uint8_t
godbolt_array_next (uint8_t s,
                    char    c)
{
	if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
		return s;
	if (s == GODBOLT_LIST_OPEN)
		return c == '[' ? GODBOLT_LIST_FIRST : GODBOLT_LIST_BAD;
	if (c == ']' && s != GODBOLT_LIST_COMMA)
		return GODBOLT_LIST_END;
	if (s == GODBOLT_LIST_NEXT)
		return c == ',' ? GODBOLT_LIST_COMMA : GODBOLT_LIST_BAD;
	return GODBOLT_LIST_VALUE;
}

/*
 * Parses the elements of the JSON array in the @p len bytes at @p body,
 * from call->off on, as far as the bounds of the step allow.  It calls @p fn
 * for each element, and frees the element after that.  The element is
 * nullptr for the JSON value null.  fn returns 0, or an error number that
 * stops the parse.
 *
 * Returns 0, or the error number of fn.  After the end of the array,
 * call->list is GODBOLT_LIST_END.  If the text is not a JSON array, or if
 * an element has more than GODBOLT_ELEMENT_MAX bytes, call->list is
 * GODBOLT_LIST_BAD and call->why has the reason.
 */
static int
godbolt_list_parse (struct godbolt *call,
                    char const     *body,
                    size_t          len,
                    int           (*fn) (struct godbolt     *call,
                                         struct json_object *element))
{
	uint64_t room = loop_room(call->loop, LOOP_PARSE);
	uint64_t const values = loop_room(call->loop, LOOP_VALUES) * GODBOLT_VALUE_BYTES;
	room = room < values ? room : values;
	uint64_t used = 0;
	int e = 0;
	while (!e && call->list < GODBOLT_LIST_END && used < room && call->off < len) {
		if (call->list != GODBOLT_LIST_VALUE) {
			call->list = godbolt_array_next(call->list, body[call->off]);
			call->start = call->off;
			call->off += call->list != GODBOLT_LIST_VALUE;
			used += call->list != GODBOLT_LIST_VALUE;
			if (call->list == GODBOLT_LIST_BAD)
				godbolt_why(call, "the text is not a JSON array");
			continue;
		}
		size_t n = len - call->off;
		n = n < room - used ? n : (size_t)(room - used);
		struct json_object *v = json_tokener_parse_ex(call->tok, body + call->off, (int)n);
		enum json_tokener_error const err = json_tokener_get_error(call->tok);
		size_t const took = err == json_tokener_success ? json_tokener_get_parse_end(call->tok) : n;
		call->off += took;
		used += took;
		if (err != json_tokener_success && err != json_tokener_continue) {
			godbolt_why(call, "%s", json_tokener_error_desc(err));
			call->list = GODBOLT_LIST_BAD;
		} else if (call->off - call->start > GODBOLT_ELEMENT_MAX) {
			godbolt_why(call, "an element has more than 4 KiB");
			call->list = GODBOLT_LIST_BAD;
		} else if (err == json_tokener_success) {
			json_tokener_reset(call->tok);
			call->list = GODBOLT_LIST_NEXT;
			e = fn(call, v);
			/* The free of the element counts as one value for each 2
			 * bytes of the element, which is the most values that it can
			 * have. */
			loop_work(call->loop, LOOP_VALUES, (call->off - call->start) / 2);
		}
		json_object_put(v);
	}
	loop_work(call->loop, LOOP_PARSE, used);
	loop_work(call->loop, LOOP_VALUES, used / GODBOLT_VALUE_BYTES);
	if (!e && call->list < GODBOLT_LIST_END && call->off == len) {
		godbolt_why(call, "the text ends before the end of the JSON array");
		call->list = GODBOLT_LIST_BAD;
	}
	return e;
}

static_assert(GODBOLT_ELEMENT_MAX == 4 << 10, "the reason says 4 KiB");

/* Returns the number of family tags in @p tags, or 0 if @p tags is not an
 * array. */
static size_t
godbolt_tags (struct json_object *tags)
{
	return json_object_is_type(tags, json_type_array) ? json_object_array_length(tags) : 0;
}

/* Returns the family tag @p i of @p tags, or an empty text if the element
 * is not a string. */
static struct tool_text
godbolt_tag (struct json_object *tags,
             size_t              i)
{
	struct json_object *tag = json_object_array_get_idx(tags, i);
	if (!json_object_is_type(tag, json_type_string))
		return (struct tool_text){};
	return (struct tool_text){ json_object_get_string(tag),
	                           (size_t)json_object_get_string_len(tag) };
}

/* Returns whether a compiler with the family tags @p tags has the family
 * @p family.  A compiler without tags has the family unknown. */
static bool
godbolt_tagged (struct json_object *tags,
                struct tool_text    family)
{
	size_t const n = godbolt_tags(tags);
	bool any = false;
	for (size_t i = 0; i < n; ++i) {
		struct tool_text const t = godbolt_tag(tags, i);
		if (t.len && godbolt_same(t, family))
			return true;
		any |= t.len != 0;
	}
	return !any && godbolt_same(family, TOOL_TEXT("unknown"));
}

/* Returns whether an element of a list matches the words of the argument
 * match and the argument ids.  Each word must be in one of the @p n texts
 * of @p f, or in one of the family tags @p tags.  @p f[0] is the id of the
 * element.  If the call has ids, the function sets the bit of the id in
 * call->found. */
static bool
godbolt_matches (struct godbolt         *call,
                 struct tool_text const *f,
                 size_t                  n,
                 struct json_object     *tags)
{
	size_t const n_tags = godbolt_tags(tags);
	for (uint8_t w = 0; w < call->n_words; ++w) {
		bool hit = false;
		for (size_t i = 0; !hit && i < n; ++i)
			hit = godbolt_has(f[i], call->words[w]);
		for (size_t i = 0; !hit && i < n_tags; ++i)
			hit = godbolt_has(godbolt_tag(tags, i), call->words[w]);
		if (!hit)
			return false;
	}
	if (!call->n_ids)
		return true;
	for (uint8_t i = 0; i < call->n_ids; ++i) {
		if (godbolt_same(f[0], call->ids[i])) {
			call->found |= (uint16_t)(1u << i);
			return true;
		}
	}
	return false;
}

/* Writes @p t as a column of a row of the catalog.  A tab, a newline, a
 * carriage return and a backslash become \t, \n, \r and \\.  Returns 0, or
 * call->e. */
static int
godbolt_put_field (struct godbolt   *call,
                   struct tool_text  t)
{
	static char const letter[] = { ['\t'] = 't', ['\n'] = 'n', ['\r'] = 'r', ['\\'] = '\\' };
	if (!t.len)
		return call->e;
	size_t from = 0;
	for (size_t i = 0; i < t.len; ++i) {
		unsigned char const c = (unsigned char)t.ptr[i];
		if (c >= sizeof letter || !letter[c])
			continue;
		godbolt_put(call, (struct tool_text){ t.ptr + from, i - from });
		godbolt_put(call, (struct tool_text){ (char const[]){ '\\', letter[c] }, 2 });
		from = i + 1;
	}
	return godbolt_put(call, (struct tool_text){ t.ptr + from, t.len - from });
}

/* Counts an element of the list of languages, and writes it as a row if the
 * filters select it and the output has fewer rows than the limit.  Returns
 * 0, or call->e. */
static int
godbolt_language_row (struct godbolt     *call,
                      struct json_object *element)
{
	struct tool_text const id = godbolt_member(element, "id");
	if (!id.len) {
		++call->malformed;
		return 0;
	}
	++call->total;
	struct tool_text const f[] = { id, godbolt_member(element, "name") };
	if (!godbolt_matches(call, f, 2, nullptr) || ++call->matching > call->limit)
		return 0;
	++call->listed;
	godbolt_put_field(call, f[0]);
	godbolt_put(call, TOOL_TEXT("\t"));
	godbolt_put_field(call, f[1]);
	return godbolt_put(call, TOOL_TEXT("\n"));
}

/* Adds 1 to the count of @p key in the counts of the catalog.  @p key is
 * NUL-terminated.  If the counts have GODBOLT_KEYS keys and @p key is not
 * one of them, the function sets call->list to GODBOLT_LIST_BAD, which
 * ends the parse of the list, and stores the reason in call->why.
 * Returns 0, or call->e, which is ENOMEM if json-c runs out of memory. */
static int
godbolt_count (struct godbolt   *call,
               struct tool_text  key)
{
	struct json_object *n = nullptr;
	if (json_object_object_get_ex(call->doc, key.ptr, &n)) {
		json_object_int_inc(n, 1);
		return call->e;
	}
	if (json_object_object_length(call->doc) == GODBOLT_KEYS) {
		godbolt_why(call, "the list has more than 256 different %s",
		            godbolt_actions[call->action].ptr);
		call->list = GODBOLT_LIST_BAD;
		return call->e;
	}
	struct json_object *one = json_object_new_int(1);
	if (!one || json_object_object_add(call->doc, key.ptr, one)) {
		json_object_put(one);
		call->e = call->e ? call->e : ENOMEM;
	}
	return call->e;
}

/* Counts each family tag in @p tags once, or the family unknown if there is
 * no tag.  @p tags has at most GODBOLT_TAGS elements.  Returns 0, or
 * call->e. */
static int
godbolt_count_families (struct godbolt     *call,
                        struct json_object *tags)
{
	size_t const n = godbolt_tags(tags);
	bool any = false;
	for (size_t i = 0; i < n; ++i) {
		struct tool_text const t = godbolt_tag(tags, i);
		bool seen = !t.len;
		for (size_t k = 0; !seen && k < i; ++k)
			seen = godbolt_same(t, godbolt_tag(tags, k));
		if (!seen)
			godbolt_count(call, t);
		any |= t.len != 0;
	}
	return any ? call->e : godbolt_count(call, TOOL_TEXT("unknown"));
}

/* Returns whether a compiler has a kind @p kind and an architecture
 * @p architecture of at most GODBOLT_VALUE_MAX bytes, and at most
 * GODBOLT_TAGS family tags @p tags of at most GODBOLT_VALUE_MAX bytes. */
static bool
godbolt_compiler_valid (struct tool_text    kind,
                        struct tool_text    architecture,
                        struct json_object *tags)
{
	size_t const n = godbolt_tags(tags);
	bool valid = n <= GODBOLT_TAGS && kind.len <= GODBOLT_VALUE_MAX &&
	             architecture.len <= GODBOLT_VALUE_MAX;
	for (size_t i = 0; valid && i < n; ++i)
		valid = godbolt_tag(tags, i).len <= GODBOLT_VALUE_MAX;
	return valid;
}

/* Counts an element of a list of compilers.  If the filters select the
 * compiler, the function writes the compiler as a row, or it counts the
 * compiler's kind, family tags or architecture.  An element without an id
 * is left out, and so is an element for which godbolt_compiler_valid()
 * returns false.  Returns 0, or call->e. */
static int
godbolt_compiler_row (struct godbolt     *call,
                      struct json_object *element)
{
	struct json_object *tags = nullptr;
	(void)json_object_object_get_ex(element, "compilerCategories", &tags);
	struct tool_text const f[] = {
		godbolt_member(element, "id"),
		godbolt_member(element, "name"),
		godbolt_or(godbolt_member(element, "compilerType"), TOOL_TEXT("default")),
		godbolt_member(element, "semver"),
		godbolt_or(godbolt_member(element, "instructionSet"), TOOL_TEXT("unknown")),
		godbolt_member(element, "releaseTrack"),
	};
	if (!f[0].len || !godbolt_compiler_valid(f[2], f[4], tags)) {
		++call->malformed;
		return 0;
	}
	++call->total;
	if ((call->kind.len && !godbolt_same(f[2], call->kind)) ||
	    (call->architecture.len && !godbolt_same(f[4], call->architecture)) ||
	    (call->family.len && !godbolt_tagged(tags, call->family)) ||
	    !godbolt_matches(call, f, sizeof f / sizeof *f, tags))
		return 0;
	++call->matching;
	if (call->action == GODBOLT_KINDS || call->action == GODBOLT_ARCHITECTURES)
		return godbolt_count(call, f[call->action == GODBOLT_KINDS ? 2 : 4]);
	if (call->action == GODBOLT_FAMILIES)
		return godbolt_count_families(call, tags);
	if (call->listed == call->limit)
		return 0;
	++call->listed;
	for (size_t i = 0; i < sizeof f / sizeof *f; ++i) {
		godbolt_put_field(call, f[i]);
		godbolt_put(call, TOOL_TEXT("\t"));
	}
	size_t const n = godbolt_tags(tags);
	struct tool_text sep = {};
	for (size_t i = 0; i < n; ++i) {
		struct tool_text const t = godbolt_tag(tags, i);
		if (!t.len)
			continue;
		godbolt_put(call, sep);
		godbolt_put_field(call, t);
		sep = TOOL_TEXT(",");
	}
	return godbolt_put(call, sep.len ? TOOL_TEXT("\n") : TOOL_TEXT("unknown\n"));
}

/* A key of the counts of the catalog, and its count. */
struct godbolt_tally {
	char const *key;
	int64_t     count;
};

/* Compares the keys of two struct godbolt_tally byte by byte, for
 * qsort(). */
static int
godbolt_tally_cmp (void const *a,
                   void const *b)
{
	return strcmp(((struct godbolt_tally const *)a)->key,
	              ((struct godbolt_tally const *)b)->key);
}

/* Writes the counts of the catalog in the byte order of their keys, as
 * many as the limit allows, and stores the number of rows in call->listed.
 * The counts have at most GODBOLT_KEYS keys of at most GODBOLT_VALUE_MAX
 * bytes, so one step writes them.  Returns 0, or call->e, which is ENOMEM
 * if memory runs out. */
static int
godbolt_tallies (struct godbolt *call)
{
	size_t const n = (size_t)json_object_object_length(call->doc);
	struct godbolt_tally *t = n ? malloc(n * sizeof *t) : nullptr;
	if (n && !t) {
		call->e = call->e ? call->e : ENOMEM;
		return call->e;
	}
	size_t i = 0;
	struct json_object_iter it;
	json_object_object_foreachC(call->doc, it)
		t[i++] = (struct godbolt_tally){ it.key, json_object_get_int64(it.val) };
	if (n)
		qsort(t, n, sizeof *t, godbolt_tally_cmp);
	for (i = 0; i < n && i < call->limit; ++i) {
		godbolt_put_field(call, (struct tool_text){ t[i].key, strlen(t[i].key) });
		godbolt_putf(call, "\t%" PRId64 "\n", t[i].count);
	}
	free(t);
	t = nullptr;
	call->listed = (uint32_t)i;
	return call->e;
}

/* Writes the ids of the call that are not in the list to @p d, separated by
 * ", ", and a NUL.  @p d has room for GODBOLT_IDS * (GODBOLT_ID_MAX + 2) + 1
 * bytes. */
static void
godbolt_absent (struct godbolt const *call,
                char                 *d)
{
	char *p = d;
	for (uint8_t i = 0; i < call->n_ids; ++i) {
		if (call->found >> i & 1)
			continue;
		p = godbolt_cat(p, p == d ? (struct tool_text){} : TOOL_TEXT(", "));
		p = godbolt_cat(p, call->ids[i]);
	}
	*p = '\0';
}

/* Ends a call of godbolt_catalog after the end of the list.  If an id of
 * the argument ids is not in the list, the call fails.  Otherwise the
 * function writes the counts of an action that counts, a line on the
 * elements that the plugin left out, and the last line of the output, which
 * has the numbers of the items.  Returns GODBOLT_ENDED. */
static enum godbolt_go
godbolt_catalog_end (struct godbolt *call)
{
	if (call->found != (1u << call->n_ids) - 1) {
		char absent[GODBOLT_IDS * (GODBOLT_ID_MAX + 2) + 1];
		godbolt_absent(call, absent);
		return call->action == GODBOLT_LANGUAGES
		     ? godbolt_refuse(call, "These ids are not in the list of languages, or the "
		                      "filters exclude them: %s.", absent)
		     : godbolt_refuse(call, "These ids are not in the list of language %.*s, or "
		                      "the filters exclude them: %s.", (int)call->language.len,
		                      call->language.ptr, absent);
	}
	uint32_t const keys = call->doc ? (uint32_t)json_object_object_length(call->doc) : 0;
	if (call->action > GODBOLT_COMPILERS)
		godbolt_tallies(call);
	if (call->malformed && call->action == GODBOLT_LANGUAGES)
		godbolt_putf(call, "The plugin left out %" PRIu32 " element%s of the list. An "
		             "element must have a string member id.\n", call->malformed,
		             godbolt_s(call->malformed));
	else if (call->malformed)
		godbolt_putf(call, "The plugin left out %" PRIu32 " element%s of the list. An "
		             "element must have a string member id, at most 64 family tags, "
		             "and at most 128 bytes in its kind, its architecture and each "
		             "family tag.\n", call->malformed, godbolt_s(call->malformed));
	if (call->action == GODBOLT_LANGUAGES)
		godbolt_putf(call, "Listed %" PRIu32 " of %" PRIu32 " matching language%s. "
		             "Compiler Explorer has %" PRIu32 " language%s.\n", call->listed,
		             call->matching, godbolt_s(call->matching), call->total,
		             godbolt_s(call->total));
	else if (call->action == GODBOLT_COMPILERS)
		godbolt_putf(call, "Listed %" PRIu32 " of %" PRIu32 " matching compiler%s. The "
		             "list of language %.*s has %" PRIu32 " compiler%s.\n", call->listed,
		             call->matching, godbolt_s(call->matching), (int)call->language.len,
		             call->language.ptr, call->total, godbolt_s(call->total));
	else
		godbolt_putf(call, "Listed %" PRIu32 " of the %" PRIu32 " %s of the %" PRIu32
		             " matching compiler%s. The list of language %.*s has %" PRIu32
		             " compiler%s.\n", call->listed, keys, keys == 1
		             ? godbolt_singulars[call->action] : godbolt_actions[call->action].ptr,
		             call->matching, godbolt_s(call->matching), (int)call->language.len,
		             call->language.ptr, call->total, godbolt_s(call->total));
	if (call->e)
		return godbolt_fail(call);
	mcp_slot_finish(call->slot, 0);
	return GODBOLT_ENDED;
}

static_assert(GODBOLT_TAGS == 64 && GODBOLT_VALUE_MAX == 128 && GODBOLT_KEYS == 256,
              "the texts of the catalog have these numbers");

/* Stores the kind of driver of a compiler of the call, if the element of
 * the list of compilers is one of them.  When the list has had each
 * compiler of the call, the function sets call->list to GODBOLT_LIST_END,
 * which ends the parse.  Returns 0. */
static int
godbolt_driver_row (struct godbolt     *call,
                    struct json_object *element)
{
	struct tool_text const id = godbolt_member(element, "id");
	struct tool_text const kind = godbolt_member(element, "compilerType");
	for (uint8_t i = 0; i < call->n_ids; ++i) {
		if (call->found >> i & 1 || !godbolt_same(id, call->ids[i]))
			continue;
		call->found |= (uint16_t)(1u << i);
		call->drivers[i] = godbolt_same(kind, TOOL_TEXT("win32-vc")) ? GODBOLT_MSVC
		                 : godbolt_same(kind, TOOL_TEXT("clang-cl")) ? GODBOLT_CLANG_CL
		                 : GODBOLT_GNU;
	}
	if (call->found == (1u << call->n_ids) - 1)
		call->list = GODBOLT_LIST_END;
	return 0;
}

/* Ends the parse of the list of compilers of a tool that compiles.  If a
 * compiler of the call is not in the list, the call fails.  Otherwise the
 * next step begins the first compilation. */
static enum godbolt_go
godbolt_drivers_end (struct godbolt *call)
{
	if (call->found != (1u << call->n_ids) - 1) {
		char absent[GODBOLT_IDS * (GODBOLT_ID_MAX + 2) + 1];
		godbolt_absent(call, absent);
		return godbolt_refuse(call, "These compiler ids are not in the list of language "
		                      "%.*s: %s. godbolt_catalog lists the ids.",
		                      (int)call->language.len, call->language.ptr, absent);
	}
	call->phase = GODBOLT_JOB;
	return GODBOLT_GO;
}

/* Sends the request of the list of languages, or of the list of compilers
 * of the call's language.  The next step reads the response.  Returns
 * GODBOLT_WAIT, or GODBOLT_ENDED if the call failed. */
static enum godbolt_go
godbolt_list_send (struct godbolt *call)
{
	char *u = godbolt_cat(call->url, (struct tool_text){ call->cfg->url, call->cfg->len });
	if (call->action == GODBOLT_LANGUAGES) {
		u = godbolt_cat(u, TOOL_TEXT("/api/languages"));
	} else {
		u = godbolt_cat(u, TOOL_TEXT("/api/compilers/"));
		u = godbolt_cat_path(u, call->language);
		u = godbolt_cat(u, TOOL_TEXT("?fields=" GODBOLT_FIELDS));
	}
	*u = '\0';
	godbolt_request(call, call->action == GODBOLT_LANGUAGES ? GODBOLT_GET_LANGUAGES
	                                                        : GODBOLT_GET_COMPILERS,
	                nullptr, 0);
	call->phase = GODBOLT_LIST;
	enum godbolt_go const r = godbolt_send(call);
	return r != GODBOLT_BAD ? r : godbolt_refuse(call, "askai could not get the list from "
	                                             "Compiler Explorer (%s).", call->why);
}

/* The phase GODBOLT_LIST.  It reads the response to the request of the
 * list.  For godbolt_catalog, it writes the first line of the output. */
static enum godbolt_go
godbolt_list (struct godbolt *call)
{
	struct web_response r;
	enum godbolt_go const got = godbolt_result(call, &r);
	if (got == GODBOLT_BAD)
		return godbolt_refuse(call, "askai could not get the list of %s from "
		                      "Compiler Explorer (%s).", call->action == GODBOLT_LANGUAGES
		                      ? "languages" : "compilers", call->why);
	if (got != GODBOLT_GO)
		return got;
	json_tokener_reset(call->tok);
	call->off = 0;
	call->list = GODBOLT_LIST_OPEN;
	call->phase = GODBOLT_ROWS;
	if (call->tool != GODBOLT_CATALOG)
		return GODBOLT_GO;
	if (call->action > GODBOLT_COMPILERS && !(call->doc = json_object_new_object())) {
		mcp_slot_fail(call->slot, ENOMEM);
		return GODBOLT_ENDED;
	}
	return godbolt_put(call, godbolt_headers[call->action]) ? godbolt_fail(call) : GODBOLT_GO;
}

/* The phase GODBOLT_ROWS.  It parses the next elements of the list.  After
 * the end of the list, godbolt_catalog ends the call, and the other tools
 * begin to compile. */
static enum godbolt_go
godbolt_rows (struct godbolt *call)
{
	struct web_response r;
	if (mcp_slot_fetched(call->slot, &r) || (!r.body && r.len))
		return godbolt_refuse(call, "askai has no response to the request of the list.");
	bool const catalog = call->tool == GODBOLT_CATALOG;
	if (godbolt_list_parse(call, r.body, r.len, !catalog ? godbolt_driver_row
	                                            : call->action == GODBOLT_LANGUAGES
	                                            ? godbolt_language_row : godbolt_compiler_row))
		return godbolt_fail(call);
	if (call->list == GODBOLT_LIST_BAD)
		return godbolt_refuse(call, "Compiler Explorer sent a list that is not valid: %s.",
		                      call->why);
	if (call->list != GODBOLT_LIST_END)
		return GODBOLT_GO;
	return catalog ? godbolt_catalog_end(call) : godbolt_drivers_end(call);
}

/* The phase GODBOLT_ESCAPE.  It escapes the next slice of the source into
 * the body of the requests, after {"source":".  The text up to the end of
 * the source is the same for every compilation.  The first step allocates
 * the body with room for an escape of 6 bytes for each byte of the source,
 * so that no step copies the body to a larger allocation. */
static enum godbolt_go
godbolt_escape_source (struct godbolt *call)
{
	struct tool_text const prefix = TOOL_TEXT("{\"source\":\"");
	if (!call->body) {
		call->cap = prefix.len + 6 * call->source.len + GODBOLT_TAIL_MAX;
		call->body = malloc(call->cap);
		if (!call->body) {
			mcp_slot_fail(call->slot, ENOMEM);
			return GODBOLT_ENDED;
		}
		call->head = (size_t)(godbolt_cat(call->body, prefix) - call->body);
	}
	uint64_t const room = loop_room(call->loop, LOOP_ESCAPE);
	size_t n = call->source.len - call->off;
	n = n < room ? n : (size_t)room;
	char *end = godbolt_escape(call->body + call->head, call->source.ptr + call->off, n);
	call->head = (size_t)(end - call->body);
	call->off += n;
	loop_work(call->loop, LOOP_ESCAPE, n);
	if (call->off < call->source.len)
		return GODBOLT_GO;
	if (call->head > WEB_SEND_MAX - GODBOLT_TAIL_MAX)
		return godbolt_refuse(call, "The body of a request would be longer than 4 MiB, "
		                      "because the source has %zu bytes as a JSON string.",
		                      call->head - prefix.len);
	if (call->tool != GODBOLT_COMPILE)
		return godbolt_list_send(call);
	call->phase = GODBOLT_JOB;
	return GODBOLT_GO;
}

static_assert(WEB_SEND_MAX == 4 << 20, "the reason says 4 MiB");

/* Returns the recipe of the compilation call->job. */
static struct godbolt_recipe const *
godbolt_job_recipe (struct godbolt const *call)
{
	return &godbolt_recipes[call->tool][call->drivers[call->job / call->n_args]];
}

/* Writes the part of the body of the request of the compilation call->job
 * that follows the source: the language, the arguments, the filters and
 * the compiler options.  The arguments are the flags of the recipe and the
 * argument string, separated by a space.  Stores in call->arg_at and
 * call->arg_len where the escaped argument string is in the body. */
static void
godbolt_job_body (struct godbolt *call)
{
	struct godbolt_recipe const *r = godbolt_job_recipe(call);
	struct tool_text const arg = call->args[call->job % call->n_args];
	char *d = godbolt_cat(call->body + call->head, TOOL_TEXT("\",\"lang\":\""));
	d = godbolt_cat(d, call->language);
	d = godbolt_cat(d, TOOL_TEXT("\",\"options\":{\"userArguments\":\""));
	char const *const flags = d;
	if (r->windows)
		d = godbolt_cat(d, godbolt_same(call->language, TOOL_TEXT("c")) ? TOOL_TEXT("/TC")
		                                                                : TOOL_TEXT("/TP"));
	d = godbolt_cat(d, d > flags && r->flags.len ? TOOL_TEXT(" ") : (struct tool_text){});
	d = godbolt_cat(d, r->flags);
	d = godbolt_cat(d, d > flags && arg.len ? TOOL_TEXT(" ") : (struct tool_text){});
	call->arg_at = (size_t)(d - call->body);
	d = godbolt_escape(d, arg.ptr, arg.len);
	call->arg_len = (size_t)(d - call->body) - call->arg_at;
	d = godbolt_cat(d, TOOL_TEXT("\",\"filters\":"));
	d = godbolt_cat_filters(d, call->tool == GODBOLT_COMPILE ? call->filters : 0);
	d = godbolt_cat(d, TOOL_TEXT(",\"compilerOptions\":"));
	d = godbolt_cat(d, r->options);
	d = godbolt_cat(d, TOOL_TEXT("}}"));
	call->len = (size_t)(d - call->body);
	loop_work(call->loop, LOOP_ESCAPE, arg.len);
}

/* Writes the first line of the record of the compilation call->job, with
 * the status in call->why.  Returns 0, or call->e. */
static int
godbolt_header (struct godbolt *call)
{
	godbolt_put(call, TOOL_TEXT("=== "));
	godbolt_put(call, call->ids[call->job / call->n_args]);
	godbolt_put(call, TOOL_TEXT(" \""));
	godbolt_put(call, (struct tool_text){ call->body + call->arg_at, call->arg_len });
	godbolt_put(call, TOOL_TEXT("\": "));
	godbolt_put(call, (struct tool_text){ call->why, call->why_len });
	call->headed = 1;
	return godbolt_put(call, TOOL_TEXT(" ===\n"));
}

/* Makes the next step begin the next compilation. */
static void
godbolt_next (struct godbolt *call)
{
	++call->job;
	call->phase = GODBOLT_JOB;
	call->sec = 0;
	call->item = 0;
	call->byte = 0;
	call->opened = 0;
	call->headed = 0;
	call->marked = 0;
	call->cut = 0;
	call->truncated = 0;
	call->n_sec = 0;
	call->no_pp = 0;
	call->probed = 0;
	godbolt_probe_fini(&call->probe);
}

/* Writes the record of the compilation call->job, which has no response,
 * with the status in call->why, and counts it as @p count.  Then the next
 * step begins the next compilation. */
static enum godbolt_go
godbolt_record (struct godbolt       *call,
                enum godbolt_outcome  count)
{
	++call->counts[count];
	if (godbolt_header(call))
		return godbolt_fail(call);
	godbolt_next(call);
	return GODBOLT_GO;
}

/* Writes the last line of the output, and ends the call.  The call fails
 * if no compilation has a result.  Returns GODBOLT_ENDED. */
static enum godbolt_go
godbolt_end (struct godbolt *call)
{
	uint32_t const *c = call->counts;
	uint32_t const jobs = (uint32_t)call->n_ids * call->n_args;
	if (call->tool == GODBOLT_TARGETS)
		godbolt_put(call, TOOL_TEXT("The facts come from predefined macros. data_model "
		                            "describes only the sizes of int, long and "
		                            "pointers.\n"));
	godbolt_putf(call, "Of %" PRIu32 " compilation%s, %" PRIu32 " ended with exit code "
	             "0, %" PRIu32 " ended with another exit code or a timeout, %" PRIu32
	             " ended with a failed request, and %" PRIu32 " ended without a "
	             "request.\n", jobs, godbolt_s(jobs), c[GODBOLT_ZERO], c[GODBOLT_OTHER],
	             c[GODBOLT_FAILED], c[GODBOLT_UNSENT]);
	if (call->e)
		return godbolt_fail(call);
	mcp_slot_finish(call->slot, c[GODBOLT_ZERO] || c[GODBOLT_OTHER] ? 0 : TOOL_FAILED);
	return GODBOLT_ENDED;
}

/* The phase GODBOLT_JOB.  It sends the request of the compilation
 * call->job, or it writes the record of a compilation that it does not
 * send.  After the last compilation, it ends the call. */
static enum godbolt_go
godbolt_job (struct godbolt *call)
{
	if (call->job == call->n_ids * call->n_args)
		return godbolt_end(call);
	godbolt_job_body(call);
	if (call->full) {
		godbolt_why(call, "not sent, because the output is full");
		return godbolt_record(call, GODBOLT_UNSENT);
	}
	if (godbolt_job_recipe(call)->from == GODBOLT_FROM_NONE) {
		godbolt_why(call, "not sent, because MSVC cannot output its predefined macros");
		return godbolt_record(call, GODBOLT_UNSENT);
	}
	char *u = godbolt_cat(call->url, (struct tool_text){ call->cfg->url, call->cfg->len });
	u = godbolt_cat(u, TOOL_TEXT("/api/compiler/"));
	u = godbolt_cat_path(u, call->ids[call->job / call->n_args]);
	*godbolt_cat(u, TOOL_TEXT("/compile")) = '\0';
	godbolt_request(call, GODBOLT_POST_COMPILE, call->body, call->len);
	call->phase = GODBOLT_ANSWER;
	enum godbolt_go const r = godbolt_send(call);
	return r == GODBOLT_BAD ? godbolt_record(call, GODBOLT_FAILED) : r;
}

/* The phase GODBOLT_ANSWER.  It reads the response to the compilation, and
 * it begins the parse of a response with a status from 200 to 299. */
static enum godbolt_go
godbolt_answer (struct godbolt *call)
{
	struct web_response r;
	enum godbolt_go const got = godbolt_result(call, &r);
	if (got == GODBOLT_BAD)
		return godbolt_record(call, GODBOLT_FAILED);
	if (got != GODBOLT_GO)
		return got;
	json_tokener_reset(call->tok);
	call->off = 0;
	call->phase = GODBOLT_PARSE;
	return GODBOLT_GO;
}

/* Returns the member @p name of @p obj, or nullptr if @p obj has no such
 * member. */
static struct json_object *
godbolt_get (struct json_object *obj,
             char const         *name)
{
	struct json_object *v = nullptr;
	return json_object_object_get_ex(obj, name, &v) ? v : nullptr;
}

/* Adds the section @p sec, whose text is @p v, to the record. */
static void
godbolt_section_add (struct godbolt     *call,
                     struct json_object *v,
                     enum godbolt_sec    sec)
{
	call->secs[call->n_sec] = v;
	call->sec_of[call->n_sec++] = sec;
}

/* Stores in call->secs the texts of the sections of the record, where the
 * recipe of the compilation has them.  A record of a recipe that
 * preprocesses gets a note if the response has no text of the
 * preprocessor, and a note on a cut if the preprocessor left out lines. */
static void
godbolt_sections_find (struct godbolt *call)
{
	uint32_t const from = godbolt_job_recipe(call)->from;
	enum godbolt_sec const main = call->tool == GODBOLT_MACROS ? GODBOLT_SEC_MACROS
	                                                               : GODBOLT_SEC_PP;
	struct json_object *pp = godbolt_get(call->doc, "ppOutput");
	struct json_object *text = godbolt_get(pp, "output");
	call->n_sec = 0;
	if (from == GODBOLT_FROM_ASM)
		godbolt_section_add(call, godbolt_get(call->doc, "asm"), GODBOLT_SEC_ASM);
	if (from == GODBOLT_FROM_PP)
		godbolt_section_add(call, text, main);
	godbolt_section_add(call, godbolt_get(call->doc, "stdout"),
	                    from == GODBOLT_FROM_STDOUT ? main : GODBOLT_SEC_STDOUT);
	godbolt_section_add(call, godbolt_get(call->doc, "stderr"), GODBOLT_SEC_STDERR);
	call->no_pp = from == GODBOLT_FROM_PP && !json_object_is_type(text, json_type_string);
	call->truncated |= from == GODBOLT_FROM_PP &&
	                   json_object_get_int64(godbolt_get(pp, "numberOfLinesFiltered"));
}

/* Returns the member text of each element of the array @p v, each with a
 * newline after it, and stores the length in @p *len.  The caller frees
 * the text.  On failure it returns nullptr and stores ENOMEM in @p *e. */
static char *
godbolt_join (struct godbolt     *call,
              struct json_object *v,
              size_t             *len,
              int                *e)
{
	size_t const n = json_object_array_length(v);
	size_t bytes = 1;
	for (size_t i = 0; i < n; ++i)
		bytes += godbolt_member(json_object_array_get_idx(v, i), "text").len + 1;
	char *text = malloc(bytes);
	if (!text) {
		*e = ENOMEM;
		return nullptr;
	}
	char *d = text;
	for (size_t i = 0; i < n; ++i) {
		d = godbolt_cat(d, godbolt_member(json_object_array_get_idx(v, i), "text"));
		*d++ = '\n';
	}
	*len = (size_t)(d - text);
	loop_work(call->loop, LOOP_COPY, *len);
	*e = 0;
	return text;
}

/* Parses the output of the probe of a call of godbolt_targets, which is the
 * main text of the record: a string, or the lines of stdout.  If the output
 * has a valid block, the record has the facts and the messages of the
 * compiler.  Otherwise the status of the record has the reason, and the
 * record has the output.  The function parses the whole output in one
 * step.  Returns 0, or ENOMEM. */
static int
godbolt_targets_parse (struct godbolt *call)
{
	struct json_object *v = call->secs[0];
	struct tool_text text = {};
	char *joined = nullptr;
	if (json_object_is_type(v, json_type_string)) {
		text = (struct tool_text){ json_object_get_string(v),
		                           (size_t)json_object_get_string_len(v) };
	} else if (json_object_is_type(v, json_type_array)) {
		int e;
		joined = godbolt_join(call, v, &text.len, &e);
		if (!joined)
			return e;
		text.ptr = joined;
	}
	int const e = godbolt_probe_init(&call->probe, text.ptr, text.len);
	free(joined);
	joined = nullptr;
	loop_work(call->loop, LOOP_PARSE, text.len);
	if (e == ENOMEM)
		return ENOMEM;
	if (e) {
		godbolt_why(call, "the probe output is not valid: %s", call->probe.why);
		return 0;
	}
	call->probed = 1;
	call->n_sec = 0;
	godbolt_section_add(call, godbolt_get(call->doc, "stderr"), GODBOLT_SEC_STDERR);
	return 0;
}

/* Reads the parsed response to a compilation, and stores its status in
 * call->why and the texts of its sections in call->secs.  The next step
 * writes the record. */
static enum godbolt_go
godbolt_classify (struct godbolt *call)
{
	struct json_object *code = godbolt_get(call->doc, "code");
	struct json_object *timed_out = godbolt_get(call->doc, "timedOut");
	struct json_object *truncated = godbolt_get(call->doc, "truncated");
	if (!json_object_is_type(code, json_type_int)) {
		godbolt_why(call, "the response is not a JSON object with an integer member "
		            "code");
		++call->counts[GODBOLT_FAILED];
		call->phase = GODBOLT_FREE;
		return godbolt_header(call) ? godbolt_fail(call) : GODBOLT_GO;
	}
	bool const timed = json_object_is_type(timed_out, json_type_boolean) &&
	                   json_object_get_boolean(timed_out);
	call->truncated = json_object_is_type(truncated, json_type_boolean) &&
	                  json_object_get_boolean(truncated);
	int64_t const n = json_object_get_int64(code);
	if (timed)
		godbolt_why(call, "timed out on Compiler Explorer");
	else
		godbolt_why(call, "exit code %" PRId64, n);
	++call->counts[!n && !timed ? GODBOLT_ZERO : GODBOLT_OTHER];
	godbolt_sections_find(call);
	call->phase = GODBOLT_WRITE;
	if (call->tool != GODBOLT_TARGETS || n || timed)
		return GODBOLT_GO;
	int const e = godbolt_targets_parse(call);
	if (!e)
		return GODBOLT_GO;
	mcp_slot_fail(call->slot, e);
	return GODBOLT_ENDED;
}

/* The phase GODBOLT_PARSE.  It parses the next slice of the response to
 * the compilation.  A response that is not valid JSON gets a record with
 * the reason. */
static enum godbolt_go
godbolt_parse (struct godbolt *call)
{
	struct web_response r;
	if (mcp_slot_fetched(call->slot, &r) || (!r.body && r.len)) {
		godbolt_why(call, "askai has no response to the request");
		return godbolt_record(call, GODBOLT_FAILED);
	}
	uint64_t room = loop_room(call->loop, LOOP_PARSE);
	uint64_t const values = loop_room(call->loop, LOOP_VALUES) * GODBOLT_VALUE_BYTES;
	room = room < values ? room : values;
	size_t n = r.len - call->off;
	n = n < room ? n : (size_t)room;
	if (!n && call->off < r.len)
		return GODBOLT_GO;
	struct json_object *doc = n ? json_tokener_parse_ex(call->tok, r.body + call->off, (int)n)
	                            : nullptr;
	enum json_tokener_error const err = n ? json_tokener_get_error(call->tok)
	                                      : json_tokener_continue;
	loop_work(call->loop, LOOP_PARSE, n);
	loop_work(call->loop, LOOP_VALUES, n / GODBOLT_VALUE_BYTES);
	call->off += n;
	if (err == json_tokener_continue && call->off < r.len)
		return GODBOLT_GO;
	if (err == json_tokener_continue)
		godbolt_why(call, "the response is not valid: the JSON text ends early");
	else if (err != json_tokener_success)
		godbolt_why(call, "the response is not valid: %s", json_tokener_error_desc(err));
	if (err != json_tokener_success)
		return godbolt_record(call, GODBOLT_FAILED);
	call->doc = doc;
	return godbolt_classify(call);
}

/* Ends the record at the limit of the output, with a note.  Later
 * compilations are not sent.  Returns 0, or call->e. */
static int
godbolt_full (struct godbolt *call)
{
	call->full = 1;
	return godbolt_put(call, TOOL_TEXT("\n[The output is cut here, because it has "
	                                   "reached 14 MiB.]\n"));
}

static_assert(GODBOLT_FULL == 14 << 20, "the text of the note has 14 MiB");

/* Writes as much of the @p len bytes at @p s, from call->byte on, as the
 * bounds of the step and the room left in the output allow, and advances
 * call->byte.  If the output has no room left, or room only for the first
 * bytes of a UTF-8 character, the function ends the record with the note
 * of godbolt_full().  Returns 0, or call->e. */
static int
godbolt_slice (struct godbolt *call,
               char const     *s,
               size_t          len)
{
	while (call->byte < len && !call->e) {
		uint64_t const room = loop_room(call->loop, LOOP_COPY);
		size_t const left = call->written < GODBOLT_FULL ? GODBOLT_FULL - call->written : 0;
		size_t n = len - call->byte;
		n = n < room ? n : (size_t)room;
		n = n < left ? n : left;
		if (!left)
			return godbolt_full(call);
		if (!n)
			return call->e;
		size_t taken = 0;
		call->e = mcp_slot_write(call->slot, (struct tool_text){ s + call->byte, n },
		                         call->byte + n < len ? TOOL_MORE : 0, &taken);
		call->written += taken;
		call->byte += taken;
		if (!taken)
			return n < left ? call->e : godbolt_full(call);
	}
	return call->e;
}

/* Writes as much of the string @p v of @p len bytes as godbolt_slice()
 * allows, and a newline after the end of a string that has none.  Returns
 * 0, or call->e. */
static int
godbolt_section_text (struct godbolt     *call,
                      struct json_object *v,
                      size_t              len)
{
	char const *s = json_object_get_string(v);
	if (godbolt_slice(call, s, len) || call->full || call->byte < len)
		return call->e;
	call->marked |= len >= sizeof "\n[Truncated]" - 1 &&
	                !memcmp(s + len - (sizeof "\n[Truncated]" - 1), "\n[Truncated]",
	                        sizeof "\n[Truncated]" - 1);
	return s[len - 1] == '\n' ? call->e : godbolt_put(call, TOOL_TEXT("\n"));
}

/* Writes as many lines of the JSON array @p v of @p n lines as the bounds
 * of the step allow, each with a newline after it.  Each line is the member
 * text of an element, and godbolt_slice() writes it, so that call->item is
 * the line and call->byte the byte of the line.  The lookups of a line
 * count as one value of LOOP_VALUES work.  Returns 0, or call->e. */
static int
godbolt_section_lines (struct godbolt     *call,
                       struct json_object *v,
                       size_t              n)
{
	while (call->item < n && !call->e && !call->full) {
		if (call->written >= GODBOLT_FULL)
			return godbolt_full(call);
		if (!loop_room(call->loop, LOOP_COPY) || !loop_room(call->loop, LOOP_VALUES))
			return call->e;
		loop_work(call->loop, LOOP_VALUES, 1);
		struct json_object *element = json_object_array_get_idx(v, call->item);
		struct tool_text const line = godbolt_member(element, "text");
		if (godbolt_slice(call, line.ptr, line.len) || call->full || call->byte < line.len)
			return call->e;
		godbolt_put(call, TOOL_TEXT("\n"));
		call->marked |= godbolt_same(line, TOOL_TEXT("[truncated; too many lines]")) ||
		                (call->item + 1 == n && godbolt_same(line, TOOL_TEXT("[Truncated]")));
		++call->item;
		call->byte = 0;
	}
	return call->e;
}

/* Writes as much of the section call->sec of the record as the bounds of
 * the step allow.  A section whose text is empty is left out.  After the
 * end of the section, call->sec is the next section.  Returns 0, or
 * call->e. */
static int
godbolt_section (struct godbolt *call)
{
	struct json_object *v = call->secs[call->sec];
	bool const text = json_object_is_type(v, json_type_string);
	size_t const n = text ? (size_t)json_object_get_string_len(v)
	               : json_object_is_type(v, json_type_array) ? json_object_array_length(v)
	               : 0;
	if (n && !call->opened)
		godbolt_put(call, godbolt_sections[call->sec_of[call->sec]]);
	call->opened = 1;
	if (n && (text ? godbolt_section_text(call, v, n) : godbolt_section_lines(call, v, n)))
		return call->e;
	if (call->full || (text ? call->byte : call->item) < n)
		return call->e;
	if (call->marked)
		godbolt_put(call, TOOL_TEXT("[Compiler Explorer cut this output.]\n"));
	call->cut |= call->marked;
	call->marked = 0;
	++call->sec;
	call->opened = 0;
	call->item = 0;
	call->byte = 0;
	return call->e;
}

/* Writes @p s, a NUL-terminated text of the probe, to the call's output.
 * Returns 0, or call->e. */
static int
godbolt_puts (struct godbolt *call,
              char const     *s)
{
	return godbolt_put(call, (struct tool_text){ s, strlen(s) });
}

/* Writes the facts about the target that follow from the output of the
 * probe, one to a line, and the versions of the compiler.  With the
 * argument headers, it also writes which target headers the compiler has.
 * Returns 0, or call->e. */
static int
godbolt_facts (struct godbolt *call)
{
	struct tool_text const names[GODBOLT_PROBE_FACTS] = {
		TOOL_TEXT("architecture: "), TOOL_TEXT("os: "), TOOL_TEXT("os_family: "),
		TOOL_TEXT("data_model: "), TOOL_TEXT("pointer_bits: "), TOOL_TEXT("endianness: "),
		TOOL_TEXT("libc: ")
	};
	struct tool_text const versions[] = {
		TOOL_TEXT("__clang_major__"), TOOL_TEXT("__GNUC__"), TOOL_TEXT("__GNUC_MINOR__"),
		TOOL_TEXT("_MSC_VER"), TOOL_TEXT("__STDC_VERSION__"), TOOL_TEXT("__cplusplus")
	};
	struct godbolt_probe const *p = &call->probe;
	for (size_t i = 0; i < GODBOLT_PROBE_FACTS; ++i) {
		struct godbolt_probe_claim const *c = &p->facts[i];
		godbolt_put(call, names[i]);
		if (c->conflict)
			godbolt_put(call, TOOL_TEXT("conflicting"));
		else if (i == GODBOLT_PROBE_POINTER_BITS && p->pointer_bits)
			godbolt_putf(call, "%" PRIu64, p->pointer_bits);
		else if (c->value)
			godbolt_puts(call, c->value);
		else
			godbolt_put(call, TOOL_TEXT("unknown"));
		for (uint32_t k = 0; k < c->n; ++k) {
			godbolt_put(call, k ? TOOL_TEXT(", ") : TOOL_TEXT(" (from "));
			godbolt_puts(call, c->evidence[k]);
		}
		godbolt_put(call, c->n ? TOOL_TEXT(")\n") : TOOL_TEXT("\n"));
	}
	bool any = false;
	for (size_t i = 0; i < sizeof versions / sizeof *versions; ++i) {
		char const *value = godbolt_probe_macro(p, versions[i].ptr);
		if (!value)
			continue;
		godbolt_put(call, any ? TOOL_TEXT(" ") : TOOL_TEXT("versions: "));
		godbolt_put(call, versions[i]);
		godbolt_put(call, TOOL_TEXT("="));
		godbolt_put_field(call, (struct tool_text){ value, strlen(value) });
		any = true;
	}
	godbolt_put(call, any ? TOOL_TEXT("\n") : TOOL_TEXT("versions: unknown\n"));
	if (!call->headers)
		return call->e;
	for (uint32_t k = 0; k < p->n_headers; ++k) {
		godbolt_put(call, k ? TOOL_TEXT(", ") : TOOL_TEXT("headers: "));
		godbolt_put_field(call, (struct tool_text){ p->headers[k].name,
		                                            strlen(p->headers[k].name) });
		godbolt_put(call, TOOL_TEXT(" "));
		godbolt_put_field(call, (struct tool_text){ p->headers[k].value,
		                                            strlen(p->headers[k].value) });
	}
	return godbolt_put(call, p->n_headers ? TOOL_TEXT("\n") : TOOL_TEXT("headers: unknown\n"));
}

/* The phase GODBOLT_WRITE.  It writes the record of the compilation, as
 * much of it as the bounds of the step allow. */
static enum godbolt_go
godbolt_write (struct godbolt *call)
{
	if (!call->headed) {
		godbolt_header(call);
		if (call->no_pp)
			godbolt_put(call, TOOL_TEXT("Compiler Explorer sent no output of the "
			                            "preprocessor for this compiler.\n"));
		if (call->probed)
			godbolt_facts(call);
	}
	while (call->sec < call->n_sec && !call->full) {
		uint8_t const sec = call->sec;
		if (godbolt_section(call))
			return godbolt_fail(call);
		if (call->sec == sec && !call->full)
			return GODBOLT_GO;
	}
	if (call->truncated && !call->cut && !call->full)
		godbolt_put(call, TOOL_TEXT("[Compiler Explorer cut this output.]\n"));
	if (call->e)
		return godbolt_fail(call);
	call->phase = GODBOLT_FREE;
	return GODBOLT_GO;
}

/* Returns the number of the children of the JSON value @p v: the elements
 * of an array, the members of an object, or 0. */
static size_t
godbolt_json_size (struct json_object *v)
{
	return json_object_is_type(v, json_type_array) ? json_object_array_length(v)
	     : json_object_is_type(v, json_type_object) ? (size_t)json_object_object_length(v)
	     : 0;
}

/* Returns the child of @p v that godbolt_json_cut() removes: the last
 * element of an array, or the first member of an object.  @p v has
 * children. */
static struct json_object *
godbolt_json_child (struct json_object *v)
{
	if (json_object_is_type(v, json_type_array))
		return json_object_array_get_idx(v, json_object_array_length(v) - 1);
	struct json_object_iterator it = json_object_iter_begin(v);
	return json_object_iter_peek_value(&it);
}

/* Removes the child that godbolt_json_child() returns from @p v, and frees
 * it.  @p v has children. */
static void
godbolt_json_cut (struct json_object *v)
{
	if (json_object_is_type(v, json_type_array)) {
		(void)json_object_array_del_idx(v, json_object_array_length(v) - 1, 1);
		return;
	}
	struct json_object_iterator it = json_object_iter_begin(v);
	json_object_object_del(v, json_object_iter_peek_name(&it));
}

/* The phase GODBOLT_FREE.  It frees the parsed response to the compilation
 * one value at a time, as many values as the bounds of the step allow.
 * The value that it frees is at the end of the path of children that
 * godbolt_json_child() returns, so it has no children of its own.  Then the
 * next step begins the next compilation. */
static enum godbolt_go
godbolt_free (struct godbolt *call)
{
	uint64_t const room = loop_room(call->loop, LOOP_VALUES);
	uint64_t freed = 0;
	while (call->doc && freed < room) {
		struct json_object *parent = nullptr;
		struct json_object *v = call->doc;
		while (godbolt_json_size(v)) {
			parent = v;
			v = godbolt_json_child(v);
		}
		if (parent) {
			godbolt_json_cut(parent);
		} else {
			json_object_put(call->doc);
			call->doc = nullptr;
		}
		++freed;
	}
	loop_work(call->loop, LOOP_VALUES, freed);
	if (call->doc)
		return GODBOLT_GO;
	godbolt_next(call);
	return GODBOLT_GO;
}

/* The functions of the phases. */
static enum godbolt_go (*const godbolt_phases[]) (struct godbolt *call) = {
	[GODBOLT_LIST]   = godbolt_list,
	[GODBOLT_ROWS]   = godbolt_rows,
	[GODBOLT_ESCAPE] = godbolt_escape_source,
	[GODBOLT_JOB]    = godbolt_job,
	[GODBOLT_ANSWER] = godbolt_answer,
	[GODBOLT_PARSE]  = godbolt_parse,
	[GODBOLT_WRITE]  = godbolt_write,
	[GODBOLT_FREE]   = godbolt_free,
};

/* The step() of every kind of the plugin.  It runs the call's phase, and it
 * queues the next step if the phase goes on. */
static void
godbolt_step (struct mcp_slot *slot)
{
	if (!slot)
		return;
	void **own = mcp_slot_own(slot);
	if (!own || !*own)
		return;
	struct godbolt *call = *own;
	if (call->wait == GODBOLT_TIMER)
		return;
	if (godbolt_phases[call->phase](call) == GODBOLT_GO)
		(void)mcp_slot_ready(slot);
}

/* Frees the call @p *p_call and what it holds, and sets @p *p_call to
 * nullptr.  The call's timer is not set. */
static void
godbolt_destroy (struct godbolt **p_call)
{
	struct godbolt *call = *p_call;
	json_tokener_free(call->tok);
	json_object_put(call->doc);
	godbolt_probe_fini(&call->probe);
	free(call->body);
	call->body = nullptr;
	free(call);
	*p_call = nullptr;
}

/* The drop() of every kind of the plugin.  It stops the call's timer, and
 * frees the call. */
static void
godbolt_drop (struct mcp_slot *slot)
{
	if (!slot)
		return;
	void **own = mcp_slot_own(slot);
	if (!own || !*own)
		return;
	struct godbolt *call = *own;
	/* If the timer cannot be stopped, it can still fire, so the memory of
	 * the call must stay.  The timer function then returns at once,
	 * because call->slot is nullptr. */
	if (loop_timer_stop(call->loop, &call->timer)) {
		call->slot = nullptr;
		return;
	}
	godbolt_destroy(&call);
	*own = nullptr;
}

/* Returns the string @p v if it has @p min to @p max bytes and no NUL
 * byte, and if each of its bytes is in @p set, unless @p set is nullptr.
 * Otherwise it returns an empty text whose ptr is nullptr. */
static struct tool_text
godbolt_text (struct json_object *v,
              size_t              min,
              size_t              max,
              char const         *set)
{
	if (!json_object_is_type(v, json_type_string))
		return (struct tool_text){};
	char const *s = json_object_get_string(v);
	size_t const n = (size_t)json_object_get_string_len(v);
	if (n < min || n > max || (set ? strspn(s, set) : strlen(s)) != n)
		return (struct tool_text){};
	return (struct tool_text){ s, n };
}

/* Reads the array of ids @p v into a->ids.  Returns 0, or EINVAL. */
static int
godbolt_args_ids (struct godbolt_args *a,
                  struct json_object  *v)
{
	size_t const n = json_object_is_type(v, json_type_array) ? json_object_array_length(v) : 0;
	if (!n || n > GODBOLT_IDS)
		return EINVAL;
	for (size_t i = 0; i < n; ++i) {
		struct tool_text const id = godbolt_text(json_object_array_get_idx(v, i), 1,
		                                         GODBOLT_ID_MAX, godbolt_id_bytes);
		if (!id.ptr || godbolt_same(id, TOOL_TEXT(".")) || godbolt_same(id, TOOL_TEXT("..")))
			return EINVAL;
		for (size_t k = 0; k < i; ++k)
			if (godbolt_same(id, a->ids[k]))
				return EINVAL;
		a->ids[i] = id;
	}
	a->n_ids = (uint8_t)n;
	return 0;
}

/* Reads the array of argument strings @p v into a->args.  Returns 0, or
 * EINVAL. */
static int
godbolt_args_strings (struct godbolt_args *a,
                      struct json_object  *v)
{
	size_t const n = json_object_is_type(v, json_type_array) ? json_object_array_length(v) : 0;
	if (!n || n > GODBOLT_ARGS)
		return EINVAL;
	for (size_t i = 0; i < n; ++i) {
		a->args[i] = godbolt_text(json_object_array_get_idx(v, i), 0, GODBOLT_ARG_MAX,
		                          nullptr);
		if (!a->args[i].ptr)
			return EINVAL;
	}
	a->n_args = (uint8_t)n;
	return 0;
}

/* Reads the words of the argument match @p v into a->words.  ASCII white
 * space separates the words.  Returns 0, or EINVAL. */
static int
godbolt_args_words (struct godbolt_args *a,
                    struct json_object  *v)
{
	struct tool_text const t = godbolt_text(v, 0, GODBOLT_MATCH_MAX, nullptr);
	if (!t.ptr)
		return EINVAL;
	a->n_words = 0;
	for (size_t i = 0; i < t.len; ) {
		size_t k = i + strcspn(t.ptr + i, " \t\n\v\f\r");
		if (k > i && a->n_words == GODBOLT_WORDS)
			return EINVAL;
		if (k > i)
			a->words[a->n_words++] = (struct tool_text){ t.ptr + i, k - i };
		i = k + 1;
	}
	return 0;
}

/* Reads the argument filters @p v into a->filters.  Returns 0, or
 * EINVAL. */
static int
godbolt_args_filters (struct godbolt_args *a,
                      struct json_object  *v)
{
	if (!json_object_is_type(v, json_type_object))
		return EINVAL;
	struct json_object_iter it;
	json_object_object_foreachC(v, it) {
		size_t i = 0;
		while (i < GODBOLT_FILTERS &&
		       (!godbolt_filters[i].bit || strcmp(it.key, godbolt_filters[i].name.ptr)))
			++i;
		if (i == GODBOLT_FILTERS || !json_object_is_type(it.val, json_type_boolean))
			return EINVAL;
		a->filters = json_object_get_boolean(it.val) ? a->filters | godbolt_filters[i].bit
		                                             : a->filters & ~godbolt_filters[i].bit;
	}
	return 0;
}

/* Reads the value @p v of the argument @p arg into @p a.  Returns 0, or
 * EINVAL if the value breaks the rule of the argument. */
static int
godbolt_args_set (struct godbolt_args *a,
                  enum godbolt_arg     arg,
                  struct json_object  *v)
{
	switch (arg) {
	case GODBOLT_ARG_ACTION: {
		struct tool_text const t = godbolt_text(v, 1, GODBOLT_ID_MAX, nullptr);
		for (a->action = 0; a->action < GODBOLT_ACTIONS; ++a->action)
			if (godbolt_same(t, godbolt_actions[a->action]))
				return 0;
		return EINVAL;
	}
	case GODBOLT_ARG_LANGUAGE:
		a->language = godbolt_text(v, 1, GODBOLT_LANGUAGE_MAX, godbolt_language_bytes);
		return a->language.ptr ? 0 : EINVAL;
	case GODBOLT_ARG_IDS:
	case GODBOLT_ARG_COMPILERS:
		return godbolt_args_ids(a, v);
	case GODBOLT_ARG_MATCH:
		return godbolt_args_words(a, v);
	case GODBOLT_ARG_KIND:
		a->kind = godbolt_text(v, 1, GODBOLT_VALUE_MAX, nullptr);
		return a->kind.ptr ? 0 : EINVAL;
	case GODBOLT_ARG_FAMILY:
		a->family = godbolt_text(v, 1, GODBOLT_VALUE_MAX, nullptr);
		return a->family.ptr ? 0 : EINVAL;
	case GODBOLT_ARG_ARCHITECTURE:
		a->architecture = godbolt_text(v, 1, GODBOLT_VALUE_MAX, nullptr);
		return a->architecture.ptr ? 0 : EINVAL;
	case GODBOLT_ARG_LIMIT: {
		int64_t const n = json_object_is_type(v, json_type_int) ? json_object_get_int64(v)
		                                                        : 0;
		a->limit = (uint32_t)n;
		return n >= 1 && n <= GODBOLT_LIMIT_MAX ? 0 : EINVAL;
	}
	case GODBOLT_ARG_OFFSET:
		/* mcp_slot_offset() checks the offset. */
		return 0;
	case GODBOLT_ARG_ARGS:
		return godbolt_args_strings(a, v);
	case GODBOLT_ARG_SOURCE:
		a->source = godbolt_text(v, 0, GODBOLT_SOURCE_MAX, nullptr);
		return a->source.ptr ? 0 : EINVAL;
	case GODBOLT_ARG_FILTERS:
		return godbolt_args_filters(a, v);
	case GODBOLT_ARG_HEADERS:
		a->headers = json_object_get_boolean(v);
		return json_object_is_type(v, json_type_boolean) ? 0 : EINVAL;
	case GODBOLT_ARG_COUNT:
		break;
	}
	return EINVAL;
}

/* Formats @p fmt and its arguments into a->text, and sets a->why to that
 * text.  Returns EINVAL. */
[[gnu::format(printf, 2, 3)]]
static int
godbolt_args_refuse (struct godbolt_args *a,
                     char const          *fmt,
                     ...)
{
	va_list ap;
	va_start(ap, fmt);
	int const n = vsnprintf(a->text, sizeof a->text, fmt, ap);
	va_end(ap);
	a->why = (struct tool_text){ a->text, n < 0 ? 0 : (size_t)n < sizeof a->text
	                                              ? (size_t)n : sizeof a->text - 1 };
	return EINVAL;
}

/* Checks the arguments @p args of a call of the tool @p tool, and reads
 * them into @p a.  Returns 0, or EINVAL with the reason in a->why. */
static int
godbolt_args_read (struct godbolt_args *a,
                   struct json_object  *args,
                   enum godbolt_tool    tool)
{
	struct godbolt_params const *t = &godbolt_params[tool];
	*a = (struct godbolt_args){
		.args = { TOOL_TEXT("") },
		.language = TOOL_TEXT("c"),
		.limit = GODBOLT_LIMIT,
		.n_args = 1,
		.filters = GODBOLT_SITE_FILTERS
	};
	if (args && !json_object_is_type(args, json_type_object)) {
		a->why = TOOL_TEXT("The arguments must be a JSON object.");
		return EINVAL;
	}
	uint32_t seen = 0;
	struct json_object_iter it;
	if (args) json_object_object_foreachC(args, it) {
		uint32_t i = 0;
		while (i < GODBOLT_ARG_COUNT && strcmp(it.key, godbolt_arg_names[i]))
			++i;
		if (i < GODBOLT_ARG_COUNT && t->args >> i & 1) {
			seen |= 1u << i;
			if (!godbolt_args_set(a, (enum godbolt_arg)i, it.val))
				continue;
			a->why = godbolt_arg_rules[i];
			return EINVAL;
		}
		return godbolt_args_refuse(a, "The tool has no argument %.*s.",
		                           (int)godbolt_cut(it.key, strnlen(it.key, 49), 48), it.key);
	}
	uint32_t const missing = t->needs & ~seen;
	uint32_t i = 0;
	while (i < GODBOLT_ARG_COUNT && !(missing >> i & 1))
		++i;
	if (i < GODBOLT_ARG_COUNT)
		return godbolt_args_refuse(a, "The argument %s is required.", godbolt_arg_names[i]);
	if (a->action == GODBOLT_LANGUAGES && tool == GODBOLT_CATALOG &&
	    (a->kind.len || a->family.len || a->architecture.len)) {
		a->why = TOOL_TEXT("kind, family and architecture are not valid with the action "
		                   "languages.");
		return EINVAL;
	}
	if ((uint32_t)a->n_ids * a->n_args > GODBOLT_JOBS) {
		a->why = TOOL_TEXT("The number of compilers times the number of argument strings "
		                   "must be at most 32.");
		return EINVAL;
	}
	return 0;
}

/* Copies @p t to *@p p, advances *@p p past the copy, and returns the
 * copy. */
static struct tool_text
godbolt_copy (char             **p,
              struct tool_text   t)
{
	struct tool_text const copy = { *p, t.len };
	*p = godbolt_cat(*p, t);
	return copy;
}

/* The line before the source of the probe that makes the compiler include
 * the target headers of the probe. */
#define GODBOLT_HEADERS_LINE "#define GODBOLT_PROBE_HEADERS 1\n"

/* Allocates the state of a call of the tool @p tool on the loop @p loop,
 * with copies of the texts of @p a, and counts the copies as LOOP_COPY
 * work.  The words of match are copied in lower case.  An empty source
 * becomes "\n", as in godbolt-c.  The source of godbolt_targets is the
 * probe, after a line that makes the compiler include the target headers
 * if the argument headers is true.  Returns the state.  On failure it
 * returns nullptr and stores ENOMEM in @p *e. */
static struct godbolt *
godbolt_create (struct godbolt_args const *a,
                enum godbolt_tool          tool,
                struct loop               *loop,
                int                       *e)
{
	bool const targets = tool == GODBOLT_TARGETS;
	struct tool_text const head = targets && a->headers ? TOOL_TEXT(GODBOLT_HEADERS_LINE)
	                                                    : (struct tool_text){};
	struct tool_text source = a->source.len ? a->source : TOOL_TEXT("\n");
	if (targets)
		source.ptr = godbolt_probe_source(&source.len);
	size_t bytes = a->language.len + head.len + source.len + a->kind.len + a->family.len +
	               a->architecture.len;
	for (uint8_t i = 0; i < a->n_ids; ++i)
		bytes += a->ids[i].len;
	for (uint8_t i = 0; i < a->n_args; ++i)
		bytes += a->args[i].len;
	for (uint8_t i = 0; i < a->n_words; ++i)
		bytes += a->words[i].len;
	struct godbolt *call = malloc(sizeof *call + bytes);
	struct json_tokener *tok = call ? json_tokener_new() : nullptr;
	if (!tok) {
		free(call);
		call = nullptr;
		*e = ENOMEM;
		return nullptr;
	}
	*call = (struct godbolt){
		.timer = { .due = UINT64_MAX, .fn = godbolt_fired },
		.tok = tok,
		.loop = loop,
		.limit = a->limit,
		.n_ids = a->n_ids,
		.n_args = a->n_args,
		.n_words = a->n_words,
		.tool = tool,
		.action = tool == GODBOLT_CATALOG ? a->action : GODBOLT_COMPILERS,
		.filters = a->filters,
		.headers = a->headers
	};
	char *p = call->blob;
	for (uint8_t i = 0; i < a->n_ids; ++i)
		call->ids[i] = godbolt_copy(&p, a->ids[i]);
	for (uint8_t i = 0; i < a->n_args; ++i)
		call->args[i] = godbolt_copy(&p, a->args[i]);
	for (uint8_t i = 0; i < a->n_words; ++i) {
		call->words[i] = (struct tool_text){ p, a->words[i].len };
		for (size_t k = 0; k < a->words[i].len; ++k)
			*p++ = godbolt_lower(a->words[i].ptr[k]);
	}
	call->language = godbolt_copy(&p, a->language);
	call->source = (struct tool_text){ p, head.len + source.len };
	p = godbolt_cat(godbolt_cat(p, head), source);
	call->kind = godbolt_copy(&p, a->kind);
	call->family = godbolt_copy(&p, a->family);
	call->architecture = godbolt_copy(&p, a->architecture);
	loop_work(loop, LOOP_COPY, bytes);
	*e = 0;
	return call;
}

#undef GODBOLT_HEADERS_LINE

/* Starts a call of the tool @p tool.  It checks the arguments and copies
 * them.  godbolt_catalog sends the request of its list.  The other tools
 * escape the source in their steps first, and godbolt_preprocess,
 * godbolt_macros and godbolt_targets then read the kinds of their
 * compilers from the list of compilers. */
static void
godbolt_start (struct mcp_slot    *slot,
               struct json_object *args,
               enum godbolt_tool   tool)
{
	if (!slot)
		return;
	struct godbolt_config const *cfg = mcp_slot_config(slot);
	struct loop *loop = mcp_slot_loop(slot);
	void **own = mcp_slot_own(slot);
	if (!cfg || !loop || !own || cfg->len > GODBOLT_URL_MAX) {
		mcp_slot_tell(slot, TOOL_TEXT("The call has no valid configuration of the "
		                              "godbolt plugin, no loop or no own word."),
		              TOOL_FAILED);
		return;
	}
	struct godbolt_args a;
	if (godbolt_args_read(&a, args, tool)) {
		mcp_slot_tell(slot, a.why, TOOL_FAILED);
		return;
	}
	if (mcp_slot_offset(slot, args))
		return;
	struct json_object *offset = nullptr;
	bool const paged = json_object_object_get_ex(args, "offset", &offset) &&
	                   json_object_get_int64(offset) > 0;
	int e;
	struct godbolt *call = godbolt_create(&a, tool, loop, &e);
	if (!call) {
		mcp_slot_fail(slot, e);
		return;
	}
	*own = call;
	call->slot = slot;
	call->cfg = cfg;
	call->paged = paged;
	if (tool != GODBOLT_CATALOG) {
		call->phase = GODBOLT_ESCAPE;
		if (!mcp_slot_ready(slot))
			mcp_slot_park(slot);
		return;
	}
	if (godbolt_list_send(call) == GODBOLT_WAIT)
		mcp_slot_park(slot);
}

#undef GODBOLT_FIELDS

static void
godbolt_catalog_start (struct mcp_slot    *slot,
                       struct json_object *args)
{
	godbolt_start(slot, args, GODBOLT_CATALOG);
}

static void
godbolt_compile_start (struct mcp_slot    *slot,
                       struct json_object *args)
{
	godbolt_start(slot, args, GODBOLT_COMPILE);
}

static void
godbolt_preprocess_start (struct mcp_slot    *slot,
                          struct json_object *args)
{
	godbolt_start(slot, args, GODBOLT_PREPROCESS);
}

static void
godbolt_macros_start (struct mcp_slot    *slot,
                      struct json_object *args)
{
	godbolt_start(slot, args, GODBOLT_MACROS);
}

static void
godbolt_targets_start (struct mcp_slot    *slot,
                       struct json_object *args)
{
	godbolt_start(slot, args, GODBOLT_TARGETS);
}

/* Stores the reason of a refusal of the options in the @p len bytes at
 * @p why, formatted from @p fmt and its arguments.  Returns EINVAL. */
[[gnu::format(printf, 3, 4)]]
static int
godbolt_configure_refuse (char       *why,
                          size_t      len,
                          char const *fmt,
                          ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(why, len, fmt, ap);
	va_end(ap);
	if (n < 0)
		*why = '\0';
	return EINVAL;
}

/* Stores the URL @p v in the configuration @p c without its trailing '/'.
 * Returns 0, or EINVAL if @p v is not an http or https URL of at most
 * GODBOLT_URL_MAX bytes, or if it has a user name, a password, a query, a
 * fragment or a byte that is not printable ASCII or is a space. */
static int
godbolt_config_url (struct godbolt_config *c,
                    struct json_object    *v)
{
	struct tool_text const t = godbolt_text(v, 1, GODBOLT_URL_MAX, nullptr);
	size_t const scheme = t.len > 7 && !memcmp(t.ptr, "http://", 7) ? 7
	                    : t.len > 8 && !memcmp(t.ptr, "https://", 8) ? 8 : 0;
	if (!scheme)
		return EINVAL;
	size_t n = t.len;
	while (n > scheme && t.ptr[n - 1] == '/')
		--n;
	size_t const host = strcspn(t.ptr + scheme, "/");
	if (n == scheme || !host || memchr(t.ptr + scheme, '@', host))
		return EINVAL;
	for (size_t i = 0; i < n; ++i)
		if (t.ptr[i] <= ' ' || t.ptr[i] > '~' || t.ptr[i] == '?' || t.ptr[i] == '#')
			return EINVAL;
	memcpy(c->url, t.ptr, n);
	c->url[n] = '\0';
	c->len = (uint32_t)n;
	return 0;
}

/* The plugin's configure().  It stores the defaults, and then the options
 * of @p options.  The only option is "url". */
static int
godbolt_configure (void               *config,
                   struct json_object *options,
                   char               *why,
                   size_t              len)
{
	if (!why || !len)
		return EINVAL;
	if (!config)
		return godbolt_configure_refuse(why, len, "askai passed no storage for the "
		                                "configuration");
	struct godbolt_config *c = config;
	*c = godbolt_defaults;
	if (!options)
		return 0;
	if (!json_object_is_type(options, json_type_object))
		return godbolt_configure_refuse(why, len, "not an object");
	struct json_object_iter it;
	json_object_object_foreachC(options, it) {
		if (strcmp(it.key, "url")) {
			int const n = (int)godbolt_cut(it.key, strnlen(it.key, 65), 64);
			return godbolt_configure_refuse(why, len, "the member %.*s is not an "
			                                "option", n, it.key);
		}
		if (godbolt_config_url(c, it.val))
			return godbolt_configure_refuse(why, len, "url must be an http or https URL "
			                                "of at most 255 bytes, without a user name, "
			                                "a password, a query or a fragment");
	}
	return 0;
}

/* Returns the definition in the JSON text @p json as a new reference.  Its
 * description is @p head, the base URL of the configuration @p service, and
 * @p tail.  Returns nullptr if json-c runs out of memory, or if the
 * configuration is not valid. */
static struct json_object *
godbolt_definition (void const       *service,
                    char const       *json,
                    struct tool_text  head,
                    struct tool_text  tail)
{
	struct godbolt_config const *cfg = service ? service : &godbolt_defaults;
	char text[2048];
	if (cfg->len > GODBOLT_URL_MAX || head.len + cfg->len + tail.len > sizeof text)
		return nullptr;
	char *end = godbolt_cat(godbolt_cat(godbolt_cat(text, head),
	                                    (struct tool_text){ cfg->url, cfg->len }), tail);
	struct json_object *def = json_tokener_parse(json);
	struct json_object *desc = json_object_new_string_len(text, (int)(end - text));
	if (!def || !desc || json_object_object_add(def, "description", desc)) {
		json_object_put(desc);
		json_object_put(def);
		return nullptr;
	}
	return def;
}

/* The parts of the definitions that several tools share. */
#define GODBOLT_DEF_LANGUAGE(what, examples)                                    \
	"\"language\":{\"type\":\"string\",\"default\":\"c\",\"description\":"  \
	"\"The Compiler Explorer id of the language of " what ", such as "       \
	examples ". The default is c.\"}"
#define GODBOLT_DEF_SOURCE                                                      \
	"\"source\":{\"type\":\"string\",\"description\":\"The source code "    \
	"itself, not a file name. It has at most 1 MiB of UTF-8.\"}"
#define GODBOLT_DEF_COMPILERS                                                   \
	"\"compilers\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"     \
	"\"minItems\":1,\"maxItems\":16,\"uniqueItems\":true,\"description\":"  \
	"\"Exact compiler ids from godbolt_catalog, not display names. The "     \
	"number of ids times the number of argument strings must be at most "    \
	"32.\"}"
#define GODBOLT_DEF_ARGS                                                        \
	"\"args\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"          \
	"\"minItems\":1,\"maxItems\":8,\"default\":[\"\"],\"description\":"     \
	"\"Argument strings, such as \\\"-O2 -std=c23\\\". Each string has all " \
	"the extra arguments of one compilation, and it is not one argument. "   \
	"Each compiler compiles once with each string. The array has at most 8 " \
	"strings, and each string has at most 8192 bytes. No shell interprets "  \
	"them.\"}"
#define GODBOLT_DEF_OFFSET                                                      \
	"\"offset\":{\"type\":\"integer\",\"minimum\":0,\"description\":"       \
	"\"The byte of the output to start from, to read on past where it was "  \
	"cut.\"}"
#define GODBOLT_DEF_HINTS                                                       \
	"\"annotations\":{\"readOnlyHint\":true,\"destructiveHint\":false,"      \
	"\"idempotentHint\":true,\"openWorldHint\":true}"

static struct json_object *
godbolt_catalog_definition (void const *service)
{
	return godbolt_definition(service,
		"{\"name\":\"godbolt_catalog\",\"title\":\"Compiler Explorer catalog\","
		"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		"\"action\":{\"type\":\"string\",\"enum\":[\"languages\",\"compilers\","
		"\"kinds\",\"families\",\"architectures\"],\"description\":\"The action "
		"selects what the call lists: languages, compilers, or numbers of "
		"compilers. kinds counts the compilers of each compilerType, families "
		"counts the compilers of each family tag, and architectures counts the "
		"compilers of each instruction set.\"},"
		GODBOLT_DEF_LANGUAGE("the compilers", "c, c++ or rust") ","
		"\"ids\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
		"\"minItems\":1,\"maxItems\":16,\"uniqueItems\":true,\"description\":"
		"\"Exact ids of the items to list. The call fails if an id is not in the "
		"list, or if another filter excludes it.\"},"
		"\"match\":{\"type\":\"string\",\"description\":\"Words that each item "
		"contains, in any letter case. For a compiler, each word must be part of "
		"its id, name, kind, version, architecture, release track or one of its "
		"family tags. For a language, each word must be part of its id or name. "
		"At most 8 words and 256 bytes.\"},"
		"\"kind\":{\"type\":\"string\",\"description\":\"The exact kind of the "
		"compilers to list. The kind is the compilerType of Compiler Explorer, or "
		"default if a compiler has no compilerType. A compiler of the kind default "
		"has a GCC-style driver. The action kinds lists the kinds. It is not valid "
		"with the action languages.\"},"
		"\"family\":{\"type\":\"string\",\"description\":\"The exact family tag "
		"of the compilers to list. The action families lists the tags. It is not "
		"valid with the action languages.\"},"
		"\"architecture\":{\"type\":\"string\",\"description\":\"The exact "
		"instruction set of the compilers to list, such as amd64 or aarch64. The "
		"action architectures lists the instruction sets. It is not valid with "
		"the action languages.\"},"
		"\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2000,"
		"\"default\":100,\"description\":\"The largest number of items in the "
		"output. The last line of the output has the number of items that "
		"match.\"},"
		GODBOLT_DEF_OFFSET "},"
		"\"required\":[\"action\"],\"additionalProperties\":false},"
		GODBOLT_DEF_HINTS "}",
		TOOL_TEXT("Lists the languages of Compiler Explorer at "),
		TOOL_TEXT(", the compilers of one language, or the numbers of compilers of "
		          "each kind, family or architecture. The other godbolt tools accept "
		          "only the exact compiler ids of this list. Each line of the output "
		          "is one item, and tabs separate its columns. The first line names "
		          "the columns. All filters apply together. askai uses a list that it "
		          "received in the last hour again, so a repeated call is usually "
		          "fast."));
}

static struct json_object *
godbolt_compile_definition (void const *service)
{
	return godbolt_definition(service,
		"{\"name\":\"godbolt_compile\",\"title\":\"Compile on Compiler Explorer\","
		"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		GODBOLT_DEF_COMPILERS "," GODBOLT_DEF_ARGS ","
		GODBOLT_DEF_LANGUAGE("the source", "c, c++ or rust") ","
		GODBOLT_DEF_SOURCE ","
		"\"filters\":{\"type\":\"object\",\"additionalProperties\":false,"
		"\"properties\":{\"intel\":{\"type\":\"boolean\",\"default\":true},"
		"\"demangle\":{\"type\":\"boolean\",\"default\":true},"
		"\"directives\":{\"type\":\"boolean\",\"default\":true},"
		"\"commentOnly\":{\"type\":\"boolean\",\"default\":true},"
		"\"labels\":{\"type\":\"boolean\",\"default\":true},"
		"\"libraryCode\":{\"type\":\"boolean\",\"default\":false},"
		"\"trim\":{\"type\":\"boolean\",\"default\":false}},"
		"\"description\":\"How Compiler Explorer filters the assembly. The "
		"defaults are those of the Compiler Explorer site. The assembly has "
		"Intel syntax and demangled names, and it has no assembler directives, "
		"no lines with only a comment and no unused labels.\"},"
		GODBOLT_DEF_OFFSET "},"
		"\"required\":[\"compilers\",\"source\"],\"additionalProperties\":false},"
		GODBOLT_DEF_HINTS "}",
		TOOL_TEXT("Compiles source code on Compiler Explorer at "),
		TOOL_TEXT(", once with each compiler and each argument string. The output "
		          "has a record for each compilation, with the exit code, the "
		          "assembly and the messages of the compiler, and the last line has "
		          "the counts of the results. askai sends the source to that service. "
		          "The compiled program does not run. The compiler ids must be exact "
		          "ids from godbolt_catalog."));
}

static struct json_object *
godbolt_preprocess_definition (void const *service)
{
	return godbolt_definition(service,
		"{\"name\":\"godbolt_preprocess\",\"title\":\"Preprocess on Compiler "
		"Explorer\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		GODBOLT_DEF_COMPILERS "," GODBOLT_DEF_ARGS ","
		GODBOLT_DEF_LANGUAGE("the source", "c or c++") ","
		GODBOLT_DEF_SOURCE "," GODBOLT_DEF_OFFSET "},"
		"\"required\":[\"compilers\",\"source\"],\"additionalProperties\":false},"
		GODBOLT_DEF_HINTS "}",
		TOOL_TEXT("Preprocesses source code on Compiler Explorer at "),
		TOOL_TEXT(", once with each compiler and each argument string. The output "
		          "has a record for each compilation, with the exit code, the "
		          "preprocessed source and the messages of the compiler, and the last "
		          "line has the counts of the results. Compiler Explorer resolves the "
		          "includes with the headers of each compiler. askai sends the source "
		          "to that service. The compiler ids must be exact ids from "
		          "godbolt_catalog."));
}

static struct json_object *
godbolt_macros_definition (void const *service)
{
	return godbolt_definition(service,
		"{\"name\":\"godbolt_macros\",\"title\":\"Predefined macros on Compiler "
		"Explorer\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		GODBOLT_DEF_COMPILERS "," GODBOLT_DEF_ARGS ","
		GODBOLT_DEF_LANGUAGE("the source", "c or c++") ","
		"\"source\":{\"type\":\"string\",\"description\":\"Source code whose "
		"macro definitions the output also has, not a file name. It has at most "
		"1 MiB of UTF-8. The default is no source.\"},"
		GODBOLT_DEF_OFFSET "},"
		"\"required\":[\"compilers\"],\"additionalProperties\":false},"
		GODBOLT_DEF_HINTS "}",
		TOOL_TEXT("Outputs the predefined macros of each compiler with each argument "
		          "string, from Compiler Explorer at "),
		TOOL_TEXT(". The output has a record for each compilation, with the exit code, "
		          "the macros and the messages of the compiler, and the last line has "
		          "the counts of the results. The record of an MSVC compiler has no "
		          "macros, because MSVC cannot output them. askai sends the source to "
		          "that service. The compiler ids must be exact ids from "
		          "godbolt_catalog."));
}

static struct json_object *
godbolt_targets_definition (void const *service)
{
	return godbolt_definition(service,
		"{\"name\":\"godbolt_targets\",\"title\":\"Target facts from Compiler "
		"Explorer\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		GODBOLT_DEF_COMPILERS "," GODBOLT_DEF_ARGS ","
		GODBOLT_DEF_LANGUAGE("the probe", "c or c++") ","
		"\"headers\":{\"type\":\"boolean\",\"default\":false,\"description\":"
		"\"true to include a few headers of the target too, such as limits.h and "
		"features.h, and to output which of them the compiler has. The "
		"compilation can then fail if a compiler on Compiler Explorer has no "
		"headers for its target.\"},"
		GODBOLT_DEF_OFFSET "},"
		"\"required\":[\"compilers\"],\"additionalProperties\":false},"
		GODBOLT_DEF_HINTS "}",
		TOOL_TEXT("Preprocesses a fixed probe on Compiler Explorer at "),
		TOOL_TEXT(", once with each compiler and each argument string. The output "
		          "has facts about each target that follow from the predefined "
		          "macros. The facts are the architecture, the operating system, the "
		          "data model, the width of a pointer, the byte order, the C library "
		          "and the versions of the compiler and of the language. The line of "
		          "each fact lists the macros that the fact comes from, and the last "
		          "line has the counts of the results. The compiler ids must be exact "
		          "ids from godbolt_catalog."));
}

#undef GODBOLT_DEF_HINTS
#undef GODBOLT_DEF_OFFSET
#undef GODBOLT_DEF_ARGS
#undef GODBOLT_DEF_COMPILERS
#undef GODBOLT_DEF_SOURCE
#undef GODBOLT_DEF_LANGUAGE

struct plugin const askai_plugin = {
	.tools = {
		{
			.definition = godbolt_catalog_definition,
			.start = godbolt_catalog_start,
			.step = godbolt_step,
			.drop = godbolt_drop,
			.flags = TOOL_LENT | TOOL_PAGED | TOOL_STREAM
		},
		{
			.definition = godbolt_compile_definition,
			.start = godbolt_compile_start,
			.step = godbolt_step,
			.drop = godbolt_drop,
			.flags = TOOL_LENT | TOOL_PAGED | TOOL_STREAM | TOOL_DETACH
		},
		{
			.definition = godbolt_preprocess_definition,
			.start = godbolt_preprocess_start,
			.step = godbolt_step,
			.drop = godbolt_drop,
			.flags = TOOL_LENT | TOOL_PAGED | TOOL_STREAM | TOOL_DETACH
		},
		{
			.definition = godbolt_macros_definition,
			.start = godbolt_macros_start,
			.step = godbolt_step,
			.drop = godbolt_drop,
			.flags = TOOL_LENT | TOOL_PAGED | TOOL_STREAM | TOOL_DETACH
		},
		{
			.definition = godbolt_targets_definition,
			.start = godbolt_targets_start,
			.step = godbolt_step,
			.drop = godbolt_drop,
			.flags = TOOL_LENT | TOOL_PAGED | TOOL_STREAM | TOOL_DETACH
		},
	},
	.configure = godbolt_configure,
	.config_size = sizeof (struct godbolt_config)
};
