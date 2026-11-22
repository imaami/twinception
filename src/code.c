/** @file code.c
 *
 * The code tool.
 */
#include <errno.h>
#include <json-c/json.h>
#include <libgen.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "build.h"
#include "child.h"
#include "clk.h"
#include "code.h"
#include "config.h"
#include "cstr_view.h"
#include "cxi.h"
#include "cxi_out.h"
#include "dstr.h"
#include "flag.h"
#include "git.h"
#include "git_run.h"
#include "jb.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "mcp.h"
#include "plugin.h"
#include "schema.h"
#include "seen.h"
#include "util.h"
#include "xlist.h"

enum {
	CODE_WORD   = 1024,      /* the maximum length of a string argument */
	CODE_CHECK  = 4096,      /* the maximum length of the --check output */
	CODE_PARSES = 8,         /* the most processes that parse translation
	                            units at a time: of one loop, and of all
	                            loops together, so that the git processes
	                            of the tools get the other children of
	                            CHILD_MAX */
	CODE_GRAPH  = 1 << 28,   /* the largest graph of a translation unit */
	CODE_UNITS  = 64 << 20,  /* the largest answer of askai-code to callers,
	                            which lists the translation units of the
	                            compile commands */
};
static_assert(CODE_PARSES + 1 <= CHILD_MAX, "the other tools need a child");

/* The service frees builds that no call uses while the graphs of its
 * builds are more bytes than this.
 */
#define CODE_KEEP ((size_t)1 << 30)

/* A call that waits for a build looks at the build again after this time:
 * a process of another loop that ends does not queue the step of the call.
 */
#define CODE_RETRY (10 * MS)

/* The arguments: X(ID, name). */
#define CODE_ARG_LIST(X)        \
        X(REPO,     "repo")     \
        X(ACTION,   "action")   \
        X(PATH,     "path")     \
        X(NAME,     "name")     \
        X(LINE,     "line")     \
        X(WARNINGS, "warnings") \
        X(REVISION, "revision") \
        X(OFFSET,   "offset")

#define CODE_ARG_ENUM(id, ...) CODE_ARG_##id,
enum code_arg : uint8_t { CODE_ARG_LIST(CODE_ARG_ENUM) CODE_ARGS };
#undef CODE_ARG_ENUM

static struct cstr_view const code_arg_names[] = { CODE_ARG_LIST(XL_VIEW) };
#undef CODE_ARG_LIST

static struct cstr_view const code_actions[] = { CXI_ACTION_LIST(XL_VIEW) };
static struct cstr_view const code_keys[] = { CXI_KEY_LIST(XL_VIEW) };

/* The size of a word of code_number(): the longest name of a key, '=', a
 * number and a NUL.
 */
enum { CODE_NUMBER = sizeof "unconfined=" + UTIL_DECIMAL };

struct code_loop;

enum code_parse_flag : uint64_t {
	CODE_AGAIN = FLAG_OWN(0),  /* the unit was BUILD_RETRY: it is not parsed
	                              once more after this process */
};

/* A process of a loop that parses a translation unit of a build: askai-code
 * with unit=N (src/cxi.h). While child is not nullptr, the place has a
 * reference to build, and the unit is BUILD_PARSING.
 */
struct code_parse {
	struct code_loop  *cl;
	struct code_build *build;
	struct child      *child;
	size_t             unit;   /* the index of the unit in the build */
	struct flag64      flags;  /* enum code_parse_flag */
};

/* The code tool's part of a client loop: the processes of the loop that
 * parse for the builds of the service, which are on the list of the
 * children of the loop, and the calls of the loop that wait for a build.
 * Only the thread of the loop reads and writes the part.
 */
struct code_loop {
	struct list        waiting;
	struct list       *children;
	struct loop       *loop;
	struct code       *code;
	struct code_parse  parse[CODE_PARSES];
};

enum code_build_flag : uint64_t {
	CODE_STALLED = FLAG_OWN(0),  /* a process found the tree of the cache at
	                                another commit: no process starts until
	                                a call has exported the commit again */
};

/* A build that the service keeps for all loops: the translation units of
 * the compile commands of a repository at a commit, with their graphs
 * (src/build.h). id is the name that askai-code gives the build in its
 * answer to callers. words contains the words of the request of a unit to
 * askai-code that each unit has, each followed by a NUL.
 *
 * The id, the words, and the path and the index of each unit do not change
 * after the build is created. The lock of the service guards the other
 * members, and the state and the graph of each unit. A unit that is
 * BUILD_GRAPH or BUILD_FAILED does not change, so a thread that has read
 * with the lock that left is 0 reads the units and the graphs without the
 * lock, while it has a reference.
 */
struct code_build {
	struct list   hook;     /* on the builds of the service */
	struct build  b;
	struct dstr   id;
	struct dstr   words;
	size_t        refs;     /* one of the service, one of each call that
	                           has joined the build, and one of each
	                           process that parses for it */
	size_t        next;     /* no unit before this one can be started
	                           (build_waits()) */
	size_t        again;    /* the units that are BUILD_AGAIN */
	size_t        left;     /* the units that are neither BUILD_GRAPH nor
	                           BUILD_FAILED */
	size_t        parsing;  /* the units that are BUILD_PARSING */
	size_t        bytes;    /* of the graphs */
	struct flag64 flags;    /* enum code_build_flag */
};

enum code_call_flag : uint64_t {
	CODE_CALLERS = FLAG_OWN(0),  /* the action is callers */
	CODE_WORDED  = FLAG_OWN(1),  /* words contains the words of
	                                code_spawn() */
	CODE_LIST    = FLAG_OWN(2),  /* the next askai-code of the call lists
	                                the units of the build and exports the
	                                commit */
};

/* A call of the code tool. pin resolves the revision, and pin.child is the
 * askai-code process once the pin is done. words contains the KEY=VALUE
 * words of the request to askai-code, each followed by a NUL.
 *
 * For callers, askai-code names the build and the callee, whose name and
 * file are copied to named. The call then has a reference to the build and
 * waits until the units are parsed or the deadline of the call comes, and
 * answer is the text that the build gives.
 */
struct code_call {
	struct git_pin       pin;
	struct dstr          words;
	struct dstr          named;
	struct cxi_out       answer;
	struct list          wait;    /* on the waiting calls of the loop while
	                                 the call has a build */
	struct loop_timer    timer;   /* when the call looks at the build again */
	struct mcp_slot     *slot;
	struct code_build   *build;   /* nullptr until askai-code has named it */
	struct build_callee  callee;
	size_t               repo;    /* the index of the repository in the
	                                 values of --code */
	struct flag64        flags;   /* enum code_call_flag */
};

int
code_check (struct config *cfg)
{
	char *exe = realpath("/proc/self/exe", nullptr);
	if (!exe)
		return config_aprintf(&cfg->code_check, "askai's executable is not found: %s",
		                      strerror(errno));
	char const *xdg = getenv("XDG_CACHE_HOME");
	char const *home = getenv("HOME");
	char const *top = xdg && *xdg == '/' ? xdg : home && *home == '/' ? home : nullptr;
	int e = config_aprintf(&cfg->code_helper, "%s/askai-code", dirname(exe));
	free(exe);
	exe = nullptr;
	if (!e && !top)
		return config_aprintf(&cfg->code_check, "no cache directory: XDG_CACHE_HOME and HOME "
		                      "are not absolute");
	char *cache = nullptr;
	if (!e)
		e = config_aprintf(&cfg->code_cache, "%s%s/askai/code", top,
		                   top == xdg ? "" : "/.cache");
	if (!e)
		e = config_aprintf(&cache, "cache=%s", cfg->code_cache);
	if (e)
		return e;
	char const *argv[] = {
		cfg->code_helper, "--check", cache,
		flag_any(cfg->flags, CFG_UNCONFINED) ? "unconfined=1" : nullptr, nullptr
	};
	char *env[] = { util_env_path(), nullptr };
	struct child_cmd const cmd = child_cmd(
		.file = cfg->code_helper,
		.argv = argv,
		.envp = env,
		.max = CODE_CHECK,
		.deadline = loop_clock() + 10 * NS,
		.flags = { CHILD_STDERR }
	);
	struct buf out = buf();
	struct child_end end = child_end();
	e = child_capture(&cmd, child_fds(), &out, &end);
	free(cache);
	cache = nullptr;
	/* After a successful check the output is the version of libclang and
	 * the confinement, one line each. Otherwise the first line of the
	 * output is the reason why the tool is off. */
	struct cstr_view const text = cstr_view_span(buf_peek(&out));
	struct cstr_view rest = text;
	struct cstr_view first = text;
	bool const two_lines = cstr_view_cut(&rest, '\n', &first) && rest.len;
	if (e == ENOENT) {
		e = config_aprintf(&cfg->code_check, "no askai-code beside askai");
	} else if (e) {
		e = config_aprintf(&cfg->code_check, "askai-code --check failed: %s", strerror(e));
	} else if (end.status || !two_lines) {
		e = first.len ? config_aprintf(&cfg->code_check, "%.*s", (int)first.len, first.data)
		              : config_aprintf(&cfg->code_check, "askai-code --check exited with "
		                               "status %d", end.status);
	} else {
		flag_set(&cfg->flags, CFG_CODE);
		e = config_aprintf(&cfg->code_check, "%.*s",
		                   (int)(text.len - (text.data[text.len - 1] == '\n')), text.data);
	}
	buf_fini(&out);
	return e;
}

void
code_report (struct config const *cfg)
{
	char const *check = cfg->code_check;
	char const *confinement = strchr(check, '\n');
	LOG_ALL(LOG_INFO, "code tool:");
	LOG_ALL(LOG_INFO, "  helper       %s", cfg->code_helper);
	LOG_ALL(LOG_INFO, "  libclang     %.*s", (int)(confinement - check), check);
	LOG_ALL(LOG_INFO, "  confinement  %s", confinement + 1);
	LOG_ALL(LOG_INFO, "  cache        %s", cfg->code_cache);
	int width = 0;
	for (size_t i = 0; i < cfg->n_codes; ++i) {
		int const len = (int)git_run_name(cfg->code_repos[i]).len;
		width = len > width ? len : width;
	}
	LOG_ALL(LOG_INFO, "code repositories:");
	for (size_t i = 0; i < cfg->n_codes; ++i) {
		struct cstr_view const name = git_run_name(cfg->code_repos[i]);
		LOG_ALL(LOG_INFO, "  %-*.*s  %s", width, (int)name.len, name.data, cfg->codes[i]);
	}
}

/* Returns the definition of the code tool for @p service, a struct code.
 * The repo argument accepts the names of the repositories of --code.
 * Returns nullptr if out of memory.
 */
static struct json_object *
code_definition (void const *service)
{
	struct code const *code = service;
	struct config const *cfg = code->cfg;
	struct json_object *repos = json_object_new_array();
	for (size_t i = 0; repos && i < cfg->n_codes; ++i)
		repos = jb_push_or_free(repos, jb_string(git_run_name(cfg->code_repos[i])));
	static struct cstr_view const required[] = {
		CSTR_VIEW_INIT("repo"), CSTR_VIEW_INIT("action"),
	};
	return jb_object(
		"name", json_object_new_string("code"),
		"title", json_object_new_string("C code"),
		"description", json_object_new_string(
			"Ask clang about the C code in a commit of a repository of the server. "
			"clang parses the committed file in path with the flags from the compile "
			"commands of the repository. outline lists the names declared in path. "
			"Each outline row starts with the line number of the name. describe "
			"accepts the line number as the line argument. describe prints the kind, "
			"the type, and the places of the declaration and the definition of name "
			"in the translation unit of path. describe also lists the value of an "
			"enumerator, and the members of a struct or union with offsets, sizes and "
			"padding. Padding inside a member of a union is listed and not counted. "
			"For an enum, describe lists the enumerators with the line and "
			"the value. For an enumerator of an enum without a name, describe also "
			"lists the enumerators of the enum. For name or an enumerator that a "
			"macro expansion writes, describe prints the line where the macro is "
			"used, the macro, and the lines of the definition of the macro. For a "
			"function, describe lists the calls of the function: the lines of the "
			"calls, the called function, and the place where the called function is "
			"declared. For a call that is written through a macro, describe adds the "
			"macro. For an indirect call, describe prints the member, variable or "
			"parameter of the function pointer. For a variable with the attribute "
			"cleanup, describe lists the function that the program calls at the end "
			"of the scope of the variable. describe does not list a call in code that "
			"the program does not evaluate, such as an operand of sizeof. A closing "
			"line contains the number of the calls that are not listed. describe "
			"lists a call in code that the program cannot reach. describe prints up "
			"to 40 numbered lines of the declaration, without the body of a function. "
			"A finding can cite the numbered lines, such as c7:120-140. callers lists "
			"the calls of the function name in every translation unit of the compile "
			"commands. callers works without path. Each function of that name is a "
			"callee, printed with the file and the lines of the definition, and "
			"with [symbol NAME] if a declaration gives the function another name "
			"for the linker. With "
			"path, the callee is the function that the translation unit of path "
			"declares. A row under a callee is the file and the lines of the calls, "
			"the calling function, and the lines of the calling function. A row ends "
			"with [macro NAME] for calls that are written through a macro, with [or "
			"another function] for a call of one of several functions, such as "
			"(c ? f : g)(x), and with [maybe not evaluated] for a call in code that "
			"the program maybe does not evaluate. For a call "
			"through a _Generic selection, describe and callers list the selected "
			"function. callers does not list a use of the function that is not a "
			"call, such as the address of the function in an initializer. name can "
			"also be a function-like macro: an object-like macro is not found. The "
			"calls of a macro are the places where "
			"a file of the repository writes the macro with arguments: in a function, "
			"in the definition of another macro, or at file scope. callers does not "
			"list a macro name that is an argument of another macro. A name that is "
			"a function and a macro is two callees. With path, it is the function. "
			"The closing "
			"rows of callers name the files that clang could not parse or reported "
			"errors for. Calls in these files can be missing. diagnostics "
			"lists the errors and warnings of clang for path. A place is path:line. "
			"To read the code at a place, call git read with \"revisions\": "
			"[\"<commit>:<path>\"] and \"line\": <line>. Only committed files are "
			"visible. A .c file must be in the compile commands. A header is parsed "
			"with the flags of the nearest .c file."),
		"inputSchema", schema_object(
			jb_object(
				"repo", schema_enum(repos, nullptr),
				"action", schema_enum(schema_names(code_actions,
				                                   array_size(code_actions)),
				                      nullptr),
				"path", schema_property("string",
					"A .c or .h file of the commit, relative to the "
					"repository's top, such as src/git.c. Only callers "
					"works without path."),
				"name", schema_property("string",
					"The symbol for describe, such as git_start or struct "
					"dstr. Give a member as TYPE.MEMBER, such as "
					"child.flags. struct, union or enum before a name "
					"selects the type. For callers, the name of a function "
					"or of a function-like macro."),
				"line", schema_count(1,
					"For describe, the line of path that contains name. "
					"Give line for a local variable, a parameter or a macro "
					"use. For a macro use, describe also prints the type and "
					"the value of the expression."),
				"warnings", schema_array(schema_property("string", nullptr),
					CXI_WARNINGS,
					"Warning options that diagnostics adds to the flags of "
					"the build, such as -Wpadded or -Wno-sign-compare."),
				"revision", schema_property("string",
					"The commit, such as HEAD, a branch, a tag or a commit "
					"ID. ^ and ~ suffixes are allowed, as in HEAD~1. The "
					"default is HEAD."),
				"offset", schema_count(0, TOOL_OFFSET_ABOUT),
				nullptr),
			required, array_size(required)),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(1),
			"openWorldHint", json_object_new_boolean(0),
			nullptr),
		nullptr);
}

/* Appends the word KEY=VALUE, with the name of @p key and @p value, to
 * @p words, the words of a request to askai-code, unless value.data is
 * nullptr. Returns 0 or ENOMEM.
 */
static int
code_word (struct dstr      *words,
           enum cxi_key      key,
           struct cstr_view  value)
{
	return !value.data || dstr_printf(words, "%s=%.*s%c", code_keys[key].data,
	                                  (int)value.len, value.data, '\0') ? 0 : ENOMEM;
}

/* Writes the word KEY=N, with the name of @p key and @p n in decimal, and
 * a NUL at the end of @p word. Returns the word.
 */
static char const *
code_number (char         word[static CODE_NUMBER],
             enum cxi_key key,
             uint64_t     n)
{
	char *const end = word + CODE_NUMBER - 1;
	*end = '\0';
	char *at = end - util_decimal(end, n, 0).len - 1;
	*at = '=';
	at -= code_keys[key].len;
	memcpy(at, code_keys[key].data, code_keys[key].len);
	return at;
}

/* Frees @p build, which nothing refers to. */
static void
code_build_free (struct code_build *build)
{
	dstr_cleanup(&build->words);
	dstr_cleanup(&build->id);
	build_fini(&build->b);
	free(build);
}

/* Lets go of a reference to @p build, and frees the build if no reference
 * is left. The caller has the lock of the service.
 */
static void
code_build_put (struct code_build *build)
{
	if (!--build->refs)
		code_build_free(build);
}

/* Makes the unit of @p parse, whose process ended without a result, wait
 * for a parse again: for its last one after CODE_AGAIN. The caller has the
 * lock of the service.
 */
static void
code_build_undo (struct code_build       *build,
                 struct code_parse const *parse)
{
	build->b.items[parse->unit].state = flag_any(parse->flags, CODE_AGAIN) ? BUILD_RETRY
	                                                                       : BUILD_NONE;
	if (parse->unit < build->next)
		build->next = parse->unit;
}

static void
code_parse_done (struct child *c);

/* Starts askai-code for the unit @p unit of @p build, which waits for a
 * parse, in @p parse, a place of a loop without a process. The unit is then
 * BUILD_PARSING, and the place has a reference to the build. The process
 * gets the clang command line of the unit, and the time of a git of the
 * git tool. The caller has the lock of the service. Returns 0, ENOMEM, or
 * the error of child_create().
 */
static int
code_parse_spawn (struct code_parse *parse,
                  struct code_build *build,
                  size_t             unit)
{
	struct code_loop const *cl = parse->cl;
	struct code *code = cl->code;
	struct config const *cfg = code->cfg;
	struct build_unit *u = &build->b.items[unit];
	struct cstr_view const command = build_words(&build->b, u);
	uint64_t const deadline = cl->loop->now + cfg->git_timeout;
	char number[2][CODE_NUMBER];
	/* The helper, the two words with a number, at most a word for each
	 * other key, "--", the words of the command, and a nullptr. */
	size_t const fixed = 3 + CXI_KEYS;
	char const **argv = malloc((fixed + 1 + cstr_view_count(command, '\0') + 1) *
	                           sizeof *argv);
	if (!argv)
		return ENOMEM;

	size_t k = 0;
	argv[k++] = cfg->code_helper;
	argv[k++] = code_number(number[0], CXI_KEY_UNIT, unit);
	argv[k++] = code_number(number[1], CXI_KEY_DEADLINE, deadline);
	for (char const *w = dstr_get(&build->words), *last = w + dstr_len(&build->words);
	     w < last && k < fixed; w += strlen(w) + 1)
		argv[k++] = w;
	argv[k++] = "--";
	for (char const *w = command.data, *last = w + command.len; w < last;
	     w += strlen(w) + 1)
		argv[k++] = w;
	argv[k] = nullptr;
	char *env[] = { util_env_path(), nullptr };
	struct child_cmd const cmd = child_cmd(
		.file     = cfg->code_helper,
		.argv     = argv,
		.envp     = env,
		.max      = CODE_GRAPH,
		.deadline = deadline,
	);
	int e = 0;
	parse->child = child_create(cl->loop, cl->children, &cmd, code_parse_done,
	                            parse, &e);
	free(argv);
	if (e)
		return e;

	parse->build = build;
	parse->unit = unit;
	parse->flags = flag64(u->state == BUILD_RETRY ? CODE_AGAIN : 0);
	u->state = BUILD_PARSING;
	++build->parsing;
	++build->refs;
	++code->parses;
	return 0;
}

/* Lets go of the process of @p parse, which is killed if it runs. The
 * place is free then. The caller has the lock of the service, and lets go
 * of the reference of the place to the build (code_build_put()).
 */
static void
code_parse_end (struct code_parse *parse)
{
	child_destroy(&parse->child);
	--parse->build->parsing;
	--parse->cl->code->parses;
}

/* Starts a process of @p cl for each unit of @p build that waits for a
 * parse, in the order of the units, while a place of the loop is free and
 * fewer than CODE_PARSES processes of all loops parse (build_waits()). A
 * unit whose command line is too long for a process (E2BIG) is not parsed:
 * at most CODE_PARSES such units end a call of this function, so that the
 * loop is not kept for long. After another error of the start, as when the
 * loops have CHILD_MAX children, the unit waits, and a later call of this
 * function starts it. A stalled build starts
 * none. While a call of another build waits for a place (code_waited()),
 * this build starts none either, so that a build of many units does not
 * keep each place that its processes free. The caller has the lock of the
 * service.
 */
static void
code_build_run (struct code_build *build,
                struct code_loop  *cl)
{
	struct code *code = cl->code;
	struct build *b = &build->b;
	if (flag_any(build->flags, CODE_STALLED) || (code->hungry && code->hungry != build))
		return;

	for (size_t failed = 0; failed < CODE_PARSES;) {
		while (build->next < b->n && !build_waits(b->items[build->next].state))
			++build->next;
		size_t i = 0;
		while (i < CODE_PARSES && cl->parse[i].child)
			++i;
		if (build->next == b->n || i == CODE_PARSES ||
		    code->parses == CODE_PARSES)
			break;

		int const e = code_parse_spawn(&cl->parse[i], build, build->next);
		if (e == E2BIG) {
			build_parsed(b, build->next, nullptr);
			--build->left;
			++failed;
		} else if (e) {
			break;
		} else if (code->hungry == build) {
			/* The build has got a place. */
			code->hungry = nullptr;
		}
	}
	if (build->next == b->n && code->hungry == build)
		code->hungry = nullptr;
}

/* Ends the parse of a unit: @p c is the process, which has ended. The
 * output of a process that exited with status 0 is the graph of the unit.
 * After CXI_STALE the unit waits for a parse again and the build is
 * stalled. A unit whose process took too long for the first time is
 * BUILD_AGAIN: a later call has it parsed once more (code_join()). A
 * process that ended in another way could not parse the
 * unit. The loop then starts its next processes for the build. If no unit
 * is left, or no process parses, the step of each call of the loop that
 * waits for a build is queued (code_waited()).
 */
static void
code_parse_done (struct child *c)
{
	struct code_parse *parse = c->owner;
	struct code_loop *cl = parse->cl;
	struct code *code = cl->code;
	struct code_build *build = parse->build;
	int const status = child_status(c);
	bool const late = flag_any(c->end.flags, CHILD_LATE);
	pthread_mutex_lock(&code->lock);
	if (status == CXI_STALE) {
		flag_set(&build->flags, CODE_STALLED);
		code_build_undo(build, parse);
	} else if (late && !flag_any(parse->flags, CODE_AGAIN)) {
		build->b.items[parse->unit].state = BUILD_AGAIN;
		++build->again;
	} else {
		build_parsed(&build->b, parse->unit, status ? nullptr : &c->out);
		size_t const bytes = buf_peek(&build->b.items[parse->unit].g.bytes).len;
		build->bytes += bytes;
		code->bytes += bytes;
		--build->left;
	}
	code_parse_end(parse);
	code_build_run(build, cl);
	bool const wake = !build->left || !build->parsing;
	code_build_put(build);
	pthread_mutex_unlock(&code->lock);
	if (!wake)
		return;

	for (struct list *at = cl->waiting.next; at != &cl->waiting; at = at->next)
		(void)mcp_slot_ready(container_of(at, struct code_call, wait)->slot);
}

/* Creates the build that askai-code names @p id in its answer to callers
 * for @p call, as the last build of @p code, with the reference of the
 * service. @p units is the rest of the answer: for each translation unit
 * the path of its file, a NUL, the number of the bytes of its clang command
 * line, a NUL, and the words of the command line, each followed by a NUL.
 * No unit is parsed. The caller has the lock of the service. Returns the
 * build, or nullptr with ENOMEM, EOVERFLOW, or EINVAL for units that are
 * not such units to the end, or for none, in @p *e.
 */
static struct code_build *
code_build_create (struct code            *code,
                   struct cstr_view        id,
                   struct cstr_view        units,
                   struct code_call const *call,
                   int                    *e)
{
	struct code_build *made = malloc(sizeof *made);
	if (!made) {
		*e = ENOMEM;
		return nullptr;
	}
	*made = (struct code_build){
		.hook  = list(&made->hook),
		.b     = build(),
		.id    = DSTR_INIT,
		.words = DSTR_INIT,
		.refs  = 1,
	};
	struct config const *cfg = code->cfg;
	struct git_pin const *pin = &call->pin;
	struct dstr *words = &made->words;
	int err = units.len ? cstr_view_store(id, &made->id) : EINVAL;
	if (!err)
		err = code_word(words, CXI_KEY_REPO, pin->repo->name);
	if (!err)
		err = code_word(words, CXI_KEY_COMMIT, oid_view(&pin->roots[0]));
	if (!err)
		err = code_word(words, CXI_KEY_CACHE, cstr_view(cfg->code_cache));
	if (!err)
		err = code_word(words, CXI_KEY_UNCONFINED, flag_any(cfg->flags, CFG_UNCONFINED)
		                ? CSTR_VIEW("1") : cstr_view(nullptr));
	while (!err && units.len) {
		struct cstr_view path;
		struct cstr_view size;
		uint64_t n = 0;
		/* The command is n bytes of the rest, and ends with a NUL. */
		err = !cstr_view_cut(&units, '\0', &path) || !path.len ||
		      !cstr_view_cut(&units, '\0', &size) ||
		      util_number(size.data, size.len, units.len, &n) ||
		      !n || units.data[n - 1]
		      ? EINVAL : build_add(&made->b, path, cstr_view_n(units.data, n));
		units = cstr_view_slice(units, n, SIZE_MAX);
	}
	*e = err;
	if (err) {
		code_build_free(made);
		return nullptr;
	}
	made->left = made->b.n;
	list_append(&code->builds, &made->hook);
	return made;
}

/* Returns the build of @p code that askai-code names @p id, or nullptr.
 * The caller has the lock of the service.
 */
static struct code_build *
code_find (struct code      *code,
           struct cstr_view  id)
{
	for (struct list *at = code->builds.next; at != &code->builds; at = at->next) {
		struct code_build *build = container_of(at, struct code_build, hook);
		if (cstr_view_equal(cstr_view_dstr(&build->id), id))
			return build;
	}
	return nullptr;
}

/* Frees builds of @p code that only the service refers to, the one that a
 * call used longest ago first, while the graphs of the builds of @p code
 * are more than CODE_KEEP bytes. The caller has the lock of the service.
 */
static void
code_trim (struct code *code)
{
	struct list *at = code->builds.next;
	while (code->bytes > CODE_KEEP && at != &code->builds) {
		struct code_build *build = container_of(at, struct code_build, hook);
		at = at->next;
		if (build->refs > 1)
			continue;

		list_del(&build->hook);
		code->bytes -= build->bytes;
		code_build_put(build);
	}
}

/* Lets @p call, a call of a loop of @p code, leave its build, if it has
 * one. The build then no longer waits for a place for that call.
 */
static void
code_leave (struct code_call *call,
            struct code      *code)
{
	struct code_build *build = call->build;
	list_del(&call->wait);
	call->build = nullptr;
	if (!build)
		return;

	pthread_mutex_lock(&code->lock);
	if (code->hungry == build)
		code->hungry = nullptr;
	code_build_put(build);
	pthread_mutex_unlock(&code->lock);
}

/* Frees the call of @p slot, which kills its child process if it runs, and
 * sets the call pointer of the slot (mcp_slot_own()) to nullptr. A call
 * that waits for a build leaves the build, and the build stays.
 */
static void
code_drop (struct mcp_slot *slot)
{
	struct code_loop const *cl = mcp_slot_part(slot);
	struct code_call *call = *mcp_slot_own(slot);
	(void)loop_timer_stop(mcp_slot_loop(slot), &call->timer);
	code_leave(call, cl->code);
	cxi_out_fini(&call->answer);
	git_pin_fini(&call->pin);
	dstr_cleanup(&call->named);
	dstr_cleanup(&call->words);
	free(call);
	*mcp_slot_own(slot) = nullptr;
}

/* Spawns askai-code for @p call, whose pin is done, as the child process
 * of the call in @p slot, and parks the call. Returns 0, ENOMEM, or the
 * error of mcp_slot_spawn().
 */
static int
code_spawn (struct code_call *call,
            struct mcp_slot  *slot)
{
	struct code_loop const *cl = mcp_slot_part(slot);
	struct config const *cfg = cl->code->cfg;
	struct git_pin *pin = &call->pin;
	int e = 0;
	/* A call that exports its commit once more has these words. */
	if (!flag_any(call->flags, CODE_WORDED)) {
		char digits[UTIL_DECIMAL];
		e = code_word(&call->words, CXI_KEY_COMMIT, oid_view(&pin->roots[0]));
		if (!e)
			e = code_word(&call->words, CXI_KEY_DEADLINE,
			              util_decimal(digits + sizeof digits, pin->deadline, 0));
		if (!e)
			e = code_word(&call->words, CXI_KEY_TOP, cstr_view(pin->repo->path));
		flag_set(&call->flags, CODE_WORDED);
	}
	if (e)
		return e;
	/* The helper, at most a word for each key but CXI_KEY_WARNING and for
	 * each warning, "--", then the argv of git. list=1 is no word of the
	 * call: only a call that needs the units has it. */
	char const *argv[GIT_RUN_ARGV];
	static_assert(1 + CXI_KEYS - 1 + CXI_WARNINGS + 1 < array_size(argv));
	size_t k = 0;
	argv[k++] = cfg->code_helper;
	for (char const *w = dstr_get(&call->words), *last = w + dstr_len(&call->words); w < last;
	     w += strlen(w) + 1)
		argv[k++] = w;
	if (flag_any(call->flags, CODE_LIST))
		argv[k++] = "list=1";
	argv[k++] = "--";
	struct git_run_dir dir = {};
	git_pin_dir(pin, &dir);
	e = git_run_dir_argv(&dir, nullptr, argv + k, array_size(argv) - k);
	if (e)
		return e;
	struct child_cmd const cmd = child_cmd(
		.file = cfg->code_helper,
		.argv = argv,
		.envp = dir.env,
		/* The output can contain the record line of describe in addition
		 * to a result of TOOL_TEXT_MAX bytes. The answer to callers is
		 * the list of the translation units. */
		.max = flag_any(call->flags, CODE_CALLERS) ? CODE_UNITS : TOOL_TEXT_MAX + 4096,
		.deadline = pin->deadline
	);
	pin->child = mcp_slot_spawn(slot, &cmd, &e);
	return e;
}

/* Parses the record line "lines FIRST LAST TOTAL AT LEN PATH" at the start
 * of @p *text, the output of describe, into @p src, and removes the line
 * from @p *text. Sets src->repo to the name of pin->repo and src->commit
 * to pin->roots[0]. Returns false if @p *text does not start with a valid
 * record line.
 */
static bool
code_record (struct cstr_view     *text,
             struct git_pin const *pin,
             struct seen_source   *src)
{
	struct cstr_view rest = *text;
	struct cstr_view line = {};
	if (!cstr_view_starts(rest, CSTR_VIEW("lines ")) || !cstr_view_cut(&rest, '\n', &line))
		return false;
	line = cstr_view_slice(line, sizeof "lines " - 1, SIZE_MAX);
	uint64_t v[5];
	for (size_t i = 0; i < array_size(v); ++i) {
		size_t const n = util_digits(line.data, line.len, UINT32_MAX, &v[i]);
		if (!n || n == line.len || line.data[n] != ' ')
			return false;
		line = cstr_view_slice(line, n + 1, SIZE_MAX);
	}
	*text = rest;
	*src = (struct seen_source){
		.repo = pin->repo->name, .commit = pin->roots[0],
		.path = line, .total = v[2], .output = { v[3], v[4] },
		.lines = { (uint32_t)v[0], (uint32_t)v[1] },
	};
	return true;
}

/* Reads the answer of askai-code to callers for @p call, the output of the
 * child of the pin: the name of the functions, a NUL, a letter and the path
 * of a file, a NUL, a name for the linker, a NUL, the name of the build,
 * a NUL, and with list=1 the translation units (code_build_create()). The
 * letter is A for each
 * function and macro of the name, or for the one that one translation unit
 * declares M if it is a macro, S if it is a static function and E if not:
 * the file is then that of the definition of a macro, that of the first
 * declaration of a static function, and that of the definition of another
 * function, or empty. The name for the linker is that of the function
 * of E, and else empty. The call gets a reference to the build of that
 * name and joins the waiting calls of @p cl, and the output of askai-code
 * is freed. Returns 0, ENOMEM, EOVERFLOW, EINVAL if the output is no such
 * answer, or ENOENT if the service does not keep the build and the answer
 * does not list the units.
 */
static int
code_join (struct code_call *call,
           struct code_loop *cl)
{
	struct cstr_view rest = cstr_view_span(buf_peek(&call->pin.child->out));
	struct build_callee *callee = &call->callee;
	struct cstr_view name;
	struct cstr_view file;
	struct cstr_view link;
	struct cstr_view id;
	if (!cstr_view_cut(&rest, '\0', &name) ||
	    !cstr_view_cut(&rest, '\0', &file) ||
	    !cstr_view_cut(&rest, '\0', &link) ||
	    !cstr_view_cut(&rest, '\0', &id) ||
	    !file.len || !memchr("AESM", *file.data, 4))
		return EINVAL;

	char const letter = *file.data;
	file = cstr_view_slice(file, 1, SIZE_MAX);
	int e = cstr_view_store(name, &call->named);
	if (!e)
		e = cstr_view_cat(file, &call->named);
	if (!e)
		e = cstr_view_cat(link, &call->named);
	if (e)
		return e;
	callee->name = cstr_view_n(dstr_get(&call->named), name.len);
	callee->file = cstr_view_n(callee->name.data + name.len, file.len);
	callee->link = cstr_view_n(callee->file.data + file.len, link.len);
	callee->mask = letter == 'A' ? 0 : GRAPH_FN_STATIC | GRAPH_FN_MACRO;
	callee->flags = letter == 'S' ? GRAPH_FN_STATIC : letter == 'M' ? GRAPH_FN_MACRO : 0;

	struct code *code = cl->code;
	pthread_mutex_lock(&code->lock);
	struct code_build *build = code_find(code, id);
	if (!build)
		build = flag_any(call->flags, CODE_LIST)
		        ? code_build_create(code, id, rest, call, &e) : nullptr;
	if (build) {
		/* The build that a call used last is the last one of the list.
		 * A call that got the units has exported the commit. A unit
		 * that took too long is parsed once more for this call. */
		list_move(&code->builds, &build->hook);
		++build->refs;
		if (flag_any(call->flags, CODE_LIST))
			flag_clear(&build->flags, (uint64_t)CODE_STALLED);
		for (size_t i = 0; build->again && i < build->b.n; ++i) {
			if (build->b.items[i].state != BUILD_AGAIN)
				continue;
			build->b.items[i].state = BUILD_RETRY;
			--build->again;
			build->next = i < build->next ? i : build->next;
		}
		code_trim(code);
	}
	pthread_mutex_unlock(&code->lock);
	if (!build)
		return e ? e : ENOENT;

	flag_clear(&call->flags, (uint64_t)CODE_LIST);
	child_destroy(&call->pin.child);
	call->build = build;
	list_append(&cl->waiting, &call->wait);
	return 0;
}

/* Appends the answer of callers from the build of @p call to the answer
 * of the call: the repository, the commit, and the rows of
 * build_callers(). The caller has the lock of the service, unless no unit
 * of the build is left.
 */
static void
code_text (struct code_call *call)
{
	struct cxi_out *out = &call->answer;
	cxi_out_lit(out, "- repo: ");
	cxi_out_view(out, call->pin.repo->name);
	cxi_out_lit(out, "\n- commit: ");
	cxi_out_view(out, oid_view(&call->pin.roots[0]));
	cxi_out_lit(out, "\n");
	build_callers(&call->build->b, &call->callee, out);
}

/* Ends the call in @p slot with the answer of @p call (code_text()). */
static void
code_answer (struct mcp_slot  *slot,
             struct code_call *call)
{
	struct cxi_out *out = &call->answer;
	if (out->e) {
		mcp_slot_fail(slot, (int)out->e);
		return;
	}
	struct cstr_view const text = cxi_out_answer(out);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, text.len);
	mcp_slot_lend(slot, text);
	mcp_slot_finish(slot, 0);
}

/* Queues the step of the call of the timer @p timer, a call that waits
 * for a build.
 */
static void
code_call_late (struct loop *,
                struct loop_timer *timer)
{
	(void)mcp_slot_ready(container_of(timer, struct code_call, timer)->slot);
}

/* Runs on the call in @p slot, which waits for the build of @p call. The
 * call ends with its answer once each unit of the build is parsed or has
 * failed, or at its deadline: the answer then counts the units that are
 * left. Before that, a call whose build is stalled without a process
 * leaves the build and spawns askai-code once more, which exports the
 * commit again. Any other call lets its loop start processes for the
 * build, and looks at the build again after CODE_RETRY, or when a process
 * of its loop has ended (code_parse_done()). If no process parses for the
 * build after that, and a unit waits for a place for a process, the build
 * gets the next place that is free.
 */
static void
code_waited (struct mcp_slot  *slot,
             struct code_call *call)
{
	struct code_loop *cl = mcp_slot_part(slot);
	struct code *code = cl->code;
	struct code_build *build = call->build;
	struct loop *loop = mcp_slot_loop(slot);
	uint64_t const deadline = call->pin.deadline;
	pthread_mutex_lock(&code->lock);
	bool const complete = !build->left;
	if (complete || loop->now >= deadline) {
		/* No unit of a complete build changes, so its graphs are read
		 * without the lock. The units of a build that is parsed on are
		 * read with it. */
		if (complete)
			pthread_mutex_unlock(&code->lock);
		code_text(call);
		if (!complete)
			pthread_mutex_unlock(&code->lock);
		/* The answer needs the build no more. A call whose result askai
		 * keeps for a while would otherwise keep the build, and a place
		 * that the build waits for. */
		(void)loop_timer_stop(loop, &call->timer);
		code_leave(call, code);
		code_answer(slot, call);
		return;
	}
	bool const stalled = flag_any(build->flags, CODE_STALLED);
	if (!stalled) {
		code_build_run(build, cl);
		/* Only a build without a process waits for a place: one that
		 * parses gets its next place when a process of its own ends. */
		if (!code->hungry && !build->parsing && build->next < build->b.n)
			code->hungry = build;
	}
	bool const idle = !build->parsing;
	pthread_mutex_unlock(&code->lock);

	int e = 0;
	if (stalled && idle) {
		(void)loop_timer_stop(loop, &call->timer);
		code_leave(call, code);
		flag_set(&call->flags, CODE_LIST);
		e = code_spawn(call, slot);
	} else {
		uint64_t const again = loop->now + CODE_RETRY;
		e = loop_timer_set(loop, &call->timer, again < deadline ? again : deadline);
	}
	if (e)
		git_step_failed(slot, e, cstr_view(nullptr), "askai-code");
}

/* Ends the call in @p slot after askai-code has exited. With exit status
 * 0, the output is the result, and the numbered lines that describe
 * printed go to the ring of the calls. The output of callers names a
 * build, which gives the result (code_join()). With exit status 1, the
 * output is a tool error. Otherwise the call fails with the reason.
 */
static void
code_ended (struct mcp_slot  *slot,
            struct code_call *call)
{
	struct code_loop *cl = mcp_slot_part(slot);
	struct child *c = call->pin.child;
	int const status = child_status(c);
	if (status != 0 && status != 1) {
		struct tool_scratch *scratch = mcp_slot_scratch(slot);
		uint64_t const timeout = cl->code->cfg->git_timeout;
		mcp_slot_tell(slot, child_end_why(&c->end, "askai-code", timeout, c->max,
		                                  scratch->text, sizeof scratch->text),
		              TOOL_FAILED);
		return;
	}
	if (!status && flag_any(call->flags, CODE_CALLERS)) {
		int e = code_join(call, cl);
		if (e == ENOENT) {
			/* The service does not keep the build of the call. */
			flag_set(&call->flags, CODE_LIST);
			child_destroy(&call->pin.child);
			e = code_spawn(call, slot);
			if (e)
				git_step_failed(slot, e, cstr_view(nullptr), "askai-code");
			return;
		}
		if (e == EINVAL)
			mcp_slot_tell(slot, CSTR_VIEW("askai-code wrote no answer to "
			                              "callers that askai can read."),
			              TOOL_FAILED);
		else if (e)
			mcp_slot_fail(slot, e);
		else
			code_waited(slot, call);
		return;
	}
	struct span const out = buf_peek(&c->out);
	/* askai-code writes valid UTF-8. jb_mend() replaces invalid bytes in
	 * case the helper is broken. */
	struct cstr_view text = cstr_view_n((char const *)out.ptr,
	                                    jb_mend((char *)out.ptr, out.len, true));
	loop_work(mcp_slot_loop(slot), LOOP_COPY, out.len);
	struct seen_source src;
	bool const record = !status && code_record(&text, &call->pin, &src);
	mcp_slot_lend(slot, text);
	/* mcp_slot_source() returns EINVAL for a record line that does not
	 * match the output. The record is then not added, and the call still
	 * succeeds. */
	if (record && mcp_slot_source(slot, &src) == ENOMEM)
		mcp_slot_fail(slot, ENOMEM);
	else
		mcp_slot_finish(slot, status ? TOOL_FAILED : 0);
}

/* Runs the call in @p slot on: after the exit of a git of the pin, or at
 * the start, spawns the next git of the pin, or askai-code once the pin is
 * done. After askai-code, ends the call, or for callers runs the call on
 * until the build has the answer (code_waited()). A call that ends is
 * dropped by askai (code_drop()).
 */
static void
code_step (struct mcp_slot *slot)
{
	struct code_call *call = *mcp_slot_own(slot);
	struct git_pin *pin = &call->pin;
	if (call->build) {
		code_waited(slot, call);
		return;
	}
	if (pin->phase == GIT_PIN_DONE) {
		code_ended(slot, call);
		return;
	}
	struct cstr_view why = {};
	int e = git_pin_step(pin, slot, &why);
	if (!e && !pin->child)
		e = code_spawn(call, slot);
	if (e)
		git_step_failed(slot, e, why, pin->phase < GIT_PIN_DONE ? "git" : "askai-code");
}

/* Checks the types of the arguments @p args of @p call, the call of
 * @p code in @p slot. askai-code checks the values of the arguments. The
 * function adds the revision to the pin of the call and appends the words
 * of the request to askai-code. Returns 0 or ENOMEM, or EINVAL after it has
 * ended the call with a text that says which argument is wrong.
 */
static int
code_args (struct code const  *code,
           struct code_call   *call,
           struct json_object *args,
           struct mcp_slot    *slot)
{
	struct json_object *of[CODE_ARGS] = {};
	uint64_t seen;
	struct cstr_view other = {};
	if (jb_args(args, code_arg_names, CODE_ARGS, UINT64_MAX, of, &seen, &other))
		return mcp_slot_tellf(slot, TOOL_FAILED, "%.*s is not an argument of the code "
		                      "tool.", (int)other.len, other.data), EINVAL;
	struct config const *cfg = code->cfg;
	struct cstr_view const repo = jb_text(of[CODE_ARG_REPO], 1, CODE_WORD, nullptr);
	size_t r = repo.data ? 0 : cfg->n_codes;
	while (r < cfg->n_codes && !cstr_view_equal(git_run_name(cfg->code_repos[r]), repo))
		++r;
	if (r == cfg->n_codes) {
		/* The names are appended one by one, so the sentence is built in
		 * the scratch text of the loop. */
		struct tool_scratch *scratch = mcp_slot_scratch(slot);
		char *const why = scratch->text;
		uint32_t len = 0;
		util_text_printf_(&len, why, sizeof scratch->text, "The repo argument must be ");
		for (size_t i = 0; i < cfg->n_codes; ++i) {
			struct cstr_view const name = git_run_name(cfg->code_repos[i]);
			util_text_printf_(&len, why, sizeof scratch->text, "%s%.*s",
			                  !i ? "" : i + 1 < cfg->n_codes ? ", " : " or ",
			                  (int)name.len, name.data);
		}
		util_text_printf_(&len, why, sizeof scratch->text, ".");
		return mcp_slot_tell(slot, cstr_view_n(why, len), TOOL_FAILED), EINVAL;
	}
	struct cstr_view const action = jb_text(of[CODE_ARG_ACTION], 1, CODE_WORD, nullptr);
	struct cstr_view const path = jb_text(of[CODE_ARG_PATH], 1, CODE_WORD, nullptr);
	struct cstr_view const name = jb_text(of[CODE_ARG_NAME], 1, CODE_WORD, nullptr);
	struct cstr_view const revision = of[CODE_ARG_REVISION]
	                                  ? jb_text(of[CODE_ARG_REVISION], 1, CODE_WORD, nullptr)
	                                  : CSTR_VIEW("HEAD");
	char const *bad = !action.data ? "action" : of[CODE_ARG_PATH] && !path.data ? "path"
	                : of[CODE_ARG_NAME] && !name.data ? "name"
	                : !revision.data ? "revision" : nullptr;
	static_assert(CODE_WORD == 1024 && CXI_WARNINGS == 16, "update the texts of the limits");
	if (bad)
		return mcp_slot_tellf(slot, TOOL_FAILED, "The %s argument must be a string of 1 to "
		                      "1024 bytes without a NUL.", bad), EINVAL;
	uint64_t const line = jb_count(of[CODE_ARG_LINE], UINT32_MAX);
	if (of[CODE_ARG_LINE] && !line)
		return mcp_slot_tell(slot, CSTR_VIEW("The line argument must be a whole number "
		                                     "from 1."), TOOL_FAILED), EINVAL;
	struct json_object *warnings = of[CODE_ARG_WARNINGS];
	size_t const n = jb_len(warnings);
	size_t i = 0;
	while (i < n && jb_text(jb_at(warnings, i), 1, CODE_WORD, nullptr).data)
		++i;
	if (warnings && (!json_object_is_type(warnings, json_type_array) || n > CXI_WARNINGS ||
	                 i < n))
		return mcp_slot_tell(slot, CSTR_VIEW("The warnings argument must be an array of at "
		                                     "most 16 strings of 1 to 1024 bytes without a "
		                                     "NUL."), TOOL_FAILED), EINVAL;
	int e = git_pin_add(&call->pin, revision);
	if (e == EINVAL)
		return mcp_slot_tell(slot, CSTR_VIEW("The revision argument must be a commit name "
		                                     "or hash, with optional ^ and ~ suffixes, such "
		                                     "as HEAD~1."), TOOL_FAILED), EINVAL;
	call->repo = r;
	if (cstr_view_equal(action, code_actions[CXI_ACTION_CALLERS]))
		flag_set(&call->flags, CODE_CALLERS);
	struct dstr *words = &call->words;
	char digits[UTIL_DECIMAL];
	if (!e)
		e = code_word(words, CXI_KEY_ACTION, action);
	if (!e)
		e = code_word(words, CXI_KEY_REPO, repo);
	if (!e)
		e = code_word(words, CXI_KEY_PATH, path);
	if (!e)
		e = code_word(words, CXI_KEY_NAME, name);
	if (!e)
		e = code_word(words, CXI_KEY_LINE,
		              line ? util_decimal(digits + sizeof digits, line, 0)
		                   : cstr_view(nullptr));
	for (i = 0; !e && i < n; ++i)
		e = code_word(words, CXI_KEY_WARNING, jb_view(jb_at(warnings, i)));
	if (!e)
		e = code_word(words, CXI_KEY_CDB, cstr_view(cfg->codes[r]));
	if (!e)
		e = code_word(words, CXI_KEY_CACHE, cstr_view(cfg->code_cache));
	return e ? e : code_word(words, CXI_KEY_UNCONFINED, flag_any(cfg->flags, CFG_UNCONFINED)
	                         ? CSTR_VIEW("1") : cstr_view(nullptr));
}

/* Starts a call of the code tool in @p slot with the arguments @p args.
 * The call runs the git processes of the pin of its revision and then
 * askai-code, each as a child process on the loop of the call.
 */
static void
code_start (struct mcp_slot    *slot,
            struct json_object *args)
{
	struct code_loop const *cl = mcp_slot_part(slot);
	struct code const *code = cl->code;
	struct code_call *call = malloc(sizeof *call);
	if (!call) {
		mcp_slot_fail(slot, ENOMEM);
		return;
	}
	*call = (struct code_call){
		.words  = DSTR_INIT,
		.named  = DSTR_INIT,
		.answer = cxi_out(cstr_view(nullptr)),
		.wait   = list(&call->wait),
		.timer  = { .fn = code_call_late },
		.slot   = slot
	};
	git_pin_init(&call->pin, code->git, jb_view(jb_get(args, "repo")),
	             mcp_slot_loop(slot)->now + code->cfg->git_timeout);
	*mcp_slot_own(slot) = call;
	int const e = code_args(code, call, args, slot);
	/* After EINVAL, code_args() has ended the call. */
	if (e == EINVAL)
		return;
	if (e)
		mcp_slot_fail(slot, e);
	else if (!mcp_slot_offset(slot, args))
		code_step(slot);
}

void *
code_loop_create (void                *service,
                  struct loop         *loop,
                  struct blocker      *,
                  struct list         *children,
                  struct tool_scratch *,
                  int                 *e)
{
	struct code_loop *cl = malloc(sizeof *cl);
	if (!cl) {
		*e = ENOMEM;
		return nullptr;
	}
	*cl = (struct code_loop){
		.waiting  = list(&cl->waiting),
		.children = children,
		.loop     = loop,
		.code     = service
	};
	for (size_t i = 0; i < CODE_PARSES; ++i)
		cl->parse[i].cl = cl;
	*e = 0;
	return cl;
}

void
code_loop_destroy (void **p_part)
{
	struct code_loop *cl = *p_part;
	if (!cl)
		return;
	*p_part = nullptr;
	struct code *code = cl->code;
	pthread_mutex_lock(&code->lock);
	for (size_t i = 0; i < CODE_PARSES; ++i) {
		struct code_parse *parse = &cl->parse[i];
		if (!parse->child)
			continue;

		code_parse_end(parse);
		code_build_undo(parse->build, parse);
		code_build_put(parse->build);
	}
	pthread_mutex_unlock(&code->lock);
	free(cl);
}

int
code_init (struct code         *dest,
           struct git const    *git,
           struct config const *cfg)
{
	*dest = (struct code){
		.git    = git,
		.cfg    = cfg,
		.builds = list(&dest->builds)
	};
	return pthread_mutex_init(&dest->lock, nullptr);
}

void
code_fini (struct code *code)
{
	while (!list_is_empty(&code->builds)) {
		struct code_build *build = container_of(code->builds.next,
		                                        struct code_build, hook);
		list_del(&build->hook);
		code_build_free(build);
	}
	pthread_mutex_destroy(&code->lock);
}

struct plugin const code_plugin = {
	.tools = {{
		.definition = code_definition,
		.start = code_start,
		.step = code_step,
		.drop = code_drop,
		.flags = TOOL_LENT | TOOL_PAGED | TOOL_DETACH
	}}
};
