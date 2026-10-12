/** @file The git tool (git.h). */
#include <errno.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "git.h"
#include "jb.h"
#include "util.h"

enum {
	OPTIONS   = 16,    /* a call's options, at most */
	REVISIONS = 4,     /* its revisions */
	PATHS     = 32,    /* its paths */
	WORD      = 1024,  /* bytes of an option, a revision, a path or a pattern */
	ENV       = 12,    /* git's environment, its nullptr included */
};

/* A repository: its name, its path, and the setting that has git trust it,
 * one string of the three. */
struct repo {
	char *name;
	char *path;
	char *safe;
};

struct git {
	char       *env[ENV];
	uint64_t    timeout;
	size_t      n;
	struct repo repo[];
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
	bool ok = n && n <= 64 &&
	          strspn(path + start, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	                               "0123456789_.-") >= n && path[start] != '.';
	*len = ok ? n : 0;
	return ok ? path + start : nullptr;
}

struct git *
git_new (char const *const paths[],
         size_t             n,
         uint64_t           timeout)
{
	static char const safe[] = "safe.directory=";
	static char const env_path[] = "PATH=";
	static char const ceiling[] = "GIT_CEILING_DIRECTORIES=";
	struct git *git = calloc(1, sizeof *git + n * sizeof *git->repo);
	char const *path = getenv("PATH");
	path = path && *path ? path : "/usr/bin:/bin";
	size_t size = sizeof env_path + strlen(path);
	char *env = git ? malloc(size) : nullptr;
	if (!env) {
		free(git);
		errno = ENOMEM;
		return nullptr;
	}
	snprintf(env, size, "%s%s", env_path, path);
	git->env[0] = env;
	git->timeout = timeout;

	/* No repository found above one listed, which git would look for
	 * without one there: the ceilings, the listed ones' parents. */
	size_t ceilings = sizeof ceiling;
	for (size_t i = 0; i < n; ++i) {
		char real[PATH_MAX];
		size_t len = 0;
		char const *name = git_name(paths[i], &len);
		if (!name || !realpath(paths[i], real)) {
			int e = name ? errno : EINVAL;
			git_free(&git);
			errno = e;
			return nullptr;
		}
		size_t real_len = strlen(real);
		char *s = malloc(len + 1 + 2 * (real_len + 1) + sizeof safe - 1);
		if (!s) {
			git_free(&git);
			errno = ENOMEM;
			return nullptr;
		}
		struct repo *r = &git->repo[i];
		r->name = memcpy(s, name, len);
		r->name[len] = '\0';
		r->path = memcpy(s + len + 1, real, real_len + 1);
		r->safe = r->path + real_len + 1;
		memcpy(r->safe, safe, sizeof safe - 1);
		memcpy(r->safe + sizeof safe - 1, real, real_len + 1);
		git->n = i + 1;
		ceilings += real_len + 1;
	}
	char *ceiling_env = malloc(ceilings);
	if (!ceiling_env) {
		git_free(&git);
		errno = ENOMEM;
		return nullptr;
	}
	size_t at = sizeof ceiling - 1;
	memcpy(ceiling_env, ceiling, at);
	for (size_t i = 0; i < n; ++i) {
		char const *p = git->repo[i].path;
		size_t len = (size_t)(strrchr(p, '/') - p);
		if (i)
			ceiling_env[at++] = ':';
		memcpy(ceiling_env + at, len ? p : "/", len ? len : 1);
		at += len ? len : 1;
	}
	ceiling_env[at] = '\0';
	char *const vars[ENV] = {
		env, ceiling_env, "HOME=/nonexistent", "LC_ALL=C.UTF-8",
		"GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null",
		"GIT_TERMINAL_PROMPT=0", "GIT_OPTIONAL_LOCKS=0", "GIT_PAGER=cat",
		"GIT_NO_LAZY_FETCH=1", "GIT_ALLOW_PROTOCOL=none", nullptr
	};
	static_assert(sizeof vars == sizeof git->env);
	memcpy(git->env, vars, sizeof vars);
	return git;
}

void
git_free (struct git **p_git)
{
	struct git *git = *p_git;
	if (!git)
		return;
	*p_git = nullptr;
	for (size_t i = 0; i < git->n; ++i)
		free(git->repo[i].name);
	free(git->env[0]);
	free(git->env[1]);
	free(git);
}

char *const *
git_env (struct git const *git)
{
	return git->env;
}

uint64_t
git_timeout (struct git const *git)
{
	return git->timeout;
}

/* Append @p str to the array @p arr, which is let go of if that fails. */
static struct json_object *
append (struct json_object *arr,
        char const         *str)
{
	return arr && jb_push(arr, json_object_new_string(str)) ? arr : nullptr;
}

static struct json_object *
strings (char const *description,
         size_t      max)
{
	return jb_object(
		"type", json_object_new_string("array"),
		"items", jb_object("type", json_object_new_string("string"), nullptr),
		"maxItems", json_object_new_int((int32_t)max),
		"description", json_object_new_string(description),
		nullptr);
}

struct json_object *
git_definition (struct git const *git)
{
	static_assert(OPTIONS == 16 && REVISIONS == 4 && PATHS == 32 &&
	              GIT_MAX == 2 << 20, "the description says");
	struct json_object *repos = json_object_new_array();
	for (size_t i = 0; i < git->n; ++i)
		repos = append(repos, git->repo[i].name);
	struct json_object *cmds = json_object_new_array();
	for (size_t i = 0; i < ARRAY_SIZE(commands); ++i)
		cmds = append(cmds, commands[i].name);
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
				"options", strings("Options, such as --oneline or "
				                   "--max-count=20.", OPTIONS),
				"revisions", strings("Revisions, such as HEAD~3, main..topic "
				                     "or a commit's hash.", REVISIONS),
				"paths", strings("Paths in the repository, relative to its "
				                 "top.", PATHS),
				"pattern", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"grep's pattern, and only grep's."),
					nullptr),
				nullptr),
			"required", append(append(json_object_new_array(), "repo"), "command"),
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
word (struct json_object *val,
      size_t             *len)
{
	char const *s = jb_text(val);
	*len = s ? (size_t)json_object_get_string_len(val) : 0;
	return s && *len && *len <= WORD && !memchr(s, '\0', *len) ? s : nullptr;
}

/* Whether @p opt, of @p len bytes, is one of @p allowed. */
static bool
allowed (char const       *opt,
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
inside (char const *path)
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

/* Put @p head, @p str cut at @p max bytes but never inside a character, and
 * @p tail in @p why; returns @p why. */
static char const *
say (char       *why,
     char const *head,
     char const *str,
     size_t      max,
     char const *tail)
{
	snprintf(why, GIT_WHY, "%s%.*s%s", head, (int)jb_utf8(str, strnlen(str, max)),
	         str, tail);
	return why;
}

char const *
git_command (struct git const   *git,
             struct json_object *args,
             char const         *argv[],
             char               *why)
{
	static_assert(WORD == 1024, "the problems say");
	static char const lists_problem[] =
		"The options, revisions and paths arguments must be arrays of strings, "
		"at most 16, 4 and 32 of them, each of 1 to 1024 bytes.";
	if (!json_object_is_type(args, json_type_object))
		return "Tool arguments must be an object.";
	struct json_object_iter it;
	json_object_object_foreachC(args, it)
		if (strcmp(it.key, "repo") && strcmp(it.key, "command") &&
		    strcmp(it.key, "options") && strcmp(it.key, "revisions") &&
		    strcmp(it.key, "paths") && strcmp(it.key, "pattern"))
			return say(why, "Unknown argument: ", it.key, 64, ".");

	char const *name = jb_text(jb_get(args, "repo"));
	struct repo const *repo = nullptr;
	for (size_t i = 0; name && !repo && i < git->n; ++i)
		repo = strcmp(name, git->repo[i].name) ? nullptr : &git->repo[i];
	if (!repo) {
		int n = snprintf(why, GIT_WHY, "The repo argument must name one of the "
		                 "repositories:");
		for (size_t i = 0; n > 0 && (size_t)n < GIT_WHY && i < git->n; ++i)
			n += snprintf(why + n, GIT_WHY - (size_t)n, "%s %s", i ? "," : "",
			              git->repo[i].name);
		if (n > 0 && (size_t)n < GIT_WHY)
			snprintf(why + n, GIT_WHY - (size_t)n, ".");
		return why;
	}
	char const *cmd = jb_text(jb_get(args, "command"));
	size_t c = 0;
	while (cmd && c < ARRAY_SIZE(commands) && strcmp(cmd, commands[c].name))
		++c;
	if (!cmd || c == ARRAY_SIZE(commands))
		return "The command argument must be one of log, show, diff, grep, blame, "
		       "ls-files, ls-tree, cat-file and rev-parse.";
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
			return lists_problem;
	}
	if (lens[1] && !(takes & REVS)) {
		snprintf(why, GIT_WHY, "git %s takes no revisions.", cmd);
		return why;
	}
	if (lens[2] && !(takes & FILES)) {
		snprintf(why, GIT_WHY, "git %s takes no paths.", cmd);
		return why;
	}
	struct json_object *pattern = jb_get(args, "pattern");
	size_t len;
	char const *pat = word(pattern, &len);
	if (takes & PATTERN ? !pat : pattern != nullptr)
		return takes & PATTERN ? "git grep needs a pattern argument: a string "
		                         "of 1 to 1024 bytes."
		                       : "Only git grep takes a pattern.";

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
	for (size_t l = 0; l < 3; ++l) {
		if (l == 1 && pat) {
			argv[k++] = "-e";
			argv[k++] = pat;
		}
		if (l == 1 && takes & END)
			argv[k++] = "--end-of-options";
		if (l == 2 && takes & FILES)
			argv[k++] = "--";
		for (size_t i = 0; i < lens[l]; ++i) {
			char const *s = word(json_object_array_get_idx(lists[l], i), &len);
			if (!s)
				return lists_problem;
			if (!l && !allowed(s, len, commands[c].options)) {
				char head[48];
				snprintf(head, sizeof head, "git %s takes no option ", cmd);
				return say(why, head, s, 100, " here.");
			}
			if (l == 1 && (*s == '-' || !inside(s)))
				return "A revision may not start with - or /, nor have a .. "
				       "component.";
			if (l == 2 && !inside(s))
				return "A path must be relative to the repository's top, with no "
				       ".. component.";
			argv[k++] = s;
		}
	}
	argv[k] = nullptr;
	return nullptr;
}
