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
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "compat.h"
#include "godbolt_probe.h"
#include "jb.h"
#include "plugin.h"
#include "util.h"
#include "xlist.h"

/* The tools: X(ID, name, flags), the constant of enum godbolt_tool, the
 * name after godbolt_, and the flags of the kind besides TOOL_LENT,
 * TOOL_PAGED and TOOL_STREAM.
 */
#define GODBOLT_TOOL_LIST(X)                   \
        X(CATALOG,    catalog,    0)           \
        X(COMPILE,    compile,    TOOL_DETACH) \
        X(PREPROCESS, preprocess, TOOL_DETACH) \
        X(MACROS,     macros,     TOOL_DETACH) \
        X(TARGETS,    targets,    TOOL_DETACH)

#define GODBOLT_TOOL_NAME(id, name, flags) "godbolt_" #name,
PLUGIN_MANIFEST(.name = "godbolt", .tools = { GODBOLT_TOOL_LIST(GODBOLT_TOOL_NAME) });
#undef GODBOLT_TOOL_NAME

/* The beginning of the state of a call, which the body of a compilation
 * leaves out.
 */
#define GODBOLT_SESSIONS "{\"sessions\":["

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
	GODBOLT_CSI_MAX      = 32,        /* bytes of the longest escape
	                                     sequence that godbolt_slice()
	                                     leaves out */
	GODBOLT_SKIP         = sizeof GODBOLT_SESSIONS - 1,  /* bytes of the body
	                                                      before the body of
	                                                      a compilation */
};

/* The fields of the list of compilers that the plugin requests. Each call
 * requests the same fields, so askai stores one list for each language.
 */
#define GODBOLT_FIELDS \
        "id,name,compilerType,compilerCategories,semver,instructionSet,releaseTrack"

/* The bytes of a compiler id, and of a language id. */
static char const godbolt_id_bytes[] = UTIL_ALNUM "+-._";
static char const godbolt_language_bytes[] =
	"+-0123456789_abcdefghijklmnopqrstuvwxyz";

/* The plugin's configuration. configure() stores it, and mcp_slot_config()
 * returns it. It is plain data without pointers.
 */
struct godbolt_config {
	UTIL_TEXT(GODBOLT_URL_MAX + 1) url;    /* the base URL without a trailing
	                                          '/' */
	uint32_t                       share;  /* enum godbolt_share */
};

/* The kinds of link for the argument share of a call, which the option
 * share selects: X(ID, name, note), the constant of enum godbolt_share,
 * the value of the option, and the sentences on links at the end of the
 * description of a tool with the argument share. SHORT is a link /z/ID
 * that Compiler Explorer makes and stores permanently. STATE is a link
 * /clientstate/BASE64 that contains the state, so that Compiler Explorer
 * stores nothing. With OFF, there is no link and the tools do not accept
 * the argument share.
 */
#define GODBOLT_SHARE_LIST(X)                                                        \
        X(SHORT, "short",                                                            \
          " With share true, the first line of the output is a link to the same "    \
          "source, compilers and arguments on Compiler Explorer. Compiler Explorer " \
          "stores the source permanently for the link. Anyone with the link can "    \
          "read the source. Set share only if the user asks for a link.")            \
        X(STATE, "state",                                                            \
          " With share true, the first line of the output is a link to the same "    \
          "source, compilers and arguments on Compiler Explorer. The link contains " \
          "the source. Compiler Explorer stores nothing. Set share only if the "     \
          "user asks for a link.")                                                   \
        X(OFF,   "off", "")

#define GODBOLT_SHARE_ENUM(id, ...) GODBOLT_SHARE_##id,
enum godbolt_share : uint32_t { GODBOLT_SHARE_LIST(GODBOLT_SHARE_ENUM) };
#undef GODBOLT_SHARE_ENUM

static struct cstr_view const godbolt_shares[] = { GODBOLT_SHARE_LIST(XL_VIEW) };

#define GODBOLT_SHARE_NOTE(id, name, note) CSTR_VIEW_INIT(note),
static struct cstr_view const godbolt_share_notes[] = { GODBOLT_SHARE_LIST(GODBOLT_SHARE_NOTE) };
#undef GODBOLT_SHARE_NOTE

/* The configuration without options. definition() uses it if askai passes
 * no configuration.
 */
#define GODBOLT_URL "https://godbolt.org"
static struct godbolt_config const godbolt_defaults = {
	.url = { sizeof GODBOLT_URL - 1, GODBOLT_URL },
	.share = GODBOLT_SHARE_SHORT
};
#undef GODBOLT_URL

/* The tools, in the order of the manifest. */
enum godbolt_tool : uint8_t {
#define GODBOLT_TOOL_ENUM(id, ...) GODBOLT_##id,
	GODBOLT_TOOL_LIST(GODBOLT_TOOL_ENUM)
#undef GODBOLT_TOOL_ENUM
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

/* The sections of a record, in the order of the output. */
enum godbolt_sec : uint8_t {
	GODBOLT_SEC_ASM,
	GODBOLT_SEC_PP,
	GODBOLT_SEC_MACROS,
	GODBOLT_SEC_STDOUT,
	GODBOLT_SEC_STDERR,
	GODBOLT_SEC_BUILD,       /* the messages of the build of a program
	                            that did not run */
	GODBOLT_SEC_RUN_STDOUT,  /* the standard output of the program */
	GODBOLT_SEC_RUN_STDERR,  /* the standard error of the program */
	GODBOLT_SECS
};

/* The kinds of request that the plugin sends. */
enum godbolt_req : uint8_t {
	GODBOLT_GET_LANGUAGES,  /* GET /api/languages?fields=... */
	GODBOLT_GET_COMPILERS,  /* GET /api/compilers/LANGUAGE?fields=... */
	GODBOLT_POST_COMPILE,   /* POST /api/compiler/ID/compile */
	GODBOLT_POST_SHORTEN,   /* POST /api/shortener */
};

/* The failures after which the plugin sends a request again. The member
 * again of each rule in godbolt_rules is a mask of some of the first three
 * bits. The plugin sends every request again after the statuses 429, 502
 * and 503. GODBOLT_AGAIN_GET is the mask of the two GET requests.
 */
enum godbolt_again : uint32_t {
	GODBOLT_AGAIN_504      = 1u << 0,
	GODBOLT_AGAIN_EIO      = 1u << 1,
	GODBOLT_AGAIN_TIMEDOUT = 1u << 2,
	GODBOLT_AGAIN_BUSY     = 1u << 3,  /* 429, 502 or 503 */
	GODBOLT_AGAIN_GET      = GODBOLT_AGAIN_504 | GODBOLT_AGAIN_EIO | GODBOLT_AGAIN_TIMEDOUT,
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
	[GODBOLT_GET_LANGUAGES] = { CSTR_VIEW_INIT("/api/languages"),
	                            CSTR_VIEW_INIT("?fields=id,name,defaultCompiler"),
	                            1 << 20, 30, 86400, GODBOLT_AGAIN_GET },
	[GODBOLT_GET_COMPILERS] = { CSTR_VIEW_INIT("/api/compilers/"),
	                            CSTR_VIEW_INIT("?fields=" GODBOLT_FIELDS),
	                            4 << 20, 30, 3600, GODBOLT_AGAIN_GET },
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

/* The actions of godbolt_catalog: X(ID, name, singular, header), the
 * constant of enum godbolt_action, the name, the singular of the noun, and
 * the first line of the output.
 */
#define GODBOLT_ACTION_LIST(X)                                                   \
        X(LANGUAGES,     "languages",     "language",     "id\tname\tdefault\n") \
        X(COMPILERS,     "compilers",     "compiler",                            \
          "id\tname\tkind\tversion\tarchitecture\ttrack\tfamilies\n")            \
        X(KINDS,         "kinds",         "kind",         "kind\tcompilers\n")   \
        X(FAMILIES,      "families",      "family",       "family\tcompilers\n") \
        X(ARCHITECTURES, "architectures", "architecture", "architecture\tcompilers\n")

enum godbolt_action : uint8_t {
#define GODBOLT_ACTION_ENUM(id, ...) GODBOLT_##id,
	GODBOLT_ACTION_LIST(GODBOLT_ACTION_ENUM)
#undef GODBOLT_ACTION_ENUM
};

static struct cstr_view const godbolt_action_names[] = { GODBOLT_ACTION_LIST(XL_VIEW) };

static struct {
	struct cstr_view header;
	char const      *singular;
} const godbolt_actions[] = {
#define GODBOLT_ACTION_ROW(id, name, singular, header) { CSTR_VIEW_INIT(header), singular },
	GODBOLT_ACTION_LIST(GODBOLT_ACTION_ROW)
#undef GODBOLT_ACTION_ROW
};

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

/* The filters of a compilation, in the order of the request body: X(ID,
 * name, set, site), the constant of enum godbolt_filter, the name, whether
 * the argument filters can set the filter, and whether the Compiler
 * Explorer site sets it by default. A filter that the argument cannot set
 * is false, except execute, which the argument run sets. Bit i of a set of
 * filters stands for godbolt_filters[i].
 */
#define GODBOLT_FILTER_LIST(X)                         \
        X(INTEL,         "intel",        true,  true)  \
        X(DEMANGLE,      "demangle",     true,  true)  \
        X(DIRECTIVES,    "directives",   true,  true)  \
        X(COMMENT_ONLY,  "commentOnly",  true,  true)  \
        X(LABELS,        "labels",       true,  true)  \
        X(LIBRARY_CODE,  "libraryCode",  true,  false) \
        X(TRIM,          "trim",         true,  false) \
        X(BINARY,        "binary",       false, false) \
        X(BINARY_OBJECT, "binaryObject", false, false) \
        X(DEBUG_CALLS,   "debugCalls",   false, false) \
        X(EXECUTE,       "execute",      false, false)

/* The names of the filters that the argument filters can set, each with
 * a comma after it for XL_JOIN().
 */
#define GODBOLT_FILTER_SET(id, name, set, site) GODBOLT_IF_##set(name,)
#define GODBOLT_IF_true(...) __VA_ARGS__
#define GODBOLT_IF_false(...)

/* The arguments of the tools: X(ID, name, rule), the constant of enum
 * godbolt_arg, the name, and the rest of the refusal of a value that breaks
 * the rule of the argument, after its name. Bit i of a mask of struct
 * godbolt_params stands for the argument godbolt_arg_names[i]. The rule of
 * OFFSET is empty, because mcp_slot_offset() checks the offset.
 */
#define GODBOLT_ARG_LIST(X)                                                             \
        X(ACTION,       "action",                                                       \
          "must be " XL_JOIN(", ", " or ", GODBOLT_ACTION_LIST(XL_TEXT)) ".")           \
        X(LANGUAGE,     "language", "must be 1 to 64 bytes of a-z, 0-9, +, - and _.")   \
        X(IDS,          "ids",                                                          \
          "must be an array of 1 to 16 different ids. Each id must be 1 to 128 bytes "  \
          "of A-Z, a-z, 0-9, +, -, . and _. An id of only . or .. is not valid.")       \
        X(MATCH,        "match", "must be a string of at most 256 bytes with at most "  \
          "8 words.")                                                                   \
        X(KIND,         "kind",         GODBOLT_VALUE_RULE)                             \
        X(FAMILY,       "family",       GODBOLT_VALUE_RULE)                             \
        X(ARCHITECTURE, "architecture", GODBOLT_VALUE_RULE)                             \
        X(LIMIT,        "limit",        "must be a whole number from 1 to 2000.")       \
        X(OFFSET,       "offset",       "")                                             \
        X(COMPILERS,    "compilers",                                                    \
          "must be an array of 1 to 16 different compiler ids. Each id must be 1 to "   \
          "128 bytes of A-Z, a-z, 0-9, +, -, . and _. An id of only . or .. is not "    \
          "valid.")                                                                     \
        X(ARGS,         "args",                                                         \
          "must be an array of 1 to 8 strings. Each string must be at most 8192 "       \
          "bytes without a NUL byte.")                                                  \
        X(SOURCE,       "source", "must be a string of at most 1 MiB without a NUL "    \
          "byte.")                                                                      \
        X(FILTERS,      "filters",                                                      \
          "must be an object with true or false members. The allowed members are "      \
          XL_JOIN(", ", " and ", GODBOLT_FILTER_LIST(GODBOLT_FILTER_SET)) ".")          \
        X(HEADERS,      "headers",      "must be true or false.")                       \
        X(SHARE,        "share",        "must be true or false.")                       \
        X(RUN,          "run",          "must be true or false.")
#define GODBOLT_VALUE_RULE "must be a string of 1 to 128 bytes without a NUL byte."

enum godbolt_arg : uint8_t {
#define GODBOLT_ARG_ENUM(id, ...) GODBOLT_ARG_##id,
	GODBOLT_ARG_LIST(GODBOLT_ARG_ENUM)
#undef GODBOLT_ARG_ENUM
	GODBOLT_ARG_COUNT
};

static_assert(GODBOLT_ARG_FAMILY == GODBOLT_ARG_KIND + 1 &&
              GODBOLT_ARG_ARCHITECTURE == GODBOLT_ARG_KIND + 2, "the order of values[]");

static struct cstr_view const godbolt_arg_names[] = { GODBOLT_ARG_LIST(XL_VIEW) };

/* The refusal of a value of each argument. */
static struct cstr_view const godbolt_arg_rules[] = {
#define GODBOLT_ARG_RULE(id, name, rule) CSTR_VIEW_INIT(name " " rule),
	GODBOLT_ARG_LIST(GODBOLT_ARG_RULE)
#undef GODBOLT_ARG_RULE
};

#undef GODBOLT_VALUE_RULE
#undef GODBOLT_ARG_LIST
#undef GODBOLT_FILTER_SET

#define GODBOLT_BIT(arg) (1u << GODBOLT_ARG_##arg)

/* The argument bits that are common to every tool except GODBOLT_CATALOG,
 * which sends no compilation.
 */
#define GODBOLT_JOB_ARGS \
        (GODBOLT_BIT(COMPILERS) | GODBOLT_BIT(ARGS) | GODBOLT_BIT(LANGUAGE) | GODBOLT_BIT(OFFSET))

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
		.args = GODBOLT_JOB_ARGS | GODBOLT_BIT(SOURCE) | GODBOLT_BIT(FILTERS) |
		        GODBOLT_BIT(SHARE) | GODBOLT_BIT(RUN),
		.needs = GODBOLT_BIT(SOURCE)
	},
	[GODBOLT_PREPROCESS] = {
		.args = GODBOLT_JOB_ARGS | GODBOLT_BIT(SOURCE) | GODBOLT_BIT(SHARE),
		.needs = GODBOLT_BIT(SOURCE)
	},
	[GODBOLT_MACROS] = {
		.args = GODBOLT_JOB_ARGS | GODBOLT_BIT(SOURCE) | GODBOLT_BIT(SHARE)
	},
	[GODBOLT_TARGETS] = {
		.args = GODBOLT_JOB_ARGS | GODBOLT_BIT(HEADERS)
	},
};

#undef GODBOLT_JOB_ARGS
#undef GODBOLT_BIT

enum godbolt_filter : uint8_t {
#define GODBOLT_FILTER_ENUM(id, ...) GODBOLT_FILTER_##id,
	GODBOLT_FILTER_LIST(GODBOLT_FILTER_ENUM)
#undef GODBOLT_FILTER_ENUM
	GODBOLT_FILTERS
};

static struct cstr_view const godbolt_filters[] = { GODBOLT_FILTER_LIST(XL_VIEW) };

/* The filters that the argument filters can set. */
#define GODBOLT_FILTER_BIT(id, name, set, site) | (uint32_t)set << GODBOLT_FILTER_##id
enum { GODBOLT_SETTABLE = 0 GODBOLT_FILTER_LIST(GODBOLT_FILTER_BIT) };
#undef GODBOLT_FILTER_BIT

/* The filters that the Compiler Explorer site sets by default. */
#define GODBOLT_FILTER_BIT(id, name, set, site) | (uint32_t)site << GODBOLT_FILTER_##id
enum { GODBOLT_SITE_FILTERS = 0 GODBOLT_FILTER_LIST(GODBOLT_FILTER_BIT) };
#undef GODBOLT_FILTER_BIT

/* The flags of the arguments headers, share, run and offset. run sets the
 * bit of the filter execute. */
enum godbolt_with : uint32_t {
	GODBOLT_WITH_RUN     = 1u << GODBOLT_FILTER_EXECUTE, /* the argument run */
	GODBOLT_WITH_HEADERS = 1u << GODBOLT_FILTERS,        /* the argument
	                                                        headers */
	GODBOLT_WITH_SHARE   = 1u << (GODBOLT_FILTERS + 1),  /* the argument
	                                                        share */
	GODBOLT_WITH_OFFSET  = 1u << (GODBOLT_FILTERS + 2),  /* the argument
	                                                        offset is above
	                                                        0 */
};

/* The first line of each section of a record. */
static struct cstr_view const godbolt_sections[GODBOLT_SECS] = {
	[GODBOLT_SEC_ASM]        = CSTR_VIEW_INIT("--- asm ---\n"),
	[GODBOLT_SEC_PP]         = CSTR_VIEW_INIT("--- pp ---\n"),
	[GODBOLT_SEC_MACROS]     = CSTR_VIEW_INIT("--- macros ---\n"),
	[GODBOLT_SEC_STDOUT]     = CSTR_VIEW_INIT("--- stdout ---\n"),
	[GODBOLT_SEC_STDERR]     = CSTR_VIEW_INIT("--- stderr ---\n"),
	[GODBOLT_SEC_BUILD]      = CSTR_VIEW_INIT("--- build stderr ---\n"),
	[GODBOLT_SEC_RUN_STDOUT] = CSTR_VIEW_INIT("--- program stdout ---\n"),
	[GODBOLT_SEC_RUN_STDERR] = CSTR_VIEW_INIT("--- program stderr ---\n"),
};

/* The note after a text that Compiler Explorer cut. */
static struct cstr_view const godbolt_cut_note =
	CSTR_VIEW_INIT("[Compiler Explorer cut the output of the compilation.]\n");

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
	uint32_t         flags;     /* the filters, bit i that of
	                               godbolt_filters[i], and enum
	                               godbolt_with */
};

/* The states of the record of a compilation. */
enum godbolt_mark : uint32_t {
	GODBOLT_REC_OPENED    = 1u << 0,  /* the first line of the section is
	                                     written */
	GODBOLT_REC_HEADED    = 1u << 1,  /* the first line of the record is
	                                     written */
	GODBOLT_REC_MARKED    = 1u << 2,  /* a line of the section marks a cut */
	GODBOLT_REC_CUT       = 1u << 3,  /* a note on a cut is written */
	GODBOLT_REC_TRUNCATED = 1u << 4,  /* the member truncated of the
	                                     response is true */
	GODBOLT_REC_NO_PP     = 1u << 5,  /* the response contains no text of
	                                     the preprocessor */
	GODBOLT_REC_PROBED    = 1u << 6,  /* probe contains the facts of the
	                                     record */
};

/* The record of the compilation that the call writes. godbolt_next()
 * resets it for the next compilation.
 */
struct godbolt_record {
	struct json_object *secs[GODBOLT_SECS];  /* the text of each section,
	                                            by enum godbolt_sec, or
	                                            nullptr */
	struct tool_cursor  at;     /* the section being written, its written
	                               lines, and the written bytes of its text
	                               or of the line */
	uint64_t            marks;  /* enum godbolt_mark */
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
	                                           blob */
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
	uint32_t                     counts[GODBOLT_COUNTS];
	int                          e;         /* the error number of the first
	                                           failure of the output, after
	                                           which the call writes
	                                           nothing */
	uint32_t                     missing;   /* bit i is set while args.ids[i]
	                                           is not in the list */
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
	uint8_t                      full;      /* 1 after godbolt_full() cut
	                                           the output */
	struct tool_why              why;       /* the reason of a failure, or
	                                           the status of a
	                                           compilation */
	/* The id that args.ids[0] points to in a call without compilers. */
	char                         default_compiler[GODBOLT_ID_MAX];
	char                         url[1024];
	char                         blob[];
};

static_assert(GODBOLT_URL_MAX + sizeof "/api/compiler/" + 3 * GODBOLT_ID_MAX +
              sizeof "/compile" <= sizeof ((struct godbolt *)nullptr)->url);
static_assert(GODBOLT_URL_MAX + sizeof "/api/compilers/" + 3 * GODBOLT_LANGUAGE_MAX +
              sizeof "?fields=" GODBOLT_FIELDS <= sizeof ((struct godbolt *)nullptr)->url);

#undef GODBOLT_FIELDS

/* Returns whether @p text contains @p word. The comparison ignores the case
 * of ASCII letters.
 */
static bool
godbolt_has (struct cstr_view text,
             struct cstr_view word)
{
	for (size_t i = 0; i + word.len <= text.len; ++i)
		if (cstr_view_starts_case(cstr_view_slice(text, i, SIZE_MAX), word))
			return true;
	return false;
}

/* Returns @p t, or @p otherwise if @p t is empty. */
static struct cstr_view
godbolt_or (struct cstr_view t,
            struct cstr_view otherwise)
{
	return t.len ? t : otherwise;
}

/* Returns the string @p v if it is a valid compiler id: 1 to
 * GODBOLT_ID_MAX bytes of godbolt_id_bytes, other than "." and "..".
 * Otherwise it returns an empty view whose data is nullptr.
 */
static struct cstr_view
godbolt_id (struct json_object *v)
{
	struct cstr_view const id = jb_text(v, 1, GODBOLT_ID_MAX, godbolt_id_bytes);
	return cstr_view_equal(id, CSTR_VIEW(".")) || cstr_view_equal(id, CSTR_VIEW(".."))
	       ? (struct cstr_view){} : id;
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

/* Appends the text that @p fmt and its arguments format to the call's
 * output, as util_format() formats it into 512 bytes. Returns 0, or
 * call->e.
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
	struct cstr_view const t = { text, util_vformat(text, sizeof text, fmt, ap) };
	va_end(ap);
	return godbolt_put(call, t);
}

/* Stores the text that @p fmt and its arguments format in call->why, as
 * util_format() formats it. Each control byte of the text becomes a space.
 */
[[gnu::format(printf, 2, 3)]]
static void
godbolt_why (struct godbolt *call,
             char const     *fmt,
             ...)
{
	va_list ap;
	va_start(ap, fmt);
	call->why.len = (uint32_t)util_vformat(call->why.data, sizeof call->why.data, fmt, ap);
	va_end(ap);
	for (uint32_t i = 0; i < call->why.len; ++i)
		if ((unsigned char)call->why.data[i] < 0x20 || call->why.data[i] == 0x7f)
			call->why.data[i] = ' ';
}

/* Ends the call in @p slot as failed with a text formatted from the other
 * arguments, as mcp_slot_tellf() does. Evaluates to GODBOLT_ENDED.
 */
#define godbolt_refuse(slot, ...) \
        (mcp_slot_tellf((slot), TOOL_FAILED, __VA_ARGS__), GODBOLT_ENDED)

/* Ends the call with the error number @p e, as mcp_slot_fail() does.
 * Evaluates to GODBOLT_ENDED.
 */
#define godbolt_abort(call, e) (mcp_slot_fail((call)->slot, (e)), GODBOLT_ENDED)

/* Ends the call. call->e is 0 or the error number of the first failed
 * write to the output. If call->e is ENOMEM or ESHUTDOWN, the function
 * passes that error number to mcp_slot_fail(). If call->e is another
 * error, such as EFBIG when a write would make the output longer than
 * TOOL_OUTPUT_MAX, the call ends as failed with the output written so far.
 * If call->e is 0, the call finishes with @p flags. Returns GODBOLT_ENDED.
 */
static enum godbolt_go
godbolt_finish (struct godbolt *call,
                uint32_t        flags)
{
	if (call->e == ENOMEM || call->e == ESHUTDOWN)
		return godbolt_abort(call, call->e);
	mcp_slot_finish(call->slot, call->e ? TOOL_FAILED : flags);
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
	char *u = godbolt_cat(call->url, util_text_view(&call->cfg->url));
	u = godbolt_cat_path(godbolt_cat(u, rule->path), segment);
	*godbolt_cat(u, rule->tail) = '\0';
	call->req = (struct web_request){
		.url = call->url,
		.body = body,
		.accept = "application/json",
		.max = rule->max,
		.timeout = rule->timeout,
		.keep = kind == GODBOLT_POST_COMPILE && !(call->args.flags & GODBOLT_WITH_OFFSET)
		      ? 1 : rule->keep
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
	                             loop_now(call->loop) + seconds * NS);
	if (e == ENOMEM)
		return godbolt_abort(call, ENOMEM);
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
	if (e == ENOMEM)
		return godbolt_abort(call, ENOMEM);
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

/* Parses the body of the last response, call->resp, in one piece, and
 * counts it as LOOP_PARSE work. Returns the parsed value, or nullptr.
 */
static struct json_object *
godbolt_resp_doc (struct godbolt *call)
{
	json_tokener_reset(call->tok);
	loop_work(call->loop, LOOP_PARSE, call->resp.len);
	return json_tokener_parse_ex(call->tok, call->resp.data, (int)call->resp.len);
}

/* Stores in call->why the reason of the response in call->resp, whose
 * status @p status is an error. The reason is the member message of a
 * JSON object, or else the first line of the body, and it is at most
 * GODBOLT_REASON_MAX bytes long.
 */
static void
godbolt_reason (struct godbolt *call,
                uint32_t        status)
{
	struct json_object *doc = call->resp.len <= LOOP_STEP_PARSE ? godbolt_resp_doc(call)
	                                                            : nullptr;
	struct cstr_view text = godbolt_or(jb_view(jb_get(doc, "message")), call->resp);
	(void)cstr_view_cut(&text, '\n', &text);
	size_t const n = util_clip(text.data, text.len, GODBOLT_REASON_MAX);
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
		godbolt_why(call, "askai got no response to the request");
		return GODBOLT_BAD;
	}
	if (r.e == ESHUTDOWN || r.e == ENOMEM)
		return godbolt_abort(call, r.e);
	uint32_t const cause = r.e == EIO ? GODBOLT_AGAIN_EIO
	                     : r.e == ETIMEDOUT ? GODBOLT_AGAIN_TIMEDOUT
	                     : r.e ? 0
	                     : r.status == 504 ? GODBOLT_AGAIN_504
	                     : r.status == 429 || r.status == 502 || r.status == 503
	                     ? GODBOLT_AGAIN_BUSY : 0;
	bool const retry = cause & (godbolt_rules[call->kind_of].again | GODBOLT_AGAIN_BUSY);
	if (retry) {
		if (r.retry > GODBOLT_RETRY_MAX) {
			godbolt_why(call, "HTTP %" PRIu32 " with Retry-After %" PRIu32 " s",
			            r.status, r.retry);
			return GODBOLT_BAD;
		}
		if (call->attempt < GODBOLT_ATTEMPTS) {
			uint32_t const wait = 1u << (call->attempt - 1);
			return godbolt_later(call, r.retry > wait ? r.retry : wait);
		}
	}
	call->resp = r.body.data ? r.body : CSTR_VIEW("");
	/* The largest body of each rule is a whole number of KiB or MiB. */
	bool const mib = call->req.max >= 1 << 20;
	if (r.e == EFBIG)
		godbolt_why(call, "the response is larger than %zu %s",
		            call->req.max >> (mib ? 20 : 10), mib ? "MiB" : "KiB");
	else if (r.e)
		godbolt_why(call, "the request failed (%.*s)",
		            (int)util_clip(r.error, strlen(r.error), GODBOLT_REASON_MAX), r.error);
	else if (r.status < 200 || r.status > 299)
		godbolt_reason(call, r.status);
	else
		return GODBOLT_GO;
	return GODBOLT_BAD;
}

/* Moves the parse of the JSON array in call->resp to the state after the
 * byte at call->off, which is outside an element, and returns the number
 * of bytes that it consumed. A byte that begins an element is not
 * consumed: call->list becomes GODBOLT_LIST_VALUE, and the parse of the
 * element begins with that byte at call->start. If the text is not a
 * JSON array, call->list is GODBOLT_LIST_BAD and call->why is the reason.
 */
static size_t
godbolt_array_next (struct godbolt *call)
{
	char const c = call->resp.data[call->off];
	uint8_t const s = call->list;
	call->start = call->off;
	call->list = cstr_view_space(c) ? s
	           : s == GODBOLT_LIST_OPEN ? (c == '[' ? GODBOLT_LIST_FIRST : GODBOLT_LIST_BAD)
	           : c == ']' && s != GODBOLT_LIST_COMMA ? GODBOLT_LIST_END
	           : s != GODBOLT_LIST_NEXT ? GODBOLT_LIST_VALUE
	           : c == ',' ? GODBOLT_LIST_COMMA : GODBOLT_LIST_BAD;
	if (call->list == GODBOLT_LIST_BAD)
		godbolt_why(call, "the text is not a JSON array");
	size_t const consumed = call->list != GODBOLT_LIST_VALUE;
	call->off += consumed;
	return consumed;
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
 * an element is longer than GODBOLT_ELEMENT_MAX bytes, call->list is
 * GODBOLT_LIST_BAD and call->why is the reason.
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
			used += godbolt_array_next(call);
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
			godbolt_why(call, "an element is larger than 4 KiB");
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
	return cstr_view_index(tags, n, family) < n ||
	       (!n && cstr_view_equal(family, CSTR_VIEW("unknown")));
}

/* Returns whether an element of a list matches the words of the argument
 * match and the argument ids, and counts a match in call->matching. Each
 * word must be in one of the @p n texts of @p f. @p f[0] is the id of the
 * element. With the argument ids, the function clears the bit of the id
 * in call->missing.
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
	size_t const i = cstr_view_index(call->args.ids, call->args.n_ids, f[0]);
	if (call->args.n_ids && i == call->args.n_ids)
		return false;
	call->missing &= ~(1u << i);
	++call->matching;
	return true;
}

/* Returns whether the limit allows another row of the catalog, and counts
 * the row in call->listed if it does.
 */
static bool
godbolt_row (struct godbolt *call)
{
	if (call->listed == call->args.limit)
		return false;
	++call->listed;
	return true;
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
 * filters select it and the limit allows another row. Returns 0, or
 * call->e.
 */
static int
godbolt_language_row (struct godbolt     *call,
                      struct json_object *element)
{
	struct cstr_view const id = jb_view(jb_get(element, "id"));
	if (!id.len) {
		++call->malformed;
		return 0;
	}
	++call->total;
	struct cstr_view const f[] = { id, jb_view(jb_get(element, "name")) };
	if (!godbolt_matches(call, f, array_size(f)) || !godbolt_row(call))
		return 0;
	godbolt_put_field(call, f[0]);
	godbolt_put(call, CSTR_VIEW("\t"));
	godbolt_put_field(call, f[1]);
	godbolt_put(call, CSTR_VIEW("\t"));
	godbolt_put_field(call, godbolt_or(jb_view(jb_get(element, "defaultCompiler")),
	                                   CSTR_VIEW("none")));
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
	struct json_object *n = jb_get(call->doc, key);
	if (n) {
		json_object_int_inc(n, 1);
		return call->e;
	}
	if (json_object_object_length(call->doc) == GODBOLT_KEYS) {
		godbolt_why(call, "the list contains more than 256 different %s",
		            godbolt_action_names[call->args.action].data);
		call->list = GODBOLT_LIST_BAD;
		return call->e;
	}
	if (jb_set(call->doc, key, json_object_new_int(1)))
		call->e = call->e ? call->e : ENOMEM;
	return call->e;
}

static_assert(GODBOLT_KEYS == 256, "the reason says 256");

/* Counts each of the @p n family tags @p tags once, or the family unknown
 * if there is no tag. Returns 0, or call->e.
 */
static int
godbolt_count_families (struct godbolt         *call,
                        struct cstr_view const *tags,
                        size_t                  n)
{
	for (size_t i = 0; i < n; ++i)
		if (cstr_view_index(tags, i, tags[i]) == i)
			godbolt_count(call, tags[i].data);
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
	struct json_object *tags = jb_get(element, "compilerCategories");
	size_t const n_tags = jb_len(tags);
	/* The columns, and then the family tags that are not empty. */
	struct cstr_view f[GODBOLT_COLUMNS + GODBOLT_TAGS] = {
		jb_view(jb_get(element, "id")),
		jb_view(jb_get(element, "name")),
		godbolt_or(jb_view(jb_get(element, "compilerType")), CSTR_VIEW("default")),
		jb_view(jb_get(element, "semver")),
		godbolt_or(jb_view(jb_get(element, "instructionSet")), CSTR_VIEW("unknown")),
		jb_view(jb_get(element, "releaseTrack")),
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
	switch (call->args.action) {
	case GODBOLT_KINDS:
		return godbolt_count(call, f[2].data);
	case GODBOLT_ARCHITECTURES:
		return godbolt_count(call, f[4].data);
	case GODBOLT_FAMILIES:
		return godbolt_count_families(call, t, n - GODBOLT_COLUMNS);
	}
	if (!godbolt_row(call))
		return 0;
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
 * call->e.
 */
static int
godbolt_tallies (struct godbolt *call,
                 size_t          n)
{
	size_t const rows = n < call->args.limit ? n : call->args.limit;
	call->listed = (uint32_t)rows;
	struct godbolt_tally t[GODBOLT_KEYS];
	size_t i = 0;
	struct json_object_iter it;
	json_object_object_foreachC(call->doc, it)
		t[i++] = (struct godbolt_tally){ it.key, json_object_get_int64(it.val) };
	qsort(t, n, sizeof *t, godbolt_tally_cmp);
	for (size_t k = 0; k < rows; ++k) {
		godbolt_put_field(call, cstr_view(t[k].key));
		godbolt_putf(call, "\t%" PRId64 "\n", t[k].count);
	}
	return call->e;
}

/* The ids of a call that are not in the list, separated by ", ". */
struct godbolt_absent {
	char ids[GODBOLT_IDS * (GODBOLT_ID_MAX + 2) + 1];
};

/* Returns the ids of @p call that are not in the list. The function
 * returns the whole object, so the initializer writes every byte of it.
 */
static struct godbolt_absent
godbolt_absent (struct godbolt const *call)
{
	struct godbolt_absent a = {};
	char *p = a.ids;
	for (uint32_t m = call->missing; m; m &= m - 1) {
		p = godbolt_cat(p, p == a.ids ? (struct cstr_view){} : CSTR_VIEW(", "));
		p = godbolt_cat(p, call->args.ids[stdc_trailing_zeros(m)]);
	}
	*p = '\0';
	return a;
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
	if (call->missing)
		return godbolt_refuse(call->slot, "Ids not in the list or excluded by another "
		                      "filter: %s. For the exact ids, retry with action \"%s\" "
		                      "and without ids or other filters.",
		                      godbolt_absent(call).ids, languages ? "languages" : "compilers");
	bool const counts = call->args.action > GODBOLT_COMPILERS;
	uint32_t const keys = counts ? (uint32_t)json_object_object_length(call->doc) : 0;
	if (keys)
		godbolt_tallies(call, keys);
	if (call->malformed)
		godbolt_putf(call, "Left out %" PRIu32 " invalid list element%s.\n",
		             call->malformed, util_s(call->malformed));
	/* The first sentence of the last line counts the listed items, and the
	 * second sentence counts all items of the list. */
	char const *const singular = godbolt_actions[call->args.action].singular;
	if (counts)
		godbolt_putf(call, "Listed %" PRIu32 " of the %" PRIu32 " %s of the %" PRIu32
		             " matching compiler%s. ", call->listed, keys,
		             keys == 1 ? singular : godbolt_action_names[call->args.action].data,
		             call->matching, util_s(call->matching));
	else
		godbolt_putf(call, "Listed %" PRIu32 " of %" PRIu32 " matching %s%s. ",
		             call->listed, call->matching, singular, util_s(call->matching));
	if (languages)
		godbolt_putf(call, "Compiler Explorer lists %" PRIu32 " language%s.\n",
		             call->total, util_s(call->total));
	else
		godbolt_putf(call, "Compiler Explorer lists %" PRIu32 " compiler%s for language "
		             "%.*s.\n", call->total, util_s(call->total),
		             (int)call->args.language.len, call->args.language.data);
	return godbolt_finish(call, 0);
}

/* Stores the kind of driver of a compiler of the call, if the element of
 * the list of compilers is one of them. When the list has had each
 * compiler of the call, the function sets call->list to GODBOLT_LIST_END,
 * which ends the parse. Returns 0.
 */
static int
godbolt_driver_row (struct godbolt     *call,
                    struct json_object *element)
{
	size_t const i = cstr_view_index(call->args.ids, call->args.n_ids,
	                                 jb_view(jb_get(element, "id")));
	if (i < call->args.n_ids && call->missing >> i & 1) {
		struct cstr_view const kind = jb_view(jb_get(element, "compilerType"));
		call->missing &= ~(1u << i);
		call->drivers[i] = cstr_view_equal(kind, CSTR_VIEW("win32-vc")) ? GODBOLT_MSVC
		                 : cstr_view_equal(kind, CSTR_VIEW("clang-cl")) ? GODBOLT_CLANG_CL
		                 : GODBOLT_GNU;
	}
	if (!call->missing)
		call->list = GODBOLT_LIST_END;
	return 0;
}

/* Ends the parse of the list of compilers of a tool that compiles. If a
 * compiler of the call is not in the list, the call fails before it sends
 * a compilation. Otherwise the next step begins the state of a call with
 * share, or the first compilation.
 */
static enum godbolt_go
godbolt_drivers_end (struct godbolt *call)
{
	if (call->missing) {
		struct cstr_view const lang = call->args.language;
		return godbolt_refuse(call->slot, "Unknown compiler ids for language %.*s: %s. "
		                      "Call godbolt_catalog with {\"action\":\"compilers\","
		                      "\"language\":\"%.*s\"} for the exact ids.", (int)lang.len,
		                      lang.data, godbolt_absent(call).ids, (int)lang.len, lang.data);
	}
	call->phase = call->args.flags & GODBOLT_WITH_SHARE ? GODBOLT_SHARE : GODBOLT_JOB;
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

/* If @p element is the call's language, sets args.ids[0] to a copy of its
 * default compiler if that is a valid id, and ends the parse. Returns 0.
 */
static int
godbolt_default_row (struct godbolt     *call,
                     struct json_object *element)
{
	if (!cstr_view_equal(jb_view(jb_get(element, "id")), call->args.language))
		return 0;
	struct cstr_view const id = godbolt_id(jb_get(element, "defaultCompiler"));
	static_assert(sizeof call->default_compiler >= GODBOLT_ID_MAX);
	call->args.ids[0] = (struct cstr_view){ call->default_compiler, id.len };
	godbolt_cat(call->default_compiler, id);
	call->list = GODBOLT_LIST_END;
	return 0;
}

/* Ends the parse of the list of languages. Without a valid default
 * compiler the call fails, and with one it reads the list of compilers.
 */
static enum godbolt_go
godbolt_default_end (struct godbolt *call)
{
	if (!call->args.ids[0].len)
		return godbolt_refuse(call->slot, "Compiler Explorer lists no valid default "
		                      "compiler for language %.*s. Call godbolt_catalog with "
		                      "{\"action\":\"languages\"} for the language ids and the "
		                      "default compilers.", (int)call->args.language.len,
		                      call->args.language.data);
	call->args.action = GODBOLT_COMPILERS;
	return godbolt_list_send(call);
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
		                      ? "languages" : "compilers", call->why.data);
	if (got != GODBOLT_GO)
		return got;
	json_tokener_reset(call->tok);
	call->off = 0;
	call->list = GODBOLT_LIST_OPEN;
	call->phase = GODBOLT_ROWS;
	if (call->tool != GODBOLT_CATALOG)
		return GODBOLT_GO;
	if (call->args.action > GODBOLT_COMPILERS && !(call->doc = json_object_new_object()))
		return godbolt_abort(call, ENOMEM);
	return godbolt_put(call, godbolt_actions[call->args.action].header)
	       ? godbolt_finish(call, TOOL_FAILED) : GODBOLT_GO;
}

/* Indexed by [the tool compiles][the list is the list of languages]. */
static struct {
	int             (*row) (struct godbolt     *call,
	                        struct json_object *element);
	enum godbolt_go (*end) (struct godbolt *call);
} const godbolt_lists[2][2] = {
	{ { godbolt_compiler_row, godbolt_catalog_end },
	  { godbolt_language_row, godbolt_catalog_end } },
	{ { godbolt_driver_row, godbolt_drivers_end },
	  { godbolt_default_row, godbolt_default_end } },
};

/* The phase GODBOLT_ROWS. It parses the next elements of the list, and
 * after its end calls the end function of godbolt_lists.
 */
static enum godbolt_go
godbolt_rows (struct godbolt *call)
{
	bool const compiles = call->tool != GODBOLT_CATALOG;
	bool const languages = call->args.action == GODBOLT_LANGUAGES;
	typeof(godbolt_lists[0][0]) const *f = &godbolt_lists[compiles][languages];
	if (godbolt_list_parse(call, f->row))
		return godbolt_finish(call, TOOL_FAILED);
	if (call->list == GODBOLT_LIST_BAD)
		return godbolt_refuse(call->slot, "Compiler Explorer sent a list that is not "
		                      "valid (%s).", call->why.data);
	if (call->list != GODBOLT_LIST_END)
		return GODBOLT_GO;
	return f->end(call);
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
	struct cstr_view const prefix = CSTR_VIEW(GODBOLT_SESSIONS "{\"source\":\"");
	if (!call->body) {
		size_t const cap = prefix.len + 6 * call->args.source.len + GODBOLT_TAIL_MAX +
		                   (call->args.flags & GODBOLT_WITH_SHARE ? godbolt_state_size(call)
		                                                          : 0);
		call->body = malloc(cap);
		if (!call->body)
			return godbolt_abort(call, ENOMEM);
		call->head = (size_t)(godbolt_cat(call->body, prefix) - call->body);
	}
	struct cstr_view const part = cstr_view_slice(call->args.source, call->off,
	                                              loop_room(call->loop, LOOP_ESCAPE));
	call->head += jb_escape(call->body + call->head, part.data, part.len);
	call->off += part.len;
	loop_work(call->loop, LOOP_ESCAPE, part.len);
	if (call->off < call->args.source.len)
		return GODBOLT_GO;
	if (call->head - GODBOLT_SKIP > TOOL_TEXT_MAX - GODBOLT_TAIL_MAX)
		return godbolt_refuse(call->slot, "The source is %zu bytes as a JSON string. A "
		                      "request body must be at most 4 MiB. Shorten the source.",
		                      call->head - prefix.len);
	return godbolt_list_send(call);
}

#undef godbolt_refuse
#undef GODBOLT_SESSIONS

static_assert(TOOL_TEXT_MAX == 4U << 20, "the reason says 4 MiB");

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
 * call->escaped. The function then ends the string and writes the member
 * filters as a JSON object with the 11 filters of godbolt_filters. A
 * filter is true if its bit is set in call->args.flags, and false
 * otherwise. Returns the end of the text in @p d.
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
	char *const end = d + jb_escape(d, a.data, a.len);
	call->escaped = (struct cstr_view){ d, (size_t)(end - d) };
	loop_work(call->loop, LOOP_ESCAPE, a.len);
	d = godbolt_cat(end, CSTR_VIEW("\",\"filters\":{"));
	for (size_t i = 0; i < array_size(godbolt_filters); ++i) {
		*d++ = '"';
		d = godbolt_cat(d, godbolt_filters[i]);
		d = godbolt_cat(d, call->args.flags >> i & 1 ? CSTR_VIEW("\":true,")
		                                             : CSTR_VIEW("\":false,"));
	}
	d[-1] = '}';
	return d;
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
	if (call->args.flags & GODBOLT_WITH_RUN)
		d = godbolt_cat(d, CSTR_VIEW(",\"executeParameters\":{\"args\":[],\"stdin\":\"\"}"));
	d = godbolt_cat(d, CSTR_VIEW(",\"compilerOptions\":"));
	d = godbolt_cat(d, r->options);
	d = godbolt_cat(d, CSTR_VIEW("}}"));
	call->len = (size_t)(d - call->body);
}

/* Writes the link to the state of the call, with the state in base64, on
 * the first line of the output. If the link would be longer than
 * GODBOLT_LINK_MAX bytes, the line contains the reason instead. Returns 0,
 * or call->e.
 */
static int
godbolt_state_link (struct godbolt *call)
{
	static char const digits[] = UTIL_ALNUM "+/";
	unsigned char const *s = (unsigned char const *)call->body;
	size_t const n = call->len;
	char link[GODBOLT_LINK_MAX];
	char *d = godbolt_cat(link, util_text_view(&call->cfg->url));
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
		       ? godbolt_finish(call, TOOL_FAILED) : GODBOLT_GO;
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
		d = godbolt_cat(d, CSTR_VIEW(",\"libs\":[]}"));
	}
	call->len = (size_t)(d - call->body);
	if (call->job < call->jobs)
		return GODBOLT_GO;
	call->len = (size_t)(godbolt_cat(d, CSTR_VIEW("]}]}")) - call->body);
	call->job = 0;
	call->phase = GODBOLT_JOB;
	if (call->cfg->share == GODBOLT_SHARE_STATE)
		return godbolt_state_link(call) ? godbolt_finish(call, TOOL_FAILED) : GODBOLT_GO;
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
		                    (int)call->why.len, call->why.data)
		       ? godbolt_finish(call, TOOL_FAILED) : GODBOLT_GO;
	struct json_object *doc = godbolt_resp_doc(call);
	struct cstr_view const link = jb_view(jb_get(doc, "url"));
	if (link.len <= GODBOLT_SHORT_MAX && util_url_scheme(link)) {
		godbolt_put(call, CSTR_VIEW("Link: "));
		godbolt_put(call, link);
		godbolt_put(call, CSTR_VIEW("\n"));
	} else {
		godbolt_put(call, CSTR_VIEW("Link: none. Compiler Explorer sent no valid link.\n"));
	}
	json_object_put(doc);
	return call->e ? godbolt_finish(call, TOOL_FAILED) : GODBOLT_GO;
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
	godbolt_put(call, util_text_view(&call->why));
	call->rec.marks |= GODBOLT_REC_HEADED;
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
godbolt_unanswered (struct godbolt       *call,
                    enum godbolt_outcome  count)
{
	++call->counts[count];
	if (godbolt_header(call))
		return godbolt_finish(call, TOOL_FAILED);
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
		godbolt_put(call, CSTR_VIEW("data_model describes only the sizes of int, long "
		                            "and pointers.\n"));
	godbolt_putf(call, "%" PRIu32 " compilation%s: %" PRIu32 " with exit code 0, %" PRIu32
	             " with another exit code or a timeout, %" PRIu32 " with a failed request, "
	             "%" PRIu32 " not sent.\n", call->jobs, util_s(call->jobs), c[GODBOLT_ZERO],
	             c[GODBOLT_OTHER], c[GODBOLT_FAILED], c[GODBOLT_UNSENT]);
	return godbolt_finish(call, c[GODBOLT_ZERO] || c[GODBOLT_OTHER] ? 0 : TOOL_FAILED);
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
		godbolt_why(call, "not sent because the output is full");
		return godbolt_unanswered(call, GODBOLT_UNSENT);
	}
	if (r->from == GODBOLT_FROM_NONE) {
		godbolt_why(call, "not sent because MSVC cannot output the predefined macros");
		return godbolt_unanswered(call, GODBOLT_UNSENT);
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
		return godbolt_unanswered(call, GODBOLT_FAILED);
	if (got != GODBOLT_GO)
		return got;
	json_tokener_reset(call->tok);
	call->off = 0;
	call->phase = GODBOLT_PARSE;
	return GODBOLT_GO;
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
		call->rec.secs[GODBOLT_SEC_ASM] = jb_get(call->doc, "asm");
	if (from == GODBOLT_FROM_PP) {
		struct json_object *pp = jb_get(call->doc, "ppOutput");
		struct json_object *text = jb_get(pp, "output");
		struct json_object *filtered = jb_get(pp, "numberOfLinesFiltered");
		call->rec.secs[main] = text;
		call->rec.marks |= (json_object_is_type(text, json_type_string) ? 0 : GODBOLT_REC_NO_PP) |
		                   (jb_int(filtered, 0) ? GODBOLT_REC_TRUNCATED : 0);
	}
	call->rec.secs[from == GODBOLT_FROM_STDOUT ? main : GODBOLT_SEC_STDOUT] =
		jb_get(call->doc, "stdout");
	call->rec.secs[GODBOLT_SEC_STDERR] = jb_get(call->doc, "stderr");
	if (!(call->args.flags & GODBOLT_WITH_RUN))
		return;
	struct json_object *run = jb_get(call->doc, "execResult");
	call->rec.secs[GODBOLT_SEC_BUILD] = jb_true(jb_get(run, "didExecute")) ? nullptr
	                                    : jb_get(jb_get(run, "buildResult"), "stderr");
	call->rec.secs[GODBOLT_SEC_RUN_STDOUT] = jb_get(run, "stdout");
	call->rec.secs[GODBOLT_SEC_RUN_STDERR] = jb_get(run, "stderr");
	call->rec.marks |= jb_true(jb_get(run, "truncated")) ? GODBOLT_REC_TRUNCATED : 0;
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
		bytes += jb_view(jb_get(json_object_array_get_idx(v, i), "text")).len + 1;
	char *text = malloc(bytes);
	if (!text) {
		*e = ENOMEM;
		return nullptr;
	}
	char *d = text;
	for (size_t i = 0; i < n; ++i) {
		d = godbolt_cat(d, jb_view(jb_get(json_object_array_get_idx(v, i), "text")));
		*d++ = '\n';
	}
	*len = (size_t)(d - text);
	loop_work(call->loop, LOOP_COPY, *len);
	*e = 0;
	return text;
}

/* Parses the output of the probe of a call of godbolt_targets, which is the
 * section pp of the record: a string, or the lines of stdout. If the
 * output contains a valid block, the record contains the facts and the
 * messages of the compiler. Otherwise it contains the output, and its
 * status is the reason. The function parses the whole output in one step.
 * Returns 0, or ENOMEM.
 */
static int
godbolt_targets_parse (struct godbolt *call)
{
	struct json_object *v = call->rec.secs[GODBOLT_SEC_PP];
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
	loop_work(call->loop, LOOP_PARSE, text.len);
	free(joined);
	joined = nullptr;
	text = (struct cstr_view){};
	if (e == ENOMEM)
		return ENOMEM;
	if (e) {
		godbolt_why(call, "the probe output is not valid (%s)", call->probe.why);
		return 0;
	}
	call->rec.marks |= GODBOLT_REC_PROBED;
	call->rec.secs[GODBOLT_SEC_PP] = nullptr;
	call->rec.secs[GODBOLT_SEC_STDOUT] = nullptr;
	return 0;
}

/* Reads the parsed response to a compilation, and stores its status in
 * call->why and the texts of its sections in call->rec.secs. The next step
 * writes the record.
 */
static enum godbolt_go
godbolt_classify (struct godbolt *call)
{
	struct json_object *code = jb_get(call->doc, "code");
	if (!json_object_is_type(code, json_type_int)) {
		godbolt_why(call, "the response is not a JSON object with an integer member "
		            "code");
		++call->counts[GODBOLT_FAILED];
		call->phase = GODBOLT_FREE;
		return godbolt_header(call) ? godbolt_finish(call, TOOL_FAILED) : GODBOLT_GO;
	}
	bool const timed = jb_true(jb_get(call->doc, "timedOut"));
	if (jb_true(jb_get(call->doc, "truncated")))
		call->rec.marks |= GODBOLT_REC_TRUNCATED;
	int64_t const n = json_object_get_int64(code);
	if (timed)
		godbolt_why(call, "timed out on Compiler Explorer");
	else
		godbolt_why(call, "exit code %" PRId64, n);
	bool const zero = !n && !timed;
	++call->counts[zero ? GODBOLT_ZERO : GODBOLT_OTHER];
	godbolt_sections_find(call);
	call->phase = GODBOLT_WRITE;
	if (call->tool != GODBOLT_TARGETS || !zero)
		return GODBOLT_GO;
	int const e = godbolt_targets_parse(call);
	return e ? godbolt_abort(call, e) : GODBOLT_GO;
}

#undef godbolt_abort

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
		godbolt_why(call, "the response is not valid JSON (%s)",
		            json_tokener_error_desc(err));
	else if (call->off < body.len)
		return GODBOLT_GO;
	else
		godbolt_why(call, "the response is not valid JSON (the text ends early)");
	return godbolt_unanswered(call, GODBOLT_FAILED);
}

/* Ends the record at the limit of the output, with a note on the cut.
 * Later compilations are not sent. Returns 0, or call->e.
 */
static int
godbolt_full (struct godbolt *call)
{
	call->full = 1;
	call->rec.marks |= GODBOLT_REC_CUT;
	return godbolt_put(call, CSTR_VIEW("\n[The output is cut here at 14 MiB.]\n"));
}

static_assert(GODBOLT_FULL == 14 << 20, "the text of the note has 14 MiB");

/* Returns the length of the CSI escape sequence at the start of @p s, such
 * as a color code in a message of a compiler, or 0 if @p s does not start
 * with one of at most GODBOLT_CSI_MAX bytes. A CSI sequence is ESC [, then
 * bytes from 0x20 to 0x3f, and then a final byte from 0x40 to 0x7e.
 */
static size_t
godbolt_csi (struct cstr_view s)
{
	if (!cstr_view_starts(s, CSTR_VIEW("\x1b[")))
		return 0;
	size_t const n = s.len < GODBOLT_CSI_MAX ? s.len : GODBOLT_CSI_MAX;
	size_t i = 2;
	while (i < n && s.data[i] >= 0x20 && s.data[i] <= 0x3f)
		++i;
	return i < n && s.data[i] >= 0x40 && s.data[i] <= 0x7e ? i + 1 : 0;
}

/* Writes as much of @p s, from call->rec.at.byte on, as the bounds of
 * the step and the room left in the output allow, and advances
 * call->rec.at.byte. The function skips each CSI escape sequence
 * (godbolt_csi()), so a part that it writes ends before the next ESC. If
 * no room is left in the output, or room only for the first bytes of a
 * UTF-8 character, the function ends the record with the note of
 * godbolt_full(). Returns 0, or call->e.
 */
static int
godbolt_slice (struct godbolt   *call,
               struct cstr_view  s)
{
	while (call->rec.at.byte < s.len && !call->e) {
		if (call->written >= GODBOLT_FULL)
			return godbolt_full(call);
		size_t const left = GODBOLT_FULL - call->written;
		uint64_t const room = loop_room(call->loop, LOOP_COPY);
		struct cstr_view const rest = cstr_view_slice(s, call->rec.at.byte, SIZE_MAX);
		struct cstr_view part = cstr_view_slice(rest, 0, room < left ? room : left);
		if (!part.len)
			return call->e;
		size_t const csi = godbolt_csi(rest);
		if (csi) {
			loop_work(call->loop, LOOP_COPY, csi);
			call->rec.at.byte += csi;
			continue;
		}
		/* A UTF-8 character cannot contain an ESC, so a part that ends
		 * before an ESC is written whole, without TOOL_MORE. */
		char const *esc = memchr(part.data + 1, '\x1b', part.len - 1);
		uint32_t more = 0;
		if (esc)
			part.len = (size_t)(esc - part.data);
		else if (call->rec.at.byte + part.len < s.len)
			more = TOOL_MORE;
		size_t taken = 0;
		call->e = mcp_slot_write(call->slot, part, more, &taken);
		call->written += taken;
		call->rec.at.byte += taken;
		if (!taken)
			return part.len < left ? call->e : godbolt_full(call);
	}
	return call->e;
}

/* Ends the section call->rec.at.section, with a note if Compiler
 * Explorer cut it, and makes call->rec.at.section the next section.
 * Returns 0, or call->e.
 */
static int
godbolt_section_end (struct godbolt *call)
{
	if (call->rec.marks & GODBOLT_REC_MARKED) {
		godbolt_put(call, godbolt_cut_note);
		call->rec.marks |= GODBOLT_REC_CUT;
	}
	call->rec.marks &= ~(GODBOLT_REC_MARKED | GODBOLT_REC_OPENED);
	call->rec.at = (struct tool_cursor){ .section = call->rec.at.section + 1 };
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
	if (godbolt_slice(call, text) || call->full || call->rec.at.byte < text.len)
		return call->e;
	call->rec.marks |= cstr_view_ends(text, mark) ? GODBOLT_REC_MARKED : 0;
	if (text.data[text.len - 1] != '\n')
		godbolt_put(call, CSTR_VIEW("\n"));
	return godbolt_section_end(call);
}

/* Writes as many lines of the JSON array @p v of @p n lines as the bounds
 * of the step allow, each with a newline after it. Each line is the member
 * text of an element, and godbolt_slice() writes it, so that
 * call->rec.at.item is the line and call->rec.at.byte the byte of the
 * line. The lookups of a line count as one value of LOOP_VALUES work.
 * After the last line, the function ends the section. Returns 0, or
 * call->e.
 */
static int
godbolt_section_lines (struct godbolt     *call,
                       struct json_object *v,
                       size_t              n)
{
	for (; call->rec.at.item < n; ++call->rec.at.item) {
		if (call->written >= GODBOLT_FULL)
			return godbolt_full(call);
		if (call->e || !loop_room(call->loop, LOOP_COPY) ||
		    !loop_room(call->loop, LOOP_VALUES))
			return call->e;
		loop_work(call->loop, LOOP_VALUES, 1);
		struct json_object *element = json_object_array_get_idx(v, call->rec.at.item);
		struct cstr_view const line = jb_view(jb_get(element, "text"));
		if (godbolt_slice(call, line) || call->full || call->rec.at.byte < line.len)
			return call->e;
		godbolt_put(call, CSTR_VIEW("\n"));
		if (cstr_view_equal(line, CSTR_VIEW("[truncated; too many lines]")) ||
		    (call->rec.at.item + 1 == n && cstr_view_equal(line, CSTR_VIEW("[Truncated]"))))
			call->rec.marks |= GODBOLT_REC_MARKED;
		call->rec.at.byte = 0;
	}
	return godbolt_section_end(call);
}

/* Writes as much of the section call->rec.at.section of the record as
 * the bounds of the step allow. A section whose text is empty is left
 * out. After the end of the section, call->rec.at.section is the next
 * section. Returns 0, or call->e.
 */
static int
godbolt_section (struct godbolt *call)
{
	struct json_object *v = call->rec.secs[call->rec.at.section];
	struct cstr_view const text = jb_view(v);
	/* text.len is 0 for an array, and jb_len() is 0 for a string. */
	size_t const n = text.len + jb_len(v);
	if (!n)
		return godbolt_section_end(call);
	if (!(call->rec.marks & GODBOLT_REC_OPENED))
		godbolt_put(call, godbolt_sections[call->rec.at.section]);
	call->rec.marks |= GODBOLT_REC_OPENED;
	return text.data ? godbolt_section_text(call, text) : godbolt_section_lines(call, v, n);
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
		[GODBOLT_PROBE_ARCHITECTURE] = CSTR_VIEW_INIT("architecture: "),
		[GODBOLT_PROBE_OS]           = CSTR_VIEW_INIT("os: "),
		[GODBOLT_PROBE_OS_FAMILY]    = CSTR_VIEW_INIT("os_family: "),
		[GODBOLT_PROBE_DATA_MODEL]   = CSTR_VIEW_INIT("data_model: "),
		[GODBOLT_PROBE_POINTER_BITS] = CSTR_VIEW_INIT("pointer_bits: "),
		[GODBOLT_PROBE_ENDIANNESS]   = CSTR_VIEW_INIT("endianness: "),
		[GODBOLT_PROBE_LIBC]         = CSTR_VIEW_INIT("libc: "),
	};
	static struct cstr_view const versions[] = {
		CSTR_VIEW_INIT("__clang_major__"), CSTR_VIEW_INIT("__GNUC__"),
		CSTR_VIEW_INIT("__GNUC_MINOR__"), CSTR_VIEW_INIT("_MSC_VER"),
		CSTR_VIEW_INIT("__STDC_VERSION__"), CSTR_VIEW_INIT("__cplusplus")
	};
	struct godbolt_probe const *p = &call->probe;
	for (size_t i = 0; i < array_size(names); ++i) {
		struct godbolt_probe_claim const *c = &p->facts[i];
		godbolt_put(call, names[i]);
		if (c->conflict)
			godbolt_put(call, CSTR_VIEW("conflicting"));
		else if (i == GODBOLT_PROBE_POINTER_BITS && p->pointer_bits)
			godbolt_putf(call, "%" PRIu64, p->pointer_bits);
		else if (c->value)
			godbolt_put(call, cstr_view(c->value));
		else if (i == GODBOLT_PROBE_LIBC && !(call->args.flags & GODBOLT_WITH_HEADERS))
			godbolt_put(call, CSTR_VIEW("unknown without headers"));
		else
			godbolt_put(call, CSTR_VIEW("unknown"));
		for (uint32_t k = 0; k < c->n; ++k) {
			godbolt_put(call, k ? CSTR_VIEW(", ") : CSTR_VIEW(" (from "));
			godbolt_put(call, cstr_view(c->evidence[k]));
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
	if (!(call->args.flags & GODBOLT_WITH_HEADERS))
		return call->e;
	for (uint32_t k = 0; k < p->n_headers; ++k) {
		godbolt_put(call, k ? CSTR_VIEW(", ") : CSTR_VIEW("headers: "));
		godbolt_put_field(call, cstr_view(p->headers[k].name));
		godbolt_put(call, CSTR_VIEW(" "));
		godbolt_put_field(call, cstr_view(p->headers[k].value));
	}
	return godbolt_put(call, p->n_headers ? CSTR_VIEW("\n") : CSTR_VIEW("headers: unknown\n"));
}

/* Writes the line about the run of the program of the compilation, which
 * contains its exit code, "timed out" or "not run". Returns 0, or call->e.
 */
static int
godbolt_run_line (struct godbolt *call)
{
	struct json_object *run = jb_get(call->doc, "execResult");
	struct json_object *code = jb_get(run, "code");
	if (jb_true(jb_get(run, "timedOut")))
		return godbolt_put(call, CSTR_VIEW("program: timed out\n"));
	if (!jb_true(jb_get(run, "didExecute")) || !json_object_is_type(code, json_type_int))
		return godbolt_put(call, CSTR_VIEW("program: not run\n"));
	return godbolt_putf(call, "program: exit code %" PRId64 "\n", json_object_get_int64(code));
}

/* The phase GODBOLT_WRITE. It writes the record of the compilation, as
 * much of it as the bounds of the step allow.
 */
static enum godbolt_go
godbolt_write (struct godbolt *call)
{
	if (!(call->rec.marks & GODBOLT_REC_HEADED)) {
		godbolt_header(call);
		if (call->rec.marks & GODBOLT_REC_NO_PP)
			godbolt_put(call, CSTR_VIEW("Compiler Explorer sent no preprocessor output "
			                            "for the compiler.\n"));
		if (call->rec.marks & GODBOLT_REC_PROBED)
			godbolt_facts(call);
		if (call->args.flags & GODBOLT_WITH_RUN)
			godbolt_run_line(call);
	}
	/* A record begins only while the output is not full, and a step that
	 * fills the output ends the loop. */
	while (call->rec.at.section < array_size(call->rec.secs)) {
		size_t const sec = call->rec.at.section;
		if (godbolt_section(call))
			return godbolt_finish(call, TOOL_FAILED);
		if (call->full)
			break;
		if (call->rec.at.section == sec)
			return GODBOLT_GO;
	}
	if ((call->rec.marks & (GODBOLT_REC_TRUNCATED | GODBOLT_REC_CUT)) == GODBOLT_REC_TRUNCATED)
		godbolt_put(call, godbolt_cut_note);
	if (call->e)
		return godbolt_finish(call, TOOL_FAILED);
	call->phase = GODBOLT_FREE;
	return GODBOLT_GO;
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
		while (jb_members(v) || jb_len(v)) {
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
	if (!call->doc)
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

/* Returns the own word of @p slot if it points to a call, or nullptr. */
static void **
godbolt_own (struct mcp_slot *slot)
{
	void **own = slot ? mcp_slot_own(slot) : nullptr;
	return own && *own ? own : nullptr;
}

/* The step() of every kind of the plugin. It runs the call's phase, and it
 * queues the next step if the phase goes on.
 */
static void
godbolt_step (struct mcp_slot *slot)
{
	void **own = godbolt_own(slot);
	if (!own)
		return;
	struct godbolt *call = *own;
	if (call->wait != GODBOLT_TIMER && godbolt_phases[call->phase](call) == GODBOLT_GO)
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
	call->tok = nullptr;
	json_object_put(call->doc);
	call->doc = nullptr;
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
	void **own = godbolt_own(slot);
	if (!own)
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

/* Reads the array of ids @p v into a->ids. Returns 0, or EINVAL. */
static int
godbolt_args_ids (struct godbolt_args *a,
                  struct json_object  *v)
{
	size_t const n = jb_len(v);
	if (!n || n > array_size(a->ids))
		return EINVAL;
	for (size_t i = 0; i < n; ++i) {
		struct cstr_view const id = godbolt_id(json_object_array_get_idx(v, i));
		if (!id.data || cstr_view_index(a->ids, i, id) < i)
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
	if (!n || n > array_size(a->strings))
		return EINVAL;
	for (size_t i = 0; i < n; ++i) {
		a->strings[i] = jb_text(json_object_array_get_idx(v, i), 0, GODBOLT_ARG_MAX,
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
	struct cstr_view const t = jb_text(v, 0, GODBOLT_MATCH_MAX, nullptr);
	if (!t.data)
		return EINVAL;
	static char const space[] = " \t\n\v\f\r";
	a->n_words = 0;
	for (size_t i = strspn(t.data, space); i < t.len; i += strspn(t.data + i, space)) {
		if (a->n_words == array_size(a->words))
			return EINVAL;
		size_t const k = strcspn(t.data + i, space);
		a->words[a->n_words++] = (struct cstr_view){ t.data + i, k };
		i += k;
	}
	return 0;
}

/* Reads the argument filters @p v into the filters of a->flags. Returns 0,
 * or EINVAL.
 */
static int
godbolt_args_filters (struct godbolt_args *a,
                      struct json_object  *v)
{
	if (!json_object_is_type(v, json_type_object))
		return EINVAL;
	struct json_object_iter it;
	json_object_object_foreachC(v, it) {
		size_t const i = cstr_view_index(godbolt_filters, array_size(godbolt_filters),
		                                 cstr_view(it.key));
		if (!(GODBOLT_SETTABLE >> i & 1) || !json_object_is_type(it.val, json_type_boolean))
			return EINVAL;
		a->flags = json_object_get_boolean(it.val) ? a->flags | 1u << i : a->flags & ~(1u << i);
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
	case GODBOLT_ARG_ACTION:
		a->action = (uint8_t)cstr_view_index(godbolt_action_names,
		                                     array_size(godbolt_action_names), jb_view(v));
		return a->action < array_size(godbolt_action_names) ? 0 : EINVAL;
	case GODBOLT_ARG_LANGUAGE:
		a->language = jb_text(v, 1, GODBOLT_LANGUAGE_MAX, godbolt_language_bytes);
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
		*value = jb_text(v, 1, GODBOLT_VALUE_MAX, nullptr);
		return value->data ? 0 : EINVAL;
	}
	case GODBOLT_ARG_LIMIT:
		a->limit = jb_count(v, GODBOLT_LIMIT_MAX);
		return a->limit ? 0 : EINVAL;
	case GODBOLT_ARG_OFFSET:
		/* mcp_slot_offset() checks the offset. */
		a->flags |= jb_int(v, 0) > 0 ? GODBOLT_WITH_OFFSET : 0;
		return 0;
	case GODBOLT_ARG_ARGS:
		return godbolt_args_strings(a, v);
	case GODBOLT_ARG_SOURCE:
		a->source = jb_text(v, 0, GODBOLT_SOURCE_MAX, nullptr);
		return a->source.data ? 0 : EINVAL;
	case GODBOLT_ARG_FILTERS:
		return godbolt_args_filters(a, v);
	case GODBOLT_ARG_HEADERS:
	case GODBOLT_ARG_SHARE:
	case GODBOLT_ARG_RUN:
		if (!json_object_is_type(v, json_type_boolean))
			return EINVAL;
		if (json_object_get_boolean(v))
			a->flags |= arg == GODBOLT_ARG_HEADERS ? GODBOLT_WITH_HEADERS
			          : arg == GODBOLT_ARG_SHARE ? GODBOLT_WITH_SHARE : GODBOLT_WITH_RUN;
		return 0;
	case GODBOLT_ARG_COUNT:
		break;
	}
	return EINVAL;
}

/* Checks the arguments @p args of a call of the tool @p tool, and reads
 * them into @p a. If @p cfg turns links off, the function refuses the
 * argument share. Returns 0, or EINVAL with the reason of the refusal in
 * the @p len bytes at @p why.
 */
static int
godbolt_args_read (struct godbolt_args         *a,
                   struct json_object          *args,
                   enum godbolt_tool            tool,
                   struct godbolt_config const *cfg,
                   char                        *why,
                   size_t                       len)
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
		.flags = tool == GODBOLT_COMPILE ? GODBOLT_SITE_FILTERS : 0
	};
	if (args && !json_object_is_type(args, json_type_object))
		return util_refuse(why, len, "The arguments must be a JSON object.");
	struct json_object *of[array_size(godbolt_arg_names)] = {};
	uint64_t seen;
	struct cstr_view other;
	if (jb_args(args, godbolt_arg_names, array_size(godbolt_arg_names), t->args & ~off, of,
	            &seen, &other))
		return t->args & off && cstr_view_equal(other, godbolt_arg_names[GODBOLT_ARG_SHARE])
		       ? util_refuse(why, len, "Links are turned off on this server. Call the "
		                     "tool without share.")
		       : util_refuse(why, len, "%.*s is not an argument of the tool.",
		                     (int)other.len, other.data);
	for (uint64_t m = seen; m; m &= m - 1) {
		size_t const i = stdc_trailing_zeros(m);
		if (godbolt_args_set(a, (enum godbolt_arg)i, of[i]))
			return util_refuse(why, len, "%s", godbolt_arg_rules[i].data);
	}
	uint32_t const missing = t->needs & ~(uint32_t)seen;
	if (missing)
		return util_refuse(why, len, "The argument %s is required.",
		                   godbolt_arg_names[stdc_trailing_zeros(missing)].data);
	/* The bits of kind, family and architecture are adjacent. */
	if (a->action == GODBOLT_LANGUAGES && seen >> GODBOLT_ARG_KIND & 7)
		return util_refuse(why, len, "kind, family and architecture are not valid "
		                   "with action languages.");
	if ((uint32_t)a->n_ids * a->n_strings > GODBOLT_JOBS)
		return util_refuse(why, len, "The number of compilers times the number of "
		                   "strings in args must be at most 32.");
	/* ids[0] is filled from the list of languages. */
	if (tool != GODBOLT_CATALOG && !a->n_ids) {
		a->n_ids = 1;
		a->action = GODBOLT_LANGUAGES;
	}
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
 * work. The function replaces an empty source with "\n", as godbolt-c
 * does. The source of godbolt_targets is the probe. If the argument
 * headers is true, GODBOLT_HEADERS_LINE comes before the probe, so that
 * the compiler includes the target headers. Returns the state. On failure
 * the function returns nullptr and stores ENOMEM in @p *e.
 */
static struct godbolt *
godbolt_create (struct godbolt_args const *a,
                enum godbolt_tool          tool,
                struct loop               *loop,
                int                       *e)
{
	struct cstr_view const head = a->flags & GODBOLT_WITH_HEADERS
	                            ? CSTR_VIEW(GODBOLT_HEADERS_LINE) : (struct cstr_view){};
	struct cstr_view const source = tool == GODBOLT_TARGETS ? godbolt_probe_source()
	                              : a->source.len ? a->source : CSTR_VIEW("\n");
	size_t const bytes = a->language.len + head.len + source.len +
	                     cstr_view_total(a->values, array_size(a->values)) +
	                     cstr_view_total(a->ids, a->n_ids) +
	                     cstr_view_total(a->strings, a->n_strings) +
	                     cstr_view_total(a->words, a->n_words);
	struct godbolt *call = malloc(sizeof *call + bytes);
	struct json_tokener *tok = call ? json_tokener_new() : nullptr;
	if (!tok) {
		free(call);
		*e = ENOMEM;
		return nullptr;
	}
	*call = (struct godbolt){
		.timer = { .due = UINT64_MAX, .fn = godbolt_fired },
		.args = *a,
		.tok = tok,
		.loop = loop,
		.missing = (1u << a->n_ids) - 1,
		.jobs = (uint8_t)(a->n_ids * a->n_strings),
		.tool = tool
	};
	char *p = call->blob;
	for (uint8_t i = 0; i < a->n_ids; ++i)
		call->args.ids[i] = godbolt_copy(&p, a->ids[i]);
	for (uint8_t i = 0; i < a->n_strings; ++i)
		call->args.strings[i] = godbolt_copy(&p, a->strings[i]);
	for (uint8_t i = 0; i < a->n_words; ++i)
		call->args.words[i] = godbolt_copy(&p, a->words[i]);
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
 * escape the source in their steps first, and then read their lists.
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
	if (!cfg || !loop || !own || cfg->url.len >= sizeof cfg->url.data ||
	    cfg->share >= array_size(godbolt_shares)) {
		mcp_slot_tellf(slot, TOOL_FAILED, "The godbolt plugin cannot start the call "
		               "because of an internal error.");
		return;
	}
	struct godbolt_args a;
	char why[TOOL_TELL_MAX];
	if (godbolt_args_read(&a, args, tool, cfg, why, sizeof why)) {
		mcp_slot_tell(slot, cstr_view(why), TOOL_FAILED);
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

/* Stores the URL @p v in the configuration @p c without its trailing '/'.
 * Returns 0, or EINVAL if @p v is not an http or https URL of at most
 * GODBOLT_URL_MAX bytes, or if it has a user name, a password, a query, a
 * fragment or a byte that is not printable ASCII or is a space.
 */
static int
godbolt_config_url (struct godbolt_config *c,
                    struct json_object    *v)
{
	struct cstr_view const t = jb_text(v, 1, GODBOLT_URL_MAX, nullptr);
	size_t const origin = util_url_origin(t);
	if (!origin || strpbrk(t.data, "?#"))
		return EINVAL;
	size_t n = t.len;
	while (n > origin && t.data[n - 1] == '/')
		--n;
	return util_text_store(&c->url, cstr_view_slice(t, 0, n));
}

/* Stores the option share @p v in the configuration @p c. Returns 0, or
 * EINVAL if @p v is not short, state or off.
 */
static int
godbolt_config_share (struct godbolt_config *c,
                      struct json_object    *v)
{
	size_t const share = cstr_view_index(godbolt_shares, array_size(godbolt_shares), jb_view(v));
	if (share == array_size(godbolt_shares))
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
		return util_refuse(why, len, "askai passed no storage for the "
		                   "configuration");
	struct godbolt_config *c = config;
	*c = godbolt_defaults;
	if (!options)
		return 0;
	if (!json_object_is_type(options, json_type_object))
		return util_refuse(why, len, "not an object");
	static struct cstr_view const names[] = { CSTR_VIEW_INIT("url"), CSTR_VIEW_INIT("share") };
	struct json_object *of[array_size(names)] = {};
	uint64_t seen;
	struct cstr_view other;
	if (jb_args(options, names, array_size(names), UINT64_MAX, of, &seen, &other))
		return util_refuse(why, len, "the member %.*s is not an option", (int)other.len,
		                   other.data);
	if (seen & 1 && godbolt_config_url(c, of[0]))
		return util_refuse(why, len, "url must be an http or https URL of at most 255 "
		                   "bytes, without a user name, a password, a query or a "
		                   "fragment");
	if (seen & 2 && godbolt_config_share(c, of[1]))
		return util_refuse(why, len, "share must be "
		                   XL_JOIN(", ", " or ", GODBOLT_SHARE_LIST(XL_TEXT)));
	return 0;
}

/* The parts of the definitions that several tools share. */
#define GODBOLT_DEF_LANGUAGE(what, examples)                                    \
        "\"language\":{\"type\":\"string\",\"default\":\"c\",\"description\":"  \
        "\"The Compiler Explorer id of the language of " what ", such as "       \
        examples ".\"}"
#define GODBOLT_DEF_SOURCE                                                      \
        "\"source\":{\"type\":\"string\",\"description\":\"The source code "    \
        "itself, not a file name. Includes resolve only to the compiler "       \
        "headers on Compiler Explorer, not to repository files. The source "    \
        "is at most 1 MiB of UTF-8.\"}"
#define GODBOLT_DEF_COMPILERS                                                   \
        "\"compilers\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"     \
        "\"minItems\":1,\"maxItems\":16,\"uniqueItems\":true,\"description\":"  \
        "\"Exact compiler ids from godbolt_catalog, not display names. "        \
        "Without compilers, the call uses the default compiler of the "         \
        "language. The number of compilers times the number of strings in "     \
        "args must be at most 32.\"}"
#define GODBOLT_DEF_ARGS                                                        \
        "\"args\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"          \
        "\"minItems\":1,\"maxItems\":8,\"default\":[\"\"],\"description\":"     \
        "\"Argument strings, such as \\\"-O2 -std=c23\\\". Each string is "     \
        "the argument list of one compilation, without shell processing. A "    \
        "string is at most 8192 bytes. GCC before 14 and Clang before 18 "      \
        "reject -std=c23.\"}"
#define GODBOLT_DEF_OFFSET                                                      \
        "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"description\":\""     \
        TOOL_OFFSET_ABOUT "\"}"
#define GODBOLT_FILTER_SCHEMA(id, name, set, site) \
        GODBOLT_IF_##set("\"" name "\":{\"type\":\"boolean\",\"default\":" #site "}",)
#define GODBOLT_DEF_HINTS                                                       \
        "\"annotations\":{\"readOnlyHint\":true,\"destructiveHint\":false,"      \
        "\"idempotentHint\":true,\"openWorldHint\":true}"

/* The definition of each tool: the JSON text, and the description before
 * and after the base URL of the configuration.
 */
static struct {
	char const      *json;
	struct cstr_view head;
	struct cstr_view tail;
} const godbolt_defs[] = {
	[GODBOLT_CATALOG] = {
		"{\"name\":\"godbolt_catalog\",\"title\":\"Compiler Explorer catalog\","
		"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		"\"action\":{\"type\":\"string\",\"enum\":["
		XL_JOIN(",", ",", GODBOLT_ACTION_LIST(XL_JSON)) "],\"description\":\"languages "
		"lists each language and the default compiler of the language. "
		"compilers lists the compilers for the language argument. kinds counts "
		"compilers per compilerType. families counts compilers per family tag. "
		"architectures counts compilers per instruction set.\"},"
		GODBOLT_DEF_LANGUAGE("the compilers", "c, c++ or rust") ","
		"\"ids\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
		"\"minItems\":1,\"maxItems\":16,\"uniqueItems\":true,\"description\":"
		"\"Exact ids of the items to list. The call fails if an id is not in the "
		"list or another filter excludes the id.\"},"
		"\"match\":{\"type\":\"string\",\"description\":\"Words to find in any "
		"letter case, such as \\\"gcc 14\\\". Each word must occur in a column of "
		"the item. For action languages, the default column does not count. "
		"match is at most 8 words and 256 bytes.\"},"
		"\"kind\":{\"type\":\"string\",\"description\":\"The exact compilerType "
		"of the compilers to list, such as clang or win32-vc. The kind default "
		"means no compilerType and a GCC-style driver. Action kinds lists the "
		"kinds.\"},"
		"\"family\":{\"type\":\"string\",\"description\":\"The exact family tag "
		"of the compilers to list, such as gcc or clang. Action families lists "
		"the tags.\"},"
		"\"architecture\":{\"type\":\"string\",\"description\":\"The exact "
		"instruction set of the compilers to list, such as amd64 or aarch64. "
		"Action architectures lists the instruction sets.\"},"
		"\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2000,"
		"\"default\":100,\"description\":\"The maximum number of items in the "
		"output. The last output line contains the number of matching "
		"items.\"},"
		GODBOLT_DEF_OFFSET "},"
		"\"required\":[\"action\"],\"additionalProperties\":false},"
		GODBOLT_DEF_HINTS "}",
		CSTR_VIEW_INIT("List the languages, the compilers or the compiler counts of "
		               "Compiler Explorer at "),
		CSTR_VIEW_INIT(". The output is tab-separated, with the column names on the first "
		               "line. All filters apply together. kind, family and architecture "
		               "are not valid with action languages. Give the other godbolt tools "
		               "exact compiler ids from action compilers."),
	},
	[GODBOLT_COMPILE] = {
		"{\"name\":\"godbolt_compile\",\"title\":\"Compile on Compiler Explorer\","
		"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		GODBOLT_DEF_COMPILERS "," GODBOLT_DEF_ARGS ","
		GODBOLT_DEF_LANGUAGE("the source", "c, c++ or rust") ","
		GODBOLT_DEF_SOURCE ","
		"\"filters\":{\"type\":\"object\",\"additionalProperties\":false,"
		"\"properties\":{" XL_JOIN(",", ",", GODBOLT_FILTER_LIST(GODBOLT_FILTER_SCHEMA)) "},"
		"\"description\":\"Assembly filters. The defaults match the Compiler "
		"Explorer site: Intel syntax, demangled names, no assembler directives, "
		"no comment-only lines and no unused labels.\"},"
		"\"run\":{\"type\":\"boolean\",\"default\":false,\"description\":\"With "
		"true, Compiler Explorer also runs the program without arguments and "
		"with empty input. Each record then contains the exit code and the "
		"output of the program.\"},"
		GODBOLT_DEF_OFFSET "},"
		"\"required\":[\"source\"],\"additionalProperties\":false},"
		GODBOLT_DEF_HINTS "}",
		CSTR_VIEW_INIT("Compile source code on Compiler Explorer at "),
		CSTR_VIEW_INIT(", once for each compiler and each argument string. The output "
		               "contains a record per compilation with the exit code, the assembly "
		               "and the compiler messages. askai sends the source to Compiler "
		               "Explorer."),
	},
	[GODBOLT_PREPROCESS] = {
		"{\"name\":\"godbolt_preprocess\",\"title\":\"Preprocess on Compiler "
		"Explorer\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		GODBOLT_DEF_COMPILERS "," GODBOLT_DEF_ARGS ","
		GODBOLT_DEF_LANGUAGE("the source", "c or c++") ","
		GODBOLT_DEF_SOURCE "," GODBOLT_DEF_OFFSET "},"
		"\"required\":[\"source\"],\"additionalProperties\":false},"
		GODBOLT_DEF_HINTS "}",
		CSTR_VIEW_INIT("Preprocess source code on Compiler Explorer at "),
		CSTR_VIEW_INIT(", once for each compiler and each argument string. The output "
		               "contains a record per compilation with the exit code, the "
		               "preprocessed source and the compiler messages. askai sends the "
		               "source to Compiler Explorer."),
	},
	[GODBOLT_MACROS] = {
		"{\"name\":\"godbolt_macros\",\"title\":\"Predefined macros on Compiler "
		"Explorer\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		GODBOLT_DEF_COMPILERS "," GODBOLT_DEF_ARGS ","
		GODBOLT_DEF_LANGUAGE("the source", "c or c++") ","
		"\"source\":{\"type\":\"string\",\"description\":\"Optional source code, "
		"not a file name. The output also contains the macros that the source "
		"defines. The source is at most 1 MiB of UTF-8.\"},"
		GODBOLT_DEF_OFFSET "},"
		"\"additionalProperties\":false},"
		GODBOLT_DEF_HINTS "}",
		CSTR_VIEW_INIT("Print the predefined macros of compilers on Compiler Explorer at "),
		CSTR_VIEW_INIT(", once for each compiler and each argument string. The output "
		               "contains a record per compilation with the exit code, the macros "
		               "and the compiler messages. askai sends the source to Compiler "
		               "Explorer. Compilers of kind win32-vc (MSVC) cannot output the "
		               "predefined macros. For target facts, the output of godbolt_targets "
		               "is shorter. For a few macros, call godbolt_preprocess with the "
		               "macro names as the source, such as \"__STDC_VERSION__ "
		               "__SIZEOF_LONG__\"."),
	},
	[GODBOLT_TARGETS] = {
		"{\"name\":\"godbolt_targets\",\"title\":\"Target facts from Compiler "
		"Explorer\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		GODBOLT_DEF_COMPILERS "," GODBOLT_DEF_ARGS ","
		GODBOLT_DEF_LANGUAGE("the probe", "c or c++") ","
		"\"headers\":{\"type\":\"boolean\",\"default\":false,\"description\":"
		"\"With true, the probe also includes target headers, such as limits.h "
		"and features.h. The output then contains a headers line. The "
		"compilation can fail when the target headers are missing on Compiler "
		"Explorer. Without headers, libc is unknown.\"},"
		GODBOLT_DEF_OFFSET "},"
		"\"additionalProperties\":false},"
		GODBOLT_DEF_HINTS "}",
		CSTR_VIEW_INIT("Get target facts from the predefined macros of compilers on "
		               "Compiler Explorer at "),
		CSTR_VIEW_INIT(", once for each compiler and each argument string. The facts are "
		               "architecture, os, os_family, data_model, pointer_bits, endianness, "
		               "libc and versions. Each fact line ends with the macros used, such "
		               "as \"(from __x86_64__)\"."),
	},
};

#undef GODBOLT_FILTER_SCHEMA
#undef GODBOLT_IF_false
#undef GODBOLT_IF_true
#undef GODBOLT_FILTER_LIST
#undef GODBOLT_ACTION_LIST
#undef GODBOLT_SHARE_LIST
#undef GODBOLT_DEF_HINTS
#undef GODBOLT_DEF_OFFSET
#undef GODBOLT_DEF_ARGS
#undef GODBOLT_DEF_COMPILERS
#undef GODBOLT_DEF_SOURCE
#undef GODBOLT_DEF_LANGUAGE

/* Returns the definition of the tool @p tool in godbolt_defs[] as a new
 * reference, with the base URL of the configuration @p service in its
 * description. If the tool accepts the argument share and the
 * configuration does not turn links off, the function adds the property
 * share and the sentences on links to the definition. With short links,
 * readOnlyHint is false, because Compiler Explorer stores the source of a
 * link. Returns nullptr if json-c runs out of memory, or if the
 * configuration is not valid.
 */
static struct json_object *
godbolt_definition (void const        *service,
                    enum godbolt_tool  tool)
{
	struct cstr_view const head = godbolt_defs[tool].head;
	struct cstr_view const tail = godbolt_defs[tool].tail;
	struct godbolt_config const *cfg = service ? service : &godbolt_defaults;
	if (cfg->url.len >= sizeof cfg->url.data || cfg->share >= array_size(godbolt_share_notes))
		return nullptr;
	struct cstr_view const url = util_text_view(&cfg->url);
	struct cstr_view const link = godbolt_params[tool].args >> GODBOLT_ARG_SHARE & 1
	                              ? godbolt_share_notes[cfg->share] : (struct cstr_view){};
	char text[2048];
	if (head.len + url.len + tail.len + link.len > sizeof text)
		return nullptr;
	char *end = godbolt_cat(godbolt_cat(text, head), url);
	end = godbolt_cat(godbolt_cat(end, tail), link);
	struct json_object *def = json_tokener_parse(godbolt_defs[tool].json);
	def = jb_set_or_free(def, "description",
	                     json_object_new_string_len(text, (int)(end - text)));
	if (!link.len)
		return def;
	struct json_object *props = jb_get(jb_get(def, "inputSchema"), "properties");
	int e = jb_set(props, "share", json_tokener_parse(
		"{\"type\":\"boolean\",\"default\":false,\"description\":\"With true, "
		"the first line of the output is a Compiler Explorer link.\"}"));
	e = e ? e : jb_set(jb_get(def, "annotations"), "readOnlyHint",
	                   json_object_new_boolean(cfg->share != GODBOLT_SHARE_SHORT));
	if (!e)
		return def;
	json_object_put(def);
	return nullptr;
}

/* The start() and definition() of each kind. */
#define GODBOLT_TOOL_ENTRIES(id, name, flags)                           \
        static void                                                     \
        godbolt_##name##_start (struct mcp_slot    *slot,               \
                                struct json_object *args)               \
        {                                                               \
                godbolt_start(slot, args, GODBOLT_##id);                \
        }                                                               \
                                                                        \
        static struct json_object *                                     \
        godbolt_##name##_definition (void const *service)               \
        {                                                               \
                return godbolt_definition(service, GODBOLT_##id);       \
        }

GODBOLT_TOOL_LIST(GODBOLT_TOOL_ENTRIES)

#undef GODBOLT_TOOL_ENTRIES

#define GODBOLT_TOOL_KIND(id, name, extra)                              \
        {                                                               \
                .definition = godbolt_##name##_definition,              \
                .start = godbolt_##name##_start,                        \
                .step = godbolt_step,                                   \
                .drop = godbolt_drop,                                   \
                .flags = TOOL_LENT | TOOL_PAGED | TOOL_STREAM | (extra) \
        },

struct plugin const askai_plugin = {
	.tools = { GODBOLT_TOOL_LIST(GODBOLT_TOOL_KIND) },
	.configure = godbolt_configure,
	.config_size = sizeof (struct godbolt_config)
};

#undef GODBOLT_TOOL_KIND
#undef GODBOLT_TOOL_LIST
