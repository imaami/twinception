/** @file git.c
 *
 * The git tool.
 */
#define _GNU_SOURCE /* mempcpy() */
#include <errno.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "child.h"
#include "cstr_view.h"
#include "dstr.h"
#include "git.h"
#include "git_view.h"
#include "jb.h"
#include "log.h"
#include "loop.h"
#include "mcp.h"
#include "oid.h"
#include "plugin.h"
#include "review.h"
#include "schema.h"
#include "util.h"

static char const git_commit_suffix[] = "^{commit}";
static char const git_ignore_rev[] = "--ignore-rev=";

/* In the argv of a call, a word that git_call_resolve() writes to the
 * text of the call. Only its address counts.
 */
static char const git_derived[] = "(derived)";

enum {
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
	ROOTS     = 2 * REVISIONS + OPTIONS,  /**< the commits that a call names */
	ENV       = 13,       /**< git's environment, its nullptr included */
	ATTR      = ENV - 2,  /**< the index of GIT_ATTR_SOURCE in it, which is
	                           set when a call has a HEAD */
};

static_assert((int)WORD <= (int)REVIEW_NAME_MAX, "a path that git reads fits a location of "
              "the review record");

static_assert(TOOL_TEXT_MAX == 4U << 20, "update git.h and the texts in this file that say "
              "4 MiB");

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
	struct dstr     ceilings;  /* the GIT_CEILING_DIRECTORIES entry of env */
	uint64_t        timeout;
	size_t          n;
	struct git_repo repo[];
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

/* A revision argument: a commit, a range of two commits, or a commit and
 * a path.
 */
struct git_revision {
	struct cstr_view path;  /* ":path" in the revision argument, or empty, before a NUL */
	size_t           root;  /* the index of its first commit in the roots of the call */
	size_t           dots;  /* 2 or 3 for a range, else 0 */
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
	size_t rev_len; /* the bytes of the revision as commit:path, escaped for
	                   a JSON string, at the start of the text of the call,
	                   or 0 until the scan begins */
};

/**
 * One deadline and one immutable history view span every subprocess. The
 * roots are the commits that the call names, in the order of the options
 * and the revisions; git_call_resolve() resolves them. The text is the
 * words of the phase to come, each ending in a NUL: the roots for rev-parse,
 * then the words that stand in for the git_derived words of argv, then the
 * revision of a read.
 */
struct git_call {
	struct child          *child;
	struct git_view       *view;
	struct git_repo const *repo;
	struct json_object    *args;
	struct buf             metadata;
	struct dstr            text;
	char const            *argv[GIT_ARGV];
	uint64_t               deadline;
	size_t                 command;
	size_t                 n_roots;
	size_t                 n_revisions;
	struct git_revision    revisions[REVISIONS];
	struct git_read        read;
	enum git_phase         phase;
	struct oid             roots[ROOTS];
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

static_assert(1 + array_size(fixed) + 5 + 3 + OPTIONS + 2 + 1 + REVISIONS + 1 +
              PATHS + 1 <= GIT_ARGV, "a command fits argv");
static_assert(1 + array_size(fixed) + 5 + 10 + ROOTS + 1 <= GIT_ARGV,
              "history validation fits argv");

struct cstr_view
git_name (char const *path)
{
	struct cstr_view const suffix = CSTR_VIEW(".git");
	size_t end = strlen(path);
	while (end > 1 && path[end - 1] == '/')
		--end;
	size_t start = end;
	while (start && path[start - 1] != '/')
		--start;
	struct cstr_view name = { path + start, end - start };
	if (name.len > suffix.len && cstr_view_ends(name, suffix))
		name.len -= suffix.len;
	if (!name.len || name.len > 64 || *name.data == '.' ||
	    strspn(name.data, UTIL_ALNUM "_.-") < name.len)
		return (struct cstr_view){};
	return name;
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
	struct cstr_view const name = git_name(path);
	if (!name.len)
		return EINVAL;
	char *real = realpath(path, nullptr);
	if (!real)
		return errno;
	size_t size = strlen(real) + 1;
	char *s = malloc(2 * size + sizeof safe - 1 + name.len + 1);
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
	*(char *)mempcpy(copy, name.data, name.len) = '\0';
	r->name = (struct cstr_view){ copy, name.len };
	return 0;
}

/* Writes GIT_CEILING_DIRECTORIES to git->ceilings: the listed
 * repositories' parents, so that git finds no repository above one listed,
 * as it would look for one without one there. Returns 0 or ENOMEM.
 */
static int
git_ceilings (struct git *git)
{
	int e = cstr_view_cat(CSTR_VIEW("GIT_CEILING_DIRECTORIES="), &git->ceilings);
	for (size_t i = 0; !e && i < git->n; ++i) {
		char const *p = git->repo[i].path;
		size_t const len = (size_t)(strrchr(p, '/') - p);
		struct cstr_view const parent = len ? (struct cstr_view){ p, len } : CSTR_VIEW("/");
		e = dstr_printf(&git->ceilings, "%s%.*s", i ? ":" : "", (int)parent.len, parent.data)
		    ? 0 : ENOMEM;
	}
	return e;
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
	int e = git_ceilings(git);
	if (e)
		return e;
	/* The last but one is GIT_ATTR_SOURCE's, when a call has a HEAD. */
	char *const vars[] = {
		util_env_path(), (char *)dstr_raw(&git->ceilings), GIT_ENV_FIXED,
		"GIT_OPTIONAL_LOCKS=0", nullptr, nullptr
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
	git->ceilings = (struct dstr)DSTR_INIT;
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
	for (size_t i = 0; i < git->n; ++i) {
		free(git->repo[i].path);
		git->repo[i].path = nullptr;
	}
	dstr_cleanup(&git->ceilings);
	free(git);
}

/* Returns the definition of the git tool. @p service is the struct git,
 * and the definition lists the names of its repositories. Returns nullptr
 * if out of memory.
 */
static struct json_object *
git_definition (void const *service)
{
	struct git const *git = service;
	static_assert(OPTIONS == 16 && REVISIONS == 4 && PATHS == 32, "the description says");
	struct json_object *repos = json_object_new_array();
	for (size_t i = 0; repos && i < git->n; ++i)
		repos = jb_push_or_free(repos, jb_string(git->repo[i].name));
	struct json_object *cmds = json_object_new_array();
	for (size_t i = 0; cmds && i < array_size(commands); ++i)
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
				"options", schema_array(schema_property("string", nullptr), OPTIONS,
					"Options, such as --oneline or --max-count=20. Use "
					"help:true to list the command's exact allowed options."),
				"revisions", schema_array(schema_property("string", nullptr), REVISIONS,
					"Revisions, such as HEAD~3, main..topic or the hash of a "
					"commit. For read, show and cat-file, HEAD:path names a "
					"file as of that commit. References and hashes may use "
					"^/~ ancestry and ^{commit}. Reflogs, stash, index and "
					"raw blob/tree IDs are excluded. The resolved commit must "
					"belong to HEAD, branch, tag or remote-tracking history."),
				"paths", schema_array(schema_property("string", nullptr), PATHS,
					"Paths in the repository, relative to its top. They limit "
					"the output of a command such as log or diff to those "
					"paths. They do not read a file."),
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
			required, array_size(required)),
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
		size_t const n = allowed->len;
		char const last = allowed->data[n - 1];
		/* An exact spelling matches only itself, which then passes the
		 * value tests below. */
		bool const exact = last != '=' && last != '#' && last != '*';
		if (exact ? !cstr_view_equal(opt, *allowed)
		          : (!cstr_view_starts(opt, cstr_view_slice(*allowed, 0, n - 1)) ||
		             (last == '=' && opt.len >= n && opt.data[n - 1] != '=')))
			continue;
		size_t const at = last == '=' ? n : n - 1;
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

/**
 * Appends to @p text the reference or hash @p word, which may have ^/~
 * ancestry and commit peels, then "^{commit}" and a NUL: the word with
 * which rev-parse resolves it to a commit. Returns 0, EINVAL if @p word is
 * not such a name, or ENOMEM.
 */
static int
git_commit (struct dstr      *text,
            struct cstr_view  word)
{
	size_t const at = dstr_len(text);
	if (cstr_view_cat(word, text))
		return ENOMEM;
	/* The copy ends in the NUL of the dstr. */
	char const *dest = dstr_get(text) + at;
	size_t const len = word.len;
	size_t const base = strcspn(dest, "^~:@{}");
	struct cstr_view const ref = { dest, base };
	if (!base || *dest == '-' || *dest == '/' || strstr(dest, "..") ||
	    cstr_view_equal(ref, CSTR_VIEW("stash")) || cstr_view_equal(ref, CSTR_VIEW("refs/stash")))
		return EINVAL;
	for (size_t i = base; i < len;) {
		if (len - i >= sizeof git_commit_suffix - 1 &&
		    !memcmp(dest + i, git_commit_suffix, sizeof git_commit_suffix - 1)) {
			i += sizeof git_commit_suffix - 1;
			continue;
		}
		if (dest[i] != '^' && dest[i] != '~')
			return EINVAL;
		++i;
		while (dest[i] >= '0' && dest[i] <= '9')
			++i;
	}
	return dstr_cat_n(text, git_commit_suffix, sizeof git_commit_suffix) ? 0 : ENOMEM;
}

/* A message about what is wrong with a call. The message starts empty,
 * and git_wrong() appends to it.
 */
struct git_why {
	UTIL_TEXT(GIT_WHY);
};

/* Appends to @p why a part of the message about what is wrong with the
 * arguments of a call, as util_text_printf() does. Returns EINVAL.
 */
#define git_wrong(why, ...) (util_text_printf((why), __VA_ARGS__), EINVAL)

/** Validates the option @p opt of the command @p command, the option at
 * @p index in the options argument. Returns 0, or EINVAL with a message in
 * @p why that explains only how to repair the option.
 */
static int
git_option_check (struct cstr_view  opt,
                  size_t            command,
                  size_t            index,
                  struct git_why   *why)
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

/* Writes to @p why the names of the repositories that a call can select.
 * Returns EINVAL.
 */
static int
git_repos_wrong (struct git const *git,
                 struct git_why   *why)
{
	util_text_printf(why, "The repo argument must name one of the repositories:");
	for (size_t i = 0; i < git->n; ++i)
		util_text_printf(why, "%s %.*s", i ? "," : "", (int)git->repo[i].name.len,
		                 git->repo[i].name.data);
	return git_wrong(why, ".");
}

/* The arguments of the tool, in the order of git_arg_names[]. A call with
 * help:true takes those before GIT_ARG_OPTIONS only.
 */
enum git_arg : uint8_t {
	GIT_ARG_REPO,
	GIT_ARG_COMMAND,
	GIT_ARG_HELP,
	GIT_ARG_OFFSET,
	GIT_ARG_OPTIONS,
	GIT_ARG_REVISIONS,
	GIT_ARG_PATHS,
	GIT_ARG_PATTERN,
	GIT_ARG_LINE,
	GIT_ARG_COUNT,
	GIT_ARGS
};

static struct cstr_view const git_arg_names[GIT_ARGS] = {
	CSTR_VIEW_INIT("repo"), CSTR_VIEW_INIT("command"), CSTR_VIEW_INIT("help"),
	CSTR_VIEW_INIT("offset"), CSTR_VIEW_INIT("options"), CSTR_VIEW_INIT("revisions"),
	CSTR_VIEW_INIT("paths"), CSTR_VIEW_INIT("pattern"), CSTR_VIEW_INIT("line"),
	CSTR_VIEW_INIT("count"),
};

/*
 * Validate the call and retain its arguments and revision roots. Returns 0,
 * EINVAL with a message about what is wrong with the arguments in @p why,
 * or ENOMEM.
 */
static int
git_call_command (struct git_call  *call,
                  struct git const *git,
                  struct git_why   *why)
{
	struct json_object *args = call->args;
	if (!args)
		return git_wrong(why, "Tool arguments must be an object.");
	struct json_object *of[GIT_ARGS] = {};
	uint64_t seen;
	struct cstr_view other;
	if (jb_args(args, git_arg_names, GIT_ARGS, UINT64_MAX, of, &seen, &other))
		return git_wrong(why, "Unknown argument: %.*s.", (int)other.len, other.data);

	struct git_repo const *repo = git_find(git, git_word(of[GIT_ARG_REPO]));
	if (!repo)
		return git_repos_wrong(git, why);
	call->repo = repo;
	struct cstr_view const cmd = git_word(of[GIT_ARG_COMMAND]);
	size_t c = 0;
	while (c < array_size(commands) && !cstr_view_equal(cmd, commands[c].name))
		++c;
	if (c == array_size(commands)) {
		if (cstr_view_equal(cmd, CSTR_VIEW("status")) ||
		    cstr_view_equal(cmd, CSTR_VIEW("ls-files")))
			return git_wrong(why, "git %.*s is unavailable: index and working-tree "
			                 "reads are disabled. Use git ls-tree with "
			                 "options [\"-r\", \"--name-only\"] to list committed "
			                 "files; revisions defaults to HEAD.",
			                 (int)cmd.len, cmd.data);
		util_text_printf(why, "The command argument must be one of:");
		for (size_t i = 0; i < array_size(commands); ++i)
			util_text_printf(why, "%s %.*s", i ? "," : "", (int)commands[i].name.len,
			                 commands[i].name.data);
		return git_wrong(why, ". Use help:true for its options.");
	}
	call->command = c;
	uint64_t const takes = commands[c].takes;
	bool const read = takes & LINES;
	struct json_object *help = of[GIT_ARG_HELP];
	if ((seen >> GIT_ARG_HELP & 1) && !json_object_is_type(help, json_type_boolean))
		return git_wrong(why, "The help argument must be a boolean.");
	if (json_object_get_boolean(help)) {
		if (seen >> GIT_ARG_OPTIONS)
			return git_wrong(why, "With help:true, give only repo, command, help and "
			                 "optional offset.");
		call->phase = GIT_HELP;
		return 0;
	}

	static size_t const most[GIT_ARGS] = {
		[GIT_ARG_OPTIONS] = OPTIONS, [GIT_ARG_REVISIONS] = REVISIONS,
		[GIT_ARG_PATHS] = PATHS,
	};
	size_t lens[GIT_ARGS] = {};
	for (size_t l = GIT_ARG_OPTIONS; l <= GIT_ARG_PATHS; ++l) {
		if (!(seen >> l & 1))
			continue;
		char const *const name = git_arg_names[l].data;
		if (!json_object_is_type(of[l], json_type_array))
			return git_wrong(why, "The %s argument must be an array of strings "
			                 "(at most %zu entries).", name, most[l]);
		lens[l] = json_object_array_length(of[l]);
		if (lens[l] > most[l])
			return git_wrong(why, "The %s argument has %zu entries; its limit is "
			                 "%zu.", name, lens[l], most[l]);
		size_t i = 0;
		while (i < lens[l] && git_word(jb_at(of[l], i)).data)
			++i;
		if (i < lens[l])
			return git_wrong(why, "%s[%zu] must be a string of 1 to %d bytes with no "
			                 "NUL.", name, i, WORD);
	}
	if ((takes & SINGLE) && lens[GIT_ARG_REVISIONS] > 1)
		return git_wrong(why, "git %.*s takes at most one revision.", (int)cmd.len,
		                 cmd.data);
	if (lens[GIT_ARG_PATHS] && !(takes & FILES))
		return git_wrong(why, "git %.*s takes no paths. To name a file, give the "
		                 "revision HEAD:path.", (int)cmd.len, cmd.data);
	struct json_object *pattern = of[GIT_ARG_PATTERN];
	struct cstr_view const pat = git_word(pattern);
	if (takes & PATTERN) {
		if (!pat.data)
			return git_wrong(why, "git grep needs a pattern argument: a string of "
			                 "1 to %d bytes.", WORD);
	} else if (pattern) {
		return git_wrong(why, "Only git grep takes a pattern.");
	}
	uint64_t const numbers = seen & (1U << GIT_ARG_LINE | 1U << GIT_ARG_COUNT);
	if (numbers && !read)
		return git_wrong(why, "Only git read takes line and count.");
	size_t *const values[] = { &call->read.line, &call->read.count };
	call->read.line = 1;
	call->read.count = SIZE_MAX;
	for (size_t i = 0; i < array_size(values); ++i) {
		size_t const k = GIT_ARG_LINE + i;
		if (!(numbers >> k & 1))
			continue;
		uint64_t const v = jb_count(of[k], UINT64_MAX);
		if (!v)
			return git_wrong(why, "The %s argument must be a whole number from 1.",
			                 git_arg_names[k].data);
		/* Where size_t is narrower than 64 bits, a larger value becomes
		 * SIZE_MAX. That line is past the end of any file, and that count
		 * sets no limit. */
		*values[i] = v < SIZE_MAX ? (size_t)v : SIZE_MAX;
	}

	size_t k = 0;
	char const **argv = call->argv;
	argv[k++] = read ? "cat-file" : cmd.data;
	for (size_t i = 0; i < array_size(commands[c].forced) && commands[c].forced[i];
	     ++i)
		argv[k++] = commands[c].forced[i];
	/* The options, then the pattern, the revisions and the paths, each
	 * checked as it goes in. */
	for (size_t i = 0; i < lens[GIT_ARG_OPTIONS]; ++i) {
		struct cstr_view const opt = jb_view(jb_at(of[GIT_ARG_OPTIONS], i));
		int e = git_option_check(opt, c, i, why);
		if (e)
			return e;
		if (strncmp(opt.data, git_ignore_rev, sizeof git_ignore_rev - 1)) {
			argv[k++] = opt.data;
			continue;
		}
		++call->n_roots;
		e = git_commit(&call->text, cstr_view_slice(opt, sizeof git_ignore_rev - 1, SIZE_MAX));
		if (e)
			return e == ENOMEM ? e
			     : git_wrong(why, "--ignore-rev needs a commit reference or hash.");
		argv[k++] = git_derived;
	}
	if (pat.data) {
		argv[k++] = "-e";
		argv[k++] = pat.data;
	}
	if (takes & END)
		argv[k++] = "--end-of-options";
	size_t const listed = lens[GIT_ARG_REVISIONS];
	size_t endpoints = listed;
	call->n_revisions = listed ? listed : 1;
	for (size_t i = 0; i < call->n_revisions; ++i) {
		struct cstr_view const s = listed ? jb_view(jb_at(of[GIT_ARG_REVISIONS], i))
		                                  : CSTR_VIEW("HEAD");
		char const *path = strchr(s.data, ':');
		size_t root = path ? (size_t)(path - s.data) : s.len;
		if (path && (!(takes & OBJECT) || !path[1] || !git_inside(path + 1)))
			return git_wrong(why, "revisions[%zu]: only read, show and cat-file "
			                 "take commit:path, with a nonempty relative path.", i);
		char const *range = strstr(s.data, "..");
		if (range && range >= s.data + root)
			range = nullptr;
		if (range && (!(takes & RANGE) || ((takes & PAIR) && listed != 1)))
			return git_wrong(why, "revisions[%zu]: ranges are allowed only by "
			                 "log, or as diff's single A..B or A...B argument.", i);
		size_t left = range ? (size_t)(range - s.data) : root;
		struct git_revision *rev = &call->revisions[i];
		rev->root = call->n_roots++;
		rev->path = cstr_view_slice(s, root, SIZE_MAX);
		int e = git_commit(&call->text, cstr_view_slice(s, 0, left));
		if (!e && range) {
			++endpoints;
			++call->n_roots;
			rev->dots = 2 + (range[2] == '.');
			e = git_commit(&call->text, cstr_view_slice(s, left + rev->dots,
			                                            root - left - rev->dots));
		}
		if (e)
			return e == ENOMEM ? e
			     : git_wrong(why, "revisions[%zu]: use a commit reference or hash "
			                 "with optional ^/~ ancestry. Stash, reflogs, index "
			                 "selectors and revision expansions are unavailable.", i);
		argv[k++] = git_derived;
	}
	if (read && !call->revisions[0].path.len)
		return git_wrong(why, "git read needs the file as its revision, such as "
		                 "\"revisions\": [\"HEAD:src/main.c\"].");
	if ((takes & PAIR) && endpoints != 2)
		return git_wrong(why, "git diff needs two commits, or one explicit "
		                 "A..B or A...B range.");
	if (takes & FILES)
		argv[k++] = "--";
	for (size_t i = 0; i < lens[GIT_ARG_PATHS]; ++i) {
		char const *s = jb_view(jb_at(of[GIT_ARG_PATHS], i)).data;
		if (!git_inside(s) || !strncmp(s, ":(", 2))
			return git_wrong(why, "paths[%zu] must be relative to the repository's "
			                 "top, with no .. component or :(...) pathspec magic.", i);
		argv[k++] = s;
	}
	argv[k] = nullptr;
	return 0;
}

#undef git_wrong

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
			"capture_bytes", json_object_new_int(TOOL_TEXT_MAX),
			"window_bytes", json_object_new_int64((int64_t)mcp_slot_window_max(slot)),
			nullptr),
		"pattern_required", json_object_new_boolean(takes & PATTERN),
		nullptr);
	if (!help) {
		mcp_slot_fail(slot, ENOMEM);
		return;
	}
	struct cstr_view const text = jb_wire(help);
	size_t taken;
	int e = text.data ? mcp_slot_write(slot, text, 0, &taken) : ENOMEM;
	json_object_put(help);
	if (e)
		mcp_slot_fail(slot, e);
	else
		mcp_slot_finish(slot, 0);
}

static void
git_call_destroy (struct git_call **p_call)
{
	if (!*p_call)
		return;
	child_destroy(&(*p_call)->child);
	git_view_destroy(&(*p_call)->view);
	buf_fini(&(*p_call)->metadata);
	dstr_cleanup(&(*p_call)->text);
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
		.text = DSTR_INIT,
		.deadline = deadline,
	};
	*e = 0;
	return call;
}

/* Returns the word at @p *next in the text of a call, and moves @p *next
 * past it and its NUL.
 */
static char const *
git_text_next (char const **next)
{
	char const *word = *next;
	*next += strlen(word) + 1;
	return word;
}

/** Run one phase without extending the call's deadline. */
static int
git_call_run (struct git_call *call,
              struct mcp_slot *slot)
{
	static char const *const meta[] = {
		"rev-parse", "--path-format=absolute", "--git-path", "objects",
		"--git-path", "shallow", "--show-object-format", "--verify", "--quiet",
		"HEAD^{commit}", nullptr
	};
#define GIT_REF(s) s,
	static char const *const refs[] = {
		"for-each-ref", "--format=%(objectname) %(refname)", GIT_VIEW_REFS(GIT_REF) nullptr
	};
#undef GIT_REF
	static char const *const resolve[] = {
		"rev-parse", "--revs-only", "--end-of-options", nullptr
	};
	static char const *const check[] = {
		"rev-list", "--max-count=1", "--not", "--branches", "--tags", "--remotes", nullptr
	};
	char const *const *const words[] = {
		[GIT_META] = meta, [GIT_REFS] = refs, [GIT_RESOLVE] = resolve, [GIT_CHECK] = check,
		[GIT_RUN] = call->argv,
	};
	bool const head = call->view && git_view_has_head(call->view);
	char const *argv[GIT_ARGV] = { "git" };
	size_t k = 1;
	for (size_t i = 0; i < array_size(fixed); ++i)
		argv[k++] = fixed[i];
	argv[k++] = "-c";
	argv[k++] = call->repo->safe;
	argv[k++] = "-C";
	argv[k++] = call->view ? git_view_directory(call->view) : call->repo->path;
	if (call->view)
		argv[k++] = "--git-dir=.";
	char const *next = dstr_get(&call->text);
	for (char const *const *w = words[call->phase]; *w; ++w)
		argv[k++] = *w == git_derived ? git_text_next(&next) : *w;
	/* rev-parse resolves the roots, and rev-list shows a commit of theirs
	 * that HEAD and the refs of the view do not reach. */
	switch (call->phase) {
	case GIT_RESOLVE:
		for (size_t i = 0; i < call->n_roots; ++i)
			argv[k++] = git_text_next(&next);
		break;
	case GIT_CHECK:
		if (head)
			argv[k++] = "HEAD";
		argv[k++] = "--not";
		argv[k++] = "--end-of-options";
		for (size_t i = 0; i < call->n_roots; ++i)
			argv[k++] = call->roots[i].hex;
		argv[k++] = "--";
		break;
	default:
		break;
	}
	argv[k] = nullptr;
	struct git const *git = mcp_slot_part(slot);
	char *env[ENV];
	memcpy(env, git->env, sizeof env);
	if (head)
		env[ATTR] = "GIT_ATTR_SOURCE=HEAD";
	struct child_cmd const cmd = {
		.file = "git",
		.argv = argv,
		.envp = env,
		.max = TOOL_TEXT_MAX,
		.deadline = call->deadline,
		.grace = GIT_GRACE,
		/* Git can warn on stderr, for example when a branch has the same
		 * name as a commit ID. A read discards stderr so that a warning
		 * does not become a line of the file. */
		.flags = call->phase == GIT_RUN && !(commands[call->command].takes & LINES)
		         ? CHILD_STDERR : 0,
	};
	int e = 0;
	call->child = mcp_slot_spawn(slot, &cmd, &e);
	return e;
}

/**
 * Replace every validated root with exactly one immutable commit ID. Then
 * write to the text of @p call the words that the command takes for the
 * --ignore-rev options and for the revisions, in the order of argv: the
 * option with the commit, and the commit with the path or the range.
 * Returns 0, EINVAL or ENOMEM.
 */
static int
git_call_resolve (struct git_call *call)
{
	struct span const s = buf_peek(&call->child->out);
	struct cstr_view out = { (char const *)s.ptr, s.len };
	struct oid *roots = call->roots;
	for (size_t i = 0; i < call->n_roots; ++i)
		if (oid_init_field(&roots[i], &out, '\n') || oid_len(&roots[i]) != oid_len(&roots[0]))
			return EINVAL;
	if (out.len)
		return EINVAL;
	struct dstr *text = &call->text;
	dstr_clear(text);
	bool ok = true;
	for (size_t i = 0; ok && i < call->revisions[0].root; ++i)
		ok = dstr_printf(text, "%s%s%c", git_ignore_rev, roots[i].hex, '\0');
	for (size_t i = 0; ok && i < call->n_revisions; ++i) {
		struct git_revision const *rev = &call->revisions[i];
		struct oid const *root = &roots[rev->root];
		ok = rev->dots ? dstr_printf(text, "%s%.*s%s%c", root->hex, (int)rev->dots, "...",
		                             root[1].hex, '\0')
		               : dstr_printf(text, "%s%.*s%c", root->hex, (int)rev->path.len,
		                             rev->path.data, '\0');
	}
	return ok ? 0 : ENOMEM;
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
	int const status = child_status(c);
	int e = status < 0 ? EIO
	      : status && !(call->phase == GIT_META && status == 1) ? EINVAL : 0;
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
	bool const late = c->flags & CHILD_LATE;
	child_destroy(&call->child);
	if (!e) {
		++call->phase;
		e = git_call_run(call, slot);
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

/* Returns true if the string @p path contains a ':' and digits after it
 * that end the path or are followed by another ':', as in a grep match
 * with its line number.
 */
static bool
git_line_hint (char const *path)
{
	for (char const *p = strchr(path, ':'); p; p = strchr(p + 1, ':')) {
		size_t n = strspn(p + 1, "0123456789");
		if (n && (!p[1 + n] || p[1 + n] == ':'))
			return true;
	}
	return false;
}

/* Destroys @p call after git has ended and its output is copied. @p takes
 * is the takes bits of its command. If git failed, if the output was cut,
 * or if there was no output, the function then writes a note about it
 * after the output. Then it finishes the call. Exit status 1 from grep
 * means no matches.
 */
static void
git_ended (struct mcp_slot *slot,
           struct git_call *call,
           uint64_t         takes)
{
	struct child *c = call->child;
	struct git const *git = mcp_slot_part(slot);
	struct cstr_view out = mcp_slot_answer(slot);
	/* The blank line before the note and its brackets are 4 bytes at most. */
	char buf[GIT_NOTE - 4];
	struct cstr_view note = child_why(c, "git", git->timeout, buf, sizeof buf);
	int const status = child_status(c);
	bool failed = true;
	if ((c->flags & (CHILD_CUT | CHILD_LATE)) == CHILD_CUT) {
		note = CSTR_VIEW("The output goes on past the 4 MiB shown.");
		failed = c->code == CLD_EXITED && c->status;
	} else if (!status) {
		note = out.len ? (struct cstr_view){} : CSTR_VIEW("No output.");
		failed = false;
	} else if (status == 1 && (takes & PATTERN)) {
		note = CSTR_VIEW("No matches.");
		failed = false;
	} else if (status > 0 && takes & (LINES | OBJECT)) {
		static char const fmt[] = "No file at that path in commit %s. ls-tree lists "
		                          "the files there.%s";
		static char const hint[] = " If :N after the file name is a line number, "
		                           "give the file alone as the revision and "
		                           "\"line\": N.";
		static_assert(sizeof fmt - 4 + OID_SHA256 + sizeof hint - 1 <= sizeof buf,
		              "the note fits");
		struct git_revision const *rev = &call->revisions[0];
		note = takes & LINES
		     ? util_format_view(buf, sizeof buf, fmt, call->roots[rev->root].hex,
		                        git_line_hint(rev->path.data + 1) ? hint : "")
		     : util_format_view(buf, sizeof buf, "The committed object could not be "
		                        "read (git exited with status %d).", status);
	}
	git_drop(slot);
	struct cstr_view const p[] = {
		!out.len ? CSTR_VIEW("") : out.data[out.len - 1] == '\n' ? CSTR_VIEW("\n")
		                                                         : CSTR_VIEW("\n\n"),
		CSTR_VIEW("["),
		note,
		CSTR_VIEW("]"),
	};
	int e = 0;
	size_t taken;
	for (size_t i = note.data ? 0 : array_size(p); i < array_size(p) && !e; ++i)
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

/* Begins the read of @p call: writes its revision, commit:path, escaped
 * for a JSON string, to the text of the call. read->room is the rest of
 * the @p window bytes after the longest header and footer. Returns 0 or
 * ENOMEM.
 */
static int
git_read_begin (struct git_call *call,
                size_t           window)
{
	struct git_read *read = &call->read;
	struct git_revision const *rev = &call->revisions[0];
	dstr_clear(&call->text);
	int e = cstr_view_cat(oid_view(&call->roots[rev->root]), &call->text);
	if (!e)
		e = jb_escape_cat(rev->path, &call->text);
	read->rev_len = dstr_len(&call->text);
	/* The header contains the repository name, the commit ID and path from
	 * the revision, and three numbers. The longest footer contains the
	 * revision and one number. */
	size_t reserve = sizeof "[Repository , file  at commit : lines  to  of at least .]\n" +
	                 sizeof "\n\n[To read on, call again with \"revisions\": "
	                        "[\"\"] and \"line\": .]" +
	                 call->repo->name.len + 2 * read->rev_len + 4 * UTIL_DECIMAL;
	read->room = window > reserve ? window - reserve : 0;
	read->last = read->line - 1;
	return e;
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
	char digits[UTIL_DECIMAL];
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

/* Writes to @p why the refusal of a read whose first line is past the end
 * of the file, or past the end of the capture if @p cut is true.
 */
static void
git_read_past (struct git_read const *read,
               bool                   cut,
               struct git_why        *why)
{
	if (!read->total)
		util_text_printf(why, "Line %zu is past the end of the file: the file is empty.",
		                 read->line);
	else if (cut)
		util_text_printf(why, "Line %zu is past the first 4 MiB of the file, which is as "
		                 "far as the git tool reads.", read->line);
	else
		util_text_printf(why, "Line %zu is past the end of the file: it has %zu line%s. "
		                 "Give a line from 1 to %zu.", read->line, read->total,
		                 util_s(read->total), read->total);
}

/* Writes the header line of the read in @p call to @p slot if it fits in
 * the LOOP_COPY room of the step. If @p cut is true, the line total is a
 * lower bound ("at least"). Returns 0, EAGAIN or the error of
 * mcp_slot_write().
 */
static int
git_read_head (struct git_call const *call,
               struct mcp_slot       *slot,
               bool                   cut)
{
	struct git_read const *read = &call->read;
	struct oid const *commit = &call->roots[call->revisions[0].root];
	size_t const n = oid_len(commit);
	char lines[sizeof ": lines  to  of at least .]\n" + 3 * UTIL_DECIMAL];
	struct cstr_view const p[] = {
		CSTR_VIEW("[Repository "), call->repo->name, CSTR_VIEW(", file "),
		{ dstr_get(&call->text) + n + 1, read->rev_len - n - 1 },
		CSTR_VIEW(" at commit "), oid_view(commit),
		read->total ? util_format_view(lines, sizeof lines, ": lines %zu to %zu of "
		                               "%s%zu.]\n", read->line, read->last,
		                               cut ? "at least " : "", read->total)
		            : CSTR_VIEW(": the file is empty.]"),
	};
	return git_write(slot, p, array_size(p));
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
	char digits[UTIL_DECIMAL + 1];
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
 * number of the line after or inside which the 4 MiB capture ends.
 * @p newline is true if the last line ends in a newline. The function
 * writes no footer for an empty file. Returns 0, EAGAIN or the error of
 * mcp_slot_write().
 */
static int
git_read_foot (struct git_call const *call,
               struct mcp_slot       *slot,
               bool                   cut,
               bool                   newline)
{
	struct git_read const *read = &call->read;
	if (!read->total)
		return 0;
	/* Each note is the text before the revision, the revision, the text
	 * before the line number, and the text after it. */
	struct cstr_view const notes[][4] = {
		{ CSTR_VIEW("[To read on, call again with \"revisions\": [\""),
		  { dstr_get(&call->text), read->rev_len }, CSTR_VIEW("\"] and \"line\": "),
		  CSTR_VIEW(".]") },
		{ CSTR_VIEW("[Line "), {}, {}, CSTR_VIEW(" is the end of the file.]") },
		{ CSTR_VIEW("[Line "), {}, {},
		  CSTR_VIEW(" is the end of the file, which has no newline after it.]") },
		{ CSTR_VIEW("[The file goes on after line "), {}, {},
		  CSTR_VIEW(", past the 4 MiB that the git tool reads.]") },
		{ CSTR_VIEW("[Line "), {}, {},
		  CSTR_VIEW(" goes on past the 4 MiB that the git tool reads.]") },
	};
	size_t kind = read->last < read->total ? 0 : 1 + 2 * cut + !newline;
	struct cstr_view const *note = notes[kind];
	char n[UTIL_DECIMAL];
	struct cstr_view const p[] = {
		newline ? CSTR_VIEW("\n") : CSTR_VIEW("\n\n"),
		note[0], note[1], note[2],
		git_digits(n + sizeof n, read->last + !kind, 0),
		note[3],
	};
	return git_write(slot, p, array_size(p));
}

/* Pass mcp_slot_source() the numbered lines of the read in @p call, from
 * read->head to @p end in the output, for the review record. If the 4 MiB
 * cut (@p cut) ends inside the last line, which then does not end in a
 * newline (@p newline), that line is left out. total is 0 (unknown) when
 * the output was cut. Return 0 or the error of mcp_slot_source().
 */
static int
git_read_shown (struct git_call const *call,
                struct mcp_slot       *slot,
                size_t                 end,
                bool                   cut,
                bool                   newline)
{
	struct git_read const *read = &call->read;
	bool const part = read->last >= read->line && cut && !newline;
	if (read->last - part < read->line)
		return 0;
	struct git_revision const *rev = &call->revisions[0];
	struct review_source const src = {
		.repo   = call->repo->name,
		.commit = oid_view(&call->roots[rev->root]),
		.path   = cstr_view_slice(rev->path, 1, SIZE_MAX),
		.at     = read->head,
		.len    = (part ? read->mark : end) - read->head,
		.first  = read->line,
		.last   = read->last - part,
		.total  = cut ? 0 : read->total,
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
		int e = git_read_begin(call, mcp_slot_window_max(slot));
		if (e) {
			mcp_slot_fail(slot, e);
			return;
		}
	}
	git_read_scan(read, blob, c->loop);
	if (read->start < blob.len) {
		mcp_slot_ready(slot);
		return;
	}
	read->total = read->lines;
	if (read->line > (read->total ? read->total : 1)) {
		struct git_why why = {};
		git_read_past(read, c->flags & CHILD_CUT, &why);
		git_drop(slot);
		mcp_slot_tell(slot, util_text_view(&why), TOOL_FAILED);
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
	bool const cut = c->flags & CHILD_CUT;
	bool const newline = read->to && blob.ptr[read->to - 1] == '\n';
	int e = 0;
	if (!read->head) {
		e = git_read_head(call, slot, cut);
		read->head = mcp_slot_answer(slot).len;
	}
	if (!e)
		e = git_read_lines(read, slot, blob);
	size_t end = mcp_slot_answer(slot).len;
	if (!e)
		e = git_read_foot(call, slot, cut, newline);
	if (!e)
		e = git_read_shown(call, slot, end, cut, newline);
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
	uint64_t const takes = commands[call->command].takes;
	/* Git's failed object lookup may disclose whether a path exists on disk. */
	if ((takes & OBJECT) && c->code == CLD_EXITED && c->status) {
		git_ended(slot, call, takes);
		return;
	}
	/* When the output goes past 4 MiB, git is killed. A read then writes
	 * the lines before the cut, as it does after a git that exited in
	 * time. */
	if (takes & LINES) {
		uint32_t const end = c->flags & (CHILD_CUT | CHILD_LATE);
		if (end ? end != CHILD_CUT : c->code != CLD_EXITED)
			git_ended(slot, call, takes);
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
	git_ended(slot, call, takes);
}

/* Starts a git call in @p slot. The call runs the command in @p args, in
 * the repository of the repo argument, as a child on the call's loop.
 */
static void
git_start (struct mcp_slot    *slot,
           struct json_object *args)
{
	struct git const *git = mcp_slot_part(slot);
	int e = 0;
	struct git_call *call = git_call_create(args, mcp_slot_loop(slot)->now + git->timeout,
	                                        &e);
	if (e) {
		mcp_slot_fail(slot, e);
		return;
	}
	struct git_why why = {};
	e = git_call_command(call, git, &why);
	if (e) {
		git_call_destroy(&call);
		if (e == EINVAL)
			mcp_slot_tell(slot, util_text_view(&why), TOOL_FAILED);
		else
			mcp_slot_fail(slot, e);
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
	e = git_call_run(call, slot);
	if (!e) {
		*mcp_slot_own(slot) = call;
		mcp_slot_park(slot);
		return;
	}
	git_call_destroy(&call);
	if (e == EBUSY) {
		mcp_slot_tell(slot, CSTR_VIEW(CHILD_BUSY), TOOL_FAILED);
		return;
	}
	LOG(LOG_WARN, "Cannot run git: %s.", strerror(e));
	mcp_slot_tellf(slot, TOOL_FAILED, "Cannot run git: %s.", strerror(e));
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
