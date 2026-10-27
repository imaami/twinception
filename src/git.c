/** @file The git tool (git.h). */
#include <errno.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "child.h"
#include "cstr_view.h"
#include "git.h"
#include "git_view.h"
#include "jb.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "mcp.h"
#include "plugin.h"
#include "util.h"

static char const git_commit_suffix[] = "^{commit}";
static char const git_ignore_rev[] = "--ignore-rev=";

enum {
	GIT_MAX   = 4 << 20,  /**< output bytes, at most */
	GIT_ARGV  = 128,      /**< a command's argv, its nullptr included */
	GIT_WHY   = 256,      /**< bytes of what is wrong with a call */
	GIT_NOTE  = 96,       /**< bytes ending the output, at most: a blank line, a note */
	OPTIONS   = 16,       /**< a call's options, at most */
	REVISIONS = 4,        /**< its revisions */
	PATHS     = 32,       /**< its paths */
	WORD      = 1024,     /**< bytes of an option, a revision, a path or a pattern */
	REV_WORD  = WORD + 64 + 1, /**< a full SHA-256 ID and the original path */
	ROOTS     = 2 * REVISIONS + OPTIONS,
	ENV       = 13,       /**< git's environment, its nullptr included */
};

/** From SIGTERM to SIGKILL at a git call's deadline. */
static constexpr uint64_t GIT_GRACE_NS = 1'000'000'000;

static_assert(GIT_MAX == 4 << 20 && GIT_GRACE_NS == 1'000'000'000, "git.h says");

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

enum git_phase : uint64_t {
	GIT_META,
	GIT_REFS,
	GIT_RESOLVE,
	GIT_CHECK,
	GIT_RUN,
};

/** A root is resolved in place before any caller-visible command runs. */
struct git_root {
	char name[WORD + sizeof git_commit_suffix];
	char option[sizeof git_ignore_rev + 64];
};

struct git_revision {
	char const *path;
	size_t      root;
	size_t      dots;
	char        text[REV_WORD];
};

/** One deadline and one immutable history view span every subprocess. */
struct git_call {
	struct child          *child;
	struct git_view       *view;
	struct git_repo const *repo;
	struct json_object    *args;
	struct buf             metadata;
	char const            *argv[GIT_ARGV];
	uint64_t               deadline;
	size_t                 command;
	size_t                 n_roots;
	size_t                 n_revisions;
	struct git_revision    revisions[REVISIONS];
	struct git_root        roots[ROOTS];
	enum git_phase         phase;
};

/* What git runs with whatever the call: GIT_OPTIONS_FIXED, no signature
 * checks, no implicit worktree operations, and no submodule's diff (which a
 * superproject's show or log -p runs as a git of its own, without these
 * options). */
static char const *const fixed[] = {
	GIT_OPTIONS_FIXED,
	"-c", "core.bare=true",
	"-c", "log.showSignature=false",
	"-c", "diff.submodule=short",
	"-c", "diff.autoRefreshIndex=false",
};

/*
 * An option a command takes, as it is, or, ending in "=", with a value after
 * it; in "#", with digits attached; in "*", with any value attached.
 */
#define O(s) CSTR_VIEW_INIT(s)
#define DISPLAY O("--oneline"), O("--stat"), O("--shortstat"), O("--numstat"), \
	O("--name-only"), O("--name-status"), O("--patch"), O("-p"), O("--no-patch"), \
	O("--abbrev-commit"), O("--format="), O("--pretty="), O("--date="), \
	O("--unified="), O("-U#"), O("--word-diff"), O("-w"), O("--ignore-all-space"), \
	O("-b"), O("--ignore-space-change"), O("--decorate"), O("--no-decorate"), \
	O("--diff-filter="), O("-M"), O("--find-renames")
static struct cstr_view const log_options[] = {
	DISPLAY, O("--graph"), O("--reverse"), O("--first-parent"), O("--no-merges"),
	O("--merges"), O("--follow"), O("--max-count="), O("-n#"),
	O("--skip="), O("--since="), O("--until="), O("--after="), O("--before="),
	O("--author="), O("--committer="), O("--grep="), O("-i"),
	O("--regexp-ignore-case"), O("--all-match"), O("--invert-grep"), O("-E"),
	O("--extended-regexp"), O("-F"), O("--fixed-strings"), O("-S*"), O("-G*"), {}
};
static struct cstr_view const show_options[] = { DISPLAY, O("-s"), {} };
static struct cstr_view const diff_options[] = { DISPLAY, {} };
#undef DISPLAY
static struct cstr_view const grep_options[] = {
	O("-n"), O("--line-number"), O("-i"), O("--ignore-case"), O("-w"),
	O("--word-regexp"), O("-v"), O("--invert-match"), O("-l"),
	O("--files-with-matches"), O("-L"), O("--files-without-match"), O("-c"),
	O("--count"), O("-E"), O("--extended-regexp"), O("-F"), O("--fixed-strings"),
	O("-P"), O("--perl-regexp"), O("-G"), O("--basic-regexp"), O("-h"), O("-H"),
	O("--full-name"), O("-o"), O("--only-matching"), O("-C#"), O("-A#"), O("-B#"),
	O("--context="), O("--after-context="), O("--before-context="), O("--max-depth="),
	O("--max-count="), O("-m#"), O("--break"), O("--heading"), O("-p"),
	O("--show-function"), O("-W"), O("--function-context"), O("--all-match"), {}
};
static struct cstr_view const blame_options[] = {
	O("-w"), O("-M"), O("-C"), O("-s"), O("-e"), O("-l"), O("-t"), O("-f"),
	O("--show-name"), O("--porcelain"), O("--line-porcelain"), O("--date="),
	O("-L*"), O("--root"), O("--ignore-rev="), {}
};
static struct cstr_view const ls_tree_options[] = {
	O("-r"), O("-t"), O("-d"), O("--name-only"), O("--name-status"),
	O("--object-only"), O("--long"), O("-l"), O("--full-name"), O("--full-tree"),
	O("--abbrev"), O("--abbrev="), O("--format="), {}
};
static struct cstr_view const cat_file_options[] = {
	O("-t"), O("-s"), O("-p"), O("-e"), {}
};
static struct cstr_view const rev_parse_options[] = {
	O("--short"), O("--short="), O("--verify"), O("-q"), O("--quiet"), {}
};
/* Rejected source selectors get a policy explanation, not syntax advice. */
static struct cstr_view const source_options[] = {
	O("--cached"), O("--staged"), O("--untracked"), O("--no-index"), {}
};
#undef O

/* Where a command takes what; all revision roots must name commits. */
enum {
	FILES   = 1 << 0,  /* paths, after "--" */
	END     = 1 << 1,  /* --end-of-options, except where git takes it amiss */
	PATTERN = 1 << 2,  /* a required pattern, after -e */
	OBJECT  = 1 << 3,  /* a committed path after a revision's colon */
	RANGE   = 1 << 4,  /* a pair of commits separated by .. or ... */
	PAIR    = 1 << 5,  /* exactly two commits, separately or as a range */
	SINGLE  = 1 << 6,  /* at most one revision */
};

static struct {
	char const             *name;
	struct cstr_view const *options;
	char const             *forced[3];  /* options askai gives it */
	char const             *description;
	uint64_t                takes;
} const commands[] = {
	{ "log",       log_options,       { "--no-ext-diff", "--no-textconv" },
	  "Committed history; defaults to HEAD. Paths restrict the history.",
	  FILES | END | RANGE },
	{ "show",      show_options,      { "--no-ext-diff", "--no-textconv" },
	  "Committed contents; defaults to HEAD. HEAD:path reads a committed file.",
	  FILES | END | OBJECT },
	{ "diff",      diff_options,      { "--no-ext-diff", "--no-textconv" },
	  "Compare two commits: give two revisions, or one explicit A..B or A...B "
	  "range. Paths restrict the comparison.", FILES | END | RANGE | PAIR },
	{ "grep",      grep_options,      { "--no-textconv" },
	  "Search committed trees; defaults to HEAD. Use -F for literal search, "
	  "-E for extended regex, -n for lines, -C3 for context.",
	  FILES | END | PATTERN },
	{ "blame",     blame_options,     { "--no-textconv" },
	  "Annotate a committed file; defaults to HEAD. Give one path; -L10,30 "
	  "limits the line range.", FILES | SINGLE },
	{ "ls-tree",   ls_tree_options,   {},
	  "Paths in a committed tree; defaults to HEAD. -r recurses.",
	  FILES | SINGLE },
	{ "cat-file",  cat_file_options,  {},
	  "Inspect committed contents: -p for contents, -t for type, -s for bytes, "
	  "-e for existence. Defaults to HEAD; revision:path names a committed file.",
	  END | OBJECT | SINGLE },
	{ "rev-parse", rev_parse_options, { "--revs-only" },
	  "Resolve commit references to immutable commit IDs; defaults to HEAD. "
	  "--verify requires one valid revision.", 0 },
};

static_assert(1 + ARRAY_SIZE(fixed) + 5 + 3 + OPTIONS + 2 + 1 + REVISIONS + 1 +
              PATHS + 1 <= GIT_ARGV, "a command fits argv");
static_assert(1 + ARRAY_SIZE(fixed) + 5 + 10 + ROOTS + 1 <= GIT_ARGV,
              "history validation fits argv");

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
	for (; git->n < n; ++git->n) {
		int e = git_repo_init(&git->repo[git->n], paths[git->n]);
		if (e)
			return e;
	}
	char *ceilings = git_ceilings(git);
	if (!ceilings)
		return ENOMEM;
	/* The last but one is GIT_ATTR_SOURCE's, when a call has a HEAD. */
	char *const vars[ENV] = {
		util_env_path(), ceilings, GIT_ENV_FIXED, "GIT_OPTIONAL_LOCKS=0",
		nullptr, nullptr
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
	free(git->env[1]);
	free(git);
}

/* Append @p str to the array @p arr, which is let go of if that fails. */
static struct json_object *
git_append (struct json_object *arr,
            char const         *str)
{
	if (arr && jb_push(arr, json_object_new_string(str))) {
		json_object_put(arr);
		arr = nullptr;
	}
	return arr;
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
	              GIT_MAX == 4 << 20, "the description says");
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
			"Run a read-only git command in one of the repositories of the "
			"server, and get its output, with its errors, up to 4 MiB. The "
			"command enum lists the available operations. Use help:true with "
			"any command for its exact options, limits and source selection. "
			"Only committed contents are visible; the index, working tree, "
			"untracked and ignored files are unavailable. Commits must be "
			"reachable from HEAD, branches, tags or remote-tracking refs. "
			"Commands default to "
			"HEAD, except diff, which needs two commits or an explicit range. "
			"To read a file as of the last commit, use "
			"show with the whole HEAD:path as the revision and no paths, for "
			"example \"revisions\": [\"HEAD:src/main.c\"]. Any other commit "
			"can replace HEAD. Give each option with its value attached "
			"(--max-count=20, not -n 20). Only the usual read-only options are "
			"allowed."),
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
				"help", jb_object(
					"type", json_object_new_string("boolean"),
					"description", json_object_new_string(
						"Describe this command without running it. "
						"Give repo, command, help:true and optional offset."),
					nullptr),
				"options", git_strings("Options, such as --oneline or "
				                       "--max-count=20. Use help:true to list "
				                       "the command's exact allowed options.", OPTIONS),
				"revisions", git_strings("Revisions, such as HEAD~3, main..topic "
				                         "or the hash of a commit. For show, "
				                         "HEAD:path names the contents of a file "
				                         "as of that commit. References and hashes "
				                         "may use ^/~ ancestry and ^{commit}. Reflogs, "
				                         "stash, index and raw blob/tree IDs are "
				                         "excluded. The resolved commit must belong "
				                         "to HEAD, branch, tag or remote-tracking "
				                         "history.", REVISIONS),
				"paths", git_strings("Paths in the repository, relative to its "
				                     "top. They limit the output of a command "
				                     "such as log or diff to those paths. They "
				                     "do not read a file.", PATHS),
				"pattern", jb_object(
					"type", json_object_new_string("string"),
					"description", json_object_new_string(
						"The pattern for grep. Only grep takes a "
						"pattern."),
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

/** Match the same spellings that help lists. ENODATA means a missing value,
 * EINVAL nondecimal digits, ENOENT no matching spelling. An accepted spelling
 * wins over a malformed alternative, such as --abbrev beside --abbrev=. */
static int
git_option_find (struct cstr_view const *allowed,
                 char const             *opt,
                 size_t                  len,
                 struct cstr_view       *match)
{
	*match = (struct cstr_view){};
	int e = ENOENT;
	for (; allowed->data; ++allowed) {
		char const *a = allowed->data;
		size_t n = allowed->len;
		char last = a[n - 1];
		if (last != '=' && last != '#' && last != '*') {
			if (len == n && !memcmp(opt, a, n)) {
				*match = *allowed;
				return 0;
			}
			continue;
		}
		if (len < n - 1 || memcmp(opt, a, n - 1) ||
		    (last == '=' && len >= n && opt[n - 1] != '='))
			continue;
		size_t at = last == '=' ? n : n - 1;
		*match = *allowed;
		if (len <= at)
			e = ENODATA;
		else if (last == '#' && strspn(opt + at, "0123456789") != len - at)
			e = EINVAL;
		else
			return 0;
	}
	return e;
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

/** A reference or hash with ^/~ ancestry or commit peels, checked as a commit. */
static int
git_commit (char       *dest,
            char const *src,
            size_t      len)
{
	memcpy(dest, src, len);
	dest[len] = '\0';
	size_t base = strcspn(dest, "^~:@{}");
	if (!base || *dest == '-' || !git_inside(dest) || strstr(dest, "..") ||
	    (base == 5 && !memcmp(dest, "stash", 5)) ||
	    (base == 10 && !memcmp(dest, "refs/stash", 10)))
		return EINVAL;
	for (size_t at = base; at < len;) {
		if (len - at >= sizeof git_commit_suffix - 1 &&
		    !memcmp(dest + at, git_commit_suffix, sizeof git_commit_suffix - 1)) {
			at += sizeof git_commit_suffix - 1;
			continue;
		}
		if (dest[at] != '^' && dest[at] != '~')
			return EINVAL;
		++at;
		while (dest[at] >= '0' && dest[at] <= '9')
			++at;
	}
	memcpy(dest + len, git_commit_suffix, sizeof git_commit_suffix);
	return 0;
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

/** Validate one option, explaining only the repair its failure needs. */
static int
git_option_check (char const *opt,
                  size_t      len,
                  size_t      command,
                  size_t      index,
                  char       *why)
{
	struct cstr_view match;
	int e = git_option_find(commands[command].options, opt, len, &match);
	if (!e)
		return 0;
	int shown = (int)jb_utf8(opt, len < 80 ? len : 80);
	if (match.data) {
		char last = match.data[match.len - 1];
		size_t at = match.len - (last != '=');
		char const *value = last == '#' ? "NUMBER" : "VALUE";
		char const *digits = last == '#' ? " containing decimal digits" : "";
		return git_wrong(why, "options[%zu]: '%.*s' %s. Use '%.*s%s' as one "
		                 "options entry%s.", index, shown, opt,
		                 e == ENODATA ? "needs an attached value"
		                              : "has a nonnumeric attached value",
		                 (int)at, match.data, value, digits);
	}
	if (!git_option_find(source_options, opt, len, &match)) {
		char const *source = commands[command].takes & PAIR
			? "supply two commits or one A..B/A...B range in revisions."
			: "select a commit with revisions (default HEAD).";
		return git_wrong(why, "options[%zu]: '%.*s' requests an unavailable data "
		                 "source. Index and working-tree reads are disabled. "
		                 "Omit this option and %s", index, shown, opt, source);
	}
	return git_wrong(why, "options[%zu]: '%.*s' is not available for git %s in "
	                 "this tool. Omit it; use help:true for this command's options.",
	                 index, shown, opt, commands[command].name);
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
		"repo", "command", "help", "options", "revisions", "paths", "pattern", "offset"
	};
	for (size_t i = 0; i < ARRAY_SIZE(keys); ++i)
		if (!strcmp(key, keys[i]))
			return true;
	return false;
}

/*
 * Validate the call and retain its arguments and revision roots. Returns 0; or
 * EINVAL, @p why, of GIT_WHY bytes, saying what is wrong with the arguments.
 */
static int
git_call_command (struct git_call   *call,
                  struct git const *git,
                  char             *why)
{
	static_assert(WORD == 1024, "the problems say");
	struct json_object *args = call->args;
	if (!args)
		return git_wrong(why, "Tool arguments must be an object.");
	struct json_object_iter it;
	json_object_object_foreachC(args, it)
		if (!git_known(it.key))
			return git_wrong_word(why, "Unknown argument: ", it.key, 64, ".");

	size_t len;
	struct git_repo const *repo = git_find(git, git_word(jb_get(args, "repo"), &len));
	if (!repo)
		return git_repos_wrong(git, why);
	call->repo = repo;
	char const *cmd = git_word(jb_get(args, "command"), &len);
	size_t c = 0;
	while (cmd && c < ARRAY_SIZE(commands) && strcmp(cmd, commands[c].name))
		++c;
	if (!cmd || c == ARRAY_SIZE(commands)) {
		if (cmd && (!strcmp(cmd, "status") || !strcmp(cmd, "ls-files")))
			return git_wrong(why, "git %s is unavailable: index and working-tree "
			                 "reads are disabled. Use git ls-tree with "
			                 "options [\"-r\", \"--name-only\"] to list committed "
			                 "files; revisions defaults to HEAD.", cmd);
		size_t n = util_format(why, GIT_WHY, "The command argument must be one of:");
		for (size_t i = 0; i < ARRAY_SIZE(commands) && n < GIT_WHY - 1; ++i)
			n += util_format(why + n, GIT_WHY - n, "%s %s", i ? "," : "",
			                 commands[i].name);
		if (n < GIT_WHY - 1)
			util_format(why + n, GIT_WHY - n, ". Use help:true for its options.");
		return EINVAL;
	}
	call->command = c;
	uint64_t takes = commands[c].takes;
	struct json_object *help;
	if (json_object_object_get_ex(args, "help", &help)) {
		if (!json_object_is_type(help, json_type_boolean))
			return git_wrong(why, "The help argument must be a boolean.");
		if (json_object_get_boolean(help)) {
			json_object_object_foreachC(args, it)
				if (strcmp(it.key, "repo") && strcmp(it.key, "command") &&
				    strcmp(it.key, "help") && strcmp(it.key, "offset"))
					return git_wrong(why, "With help:true, give only repo, "
					                 "command, help and optional offset.");
			return 0;
		}
	}

	static char const *const names[3] = { "options", "revisions", "paths" };
	static size_t const most[3] = { OPTIONS, REVISIONS, PATHS };
	struct json_object *lists[3] = {};
	size_t lens[3] = {};
	for (size_t l = 0; l < 3; ++l) {
		if (!json_object_object_get_ex(args, names[l], &lists[l]))
			continue;
		if (!json_object_is_type(lists[l], json_type_array))
			return git_wrong(why, "The %s argument must be an array of strings "
			                 "(at most %zu entries).", names[l], most[l]);
		lens[l] = json_object_array_length(lists[l]);
		if (lens[l] > most[l])
			return git_wrong(why, "The %s argument has %zu entries; its limit is "
			                 "%zu.", names[l], lens[l], most[l]);
		for (size_t i = 0; i < lens[l]; ++i)
			if (!git_word(json_object_array_get_idx(lists[l], i), &len))
				return git_wrong(why, "%s[%zu] must be a string of 1 to %d "
				                 "bytes with no NUL.", names[l], i, WORD);
	}
	if ((takes & SINGLE) && lens[1] > 1)
		return git_wrong(why, "git %s takes at most one revision.", cmd);
	if (lens[2] && !(takes & FILES))
		return git_wrong(why, "git %s takes no paths. To name a file, give the "
		                 "revision HEAD:path.", cmd);
	struct json_object *pattern = jb_get(args, "pattern");
	char const *pat = git_word(pattern, &len);
	if (takes & PATTERN) {
		if (!pat)
			return git_wrong(why, "git grep needs a pattern argument: a string of "
			                 "1 to 1024 bytes.");
	} else if (pattern) {
		return git_wrong(why, "Only git grep takes a pattern.");
	}

	size_t k = 0;
	char const **argv = call->argv;
	argv[k++] = cmd;
	for (size_t i = 0; i < ARRAY_SIZE(commands[c].forced) && commands[c].forced[i];
	     ++i)
		argv[k++] = commands[c].forced[i];
	/* The options, then the pattern, the revisions and the paths, each
	 * checked as it goes in. */
	for (size_t i = 0; i < lens[0]; ++i) {
		char const *s = jb_str(json_object_array_get_idx(lists[0], i), &len);
		int e = git_option_check(s, len, c, i, why);
		if (e)
			return e;
		if (!strncmp(s, git_ignore_rev, sizeof git_ignore_rev - 1)) {
			struct git_root *root = &call->roots[call->n_roots++];
			if (git_commit(root->name, s + sizeof git_ignore_rev - 1,
			               len - (sizeof git_ignore_rev - 1)))
				return git_wrong(why, "--ignore-rev needs a commit reference or hash.");
			root->option[0] = '-';
			s = root->option;
		}
		argv[k++] = s;
	}
	if (pat) {
		argv[k++] = "-e";
		argv[k++] = pat;
	}
	if (takes & END)
		argv[k++] = "--end-of-options";
	size_t endpoints = lens[1];
	call->n_revisions = lens[1] ? lens[1] : 1;
	for (size_t i = 0; i < call->n_revisions; ++i) {
		char const *s = "HEAD";
		len = sizeof "HEAD" - 1;
		if (lens[1])
			s = jb_str(json_object_array_get_idx(lists[1], i), &len);
		char const *path = strchr(s, ':');
		size_t root = path ? (size_t)(path - s) : len;
		if (path && (!(takes & OBJECT) || !path[1] || !git_inside(path + 1)))
			return git_wrong(why, "revisions[%zu]: only show and cat-file take "
			                 "commit:path, with a nonempty relative path.", i);
		char const *range = strstr(s, "..");
		if (range && range >= s + root)
			range = nullptr;
		if (range && (!(takes & RANGE) || ((takes & PAIR) && lens[1] != 1)))
			return git_wrong(why, "revisions[%zu]: ranges are allowed only by "
			                 "log, or as diff's single A..B or A...B argument.", i);
		size_t left = range ? (size_t)(range - s) : root;
		struct git_revision *rev = &call->revisions[i];
		rev->root = call->n_roots;
		rev->path = path ? path : "";
		int e = git_commit(call->roots[call->n_roots++].name, s, left);
		if (!e && range) {
			++endpoints;
			rev->dots = 2 + (range[2] == '.');
			e = git_commit(call->roots[call->n_roots++].name, range + rev->dots,
			               root - left - rev->dots);
		}
		if (e)
			return git_wrong(why, "revisions[%zu]: use a commit reference or hash "
			                 "with optional ^/~ ancestry. Stash, reflogs, index "
			                 "selectors and revision expansions are unavailable.", i);
		argv[k++] = rev->text;
	}
	if ((takes & PAIR) && endpoints != 2)
		return git_wrong(why, "git diff needs two commits, or one explicit "
		                 "A..B or A...B range.");
	if (takes & FILES)
		argv[k++] = "--";
	for (size_t i = 0; i < lens[2]; ++i) {
		char const *s = jb_text(json_object_array_get_idx(lists[2], i));
		if (!git_inside(s) || !strncmp(s, ":(", 2))
			return git_wrong(why, "paths[%zu] must be relative to the repository's "
			                 "top, with no .. component or :(...) pathspec magic.", i);
		argv[k++] = s;
	}
	argv[k] = nullptr;
	return 0;
}

/** Describe exactly the command and options that validation accepts. */
static void
git_help (struct mcp_slot *slot,
          size_t           command)
{
	struct json_object *options = json_object_new_array();
	for (struct cstr_view const *o = commands[command].options; o->data; ++o)
		options = git_append(options, o->data);
	uint64_t takes = commands[command].takes;
	struct json_object *help = jb_object(
		"command", json_object_new_string(commands[command].name),
		"description", json_object_new_string(commands[command].description),
		"options", options,
		"option_values", json_object_new_string(
			"Each option is one array entry. Append a value after a trailing =; "
			"replace trailing # with digits or * with a value. For example: "
			"--max-count=20, -n20, -C3. Do not split an option and its value."),
		"pagination", json_object_new_string(
			"The capture limit is 4 MiB. A response window may be smaller; "
			"its footer gives the exact next offset for the same arguments."),
		"limits", jb_object(
			"options", json_object_new_int(OPTIONS),
			"revisions", json_object_new_int(takes & SINGLE ? 1 :
			                                takes & PAIR ? 2 : REVISIONS),
			"paths", json_object_new_int(takes & FILES ? PATHS : 0),
			"argument_bytes", json_object_new_int(WORD),
			"capture_bytes", json_object_new_int(GIT_MAX),
			nullptr),
		"pattern_required", json_object_new_boolean(takes & PATTERN),
		nullptr);
	if (!help) {
		mcp_slot_fail(slot, ENOMEM);
		return;
	}
	size_t len;
	char const *text = json_object_to_json_string_length(help, JB_WIRE, &len);
	size_t taken;
	int e = text ? mcp_slot_write(slot, (struct tool_text){ text, len }, 0, &taken)
	             : ENOMEM;
	json_object_put(help);
	if (e)
		mcp_slot_fail(slot, e);
	else
		mcp_slot_finish(slot, 0);
}

/* A git call's git has ended, its output all in: take it in steps of the
 * call's own. */
static void
git_back (struct child *c)
{
	mcp_slot_ready(c->owner);
}

static void
git_call_destroy (struct git_call **p_call)
{
	if (!*p_call)
		return;
	child_destroy(&(*p_call)->child);
	git_view_destroy(&(*p_call)->view);
	buf_fini(&(*p_call)->metadata);
	json_object_put((*p_call)->args);
	free(*p_call);
	*p_call = nullptr;
}

static struct git_call *
git_call_create (struct json_object *args,
                 uint64_t            deadline,
                 int                *e)
{
	struct git_call *call = malloc(sizeof *call);
	if (!call) {
		*e = ENOMEM;
		return nullptr;
	}
	*call = (struct git_call){
		.args = json_object_get(args),
		.deadline = deadline,
	};
	*e = 0;
	return call;
}

/** Run one phase without extending the call's deadline. */
static int
git_call_run (struct git_call *call,
              struct git_loop *gl,
              struct mcp_slot *slot)
{
	char const *argv[GIT_ARGV] = { "git" };
	size_t k = 1;
	for (size_t i = 0; i < ARRAY_SIZE(fixed); ++i)
		argv[k++] = fixed[i];
	argv[k++] = "-c";
	argv[k++] = call->repo->safe;
	argv[k++] = "-C";
	argv[k++] = call->view ? git_view_directory(call->view) : call->repo->path;
	if (call->view)
		argv[k++] = "--git-dir=.";
	static char const *const meta[] = {
		"rev-parse", "--path-format=absolute", "--git-path", "objects",
		"--git-path", "shallow", "--show-object-format", "--verify", "--quiet",
		"HEAD^{commit}", nullptr
	};
	static char const *const refs[] = {
		"for-each-ref", "--format=%(objectname) %(refname)",
		"refs/heads/", "refs/tags/", "refs/remotes/", nullptr
	};
	char const *const *words = call->argv;
	if (call->phase == GIT_META)
		words = meta;
	else if (call->phase == GIT_REFS)
		words = refs;
	if (call->phase == GIT_RESOLVE || call->phase == GIT_CHECK) {
		bool resolve = call->phase == GIT_RESOLVE;
		argv[k++] = resolve ? "rev-parse" : "rev-list";
		argv[k++] = resolve ? "--revs-only" : "--max-count=1";
		if (!resolve) {
			argv[k++] = "--not";
			if (git_view_has_head(call->view))
				argv[k++] = "HEAD";
			argv[k++] = "--branches";
			argv[k++] = "--tags";
			argv[k++] = "--remotes";
			argv[k++] = "--not";
		}
		argv[k++] = "--end-of-options";
		for (size_t i = 0; i < call->n_roots; ++i)
			argv[k++] = call->roots[i].name;
		if (!resolve)
			argv[k++] = "--";
	} else {
		for (size_t i = 0; words[i]; ++i)
			argv[k++] = words[i];
	}
	argv[k] = nullptr;
	char *env[ENV];
	memcpy(env, gl->git->env, sizeof env);
	if (call->view && git_view_has_head(call->view))
		env[ENV - 2] = "GIT_ATTR_SOURCE=HEAD";
	struct child_cmd const cmd = {
		.file = "git",
		.argv = argv,
		.envp = env,
		.max = GIT_MAX,
		.deadline = call->deadline,
		.grace = GIT_GRACE_NS,
		.flags = call->phase == GIT_RUN ? CHILD_STDERR : 0,
	};
	int e = 0;
	call->child = child_create(gl->loop, &gl->children, &cmd, git_back, slot, &e);
	return e;
}

/** Replace every validated root with exactly one immutable commit ID. */
static int
git_call_resolve (struct git_call *call)
{
	struct span out = buf_peek(&call->child->out);
	size_t width = 0;
	for (size_t i = 0; i < call->n_roots; ++i) {
		unsigned char *end = out.len ? memchr(out.ptr, '\n', out.len) : nullptr;
		if (!end)
			return EINVAL;
		size_t len = (size_t)(end - out.ptr);
		if (!util_is_oid((char const *)out.ptr, len) || (width && len != width))
			return EINVAL;
		width = len;
		struct git_root *root = &call->roots[i];
		memcpy(root->name, out.ptr, len);
		root->name[len] = '\0';
		if (root->option[0])
			util_format(root->option, sizeof root->option, "%s%s", git_ignore_rev,
			            root->name);
		out.ptr = end + 1;
		out.len -= len + 1;
	}
	if (out.len)
		return EINVAL;
	for (size_t i = 0; i < call->n_revisions; ++i) {
		struct git_revision *rev = &call->revisions[i];
		char const *first = call->roots[rev->root].name;
		if (rev->dots)
			util_format(rev->text, sizeof rev->text, "%s%.*s%s", first,
			            (int)rev->dots, "...", call->roots[rev->root + 1].name);
		else
			util_format(rev->text, sizeof rev->text, "%s%s", first, rev->path);
	}
	return 0;
}

/** Internal discovery and authorization output must never reach the caller. */
static void
git_call_advance (struct git_call *call,
                  struct mcp_slot *slot)
{
	struct child *c = call->child;
	int e = c->late || c->cut || c->code != CLD_EXITED ? EIO : 0;
	if (!e && c->status && !(call->phase == GIT_META && c->status == 1))
		e = EINVAL;
	if (!e) {
		switch (call->phase) {
		case GIT_META:
			call->metadata = c->out;
			c->out = (struct buf){};
			break;
		case GIT_REFS:
			call->view = git_view_create(buf_peek(&call->metadata), buf_peek(&c->out), &e);
			buf_fini(&call->metadata);
			break;
		case GIT_RESOLVE:
			e = git_call_resolve(call);
			break;
		case GIT_CHECK:
			e = buf_peek(&c->out).len ? EACCES : 0;
			break;
		case GIT_RUN:
			break;
		}
	}
	bool late = c->late;
	child_destroy(&call->child);
	if (!e) {
		++call->phase;
		e = git_call_run(call, mcp_slot_part(slot), slot);
	}
	if (!e) {
		mcp_slot_park(slot);
		return;
	}
	*mcp_slot_own(slot) = nullptr;
	git_call_destroy(&call);
	mcp_slot_tell(slot, late ? TOOL_TEXT("Git history validation timed out.")
	                         : TOOL_TEXT("Cannot read the requested committed history. "
	                                     "Use commits reachable from HEAD, branches, "
	                                     "tags or remote-tracking refs."), TOOL_FAILED);
}

/* The text that util_format() writes into the array @p a. */
#define FORMAT(a, ...) ((struct tool_text){ a, util_format(a, sizeof a, __VA_ARGS__) })

/* @p call's git has ended, its output all taken: let go
 * of it; after the output, a blank line between, say how git ended if it
 * failed, that the output was cut, or that there was none; then finish.  An
 * exit status of 1 from grep means it found no matches. */
static void
git_ended (struct mcp_slot *slot,
           struct git_call *call)
{
	static_assert(GIT_MAX == 4 << 20, "the note says 4 MiB");
	struct child *c = call->child;
	struct tool_text out = mcp_slot_answer(slot);
	char buf[GIT_NOTE - 2];  /* the blank line takes 2 bytes at most */
	struct tool_text note;
	bool failed = true;
	if (c->late) {
		struct git_loop const *gl = mcp_slot_part(slot);
		note = FORMAT(buf, "[git took longer than %.0f s, and was stopped.]",
		              (double)gl->git->timeout / 1e9);
	} else if (c->cut) {
		note = TOOL_TEXT("[The output goes on past the 4 MiB shown.]");
		failed = c->code == CLD_EXITED && c->status;
	} else if (c->code != CLD_EXITED) {
		note = FORMAT(buf, "[git was killed by signal %d.]", c->status);
	} else if (c->status == 1 && (commands[call->command].takes & PATTERN)) {
		note = TOOL_TEXT("[No matches.]");
		failed = false;
	} else if (c->status) {
		char const *fmt = commands[call->command].takes & OBJECT
			? "[The committed object could not be read (git exited with status %d).]"
			: "[git exited with status %d.]";
		note = FORMAT(buf, fmt, c->status);
	} else {
		note = out.len ? (struct tool_text){} : TOOL_TEXT("[No output.]");
		failed = false;
	}
	*mcp_slot_own(slot) = nullptr;
	git_call_destroy(&call);
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
	struct git_call *call = *mcp_slot_own(slot);
	if (call->phase != GIT_RUN) {
		git_call_advance(call, slot);
		return;
	}
	struct child *c = call->child;
	/* Git's failed object lookup may disclose whether a path exists on disk. */
	if ((commands[call->command].takes & OBJECT) &&
	    c->code == CLD_EXITED && c->status) {
		git_ended(slot, call);
		return;
	}
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
	git_ended(slot, call);
}

/* Start a git call in @p slot: the command @p args asks for, in the
 * repository it names, run as the call's own child on the loop of the
 * tool's part. */
static void
git_start (struct mcp_slot    *slot,
           struct json_object *args)
{
	struct git_loop *gl = mcp_slot_part(slot);
	int e = 0;
	struct git_call *call = git_call_create(args, gl->loop->now + gl->git->timeout, &e);
	if (e) {
		mcp_slot_fail(slot, e);
		return;
	}
	char why[GIT_WHY];
	if (git_call_command(call, gl->git, why)) {
		git_call_destroy(&call);
		mcp_slot_tell(slot, (struct tool_text){ why, strlen(why) }, TOOL_FAILED);
		return;
	}
	if (mcp_slot_offset(slot, args)) {
		git_call_destroy(&call);
		return;
	}
	if (json_object_get_boolean(jb_get(args, "help"))) {
		git_help(slot, call->command);
		git_call_destroy(&call);
		return;
	}
	e = git_call_run(call, gl, slot);
	if (e)
		git_call_destroy(&call);
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
	*mcp_slot_own(slot) = call;
	mcp_slot_park(slot);
}

/* Let go of a git call's git, which kills it if it runs. */
static void
git_drop (struct mcp_slot *slot)
{
	struct git_call *call = *mcp_slot_own(slot);
	git_call_destroy(&call);
	*mcp_slot_own(slot) = nullptr;
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
