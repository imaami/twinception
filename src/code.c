/** @file code.c
 *
 * The code tool.
 */
#include <errno.h>
#include <json-c/json.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "child.h"
#include "code.h"
#include "config.h"
#include "cstr_view.h"
#include "cxi.h"
#include "dstr.h"
#include "git.h"
#include "git_run.h"
#include "jb.h"
#include "log.h"
#include "loop.h"
#include "mcp.h"
#include "plugin.h"
#include "schema.h"
#include "seen.h"
#include "util.h"
#include "xlist.h"

enum {
	CODE_WORD  = 1024,  /* the maximum length of a string argument */
	CODE_CHECK = 4096,  /* the maximum length of the --check output */
};

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

/* A call of the code tool. pin resolves the revision, and pin.child is the
 * askai-code process once the pin is done. words contains the KEY=VALUE
 * words of the request to askai-code, each followed by a NUL.
 */
struct code_call {
	struct git_pin pin;
	struct dstr    words;
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
	e = e ? e : config_aprintf(&cfg->code_cache, "%s%s/askai/code", top,
	                           top == xdg ? "" : "/.cache");
	e = e ? e : config_aprintf(&cache, "cache=%s", cfg->code_cache);
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
		.max = CODE_CHECK,
		.deadline = loop_clock() + 10 * NS,
		.flags = CHILD_STDERR,
	};
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
		cfg->flags |= CFG_CODE;
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
	static struct cstr_view const actions[] = { CXI_ACTION_LIST(XL_VIEW) };
	static struct cstr_view const required[] = {
		CSTR_VIEW_INIT("repo"), CSTR_VIEW_INIT("action"), CSTR_VIEW_INIT("path"),
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
			"in the translation unit of path. describe also lists the calls of a "
			"function, the value of an enumerator, and the members of a struct or "
			"union with offsets, sizes and padding. describe prints up to 40 "
			"numbered lines of the declaration, without the body of a function. A "
			"finding can cite the numbered lines, such as c7:120-140. diagnostics "
			"lists the errors and warnings of clang for path. A place is path:line. "
			"To read the code at a place, call git read with \"revisions\": "
			"[\"<commit>:<path>\"] and \"line\": <line>. Only committed files are "
			"visible. A .c file must be in the compile commands. A header is parsed "
			"with the flags of the nearest .c file."),
		"inputSchema", schema_object(
			jb_object(
				"repo", schema_enum(repos, nullptr),
				"action", schema_enum(schema_names(actions, array_size(actions)),
				                      nullptr),
				"path", schema_property("string",
					"A .c or .h file of the commit, relative to the "
					"repository's top, such as src/git.c."),
				"name", schema_property("string",
					"The symbol for describe, such as git_start or struct "
					"dstr. Give a member as TYPE.MEMBER, such as "
					"child.flags. struct, union or enum before a name "
					"selects the type."),
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
	int e = code_word(call, CXI_KEY_COMMIT, oid_view(&pin->roots[0]));
	e = e ? e : code_word(call, CXI_KEY_DEADLINE,
	                      util_decimal(digits + sizeof digits, pin->deadline, 0));
	e = e ? e : code_word(call, CXI_KEY_TOP, cstr_view(pin->repo->path));
	if (e)
		return e;
	/* The helper, at most a word for each key but CXI_KEY_WARNING and for
	 * each warning, "--", then the argv of git. */
	char const *argv[GIT_RUN_ARGV];
	static_assert(1 + CXI_KEYS - 1 + CXI_WARNINGS + 1 < array_size(argv));
	size_t k = 0;
	argv[k++] = code->cfg->code_helper;
	for (char const *w = dstr_get(&call->words), *last = w + dstr_len(&call->words); w < last;
	     w += strlen(w) + 1)
		argv[k++] = w;
	argv[k++] = "--";
	struct git_run_dir dir;
	git_pin_dir(pin, &dir);
	e = git_run_dir_argv(&dir, nullptr, argv + k, array_size(argv) - k);
	if (e)
		return e;
	struct child_cmd const cmd = {
		.file = code->cfg->code_helper,
		.argv = argv,
		.envp = dir.env,
		/* The output can contain the record line of describe in addition
		 * to a result of TOOL_TEXT_MAX bytes. */
		.max = TOOL_TEXT_MAX + 4096,
		.deadline = pin->deadline,
	};
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

/* Ends the call in @p slot after askai-code has exited. With exit status
 * 0, the output is the result, and the numbered lines that describe
 * printed go to the ring of the calls. With exit status 1, the output
 * is a tool error. Otherwise the call fails with the reason.
 */
static void
code_ended (struct mcp_slot  *slot,
            struct code_call *call)
{
	struct code const *code = mcp_slot_part(slot);
	struct child *c = call->pin.child;
	int const status = child_status(c);
	if (status != 0 && status != 1) {
		struct tool_scratch *scratch = mcp_slot_scratch(slot);
		mcp_slot_tell(slot, child_end_why(&c->end, "askai-code", code->cfg->git_timeout,
		                                  c->max, scratch->text, sizeof scratch->text),
		              TOOL_FAILED);
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
	char const *bad = !action.data ? "action" : !path.data ? "path"
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
	char digits[UTIL_DECIMAL];
	e = e ? e : code_word(call, CXI_KEY_ACTION, action);
	e = e ? e : code_word(call, CXI_KEY_REPO, repo);
	e = e ? e : code_word(call, CXI_KEY_PATH, path);
	e = e ? e : code_word(call, CXI_KEY_NAME, name);
	e = e ? e : code_word(call, CXI_KEY_LINE,
	                      line ? util_decimal(digits + sizeof digits, line, 0)
	                           : cstr_view(nullptr));
	for (i = 0; !e && i < n; ++i)
		e = code_word(call, CXI_KEY_WARNING, jb_view(jb_at(warnings, i)));
	e = e ? e : code_word(call, CXI_KEY_CDB, cstr_view(cfg->codes[r]));
	e = e ? e : code_word(call, CXI_KEY_CACHE, cstr_view(cfg->code_cache));
	return e ? e : code_word(call, CXI_KEY_UNCONFINED, cfg->flags & CFG_UNCONFINED
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
	struct code const *code = mcp_slot_part(slot);
	struct code_call *call = malloc(sizeof *call);
	if (!call) {
		mcp_slot_fail(slot, ENOMEM);
		return;
	}
	*call = (struct code_call){ .words = DSTR_INIT };
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

struct plugin const code_plugin = {
	.tools = {{
		.definition = code_definition,
		.start = code_start,
		.step = code_step,
		.drop = code_drop,
		.flags = TOOL_LENT | TOOL_PAGED | TOOL_DETACH
	}}
};
