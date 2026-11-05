/** @file godbolt.c
 *
 * The godbolt plugin. Its tools use a Compiler Explorer service,
 * https://godbolt.org unless the option "url" names another one.
 *
 * godbolt_catalog lists the languages of the service, the compilers of one
 * language, or the numbers of compilers of each kind, family or
 * architecture. godbolt_compile compiles source code once with each of
 * one or more compilers and each of one or more argument strings. Its
 * output has a record for each compilation, with the exit code, the
 * assembly and the messages of the compiler. godbolt_preprocess,
 * godbolt_macros and godbolt_targets compile in the same way. Their
 * records have the preprocessed source, the predefined macros, or facts
 * about the target that follow from the predefined macros
 * (src/godbolt_probe.h). The flags of the compiler's driver depend on the
 * kind of the compiler, which the list of compilers has. With the argument
 * share, the first line of the output of godbolt_compile,
 * godbolt_preprocess or godbolt_macros is a link to the same state on
 * Compiler Explorer. README.md describes the tools and the options url and
 * share.
 *
 * A call sends its HTTP requests through askai with mcp_slot_fetch(), one
 * at a time. askai can store a response for the seconds of the keep of
 * the request, so the plugin stores nothing between calls. Each call has
 * a struct godbolt, to which the call's own word points. The plugin has
 * no instances and no mutable static variable.
 *
 * A step of a call does no more work than the bounds of the loop allow
 * (src/loop_export.h). A list of compilers is a JSON array, and the
 * plugin parses it one element at a time, so it never has a tree of the
 * whole list. The plugin parses a response to a compilation in slices
 * into one tree, writes it in slices, and frees it in slices. One step
 * parses the output of the probe of godbolt_targets, which is usually less
 * than 100 KiB, but can be as long as a response.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbit.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "compat.h"
#include "godbolt_probe.h"
#include "jb.h"
#include "plugin.h"

/* The tools: the name after godbolt_, the constant of enum godbolt_tool,
 * and the flags of the kind besides TOOL_LENT, TOOL_PAGED and
 * TOOL_STREAM.
 */
#define GODBOLT_TOOLS_X(X)                      \
        X(catalog, CATALOG, 0)                  \
        X(compile, COMPILE, TOOL_DETACH)        \
        X(preprocess, PREPROCESS, TOOL_DETACH)  \
        X(macros, MACROS, TOOL_DETACH)          \
        X(targets, TARGETS, TOOL_DETACH)

#define GODBOLT_NAME(name, NAME, extra) "godbolt_" #name,
PLUGIN_MANIFEST(.name = "godbolt", .tools = { GODBOLT_TOOLS_X(GODBOLT_NAME) });
#undef GODBOLT_NAME

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
	GODBOLT_LINK_MAX     = 8192,      /* bytes of a link to the state, which
	                                     is the longest URL that the CDN of
	                                     godbolt.org passes on */
	GODBOLT_SHORT_MAX    = 512,       /* bytes of a short link */
	GODBOLT_SKIP         = sizeof "{\"sessions\":[" - 1,  /* bytes of the
	                                                         body before the
	                                                         body of a
	                                                         compilation */
};

/* The fields of the list of compilers that the plugin requests. Each call
 * requests the same fields, so askai stores one list for each language.
 */
#define GODBOLT_FIELDS \
        "id,name,compilerType,compilerCategories,semver,instructionSet,releaseTrack"

/* The bytes of a compiler id, and of a language id. */
static char const godbolt_id_bytes[] =
	"+-.0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
static char const godbolt_language_bytes[] =
	"+-0123456789_abcdefghijklmnopqrstuvwxyz";

/* The plugin's configuration. configure() stores it, and mcp_slot_config()
 * returns it. It is plain data without pointers.
 */
struct godbolt_config {
	uint32_t len;                       /* the length of url in bytes */
	uint32_t share;                     /* enum godbolt_share */
	char     url[GODBOLT_URL_MAX + 1];  /* the base URL without a trailing
	                                       '/', NUL-terminated */
};

/* The kinds of link for the argument share of a call. The option share
 * selects the kind.
 */
enum godbolt_share : uint32_t {
	GODBOLT_SHARE_SHORT,  /* a link /z/ID that Compiler Explorer makes and
	                         stores permanently */
	GODBOLT_SHARE_STATE,  /* a link /clientstate/BASE64 that has the state,
	                         so that Compiler Explorer stores nothing */
	GODBOLT_SHARE_OFF,    /* no link, and the tools have no argument share */
	GODBOLT_SHARES
};

/* Each entry is the name of a value of the option share. */
static struct cstr_view const godbolt_shares[GODBOLT_SHARES] = {
	[GODBOLT_SHARE_SHORT] = CSTR_VIEW_INIT("short"),
	[GODBOLT_SHARE_STATE] = CSTR_VIEW_INIT("state"),
	[GODBOLT_SHARE_OFF]   = CSTR_VIEW_INIT("off"),
};

/* Each entry is the sentence on links that ends the description of a tool
 * with the argument share, for a value of the option share. With share
 * off, the tools do not accept the argument share, and the sentence is
 * empty.
 */
static struct cstr_view const godbolt_share_notes[GODBOLT_SHARES] = {
	[GODBOLT_SHARE_SHORT] = CSTR_VIEW_INIT(
		" With share true, the first line of the output is a link to the same "
		"source, compilers and arguments on Compiler Explorer. For the link, "
		"Compiler Explorer stores the source permanently, and anyone with the "
		"link can read it. Set share only if the user asks for a link."),
	[GODBOLT_SHARE_STATE] = CSTR_VIEW_INIT(
		" With share true, the first line of the output is a link to the same "
		"source, compilers and arguments on Compiler Explorer. The link contains "
		"the source, and Compiler Explorer stores nothing. Set share only if the "
		"user asks for a link."),
};

/* The configuration without options. definition() uses it if askai passes
 * no configuration.
 */
static struct godbolt_config const godbolt_defaults = {
	.len = sizeof "https://godbolt.org" - 1,
	.share = GODBOLT_SHARE_SHORT,
	.url = "https://godbolt.org"
};

/* The tools, in the order of the manifest. */
enum godbolt_tool : uint8_t {
#define GODBOLT_TOOL(name, NAME, extra) GODBOLT_##NAME,
	GODBOLT_TOOLS_X(GODBOLT_TOOL)
#undef GODBOLT_TOOL
	GODBOLT_TOOLS
};

/* The kinds of driver of a compiler. The list of compilers has the kind of
 * each compiler as its compilerType. GODBOLT_GNU is every kind but
 * win32-vc and clang-cl.
 */
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
	GODBOLT_POST_SHORTEN,   /* POST /api/shortener */
};

/* The failures after which the plugin sends a request again, besides the
 * statuses 429, 502 and 503.
 */
enum godbolt_again : uint32_t {
	GODBOLT_AGAIN_504      = 1u << 0,
	GODBOLT_AGAIN_EIO      = 1u << 1,
	GODBOLT_AGAIN_TIMEDOUT = 1u << 2,
};

/* The URL and the limits of a kind of request, and the failures after
 * which the plugin sends it again. The URL is the base URL, the path, the
 * segment that godbolt_request() gets, and the tail.
 */
struct godbolt_rule {
	struct cstr_view path;
	struct cstr_view tail;
	uint32_t         max;      /* bytes of the response body */
	uint32_t         timeout;  /* seconds */
	uint32_t         keep;     /* seconds, but godbolt_request() sets the keep
	                              of a compilation */
	uint32_t         again;    /* enum godbolt_again */
};

/* The plugin does not send a compilation again after 504 or a timeout,
 * because Compiler Explorer can still be compiling it. It sends the
 * request of a short link again after 504 and EIO, because Compiler
 * Explorer returns the same link for the same state.
 */
static struct godbolt_rule const godbolt_rules[] = {
	[GODBOLT_GET_LANGUAGES] = { CSTR_VIEW_INIT("/api/languages"), CSTR_VIEW_INIT(""),
	                            1 << 20, 30, 86400, GODBOLT_AGAIN_504 | GODBOLT_AGAIN_EIO |
	                                                GODBOLT_AGAIN_TIMEDOUT },
	[GODBOLT_GET_COMPILERS] = { CSTR_VIEW_INIT("/api/compilers/"),
	                            CSTR_VIEW_INIT("?fields=" GODBOLT_FIELDS),
	                            4 << 20, 30, 3600, GODBOLT_AGAIN_504 | GODBOLT_AGAIN_EIO |
	                                               GODBOLT_AGAIN_TIMEDOUT },
	[GODBOLT_POST_COMPILE]  = { CSTR_VIEW_INIT("/api/compiler/"), CSTR_VIEW_INIT("/compile"),
	                            4 << 20, 60, 300, 0 },
	[GODBOLT_POST_SHORTEN]  = { CSTR_VIEW_INIT("/api/shortener"), CSTR_VIEW_INIT(""),
	                            4 << 10, 30, 86400, GODBOLT_AGAIN_504 | GODBOLT_AGAIN_EIO },
};

/* What the next step of a call does. */
enum godbolt_phase : uint8_t {
	GODBOLT_LIST,    /* reads the response to the request of a list */
	GODBOLT_ROWS,    /* parses the list, and writes rows or reads the
	                    kinds of the compilers */
	GODBOLT_ESCAPE,  /* escapes the source into the body of the requests */
	GODBOLT_SHARE,   /* begins the state of the call */
	GODBOLT_STATE,   /* writes the compilers of the state, and then the
	                    link to the state or the request of a short link */
	GODBOLT_LINK,    /* reads the response with the short link */
	GODBOLT_JOB,     /* sends the request of the next compilation */
	GODBOLT_ANSWER,  /* reads the response to a compilation */
	GODBOLT_PARSE,   /* parses the response to a compilation */
	GODBOLT_WRITE,   /* writes the record of a compilation */
	GODBOLT_FREE,    /* frees the parsed response to a compilation */
};

/* The state of the call's request. */
enum godbolt_wait : uint8_t {
	GODBOLT_IDLE,     /* the request is under way or done, or there is
	                     none */
	GODBOLT_TIMER,    /* the timer sends the request again later */
	GODBOLT_REFUSED,  /* askai refused the request, or the timer could not
	                     be set, and call->why has the reason, which the
	                     next step reads as the result of the request */
};

/* The values that the functions of a phase return. */
enum godbolt_go {
	GODBOLT_GO,     /* the next step goes on */
	GODBOLT_WAIT,   /* a request is under way, the timer is set, or the
	                   next step is queued */
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

/* The actions of godbolt_catalog: the constant of enum godbolt_action, the
 * name, the singular of the noun, and the first line of the output.
 */
#define GODBOLT_ACTIONS_X(X)                                          \
        X(LANGUAGES, "languages", "language", "id\tname\n")           \
        X(COMPILERS, "compilers", "compiler",                         \
          "id\tname\tkind\tversion\tarchitecture\ttrack\tfamilies\n") \
        X(KINDS, "kinds", "kind", "kind\tcompilers\n")                \
        X(FAMILIES, "families", "family", "family\tcompilers\n")      \
        X(ARCHITECTURES, "architectures", "architecture", "architecture\tcompilers\n")

enum godbolt_action : uint8_t {
#define GODBOLT_ACTION(NAME, name, singular, header) GODBOLT_##NAME,
	GODBOLT_ACTIONS_X(GODBOLT_ACTION)
#undef GODBOLT_ACTION
	GODBOLT_ACTIONS
};

static struct {
	struct cstr_view name;
	struct cstr_view header;
	char const      *singular;
} const godbolt_actions[] = {
#define GODBOLT_ACTION(NAME, name, singular, header) \
        { CSTR_VIEW_INIT(name), CSTR_VIEW_INIT(header), singular },
	GODBOLT_ACTIONS_X(GODBOLT_ACTION)
#undef GODBOLT_ACTION
};

#undef GODBOLT_ACTIONS_X

/* The ends of the compilations, which the last line of the output
 * counts.
 */
enum godbolt_outcome : uint8_t {
	GODBOLT_ZERO,    /* exit code 0 */
	GODBOLT_OTHER,   /* another exit code, or a timeout */
	GODBOLT_FAILED,  /* a failed request */
	GODBOLT_UNSENT,  /* no request */
	GODBOLT_COUNTS
};

/* The arguments of the tools. Bit i of a mask of struct godbolt_params
 * stands for the argument godbolt_arg_names[i].
 */
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
	GODBOLT_ARG_SHARE,
	GODBOLT_ARG_COUNT
};

static struct cstr_view const godbolt_arg_names[] = {
	CSTR_VIEW_INIT("action"), CSTR_VIEW_INIT("language"), CSTR_VIEW_INIT("ids"),
	CSTR_VIEW_INIT("match"), CSTR_VIEW_INIT("kind"), CSTR_VIEW_INIT("family"),
	CSTR_VIEW_INIT("architecture"), CSTR_VIEW_INIT("limit"), CSTR_VIEW_INIT("offset"),
	CSTR_VIEW_INIT("compilers"), CSTR_VIEW_INIT("args"), CSTR_VIEW_INIT("source"),
	CSTR_VIEW_INIT("filters"), CSTR_VIEW_INIT("headers"), CSTR_VIEW_INIT("share")
};

static_assert(array_size(godbolt_arg_names) == GODBOLT_ARG_COUNT);
static_assert(GODBOLT_ARG_FAMILY == GODBOLT_ARG_KIND + 1 &&
              GODBOLT_ARG_ARCHITECTURE == GODBOLT_ARG_KIND + 2, "the order of values[]");

/* The reason of the refusal of each argument. */
static struct cstr_view const godbolt_arg_rules[] = {
	[GODBOLT_ARG_ACTION] = CSTR_VIEW_INIT(
		"action must be languages, compilers, kinds, families or architectures."),
	[GODBOLT_ARG_LANGUAGE] = CSTR_VIEW_INIT(
		"language must have 1 to 64 bytes of a-z, 0-9, +, - and _."),
	[GODBOLT_ARG_IDS] = CSTR_VIEW_INIT(
		"ids must be an array of 1 to 16 different ids. An id has 1 to 128 "
		"bytes of A-Z, a-z, 0-9, +, -, . and _, and it is not . or .."),
	[GODBOLT_ARG_MATCH] = CSTR_VIEW_INIT(
		"match must be a string of at most 256 bytes with at most 8 words."),
	[GODBOLT_ARG_KIND] = CSTR_VIEW_INIT(
		"kind must be a string of 1 to 128 bytes without a NUL byte."),
	[GODBOLT_ARG_FAMILY] = CSTR_VIEW_INIT(
		"family must be a string of 1 to 128 bytes without a NUL byte."),
	[GODBOLT_ARG_ARCHITECTURE] = CSTR_VIEW_INIT(
		"architecture must be a string of 1 to 128 bytes without a NUL byte."),
	[GODBOLT_ARG_LIMIT] = CSTR_VIEW_INIT(
		"limit must be a whole number from 1 to 2000."),
	[GODBOLT_ARG_COMPILERS] = CSTR_VIEW_INIT(
		"compilers must be an array of 1 to 16 different compiler ids. A "
		"compiler id has 1 to 128 bytes of A-Z, a-z, 0-9, +, -, . and _, and "
		"it is not . or .."),
	[GODBOLT_ARG_ARGS] = CSTR_VIEW_INIT(
		"args must be an array of 1 to 8 strings. Each string has at most "
		"8192 bytes and no NUL byte."),
	[GODBOLT_ARG_SOURCE] = CSTR_VIEW_INIT(
		"source must be a string of at most 1 MiB without a NUL byte."),
	[GODBOLT_ARG_FILTERS] = CSTR_VIEW_INIT(
		"filters must be an object whose members are among intel, demangle, "
		"directives, commentOnly, labels, libraryCode and trim. Each member "
		"is true or false."),
	[GODBOLT_ARG_HEADERS] = CSTR_VIEW_INIT(
		"headers must be true or false."),
	[GODBOLT_ARG_SHARE] = CSTR_VIEW_INIT(
		"share must be true or false."),
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
		        GODBOLT_BIT(SOURCE) | GODBOLT_BIT(FILTERS) | GODBOLT_BIT(SHARE) |
		        GODBOLT_BIT(OFFSET),
		.needs = GODBOLT_BIT(COMPILERS) | GODBOLT_BIT(SOURCE)
	},
	[GODBOLT_PREPROCESS] = {
		.args = GODBOLT_BIT(COMPILERS) | GODBOLT_BIT(ARGS) | GODBOLT_BIT(LANGUAGE) |
		        GODBOLT_BIT(SOURCE) | GODBOLT_BIT(SHARE) | GODBOLT_BIT(OFFSET),
		.needs = GODBOLT_BIT(COMPILERS) | GODBOLT_BIT(SOURCE)
	},
	[GODBOLT_MACROS] = {
		.args = GODBOLT_BIT(COMPILERS) | GODBOLT_BIT(ARGS) | GODBOLT_BIT(LANGUAGE) |
		        GODBOLT_BIT(SOURCE) | GODBOLT_BIT(SHARE) | GODBOLT_BIT(OFFSET),
		.needs = GODBOLT_BIT(COMPILERS)
	},
	[GODBOLT_TARGETS] = {
		.args = GODBOLT_BIT(COMPILERS) | GODBOLT_BIT(ARGS) | GODBOLT_BIT(LANGUAGE) |
		        GODBOLT_BIT(HEADERS) | GODBOLT_BIT(OFFSET),
		.needs = GODBOLT_BIT(COMPILERS)
	},
};

#undef GODBOLT_BIT

/* The filters of a compilation, in the order of the request body. A
 * filter whose bit is 0 is always false. The argument filters can set
 * each of the others.
 */
static struct {
	struct cstr_view name;
	uint8_t          bit;
} const godbolt_filters[] = {
	{ CSTR_VIEW_INIT("binary"), 0 },
	{ CSTR_VIEW_INIT("binaryObject"), 0 },
	{ CSTR_VIEW_INIT("commentOnly"), 1u << 0 },
	{ CSTR_VIEW_INIT("debugCalls"), 0 },
	{ CSTR_VIEW_INIT("demangle"), 1u << 1 },
	{ CSTR_VIEW_INIT("directives"), 1u << 2 },
	{ CSTR_VIEW_INIT("execute"), 0 },
	{ CSTR_VIEW_INIT("intel"), 1u << 3 },
	{ CSTR_VIEW_INIT("labels"), 1u << 4 },
	{ CSTR_VIEW_INIT("libraryCode"), 1u << 5 },
	{ CSTR_VIEW_INIT("trim"), 1u << 6 },
};

enum { GODBOLT_FILTERS = array_size(godbolt_filters) };

/* The filters that the Compiler Explorer site sets by default:
 * commentOnly, demangle, directives, intel and labels.
 */
static constexpr uint8_t GODBOLT_SITE_FILTERS = 0x1f;

/* The first line of each section of a record. */
static struct cstr_view const godbolt_sections[] = {
	[GODBOLT_SEC_ASM]    = CSTR_VIEW_INIT("--- asm ---\n"),
	[GODBOLT_SEC_STDOUT] = CSTR_VIEW_INIT("--- stdout ---\n"),
	[GODBOLT_SEC_STDERR] = CSTR_VIEW_INIT("--- stderr ---\n"),
	[GODBOLT_SEC_PP]     = CSTR_VIEW_INIT("--- pp ---\n"),
	[GODBOLT_SEC_MACROS] = CSTR_VIEW_INIT("--- macros ---\n"),
};

/* The largest number of sections of a record. */
enum { GODBOLT_SECTIONS = 3 };

/* How a tool compiles with a kind of driver. */
struct godbolt_recipe {
	struct cstr_view flags;    /* the arguments before the argument string */
	struct cstr_view options;  /* the member compilerOptions of the body */
	uint32_t         from;     /* enum godbolt_from */
	uint32_t         windows;  /* 1 if /TC comes before the flags, or /TP
	                              for a language other than c */
};

#define GODBOLT_PP \
        CSTR_VIEW_INIT("{\"producePp\":{\"filter-headers\":false,\"clang-format\":false}," \
                       "\"skipAsm\":true}")
#define GODBOLT_SKIP_ASM CSTR_VIEW_INIT("{\"skipAsm\":true}")

/* The recipes of the tools for each kind of driver. These are the recipes
 * of godbolt-c with one change: godbolt-c refused a kind of compiler
 * without a recipe, and here such a kind gets the recipe of GCC. Compiler
 * Explorer itself appends -E for the preprocessed view of any C or C++
 * compiler. An MSVC compiler cannot output its predefined macros.
 */
static struct godbolt_recipe const godbolt_recipes[GODBOLT_TOOLS][GODBOLT_DRIVERS] = {
	[GODBOLT_COMPILE] = {
		[GODBOLT_GNU]      = { CSTR_VIEW_INIT(""), CSTR_VIEW_INIT("{}"), GODBOLT_FROM_ASM, 0 },
		[GODBOLT_MSVC]     = { CSTR_VIEW_INIT(""), CSTR_VIEW_INIT("{}"), GODBOLT_FROM_ASM, 0 },
		[GODBOLT_CLANG_CL] = { CSTR_VIEW_INIT(""), CSTR_VIEW_INIT("{}"), GODBOLT_FROM_ASM, 0 },
	},
	[GODBOLT_PREPROCESS] = {
		[GODBOLT_GNU]      = { CSTR_VIEW_INIT("-E"), GODBOLT_PP, GODBOLT_FROM_PP, 0 },
		[GODBOLT_MSVC]     = { CSTR_VIEW_INIT("/EP"), GODBOLT_SKIP_ASM, GODBOLT_FROM_STDOUT, 1 },
		[GODBOLT_CLANG_CL] = { CSTR_VIEW_INIT("/EP"), GODBOLT_SKIP_ASM, GODBOLT_FROM_STDOUT, 1 },
	},
	[GODBOLT_MACROS] = {
		[GODBOLT_GNU]      = { CSTR_VIEW_INIT("-E -dM"), GODBOLT_PP, GODBOLT_FROM_PP, 0 },
		[GODBOLT_MSVC]     = { CSTR_VIEW_INIT(""), CSTR_VIEW_INIT(""), GODBOLT_FROM_NONE, 0 },
		[GODBOLT_CLANG_CL] = { CSTR_VIEW_INIT("/E /clang:-dM"), GODBOLT_SKIP_ASM,
		                       GODBOLT_FROM_STDOUT, 1 },
	},
	[GODBOLT_TARGETS] = {
		[GODBOLT_GNU]      = { CSTR_VIEW_INIT("-E"), GODBOLT_PP, GODBOLT_FROM_PP, 0 },
		[GODBOLT_MSVC]     = { CSTR_VIEW_INIT("/EP"), GODBOLT_SKIP_ASM, GODBOLT_FROM_STDOUT, 1 },
		[GODBOLT_CLANG_CL] = { CSTR_VIEW_INIT("/EP"), GODBOLT_SKIP_ASM, GODBOLT_FROM_STDOUT, 1 },
	},
};

#undef GODBOLT_SKIP_ASM
#undef GODBOLT_PP

/* struct godbolt_args contains the arguments of a call after the checks.
 * godbolt_args_read() stores views into the JSON arguments of the call,
 * and godbolt_create() copies the texts to the blob of struct godbolt.
 */
struct godbolt_args {
	struct cstr_view ids[GODBOLT_IDS];
	struct cstr_view strings[GODBOLT_ARGS];
	struct cstr_view words[GODBOLT_WORDS];
	struct cstr_view language;
	struct cstr_view source;
	struct cstr_view values[3]; /* the arguments kind, family and
	                               architecture, in this order */
	uint64_t         limit;
	uint8_t          n_ids;
	uint8_t          n_strings;
	uint8_t          n_words;
	uint8_t          action;    /* enum godbolt_action */
	uint8_t          filters;   /* bits of godbolt_filters[] */
	uint8_t          headers;   /* the argument headers */
	uint8_t          share;     /* the argument share */
	uint8_t          paged;     /* 1 if the argument offset is above 0 */
};

/* The state of the record of the compilation that the call writes.
 * godbolt_next() resets it for the next compilation.
 */
struct godbolt_record {
	struct json_object *secs[GODBOLT_SECTIONS];  /* the texts of the
	                                                sections */
	size_t              byte;       /* written bytes of the text of the
	                                   section */
	size_t              item;       /* written lines of the section */
	uint8_t             sec_of[GODBOLT_SECTIONS];  /* enum godbolt_sec
	                                                  of secs[i] */
	uint8_t             n_sec;      /* the number of sections */
	uint8_t             sec;        /* the section being written */
	uint8_t             opened;     /* 1 once the first line of the
	                                   section is written */
	uint8_t             headed;     /* 1 once the first line of the
	                                   record is written */
	uint8_t             marked;     /* 1 if a line of the section marks a
	                                   cut */
	uint8_t             cut;        /* 1 once a note on a cut is written */
	uint8_t             truncated;  /* 1 if the member truncated of the
	                                   response is true */
	uint8_t             no_pp;      /* 1 if the response contains no text
	                                   of the preprocessor */
	uint32_t            probed;     /* 1 if probe contains the facts of
	                                   the record */
};

/* A call of a tool. The call's own word points to it, and drop() frees
 * it. The texts of the call's arguments are copies in blob, at the
 * end of the same allocation.
 */
struct godbolt {
	struct loop_timer            timer;     /* sends the request again */
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
	char                        *body;      /* the state of the call, which
	                                           begins with {"sessions":[, or
	                                           from GODBOLT_SKIP on the body
	                                           of a compilation */
	struct godbolt_record        rec;       /* the record being written */
	struct godbolt_args          args;      /* the arguments, copied to
	                                           blob, with the words in lower
	                                           case */
	struct cstr_view             resp;      /* the body of the last response,
	                                           valid until the next request.
	                                           resp.data is not nullptr after
	                                           godbolt_result() stores it. */
	size_t                       head;      /* bytes of body before the part
	                                           that differs for the state and
	                                           for each compilation */
	size_t                       len;       /* bytes of the state or of the
	                                           compilation in body */
	size_t                       off;       /* bytes of the source escaped,
	                                           or of the response parsed */
	size_t                       start;     /* the offset in the list of the
	                                           element being parsed */
	size_t                       written;   /* bytes of output */
	struct cstr_view             escaped;   /* the escaped argument string
	                                           that godbolt_cat_arguments()
	                                           wrote last to body */
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
	uint16_t                     found;     /* bit i is set if args.ids[i]
	                                           is in the list */
	uint8_t                      drivers[GODBOLT_IDS];  /* enum
	                                                       godbolt_driver of
	                                                       args.ids[i] */
	uint8_t                      tool;      /* enum godbolt_tool */
	uint8_t                      phase;     /* enum godbolt_phase */
	uint8_t                      wait;      /* enum godbolt_wait */
	uint8_t                      kind_of;   /* enum godbolt_req */
	uint8_t                      list;      /* enum godbolt_array */
	uint8_t                      jobs;      /* compilations of the call,
	                                           args.n_ids times
	                                           args.n_strings */
	uint8_t                      job;       /* the compilation, or in the
	                                           phase GODBOLT_STATE the next
	                                           compilation to add to the
	                                           state */
	uint8_t                      full;      /* 1 after the output reached
	                                           GODBOLT_FULL bytes */
	char                         why[GODBOLT_REASON_MAX + 120];
	char                         url[1024];
	char                         blob[];
};

static_assert(GODBOLT_URL_MAX + sizeof "/api/compiler/" + 3 * GODBOLT_ID_MAX +
              sizeof "/compile" <= sizeof ((struct godbolt *)nullptr)->url);
static_assert(GODBOLT_URL_MAX + sizeof "/api/compilers/" + 3 * GODBOLT_LANGUAGE_MAX +
              sizeof "?fields=" GODBOLT_FIELDS <= sizeof ((struct godbolt *)nullptr)->url);

#undef GODBOLT_FIELDS

/* Returns the lower-case form of the ASCII letter @p c, or @p c. */
static char
godbolt_lower (char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c | 0x20) : c;
}

/* Returns whether @p text contains @p word in any letter case. @p word is
 * in lower case and not empty.
 */
static bool
godbolt_has (struct cstr_view text,
             struct cstr_view word)
{
	for (size_t i = 0; i + word.len <= text.len; ++i) {
		size_t k = 0;
		while (k < word.len && godbolt_lower(text.data[i + k]) == word.data[k])
			++k;
		if (k == word.len)
			return true;
	}
	return false;
}

/* Returns @p t, or @p otherwise if @p t is empty. */
static struct cstr_view
godbolt_or (struct cstr_view t,
            struct cstr_view otherwise)
{
	return t.len ? t : otherwise;
}

/* Returns the length of the scheme of the URL @p t with its "://", which
 * is 7 for http and 8 for https. Returns 0 for another scheme, for a URL
 * that ends after its scheme, and for a URL with a byte that is not
 * printable ASCII or is a space.
 */
static size_t
godbolt_scheme (struct cstr_view t)
{
	struct cstr_view const http = CSTR_VIEW("http://");
	struct cstr_view const https = CSTR_VIEW("https://");
	size_t const scheme = cstr_view_starts(t, http) ? http.len
	                    : cstr_view_starts(t, https) ? https.len : 0;
	if (!scheme || scheme == t.len)
		return 0;
	for (size_t i = 0; i < t.len; ++i)
		if (t.data[i] <= ' ' || t.data[i] > '~')
			return 0;
	return scheme;
}

/* Returns the member @p name of @p obj, or nullptr if @p obj has no such
 * member.
 */
static struct json_object *
godbolt_get (struct json_object *obj,
             char const         *name)
{
	struct json_object *v = nullptr;
	return json_object_object_get_ex(obj, name, &v) ? v : nullptr;
}

/* Returns the string member @p name of @p obj, as jb_view() does. The
 * text is NUL-terminated.
 */
static struct cstr_view
godbolt_member (struct json_object *obj,
                char const         *name)
{
	return jb_view(godbolt_get(obj, name));
}

/* Returns @p max, or t.len if @p t is shorter. The function returns less if
 * the first @p max bytes of @p t would end inside a UTF-8 character, so
 * that a cut after the returned length does not split a character. The
 * cut moves back by at most 3 bytes.
 */
static size_t
godbolt_cut (struct cstr_view t,
             size_t           max)
{
	if (max >= t.len)
		return t.len;
	size_t n = max;
	for (size_t k = 0; k < 3 && n && ((unsigned char)t.data[n] & 0xc0) == 0x80; ++k)
		--n;
	return n;
}

/* Returns "s" if @p n is not 1, for the plural of a noun after a count, and
 * "" if it is.
 */
static char const *
godbolt_s (uint64_t n)
{
	return n == 1 ? "" : "s";
}

/* Copies @p t to @p d, and returns the end of the copy. */
static char *
godbolt_cat (char             *d,
             struct cstr_view  t)
{
	if (t.len)
		memcpy(d, t.data, t.len);
	return d + t.len;
}

/* Writes @p t to @p d, with each '+' as %2B. '+' is the only byte of an
 * id that needs an escape in the path of a URL. Returns the end of the
 * text in @p d.
 */
static char *
godbolt_cat_path (char             *d,
                  struct cstr_view  t)
{
	for (size_t i = 0; i < t.len; ++i) {
		if (t.data[i] == '+')
			d = godbolt_cat(d, CSTR_VIEW("%2B"));
		else
			*d++ = t.data[i];
	}
	return d;
}

/* Writes @p s as the inside of a JSON string to @p d. The caller provides
 * room for 6 * s.len bytes at @p d. Each quote, backslash and control byte
 * is written as an escape sequence. Returns the end of the text in @p d.
 */
static char *
godbolt_escape (char             *d,
                struct cstr_view  s)
{
	static char const hex[] = "0123456789abcdef";
	static char const letter[0x20] = {
		['\b'] = 'b', ['\t'] = 't', ['\n'] = 'n', ['\f'] = 'f', ['\r'] = 'r'
	};
	for (size_t i = 0; i < s.len; ++i) {
		unsigned char const c = (unsigned char)s.data[i];
		if (c >= 0x20 && c != '"' && c != '\\') {
			*d++ = (char)c;
			continue;
		}
		*d++ = '\\';
		if (c >= 0x20 || letter[c]) {
			*d++ = c >= 0x20 ? (char)c : letter[c];
			continue;
		}
		d = godbolt_cat(d, CSTR_VIEW("u00"));
		*d++ = hex[c >> 4];
		*d++ = hex[c & 15];
	}
	return d;
}

/* Writes the 11 filters of a compilation as a JSON object to @p d. The
 * filters that @p bits has are true. Returns the end of the text in
 * @p d.
 */
static char *
godbolt_cat_filters (char    *d,
                     uint8_t  bits)
{
	char sep = '{';
	for (size_t i = 0; i < GODBOLT_FILTERS; ++i) {
		*d++ = sep;
		*d++ = '"';
		d = godbolt_cat(d, godbolt_filters[i].name);
		d = godbolt_cat(d, bits & godbolt_filters[i].bit ? CSTR_VIEW("\":true")
		                                                 : CSTR_VIEW("\":false"));
		sep = ',';
	}
	*d++ = '}';
	return d;
}

/* Appends @p t to the call's output, unless an earlier write failed.
 * Returns 0, or call->e, the error number of the call's first failed
 * write.
 */
static int
godbolt_put (struct godbolt   *call,
             struct cstr_view  t)
{
	size_t taken = 0;
	if (!call->e && t.len)
		call->e = mcp_slot_write(call->slot, t, 0, &taken);
	call->written += taken;
	return call->e;
}

/* Formats @p fmt and @p ap into the @p size bytes at @p buf, cut at
 * size - 1 bytes and terminated with a NUL. Returns a view of the text, or
 * an empty view whose data is nullptr, with an empty text in @p buf, if
 * vsnprintf() fails.
 */
[[gnu::format(printf, 3, 0)]]
static struct cstr_view
godbolt_vformat (char       *buf,
                 size_t      size,
                 char const *fmt,
                 va_list     ap)
{
	int const n = vsnprintf(buf, size, fmt, ap);
	if (n < 0) {
		*buf = '\0';
		return (struct cstr_view){};
	}
	return (struct cstr_view){ buf, (size_t)n < size ? (size_t)n : size - 1 };
}

/* Appends the text that @p fmt and its arguments format to the call's
 * output. The text is cut at 511 bytes. Returns 0, or call->e.
 */
[[gnu::format(printf, 2, 3)]]
static int
godbolt_putf (struct godbolt *call,
              char const     *fmt,
              ...)
{
	char text[512];
	va_list ap;
	va_start(ap, fmt);
	struct cstr_view const t = godbolt_vformat(text, sizeof text, fmt, ap);
	va_end(ap);
	if (t.data)
		return godbolt_put(call, t);
	call->e = call->e ? call->e : EINVAL;
	return call->e;
}

/* Stores the text that @p fmt and its arguments format in call->why. Each
 * control byte of the text becomes a space. If the text cannot be
 * formatted, call->why is empty.
 */
[[gnu::format(printf, 2, 3)]]
static void
godbolt_why (struct godbolt *call,
             char const     *fmt,
             ...)
{
	va_list ap;
	va_start(ap, fmt);
	call->why_len = (uint32_t)godbolt_vformat(call->why, sizeof call->why, fmt, ap).len;
	va_end(ap);
	for (uint32_t i = 0; i < call->why_len; ++i)
		if ((unsigned char)call->why[i] < 0x20 || call->why[i] == 0x7f)
			call->why[i] = ' ';
}

/** Format a compiler lookup that preserves the call's language. */
static void
godbolt_catalog_hint (struct godbolt *call,
                      char const     *reason)
{
	godbolt_why(call, "%sCall godbolt_catalog with {\"action\":\"compilers\","
	            "\"language\":\"%.*s\"}. Optional match searches names, versions and "
	            "family tags. Use exact returned ids.", reason,
	            (int)call->args.language.len, call->args.language.data);
}

/* Ends the call in @p slot as failed with the text that @p fmt and its
 * arguments format. Returns GODBOLT_ENDED.
 */
[[gnu::format(printf, 2, 3)]]
static enum godbolt_go
godbolt_refuse (struct mcp_slot *slot,
                char const      *fmt,
                ...)
{
	char text[TOOL_TELL_MAX];
	va_list ap;
	va_start(ap, fmt);
	struct cstr_view const t = godbolt_vformat(text, sizeof text, fmt, ap);
	va_end(ap);
	mcp_slot_tell(slot, t.data ? t : CSTR_VIEW("The plugin could not format the reason of "
	                                           "the failure."), TOOL_FAILED);
	return GODBOLT_ENDED;
}

/* Ends the call after the first failure of its output, whose error number
 * is call->e. After ENOMEM or ESHUTDOWN, the call ends with the error
 * number. After any other error, for example EFBIG when the output has
 * reached its largest length, the call fails with the output that it has.
 * Returns GODBOLT_ENDED.
 */
static enum godbolt_go
godbolt_fail (struct godbolt *call)
{
	if (call->e == ENOMEM || call->e == ESHUTDOWN)
		mcp_slot_fail(call->slot, call->e);
	else
		mcp_slot_finish(call->slot, TOOL_FAILED);
	return GODBOLT_ENDED;
}

/* Sets the call's request to a request of the kind @p kind, with the body
 * @p body. The function writes the URL of the rule of @p kind, with
 * @p segment after the path, to call->url. The keep of a compilation is 1
 * for a call without offset, so that askai does not answer it with a
 * response older than a second, and 300 for a call with offset, so that
 * askai answers it with the stored response of the same call without
 * offset.
 */
static void
godbolt_request (struct godbolt   *call,
                 enum godbolt_req  kind,
                 struct cstr_view  segment,
                 struct cstr_view  body)
{
	struct godbolt_rule const *rule = &godbolt_rules[kind];
	char *u = godbolt_cat(call->url, (struct cstr_view){ call->cfg->url, call->cfg->len });
	u = godbolt_cat_path(godbolt_cat(u, rule->path), segment);
	*godbolt_cat(u, rule->tail) = '\0';
	call->req = (struct web_request){
		.url = call->url,
		.body = body,
		.accept = "application/json",
		.max = rule->max,
		.timeout = rule->timeout,
		.keep = kind == GODBOLT_POST_COMPILE && !call->args.paged ? 1 : rule->keep
	};
	call->kind_of = kind;
	call->attempt = 0;
}

/* Makes the next step read the failure in call->why as the result of the
 * call's request: godbolt_result() then returns GODBOLT_BAD. Returns
 * GODBOLT_WAIT.
 */
static enum godbolt_go
godbolt_refused (struct godbolt *call)
{
	call->wait = GODBOLT_REFUSED;
	(void)mcp_slot_ready(call->slot);
	return GODBOLT_WAIT;
}

/* Sets the call's timer to send the request again in @p seconds. Returns
 * GODBOLT_WAIT, or GODBOLT_ENDED after ENOMEM. If the timer cannot be
 * set, the next step reads the reason (godbolt_refused()).
 */
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
		return godbolt_refused(call);
	}
	call->wait = GODBOLT_TIMER;
	return GODBOLT_WAIT;
}

/* Sends the call's request. Returns GODBOLT_WAIT, or GODBOLT_ENDED. The
 * next step reads the response, or the reason if askai refused the request
 * (godbolt_refused()).
 */
static enum godbolt_go
godbolt_send (struct godbolt *call)
{
	int const e = mcp_slot_fetch(call->slot, &call->req);
	++call->attempt;
	if (!e)
		return GODBOLT_WAIT;
	if (e == EAGAIN && call->attempt < GODBOLT_ATTEMPTS)
		return godbolt_later(call, 1);
	if (e == ENOMEM) {
		mcp_slot_fail(call->slot, ENOMEM);
		return GODBOLT_ENDED;
	}
	godbolt_why(call, "%s", e == EAGAIN ? "askai sends too many requests at once"
	                                    : "askai refused the request");
	return godbolt_refused(call);
}

/* The timer function of a call. It sends the call's request again. */
static void
godbolt_fired (struct loop       *loop,
               struct loop_timer *timer)
{
	(void)loop;
	struct godbolt *call = timer ? container_of(timer, struct godbolt, timer) : nullptr;
	if (!call || !call->slot)
		return;
	call->wait = GODBOLT_IDLE;
	(void)godbolt_send(call);
}

/* Stores in call->why the reason of the response in call->resp, whose
 * status @p status is an error. The reason is the member message of a
 * JSON object, or else the first line of the body, and it has at most
 * GODBOLT_REASON_MAX bytes. A compilation with an unknown compiler id gets
 * 404, and its reason names godbolt_catalog.
 */
static void
godbolt_reason (struct godbolt *call,
                uint32_t        status)
{
	if (call->kind_of == GODBOLT_POST_COMPILE && status == 404) {
		godbolt_catalog_hint(call, "HTTP 404: Compiler Explorer rejected this compiler id. ");
		return;
	}
	struct cstr_view text = call->resp;
	struct json_object *doc = nullptr;
	if (text.len <= LOOP_STEP_PARSE) {
		json_tokener_reset(call->tok);
		doc = json_tokener_parse_ex(call->tok, text.data, (int)text.len);
		loop_work(call->loop, LOOP_PARSE, text.len);
		text = godbolt_or(godbolt_member(doc, "message"), text);
	}
	char const *nl = memchr(text.data, '\n', text.len);
	size_t const line = nl ? (size_t)(nl - text.data) : text.len;
	size_t const n = godbolt_cut(cstr_view_slice(text, 0, line), GODBOLT_REASON_MAX);
	godbolt_why(call, "HTTP %" PRIu32 "%s%.*s", status, n ? ": " : "", (int)n, text.data);
	json_object_put(doc);
}

/* Reads the result of the call's request, and stores the body of the
 * response in call->resp. Returns GODBOLT_GO for a whole response with a
 * status from 200 to 299. Returns GODBOLT_WAIT if the request is under
 * way, if the function set the timer to send it again, or if it could not
 * set the timer, when the next step returns GODBOLT_BAD. Returns
 * GODBOLT_BAD with the reason in call->why if the request failed or askai
 * refused it, or GODBOLT_ENDED if the function ended the call.
 */
static enum godbolt_go
godbolt_result (struct godbolt *call)
{
	if (call->wait == GODBOLT_REFUSED) {
		call->wait = GODBOLT_IDLE;
		return GODBOLT_BAD;
	}
	struct web_response r;
	int e = mcp_slot_fetched(call->slot, &r);
	if (e == EINPROGRESS)
		return GODBOLT_WAIT;
	call->wait = GODBOLT_IDLE;
	if (e || (!r.body.data && r.body.len) || !r.error) {
		godbolt_why(call, "askai has no response to the request");
		return GODBOLT_BAD;
	}
	if (r.e == ESHUTDOWN || r.e == ENOMEM) {
		mcp_slot_fail(call->slot, r.e);
		return GODBOLT_ENDED;
	}
	uint32_t const again = godbolt_rules[call->kind_of].again;
	bool const retry = r.e ? (r.e == EIO && again & GODBOLT_AGAIN_EIO) ||
	                         (r.e == ETIMEDOUT && again & GODBOLT_AGAIN_TIMEDOUT)
	                       : r.status == 429 || r.status == 502 || r.status == 503 ||
	                         (r.status == 504 && again & GODBOLT_AGAIN_504);
	if (retry && r.retry > GODBOLT_RETRY_MAX) {
		godbolt_why(call, "HTTP %" PRIu32 ", and Retry-After is %" PRIu32 " s",
		            r.status, r.retry);
		return GODBOLT_BAD;
	}
	if (retry && call->attempt < GODBOLT_ATTEMPTS) {
		uint32_t const wait = 1u << (call->attempt - 1);
		return godbolt_later(call, r.retry > wait ? r.retry : wait);
	}
	call->resp = r.body.data ? r.body : CSTR_VIEW("");
	/* The largest body of each rule is a whole number of KiB or MiB. */
	bool const mib = call->req.max >= 1 << 20;
	if (r.e == EFBIG)
		godbolt_why(call, "the response is larger than %zu %s",
		            call->req.max >> (mib ? 20 : 10), mib ? "MiB" : "KiB");
	else if (r.e)
		godbolt_why(call, "the request failed: %.*s",
		            (int)godbolt_cut(cstr_view(r.error), GODBOLT_REASON_MAX), r.error);
	else if (r.status < 200 || r.status > 299)
		godbolt_reason(call, r.status);
	else
		return GODBOLT_GO;
	return GODBOLT_BAD;
}

/* Returns the state of the parse of a JSON array after the byte @p c,
 * which is outside an element, in the state @p s. For a byte that begins
 * an element, the function returns GODBOLT_LIST_VALUE, and the parse of the
 * element begins with that byte.
 */
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

/* Returns the bytes of JSON text that the step may still parse. */
static uint64_t
godbolt_parse_room (struct godbolt const *call)
{
	uint64_t const room = loop_room(call->loop, LOOP_PARSE);
	uint64_t const values = loop_room(call->loop, LOOP_VALUES) * GODBOLT_VALUE_BYTES;
	return room < values ? room : values;
}

/* Counts @p n bytes of JSON text parsed as work of the step. */
static void
godbolt_parsed (struct godbolt const *call,
                uint64_t              n)
{
	loop_work(call->loop, LOOP_PARSE, n);
	loop_work(call->loop, LOOP_VALUES, n / GODBOLT_VALUE_BYTES);
}

/*
 * Parses the elements of the JSON array in call->resp, from call->off on,
 * as far as the bounds of the step allow. It calls @p fn for each element,
 * and frees the element after that. The element is nullptr for the JSON
 * value null. fn returns 0, or an error number that stops the parse.
 *
 * Returns 0, or the error number of fn. After the end of the array,
 * call->list is GODBOLT_LIST_END. If the text is not a JSON array, or if
 * an element has more than GODBOLT_ELEMENT_MAX bytes, call->list is
 * GODBOLT_LIST_BAD and call->why has the reason.
 */
static int
godbolt_list_parse (struct godbolt *call,
                    int           (*fn) (struct godbolt     *call,
                                         struct json_object *element))
{
	struct cstr_view const body = call->resp;
	uint64_t const room = godbolt_parse_room(call);
	uint64_t used = 0;
	int e = 0;
	while (!e && call->list < GODBOLT_LIST_END && used < room && call->off < body.len) {
		if (call->list != GODBOLT_LIST_VALUE) {
			call->list = godbolt_array_next(call->list, body.data[call->off]);
			call->start = call->off;
			/* A byte that begins an element is parsed with the element. */
			size_t const consumed = call->list != GODBOLT_LIST_VALUE;
			call->off += consumed;
			used += consumed;
			if (call->list == GODBOLT_LIST_BAD)
				godbolt_why(call, "the text is not a JSON array");
			continue;
		}
		struct cstr_view const part = cstr_view_slice(body, call->off, room - used);
		struct json_object *v = json_tokener_parse_ex(call->tok, part.data, (int)part.len);
		enum json_tokener_error const err = json_tokener_get_error(call->tok);
		size_t const took = err == json_tokener_success ? json_tokener_get_parse_end(call->tok)
		                                                : part.len;
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
	godbolt_parsed(call, used);
	if (!e && call->list < GODBOLT_LIST_END && call->off == body.len) {
		godbolt_why(call, "the text ends before the end of the JSON array");
		call->list = GODBOLT_LIST_BAD;
	}
	return e;
}

static_assert(GODBOLT_ELEMENT_MAX == 4 << 10, "the reason says 4 KiB");

/* Returns whether a compiler with the @p n family tags @p tags has the
 * family @p family. A compiler without tags has the family unknown.
 */
static bool
godbolt_tagged (struct cstr_view const *tags,
                size_t                  n,
                struct cstr_view        family)
{
	for (size_t i = 0; i < n; ++i)
		if (cstr_view_equal(tags[i], family))
			return true;
	return !n && cstr_view_equal(family, CSTR_VIEW("unknown"));
}

/* Returns whether an element of a list matches the words of the argument
 * match and the argument ids. Each word must be in one of the @p n texts
 * of @p f. @p f[0] is the id of the element. If the call has ids, the
 * function sets the bit of the id in call->found.
 */
static bool
godbolt_matches (struct godbolt         *call,
                 struct cstr_view const *f,
                 size_t                  n)
{
	for (uint8_t w = 0; w < call->args.n_words; ++w) {
		bool hit = false;
		for (size_t i = 0; !hit && i < n; ++i)
			hit = godbolt_has(f[i], call->args.words[w]);
		if (!hit)
			return false;
	}
	if (!call->args.n_ids)
		return true;
	for (uint8_t i = 0; i < call->args.n_ids; ++i) {
		if (cstr_view_equal(f[0], call->args.ids[i])) {
			call->found |= (uint16_t)(1u << i);
			return true;
		}
	}
	return false;
}

/* Writes @p t as a column of a row of the catalog. A tab, a newline, a
 * carriage return and a backslash become \t, \n, \r and \\. Returns 0, or
 * call->e.
 */
static int
godbolt_put_field (struct godbolt   *call,
                   struct cstr_view  t)
{
	static char const letter[] = { ['\t'] = 't', ['\n'] = 'n', ['\r'] = 'r', ['\\'] = '\\' };
	if (!t.len)
		return call->e;
	size_t from = 0;
	for (size_t i = 0; i < t.len; ++i) {
		unsigned char const c = (unsigned char)t.data[i];
		if (c >= sizeof letter || !letter[c])
			continue;
		godbolt_put(call, (struct cstr_view){ t.data + from, i - from });
		godbolt_put(call, (struct cstr_view){ (char const[]){ '\\', letter[c] }, 2 });
		from = i + 1;
	}
	return godbolt_put(call, (struct cstr_view){ t.data + from, t.len - from });
}

/* Counts an element of the list of languages, and writes it as a row if the
 * filters select it and the output has fewer rows than the limit. Returns
 * 0, or call->e.
 */
static int
godbolt_language_row (struct godbolt     *call,
                      struct json_object *element)
{
	struct cstr_view const id = godbolt_member(element, "id");
	if (!id.len) {
		++call->malformed;
		return 0;
	}
	++call->total;
	struct cstr_view const f[] = { id, godbolt_member(element, "name") };
	if (!godbolt_matches(call, f, array_size(f)) || ++call->matching > call->args.limit)
		return 0;
	++call->listed;
	godbolt_put_field(call, f[0]);
	godbolt_put(call, CSTR_VIEW("\t"));
	godbolt_put_field(call, f[1]);
	return godbolt_put(call, CSTR_VIEW("\n"));
}

/* Adds 1 to the count of the string @p key in the counts of the catalog.
 * If the counts contain GODBOLT_KEYS keys and @p key is not one of them,
 * the function sets call->list to GODBOLT_LIST_BAD, which ends the parse
 * of the list, and stores the reason in call->why. Returns 0, or call->e,
 * which is ENOMEM if json-c runs out of memory.
 */
static int
godbolt_count (struct godbolt *call,
               char const     *key)
{
	struct json_object *n = nullptr;
	if (json_object_object_get_ex(call->doc, key, &n)) {
		json_object_int_inc(n, 1);
		return call->e;
	}
	if (json_object_object_length(call->doc) == GODBOLT_KEYS) {
		godbolt_why(call, "the list has more than 256 different %s",
		            godbolt_actions[call->args.action].name.data);
		call->list = GODBOLT_LIST_BAD;
		return call->e;
	}
	struct json_object *one = json_object_new_int(1);
	if (!one || json_object_object_add(call->doc, key, one)) {
		json_object_put(one);
		call->e = call->e ? call->e : ENOMEM;
	}
	return call->e;
}

/* Counts each of the @p n family tags @p tags once, or the family unknown
 * if there is no tag. Returns 0, or call->e.
 */
static int
godbolt_count_families (struct godbolt         *call,
                        struct cstr_view const *tags,
                        size_t                  n)
{
	for (size_t i = 0; i < n; ++i) {
		size_t k = 0;
		while (k < i && !cstr_view_equal(tags[i], tags[k]))
			++k;
		if (k == i)
			godbolt_count(call, tags[i].data);
	}
	return n ? call->e : godbolt_count(call, "unknown");
}

/* The columns of a row of the catalog before the family tags. */
enum { GODBOLT_COLUMNS = 6 };

/* Counts an element of a list of compilers. If the filters select the
 * compiler, the function writes the compiler as a row, or it counts the
 * compiler's kind, family tags or architecture. An element is left out
 * if it has no id, more than GODBOLT_TAGS family tags, or more than
 * GODBOLT_VALUE_MAX bytes in its kind, its architecture or a family tag.
 * Returns 0, or call->e.
 */
static int
godbolt_compiler_row (struct godbolt     *call,
                      struct json_object *element)
{
	struct json_object *tags = godbolt_get(element, "compilerCategories");
	size_t const n_tags = jb_len(tags);
	/* The columns, and then the family tags that are not empty. */
	struct cstr_view f[GODBOLT_COLUMNS + GODBOLT_TAGS] = {
		godbolt_member(element, "id"),
		godbolt_member(element, "name"),
		godbolt_or(godbolt_member(element, "compilerType"), CSTR_VIEW("default")),
		godbolt_member(element, "semver"),
		godbolt_or(godbolt_member(element, "instructionSet"), CSTR_VIEW("unknown")),
		godbolt_member(element, "releaseTrack"),
	};
	size_t n = GODBOLT_COLUMNS;
	bool valid = f[0].len && n_tags <= GODBOLT_TAGS && f[2].len <= GODBOLT_VALUE_MAX &&
	             f[4].len <= GODBOLT_VALUE_MAX;
	for (size_t i = 0; valid && i < n_tags; ++i) {
		f[n] = jb_view(json_object_array_get_idx(tags, i));
		valid = f[n].len <= GODBOLT_VALUE_MAX;
		n += f[n].len != 0;
	}
	if (!valid) {
		++call->malformed;
		return 0;
	}
	++call->total;
	struct cstr_view const *const t = f + GODBOLT_COLUMNS;
	struct cstr_view const *const v = call->args.values;
	if ((v[0].len && !cstr_view_equal(f[2], v[0])) ||
	    (v[1].len && !godbolt_tagged(t, n - GODBOLT_COLUMNS, v[1])) ||
	    (v[2].len && !cstr_view_equal(f[4], v[2])) ||
	    !godbolt_matches(call, f, n))
		return 0;
	++call->matching;
	switch (call->args.action) {
	case GODBOLT_KINDS:
		return godbolt_count(call, f[2].data);
	case GODBOLT_ARCHITECTURES:
		return godbolt_count(call, f[4].data);
	case GODBOLT_FAMILIES:
		return godbolt_count_families(call, t, n - GODBOLT_COLUMNS);
	}
	if (call->listed == call->args.limit)
		return 0;
	++call->listed;
	for (size_t i = 0; i < n; ++i) {
		godbolt_put_field(call, f[i]);
		godbolt_put(call, i < GODBOLT_COLUMNS ? CSTR_VIEW("\t")
		                  : i + 1 < n ? CSTR_VIEW(",") : (struct cstr_view){});
	}
	return godbolt_put(call, n > GODBOLT_COLUMNS ? CSTR_VIEW("\n") : CSTR_VIEW("unknown\n"));
}

/* A key of the counts of the catalog, and its count. */
struct godbolt_tally {
	char const *key;
	int64_t     count;
};

/* Compares the keys of two struct godbolt_tally byte by byte, for
 * qsort().
 */
static int
godbolt_tally_cmp (void const *a,
                   void const *b)
{
	return strcmp(((struct godbolt_tally const *)a)->key,
	              ((struct godbolt_tally const *)b)->key);
}

/* Writes the @p n counts of the catalog in the byte order of their keys,
 * as many as the limit allows, and stores the number of rows in
 * call->listed. @p n is from 1 to GODBOLT_KEYS, and a key is at most
 * GODBOLT_VALUE_MAX bytes long, so one step writes them. Returns 0, or
 * call->e, which is ENOMEM if memory runs out.
 */
static int
godbolt_tallies (struct godbolt *call,
                 size_t          n)
{
	size_t const rows = n < call->args.limit ? n : call->args.limit;
	call->listed = (uint32_t)rows;
	struct godbolt_tally *t = malloc(n * sizeof *t);
	if (!t) {
		call->e = call->e ? call->e : ENOMEM;
		return call->e;
	}
	size_t i = 0;
	struct json_object_iter it;
	json_object_object_foreachC(call->doc, it)
		t[i++] = (struct godbolt_tally){ it.key, json_object_get_int64(it.val) };
	qsort(t, n, sizeof *t, godbolt_tally_cmp);
	for (size_t k = 0; k < rows; ++k) {
		godbolt_put_field(call, cstr_view(t[k].key));
		godbolt_putf(call, "\t%" PRId64 "\n", t[k].count);
	}
	free(t);
	t = nullptr;
	return call->e;
}

/* Writes the ids of the call that are not in the list to @p d, separated by
 * ", ", and a NUL. @p d has room for GODBOLT_IDS * (GODBOLT_ID_MAX + 2) + 1
 * bytes.
 */
static void
godbolt_absent (struct godbolt const *call,
                char                 *d)
{
	char *p = d;
	for (uint8_t i = 0; i < call->args.n_ids; ++i) {
		if (call->found >> i & 1)
			continue;
		p = godbolt_cat(p, p == d ? (struct cstr_view){} : CSTR_VIEW(", "));
		p = godbolt_cat(p, call->args.ids[i]);
	}
	*p = '\0';
}

/* Ends a call of godbolt_catalog after the end of the list. If an id of
 * the argument ids is not in the list, the call fails. Otherwise the
 * function writes the counts of an action that counts, a line on the
 * elements that the plugin left out, and the last line of the output with
 * the numbers of the items. Returns GODBOLT_ENDED.
 */
static enum godbolt_go
godbolt_catalog_end (struct godbolt *call)
{
	bool const languages = call->args.action == GODBOLT_LANGUAGES;
	if (call->found != (1u << call->args.n_ids) - 1) {
		char absent[GODBOLT_IDS * (GODBOLT_ID_MAX + 2) + 1];
		godbolt_absent(call, absent);
		return godbolt_refuse(call->slot, "These ids were not selected: %s. Retry with "
		                      "action \"%s\" and without ids or other filters.", absent,
		                      languages ? "languages" : "compilers");
	}
	bool const counts = call->args.action > GODBOLT_COMPILERS;
	uint32_t const keys = counts ? (uint32_t)json_object_object_length(call->doc) : 0;
	if (keys)
		godbolt_tallies(call, keys);
	if (call->malformed)
		godbolt_putf(call, "The plugin left out %" PRIu32 " element%s of the list. An "
		             "element must have a string member id%s.\n", call->malformed,
		             godbolt_s(call->malformed), languages ? "" : ", at most 64 family "
		             "tags, and at most 128 bytes in its kind, its architecture and each "
		             "family tag");
	/* The first sentence of the last line counts the listed items, and the
	 * second sentence counts all items of the list. */
	typeof(godbolt_actions[0]) *const act = &godbolt_actions[call->args.action];
	if (counts)
		godbolt_putf(call, "Listed %" PRIu32 " of the %" PRIu32 " %s of the %" PRIu32
		             " matching compiler%s. ", call->listed, keys,
		             keys == 1 ? act->singular : act->name.data, call->matching,
		             godbolt_s(call->matching));
	else
		godbolt_putf(call, "Listed %" PRIu32 " of %" PRIu32 " matching %s%s. ",
		             call->listed, call->matching, act->singular,
		             godbolt_s(call->matching));
	if (languages)
		godbolt_putf(call, "Compiler Explorer has %" PRIu32 " language%s.\n", call->total,
		             godbolt_s(call->total));
	else
		godbolt_putf(call, "The list of language %.*s has %" PRIu32 " compiler%s.\n",
		             (int)call->args.language.len, call->args.language.data, call->total,
		             godbolt_s(call->total));
	if (call->e)
		return godbolt_fail(call);
	mcp_slot_finish(call->slot, 0);
	return GODBOLT_ENDED;
}

static_assert(GODBOLT_TAGS == 64 && GODBOLT_VALUE_MAX == 128 && GODBOLT_KEYS == 256,
              "the texts of the catalog have these numbers");

/* Stores the kind of driver of a compiler of the call, if the element of
 * the list of compilers is one of them. When the list has had each
 * compiler of the call, the function sets call->list to GODBOLT_LIST_END,
 * which ends the parse. Returns 0.
 */
static int
godbolt_driver_row (struct godbolt     *call,
                    struct json_object *element)
{
	struct cstr_view const id = godbolt_member(element, "id");
	struct cstr_view const kind = godbolt_member(element, "compilerType");
	for (uint8_t i = 0; i < call->args.n_ids; ++i) {
		if (call->found >> i & 1 || !cstr_view_equal(id, call->args.ids[i]))
			continue;
		call->found |= (uint16_t)(1u << i);
		call->drivers[i] = cstr_view_equal(kind, CSTR_VIEW("win32-vc")) ? GODBOLT_MSVC
		                 : cstr_view_equal(kind, CSTR_VIEW("clang-cl")) ? GODBOLT_CLANG_CL
		                 : GODBOLT_GNU;
	}
	if (call->found == (1u << call->args.n_ids) - 1)
		call->list = GODBOLT_LIST_END;
	return 0;
}

/* Ends the parse of the list of compilers of a tool that compiles. If a
 * compiler of the call is not in the list, the call fails. Otherwise the
 * next step begins the state of a call with share, or the first
 * compilation.
 */
static enum godbolt_go
godbolt_drivers_end (struct godbolt *call)
{
	if (call->found != (1u << call->args.n_ids) - 1) {
		char absent[GODBOLT_IDS * (GODBOLT_ID_MAX + 2) + 1];
		godbolt_absent(call, absent);
		godbolt_catalog_hint(call, "");
		return godbolt_refuse(call->slot, "These compiler ids are not in the list of "
		                      "language %.*s: %s. %s", (int)call->args.language.len,
		                      call->args.language.data, absent, call->why);
	}
	call->phase = call->args.share ? GODBOLT_SHARE : GODBOLT_JOB;
	return GODBOLT_GO;
}

/* Sends the request of the list of languages, or of the list of compilers
 * of the call's language. The next step reads the response. Returns
 * GODBOLT_WAIT, or GODBOLT_ENDED.
 */
static enum godbolt_go
godbolt_list_send (struct godbolt *call)
{
	if (call->args.action == GODBOLT_LANGUAGES)
		godbolt_request(call, GODBOLT_GET_LANGUAGES, (struct cstr_view){}, (struct cstr_view){});
	else
		godbolt_request(call, GODBOLT_GET_COMPILERS, call->args.language, (struct cstr_view){});
	call->phase = GODBOLT_LIST;
	return godbolt_send(call);
}

/* The phase GODBOLT_LIST. It reads the response to the request of the
 * list. For godbolt_catalog, it writes the first line of the output.
 */
static enum godbolt_go
godbolt_list (struct godbolt *call)
{
	enum godbolt_go const got = godbolt_result(call);
	if (got == GODBOLT_BAD)
		return godbolt_refuse(call->slot, "askai could not get the list of %s from "
		                      "Compiler Explorer (%s).", call->args.action == GODBOLT_LANGUAGES
		                      ? "languages" : "compilers", call->why);
	if (got != GODBOLT_GO)
		return got;
	json_tokener_reset(call->tok);
	call->off = 0;
	call->list = GODBOLT_LIST_OPEN;
	call->phase = GODBOLT_ROWS;
	if (call->tool != GODBOLT_CATALOG)
		return GODBOLT_GO;
	if (call->args.action > GODBOLT_COMPILERS && !(call->doc = json_object_new_object())) {
		mcp_slot_fail(call->slot, ENOMEM);
		return GODBOLT_ENDED;
	}
	return godbolt_put(call, godbolt_actions[call->args.action].header) ? godbolt_fail(call)
	                                                                    : GODBOLT_GO;
}

/* The phase GODBOLT_ROWS. It parses the next elements of the list. After
 * the end of the list, godbolt_catalog ends the call, and the other tools
 * begin to compile.
 */
static enum godbolt_go
godbolt_rows (struct godbolt *call)
{
	bool const catalog = call->tool == GODBOLT_CATALOG;
	if (godbolt_list_parse(call, !catalog ? godbolt_driver_row
	                             : call->args.action == GODBOLT_LANGUAGES ? godbolt_language_row
	                             : godbolt_compiler_row))
		return godbolt_fail(call);
	if (call->list == GODBOLT_LIST_BAD)
		return godbolt_refuse(call->slot, "Compiler Explorer sent a list that is not "
		                      "valid: %s.", call->why);
	if (call->list != GODBOLT_LIST_END)
		return GODBOLT_GO;
	return catalog ? godbolt_catalog_end(call) : godbolt_drivers_end(call);
}

/* Returns the largest number of bytes that the state of the call can need
 * after the source in the body.
 */
static size_t
godbolt_state_size (struct godbolt const *call)
{
	size_t need = 0;
	for (uint32_t j = 0; j < call->jobs; ++j)
		need += 6 * call->args.strings[j % call->args.n_strings].len + GODBOLT_ID_MAX + 512;
	return need;
}

/* The phase GODBOLT_ESCAPE. It escapes the next slice of the source into
 * the body, after {"sessions":[{"source":". The text up to the end of the
 * source is the same for the state and for every compilation. The first
 * step allocates the body with room for an escape of 6 bytes for each byte
 * of the source, and for the state of a call with share, so that no step
 * copies the body to a larger allocation.
 */
static enum godbolt_go
godbolt_escape_source (struct godbolt *call)
{
	struct cstr_view const prefix = CSTR_VIEW("{\"sessions\":[{\"source\":\"");
	if (!call->body) {
		size_t const cap = prefix.len + 6 * call->args.source.len + GODBOLT_TAIL_MAX +
		                   (call->args.share ? godbolt_state_size(call) : 0);
		call->body = malloc(cap);
		if (!call->body) {
			mcp_slot_fail(call->slot, ENOMEM);
			return GODBOLT_ENDED;
		}
		call->head = (size_t)(godbolt_cat(call->body, prefix) - call->body);
	}
	struct cstr_view const part = cstr_view_slice(call->args.source, call->off,
	                                              loop_room(call->loop, LOOP_ESCAPE));
	call->head = (size_t)(godbolt_escape(call->body + call->head, part) - call->body);
	call->off += part.len;
	loop_work(call->loop, LOOP_ESCAPE, part.len);
	if (call->off < call->args.source.len)
		return GODBOLT_GO;
	if (call->head - GODBOLT_SKIP > WEB_SEND_MAX - GODBOLT_TAIL_MAX)
		return godbolt_refuse(call->slot, "The body of a request would be longer than "
		                      "4 MiB, because the source has %zu bytes as a JSON string.",
		                      call->head - prefix.len);
	if (call->tool != GODBOLT_COMPILE)
		return godbolt_list_send(call);
	call->phase = call->args.share ? GODBOLT_SHARE : GODBOLT_JOB;
	return GODBOLT_GO;
}

static_assert(WEB_SEND_MAX == 4 << 20, "the reason says 4 MiB");

/* Returns the recipe of the compilation @p job of the call. */
static struct godbolt_recipe const *
godbolt_job_recipe (struct godbolt const *call,
                    uint32_t              job)
{
	return &godbolt_recipes[call->tool][call->drivers[job / call->args.n_strings]];
}

/* Writes the arguments of a compilation with the recipe @p r and the
 * argument string @p a to @p d, which is in the body, as the inside of a
 * JSON string. The arguments are the flags of the recipe and the argument
 * string, separated by spaces. Stores the escaped argument string in
 * call->escaped. Returns the end of the text in @p d.
 */
static char *
godbolt_cat_arguments (struct godbolt              *call,
                       char                        *d,
                       struct godbolt_recipe const *r,
                       struct cstr_view             a)
{
	char const *const flags = d;
	if (r->windows)
		d = godbolt_cat(d, cstr_view_equal(call->args.language, CSTR_VIEW("c")) ? CSTR_VIEW("/TC")
		                                                                        : CSTR_VIEW("/TP"));
	d = godbolt_cat(d, d > flags && r->flags.len ? CSTR_VIEW(" ") : (struct cstr_view){});
	d = godbolt_cat(d, r->flags);
	d = godbolt_cat(d, d > flags && a.len ? CSTR_VIEW(" ") : (struct cstr_view){});
	char *const end = godbolt_escape(d, a);
	call->escaped = (struct cstr_view){ d, (size_t)(end - d) };
	loop_work(call->loop, LOOP_ESCAPE, a.len);
	return end;
}

/* Writes the language, the arguments, the filters and the compiler options
 * of the compilation call->job, whose recipe is @p r, to the body of its
 * request after the source.
 */
static void
godbolt_job_body (struct godbolt              *call,
                  struct godbolt_recipe const *r)
{
	char *d = godbolt_cat(call->body + call->head, CSTR_VIEW("\",\"lang\":\""));
	d = godbolt_cat(d, call->args.language);
	d = godbolt_cat(d, CSTR_VIEW("\",\"options\":{\"userArguments\":\""));
	d = godbolt_cat_arguments(call, d, r, call->args.strings[call->job % call->args.n_strings]);
	d = godbolt_cat(d, CSTR_VIEW("\",\"filters\":"));
	d = godbolt_cat_filters(d, call->args.filters);
	d = godbolt_cat(d, CSTR_VIEW(",\"compilerOptions\":"));
	d = godbolt_cat(d, r->options);
	d = godbolt_cat(d, CSTR_VIEW("}}"));
	call->len = (size_t)(d - call->body);
}

/* Writes the link to the state of the call, which has the state in base64,
 * on the first line of the output. If the link would be longer than
 * GODBOLT_LINK_MAX bytes, the line has the reason instead. Returns 0, or
 * call->e.
 */
static int
godbolt_state_link (struct godbolt *call)
{
	static char const digits[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	unsigned char const *s = (unsigned char const *)call->body;
	size_t const n = call->len;
	char link[GODBOLT_LINK_MAX];
	char *d = godbolt_cat(link, (struct cstr_view){ call->cfg->url, call->cfg->len });
	d = godbolt_cat(d, CSTR_VIEW("/clientstate/"));
	if ((size_t)(d - link) + (n + 2) / 3 * 4 > sizeof link)
		return godbolt_put(call, CSTR_VIEW("Link: none. The link would be longer than "
		                                   "8192 bytes.\n"));
	for (size_t i = 0; i < n; i += 3) {
		uint32_t const w = (uint32_t)s[i] << 16 |
		                   (i + 1 < n ? (uint32_t)s[i + 1] << 8 : 0) |
		                   (i + 2 < n ? s[i + 2] : 0);
		*d++ = digits[w >> 18];
		*d++ = digits[w >> 12 & 63];
		*d++ = i + 1 < n ? digits[w >> 6 & 63] : '=';
		*d++ = i + 2 < n ? digits[w & 63] : '=';
	}
	loop_work(call->loop, LOOP_COPY, n);
	godbolt_put(call, CSTR_VIEW("Link: "));
	godbolt_put(call, (struct cstr_view){ link, (size_t)(d - link) });
	return godbolt_put(call, CSTR_VIEW("\n"));
}

static_assert(GODBOLT_LINK_MAX == 8192, "the text of the line has 8192 bytes");

/* The phase GODBOLT_SHARE. It begins the state of the call after the
 * source in the body, as a ClientState of Compiler Explorer. The state has
 * one session with the language and a compiler for each compilation that
 * the call sends. A call that sends no compilation gets no link.
 */
static enum godbolt_go
godbolt_state_begin (struct godbolt *call)
{
	uint32_t sent = 0;
	for (uint32_t j = 0; j < call->jobs; ++j)
		sent += godbolt_job_recipe(call, j)->from != GODBOLT_FROM_NONE;
	call->phase = GODBOLT_JOB;
	if (!sent)
		return godbolt_put(call, CSTR_VIEW("Link: none. The call sends no compilation.\n"))
		       ? godbolt_fail(call) : GODBOLT_GO;
	char *d = godbolt_cat(call->body + call->head, CSTR_VIEW("\",\"id\":1,\"language\":\""));
	d = godbolt_cat(d, call->args.language);
	d = godbolt_cat(d, CSTR_VIEW("\",\"compilers\":["));
	call->len = (size_t)(d - call->body);
	call->phase = GODBOLT_STATE;
	return GODBOLT_GO;
}

/* The phase GODBOLT_STATE. It writes a compiler to the state for each
 * compilation that the call sends, with the arguments and the filters of
 * the compilation, as many as the bounds of the step allow. Then call->len
 * bytes from the start of the body are the state. With the option share
 * state, the function writes the link to the state. With the option share
 * short, it sends the state to Compiler Explorer, which stores the state
 * and returns a short link.
 */
static enum godbolt_go
godbolt_state (struct godbolt *call)
{
	char *d = call->body + call->len;
	for (; call->job < call->jobs; ++call->job) {
		struct godbolt_recipe const *r = godbolt_job_recipe(call, call->job);
		struct cstr_view const a = call->args.strings[call->job % call->args.n_strings];
		if (r->from == GODBOLT_FROM_NONE)
			continue;
		if (loop_room(call->loop, LOOP_ESCAPE) < a.len)
			break;
		d = godbolt_cat(d, d[-1] == '[' ? CSTR_VIEW("{\"id\":\"") : CSTR_VIEW(",{\"id\":\""));
		d = godbolt_cat(d, call->args.ids[call->job / call->args.n_strings]);
		d = godbolt_cat(d, CSTR_VIEW("\",\"options\":\""));
		d = godbolt_cat_arguments(call, d, r, a);
		d = godbolt_cat(d, CSTR_VIEW("\",\"filters\":"));
		d = godbolt_cat_filters(d, call->args.filters);
		d = godbolt_cat(d, CSTR_VIEW(",\"libs\":[]}"));
	}
	call->len = (size_t)(d - call->body);
	if (call->job < call->jobs)
		return GODBOLT_GO;
	call->len = (size_t)(godbolt_cat(d, CSTR_VIEW("]}]}")) - call->body);
	call->job = 0;
	call->phase = GODBOLT_JOB;
	if (call->cfg->share == GODBOLT_SHARE_STATE)
		return godbolt_state_link(call) ? godbolt_fail(call) : GODBOLT_GO;
	godbolt_request(call, GODBOLT_POST_SHORTEN, (struct cstr_view){},
	                (struct cstr_view){ call->body, call->len });
	call->phase = GODBOLT_LINK;
	return godbolt_send(call);
}

/* The phase GODBOLT_LINK. It reads the response with the short link, and
 * writes the link on the first line of the output. The link must be an
 * http or https URL of at most GODBOLT_SHORT_MAX bytes of printable ASCII
 * without spaces. After a failed request, the line has no link but the
 * reason, and the compilations go on.
 */
static enum godbolt_go
godbolt_link (struct godbolt *call)
{
	enum godbolt_go const got = godbolt_result(call);
	if (got == GODBOLT_WAIT || got == GODBOLT_ENDED)
		return got;
	call->phase = GODBOLT_JOB;
	if (got == GODBOLT_BAD)
		return godbolt_putf(call, "Link: none. The request for a short link failed (%.*s).\n",
		                    (int)call->why_len, call->why) ? godbolt_fail(call) : GODBOLT_GO;
	json_tokener_reset(call->tok);
	struct json_object *doc = json_tokener_parse_ex(call->tok, call->resp.data,
	                                                (int)call->resp.len);
	loop_work(call->loop, LOOP_PARSE, call->resp.len);
	struct cstr_view const link = godbolt_member(doc, "url");
	if (link.len <= GODBOLT_SHORT_MAX && godbolt_scheme(link)) {
		godbolt_put(call, CSTR_VIEW("Link: "));
		godbolt_put(call, link);
		godbolt_put(call, CSTR_VIEW("\n"));
	} else {
		godbolt_put(call, CSTR_VIEW("Link: none. The response of Compiler Explorer has "
		                            "no valid link.\n"));
	}
	json_object_put(doc);
	return call->e ? godbolt_fail(call) : GODBOLT_GO;
}

/* Writes the first line of the record of the compilation call->job, with
 * the status in call->why. Returns 0, or call->e.
 */
static int
godbolt_header (struct godbolt *call)
{
	godbolt_put(call, CSTR_VIEW("=== "));
	godbolt_put(call, call->args.ids[call->job / call->args.n_strings]);
	godbolt_put(call, CSTR_VIEW(" \""));
	godbolt_put(call, call->escaped);
	godbolt_put(call, CSTR_VIEW("\": "));
	godbolt_put(call, (struct cstr_view){ call->why, call->why_len });
	call->rec.headed = 1;
	return godbolt_put(call, CSTR_VIEW(" ===\n"));
}

/* Makes the next step begin the next compilation. */
static void
godbolt_next (struct godbolt *call)
{
	++call->job;
	call->phase = GODBOLT_JOB;
	call->rec = (struct godbolt_record){};
	godbolt_probe_fini(&call->probe);
}

/* Writes the record of the compilation call->job, which has no response,
 * with the status in call->why, and counts it as @p count. Then the next
 * step begins the next compilation.
 */
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

/* Writes the last line of the output, and ends the call. The call fails
 * if no compilation has a result. Returns GODBOLT_ENDED.
 */
static enum godbolt_go
godbolt_end (struct godbolt *call)
{
	uint32_t const *c = call->counts;
	if (call->tool == GODBOLT_TARGETS)
		godbolt_put(call, CSTR_VIEW("The facts come from predefined macros. data_model "
		                            "describes only the sizes of int, long and "
		                            "pointers.\n"));
	godbolt_putf(call, "Of %" PRIu32 " compilation%s, %" PRIu32 " ended with exit code "
	             "0, %" PRIu32 " ended with another exit code or a timeout, %" PRIu32
	             " ended with a failed request, and %" PRIu32 " ended without a "
	             "request.\n", call->jobs, godbolt_s(call->jobs), c[GODBOLT_ZERO],
	             c[GODBOLT_OTHER], c[GODBOLT_FAILED], c[GODBOLT_UNSENT]);
	if (call->e)
		return godbolt_fail(call);
	mcp_slot_finish(call->slot, c[GODBOLT_ZERO] || c[GODBOLT_OTHER] ? 0 : TOOL_FAILED);
	return GODBOLT_ENDED;
}

/* The phase GODBOLT_JOB. It sends the request of the compilation
 * call->job, or it writes the record of a compilation that it does not
 * send. After the last compilation, it ends the call.
 */
static enum godbolt_go
godbolt_job (struct godbolt *call)
{
	if (call->job == call->jobs)
		return godbolt_end(call);
	struct godbolt_recipe const *r = godbolt_job_recipe(call, call->job);
	godbolt_job_body(call, r);
	if (call->full) {
		godbolt_why(call, "not sent, because the output is full");
		return godbolt_record(call, GODBOLT_UNSENT);
	}
	if (r->from == GODBOLT_FROM_NONE) {
		godbolt_why(call, "not sent, because MSVC cannot output its predefined macros");
		return godbolt_record(call, GODBOLT_UNSENT);
	}
	godbolt_request(call, GODBOLT_POST_COMPILE,
	                call->args.ids[call->job / call->args.n_strings],
	                (struct cstr_view){ call->body + GODBOLT_SKIP, call->len - GODBOLT_SKIP });
	call->phase = GODBOLT_ANSWER;
	return godbolt_send(call);
}

/* The phase GODBOLT_ANSWER. It reads the response to the compilation, and
 * it begins the parse of a response with a status from 200 to 299.
 */
static enum godbolt_go
godbolt_answer (struct godbolt *call)
{
	enum godbolt_go const got = godbolt_result(call);
	if (got == GODBOLT_BAD)
		return godbolt_record(call, GODBOLT_FAILED);
	if (got != GODBOLT_GO)
		return got;
	json_tokener_reset(call->tok);
	call->off = 0;
	call->phase = GODBOLT_PARSE;
	return GODBOLT_GO;
}

/* Adds the section @p sec, whose text is @p v, to the record. */
static void
godbolt_section_add (struct godbolt     *call,
                     struct json_object *v,
                     enum godbolt_sec    sec)
{
	call->rec.secs[call->rec.n_sec] = v;
	call->rec.sec_of[call->rec.n_sec++] = sec;
}

/* Stores in call->rec.secs the texts of the sections of the record, as the
 * recipe of the compilation selects them. For a recipe that preprocesses,
 * the function marks the record for a note if the response contains no
 * text of the preprocessor, and for a note on a cut if the preprocessor
 * left out lines.
 */
static void
godbolt_sections_find (struct godbolt *call)
{
	uint32_t const from = godbolt_job_recipe(call, call->job)->from;
	enum godbolt_sec const main = call->tool == GODBOLT_MACROS ? GODBOLT_SEC_MACROS
	                                                           : GODBOLT_SEC_PP;
	if (from == GODBOLT_FROM_ASM)
		godbolt_section_add(call, godbolt_get(call->doc, "asm"), GODBOLT_SEC_ASM);
	if (from == GODBOLT_FROM_PP) {
		struct json_object *pp = godbolt_get(call->doc, "ppOutput");
		struct json_object *text = godbolt_get(pp, "output");
		struct json_object *filtered = godbolt_get(pp, "numberOfLinesFiltered");
		godbolt_section_add(call, text, main);
		call->rec.no_pp = !json_object_is_type(text, json_type_string);
		call->rec.truncated |= json_object_get_int64(filtered) != 0;
	}
	godbolt_section_add(call, godbolt_get(call->doc, "stdout"),
	                    from == GODBOLT_FROM_STDOUT ? main : GODBOLT_SEC_STDOUT);
	godbolt_section_add(call, godbolt_get(call->doc, "stderr"), GODBOLT_SEC_STDERR);
}

/* Returns the member text of each element of the array @p v, each with a
 * newline after it, and stores the length in @p *len. The caller frees
 * the text. On failure it returns nullptr and stores ENOMEM in @p *e.
 */
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
 * main text of the record: a string, or the lines of stdout. If the output
 * has a valid block, the record has the facts and the messages of the
 * compiler. Otherwise the status of the record has the reason, and the
 * record has the output. The function parses the whole output in one
 * step. Returns 0, or ENOMEM.
 */
static int
godbolt_targets_parse (struct godbolt *call)
{
	struct json_object *v = call->rec.secs[0];
	struct cstr_view text = jb_view(v);
	char *joined = nullptr;
	if (json_object_is_type(v, json_type_array)) {
		int e;
		joined = godbolt_join(call, v, &text.len, &e);
		if (!joined)
			return e;
		text.data = joined;
	}
	int const e = godbolt_probe_init(&call->probe, text);
	free(joined);
	joined = nullptr;
	loop_work(call->loop, LOOP_PARSE, text.len);
	if (e == ENOMEM)
		return ENOMEM;
	if (e) {
		godbolt_why(call, "the probe output is not valid: %s", call->probe.why);
		return 0;
	}
	call->rec.probed = 1;
	call->rec.n_sec = 0;
	godbolt_section_add(call, godbolt_get(call->doc, "stderr"), GODBOLT_SEC_STDERR);
	return 0;
}

/* Reads the parsed response to a compilation, and stores its status in
 * call->why and the texts of its sections in call->rec.secs. The next step
 * writes the record.
 */
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
	call->rec.truncated = json_object_is_type(truncated, json_type_boolean) &&
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

/* The phase GODBOLT_PARSE. It parses the next slice of the response to
 * the compilation. A response that is not valid JSON gets a record with
 * the reason.
 */
static enum godbolt_go
godbolt_parse (struct godbolt *call)
{
	struct cstr_view const body = call->resp;
	struct cstr_view const part = cstr_view_slice(body, call->off, godbolt_parse_room(call));
	if (!part.len && call->off < body.len)
		return GODBOLT_GO;
	/* The part is empty here only if the body is empty. json-c reports
	 * json_tokener_continue for an empty part. */
	struct json_object *doc = json_tokener_parse_ex(call->tok, part.data, (int)part.len);
	enum json_tokener_error const err = json_tokener_get_error(call->tok);
	godbolt_parsed(call, part.len);
	call->off += part.len;
	if (err == json_tokener_success) {
		call->doc = doc;
		return godbolt_classify(call);
	}
	if (err != json_tokener_continue)
		godbolt_why(call, "the response is not valid: %s", json_tokener_error_desc(err));
	else if (call->off < body.len)
		return GODBOLT_GO;
	else
		godbolt_why(call, "the response is not valid: the JSON text ends early");
	return godbolt_record(call, GODBOLT_FAILED);
}

/* Ends the record at the limit of the output, with a note. Later
 * compilations are not sent. Returns 0, or call->e.
 */
static int
godbolt_full (struct godbolt *call)
{
	call->full = 1;
	return godbolt_put(call, CSTR_VIEW("\n[The output is cut here, because it has "
	                                   "reached 14 MiB.]\n"));
}

static_assert(GODBOLT_FULL == 14 << 20, "the text of the note has 14 MiB");

/* Writes as much of @p s, from call->rec.byte on, as the bounds of the
 * step and the room left in the output allow, and advances
 * call->rec.byte. If no room is left in the output, or room only for the
 * first bytes of a UTF-8 character, the function ends the record with the
 * note of godbolt_full(). Returns 0, or call->e.
 */
static int
godbolt_slice (struct godbolt   *call,
               struct cstr_view  s)
{
	while (call->rec.byte < s.len && !call->e) {
		if (call->written >= GODBOLT_FULL)
			return godbolt_full(call);
		size_t const left = GODBOLT_FULL - call->written;
		uint64_t const room = loop_room(call->loop, LOOP_COPY);
		struct cstr_view const part = cstr_view_slice(s, call->rec.byte,
		                                              room < left ? room : left);
		if (!part.len)
			return call->e;
		size_t taken = 0;
		call->e = mcp_slot_write(call->slot, part,
		                         call->rec.byte + part.len < s.len ? TOOL_MORE : 0, &taken);
		call->written += taken;
		call->rec.byte += taken;
		if (!taken)
			return part.len < left ? call->e : godbolt_full(call);
	}
	return call->e;
}

/* Ends the section call->rec.sec, with a note if Compiler Explorer cut
 * it, and makes call->rec.sec the next section. Returns 0, or call->e.
 */
static int
godbolt_section_end (struct godbolt *call)
{
	if (call->rec.marked)
		godbolt_put(call, CSTR_VIEW("[Compiler Explorer cut this output.]\n"));
	call->rec.cut |= call->rec.marked;
	call->rec.marked = 0;
	++call->rec.sec;
	call->rec.opened = 0;
	call->rec.item = 0;
	call->rec.byte = 0;
	return call->e;
}

/* Writes as much of the string @p text, which is not empty, as
 * godbolt_slice() allows. After the last byte, the function writes a
 * newline unless the string ends with one, and it ends the section.
 * Returns 0, or call->e.
 */
static int
godbolt_section_text (struct godbolt   *call,
                      struct cstr_view  text)
{
	struct cstr_view const mark = CSTR_VIEW("\n[Truncated]");
	if (godbolt_slice(call, text) || call->full || call->rec.byte < text.len)
		return call->e;
	call->rec.marked |= text.len >= mark.len &&
	                    cstr_view_equal(cstr_view_slice(text, text.len - mark.len, SIZE_MAX),
	                                    mark);
	if (text.data[text.len - 1] != '\n')
		godbolt_put(call, CSTR_VIEW("\n"));
	return godbolt_section_end(call);
}

/* Writes as many lines of the JSON array @p v of @p n lines as the bounds
 * of the step allow, each with a newline after it. Each line is the member
 * text of an element, and godbolt_slice() writes it, so that
 * call->rec.item is the line and call->rec.byte the byte of the line. The
 * lookups of a line count as one value of LOOP_VALUES work. After the last
 * line, the function ends the section. Returns 0, or call->e.
 */
static int
godbolt_section_lines (struct godbolt     *call,
                       struct json_object *v,
                       size_t              n)
{
	for (; call->rec.item < n; ++call->rec.item) {
		if (call->written >= GODBOLT_FULL)
			return godbolt_full(call);
		if (call->e || !loop_room(call->loop, LOOP_COPY) ||
		    !loop_room(call->loop, LOOP_VALUES))
			return call->e;
		loop_work(call->loop, LOOP_VALUES, 1);
		struct json_object *element = json_object_array_get_idx(v, call->rec.item);
		struct cstr_view const line = godbolt_member(element, "text");
		if (godbolt_slice(call, line) || call->full || call->rec.byte < line.len)
			return call->e;
		godbolt_put(call, CSTR_VIEW("\n"));
		call->rec.marked |= cstr_view_equal(line, CSTR_VIEW("[truncated; too many lines]")) ||
		                    (call->rec.item + 1 == n &&
		                     cstr_view_equal(line, CSTR_VIEW("[Truncated]")));
		call->rec.byte = 0;
	}
	return godbolt_section_end(call);
}

/* Writes as much of the section call->rec.sec of the record as the bounds
 * of the step allow. A section whose text is empty is left out. After the
 * end of the section, call->rec.sec is the next section. Returns 0, or
 * call->e.
 */
static int
godbolt_section (struct godbolt *call)
{
	struct json_object *v = call->rec.secs[call->rec.sec];
	struct cstr_view const text = jb_view(v);
	/* text.len is 0 for an array, and jb_len() is 0 for a string. */
	size_t const n = text.len + jb_len(v);
	if (!n)
		return godbolt_section_end(call);
	if (!call->rec.opened)
		godbolt_put(call, godbolt_sections[call->rec.sec_of[call->rec.sec]]);
	call->rec.opened = 1;
	return text.data ? godbolt_section_text(call, text) : godbolt_section_lines(call, v, n);
}

/* Writes @p s, a NUL-terminated text of the probe, to the call's output.
 * Returns 0, or call->e.
 */
static int
godbolt_puts (struct godbolt *call,
              char const     *s)
{
	return godbolt_put(call, cstr_view(s));
}

/* Writes the facts about the target that follow from the output of the
 * probe, one to a line, and the versions of the compiler. With the
 * argument headers, it also writes which target headers the compiler can
 * include. Returns 0, or call->e.
 */
static int
godbolt_facts (struct godbolt *call)
{
	static struct cstr_view const names[GODBOLT_PROBE_FACTS] = {
		CSTR_VIEW_INIT("architecture: "), CSTR_VIEW_INIT("os: "),
		CSTR_VIEW_INIT("os_family: "), CSTR_VIEW_INIT("data_model: "),
		CSTR_VIEW_INIT("pointer_bits: "), CSTR_VIEW_INIT("endianness: "),
		CSTR_VIEW_INIT("libc: ")
	};
	static struct cstr_view const versions[] = {
		CSTR_VIEW_INIT("__clang_major__"), CSTR_VIEW_INIT("__GNUC__"),
		CSTR_VIEW_INIT("__GNUC_MINOR__"), CSTR_VIEW_INIT("_MSC_VER"),
		CSTR_VIEW_INIT("__STDC_VERSION__"), CSTR_VIEW_INIT("__cplusplus")
	};
	struct godbolt_probe const *p = &call->probe;
	for (size_t i = 0; i < GODBOLT_PROBE_FACTS; ++i) {
		struct godbolt_probe_claim const *c = &p->facts[i];
		godbolt_put(call, names[i]);
		if (c->conflict)
			godbolt_put(call, CSTR_VIEW("conflicting"));
		else if (i == GODBOLT_PROBE_POINTER_BITS && p->pointer_bits)
			godbolt_putf(call, "%" PRIu64, p->pointer_bits);
		else if (c->value)
			godbolt_puts(call, c->value);
		else
			godbolt_put(call, CSTR_VIEW("unknown"));
		for (uint32_t k = 0; k < c->n; ++k) {
			godbolt_put(call, k ? CSTR_VIEW(", ") : CSTR_VIEW(" (from "));
			godbolt_puts(call, c->evidence[k]);
		}
		godbolt_put(call, c->n ? CSTR_VIEW(")\n") : CSTR_VIEW("\n"));
	}
	bool any = false;
	for (size_t i = 0; i < array_size(versions); ++i) {
		char const *value = godbolt_probe_macro(p, versions[i].data);
		if (!value)
			continue;
		godbolt_put(call, any ? CSTR_VIEW(" ") : CSTR_VIEW("versions: "));
		godbolt_put(call, versions[i]);
		godbolt_put(call, CSTR_VIEW("="));
		godbolt_put_field(call, cstr_view(value));
		any = true;
	}
	godbolt_put(call, any ? CSTR_VIEW("\n") : CSTR_VIEW("versions: unknown\n"));
	if (!call->args.headers)
		return call->e;
	for (uint32_t k = 0; k < p->n_headers; ++k) {
		godbolt_put(call, k ? CSTR_VIEW(", ") : CSTR_VIEW("headers: "));
		godbolt_put_field(call, cstr_view(p->headers[k].name));
		godbolt_put(call, CSTR_VIEW(" "));
		godbolt_put_field(call, cstr_view(p->headers[k].value));
	}
	return godbolt_put(call, p->n_headers ? CSTR_VIEW("\n") : CSTR_VIEW("headers: unknown\n"));
}

/* The phase GODBOLT_WRITE. It writes the record of the compilation, as
 * much of it as the bounds of the step allow.
 */
static enum godbolt_go
godbolt_write (struct godbolt *call)
{
	if (!call->rec.headed) {
		godbolt_header(call);
		if (call->rec.no_pp)
			godbolt_put(call, CSTR_VIEW("Compiler Explorer sent no output of the "
			                            "preprocessor for this compiler.\n"));
		if (call->rec.probed)
			godbolt_facts(call);
	}
	while (call->rec.sec < call->rec.n_sec && !call->full) {
		uint8_t const sec = call->rec.sec;
		if (godbolt_section(call))
			return godbolt_fail(call);
		if (call->rec.sec == sec && !call->full)
			return GODBOLT_GO;
	}
	if (call->rec.truncated && !call->rec.cut && !call->full)
		godbolt_put(call, CSTR_VIEW("[Compiler Explorer cut this output.]\n"));
	if (call->e)
		return godbolt_fail(call);
	call->phase = GODBOLT_FREE;
	return GODBOLT_GO;
}

/* Returns the number of the children of the JSON value @p v: the elements
 * of an array, the members of an object, or 0.
 */
static size_t
godbolt_json_size (struct json_object *v)
{
	return json_object_is_type(v, json_type_object) ? (size_t)json_object_object_length(v)
	                                                : jb_len(v);
}

/* Returns the child of @p v that godbolt_json_cut() removes: the last
 * element of an array, or the first member of an object. @p v has
 * children.
 */
static struct json_object *
godbolt_json_child (struct json_object *v)
{
	if (json_object_is_type(v, json_type_array))
		return json_object_array_get_idx(v, json_object_array_length(v) - 1);
	struct json_object_iterator it = json_object_iter_begin(v);
	return json_object_iter_peek_value(&it);
}

/* Removes the child that godbolt_json_child() returns from @p v, and frees
 * it. @p v has children.
 */
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

/* The phase GODBOLT_FREE. It frees the parsed response to the compilation
 * one value at a time, as many values as the bounds of the step allow.
 * The value that it frees is at the end of the path of children that
 * godbolt_json_child() returns, so it has no children of its own. Then the
 * next step begins the next compilation.
 */
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
	[GODBOLT_SHARE]  = godbolt_state_begin,
	[GODBOLT_STATE]  = godbolt_state,
	[GODBOLT_LINK]   = godbolt_link,
	[GODBOLT_JOB]    = godbolt_job,
	[GODBOLT_ANSWER] = godbolt_answer,
	[GODBOLT_PARSE]  = godbolt_parse,
	[GODBOLT_WRITE]  = godbolt_write,
	[GODBOLT_FREE]   = godbolt_free,
};

/* The step() of every kind of the plugin. It runs the call's phase, and it
 * queues the next step if the phase goes on.
 */
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
 * nullptr. The call's timer is not set.
 */
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

/* The drop() of every kind of the plugin. It stops the call's timer, and
 * frees the call.
 */
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
	 * the call must stay. The timer function then returns at once,
	 * because call->slot is nullptr. */
	if (loop_timer_stop(call->loop, &call->timer)) {
		call->slot = nullptr;
		return;
	}
	godbolt_destroy(&call);
	*own = nullptr;
}

/* Returns the string @p v if it is @p min to @p max bytes long, if it
 * contains no NUL byte, and if each of its bytes is in @p set, unless
 * @p set is nullptr. Otherwise it returns an empty view whose data is
 * nullptr.
 */
static struct cstr_view
godbolt_text (struct json_object *v,
              size_t              min,
              size_t              max,
              char const         *set)
{
	struct cstr_view const t = jb_view(v);
	if (!t.data || t.len < min || t.len > max ||
	    (set ? strspn(t.data, set) : strlen(t.data)) != t.len)
		return (struct cstr_view){};
	return t;
}

/* Reads the array of ids @p v into a->ids. Returns 0, or EINVAL. */
static int
godbolt_args_ids (struct godbolt_args *a,
                  struct json_object  *v)
{
	size_t const n = jb_len(v);
	if (!n || n > GODBOLT_IDS)
		return EINVAL;
	for (size_t i = 0; i < n; ++i) {
		struct cstr_view const id = godbolt_text(json_object_array_get_idx(v, i), 1,
		                                         GODBOLT_ID_MAX, godbolt_id_bytes);
		if (!id.data || cstr_view_equal(id, CSTR_VIEW(".")) || cstr_view_equal(id, CSTR_VIEW("..")))
			return EINVAL;
		for (size_t k = 0; k < i; ++k)
			if (cstr_view_equal(id, a->ids[k]))
				return EINVAL;
		a->ids[i] = id;
	}
	a->n_ids = (uint8_t)n;
	return 0;
}

/* Reads the array of argument strings @p v into a->strings. Returns 0, or
 * EINVAL.
 */
static int
godbolt_args_strings (struct godbolt_args *a,
                      struct json_object  *v)
{
	size_t const n = jb_len(v);
	if (!n || n > GODBOLT_ARGS)
		return EINVAL;
	for (size_t i = 0; i < n; ++i) {
		a->strings[i] = godbolt_text(json_object_array_get_idx(v, i), 0, GODBOLT_ARG_MAX,
		                             nullptr);
		if (!a->strings[i].data)
			return EINVAL;
	}
	a->n_strings = (uint8_t)n;
	return 0;
}

/* Reads the words of the argument match @p v into a->words. ASCII white
 * space separates the words. Returns 0, or EINVAL.
 */
static int
godbolt_args_words (struct godbolt_args *a,
                    struct json_object  *v)
{
	struct cstr_view const t = godbolt_text(v, 0, GODBOLT_MATCH_MAX, nullptr);
	if (!t.data)
		return EINVAL;
	static char const space[] = " \t\n\v\f\r";
	a->n_words = 0;
	for (size_t i = strspn(t.data, space); i < t.len; i += strspn(t.data + i, space)) {
		if (a->n_words == GODBOLT_WORDS)
			return EINVAL;
		size_t const k = strcspn(t.data + i, space);
		a->words[a->n_words++] = (struct cstr_view){ t.data + i, k };
		i += k;
	}
	return 0;
}

/* Reads the argument filters @p v into a->filters. Returns 0, or
 * EINVAL.
 */
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
		       (!godbolt_filters[i].bit || strcmp(it.key, godbolt_filters[i].name.data)))
			++i;
		if (i == GODBOLT_FILTERS || !json_object_is_type(it.val, json_type_boolean))
			return EINVAL;
		a->filters = json_object_get_boolean(it.val) ? a->filters | godbolt_filters[i].bit
		                                             : a->filters & ~godbolt_filters[i].bit;
	}
	return 0;
}

/* Reads the value @p v of the argument @p arg into @p a. Returns 0, or
 * EINVAL if the value breaks the rule of the argument.
 */
static int
godbolt_args_set (struct godbolt_args *a,
                  enum godbolt_arg     arg,
                  struct json_object  *v)
{
	switch (arg) {
	case GODBOLT_ARG_ACTION: {
		struct cstr_view const t = godbolt_text(v, 1, GODBOLT_ID_MAX, nullptr);
		for (a->action = 0; a->action < GODBOLT_ACTIONS; ++a->action)
			if (cstr_view_equal(t, godbolt_actions[a->action].name))
				return 0;
		return EINVAL;
	}
	case GODBOLT_ARG_LANGUAGE:
		a->language = godbolt_text(v, 1, GODBOLT_LANGUAGE_MAX, godbolt_language_bytes);
		return a->language.data ? 0 : EINVAL;
	case GODBOLT_ARG_IDS:
	case GODBOLT_ARG_COMPILERS:
		return godbolt_args_ids(a, v);
	case GODBOLT_ARG_MATCH:
		return godbolt_args_words(a, v);
	case GODBOLT_ARG_KIND:
	case GODBOLT_ARG_FAMILY:
	case GODBOLT_ARG_ARCHITECTURE: {
		struct cstr_view *const value = &a->values[arg - GODBOLT_ARG_KIND];
		*value = godbolt_text(v, 1, GODBOLT_VALUE_MAX, nullptr);
		return value->data ? 0 : EINVAL;
	}
	case GODBOLT_ARG_LIMIT:
		return jb_uint_checked(v, GODBOLT_LIMIT_MAX, &a->limit) || !a->limit ? EINVAL : 0;
	case GODBOLT_ARG_OFFSET:
		/* mcp_slot_offset() checks the offset. */
		a->paged = json_object_get_int64(v) > 0;
		return 0;
	case GODBOLT_ARG_ARGS:
		return godbolt_args_strings(a, v);
	case GODBOLT_ARG_SOURCE:
		a->source = godbolt_text(v, 0, GODBOLT_SOURCE_MAX, nullptr);
		return a->source.data ? 0 : EINVAL;
	case GODBOLT_ARG_FILTERS:
		return godbolt_args_filters(a, v);
	case GODBOLT_ARG_HEADERS:
		a->headers = json_object_get_boolean(v);
		return json_object_is_type(v, json_type_boolean) ? 0 : EINVAL;
	case GODBOLT_ARG_SHARE:
		a->share = json_object_get_boolean(v);
		return json_object_is_type(v, json_type_boolean) ? 0 : EINVAL;
	case GODBOLT_ARG_COUNT:
		break;
	}
	return EINVAL;
}

/* Stores the reason of a refusal in the @p len bytes at @p why, formatted
 * from @p fmt and its arguments, and its length in @p *n unless @p n is
 * nullptr. Returns EINVAL.
 */
[[gnu::format(printf, 4, 5)]]
static int
godbolt_invalid (char       *why,
                 size_t      len,
                 size_t     *n,
                 char const *fmt,
                 ...)
{
	va_list ap;
	va_start(ap, fmt);
	size_t const k = godbolt_vformat(why, len, fmt, ap).len;
	va_end(ap);
	if (n)
		*n = k;
	return EINVAL;
}

/* Checks the arguments @p args of a call of the tool @p tool, and reads
 * them into @p a. If the configuration @p cfg turns links off, the tool
 * has no argument share. Returns 0, or EINVAL with the reason of the
 * refusal in the @p len bytes at @p why and its length in @p *n.
 */
static int
godbolt_args_read (struct godbolt_args         *a,
                   struct json_object          *args,
                   enum godbolt_tool            tool,
                   struct godbolt_config const *cfg,
                   char                        *why,
                   size_t                       len,
                   size_t                      *n)
{
	struct godbolt_params const *t = &godbolt_params[tool];
	uint32_t const off = cfg->share == GODBOLT_SHARE_OFF ? 1u << GODBOLT_ARG_SHARE : 0;
	/* godbolt_catalog requires the argument action, and the other tools
	 * read the list of compilers. */
	*a = (struct godbolt_args){
		.strings = { CSTR_VIEW("") },
		.language = CSTR_VIEW("c"),
		.limit = GODBOLT_LIMIT,
		.n_strings = 1,
		.action = GODBOLT_COMPILERS,
		.filters = tool == GODBOLT_COMPILE ? GODBOLT_SITE_FILTERS : 0
	};
	if (args && !json_object_is_type(args, json_type_object))
		return godbolt_invalid(why, len, n, "The arguments must be a JSON object.");
	uint32_t seen = 0;
	struct json_object_iter it;
	if (args) json_object_object_foreachC(args, it) {
		size_t const i = cstr_view_index(godbolt_arg_names, GODBOLT_ARG_COUNT,
		                                 cstr_view(it.key));
		uint32_t const bit = i < GODBOLT_ARG_COUNT ? 1u << i : 0;
		if (t->args & ~off & bit) {
			seen |= bit;
			if (!godbolt_args_set(a, (enum godbolt_arg)i, it.val))
				continue;
			return godbolt_invalid(why, len, n, "%s", godbolt_arg_rules[i].data);
		}
		if (t->args & off & bit)
			return godbolt_invalid(why, len, n, "Links are turned off on this server, so "
			                       "the tool has no argument share.");
		return godbolt_invalid(why, len, n, "The tool has no argument %.*s.",
		                       (int)godbolt_cut(cstr_view(it.key), 48), it.key);
	}
	uint32_t const missing = t->needs & ~seen;
	if (missing)
		return godbolt_invalid(why, len, n, "The argument %s is required.",
		                       godbolt_arg_names[stdc_trailing_zeros(missing)].data);
	/* The bits of kind, family and architecture are adjacent. */
	if (a->action == GODBOLT_LANGUAGES && seen >> GODBOLT_ARG_KIND & 7)
		return godbolt_invalid(why, len, n, "kind, family and architecture are not valid "
		                       "with the action languages.");
	if ((uint32_t)a->n_ids * a->n_strings > GODBOLT_JOBS)
		return godbolt_invalid(why, len, n, "The number of compilers times the number of "
		                       "argument strings must be at most 32.");
	return 0;
}

/* Copies @p t to *@p p, advances *@p p past the copy, and returns the
 * copy.
 */
static struct cstr_view
godbolt_copy (char             **p,
              struct cstr_view   t)
{
	struct cstr_view const copy = { *p, t.len };
	*p = godbolt_cat(*p, t);
	return copy;
}

/* The line before the source of the probe that makes the compiler include
 * the target headers of the probe.
 */
#define GODBOLT_HEADERS_LINE "#define GODBOLT_PROBE_HEADERS 1\n"

/* Allocates the state of a call of the tool @p tool on the loop @p loop,
 * with copies of the texts of @p a, and counts the copies as LOOP_COPY
 * work. The words of match are copied in lower case. An empty source
 * becomes "\n", as in godbolt-c. The source of godbolt_targets is the
 * probe, after a line that makes the compiler include the target headers
 * if the argument headers is true. Returns the state. On failure it
 * returns nullptr and stores ENOMEM in @p *e.
 */
static struct godbolt *
godbolt_create (struct godbolt_args const *a,
                enum godbolt_tool          tool,
                struct loop               *loop,
                int                       *e)
{
	struct cstr_view const head = a->headers ? CSTR_VIEW(GODBOLT_HEADERS_LINE)
	                                         : (struct cstr_view){};
	struct cstr_view const source = tool == GODBOLT_TARGETS ? godbolt_probe_source()
	                              : a->source.len ? a->source : CSTR_VIEW("\n");
	size_t bytes = a->language.len + head.len + source.len;
	for (size_t i = 0; i < array_size(a->values); ++i)
		bytes += a->values[i].len;
	for (uint8_t i = 0; i < a->n_ids; ++i)
		bytes += a->ids[i].len;
	for (uint8_t i = 0; i < a->n_strings; ++i)
		bytes += a->strings[i].len;
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
		.args = *a,
		.tok = tok,
		.loop = loop,
		.jobs = (uint8_t)(a->n_ids * a->n_strings),
		.tool = tool
	};
	char *p = call->blob;
	for (uint8_t i = 0; i < a->n_ids; ++i)
		call->args.ids[i] = godbolt_copy(&p, a->ids[i]);
	for (uint8_t i = 0; i < a->n_strings; ++i)
		call->args.strings[i] = godbolt_copy(&p, a->strings[i]);
	for (uint8_t i = 0; i < a->n_words; ++i) {
		call->args.words[i] = (struct cstr_view){ p, a->words[i].len };
		for (size_t k = 0; k < a->words[i].len; ++k)
			*p++ = godbolt_lower(a->words[i].data[k]);
	}
	call->args.language = godbolt_copy(&p, a->language);
	call->args.source = (struct cstr_view){ p, head.len + source.len };
	p = godbolt_cat(godbolt_cat(p, head), source);
	for (size_t i = 0; i < array_size(a->values); ++i)
		call->args.values[i] = godbolt_copy(&p, a->values[i]);
	loop_work(loop, LOOP_COPY, bytes);
	*e = 0;
	return call;
}

#undef GODBOLT_HEADERS_LINE

/* Starts a call of the tool @p tool. It checks the arguments and copies
 * them. godbolt_catalog sends the request of its list. The other tools
 * escape the source in their steps first, and godbolt_preprocess,
 * godbolt_macros and godbolt_targets then read the kinds of their
 * compilers from the list of compilers.
 */
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
	if (!cfg || !loop || !own || cfg->len > GODBOLT_URL_MAX ||
	    cfg->share >= GODBOLT_SHARES) {
		godbolt_refuse(slot, "The call has no valid configuration of the godbolt plugin, "
		               "no loop or no own word.");
		return;
	}
	struct godbolt_args a;
	char why[TOOL_TELL_MAX];
	size_t n;
	if (godbolt_args_read(&a, args, tool, cfg, why, sizeof why, &n)) {
		mcp_slot_tell(slot, (struct cstr_view){ why, n }, TOOL_FAILED);
		return;
	}
	if (mcp_slot_offset(slot, args))
		return;
	int e;
	struct godbolt *call = godbolt_create(&a, tool, loop, &e);
	if (!call) {
		mcp_slot_fail(slot, e);
		return;
	}
	*own = call;
	call->slot = slot;
	call->cfg = cfg;
	if (tool != GODBOLT_CATALOG) {
		call->phase = GODBOLT_ESCAPE;
		if (!mcp_slot_ready(slot))
			mcp_slot_park(slot);
		return;
	}
	if (godbolt_list_send(call) == GODBOLT_WAIT)
		mcp_slot_park(slot);
}

/* The start() of each kind. */
#define GODBOLT_START(name, NAME, extra)                        \
        static void                                             \
        godbolt_##name##_start (struct mcp_slot    *slot,       \
                                struct json_object *args)       \
        {                                                       \
                godbolt_start(slot, args, GODBOLT_##NAME);      \
        }

GODBOLT_TOOLS_X(GODBOLT_START)

#undef GODBOLT_START

/* Stores the URL @p v in the configuration @p c without its trailing '/'.
 * Returns 0, or EINVAL if @p v is not an http or https URL of at most
 * GODBOLT_URL_MAX bytes, or if it has a user name, a password, a query, a
 * fragment or a byte that is not printable ASCII or is a space.
 */
static int
godbolt_config_url (struct godbolt_config *c,
                    struct json_object    *v)
{
	struct cstr_view const t = godbolt_text(v, 1, GODBOLT_URL_MAX, nullptr);
	size_t const scheme = godbolt_scheme(t);
	if (!scheme)
		return EINVAL;
	size_t n = t.len;
	while (n > scheme && t.data[n - 1] == '/')
		--n;
	size_t const host = strcspn(t.data + scheme, "/");
	if (n == scheme || !host || memchr(t.data + scheme, '@', host) || strpbrk(t.data, "?#"))
		return EINVAL;
	memcpy(c->url, t.data, n);
	c->url[n] = '\0';
	c->len = (uint32_t)n;
	return 0;
}

/* Stores the option share @p v in the configuration @p c. Returns 0, or
 * EINVAL if @p v is not short, state or off.
 */
static int
godbolt_config_share (struct godbolt_config *c,
                      struct json_object    *v)
{
	size_t const share = cstr_view_index(godbolt_shares, GODBOLT_SHARES, jb_view(v));
	if (share == GODBOLT_SHARES)
		return EINVAL;
	c->share = (uint32_t)share;
	return 0;
}

/* The plugin's configure(). It stores the defaults, and then the options
 * of @p options: "url" and "share".
 */
static int
godbolt_configure (void               *config,
                   struct json_object *options,
                   char               *why,
                   size_t              len)
{
	if (!why || !len)
		return EINVAL;
	if (!config)
		return godbolt_invalid(why, len, nullptr, "askai passed no storage for the "
		                       "configuration");
	struct godbolt_config *c = config;
	*c = godbolt_defaults;
	if (!options)
		return 0;
	if (!json_object_is_type(options, json_type_object))
		return godbolt_invalid(why, len, nullptr, "not an object");
	struct json_object_iter it;
	json_object_object_foreachC(options, it) {
		if (!strcmp(it.key, "url")) {
			if (godbolt_config_url(c, it.val))
				return godbolt_invalid(why, len, nullptr, "url must be an http or "
				                       "https URL of at most 255 bytes, "
				                       "without a user name, a password, a "
				                       "query or a fragment");
		} else if (!strcmp(it.key, "share")) {
			if (godbolt_config_share(c, it.val))
				return godbolt_invalid(why, len, nullptr, "share must be short, state "
				                       "or off");
		} else {
			return godbolt_invalid(why, len, nullptr, "the member %.*s is not an "
			                       "option", (int)godbolt_cut(cstr_view(it.key), 64), it.key);
		}
	}
	return 0;
}

/* Adds @p val as the member @p key of @p obj, which is a part of the
 * definition @p def, and returns @p def. If @p obj or @p val is nullptr,
 * or if json-c cannot add the member, the function frees @p val and @p def
 * and returns nullptr.
 */
static struct json_object *
godbolt_definition_add (struct json_object *def,
                        struct json_object *obj,
                        char const         *key,
                        struct json_object *val)
{
	if (obj && val && !json_object_object_add(obj, key, val))
		return def;
	json_object_put(val);
	json_object_put(def);
	return nullptr;
}

/* Returns the definition of the tool @p tool in the JSON text @p json as a
 * new reference. Its description is @p head, the base URL of the
 * configuration @p service, and @p tail. If the tool accepts the argument
 * share and the configuration does not turn links off, the function adds
 * the property share and a sentence on links to the definition. With
 * short links, readOnlyHint is false, because Compiler Explorer stores the
 * source of a link. Returns nullptr if json-c runs out of memory, or if
 * the configuration is not valid.
 */
static struct json_object *
godbolt_definition (void const        *service,
                    enum godbolt_tool  tool,
                    char const        *json,
                    struct cstr_view   head,
                    struct cstr_view   tail)
{
	struct godbolt_config const *cfg = service ? service : &godbolt_defaults;
	if (cfg->len > GODBOLT_URL_MAX || cfg->share >= GODBOLT_SHARES)
		return nullptr;
	struct cstr_view const url = { cfg->url, cfg->len };
	struct cstr_view const link = godbolt_params[tool].args >> GODBOLT_ARG_SHARE & 1
	                              ? godbolt_share_notes[cfg->share] : (struct cstr_view){};
	char text[2048];
	if (head.len + url.len + tail.len + link.len > sizeof text)
		return nullptr;
	char *end = godbolt_cat(godbolt_cat(text, head), url);
	end = godbolt_cat(godbolt_cat(end, tail), link);
	struct json_object *def = json_tokener_parse(json);
	def = godbolt_definition_add(def, def, "description",
	                             json_object_new_string_len(text, (int)(end - text)));
	if (!link.len)
		return def;
	struct json_object *props = godbolt_get(godbolt_get(def, "inputSchema"), "properties");
	def = godbolt_definition_add(def, props, "share", json_tokener_parse(
		"{\"type\":\"boolean\",\"default\":false,\"description\":\"true to begin "
		"the output with a link to the same source, compilers and arguments on "
		"Compiler Explorer.\"}"));
	return godbolt_definition_add(def, godbolt_get(def, "annotations"), "readOnlyHint",
	                              json_object_new_boolean(cfg->share != GODBOLT_SHARE_SHORT));
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
	return godbolt_definition(service, GODBOLT_CATALOG,
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
		CSTR_VIEW("Lists the languages of Compiler Explorer at "),
		CSTR_VIEW(", the compilers of one language, or the numbers of compilers of "
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
	return godbolt_definition(service, GODBOLT_COMPILE,
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
		CSTR_VIEW("Compiles source code on Compiler Explorer at "),
		CSTR_VIEW(", once with each compiler and each argument string. The output "
		          "has a record for each compilation, with the exit code, the "
		          "assembly and the messages of the compiler, and the last line has "
		          "the counts of the results. askai sends the source to that service. "
		          "The compiled program does not run. The compiler ids must be exact "
		          "ids from godbolt_catalog."));
}

static struct json_object *
godbolt_preprocess_definition (void const *service)
{
	return godbolt_definition(service, GODBOLT_PREPROCESS,
		"{\"name\":\"godbolt_preprocess\",\"title\":\"Preprocess on Compiler "
		"Explorer\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		GODBOLT_DEF_COMPILERS "," GODBOLT_DEF_ARGS ","
		GODBOLT_DEF_LANGUAGE("the source", "c or c++") ","
		GODBOLT_DEF_SOURCE "," GODBOLT_DEF_OFFSET "},"
		"\"required\":[\"compilers\",\"source\"],\"additionalProperties\":false},"
		GODBOLT_DEF_HINTS "}",
		CSTR_VIEW("Preprocesses source code on Compiler Explorer at "),
		CSTR_VIEW(", once with each compiler and each argument string. The output "
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
	return godbolt_definition(service, GODBOLT_MACROS,
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
		CSTR_VIEW("Outputs the predefined macros of each compiler with each argument "
		          "string, from Compiler Explorer at "),
		CSTR_VIEW(". The output has a record for each compilation, with the exit code, "
		          "the macros and the messages of the compiler, and the last line has "
		          "the counts of the results. The record of an MSVC compiler has no "
		          "macros, because MSVC cannot output them. askai sends the source to "
		          "that service. The compiler ids must be exact ids from "
		          "godbolt_catalog."));
}

static struct json_object *
godbolt_targets_definition (void const *service)
{
	return godbolt_definition(service, GODBOLT_TARGETS,
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
		CSTR_VIEW("Preprocesses a fixed probe on Compiler Explorer at "),
		CSTR_VIEW(", once with each compiler and each argument string. The output "
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

#define GODBOLT_KIND(name, NAME, extra)                                 \
        {                                                               \
                .definition = godbolt_##name##_definition,              \
                .start = godbolt_##name##_start,                        \
                .step = godbolt_step,                                   \
                .drop = godbolt_drop,                                   \
                .flags = TOOL_LENT | TOOL_PAGED | TOOL_STREAM | (extra) \
        },

struct plugin const askai_plugin = {
	.tools = { GODBOLT_TOOLS_X(GODBOLT_KIND) },
	.configure = godbolt_configure,
	.config_size = sizeof (struct godbolt_config)
};

#undef GODBOLT_KIND
#undef GODBOLT_TOOLS_X
