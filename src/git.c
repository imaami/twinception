/** @file git.c
 *
 * The git tool.
 */
#define _GNU_SOURCE /* mempcpy() */
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
#include "review.h"
#include "schema.h"
#include "util.h"

static char const git_commit_suffix[] = "^{commit}";
static char const git_ignore_rev[] = "--ignore-rev=";

enum {
	GIT_MAX   = 4 << 20,  /**< output bytes, at most */
	GIT_ARGV  = 128,      /**< a command's argv, its nullptr included */
	GIT_WHY   = 256,      /**< bytes of what is wrong with a call */
	GIT_NOTE  = 256,      /**< the most bytes of the note at the end of the
	                           output, with the blank line before it */
	BINARY    = 8000,     /**< read refuses a file with a NUL in its first
	                           BINARY bytes, as git's binary test does */
	LINE_COST = 32,       /**< LOOP_COPY work that read counts for each line
	                           it writes, in addition to its bytes */
	OPTIONS   = 16,       /**< a call's options, at most */
	REVISIONS = 4,        /**< its revisions */
	PATHS     = 32,       /**< its paths */
	WORD      = 1024,     /**< bytes of an option, a revision, a path or a pattern */
	REV_WORD  = WORD + 64 + 1, /**< a full SHA-256 ID and the original path */
	ROOTS     = 2 * REVISIONS + OPTIONS,
	ENV       = 13,       /**< git's environment, its nullptr included */
	CEILINGS  = 1,        /**< the index of GIT_CEILING_DIRECTORIES in it */
	ATTR      = ENV - 2,  /**< the index of GIT_ATTR_SOURCE in it, which is
	                           set when a call has a HEAD */
};

/** From SIGTERM to SIGKILL at a git call's deadline. */
static constexpr uint64_t GIT_GRACE_NS = 1'000'000'000;

static_assert(GIT_MAX == 4 << 20 && GIT_GRACE_NS == 1'000'000'000, "git.h says");

/* A configured repository. path, safe and name share one allocation, which
 * begins at path: the real path, the safe.directory setting for git, and
 * the name. Each of the three ends in a NUL.
 */
struct git_repo {
	struct cstr_view name;
	char            *path;
	char            *safe;
};

struct git {
	char           *env[ENV];
	uint64_t        timeout;
	size_t          n;
	struct git_repo repo[];
};

/* What a client loop has of the git tool, its part of it: the children of
 * the calls there, the git, and the loop.
 */
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
	GIT_NUMBER,  /* read has scanned the file and writes the numbered lines */
	GIT_HELP,    /* the call describes its command with help:true and runs no git */
};

/** A root is resolved in place before any caller-visible command runs. */
struct git_root {
	char name[WORD + sizeof git_commit_suffix];
	char option[sizeof git_ignore_rev + 64];
};

struct git_revision {
	struct cstr_view path;  /* ":path" in the revision argument, or empty */
	size_t           root;
	size_t           dots;
	size_t           len;   /* the length of text */
	char             text[REV_WORD];
};

/* The lines that a read asks for, and how far the scan and then the
 * writing have come. Offsets are into git's output, call->child->out,
 * except head and mark, which are into the call's output (mcp_slot_answer()).
 */
struct git_read {
	size_t line;    /* the number of the first line to give, from 1 */
	size_t count;   /* the maximum number of lines to give */
	size_t room;    /* the bytes of the window left for numbered lines */
	size_t at;      /* the offset of the next byte to scan, then to write */
	size_t start;   /* the offset where the line that contains at starts */
	size_t lines;   /* the number of lines before start */
	size_t total;   /* the number of lines in the file, set when the scan ends */
	size_t from;    /* the offset of the first line to give */
	size_t to;      /* the offset after the last line to give */
	size_t last;    /* the number of the last line to give */
	size_t head;    /* the bytes of the header in the output, 0 until it is
	                   written */
	size_t mark;    /* the offset in the output where the last line written
	                   starts */
	size_t oid_len; /* the length of the commit ID at the start of rev */
	size_t rev_len; /* the length of rev, or 0 until the scan begins */
	char   rev[6 * REV_WORD]; /* the revision as commit:path, escaped for a
	                             JSON string */
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
	struct git_read        read;
	enum git_phase         phase;
};

/* What git runs with whatever the call: GIT_OPTIONS_FIXED, no signature
 * checks, no implicit worktree operations, no submodule's diff (which a
 * superproject's show or log -p runs as a git of its own, without these
 * options), and path names as they are, not quoted, unless they have a
 * '"', a '\' or a control character.
 */
static char const *const fixed[] = {
	GIT_OPTIONS_FIXED,
	"-c", "core.bare=true",
	"-c", "log.showSignature=false",
	"-c", "diff.submodule=short",
	"-c", "diff.autoRefreshIndex=false",
	"-c", "core.quotePath=false",
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
static struct cstr_view const read_options[] = { {} };
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
	LINES   = 1 << 7,  /* the line and count arguments, and numbered lines
	                      of one committed file that cat-file reads */
};

static struct {
	struct cstr_view        name;
	struct cstr_view const *options;
	char const             *forced[3];  /* options askai gives it */
	char const             *description;
	uint64_t                takes;
} const commands[] = {
	{ CSTR_VIEW_INIT("read"),      read_options,      { "blob" },
	  "Numbered lines of a committed file. Give the file as the one revision, "
	  "commit:path such as HEAD:src/main.c, the first line as line (default 1) "
	  "and the most lines as count (default: as many as the window holds). The "
	  "output's first line names the commit; its last line says where to read "
	  "on.", END | OBJECT | SINGLE | LINES },
	{ CSTR_VIEW_INIT("log"),       log_options,       { "--no-ext-diff", "--no-textconv" },
	  "Committed history; defaults to HEAD. Paths restrict the history.",
	  FILES | END | RANGE },
	{ CSTR_VIEW_INIT("show"),      show_options,      { "--no-ext-diff", "--no-textconv" },
	  "Committed contents; defaults to HEAD. HEAD:path reads a committed file.",
	  FILES | END | OBJECT },
	{ CSTR_VIEW_INIT("diff"),      diff_options,      { "--no-ext-diff", "--no-textconv" },
	  "Compare two commits: give two revisions, or one explicit A..B or A...B "
	  "range. Paths restrict the comparison.", FILES | END | RANGE | PAIR },
	{ CSTR_VIEW_INIT("grep"),      grep_options,      { "--no-textconv", "-n" },
	  "Search committed trees; defaults to HEAD. Each match is "
	  "commit:path:line:text; read takes commit:path as its revision and that "
	  "line. Use -F for literal search, -E for extended regex, -C3 for context.",
	  FILES | END | PATTERN },
	{ CSTR_VIEW_INIT("blame"),     blame_options,     { "--no-textconv" },
	  "Annotate a committed file; defaults to HEAD. Give one path; -L10,30 "
	  "limits the line range.", FILES | SINGLE },
	{ CSTR_VIEW_INIT("ls-tree"),   ls_tree_options,   {},
	  "Paths in a committed tree; defaults to HEAD. -r recurses.",
	  FILES | SINGLE },
	{ CSTR_VIEW_INIT("cat-file"),  cat_file_options,  {},
	  "Inspect committed contents: -p for contents, -t for type, -s for bytes, "
	  "-e for existence. Defaults to HEAD; revision:path names a committed file.",
	  END | OBJECT | SINGLE },
	{ CSTR_VIEW_INIT("rev-parse"), rev_parse_options, { "--revs-only" },
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

/* Sets up @p r for the repository at @p path. The function stores the
 * name, the real path and the safe.directory setting of the repository in
 * one allocation.
 */
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
	size_t size = strlen(real) + 1;
	char *s = malloc(2 * size + sizeof safe - 1 + len + 1);
	if (!s) {
		free(real);
		real = nullptr;
		return ENOMEM;
	}
	r->path = s;
	r->safe = mempcpy(s, real, size);
	char *copy = mempcpy(mempcpy(r->safe, safe, sizeof safe - 1), real, size);
	free(real);
	real = nullptr;
	*(char *)mempcpy(copy, name, len) = '\0';
	r->name = (struct cstr_view){ copy, len };
	return 0;
}

/* GIT_CEILING_DIRECTORIES: the listed repositories' parents, so that git
 * finds no repository above one listed, as it would look for one without
 * one there; nullptr if out of memory.
 */
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
	char *const vars[] = {
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
		free(git->repo[i].path);
	free(git->env[CEILINGS]);
	free(git);
}

/* The schema of an argument that is at most @p max strings. */
static struct json_object *
git_strings (char const *description,
             size_t      max)
{
	return jb_object(
		"type", json_object_new_string("array"),
		"items", schema_property("string", nullptr),
		"maxItems", json_object_new_int((int32_t)max),
		"description", json_object_new_string(description),
		nullptr);
}

/* Returns the definition of the git tool. @p service is the struct git,
 * and the definition lists the names of its repositories. Returns nullptr
 * if out of memory.
 */
static struct json_object *
git_definition (void const *service)
{
	struct git const *git = service;
	static_assert(OPTIONS == 16 && REVISIONS == 4 && PATHS == 32 &&
	              GIT_MAX == 4 << 20, "the description says");
	struct json_object *repos = json_object_new_array();
	for (size_t i = 0; repos && i < git->n; ++i)
		repos = jb_push_or_free(repos, jb_string(git->repo[i].name));
	struct json_object *cmds = json_object_new_array();
	for (size_t i = 0; cmds && i < ARRAY_SIZE(commands); ++i)
		cmds = jb_push_or_free(cmds, jb_string(commands[i].name));
	struct cstr_view const required[] = { CSTR_VIEW("repo"), CSTR_VIEW("command") };
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
			"To read a file, use read with the file as the revision: "
			"{\"command\": \"read\", \"revisions\": [\"HEAD:src/main.c\"], "
			"\"line\": 120, \"count\": 40} gives lines 120 to 159 with their "
			"numbers and names the commit. A grep match "
			"<commit>:<path>:<line>:<text> reads as \"revisions\": "
			"[\"<commit>:<path>\"] and \"line\": <line>. Any other commit "
			"can replace HEAD. Give each option with its value attached "
			"(--max-count=20, not -n 20). Only the usual read-only options are "
			"allowed."),
		"inputSchema", schema_object(
			jb_object(
				"repo", schema_enum(repos, nullptr),
				"command", schema_enum(cmds, nullptr),
				"help", schema_property("boolean",
					"Describe this command without running it. "
					"Give repo, command, help:true and optional offset."),
				"options", git_strings("Options, such as --oneline or "
				                       "--max-count=20. Use help:true to list "
				                       "the command's exact allowed options.", OPTIONS),
				"revisions", git_strings("Revisions, such as HEAD~3, main..topic "
				                         "or the hash of a commit. For read, show "
				                         "and cat-file, HEAD:path names a file as "
				                         "of that commit. References and hashes "
				                         "may use ^/~ ancestry and ^{commit}. Reflogs, "
				                         "stash, index and raw blob/tree IDs are "
				                         "excluded. The resolved commit must belong "
				                         "to HEAD, branch, tag or remote-tracking "
				                         "history.", REVISIONS),
				"paths", git_strings("Paths in the repository, relative to its "
				                     "top. They limit the output of a command "
				                     "such as log or diff to those paths. They "
				                     "do not read a file.", PATHS),
				"pattern", schema_property("string",
					"The pattern for grep. Only grep takes a pattern."),
				"offset", schema_count(0,
					"The byte of the output to start from, to read on "
					"past where it was cut."),
				"line", schema_count(1,
					"For read: the number of the first line to give, "
					"from 1. The default is 1."),
				"count", schema_count(1,
					"For read: the most lines to give. The default is "
					"as many as the response window holds."),
				nullptr),
			schema_names(required, ARRAY_SIZE(required))),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(1),
			"openWorldHint", json_object_new_boolean(0),
			nullptr),
		nullptr);
}

/* Returns a view of the string @p val if it is 1 to WORD bytes long and
 * contains no NUL, or else an empty view with data nullptr. The bytes of
 * the view are followed by the NUL that json-c stores after each string.
 */
static struct cstr_view
git_word (struct json_object *val)
{
	struct cstr_view const s = jb_view(val);
	return s.len && s.len <= WORD && !memchr(s.data, '\0', s.len) ? s : (struct cstr_view){};
}

/** Looks up the option @p opt, a word of git_word(), in @p allowed, the
 * spellings that help lists. Stores the spelling that matches in @p *match
 * and returns 0, ENODATA for a missing value, EINVAL for nondecimal
 * digits, or ENOENT if no spelling matches. A spelling that accepts @p opt
 * is preferred over one whose value is malformed, such as --abbrev beside
 * --abbrev=.
 */
static int
git_option_find (struct cstr_view const *allowed,
                 struct cstr_view        opt,
                 struct cstr_view       *match)
{
	*match = (struct cstr_view){};
	int e = ENOENT;
	for (; allowed->data; ++allowed) {
		size_t n = allowed->len;
		char last = allowed->data[n - 1];
		if (last != '=' && last != '#' && last != '*') {
			if (cstr_view_equal(opt, *allowed)) {
				*match = *allowed;
				return 0;
			}
			continue;
		}
		if (!cstr_view_starts(opt, cstr_view_slice(*allowed, 0, n - 1)) ||
		    (last == '=' && opt.len >= n && opt.data[n - 1] != '='))
			continue;
		size_t at = last == '=' ? n : n - 1;
		*match = *allowed;
		if (opt.len <= at)
			e = ENODATA;
		else if (last == '#' && strspn(opt.data + at, "0123456789") != opt.len - at)
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
 * returns EINVAL.
 */
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
 * wrong, after @p head and before @p tail; returns EINVAL.
 */
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

/** Validates the option @p opt of the command @p command, the option at
 * @p index in the options argument. Returns 0, or EINVAL with a message in
 * @p why that explains only how to repair the option.
 */
static int
git_option_check (struct cstr_view  opt,
                  size_t            command,
                  size_t            index,
                  char             *why)
{
	struct cstr_view match;
	int e = git_option_find(commands[command].options, opt, &match);
	if (!e)
		return 0;
	int quoted = (int)jb_utf8(opt.data, opt.len < 80 ? opt.len : 80);
	if (match.data) {
		char last = match.data[match.len - 1];
		size_t at = match.len - (last != '=');
		char const *value = last == '#' ? "NUMBER" : "VALUE";
		char const *digits = last == '#' ? " containing decimal digits" : "";
		return git_wrong(why, "options[%zu]: '%.*s' %s. Use '%.*s%s' as one "
		                 "options entry%s.", index, quoted, opt.data,
		                 e == ENODATA ? "needs an attached value"
		                              : "has a nonnumeric attached value",
		                 (int)at, match.data, value, digits);
	}
	if (!git_option_find(source_options, opt, &match)) {
		char const *source = commands[command].takes & PAIR
			? "supply two commits or one A..B/A...B range in revisions."
			: "select a commit with revisions (default HEAD).";
		return git_wrong(why, "options[%zu]: '%.*s' requests an unavailable data "
		                 "source. Index and working-tree reads are disabled. "
		                 "Omit this option and %s", index, quoted, opt.data, source);
	}
	return git_wrong(why, "options[%zu]: '%.*s' is not available for git %.*s in "
	                 "this tool. Omit it; use help:true for this command's options.",
	                 index, quoted, opt.data, (int)commands[command].name.len,
	                 commands[command].name.data);
}

/* Returns the repository whose name is @p name, or nullptr if there is
 * none.
 */
static struct git_repo const *
git_find (struct git const *git,
          struct cstr_view  name)
{
	for (size_t i = 0; i < git->n; ++i)
		if (cstr_view_equal(name, git->repo[i].name))
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
	/* After util_format() cuts the text short and ends it with "...", the
	 * loop appends nothing more. */
	for (size_t i = 0; i < git->n && n < GIT_WHY - 1; ++i)
		n += util_format(why + n, GIT_WHY - n, "%s %.*s", i ? "," : "",
		                 (int)git->repo[i].name.len, git->repo[i].name.data);
	if (n < GIT_WHY - 1)
		util_format(why + n, GIT_WHY - n, ".");
	return EINVAL;
}

/* Whether @p key is an argument the tool takes. */
static bool
git_known (char const *key)
{
	static char const *const keys[] = {
		"repo", "command", "help", "options", "revisions", "paths", "pattern", "offset",
		"line", "count"
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

	struct git_repo const *repo = git_find(git, git_word(jb_get(args, "repo")));
	if (!repo)
		return git_repos_wrong(git, why);
	call->repo = repo;
	struct cstr_view const cmd = git_word(jb_get(args, "command"));
	size_t c = 0;
	while (c < ARRAY_SIZE(commands) && !cstr_view_equal(cmd, commands[c].name))
		++c;
	if (c == ARRAY_SIZE(commands)) {
		if (cstr_view_equal(cmd, CSTR_VIEW("status")) ||
		    cstr_view_equal(cmd, CSTR_VIEW("ls-files")))
			return git_wrong(why, "git %.*s is unavailable: index and working-tree "
			                 "reads are disabled. Use git ls-tree with "
			                 "options [\"-r\", \"--name-only\"] to list committed "
			                 "files; revisions defaults to HEAD.",
			                 (int)cmd.len, cmd.data);
		size_t n = util_format(why, GIT_WHY, "The command argument must be one of:");
		for (size_t i = 0; i < ARRAY_SIZE(commands) && n < GIT_WHY - 1; ++i)
			n += util_format(why + n, GIT_WHY - n, "%s %.*s", i ? "," : "",
			                 (int)commands[i].name.len, commands[i].name.data);
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
			call->phase = GIT_HELP;
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
			if (!git_word(json_object_array_get_idx(lists[l], i)).data)
				return git_wrong(why, "%s[%zu] must be a string of 1 to %d "
				                 "bytes with no NUL.", names[l], i, WORD);
	}
	if ((takes & SINGLE) && lens[1] > 1)
		return git_wrong(why, "git %.*s takes at most one revision.", (int)cmd.len,
		                 cmd.data);
	if (lens[2] && !(takes & FILES))
		return git_wrong(why, "git %.*s takes no paths. To name a file, give the "
		                 "revision HEAD:path.", (int)cmd.len, cmd.data);
	struct json_object *pattern = jb_get(args, "pattern");
	struct cstr_view const pat = git_word(pattern);
	if (takes & PATTERN) {
		if (!pat.data)
			return git_wrong(why, "git grep needs a pattern argument: a string of "
			                 "1 to 1024 bytes.");
	} else if (pattern) {
		return git_wrong(why, "Only git grep takes a pattern.");
	}
	static char const *const numbers[2] = { "line", "count" };
	size_t *const values[2] = { &call->read.line, &call->read.count };
	call->read.line = 1;
	call->read.count = SIZE_MAX;
	for (size_t i = 0; i < 2; ++i) {
		struct json_object *val;
		if (!json_object_object_get_ex(args, numbers[i], &val))
			continue;
		if (!(takes & LINES))
			return git_wrong(why, "Only git read takes line and count.");
		int64_t v = jb_int(val, 0);
		if (v < 1)
			return git_wrong(why, "The %s argument must be a whole number from 1.",
			                 numbers[i]);
		/* Where size_t is narrower than 64 bits, a larger value becomes
		 * SIZE_MAX. That line is past the end of any file, and that count
		 * sets no limit. */
		*values[i] = (uint64_t)v < SIZE_MAX ? (size_t)v : SIZE_MAX;
	}

	size_t k = 0;
	char const **argv = call->argv;
	argv[k++] = takes & LINES ? "cat-file" : cmd.data;
	for (size_t i = 0; i < ARRAY_SIZE(commands[c].forced) && commands[c].forced[i];
	     ++i)
		argv[k++] = commands[c].forced[i];
	/* The options, then the pattern, the revisions and the paths, each
	 * checked as it goes in. */
	for (size_t i = 0; i < lens[0]; ++i) {
		struct cstr_view const opt = jb_view(json_object_array_get_idx(lists[0], i));
		int e = git_option_check(opt, c, i, why);
		if (e)
			return e;
		if (strncmp(opt.data, git_ignore_rev, sizeof git_ignore_rev - 1)) {
			argv[k++] = opt.data;
			continue;
		}
		struct git_root *root = &call->roots[call->n_roots++];
		if (git_commit(root->name, opt.data + sizeof git_ignore_rev - 1,
		               opt.len - (sizeof git_ignore_rev - 1)))
			return git_wrong(why, "--ignore-rev needs a commit reference or hash.");
		root->option[0] = '-';
		argv[k++] = root->option;
	}
	if (pat.data) {
		argv[k++] = "-e";
		argv[k++] = pat.data;
	}
	if (takes & END)
		argv[k++] = "--end-of-options";
	size_t endpoints = lens[1];
	call->n_revisions = lens[1] ? lens[1] : 1;
	for (size_t i = 0; i < call->n_revisions; ++i) {
		struct cstr_view const s = lens[1] ? jb_view(json_object_array_get_idx(lists[1], i))
		                                   : CSTR_VIEW("HEAD");
		char const *path = strchr(s.data, ':');
		size_t root = path ? (size_t)(path - s.data) : s.len;
		if (path && (!(takes & OBJECT) || !path[1] || !git_inside(path + 1)))
			return git_wrong(why, "revisions[%zu]: only read, show and cat-file "
			                 "take commit:path, with a nonempty relative path.", i);
		char const *range = strstr(s.data, "..");
		if (range && range >= s.data + root)
			range = nullptr;
		if (range && (!(takes & RANGE) || ((takes & PAIR) && lens[1] != 1)))
			return git_wrong(why, "revisions[%zu]: ranges are allowed only by "
			                 "log, or as diff's single A..B or A...B argument.", i);
		size_t left = range ? (size_t)(range - s.data) : root;
		struct git_revision *rev = &call->revisions[i];
		rev->root = call->n_roots;
		rev->path = cstr_view_slice(s, root, SIZE_MAX);
		int e = git_commit(call->roots[call->n_roots++].name, s.data, left);
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
	if ((takes & LINES) && !call->revisions[0].path.len)
		return git_wrong(why, "git read needs the file as its revision, such as "
		                 "\"revisions\": [\"HEAD:src/main.c\"].");
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
	for (struct cstr_view const *o = commands[command].options; options && o->data; ++o)
		options = jb_push_or_free(options, jb_string(*o));
	uint64_t takes = commands[command].takes;
	struct json_object *help = jb_object(
		"command", jb_string(commands[command].name),
		"description", json_object_new_string(commands[command].description),
		"options", options,
		"option_values", json_object_new_string(
			"Each option is one array entry. Append a value after a trailing =; "
			"replace trailing # with digits or * with a value. For example: "
			"--max-count=20, -n20, -C3. Do not split an option and its value."),
		"pagination", json_object_new_string(takes & LINES
			? "The capture limit is 4 MiB. read gives whole lines, each after "
			  "its number and a tab, as many as count asks and window_bytes "
			  "holds. The last line of the output gives the revision and line "
			  "to read on from. Only a line longer than the window is cut, and "
			  "then its footer gives the next offset."
			: "The capture limit is 4 MiB. The response window is window_bytes; "
			  "a longer output ends with a footer that gives the exact next "
			  "offset for the same arguments."),
		"limits", jb_object(
			"options", json_object_new_int(OPTIONS),
			"revisions", json_object_new_int(takes & SINGLE ? 1 :
			                                takes & PAIR ? 2 : REVISIONS),
			"paths", json_object_new_int(takes & FILES ? PATHS : 0),
			"argument_bytes", json_object_new_int(WORD),
			"capture_bytes", json_object_new_int(GIT_MAX),
			"window_bytes", json_object_new_int64((int64_t)mcp_slot_window_max(slot)),
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
	int e = text ? mcp_slot_write(slot, (struct cstr_view){ text, len }, 0, &taken)
	             : ENOMEM;
	json_object_put(help);
	if (e)
		mcp_slot_fail(slot, e);
	else
		mcp_slot_finish(slot, 0);
}

/* A git call's git has ended, its output all in: take it in steps of the
 * call's own.
 */
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

/* Destroy the git call of @p slot and clear its own word. This kills git
 * if it still runs.
 */
static void
git_drop (struct mcp_slot *slot)
{
	struct git_call *call = *mcp_slot_own(slot);
	git_call_destroy(&call);
	*mcp_slot_own(slot) = nullptr;
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
		env[ATTR] = "GIT_ATTR_SOURCE=HEAD";
	struct child_cmd const cmd = {
		.file = "git",
		.argv = argv,
		.envp = env,
		.max = GIT_MAX,
		.deadline = call->deadline,
		.grace = GIT_GRACE_NS,
		/* Git can warn on stderr, for example when a branch has the same
		 * name as a commit ID. A read discards stderr so that a warning
		 * does not become a line of the file. */
		.flags = call->phase == GIT_RUN && !(commands[call->command].takes & LINES)
		         ? CHILD_STDERR : 0,
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
	call->read.oid_len = width;
	for (size_t i = 0; i < call->n_revisions; ++i) {
		struct git_revision *rev = &call->revisions[i];
		char const *first = call->roots[rev->root].name;
		rev->len = rev->dots
		         ? util_format(rev->text, sizeof rev->text, "%s%.*s%s", first,
		                       (int)rev->dots, "...", call->roots[rev->root + 1].name)
		         : util_format(rev->text, sizeof rev->text, "%s%.*s", first,
		                       (int)rev->path.len, rev->path.data);
	}
	return 0;
}

/** Handles the output of an internal phase of @p call and starts the next
 *  phase. The output of the discovery and authorization phases never goes
 *  to the caller.
 */
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
		case GIT_NUMBER:
		case GIT_HELP:
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
	git_drop(slot);
	mcp_slot_tell(slot, late ? CSTR_VIEW("Git history validation timed out.")
	                         : CSTR_VIEW("Cannot read the requested committed history. "
	                                     "Use commits reachable from HEAD, branches, "
	                                     "tags or remote-tracking refs."), TOOL_FAILED);
}

/* Returns true if @p path contains a ':' and digits after it that end the
 * path or are followed by another ':', as in a grep match with its line
 * number.
 */
static bool
git_line_hint (struct cstr_view path)
{
	char const *end = path.data + path.len;
	for (char const *p = path.data; (p = memchr(p, ':', (size_t)(end - p))); ++p) {
		uint64_t line;
		size_t n = util_digits(p + 1, (size_t)(end - p - 1), UINT64_MAX, &line);
		if (n && (p + 1 + n == end || p[1 + n] == ':'))
			return true;
	}
	return false;
}

/* Destroys @p call after git has ended and its output is copied. If git
 * failed, if the output was cut, or if there was no output, the function
 * then writes a note about it after the output. Then it finishes the call.
 * Exit status 1 from grep means no matches.
 */
static void
git_ended (struct mcp_slot *slot,
           struct git_call *call)
{
	static_assert(GIT_MAX == 4 << 20, "the note says 4 MiB");
	uint64_t const takes = commands[call->command].takes;
	struct child *c = call->child;
	struct cstr_view out = mcp_slot_answer(slot);
	char buf[GIT_NOTE - 2];  /* the blank line before the note is 2 bytes at most */
	struct cstr_view note;
	bool failed = true;
	if (c->late) {
		struct git_loop const *gl = mcp_slot_part(slot);
		note = util_format_view(buf, sizeof buf, "[git took longer than %.0f s, and was "
		                        "stopped.]", (double)gl->git->timeout / 1e9);
	} else if (c->cut) {
		note = CSTR_VIEW("[The output goes on past the 4 MiB shown.]");
		failed = c->code == CLD_EXITED && c->status;
	} else if (c->code != CLD_EXITED) {
		note = util_format_view(buf, sizeof buf, "[git was killed by signal %d.]",
		                        c->status);
	} else if (!c->status) {
		note = out.len ? (struct cstr_view){} : CSTR_VIEW("[No output.]");
		failed = false;
	} else if (c->status == 1 && (takes & PATTERN)) {
		note = CSTR_VIEW("[No matches.]");
		failed = false;
	} else if (takes & LINES) {
		static char const fmt[] = "[No file at that path in commit %s. ls-tree "
		                          "lists the files there.%s]";
		static char const hint[] = " If :N after the file name is a line number, "
		                           "give the file alone as the revision and "
		                           "\"line\": N.";
		static_assert(sizeof fmt - 4 + 64 + sizeof hint - 1 <= sizeof buf,
		              "the note fits");
		struct git_revision const *rev = &call->revisions[0];
		note = util_format_view(buf, sizeof buf, fmt, call->roots[rev->root].name,
		                        git_line_hint(cstr_view_slice(rev->path, 1, SIZE_MAX))
		                        ? hint : "");
	} else {
		note = util_format_view(buf, sizeof buf, takes & OBJECT
		                        ? "[The committed object could not be read (git "
		                          "exited with status %d).]"
		                        : "[git exited with status %d.]", c->status);
	}
	git_drop(slot);
	struct cstr_view const p[] = {
		!out.len ? CSTR_VIEW("") : out.data[out.len - 1] == '\n' ? CSTR_VIEW("\n")
		                                                         : CSTR_VIEW("\n\n"),
		note,
	};
	int e = 0;
	size_t taken;
	for (size_t i = note.data ? 0 : ARRAY_SIZE(p); i < ARRAY_SIZE(p) && !e; ++i)
		e = mcp_slot_write(slot, p[i], 0, &taken);
	if (e) {
		mcp_slot_fail(slot, e);
		return;
	}
	mcp_slot_finish(slot, failed ? TOOL_FAILED : 0);
}

/* Write @p n in decimal into the bytes before @p end. Return the text from
 * the first digit to @p end, and @p tail bytes more.
 */
static struct cstr_view
git_digits (char   *end,
            size_t  n,
            size_t  tail)
{
	char *p = util_decimal(end, n);
	return (struct cstr_view){ p, (size_t)(end - p) + tail };
}

/* Write the @p n texts at @p p to the output of @p slot if the step has
 * LOOP_COPY room for all of them. Return 0, EAGAIN if the room is too
 * small, or the error of mcp_slot_write().
 */
static int
git_write (struct mcp_slot        *slot,
           struct cstr_view const *p,
           size_t                  n)
{
	size_t len = 0;
	for (size_t i = 0; i < n; ++i)
		len += p[i].len;
	if (loop_room(mcp_slot_loop(slot), LOOP_COPY) < len)
		return EAGAIN;
	int e = 0;
	size_t taken;
	for (size_t i = 0; i < n && !e; ++i)
		e = mcp_slot_write(slot, p[i], 0, &taken);
	return e;
}

/* Begins @p read of @p rev, which is commit:path with a commit ID of
 * read->oid_len bytes, in the repository @p repo. read->room is the rest
 * of the @p window bytes after the longest header and footer.
 */
static void
git_read_begin (struct git_read       *read,
                struct cstr_view       rev,
                struct git_repo const *repo,
                size_t                 window)
{
	read->rev_len = jb_escape(read->rev, rev.data, rev.len);
	/* The header contains the repository name, the commit ID and path from
	 * read->rev, and three numbers. The longest footer contains read->rev
	 * and one number. A number is at most 20 digits long. */
	size_t reserve = sizeof "[Repository , file  at commit : lines  to  of at least .]\n" +
	                 sizeof "\n\n[To read on, call again with \"revisions\": "
	                        "[\"\"] and \"line\": .]" +
	                 repo->name.len + 2 * read->rev_len + 4 * 20;
	read->room = window > reserve ? window - reserve : 0;
	read->last = read->line - 1;
}

/* Count the line of the file that ends at @p end, with a newline if
 * @p newline is true. Take the line if it is asked for, if it follows the
 * last line taken, and if its number, a tab, its bytes and a newline fit
 * in read->room. The first line asked for is always taken.
 */
static void
git_read_line (struct git_read *read,
               size_t           end,
               bool             newline)
{
	size_t n = ++read->lines;
	if (n == read->line)
		read->from = read->start;
	char digits[20];
	size_t size = n == read->last + 1 && n - read->line < read->count
	              ? git_digits(digits + sizeof digits, n, 1).len + end - read->start +
	                !newline
	              : SIZE_MAX;
	if (n == read->line || size <= read->room) {
		read->room -= size < read->room ? size : read->room;
		read->to = end;
		read->last = n;
	}
	read->start = end;
}

/* Count the lines of the file @p blob from read->at on, as far as the
 * LOOP_PARSE room of the step on @p loop goes.
 */
static void
git_read_scan (struct git_read *read,
               struct span      blob,
               struct loop     *loop)
{
	size_t room = loop_room(loop, LOOP_PARSE);
	size_t end = blob.len - read->at < room ? blob.len : read->at + room;
	loop_work(loop, LOOP_PARSE, end - read->at);
	while (read->at < end) {
		unsigned char const *nl = memchr(blob.ptr + read->at, '\n', end - read->at);
		read->at = nl ? (size_t)(nl - blob.ptr) + 1 : end;
		if (nl)
			git_read_line(read, read->at, true);
	}
	/* Count a last line without a newline too, also when the cut ends it. */
	if (read->at == blob.len && read->start < blob.len)
		git_read_line(read, blob.len, false);
}

/* Write into @p why, a buffer of GIT_WHY bytes, the refusal of a read whose
 * first line is past the end of the file, or past the end of the capture if
 * @p cut is true.
 */
static void
git_read_past (struct git_read const *read,
               bool                   cut,
               char                  *why)
{
	if (!read->total)
		git_wrong(why, "Line %zu is past the end of the file: the file is empty.",
		          read->line);
	else if (cut)
		git_wrong(why, "Line %zu is past the first 4 MiB of the file, which is as "
		          "far as the git tool reads.", read->line);
	else
		git_wrong(why, "Line %zu is past the end of the file: it has %zu line%s. "
		          "Give a line from 1 to %zu.", read->line, read->total,
		          read->total == 1 ? "" : "s", read->total);
}

/* Writes the header line to @p slot if it fits in the LOOP_COPY room of the
 * step. If @p cut is true, the line total is a lower bound ("at least").
 * Returns 0, EAGAIN or the error of mcp_slot_write().
 */
static int
git_read_head (struct git_read const *read,
               struct mcp_slot       *slot,
               struct git_repo const *repo,
               bool                   cut)
{
	char lines[sizeof ": lines  to  of at least .]\n" + 3 * 20];
	struct cstr_view const p[] = {
		CSTR_VIEW("[Repository "), repo->name, CSTR_VIEW(", file "),
		{ read->rev + read->oid_len + 1, read->rev_len - read->oid_len - 1 },
		CSTR_VIEW(" at commit "), { read->rev, read->oid_len },
		read->total ? util_format_view(lines, sizeof lines, ": lines %zu to %zu of "
		                               "%s%zu.]\n", read->line, read->last,
		                               cut ? "at least " : "", read->total)
		            : CSTR_VIEW(": the file is empty.]"),
	};
	return git_write(slot, p, ARRAY_SIZE(p));
}

/* Write the lines of the file @p blob from read->at on to @p slot, each
 * after its number and a tab, as far as the LOOP_COPY room of the step
 * goes. Each line counts LINE_COST in addition to its bytes, so that a
 * step writes a bounded number of lines. Return 0 once all lines are
 * written, EAGAIN, or the error of mcp_slot_write().
 */
static int
git_read_lines (struct git_read *read,
                struct mcp_slot *slot,
                struct span      blob)
{
	struct loop *loop = mcp_slot_loop(slot);
	char digits[21];
	char *tab = digits + sizeof digits - 1;
	*tab = '\t';
	while (read->at < read->to) {
		size_t room = loop_room(loop, LOOP_COPY);
		struct cstr_view num = read->at == read->start
		                       ? git_digits(tab, read->lines + 1, 1)
		                       : CSTR_VIEW("");
		/* Write a piece only if the number, LINE_COST and 4 bytes fit in
		 * the room of the step. The piece then contains at least 4 bytes of
		 * the line, or all of a shorter line. With TOOL_MORE,
		 * mcp_slot_write() leaves out at most 3 bytes of an incomplete
		 * character, so it writes at least one byte, and the next step goes
		 * on inside the line without writing the number again. LINE_COST
		 * is counted when the line ends. */
		size_t fixed = num.len + LINE_COST;
		if (room < fixed + 4)
			return EAGAIN;
		if (num.len)
			read->mark = mcp_slot_answer(slot).len;
		size_t left = read->to - read->at;
		size_t n = left < room - fixed ? left : room - fixed;
		unsigned char const *nl = memchr(blob.ptr + read->at, '\n', n);
		if (nl)
			n = (size_t)(nl - blob.ptr) + 1 - read->at;
		bool ends = nl || n == left;
		size_t taken;
		int e = mcp_slot_write(slot, num, 0, &taken);
		if (!e)
			e = mcp_slot_write(slot, (struct cstr_view){ (char const *)blob.ptr +
			                                             read->at, n },
			                   ends ? 0 : TOOL_MORE, &taken);
		if (e)
			return e;
		read->at += taken;
		if (!ends)
			return EAGAIN;
		loop_work(loop, LOOP_COPY, LINE_COST);
		read->start = read->at;
		++read->lines;
	}
	return 0;
}

/* Writes the footer to @p slot if it fits in the LOOP_COPY room of the
 * step. The footer consists of a newline that ends the last line if the
 * line does not end in one, a blank line, and a note. The note contains
 * the revision and the line to read on from, or the number of the last
 * line of the file. If @p cut is true, the note instead contains the
 * number of the line after or inside which the 4 MiB capture ends. The
 * function writes no footer for an empty file. Returns 0, EAGAIN or the
 * error of mcp_slot_write().
 */
static int
git_read_foot (struct git_read const *read,
               struct mcp_slot       *slot,
               struct span            blob,
               bool                   cut)
{
	if (!read->total)
		return 0;
	/* Each note is the text before the revision, the revision, the text
	 * before the line number, and the text after it. */
	struct cstr_view const notes[][4] = {
		{ CSTR_VIEW("[To read on, call again with \"revisions\": [\""),
		  { read->rev, read->rev_len }, CSTR_VIEW("\"] and \"line\": "), CSTR_VIEW(".]") },
		{ CSTR_VIEW("[Line "), {}, {}, CSTR_VIEW(" is the end of the file.]") },
		{ CSTR_VIEW("[Line "), {}, {},
		  CSTR_VIEW(" is the end of the file, which has no newline after it.]") },
		{ CSTR_VIEW("[The file goes on after line "), {}, {},
		  CSTR_VIEW(", past the 4 MiB that the git tool reads.]") },
		{ CSTR_VIEW("[Line "), {}, {},
		  CSTR_VIEW(" goes on past the 4 MiB that the git tool reads.]") },
	};
	bool newline = blob.ptr[read->to - 1] == '\n';
	size_t kind = read->last < read->total ? 0 : 1 + 2 * cut + !newline;
	struct cstr_view const *note = notes[kind];
	char n[20];
	struct cstr_view const p[] = {
		newline ? CSTR_VIEW("\n") : CSTR_VIEW("\n\n"),
		note[0], note[1], note[2],
		git_digits(n + sizeof n, read->last + !kind, 0),
		note[3],
	};
	return git_write(slot, p, ARRAY_SIZE(p));
}

/* Pass mcp_slot_source() the numbered lines of the read in @p call, from
 * read->head to @p end in the output, for the review record. If the 4 MiB
 * cut ends inside the last line, that line is left out. total is 0
 * (unknown) when the output was cut. Return 0 or the error of
 * mcp_slot_source().
 */
static int
git_read_shown (struct git_read const *read,
                struct mcp_slot       *slot,
                struct git_call const *call,
                size_t                 end)
{
	struct child const *c = call->child;
	bool part = read->last >= read->line && c->cut &&
	            buf_peek(&c->out).ptr[read->to - 1] != '\n';
	if (read->last - part < read->line)
		return 0;
	struct git_revision const *rev = &call->revisions[0];
	struct review_source const src = {
		.repo   = call->repo->name,
		.commit = { call->roots[rev->root].name, read->oid_len },
		.path   = cstr_view_slice(rev->path, 1, SIZE_MAX),
		.at     = read->head,
		.len    = (part ? read->mark : end) - read->head,
		.first  = read->line,
		.last   = read->last - part,
		.total  = c->cut ? 0 : read->total,
	};
	return mcp_slot_source(slot, &src);
}

/* Scans the file of the read in @p call, as far as the room of the step
 * goes, after a check that its first BINARY bytes contain no NUL. When the
 * scan ends, the function numbers the lines in the next step, or refuses
 * the read if the first line asked for is not in the file.
 */
static void
git_scan (struct mcp_slot *slot,
          struct git_call *call)
{
	struct git_read *read = &call->read;
	struct child *c = call->child;
	struct span blob = buf_peek(&c->out);
	if (!read->rev_len) {
		size_t head = blob.len < BINARY ? blob.len : BINARY;
		if (head && memchr(blob.ptr, '\0', head)) {
			git_drop(slot);
			mcp_slot_tell(slot, CSTR_VIEW("The file has a NUL byte, so it is "
			                              "binary, and read gives text only. "
			                              "cat-file with -s gives its size."),
			              TOOL_FAILED);
			return;
		}
		struct git_revision const *rev = &call->revisions[0];
		git_read_begin(read, (struct cstr_view){ rev->text, rev->len }, call->repo,
		               mcp_slot_window_max(slot));
	}
	git_read_scan(read, blob, c->loop);
	if (read->start < blob.len) {
		mcp_slot_ready(slot);
		return;
	}
	read->total = read->lines;
	if (read->line > (read->total ? read->total : 1)) {
		char why[GIT_WHY];
		git_read_past(read, c->cut, why);
		git_drop(slot);
		mcp_slot_tell(slot, cstr_view(why), TOOL_FAILED);
		return;
	}
	read->at = read->start = read->from;
	read->lines = read->line - 1;
	call->phase = GIT_NUMBER;
	mcp_slot_ready(slot);
}

/* Write the header, the lines and the footer of the read in @p call, as
 * far as the room of each step goes. Then pass the shown lines to
 * mcp_slot_source() (git_read_shown()) and finish the call.
 */
static void
git_number (struct mcp_slot *slot,
            struct git_call *call)
{
	struct git_read *read = &call->read;
	struct child *c = call->child;
	struct span blob = buf_peek(&c->out);
	int e = 0;
	if (!read->head) {
		e = git_read_head(read, slot, call->repo, c->cut);
		read->head = mcp_slot_answer(slot).len;
	}
	if (!e)
		e = git_read_lines(read, slot, blob);
	size_t end = mcp_slot_answer(slot).len;
	if (!e)
		e = git_read_foot(read, slot, blob, c->cut);
	if (!e)
		e = git_read_shown(read, slot, call, end);
	if (e == EAGAIN) {
		mcp_slot_ready(slot);
		return;
	}
	if (e) {
		mcp_slot_fail(slot, e);
		return;
	}
	git_drop(slot);
	mcp_slot_finish(slot, 0);
}

/* Copy the output of git to the output of the call in @p slot, as far as
 * the LOOP_COPY room of the step goes, and make it UTF-8 text. A character
 * that the end of the room cuts is left to the next step. When all of the
 * output is copied, end the call, in the next step if the room left is too
 * small for the note. A read numbers the lines of its file instead.
 */
static void
git_step (struct mcp_slot *slot)
{
	struct git_call *call = *mcp_slot_own(slot);
	if (call->phase < GIT_RUN) {
		git_call_advance(call, slot);
		return;
	}
	struct child *c = call->child;
	uint64_t takes = commands[call->command].takes;
	/* Git's failed object lookup may disclose whether a path exists on disk. */
	if ((takes & OBJECT) && c->code == CLD_EXITED && c->status) {
		git_ended(slot, call);
		return;
	}
	/* When the output goes past 4 MiB, git is killed. A read then writes
	 * the lines before the cut. */
	if (takes & LINES) {
		if (c->late || (!c->cut && c->code != CLD_EXITED))
			git_ended(slot, call);
		else if (call->phase == GIT_RUN)
			git_scan(slot, call);
		else
			git_number(slot, call);
		return;
	}
	struct span left = buf_peek(&c->out);
	size_t room = loop_room(c->loop, LOOP_COPY);
	size_t n = left.len < room ? left.len : room;
	int e = mcp_slot_write(slot, (struct cstr_view){ (char const *)left.ptr, n },
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

/* Starts a git call in @p slot. The call runs the command in @p args, in
 * the repository of the repo argument, as a child on the loop of the
 * tool's part.
 */
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
		mcp_slot_tell(slot, cstr_view(why), TOOL_FAILED);
		return;
	}
	if (mcp_slot_offset(slot, args)) {
		git_call_destroy(&call);
		return;
	}
	if (call->phase == GIT_HELP) {
		git_help(slot, call->command);
		git_call_destroy(&call);
		return;
	}
	e = git_call_run(call, gl, slot);
	if (!e) {
		*mcp_slot_own(slot) = call;
		mcp_slot_park(slot);
		return;
	}
	git_call_destroy(&call);
	if (e == EBUSY) {
		mcp_slot_tell(slot, CSTR_VIEW("Too many commands are running; try again in a "
		                              "few seconds."), TOOL_FAILED);
		return;
	}
	struct cstr_view const text =
		util_format_view(why, sizeof why, "Cannot run git: %s.", strerror(e));
	LOG(LOG_WARN, "%s", why);
	mcp_slot_tell(slot, text, TOOL_FAILED);
}

/* Creates the git tool's part of @p loop for @p service, a struct git. The
 * part has the list of the children that the git calls on the loop run.
 * Returns the part, or nullptr with ENOMEM in @p *e.
 */
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
 * set it to nullptr.
 */
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
