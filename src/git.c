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
#include "loop.h"
#include "mcp.h"
#include "oid.h"
#include "plugin.h"
#include "review.h"
#include "schema.h"
#include "util.h"
#include "xlist.h"

static struct cstr_view const git_ignore_rev = CSTR_VIEW_INIT("--ignore-rev=");

/* In the argv of a call, a word that git_call_spawn() writes to the text
 * of the pin. Only its address counts.
 */
static char const git_derived[] = "(derived)";

enum {
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
	ATTR      = GIT_ENV - 2,  /**< the index of GIT_ATTR_SOURCE in git's
	                               environment, which is set only if the
	                               view contains HEAD */
};

static_assert(2 * REVISIONS + OPTIONS <= GIT_ROOTS, "the commits that a call names fit a pin");

static_assert((int)WORD <= (int)REVIEW_NAME_MAX, "a path that git reads fits a location of "
              "the review record");

static_assert(TOOL_TEXT_MAX == 4U << 20, "update git.h and the texts in this file that say "
              "4 MiB");

struct git {
	char           *env[GIT_ENV];
	struct dstr     ceilings;  /* the GIT_CEILING_DIRECTORIES entry of env */
	uint64_t        timeout;
	size_t          n;
	struct git_repo repo[];
};

enum git_phase : uint64_t {
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
 * writing have come. Offsets are into git's output, call->pin.child->out,
 * except head and mark, which are into the call's output (mcp_slot_answer()).
 */
struct git_read {
	size_t line;    /* the number of the first line to give, from 1 */
	size_t count;   /* the maximum number of lines to give */
	size_t room;    /* the bytes of the window left for numbered lines */
	/* scan.at is the offset of the next byte to scan, then to write, and
	 * scan.start the offset where the line that contains scan.at starts. */
	struct loop_lines scan;
	size_t lines;   /* the number of lines before scan.start */
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
 * All git processes of a call use the deadline and the view of its pin.
 * The roots of the pin are the commits in the options and in the
 * revisions of the call, in that order. Once the pin is done, the text of
 * the pin contains the words that replace the git_derived words of argv,
 * each followed by a NUL, and then the revision of a read call.
 */
struct git_call {
	struct git_pin         pin;
	struct json_object    *args;
	char const            *argv[GIT_ARGV];
	size_t                 command;
	size_t                 n_revisions;
	struct git_revision    revisions[REVISIONS];
	struct git_read        read;
	enum git_phase         phase;
};

/* What git runs with whatever the call: GIT_OPTIONS_FIXED, no implicit
 * worktree operations, no submodule's diff (which a superproject's show
 * or log -p runs as a git of its own, without these options), and path
 * names as they are, not quoted, unless they have a '"', a '\' or a
 * control character.
 */
static char const *const fixed[] = {
	GIT_OPTIONS_FIXED,
	"-c", "core.bare=true",
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
	O("--all"), O("--branches"), O("--tags"), O("--remotes"),
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
	MODE    = 1 << 8,  /* exactly one option, which selects what to show */
	ONEPATH = 1 << 9,  /* exactly one path, the file */
};

/* The commands: X(ID, name, options, takes, description, forced...), an
 * ID that no code uses, the name, the options that it accepts, the flags
 * above for what it takes, its help, and the options that askai adds.
 */
#define GIT_COMMAND_LIST(X)                                                                \
        X(READ,      "read",      read_options,      END | OBJECT | SINGLE | LINES,        \
          "Numbered lines of a committed file. Give the file as the one revision, "        \
          "commit:path such as HEAD:src/journal.c, the first line as line "                \
          "(default 1) and the most lines as count (default: as many as fit in one "       \
          "result). The output's first line names the commit. Its last line gives "        \
          "the revision and line to read on from; the revision names the commit by "       \
          "its ID, so every part comes from the same commit.", "blob")                     \
        X(LOG,       "log",       log_options,       FILES | END | RANGE,                  \
          "Committed history; defaults to HEAD. Paths restrict the history.",              \
          "--no-ext-diff", "--no-textconv")                                                \
        X(SHOW,      "show",      show_options,      FILES | END | OBJECT,                 \
          "Committed contents; defaults to HEAD. HEAD:path reads a committed file.",       \
          "--no-ext-diff", "--no-textconv")                                                \
        X(DIFF,      "diff",      diff_options,      FILES | END | RANGE | PAIR,           \
          "Compare two commits: give two revisions, or one explicit A..B or A...B "        \
          "range. Paths restrict the comparison. --stat or --name-status gives a "         \
          "summary instead of the patch.", "--no-ext-diff", "--no-textconv")               \
        X(GREP,      "grep",      grep_options,      FILES | END | PATTERN,                \
          "Search committed trees; defaults to HEAD. The pattern is an extended "          \
          "regex: -G makes it basic, -F plain text, -P Perl; -i ignores case. Each "       \
          "match is commit:path:line:text with the full commit ID; read takes "            \
          "commit:path as its revision and that line. --heading gives commit:path "        \
          "once per file. -C3 adds three lines of context, -p the function's first "       \
          "line, -W the whole function. --max-count=N caps the matches per file, and "     \
          "-l lists only the files.", "--no-textconv", "-n", "-E")                         \
        X(BLAME,     "blame",     blame_options,     FILES | SINGLE | ONEPATH,             \
          "Annotate a committed file; defaults to HEAD. Give the file as the one "         \
          "path, such as \"paths\": [\"src/git.c\"], and its commit in revisions. "        \
          "-L10,30 limits the lines; -s leaves out the author and date.", "--no-textconv") \
        X(LS_TREE,   "ls-tree",   ls_tree_options,   FILES | SINGLE,                       \
          "Entries of a committed tree; defaults to HEAD. Paths [\"src/\"] list the "      \
          "entries in src, and [\"src\"] only src itself. -r recurses into "               \
          "subtrees, and -d lists only trees.")                                            \
        X(CAT_FILE,  "cat-file",  cat_file_options,  END | OBJECT | SINGLE | MODE,         \
          "Inspect a committed object; defaults to HEAD, and commit:path names a "         \
          "file. Give exactly one option: -p for its contents, -t for its type, -s "       \
          "for its size in bytes, -e to check that it exists (no output if it does).")     \
        X(REV_PARSE, "rev-parse", rev_parse_options, 0,                                    \
          "Resolve commit references to immutable commit IDs; defaults to HEAD. "          \
          "--verify requires one valid revision.", "--revs-only")

static struct {
	struct cstr_view const *options;
	char const             *forced[3];  /* options askai gives it */
	char const             *description;
	uint64_t                takes;
} const commands[] = {
#define GIT_COMMAND_ROW(id, name, options, takes, text, ...) { options, { __VA_ARGS__ }, text, takes },
	GIT_COMMAND_LIST(GIT_COMMAND_ROW)
#undef GIT_COMMAND_ROW
};

static struct cstr_view const git_command_names[] = { GIT_COMMAND_LIST(XL_VIEW) };

static_assert(1 + array_size(fixed) + 5 + 3 + OPTIONS + 2 + 1 + REVISIONS + 1 +
              PATHS + 1 <= GIT_ARGV, "a command fits argv");
static_assert(1 + array_size(fixed) + 5 + 10 + GIT_ROOTS + 1 <= GIT_ARGV,
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
	size_t const size = strlen(real) + 1;
	char *s = malloc(2 * size + sizeof safe - 1 + name.len + 1);
	if (!s) {
		free(real);
		return ENOMEM;
	}
	char *copy = mempcpy(mempcpy(mempcpy(s, real, size), safe, sizeof safe - 1), real, size);
	free(real);
	*(char *)mempcpy(copy, name.data, name.len) = '\0';
	*r = (struct git_repo){ .name = { copy, name.len }, .path = s, .safe = s + size };
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
	struct git *git = malloc(sizeof *git + n * sizeof *git->repo);
	if (!git) {
		*e = ENOMEM;
		return nullptr;
	}
	*git = (struct git){ .ceilings = DSTR_INIT, .timeout = timeout };
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
		git->repo[i] = (struct git_repo){};
	}
	dstr_cleanup(&git->ceilings);
	memset(git->env, 0, sizeof git->env);
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
	struct json_object *cmds = schema_names(git_command_names, array_size(git_command_names));
	struct cstr_view const required[] = { CSTR_VIEW("repo"), CSTR_VIEW("command") };
	return jb_object(
		"name", json_object_new_string("git"),
		"title", json_object_new_string("Git"),
		"description", json_object_new_string(
			"Run a read-only git command in one of the repositories of the "
			"server, and get its output, with its errors, cut at 4 MiB. "
			"Use help:true with "
			"any command for its exact options, limits and source selection. "
			"Only committed contents are visible; the index, working tree, "
			"untracked and ignored files are unavailable. Commits must be "
			"reachable from HEAD, branches, tags or remote-tracking refs. "
			"Commands default to "
			"HEAD, except diff, which needs two commits or an explicit range. "
			"To read a file, use read with the file as the revision: "
			"{\"command\": \"read\", \"revisions\": [\"HEAD:src/journal.c\"], "
			"\"line\": 120, \"count\": 40} gives lines 120 to 159 with their "
			"numbers and names the commit. The output's last line gives the next "
			"call, with the commit's ID in place of HEAD. A grep match "
			"<commit>:<path>:<line>:<text> reads as \"revisions\": "
			"[\"<commit>:<path>\"] and \"line\": <line>. Give each option with "
			"its value attached (--max-count=20, not -n 20)."),
		"inputSchema", schema_object(
			jb_object(
				"repo", schema_enum(repos, nullptr),
				"command", schema_enum(cmds, nullptr),
				"help", schema_property("boolean",
					"Describe this command instead of running it."),
				"options", schema_array(schema_property("string", nullptr), OPTIONS,
					"Options, such as --oneline or --max-count=20. Use "
					"help:true to list the command's exact allowed options."),
				"revisions", schema_array(schema_property("string", nullptr), REVISIONS,
					"Revisions, such as HEAD~3, main..topic or the hash of a "
					"commit. For read, show and cat-file, HEAD:path names a "
					"file as of that commit. References and hashes may use "
					"^/~ ancestry and ^{commit}. Reflogs, stash, index and "
					"raw blob/tree IDs are excluded."),
				"paths", schema_array(schema_property("string", nullptr), PATHS,
					"Paths in the repository, relative to its top. They limit "
					"the output of a command such as log or diff to those "
					"paths; blame takes the file as its one path."),
				"pattern", schema_property("string",
					"The pattern for grep, an extended regex unless options "
					"give -G (basic), -F (plain text) or -P (Perl)."),
				"offset", schema_count(0, TOOL_OFFSET_ABOUT),
				"line", schema_count(1,
					"For read: the number of the first line to give, "
					"from 1. The default is 1."),
				"count", schema_count(1,
					"For read: the most lines to give. The default is "
					"as many as fit in one result."),
				nullptr),
			required, array_size(required)),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(1),
			"openWorldHint", json_object_new_boolean(0),
			nullptr),
		nullptr);
}

/** Looks up the option @p opt in @p allowed and stores the spelling that
 * matches in @p *match. @p allowed is an array of option spellings that ends
 * with a view whose data is nullptr. @p opt is an options entry that
 * jb_text() checked, so a NUL comes after its bytes. Returns 0, ENODATA for a
 * missing value, EINVAL for nondecimal digits, or ENOENT if no spelling
 * matches. A spelling that matches @p opt with a valid value is preferred
 * over one whose value is missing or malformed, such as --abbrev over
 * --abbrev= for the option --abbrev.
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
	struct cstr_view const suffix = CSTR_VIEW("^{commit}");
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
		if (cstr_view_starts((struct cstr_view){ dest + i, len - i }, suffix)) {
			i += suffix.len;
			continue;
		}
		if (dest[i] != '^' && dest[i] != '~')
			return EINVAL;
		++i;
		while (dest[i] >= '0' && dest[i] <= '9')
			++i;
	}
	return dstr_cat_n(text, suffix.data, suffix.len + 1) ? 0 : ENOMEM;
}

/* Appends to @p why a part of the message about what is wrong with the
 * arguments of a call, as util_text_printf() does, after a space unless
 * the message is empty. Returns EINVAL.
 */
#define git_wrong(why, ...) (util_text_printf((why), "%s", (why)->len ? " " : ""), \
                             util_text_printf((why), __VA_ARGS__), EINVAL)

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
	return git_wrong(why, "options[%zu]: '%.*s' is not available for git %s in "
	                 "this tool. Omit it; use help:true for this command's options.",
	                 index, quoted, opt.data, git_command_names[command].data);
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
	util_text_printf(why, "The repo argument must be ");
	for (size_t i = 0; i < git->n; ++i)
		util_text_printf(why, "%s%.*s", !i ? "" : i + 1 < git->n ? ", " : " or ",
		                 (int)git->repo[i].name.len, git->repo[i].name.data);
	util_text_printf(why, ".");
	return EINVAL;
}

/* Returns the word at @p *next in a sequence of words that each end in a
 * NUL, and advances @p *next past the word and its NUL.
 */
static char const *
git_text_next (char const **next)
{
	char const *word = *next;
	*next += strlen(word) + 1;
	return word;
}

void
git_pin_init (struct git_pin   *pin,
              struct git const *git,
              struct cstr_view  repo,
              uint64_t          deadline)
{
	*pin = (struct git_pin){
		.text = DSTR_INIT,
		.git = git,
		.repo = git_find(git, repo),
		.deadline = deadline,
	};
}

void
git_pin_fini (struct git_pin *pin)
{
	child_destroy(&pin->child);
	git_view_destroy(&pin->view);
	buf_fini(&pin->metadata);
	dstr_cleanup(&pin->text);
}

int
git_pin_add (struct git_pin   *pin,
             struct cstr_view  word)
{
	if (pin->n_roots == GIT_ROOTS)
		return EINVAL;
	int e = git_commit(&pin->text, word);
	pin->n_roots += !e;
	return e;
}

size_t
git_pin_argv (struct git_pin const *pin,
              char const          **argv,
              char                **env)
{
	size_t k = 0;
	argv[k++] = "git";
	for (size_t i = 0; i < array_size(fixed); ++i)
		argv[k++] = fixed[i];
	argv[k++] = "-c";
	argv[k++] = pin->repo->safe;
	argv[k++] = "-C";
	memcpy(env, pin->git->env, sizeof pin->git->env);
	if (!pin->view) {
		argv[k++] = pin->repo->path;
	} else {
		argv[k++] = git_view_directory(pin->view);
		argv[k++] = "--git-dir=.";
		if (git_view_has_head(pin->view))
			env[ATTR] = "GIT_ATTR_SOURCE=HEAD";
	}
	return k;
}

/* Spawns in @p slot the git of the current phase of @p pin, or once the
 * pin is done, the command @p run of a git call with the child flags
 * @p flags, whose git_derived words are taken from the text of the pin.
 * Then parks the call. Returns 0 or the error of mcp_slot_spawn().
 */
static int
git_pin_spawn (struct git_pin           *pin,
               struct mcp_slot          *slot,
               char const *const        *run,
               uint32_t                  flags)
{
	static char const *const meta[] = {
		"rev-parse", "--path-format=absolute", "--git-path", "objects",
		"--git-path", "shallow", "--show-object-format", "--verify", "--quiet",
		"HEAD^{commit}", nullptr
	};
#define GIT_REF(s) s,
	static char const *const refs[] = {
		"for-each-ref", "--format=%(objectname) %(refname)", GIT_VIEW_REF_LIST(GIT_REF) nullptr
	};
#undef GIT_REF
	static char const *const resolve[] = {
		"rev-parse", "--revs-only", "--end-of-options", nullptr
	};
	static char const *const check[] = {
		"rev-list", "--max-count=1", "--not", "--branches", "--tags", "--remotes", nullptr
	};
	char const *const *const words[] = {
		[GIT_PIN_META] = meta, [GIT_PIN_REFS] = refs, [GIT_PIN_RESOLVE] = resolve,
		[GIT_PIN_CHECK] = check, [GIT_PIN_DONE] = run,
	};
	char const *argv[GIT_ARGV];
	char *env[GIT_ENV];
	size_t k = git_pin_argv(pin, argv, env);
	char const *next = dstr_get(&pin->text);
	for (char const *const *w = words[pin->phase]; *w; ++w)
		argv[k++] = *w == git_derived ? git_text_next(&next) : *w;
	/* rev-parse resolves the roots. rev-list prints a commit that is
	 * reachable from a root but not from HEAD or the refs of the view. */
	switch (pin->phase) {
	case GIT_PIN_RESOLVE:
		for (size_t i = 0; i < pin->n_roots; ++i)
			argv[k++] = git_text_next(&next);
		break;
	case GIT_PIN_CHECK:
		if (env[ATTR])
			argv[k++] = "HEAD";
		argv[k++] = "--not";
		argv[k++] = "--end-of-options";
		for (size_t i = 0; i < pin->n_roots; ++i)
			argv[k++] = pin->roots[i].hex;
		argv[k++] = "--";
		break;
	default:
		break;
	}
	argv[k] = nullptr;
	struct child_cmd const cmd = {
		.file = "git",
		.argv = argv,
		.envp = env,
		.max = TOOL_TEXT_MAX,
		.deadline = pin->deadline,
		.grace = GIT_GRACE,
		.flags = flags,
	};
	int e;
	pin->child = mcp_slot_spawn(slot, &cmd, &e);
	return e;
}

/* Replaces each root of @p pin with a commit ID from @p out, the output of
 * rev-parse. Returns 0, or EINVAL if @p out does not contain exactly one
 * ID of the same length for each root.
 */
static int
git_pin_resolve (struct git_pin *pin,
                 struct span     out)
{
	struct cstr_view text = { (char const *)out.ptr, out.len };
	struct oid *roots = pin->roots;
	for (size_t i = 0; i < pin->n_roots; ++i)
		if (oid_init_field(&roots[i], &text, '\n') || oid_len(&roots[i]) != oid_len(&roots[0]))
			return EINVAL;
	return text.len ? EINVAL : 0;
}

int
git_pin_step (struct git_pin   *pin,
              struct mcp_slot  *slot,
              struct cstr_view *why)
{
	struct child *c = pin->child;
	if (!c)
		return git_pin_spawn(pin, slot, nullptr, 0);
	int const status = child_status(c);
	int e = status < 0 ? EIO
	      : status && !(pin->phase == GIT_PIN_META && status == 1) ? EINVAL : 0;
	if (!e) {
		switch (pin->phase) {
		case GIT_PIN_META:
			pin->metadata = c->out;
			c->out = (struct buf){};
			break;
		case GIT_PIN_REFS:
			pin->view = git_view_create(buf_peek(&pin->metadata), buf_peek(&c->out), &e);
			buf_fini(&pin->metadata);
			break;
		case GIT_PIN_RESOLVE:
			e = git_pin_resolve(pin, buf_peek(&c->out));
			break;
		case GIT_PIN_CHECK:
			e = buf_peek(&c->out).len ? EACCES : 0;
			break;
		case GIT_PIN_DONE:
			break;
		}
	}
	bool const late = c->flags & CHILD_LATE;
	child_destroy(&pin->child);
	if (!e)
		return ++pin->phase == GIT_PIN_DONE ? 0 : git_pin_spawn(pin, slot, nullptr, 0);
	/* rev-parse fails (EINVAL) for a root that is not a commit, and rev-list
	 * prints a commit that is not reachable from the refs (EACCES). Both
	 * errors use the same text, so that a caller cannot tell from the text
	 * whether an unreachable object exists. */
	bool const revision = pin->phase >= GIT_PIN_RESOLVE && (e == EINVAL || e == EACCES);
	*why = late ? CSTR_VIEW("Git history validation timed out.")
	     : revision ? CSTR_VIEW("A revision names no commit that the git tool can read: a "
	                            "name or hash that does not exist, ~N past the first "
	                            "commit, a blob or tree hash (use commit:path), or a "
	                            "commit that HEAD, branches, tags and remote-tracking "
	                            "refs do not reach.")
	     : CSTR_VIEW("Git cannot read the repository.");
	return e;
}

/* GIT_ARG_LIST(X) expands to X(ID, name) for each argument of the git
 * tool. With help:true, git_call_command() returns before it reads OPTIONS
 * and the arguments after it.
 */
#define GIT_ARG_LIST(X)                                                           \
        X(REPO, "repo") X(COMMAND, "command") X(HELP, "help") X(OFFSET, "offset") \
        X(OPTIONS, "options") X(REVISIONS, "revisions") X(PATHS, "paths")         \
        X(PATTERN, "pattern") X(LINE, "line") X(COUNT, "count")

#define GIT_ARG_ENUM(id, ...) GIT_ARG_##id,
enum git_arg : uint8_t { GIT_ARG_LIST(GIT_ARG_ENUM) GIT_ARGS };
#undef GIT_ARG_ENUM

static struct cstr_view const git_arg_names[] = { GIT_ARG_LIST(XL_VIEW) };
#undef GIT_ARG_LIST

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
		return git_wrong(why, "The tool has no argument %.*s.", (int)other.len, other.data);

	if (!call->pin.repo)
		return git_repos_wrong(git, why);
	struct cstr_view const cmd = jb_text(of[GIT_ARG_COMMAND], 1, WORD, nullptr);
	size_t const c = cstr_view_index(git_command_names, array_size(git_command_names), cmd);
	if (c == array_size(git_command_names)) {
		if (cstr_view_equal(cmd, CSTR_VIEW("status")) ||
		    cstr_view_equal(cmd, CSTR_VIEW("ls-files")))
			return git_wrong(why, "git %.*s is unavailable: index and working-tree "
			                 "reads are disabled. Use git ls-tree with "
			                 "options [\"-r\", \"--name-only\"] to list committed "
			                 "files; revisions defaults to HEAD.",
			                 (int)cmd.len, cmd.data);
		return git_wrong(why, "The command argument must be "
		                 XL_JOIN(", ", " or ", GIT_COMMAND_LIST(XL_TEXT))
		                 ". Use help:true with one of them for its options.");
	}
	call->command = c;
	uint64_t const takes = commands[c].takes;
	bool const read = takes & LINES;
	struct json_object *help = of[GIT_ARG_HELP];
	if ((seen >> GIT_ARG_HELP & 1) && !json_object_is_type(help, json_type_boolean))
		return git_wrong(why, "The help argument must be a boolean.");
	if (json_object_get_boolean(help)) {
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
		while (i < lens[l] && jb_text(jb_at(of[l], i), 1, WORD, nullptr).data)
			++i;
		if (i < lens[l])
			return git_wrong(why, "%s[%zu] must be a string of 1 to %d bytes with no "
			                 "NUL.", name, i, WORD);
	}
	/* The counts above keep argv in bounds, so the checks below go on
	 * after a problem. */
	int bad = 0;
	if ((takes & SINGLE) && lens[GIT_ARG_REVISIONS] > 1)
		bad = git_wrong(why, "git %.*s takes at most one revision.", (int)cmd.len,
		                cmd.data);
	if (lens[GIT_ARG_PATHS] && !(takes & FILES))
		bad = git_wrong(why, "git %.*s takes no paths.%s", (int)cmd.len, cmd.data,
		                takes & OBJECT ? " To name a file, give the revision HEAD:path."
		                               : "");
	if ((takes & ONEPATH) && lens[GIT_ARG_PATHS] != 1)
		bad = git_wrong(why, "git blame needs the file as its one path, such as "
		                "\"paths\": [\"src/git.c\"].");
	if ((takes & MODE) && lens[GIT_ARG_OPTIONS] != 1)
		bad = git_wrong(why, "git cat-file needs exactly one option: -p for the "
		                "contents, -t for the type, -s for the size, or -e to "
		                "check that the object exists.");
	struct json_object *pattern = of[GIT_ARG_PATTERN];
	struct cstr_view const pat = jb_text(pattern, 1, WORD, nullptr);
	if (takes & PATTERN) {
		if (!pat.data)
			bad = git_wrong(why, "git grep needs a pattern argument: a string of "
			                "1 to %d bytes.", WORD);
	} else if (pattern) {
		bad = git_wrong(why, "Only git grep takes a pattern.");
	}
	uint64_t numbers = seen & (1U << GIT_ARG_LINE | 1U << GIT_ARG_COUNT);
	if (numbers && !read) {
		bad = git_wrong(why, "Only git read takes line and count. For lines of a "
		                "file, call read with commit:path as the revision.");
		numbers = 0;
	}
	size_t *const values[] = { &call->read.line, &call->read.count };
	call->read.line = 1;
	call->read.count = SIZE_MAX;
	for (size_t i = 0; i < array_size(values); ++i) {
		size_t const k = GIT_ARG_LINE + i;
		if (!(numbers >> k & 1))
			continue;
		uint64_t const v = jb_count(of[k], UINT64_MAX);
		if (!v)
			bad = git_wrong(why, "The %s argument must be a whole number from 1.",
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
		if (e) {
			bad = e;
			continue;
		}
		if (!cstr_view_starts(opt, git_ignore_rev)) {
			argv[k++] = opt.data;
			continue;
		}
		e = git_pin_add(&call->pin, cstr_view_slice(opt, git_ignore_rev.len, SIZE_MAX));
		if (e == ENOMEM)
			return e;
		if (e)
			bad = git_wrong(why, "--ignore-rev needs a commit reference or hash.");
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
		struct git_revision *rev = &call->revisions[i];
		rev->path = cstr_view_slice(s, root, SIZE_MAX);
		char const *range = strstr(s.data, "..");
		if (range && range >= s.data + root)
			range = nullptr;
		endpoints += range != nullptr;
		if (path && (!(takes & OBJECT) ||
		             !util_path_inside(cstr_view_slice(rev->path, 1, SIZE_MAX)))) {
			bad = git_wrong(why, takes & OBJECT
			                ? "revisions[%zu]: git %.*s needs a nonempty relative "
			                  "path after the colon, with no .. component."
			                : takes & FILES
			                ? "revisions[%zu]: git %.*s takes the commit in "
			                  "revisions and the path in paths."
			                : "revisions[%zu]: git %.*s takes commits, not "
			                  "commit:path.", i, (int)cmd.len, cmd.data);
			continue;
		}
		if (range && (!(takes & RANGE) || ((takes & PAIR) && listed != 1))) {
			bad = git_wrong(why, "revisions[%zu]: ranges are allowed only by "
			                "log, or as diff's single A..B or A...B argument.", i);
			continue;
		}
		size_t left = range ? (size_t)(range - s.data) : root;
		rev->root = call->pin.n_roots;
		int e = git_pin_add(&call->pin, cstr_view_slice(s, 0, left));
		if (!e && range) {
			rev->dots = 2 + (range[2] == '.');
			e = git_pin_add(&call->pin, cstr_view_slice(s, left + rev->dots,
			                                           root - left - rev->dots));
		}
		if (e == ENOMEM)
			return e;
		if (e)
			bad = git_wrong(why, "revisions[%zu]: use a commit reference or hash "
			                "with optional ^/~ ancestry. Stash, reflogs, index "
			                "selectors and revision expansions are unavailable.", i);
		argv[k++] = git_derived;
	}
	if (read && !call->revisions[0].path.len)
		bad = git_wrong(why, "git read needs the file as its revision, such as "
		                "\"revisions\": [\"HEAD:src/journal.c\"].");
	if ((takes & PAIR) && endpoints != 2)
		bad = git_wrong(why, "git diff needs two commits, or one explicit "
		                "A..B or A...B range.");
	if (takes & FILES)
		argv[k++] = "--";
	for (size_t i = 0; i < lens[GIT_ARG_PATHS]; ++i) {
		struct cstr_view const s = jb_view(jb_at(of[GIT_ARG_PATHS], i));
		if (!util_path_inside(s) || cstr_view_starts(s, CSTR_VIEW(":(")))
			bad = git_wrong(why, "paths[%zu] must be relative to the repository's "
			                "top, with no .. component or :(...) pathspec magic.", i);
		argv[k++] = s.data;
	}
	argv[k] = nullptr;
	return bad;
}

#undef GIT_COMMAND_LIST
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
		"command", jb_string(git_command_names[command]),
		"description", json_object_new_string(commands[command].description),
		"options", options,
		"option_values", json_object_new_string(
			"Each option is one array entry. Append a value after a trailing =; "
			"replace trailing # with digits or * with a value. For example: "
			"--max-count=20, -n20, -C3. Do not split an option and its value."),
		"pagination", json_object_new_string(takes & LINES
			? "The capture limit is 4 MiB. read gives whole lines, each after "
			  "its number and a tab, as many as count asks and as fit in "
			  "window_bytes. The last line of the output gives the revision and "
			  "line to read on from. Only a line longer than the window is cut, "
			  "and then its note gives the next offset."
			: "The capture limit is 4 MiB. A result takes at most window_bytes; "
			  "a longer output ends with a note that gives the exact next "
			  "offset for the same arguments."),
		"limits", jb_object(
			"options", json_object_new_int(takes & MODE ? 1 : OPTIONS),
			"revisions", json_object_new_int(takes & SINGLE ? 1 :
			                                takes & PAIR ? 2 : REVISIONS),
			"paths", json_object_new_int(takes & ONEPATH ? 1 :
			                            takes & FILES ? PATHS : 0),
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

/* Destroy the git call of @p slot and clear its own word. This kills git
 * if it still runs.
 */
static void
git_drop (struct mcp_slot *slot)
{
	struct git_call *call = *mcp_slot_own(slot);
	git_pin_fini(&call->pin);
	json_object_put(call->args);
	call->args = nullptr;
	free(call);
	*mcp_slot_own(slot) = nullptr;
}

/* Returns a new call of @p git with the arguments @p args, whose pin ends
 * at @p deadline.
 */
static struct git_call *
git_call_create (struct json_object *args,
                 struct git const   *git,
                 uint64_t            deadline,
                 int                *e)
{
	struct git_call *call = malloc(sizeof *call);
	if (!call) {
		*e = ENOMEM;
		return nullptr;
	}
	*call = (struct git_call){};
	git_pin_init(&call->pin, git, jb_text(jb_get(args, "repo"), 1, WORD, nullptr), deadline);
	/* The call reads its arguments after it parks (tool.h). */
	if (args && json_object_deep_copy(args, &call->args, nullptr)) {
		free(call);
		*e = ENOMEM;
		return nullptr;
	}
	*e = 0;
	return call;
}

/**
 * Writes to the text of the pin of @p call the words that replace the
 * git_derived words of argv, in the order of argv: an --ignore-rev option
 * with its commit ID, and for each revision its commit ID with the path,
 * or the range of two commit IDs. Then spawns the command of the call.
 * Returns 0, ENOMEM or the error of mcp_slot_spawn().
 */
static int
git_call_spawn (struct git_call *call,
                struct mcp_slot *slot)
{
	struct oid const *roots = call->pin.roots;
	struct dstr *text = &call->pin.text;
	dstr_clear(text);
	bool ok = true;
	for (size_t i = 0; ok && i < call->revisions[0].root; ++i)
		ok = dstr_printf(text, "%s%s%c", git_ignore_rev.data, roots[i].hex, '\0');
	for (size_t i = 0; ok && i < call->n_revisions; ++i) {
		struct git_revision const *rev = &call->revisions[i];
		struct oid const *root = &roots[rev->root];
		ok = rev->dots ? dstr_printf(text, "%s%.*s%s%c", root->hex, (int)rev->dots, "...",
		                             root[1].hex, '\0')
		               : dstr_printf(text, "%s%.*s%c", root->hex, (int)rev->path.len,
		                             rev->path.data, '\0');
	}
	/* Git can warn on stderr, for example when a branch has the same name
	 * as a commit ID. A read discards stderr so that a warning does not
	 * become a line of the file. */
	return !ok ? ENOMEM : git_pin_spawn(&call->pin, slot, call->argv,
	                                    commands[call->command].takes & LINES ? 0
	                                                                         : CHILD_STDERR);
}

/** Takes the output of the ended git of the pin of @p call, if any, and
 *  spawns the next git: a phase of the pin, or the command of the call once
 *  the pin is done. Ends the call if that fails. The output of the pin is
 *  never sent to the caller.
 */
static void
git_call_advance (struct git_call *call,
                  struct mcp_slot *slot)
{
	struct cstr_view why = {};
	int e = git_pin_step(&call->pin, slot, &why);
	if (!e && !call->pin.child)
		e = git_call_spawn(call, slot);
	if (!e)
		return;
	git_drop(slot);
	char buf[GIT_WHY];
	if (e == ENOMEM)
		mcp_slot_fail(slot, e);
	else
		mcp_slot_tell(slot, why.data ? why : child_spawn_why(e, "git", buf, sizeof buf),
		              TOOL_FAILED);
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
	struct child *c = call->pin.child;
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
	} else if (status > 0 && takes & OBJECT) {
		static char const fmt[] = "No file at that path in commit %s. ls-tree lists "
		                          "the files there.%s";
		static char const hint[] = " If :N after the file name is a line number, "
		                           "call read with the file alone as the revision "
		                           "and \"line\": N.";
		static_assert(sizeof fmt - 4 + OID_SHA256 + sizeof hint - 1 <= sizeof buf,
		              "the note fits");
		struct git_revision const *rev = &call->revisions[0];
		note = takes & (LINES | MODE) && rev->path.len
		     ? util_format_view(buf, sizeof buf, fmt, call->pin.roots[rev->root].hex,
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

/* Write the @p n texts at @p p to the output of @p slot if the step has
 * LOOP_COPY room for all of them. Return 0, EAGAIN if the room is too
 * small, or the error of mcp_slot_write().
 */
static int
git_write (struct mcp_slot        *slot,
           struct cstr_view const *p,
           size_t                  n)
{
	if (loop_room(mcp_slot_loop(slot), LOOP_COPY) < cstr_view_total(p, n))
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
	struct dstr *text = &call->pin.text;
	dstr_clear(text);
	int e = cstr_view_cat(oid_view(&call->pin.roots[rev->root]), text);
	if (!e)
		e = jb_escape_cat(rev->path, text);
	read->rev_len = dstr_len(text);
	/* The header contains the repository name, the commit ID and path from
	 * the revision, and three numbers. The longest footer contains the
	 * revision and one number. */
	size_t reserve = sizeof "[Repository , file  at commit : lines  to  of at least .]\n" +
	                 sizeof "\n\n[To read on, call again with \"revisions\": "
	                        "[\"\"] and \"line\": .]" +
	                 call->pin.repo->name.len + 2 * read->rev_len + 4 * UTIL_DECIMAL;
	read->room = window > reserve ? window - reserve : 0;
	read->last = read->line - 1;
	return e;
}

/* Count the line of the file that begins at @p start, has @p len bytes
 * without its newline, and ends at read->scan.start, after its newline if
 * it has one. Take the line if it is asked for, if it follows the last
 * line taken, and if its number, a tab, its bytes and a newline fit in
 * read->room. The first line asked for is always taken.
 */
static void
git_read_line (struct git_read *read,
               size_t           start,
               size_t           len)
{
	size_t n = ++read->lines;
	if (n == read->line)
		read->from = start;
	char digits[UTIL_DECIMAL];
	size_t size = n == read->last + 1 && n - read->line < read->count
	              ? util_decimal(digits + sizeof digits, n, 1).len + len + 1
	              : SIZE_MAX;
	if (n == read->line || size <= read->room) {
		read->room -= size < read->room ? size : read->room;
		read->to = read->scan.start;
		read->last = n;
	}
}

/* Count the lines of the file @p blob from read->scan.at on, as far as
 * the LOOP_PARSE room of the step on @p loop goes, a last line without a
 * newline too, also when the cut ends it.
 */
static void
git_read_scan (struct git_read *read,
               struct span      blob,
               struct loop     *loop)
{
	struct cstr_view const text = { (char const *)blob.ptr, blob.len };
	for (struct cstr_view line; loop_lines_next(&read->scan, loop, text, &line); )
		git_read_line(read, (size_t)(line.data - text.data), line.len);
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
	struct oid const *commit = &call->pin.roots[call->revisions[0].root];
	size_t const n = oid_len(commit);
	char lines[sizeof ": lines  to  of at least .]\n" + 3 * UTIL_DECIMAL];
	struct cstr_view const p[] = {
		CSTR_VIEW("[Repository "), call->pin.repo->name, CSTR_VIEW(", file "),
		{ dstr_get(&call->pin.text) + n + 1, read->rev_len - n - 1 },
		CSTR_VIEW(" at commit "), oid_view(commit),
		read->total ? util_format_view(lines, sizeof lines, ": lines %zu to %zu of "
		                               "%s%zu.]\n", read->line, read->last,
		                               cut ? "at least " : "", read->total)
		            : CSTR_VIEW(": the file is empty.]"),
	};
	return git_write(slot, p, array_size(p));
}

/* Write the lines of the file @p blob from read->scan.at on to @p slot,
 * each after its number and a tab, as far as the LOOP_COPY room of the
 * step goes. Each line counts LINE_COST in addition to its bytes, so that
 * a step writes a bounded number of lines. Return 0 once all lines are
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
	while (read->scan.at < read->to) {
		size_t room = loop_room(loop, LOOP_COPY);
		struct cstr_view num = read->scan.at == read->scan.start
		                       ? util_decimal(tab, read->lines + 1, 1)
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
		size_t left = read->to - read->scan.at;
		size_t n = left < room - fixed ? left : room - fixed;
		unsigned char const *nl = memchr(blob.ptr + read->scan.at, '\n', n);
		if (nl)
			n = (size_t)(nl - blob.ptr) + 1 - read->scan.at;
		bool ends = nl || n == left;
		size_t taken;
		int e = mcp_slot_write(slot, num, 0, &taken);
		if (!e)
			e = mcp_slot_write(slot, (struct cstr_view){ (char const *)blob.ptr +
			                                             read->scan.at, n },
			                   ends ? 0 : TOOL_MORE, &taken);
		if (e)
			return e;
		read->scan.at += taken;
		if (!ends)
			return EAGAIN;
		loop_work(loop, LOOP_COPY, LINE_COST);
		read->scan.start = read->scan.at;
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
		  { dstr_get(&call->pin.text), read->rev_len }, CSTR_VIEW("\"] and \"line\": "),
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
		util_decimal(n + sizeof n, read->last + !kind, 0),
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
		.repo   = call->pin.repo->name,
		.commit = oid_view(&call->pin.roots[rev->root]),
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
	struct child *c = call->pin.child;
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
	if (read->scan.start < blob.len) {
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
	read->scan = (struct loop_lines){ read->from, read->from };
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
	struct child *c = call->pin.child;
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
	if (call->pin.phase < GIT_PIN_DONE) {
		git_call_advance(call, slot);
		return;
	}
	struct child *c = call->pin.child;
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
	int e;
	struct git_call *call = git_call_create(args, git,
	                                        mcp_slot_loop(slot)->now + git->timeout, &e);
	if (e) {
		mcp_slot_fail(slot, e);
		return;
	}
	*mcp_slot_own(slot) = call;
	struct git_why why = {};
	e = git_call_command(call, git, &why);
	if (e == EINVAL)
		mcp_slot_tell(slot, util_text_view(&why), TOOL_FAILED);
	else if (e)
		mcp_slot_fail(slot, e);
	else if (mcp_slot_offset(slot, args))
		return;
	else if (call->phase == GIT_HELP)
		git_help(slot, call->command);
	else
		git_call_advance(call, slot);
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
