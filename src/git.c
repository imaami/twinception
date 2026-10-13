/** @file The git tool (git.h). */
#include <errno.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "child.h"
#include "git.h"
#include "jb.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "mcp.h"
#include "plugin.h"
#include "util.h"

enum {
	GIT_MAX   = 2 << 20,  /**< output bytes, at most */
	GIT_ARGV  = 128,      /**< a command's argv, its nullptr included */
	GIT_WHY   = 256,      /**< bytes of what is wrong with a call */
	GIT_NOTE  = 96,       /**< bytes ending the output, at most: a blank line, a note */
	OPTIONS   = 16,       /**< a call's options, at most */
	REVISIONS = 4,        /**< its revisions */
	PATHS     = 32,       /**< its paths */
	WORD      = 1024,     /**< bytes of an option, a revision, a path or a pattern */
	ENV       = 12,       /**< git's environment, its nullptr included */
};

/** From SIGTERM to SIGKILL at a git call's deadline. */
static constexpr uint64_t GIT_GRACE_NS = 1'000'000'000;

static_assert(GIT_MAX == 2 << 20 && GIT_GRACE_NS == 1'000'000'000, "git.h says");

/* A repository: its name, its path, and the setting that has git trust it,
 * one string of the three. */
struct git_repo {
	char *name;
	char *path;
	char *safe;
};

struct git {
	char           *env[ENV];
	uint64_t        timeout;
	size_t          n;
	struct git_repo repo[];
};

/* What a client loop has of the git tool, its part of it: the children of
 * the calls there, the git, and the loop. */
struct git_loop {
	struct list       children;
	struct git const *git;
	struct loop      *loop;
};

/* What git runs with whatever the call: no pager, nor a program that a
 * repository's configuration names for the fsmonitor, gpg (which a log's
 * format may ask for), hooks, or a submodule's diff (which a superproject's
 * show or log -p runs as a git of its own, without the options below); no
 * remote protocol (the environment's GIT_ALLOW_PROTOCOL, which a
 * repository's protocol.<name>.allow cannot override, stops them all); no
 * index refreshed by a work tree's diff; and options that writes would have
 * (gc.auto, maintenance.auto) off. */
static char const *const fixed[] = {
	"--no-pager",
	"-c", "core.fsmonitor=false",
	"-c", "log.showSignature=false",
	"-c", "gpg.program=false",
	"-c", "gpg.ssh.program=false",
	"-c", "gpg.x509.program=false",
	"-c", "core.hooksPath=/dev/null",
	"-c", "protocol.allow=never",
	"-c", "diff.submodule=short",
	"-c", "diff.autoRefreshIndex=false",
	"-c", "gc.auto=0",
	"-c", "maintenance.auto=false",
};

/*
 * An option a command takes, as it is, or, ending in "=", with a value after
 * it; in "#", with digits attached; in "*", with any value attached.
 */
struct opt {
	char const *str;
	size_t      len;
};

#define O(s) { s, sizeof s - 1 }
#define DISPLAY O("--oneline"), O("--stat"), O("--shortstat"), O("--numstat"), \
	O("--name-only"), O("--name-status"), O("--patch"), O("-p"), O("--no-patch"), \
	O("--abbrev-commit"), O("--format="), O("--pretty="), O("--date="), \
	O("--unified="), O("-U#"), O("--word-diff"), O("-w"), O("--ignore-all-space"), \
	O("-b"), O("--ignore-space-change"), O("--decorate"), O("--no-decorate"), \
	O("--diff-filter="), O("-M"), O("--find-renames")
static struct opt const log_options[] = {
	DISPLAY, O("--graph"), O("--reverse"), O("--first-parent"), O("--no-merges"),
	O("--merges"), O("--all"), O("--follow"), O("--max-count="), O("-n#"),
	O("--skip="), O("--since="), O("--until="), O("--after="), O("--before="),
	O("--author="), O("--committer="), O("--grep="), O("-i"),
	O("--regexp-ignore-case"), O("--all-match"), O("--invert-grep"), O("-E"),
	O("--extended-regexp"), O("-F"), O("--fixed-strings"), O("-S*"), O("-G*"), {}
};
static struct opt const show_options[] = { DISPLAY, O("-s"), {} };
static struct opt const diff_options[] = {
	DISPLAY, O("--cached"), O("--staged"), {}
};
#undef DISPLAY
static struct opt const grep_options[] = {
	O("-n"), O("--line-number"), O("-i"), O("--ignore-case"), O("-w"),
	O("--word-regexp"), O("-v"), O("--invert-match"), O("-l"),
	O("--files-with-matches"), O("-L"), O("--files-without-match"), O("-c"),
	O("--count"), O("-E"), O("--extended-regexp"), O("-F"), O("--fixed-strings"),
	O("-P"), O("--perl-regexp"), O("-G"), O("--basic-regexp"), O("--cached"),
	O("--untracked"), O("-h"), O("-H"), O("--full-name"), O("-o"),
	O("--only-matching"), O("-C#"), O("-A#"), O("-B#"), O("--context="),
	O("--after-context="), O("--before-context="), O("--max-depth="),
	O("--max-count="), O("-m#"), O("--break"), O("--heading"), O("-p"),
	O("--show-function"), O("-W"), O("--function-context"), O("--all-match"), {}
};
static struct opt const blame_options[] = {
	O("-w"), O("-M"), O("-C"), O("-s"), O("-e"), O("-l"), O("-t"), O("-f"),
	O("--show-name"), O("--porcelain"), O("--line-porcelain"), O("--date="),
	O("-L*"), O("--root"), O("--ignore-rev="), {}
};
static struct opt const ls_files_options[] = {
	O("--cached"), O("-c"), O("--others"), O("-o"), O("--exclude-standard"),
	O("--stage"), O("-s"), O("--deleted"), O("-d"), O("--modified"), O("-m"),
	O("--full-name"), O("--abbrev"), O("--abbrev="), O("--directory"),
	O("--no-empty-directory"), O("--eol"), O("--format="), O("--deduplicate"), {}
};
static struct opt const ls_tree_options[] = {
	O("-r"), O("-t"), O("-d"), O("--name-only"), O("--name-status"),
	O("--object-only"), O("--long"), O("-l"), O("--full-name"), O("--full-tree"),
	O("--abbrev"), O("--abbrev="), O("--format="), {}
};
static struct opt const cat_file_options[] = {
	O("-t"), O("-s"), O("-p"), O("-e"), {}
};
static struct opt const rev_parse_options[] = {
	O("--abbrev-ref"), O("--abbrev-ref="), O("--short"), O("--short="),
	O("--verify"), O("--symbolic"), O("--symbolic-full-name"), O("-q"),
	O("--quiet"), O("--is-inside-work-tree"), O("--is-bare-repository"),
	O("--is-shallow-repository"), {}
};
#undef O

/* Where a command takes what. */
enum {
	REVS    = 1 << 0,  /* revisions */
	FILES   = 1 << 1,  /* paths, after "--" */
	END     = 1 << 2,  /* --end-of-options before its revisions, which those
	                      that take it amiss lack: blame, which takes "--"
	                      for a revision then; ls-files and ls-tree, which
	                      take "--" for a path; rev-parse, which echoes it */
	PATTERN = 1 << 3,  /* a pattern, which it needs, after -e */
};

static struct {
	char const       *name;
	struct opt const *options;
	char const       *forced[2];  /* options askai gives it */
	uint64_t          takes;
} const commands[] = {
	{ "log",       log_options,       { "--no-ext-diff", "--no-textconv" },
	  REVS | FILES | END },
	{ "show",      show_options,      { "--no-ext-diff", "--no-textconv" },
	  REVS | FILES | END },
	{ "diff",      diff_options,      { "--no-ext-diff", "--no-textconv" },
	  REVS | FILES | END },
	{ "grep",      grep_options,      { "--no-textconv" },
	  REVS | FILES | END | PATTERN },
	{ "blame",     blame_options,     { "--no-textconv" }, REVS | FILES },
	{ "ls-files",  ls_files_options,  {}, FILES },
	{ "ls-tree",   ls_tree_options,   {}, REVS | FILES },
	{ "cat-file",  cat_file_options,  {}, REVS | END },
	/* Revisions only: no other argument is looked at as a path. */
	{ "rev-parse", rev_parse_options, { "--revs-only" }, REVS },
};

static_assert(1 + ARRAY_SIZE(fixed) + 5 + 2 + OPTIONS + 2 + 1 + REVISIONS + 1 +
              PATHS + 1 <= GIT_ARGV, "a command fits argv");

char const *
git_name (char const *path,
          size_t     *len)
{
	static char const suffix[] = ".git";
	size_t end = strlen(path);
	while (end > 1 && path[end - 1] == '/')
		--end;
	size_t start = end;
	while (start && path[start - 1] != '/')
		--start;
	size_t n = end - start;
	if (n > sizeof suffix - 1 &&
	    !memcmp(path + end - (sizeof suffix - 1), suffix, sizeof suffix - 1))
		n -= sizeof suffix - 1;
	*len = 0;
	if (!n || n > 64 || path[start] == '.' ||
	    strspn(path + start, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	                         "0123456789_.-") < n)
		return nullptr;
	*len = n;
	return path + start;
}

/* Set @p r up for the repository listed as @p path: its name, its real
 * path, and its safe.directory setting, in one allocation. */
static int
git_repo_init (struct git_repo *r,
               char const      *path)
{
	static char const safe[] = "safe.directory=";
	size_t len = 0;
	char const *name = git_name(path, &len);
	if (!name)
		return EINVAL;
	char *real = realpath(path, nullptr);
	if (!real)
		return errno;
	size_t real_len = strlen(real);
	char *s = malloc(len + 1 + 2 * (real_len + 1) + sizeof safe - 1);
	if (!s) {
		free(real);
		return ENOMEM;
	}
	r->name = memcpy(s, name, len);
	r->name[len] = '\0';
	r->path = memcpy(s + len + 1, real, real_len + 1);
	r->safe = r->path + real_len + 1;
	memcpy(r->safe, safe, sizeof safe - 1);
	memcpy(r->safe + sizeof safe - 1, real, real_len + 1);
	free(real);
	return 0;
}

/* GIT_CEILING_DIRECTORIES: the listed repositories' parents, so that git
 * finds no repository above one listed, as it would look for one without
 * one there; nullptr if out of memory. */
static char *
git_ceilings (struct git const *git)
{
	static char const ceiling[] = "GIT_CEILING_DIRECTORIES=";
	size_t size = sizeof ceiling;
	/* Each path, and its NUL, goes before its safe.directory setting. */
	for (size_t i = 0; i < git->n; ++i)
		size += (size_t)(git->repo[i].safe - git->repo[i].path);
	char *env = malloc(size);
	if (!env)
		return nullptr;
	size_t at = sizeof ceiling - 1;
	memcpy(env, ceiling, at);
	for (size_t i = 0; i < git->n; ++i) {
		char const *p = git->repo[i].path;
		size_t len = (size_t)(strrchr(p, '/') - p);
		if (i)
			env[at++] = ':';
		memcpy(env + at, len ? p : "/", len ? len : 1);
		at += len ? len : 1;
	}
	env[at] = '\0';
	return env;
}

/* Set @p git up for the @p n repositories at @p paths. */
static int
git_load (struct git        *git,
          char const *const  paths[],
          size_t             n)
{
	static char const env_path[] = "PATH=";
	char const *path = getenv("PATH");
	char *env = util_concat(env_path, sizeof env_path - 1,
	                        path && *path ? path : "/usr/bin:/bin");
	if (!env)
		return ENOMEM;
	git->env[0] = env;
	for (; git->n < n; ++git->n) {
		int e = git_repo_init(&git->repo[git->n], paths[git->n]);
		if (e)
			return e;
	}
	char *ceilings = git_ceilings(git);
	if (!ceilings)
		return ENOMEM;
	char *const vars[ENV] = {
		env, ceilings, "HOME=/nonexistent", "LC_ALL=C.UTF-8",
		"GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null",
		"GIT_TERMINAL_PROMPT=0", "GIT_OPTIONAL_LOCKS=0", "GIT_PAGER=cat",
		"GIT_NO_LAZY_FETCH=1", "GIT_ALLOW_PROTOCOL=none", nullptr
	};
	static_assert(sizeof vars == sizeof git->env);
	memcpy(git->env, vars, sizeof vars);
	return 0;
}

struct git *
git_create (char const *const paths[],
            size_t             n,
            uint64_t           timeout,
            int               *e)
{
	struct git *git = calloc(1, sizeof *git + n * sizeof *git->repo);
	if (!git) {
		*e = ENOMEM;
		return nullptr;
	}
	git->timeout = timeout;
	if ((*e = git_load(git, paths, n)))
		git_destroy(&git);
	return git;
}

void
git_destroy (struct git **p_git)
{
	if (!p_git || !*p_git)
		return;
	struct git *git = *p_git;
	*p_git = nullptr;
	for (size_t i = 0; i < git->n; ++i)
		free(git->repo[i].name);
	free(git->env[0]);
	free(git->env[1]);
	free(git);
}

/* Append @p str to the array @p arr, which is let go of if that fails. */
static struct json_object *
git_append (struct json_object *arr,
            char const         *str)
{
	return arr && !jb_push(arr, json_object_new_string(str)) ? arr : nullptr;
}

/* The schema of an argument that is at most @p max strings. */
static struct json_object *
git_strings (char const *description,
             size_t      max)
{
	return jb_object(
		"type", json_object_new_string("array"),
		"items", jb_object("type", json_object_new_string("string"), nullptr),
		"maxItems", json_object_new_int((int32_t)max),
		"description", json_object_new_string(description),
		nullptr);
}

/* Returns the definition of the git tool.  @p service is the struct git,
 * and the definition lists the names of its repositories.  Returns nullptr
 * if out of memory. */
static struct json_object *
git_definition (void const *service)
{
	struct git const *git = service;
	static_assert(OPTIONS == 16 && REVISIONS == 4 && PATHS == 32 &&
	              GIT_MAX == 2 << 20, "the description says");
	struct json_object *repos = json_object_new_array();
	for (size_t i = 0; i < git->n; ++i)
		repos = git_append(repos, git->repo[i].name);
	struct json_object *cmds = json_object_new_array();
	for (size_t i = 0; i < ARRAY_SIZE(commands); ++i)
		cmds = git_append(cmds, commands[i].name);
	return jb_object(
		"name", json_object_new_string("git"),
		"title", json_object_new_string("Git"),
		"description", json_object_new_string(
			"Run a read-only git command in one of the server's repositories, "
			"and get its output, its errors included, cut at 2 MiB: log, show, "
			"diff, grep, blame, ls-files, ls-tree, cat-file or rev-parse. Give "
			"options with their values attached (--max-count=20, not -n 20); "
			"only the usual read-only ones are taken. Revisions and paths go "
			"apart, and grep's pattern too."),
		"inputSchema", jb_object(
			"type", json_object_new_string("object"),
			"properties", jb_object(
				"repo", jb_object(
					"type", json_object_new_string("string"),
					"enum", repos,
					nullptr),
				"command", jb_object(
					"type", json_object_new_string("string"),
					"enum", cmds,
					nullptr),
				"options", git_strings("Options, such as --oneline or "
				                       "--max-count=20.", OPTIONS),
				"revisions", git_strings("Revisions, such as HEAD~3, main..topic "
				                         "or a commit's hash.", REVISIONS),
				"paths", git_strings("Paths in the repository, relative to its "
				                     "top.", PATHS),
				"pattern", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"grep's pattern, and only grep's."),
					nullptr),
				"offset", jb_object(
					"type", json_object_new_string("integer"),
					"minimum", json_object_new_int(0),
					"description", json_object_new_string(
						"The byte of the output to start from, to read on "
						"past where it was cut."),
					nullptr),
				nullptr),
			"required", git_append(git_append(json_object_new_array(), "repo"),
			                       "command"),
			"additionalProperties", json_object_new_boolean(0),
			nullptr),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(1),
			"openWorldHint", json_object_new_boolean(0),
			nullptr),
		nullptr);
}

/* @p val's string, of 1 to WORD bytes and no NUL, and its length in @p *len;
 * nullptr if it is not one. */
static char const *
git_word (struct json_object *val,
          size_t             *len)
{
	char const *s = jb_text(val);
	*len = s ? (size_t)json_object_get_string_len(val) : 0;
	return s && *len && *len <= WORD && !memchr(s, '\0', *len) ? s : nullptr;
}

/* Whether @p opt, of @p len bytes, is one of @p allowed. */
static bool
git_allowed (char const       *opt,
             size_t            len,
             struct opt const *allowed)
{
	for (; allowed->str; ++allowed) {
		char const *a = allowed->str;
		size_t n = allowed->len;
		char last = a[n - 1];
		if (last != '=' && last != '#' && last != '*') {
			if (len == n && !memcmp(opt, a, n))
				return true;
			continue;
		}
		/* The value: after '=' its own, attached for the others. */
		size_t at = last == '=' ? n : n - 1;
		if (len <= at || memcmp(opt, a, at))
			continue;
		if (last != '#' || strspn(opt + at, "0123456789") == len - at)
			return true;
	}
	return false;
}

/* Whether @p path is relative, with no ".." component. */
static bool
git_inside (char const *path)
{
	if (*path == '/')
		return false;
	for (char const *p = path; p; p = strchr(p, '/')) {
		p += *p == '/';
		if (!strncmp(p, "..", 2) && (p[2] == '/' || !p[2]))
			return false;
	}
	return true;
}

/* Say in @p why, of GIT_WHY bytes, what is wrong with a call's arguments;
 * returns EINVAL. */
[[gnu::format(printf, 2, 3)]]
static int
git_wrong (char       *why,
           char const *fmt,
           ...)
{
	va_list ap;
	va_start(ap, fmt);
	util_vformat(why, GIT_WHY, fmt, ap);
	va_end(ap);
	return EINVAL;
}

/* Say that @p str, cut at @p max bytes but never inside a character, is
 * wrong, after @p head and before @p tail; returns EINVAL. */
static int
git_wrong_word (char       *why,
                char const *head,
                char const *str,
                size_t      max,
                char const *tail)
{
	return git_wrong(why, "%s%.*s%s", head, (int)jb_utf8(str, strnlen(str, max)),
	                 str, tail);
}

/* The repository named @p name, if one is listed so; else nullptr. */
static struct git_repo const *
git_find (struct git const *git,
          char const       *name)
{
	for (size_t i = 0; name && i < git->n; ++i)
		if (!strcmp(name, git->repo[i].name))
			return &git->repo[i];
	return nullptr;
}

/* Say which repositories a call may name; returns EINVAL. */
static int
git_repos_wrong (struct git const *git,
                 char             *why)
{
	size_t n = util_format(why, GIT_WHY, "The repo argument must name one of the "
	                       "repositories:");
	/* Once cut short, with "...", the list says no more. */
	for (size_t i = 0; i < git->n && n < GIT_WHY - 1; ++i)
		n += util_format(why + n, GIT_WHY - n, "%s %s", i ? "," : "",
		                 git->repo[i].name);
	if (n < GIT_WHY - 1)
		util_format(why + n, GIT_WHY - n, ".");
	return EINVAL;
}

/* Whether @p key is an argument the tool takes. */
static bool
git_known (char const *key)
{
	static char const *const keys[] = {
		"repo", "command", "options", "revisions", "paths", "pattern", "offset"
	};
	for (size_t i = 0; i < ARRAY_SIZE(keys); ++i)
		if (!strcmp(key, keys[i]))
			return true;
	return false;
}

/*
 * Fill @p argv, of GIT_ARGV entries, with the git command that @p args, a
 * call's arguments, ask for, its strings pointing into @p args and @p git,
 * which must outlive it.  Returns 0; or EINVAL, @p why, of GIT_WHY bytes,
 * saying what is wrong with the arguments, for the model to read.
 */
static int
git_command (struct git const   *git,
             struct json_object *args,
             char const         *argv[],
             char               *why)
{
	static_assert(WORD == 1024, "the problems say");
	static char const lists_problem[] =
		"The options, revisions and paths arguments must be arrays of strings, "
		"at most 16, 4 and 32 of them, each of 1 to 1024 bytes.";
	if (!args)
		return git_wrong(why, "Tool arguments must be an object.");
	struct json_object_iter it;
	json_object_object_foreachC(args, it)
		if (!git_known(it.key))
			return git_wrong_word(why, "Unknown argument: ", it.key, 64, ".");

	struct git_repo const *repo = git_find(git, jb_text(jb_get(args, "repo")));
	if (!repo)
		return git_repos_wrong(git, why);
	char const *cmd = jb_text(jb_get(args, "command"));
	size_t c = 0;
	while (cmd && c < ARRAY_SIZE(commands) && strcmp(cmd, commands[c].name))
		++c;
	if (!cmd || c == ARRAY_SIZE(commands))
		return git_wrong(why, "The command argument must be one of log, show, diff, "
		                 "grep, blame, ls-files, ls-tree, cat-file and rev-parse.");
	uint64_t takes = commands[c].takes;

	struct json_object *lists[3] = {
		jb_get(args, "options"), jb_get(args, "revisions"), jb_get(args, "paths")
	};
	static size_t const most[3] = { OPTIONS, REVISIONS, PATHS };
	size_t lens[3] = {};
	for (size_t l = 0; l < 3; ++l) {
		bool array = json_object_is_type(lists[l], json_type_array);
		lens[l] = array ? json_object_array_length(lists[l]) : 0;
		if (lists[l] && (!array || lens[l] > most[l]))
			return git_wrong(why, "%s", lists_problem);
	}
	if (lens[1] && !(takes & REVS))
		return git_wrong(why, "git %s takes no revisions.", cmd);
	if (lens[2] && !(takes & FILES))
		return git_wrong(why, "git %s takes no paths.", cmd);
	struct json_object *pattern = jb_get(args, "pattern");
	size_t len;
	char const *pat = git_word(pattern, &len);
	if (takes & PATTERN) {
		if (!pat)
			return git_wrong(why, "git grep needs a pattern argument: a string of "
			                 "1 to 1024 bytes.");
	} else if (pattern) {
		return git_wrong(why, "Only git grep takes a pattern.");
	}

	size_t k = 0;
	argv[k++] = "git";
	for (size_t i = 0; i < ARRAY_SIZE(fixed); ++i)
		argv[k++] = fixed[i];
	argv[k++] = "-c";
	argv[k++] = repo->safe;
	argv[k++] = "-C";
	argv[k++] = repo->path;
	argv[k++] = cmd;
	for (size_t i = 0; i < ARRAY_SIZE(commands[c].forced) && commands[c].forced[i];
	     ++i)
		argv[k++] = commands[c].forced[i];
	/* The options, then the pattern, the revisions and the paths, each
	 * checked as it goes in. */
	for (size_t i = 0; i < lens[0]; ++i) {
		char const *s = git_word(json_object_array_get_idx(lists[0], i), &len);
		if (!s)
			return git_wrong(why, "%s", lists_problem);
		if (!git_allowed(s, len, commands[c].options)) {
			char head[48];
			util_format(head, sizeof head, "git %s takes no option ", cmd);
			return git_wrong_word(why, head, s, 100, " here.");
		}
		argv[k++] = s;
	}
	if (pat) {
		argv[k++] = "-e";
		argv[k++] = pat;
	}
	if (takes & END)
		argv[k++] = "--end-of-options";
	for (size_t i = 0; i < lens[1]; ++i) {
		char const *s = git_word(json_object_array_get_idx(lists[1], i), &len);
		if (!s)
			return git_wrong(why, "%s", lists_problem);
		if (*s == '-' || !git_inside(s))
			return git_wrong(why, "A revision may not start with - or /, nor have a "
			                 ".. component.");
		argv[k++] = s;
	}
	if (takes & FILES)
		argv[k++] = "--";
	for (size_t i = 0; i < lens[2]; ++i) {
		char const *s = git_word(json_object_array_get_idx(lists[2], i), &len);
		if (!s)
			return git_wrong(why, "%s", lists_problem);
		if (!git_inside(s))
			return git_wrong(why, "A path must be relative to the repository's top, "
			                 "with no .. component.");
		argv[k++] = s;
	}
	argv[k] = nullptr;
	return 0;
}

/* A git call's git has ended, its output all in: take it in steps of the
 * call's own. */
static void
git_back (struct child *c)
{
	mcp_slot_ready(c->owner);
}

/* The text that util_format() writes into the array @p a. */
#define FORMAT(a, ...) ((struct tool_text){ a, util_format(a, sizeof a, __VA_ARGS__) })

/* @p c, the git of @p slot's call, has ended, its output all taken: let go
 * of it; after the output, a blank line between, say how git ended if it
 * failed, that the output was cut, or that there was none; then finish.  An
 * exit status of 1 with nothing written, as grep's finding none, is no
 * failure. */
static void
git_ended (struct mcp_slot *slot,
           struct child    *c)
{
	static_assert(GIT_MAX == 2 << 20, "the note says 2 MiB");
	struct tool_text out = mcp_slot_answer(slot);
	char buf[GIT_NOTE - 2];  /* the blank line takes 2 bytes at most */
	struct tool_text note;
	bool failed = true;
	if (c->late) {
		struct git_loop const *gl = mcp_slot_part(slot);
		note = FORMAT(buf, "[git took longer than %.0f s, and was stopped.]",
		              (double)gl->git->timeout / 1e9);
	} else if (c->cut) {
		note = TOOL_TEXT("[The output goes on past the 2 MiB shown.]");
		failed = c->code == CLD_EXITED && c->status && (c->status != 1 || out.len);
	} else if (c->code != CLD_EXITED) {
		note = FORMAT(buf, "[git was killed by signal %d.]", c->status);
	} else if (c->status) {
		note = FORMAT(buf, "[git exited with status %d.]", c->status);
		failed = c->status != 1 || out.len;
	} else {
		note = out.len ? (struct tool_text){} : TOOL_TEXT("[No output.]");
		failed = false;
	}
	*mcp_slot_own(slot) = nullptr;
	child_destroy(&c);
	struct tool_text const p[] = {
		!out.len ? TOOL_TEXT("") : out.ptr[out.len - 1] == '\n' ? TOOL_TEXT("\n")
		                                                        : TOOL_TEXT("\n\n"),
		note,
	};
	int e = 0;
	size_t taken;
	for (size_t i = note.ptr ? 0 : ARRAY_SIZE(p); i < ARRAY_SIZE(p) && !e; ++i)
		e = mcp_slot_write(slot, p[i], 0, &taken);
	if (e) {
		mcp_slot_fail(slot, e);
		return;
	}
	mcp_slot_finish(slot, failed ? TOOL_FAILED : 0);
}
#undef FORMAT

/* Take a share of the output of @p slot's git into the call's output, made
 * UTF-8 text, a character cut at the share's end left to the next; once all
 * is in, end, in the next step if the share has no room left for the
 * note. */
static void
git_step (struct mcp_slot *slot)
{
	struct child *c = *mcp_slot_own(slot);
	struct span left = buf_peek(&c->out);
	size_t room = loop_room(c->loop, LOOP_COPY);
	size_t n = left.len < room ? left.len : room;
	int e = mcp_slot_write(slot, (struct tool_text){ (char const *)left.ptr, n },
	                       n < left.len ? TOOL_MORE : 0, &n);
	if (e) {
		mcp_slot_fail(slot, e);
		return;
	}
	buf_consume(&c->out, n);
	if (n < left.len || loop_room(c->loop, LOOP_COPY) < GIT_NOTE) {
		mcp_slot_ready(slot);
		return;
	}
	git_ended(slot, c);
}

/* Start a git call in @p slot: the command @p args asks for, in the
 * repository it names, run as the call's own child on the loop of the
 * tool's part. */
static void
git_start (struct mcp_slot    *slot,
           struct json_object *args)
{
	struct git_loop *gl = mcp_slot_part(slot);
	struct git const *git = gl->git;
	char const *argv[GIT_ARGV];
	char why[GIT_WHY];
	if (git_command(git, args, argv, why)) {
		mcp_slot_tell(slot, (struct tool_text){ why, strlen(why) }, TOOL_FAILED);
		return;
	}
	if (mcp_slot_offset(slot, args))
		return;
	struct child_cmd const cmd = {
		.file = "git",
		.argv = argv,
		.envp = git->env,
		.max = GIT_MAX,
		.deadline = gl->loop->now + git->timeout,
		.grace = GIT_GRACE_NS,
		.flags = CHILD_STDERR
	};
	int e = 0;
	*mcp_slot_own(slot) = child_create(gl->loop, &gl->children, &cmd, git_back, slot, &e);
	if (e == EBUSY) {
		mcp_slot_tell(slot, TOOL_TEXT("Too many commands are running; try again in a "
		                              "few seconds."), TOOL_FAILED);
		return;
	}
	if (e) {
		size_t n = util_format(why, sizeof why, "Cannot run git: %s.", strerror(e));
		LOG(LOG_WARN, "%s", why);
		mcp_slot_tell(slot, (struct tool_text){ why, n }, TOOL_FAILED);
		return;
	}
	mcp_slot_park(slot);
}

/* Let go of a git call's git, which kills it if it runs. */
static void
git_drop (struct mcp_slot *slot)
{
	struct child *c = *mcp_slot_own(slot);
	child_destroy(&c);
}

/* Creates the git tool's part of @p loop for @p service, a struct git.  The
 * part has the list of the children that the git calls on the loop run.
 * Returns the part, or nullptr with ENOMEM in @p *e. */
void *
git_loop_create (void        *service,
                 struct loop *loop,
                 int         *e)
{
	struct git_loop *gl = malloc(sizeof *gl);
	if (!gl) {
		*e = ENOMEM;
		return nullptr;
	}
	*gl = (struct git_loop){ .git = service, .loop = loop };
	list_init(&gl->children);
	*e = 0;
	return gl;
}

/* Free the children of the calls on the loop, as it stops, killing any
 * still alive; then free @p *p_part, the git tool's part of the loop, and
 * set it to nullptr. */
void
git_loop_destroy (void **p_part)
{
	struct git_loop *gl = *p_part;
	if (!gl)
		return;
	*p_part = nullptr;
	child_destroy_all(&gl->children);
	free(gl);
}

struct plugin const git_plugin = {
	.tools = {{
		.definition = git_definition,
		.start = git_start,
		.step = git_step,
		.drop = git_drop,
		.flags = TOOL_LENT | TOOL_PAGED | TOOL_DETACH | TOOL_STREAM
	}}
};
