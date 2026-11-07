/** @file code.c
 *
 * The code tool.
 */
#define _GNU_SOURCE /* asprintf() */
#include <errno.h>
#include <json-c/json.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "child.h"
#include "code.h"
#include "config.h"
#include "cstr_view.h"
#include "cxi.h"
#include "dstr.h"
#include "git.h"
#include "jb.h"
#include "log.h"
#include "loop.h"
#include "mcp.h"
#include "plugin.h"
#include "review.h"
#include "schema.h"
#include "util.h"
#include "xlist.h"

enum {
	CODE_WORD  = 1024,  /* the maximum length of a string argument */
	CODE_CHECK = 4096,  /* the maximum length of the --check output */
	CODE_WHY   = 1024,  /* the size of the error text of a call */
};

/* The arguments: X(ID, name). */
#define CODE_ARG_LIST(X)                                                   \
        X(REPO, "repo") X(ACTION, "action") X(PATH, "path") X(NAME, "name") \
        X(LINE, "line") X(WARNINGS, "warnings") X(REVISION, "revision")      \
        X(OFFSET, "offset")

#define CODE_ARG_ENUM(id, ...) CODE_ARG_##id,
enum code_arg : uint8_t { CODE_ARG_LIST(CODE_ARG_ENUM) CODE_ARGS };
#undef CODE_ARG_ENUM

static struct cstr_view const code_arg_names[] = { CODE_ARG_LIST(XL_VIEW) };

/* A call of the code tool. pin resolves the revision, and pin.child is the
 * askai-code process once the pin is done. words contains the KEY=VALUE
 * words of the request to askai-code, each followed by a NUL.
 */
struct code_call {
	struct git_pin pin;
	struct dstr    words;
};

/* The error text of a call with invalid arguments. */
struct code_why {
	UTIL_TEXT(CODE_WHY);
};

/* Sets @p *str to a new string that @p fmt and its arguments format, as
 * printf() does, or to nullptr if out of memory. Returns 0 or ENOMEM.
 */
[[gnu::format(printf, 2, 3)]]
static int
code_aprintf (char       **str,
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

int
code_check (struct config *cfg)
{
	char *exe = realpath("/proc/self/exe", nullptr);
	if (!exe)
		return code_aprintf(&cfg->code_check, "askai's executable is not found: %s",
		                    strerror(errno));
	char const *xdg = getenv("XDG_CACHE_HOME");
	char const *home = getenv("HOME");
	char const *top = xdg && *xdg == '/' ? xdg : home && *home == '/' ? home : nullptr;
	int e = code_aprintf(&cfg->code_helper, "%s/askai-code", dirname(exe));
	free(exe);
	exe = nullptr;
	if (!e && !top)
		return code_aprintf(&cfg->code_check, "no cache directory: XDG_CACHE_HOME and HOME "
		                    "are not absolute");
	char *cache = nullptr;
	e = e ? e : code_aprintf(&cfg->code_cache, "%s%s/askai/code", top, top == xdg ? ""
	                                                                              : "/.cache");
	e = e ? e : code_aprintf(&cache, "cache=%s", cfg->code_cache);
	if (e)
		return e;
	char const *argv[] = {
		cfg->code_helper, "--check", cache,
		cfg->flags & CFG_UNCONFINED ? "unconfined=1" : nullptr, nullptr
	};
	char *env[] = { util_env_path(), nullptr };
	struct child_cmd const cmd = {
		.file = cfg->code_helper,
		.argv = argv,
		.envp = env,
		.deadline = loop_clock() + 10 * NS,
		.flags = CHILD_STDERR,
	};
	char out[CODE_CHECK];
	size_t len = 0;
	int status = 0;
	e = child_capture(&cmd, child_fds(), out, sizeof out, &len, &status);
	free(cache);
	cache = nullptr;
	if (e == ENOENT)
		return code_aprintf(&cfg->code_check, "no askai-code beside askai");
	if (e)
		return code_aprintf(&cfg->code_check, "askai-code --check failed: %s", strerror(e));
	/* After a successful check the output is the version of libclang and
	 * the confinement, one line each. Otherwise its first line is the
	 * reason why the tool is off. */
	char const *nl = memchr(out, '\n', len);
	size_t const first = nl ? (size_t)(nl - out) : len;
	if (status || first + 1 >= len)
		return first ? code_aprintf(&cfg->code_check, "%.*s", (int)first, out)
		             : code_aprintf(&cfg->code_check, "askai-code --check exited with "
		                            "status %d", status);
	cfg->flags |= CFG_CODE;
	return code_aprintf(&cfg->code_check, "%.*s", (int)(len - (out[len - 1] == '\n')), out);
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
		int const len = (int)git_name(cfg->code_repos[i]).len;
		width = len > width ? len : width;
	}
	LOG_ALL(LOG_INFO, "code repositories:");
	for (size_t i = 0; i < cfg->n_codes; ++i) {
		struct cstr_view const name = git_name(cfg->code_repos[i]);
		LOG_ALL(LOG_INFO, "  %-*.*s  %s", width, (int)name.len, name.data,
		        strchr(cfg->codes[i], '=') + 1);
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
		repos = jb_push_or_free(repos, jb_string(git_name(cfg->code_repos[i])));
	static struct cstr_view const actions[] = { CXI_ACTION_LIST(XL_VIEW) };
	static struct cstr_view const required[] = {
		CSTR_VIEW_INIT("repo"), CSTR_VIEW_INIT("action"), CSTR_VIEW_INIT("path"),
	};
	return jb_object(
		"name", json_object_new_string("code"),
		"title", json_object_new_string("C code"),
		"description", json_object_new_string(
			"Ask clang about the C code of a commit of one of the server's "
			"repositories. clang parses the file in path, as committed, with the "
			"flags of the repository's compile commands, so the answers come from "
			"the compiler and not from a text search. describe prints what name is "
			"in the translation unit of path: the type of a variable, the signature "
			"and calls of a function, the value of an enumerator, or a macro and its "
			"value where line uses it. It prints the places where name is declared "
			"and defined, and the numbered lines of its declaration, which count as "
			"read for findings. For a struct or union it lists the members with "
			"their offsets, sizes and padding, inside anonymous members too. outline "
			"lists the names that path declares, each with the line where it is "
			"written, which can be passed to describe as line. diagnostics lists "
			"clang's errors and warnings for path. Places are path:line; git read "
			"accepts the commit and the path. Only committed files are visible. A .c "
			"file must be in the compile commands; a header is parsed with the flags "
			"of the .c file nearest to it."),
		"inputSchema", schema_object(
			jb_object(
				"repo", schema_enum(repos, nullptr),
				"action", schema_enum(schema_names(actions, array_size(actions)),
				                      nullptr),
				"path", schema_property("string",
					"A .c or .h file of the commit, relative to the "
					"repository's top, such as src/git.c."),
				"name", schema_property("string",
					"For describe: a function, variable, type, struct, union, "
					"enum, enumerator, typedef or macro, such as git_start, or a "
					"member as struct.member, such as child.flags. struct, union "
					"or enum before a name picks the type, as in struct dstr."),
				"line", schema_count(1,
					"For describe: the line of path where name is written, such "
					"as the line of a local variable or of a macro use."),
				"warnings", schema_array(schema_property("string", nullptr),
					CXI_WARNINGS,
					"For diagnostics: warning options to add to the build's, "
					"such as -Wpadded or -Wsign-conversion."),
				"revision", schema_property("string",
					"The commit, in a form that the git tool accepts: HEAD "
					"(the default), a branch, a tag or a commit ID, with ^ "
					"and ~ as in HEAD~1."),
				"offset", schema_count(0, TOOL_OFFSET_ABOUT),
				nullptr),
			required, array_size(required)),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(1),
			"openWorldHint", json_object_new_boolean(0),
			nullptr),
		nullptr);
}

/* Frees the call of @p slot, which kills its child process if it runs, and
 * sets the call pointer of the slot (mcp_slot_own()) to nullptr.
 */
static void
code_drop (struct mcp_slot *slot)
{
	struct code_call *call = *mcp_slot_own(slot);
	git_pin_fini(&call->pin);
	dstr_cleanup(&call->words);
	free(call);
	*mcp_slot_own(slot) = nullptr;
}

/* Appends the word KEY=VALUE, with the name of @p key and @p value, to the
 * words of the request of @p call to askai-code, unless value.data is
 * nullptr. Returns 0 or ENOMEM.
 */
static int
code_word (struct code_call *call,
           enum cxi_key      key,
           struct cstr_view  value)
{
	static struct cstr_view const keys[] = { CXI_KEY_LIST(XL_VIEW) };
	return !value.data || dstr_printf(&call->words, "%s=%.*s%c", keys[key].data,
	                                  (int)value.len, value.data, '\0') ? 0 : ENOMEM;
}

/* Spawns askai-code for @p call, whose pin is done, as the child process
 * of the call in @p slot, and parks the call. Returns 0, ENOMEM, or the
 * error of mcp_slot_spawn().
 */
static int
code_spawn (struct code_call *call,
            struct mcp_slot  *slot)
{
	struct code const *code = mcp_slot_part(slot);
	struct git_pin *pin = &call->pin;
	char digits[UTIL_DECIMAL];
	char *const end = digits + sizeof digits;
	char const *deadline = util_decimal(end, pin->deadline);
	int e = code_word(call, CXI_KEY_COMMIT, oid_view(&pin->roots[0]));
	e = e ? e : code_word(call, CXI_KEY_DEADLINE, (struct cstr_view){
		deadline, (size_t)(end - deadline) });
	e = e ? e : code_word(call, CXI_KEY_TOP, cstr_view(pin->repo->path));
	if (e)
		return e;
	char const *argv[GIT_ARGV];
	char *env[GIT_ENV];
	size_t k = 0;
	argv[k++] = code->cfg->code_helper;
	for (char const *w = dstr_get(&call->words), *last = w + dstr_len(&call->words); w < last;
	     w += strlen(w) + 1)
		argv[k++] = w;
	argv[k++] = "--";
	k += git_pin_argv(pin, argv + k, env);
	argv[k] = nullptr;
	struct child_cmd const cmd = {
		.file = code->cfg->code_helper,
		.argv = argv,
		.envp = env,
		/* The output can contain the record line of describe in addition
		 * to a result of TOOL_TEXT_MAX bytes. */
		.max = TOOL_TEXT_MAX + 4096,
		.deadline = pin->deadline,
	};
	pin->child = mcp_slot_spawn(slot, &cmd, &e);
	if (!e)
		mcp_slot_park(slot);
	return e;
}

/* Parses the record line "lines FIRST LAST TOTAL AT LEN PATH" at the start
 * of @p *text, the output of describe, into @p src, and removes the line
 * from @p *text. Returns false if @p *text does not start with a valid
 * record line.
 */
static bool
code_record (struct cstr_view      *text,
             struct review_source  *src)
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
	*src = (struct review_source){
		.path = line, .total = v[2], .at = v[3], .len = v[4],
		.first = (uint32_t)v[0], .last = (uint32_t)v[1],
	};
	return true;
}

/* Ends the call in @p slot after askai-code has exited. With exit status
 * 0, the output is the result, and the numbered lines that describe
 * printed are added to the review record. With exit status 1, the output
 * is a tool error. Otherwise the call fails with the reason.
 */
static void
code_ended (struct mcp_slot  *slot,
            struct code_call *call)
{
	struct code const *code = mcp_slot_part(slot);
	struct child *c = call->pin.child;
	int const status = child_status(c);
	char buf[CODE_WHY];
	if (status != 0 && status != 1) {
		mcp_slot_tell(slot, (c->flags & (CHILD_CUT | CHILD_LATE)) == CHILD_CUT
		                    ? CSTR_VIEW("askai-code wrote more than 4 MiB.")
		                    : child_why(c, "askai-code", code->cfg->git_timeout, buf,
		                                sizeof buf), TOOL_FAILED);
		return;
	}
	struct span const out = buf_peek(&c->out);
	/* askai-code writes valid UTF-8. jb_mend() replaces invalid bytes in
	 * case the helper is broken. */
	struct cstr_view text = { (char const *)out.ptr, jb_mend((char *)out.ptr, out.len, true) };
	loop_work(mcp_slot_loop(slot), LOOP_COPY, out.len);
	struct review_source src;
	bool const record = !status && code_record(&text, &src);
	mcp_slot_lend(slot, text);
	src.repo = call->pin.repo->name;
	src.commit = oid_view(&call->pin.roots[0]);
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
 * done. After askai-code, ends the call. A call that ends is dropped by
 * askai (code_drop()).
 */
static void
code_step (struct mcp_slot *slot)
{
	struct code_call *call = *mcp_slot_own(slot);
	struct git_pin *pin = &call->pin;
	if (pin->phase == GIT_PIN_DONE) {
		code_ended(slot, call);
		return;
	}
	struct cstr_view why = {};
	int e = git_pin_step(pin, slot, &why);
	if (!e && !pin->child)
		e = code_spawn(call, slot);
	char buf[CODE_WHY];
	if (e == ENOMEM)
		mcp_slot_fail(slot, e);
	else if (e)
		mcp_slot_tell(slot, why.data ? why : child_spawn_why(e, pin->phase < GIT_PIN_DONE
		                                                     ? "git" : "askai-code", buf,
		                                                     sizeof buf), TOOL_FAILED);
}

/* Returns a view of the string @p val if it is 1 to CODE_WORD bytes long
 * without a NUL, or else an empty view with data nullptr.
 */
static struct cstr_view
code_string (struct json_object *val)
{
	struct cstr_view const s = jb_view(val);
	return s.len && s.len <= CODE_WORD && !memchr(s.data, '\0', s.len) ? s : (struct cstr_view){};
}

/* Checks the types of the arguments @p args of @p call, a call of @p code,
 * whose values askai-code checks. Adds the revision to the pin of the call
 * and appends the words of the request to askai-code. Returns 0, EINVAL
 * with the error text in @p why, or ENOMEM.
 */
static int
code_args (struct code const  *code,
           struct code_call   *call,
           struct json_object *args,
           struct code_why    *why)
{
	struct json_object *of[CODE_ARGS] = {};
	uint64_t seen;
	struct cstr_view other;
	if (jb_args(args, code_arg_names, CODE_ARGS, UINT64_MAX, of, &seen, &other))
		return util_text_printf(why, "The tool has no argument %.*s.", (int)other.len,
		                        other.data), EINVAL;
	struct config const *cfg = code->cfg;
	struct cstr_view const repo = code_string(of[CODE_ARG_REPO]);
	size_t r = repo.data ? 0 : cfg->n_codes;
	while (r < cfg->n_codes && !cstr_view_equal(git_name(cfg->code_repos[r]), repo))
		++r;
	if (r == cfg->n_codes) {
		util_text_printf(why, "The repo argument must be ");
		for (size_t i = 0; i < cfg->n_codes; ++i) {
			struct cstr_view const name = git_name(cfg->code_repos[i]);
			util_text_printf(why, "%s%.*s", !i ? "" : i + 1 < cfg->n_codes ? ", " : " or ",
			                 (int)name.len, name.data);
		}
		return util_text_printf(why, "."), EINVAL;
	}
	struct cstr_view const action = code_string(of[CODE_ARG_ACTION]);
	struct cstr_view const path = code_string(of[CODE_ARG_PATH]);
	struct cstr_view const name = code_string(of[CODE_ARG_NAME]);
	struct cstr_view const revision = of[CODE_ARG_REVISION] ? code_string(of[CODE_ARG_REVISION])
	                                                        : CSTR_VIEW("HEAD");
	char const *bad = !action.data ? "action" : !path.data ? "path"
	                : of[CODE_ARG_NAME] && !name.data ? "name"
	                : !revision.data ? "revision" : nullptr;
	static_assert(CODE_WORD == 1024 && CXI_WARNINGS == 16, "update the texts of the limits");
	if (bad)
		return util_text_printf(why, "The %s argument must be a string of 1 to 1024 bytes "
		                        "without a NUL.", bad), EINVAL;
	uint64_t const line = jb_count(of[CODE_ARG_LINE], UINT32_MAX);
	if (of[CODE_ARG_LINE] && !line)
		return util_text_printf(why, "The line argument must be a whole number from 1."),
		       EINVAL;
	struct json_object *warnings = of[CODE_ARG_WARNINGS];
	size_t const n = jb_len(warnings);
	size_t i = 0;
	while (i < n && code_string(jb_at(warnings, i)).data)
		++i;
	if (warnings && (!json_object_is_type(warnings, json_type_array) || n > CXI_WARNINGS ||
	                 i < n))
		return util_text_printf(why, "The warnings argument must be an array of at most 16 "
		                        "strings of 1 to 1024 bytes without a NUL."), EINVAL;
	int e = git_pin_add(&call->pin, revision);
	if (e == EINVAL)
		return util_text_printf(why, "The revision argument must be a commit reference or "
		                        "hash with ^ or ~, such as HEAD~1."), EINVAL;
	char digits[UTIL_DECIMAL];
	char *const end = digits + sizeof digits;
	char const *number = util_decimal(end, line);
	e = e ? e : code_word(call, CXI_KEY_ACTION, action);
	e = e ? e : code_word(call, CXI_KEY_REPO, repo);
	e = e ? e : code_word(call, CXI_KEY_PATH, path);
	e = e ? e : code_word(call, CXI_KEY_NAME, name);
	e = e ? e : code_word(call, CXI_KEY_LINE, (struct cstr_view){ line ? number : nullptr,
	                                                              (size_t)(end - number) });
	for (i = 0; !e && i < n; ++i)
		e = code_word(call, CXI_KEY_WARNING, jb_view(jb_at(warnings, i)));
	e = e ? e : code_word(call, CXI_KEY_CDB, cstr_view(strchr(cfg->codes[r], '=') + 1));
	e = e ? e : code_word(call, CXI_KEY_CACHE, cstr_view(cfg->code_cache));
	return e ? e : code_word(call, CXI_KEY_UNCONFINED, cfg->flags & CFG_UNCONFINED
	                         ? CSTR_VIEW("1") : (struct cstr_view){});
}

/* Starts a call of the code tool in @p slot with the arguments @p args.
 * The call runs the git processes of the pin of its revision and then
 * askai-code, each as a child process on the loop of the call.
 */
static void
code_start (struct mcp_slot    *slot,
            struct json_object *args)
{
	struct code const *code = mcp_slot_part(slot);
	struct code_call *call = malloc(sizeof *call);
	if (!call) {
		mcp_slot_fail(slot, ENOMEM);
		return;
	}
	call->words = (struct dstr)DSTR_INIT;
	git_pin_init(&call->pin, code->git, jb_view(jb_get(args, "repo")),
	             mcp_slot_loop(slot)->now + code->cfg->git_timeout);
	*mcp_slot_own(slot) = call;
	struct code_why why = {};
	int e = code_args(code, call, args, &why);
	if (e == EINVAL)
		mcp_slot_tell(slot, util_text_view(&why), TOOL_FAILED);
	else if (e)
		mcp_slot_fail(slot, e);
	else if (!mcp_slot_offset(slot, args))
		code_step(slot);
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
