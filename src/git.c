/** @file The git tool (git.h). */
#include <errno.h>
#include <json-c/json.h>
#include <stdarg.h>
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
	for (size_t i = 0; i < git->n; ++i)
		size += strlen(git->repo[i].path) + 1;
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
git_append (struct json_object *arr,
            char const         *str)
{
	return arr && jb_push(arr, json_object_new_string(str)) ? arr : nullptr;
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

struct json_object *
git_definition (struct git const *git)
{
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
	(void)util_vformat(why, GIT_WHY, fmt, ap);
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
git_repo (struct git const *git,
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
	for (size_t i = 0; i < git->n; ++i)
		n += util_format(why + n, GIT_WHY - n, "%s %s", i ? "," : "",
		                 git->repo[i].name);
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

int
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
		return git_wrong(why, "Tool arguments must be an object.");
	struct json_object_iter it;
	json_object_object_foreachC(args, it)
		if (!git_known(it.key))
			return git_wrong_word(why, "Unknown argument: ", it.key, 64, ".");

	struct git_repo const *repo = git_repo(git, jb_text(jb_get(args, "repo")));
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
