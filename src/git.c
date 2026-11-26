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
#include "git_run.h"
#include "git_view.h"
#include "jb.h"
#include "loop.h"
#include "mcp.h"
#include "oid.h"
#include "plugin.h"
#include "schema.h"
#include "seen.h"
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
	LINE_COST = 32,       /**< LOOP_COPY work that read and grep count for
	                           each line they write, in addition to the
	                           bytes of the line */
	OPTIONS   = 16,       /**< a call's options, at most */
	REVISIONS = 4,        /**< its revisions */
	PATHS     = 32,       /**< its paths */
	WORD      = 1024,     /**< bytes of an option, a revision, a path or a pattern */
	ARGV      = 64,       /**< words of the argument argv, at most */
	/** The most words that git_run_dir_argv() writes before a command:
	 *  GIT_RUN_PREFIX and the options of a repository. */
	PREFIX    = GIT_RUN_PREFIX + array_size((struct git_repo){}.opts) - 1,
};

static_assert(2 * REVISIONS + OPTIONS <= GIT_ROOTS, "the commits that a call names fit a pin");

static_assert((int)WORD <= (int)SEEN_NAME_MAX, "a path that git reads fits a struct "
              "seen_source");

static_assert(TOOL_TEXT_MAX == 4U << 20, "update git.h and the texts in this file that say "
              "4 MiB");

struct git {
	/* The struct git_run_dir of the git processes of the tool, without the
	 * path, the options and the flags. dirs[1] is for a view that contains
	 * HEAD, and dirs[0] for the others. */
	struct git_run_dir dirs[2];
	struct dstr        ceilings;  /* the GIT_CEILING_DIRECTORIES entry of
	                                 the environment of dirs */
	uint64_t           timeout;
	size_t             n;
	struct git_repo    repo[];
};

enum git_phase : uint64_t {
	GIT_RUN,
	GIT_NUMBER,  /* read has scanned the file and writes the numbered lines */
	GIT_HELP,    /* the call describes its command with help:true and runs no git */
	GIT_LINE,    /* a grep of one commit has written the header, and the
	                next byte of the output of git starts a line */
	GIT_COPY,    /* as GIT_LINE, but the next byte is inside a line */
};

/* The bits of git_call.grep: the options -L and -v of a grep call. They
 * are the index of the note when git exits with status 1.
 */
enum {
	GIT_GREP_WITHOUT = 1 << 0,  /* -L or --files-without-match */
	GIT_GREP_INVERT  = 1 << 1,  /* -v or --invert-match */
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

/* The kinds of the words of a call, in the order of the arguments options,
 * revisions and paths.
 */
enum git_kind : uint8_t { GIT_OPTION, GIT_REVISION, GIT_PATH, GIT_KINDS };

static_assert(OPTIONS + REVISIONS + PATHS <= ARGV, "the words of a call are at most ARGV");

/* The options, the revisions and the paths of a call, each kind in the
 * order of the call, and its command and pattern. Each text ends with a
 * NUL and is a string of the arguments of the call, or a string that
 * git_argv() wrote into the member made of the call. Sorting the words of
 * argv allocates nothing but that one text.
 */
struct git_words {
	struct cstr_view word[ARGV];
	struct cstr_view command;   /* data is nullptr if the command is no
	                               string of 1 to WORD bytes */
	struct cstr_view pattern;   /* data is nullptr without a pattern */
	struct cstr_view tree;      /* argv: the directory of the tree
	                               commit:dir of an ls-tree, with a slash at
	                               its end, or data is nullptr */
	uint16_t         n;         /* the words */
	uint8_t          kind[ARGV];         /* enum git_kind of each word */
	uint8_t          of[GIT_KINDS];      /* the words of each kind */
	uint8_t          argv;      /* 1 if the words are from the argument argv */
	uint8_t          patterns;  /* argv: the patterns. A call takes one. */
	uint8_t          maybe;     /* argv: the last maybe revisions are words
	                               that are revisions only if git resolves
	                               them, and the first revision is a HEAD
	                               that git_argv_maybe() added
	                               (git_argv_settle()) */
};

/* Adds the word @p word of the kind @p kind to @p w. The limits of the
 * arguments and of argv keep the words within ARGV.
 */
static void
git_words_add (struct git_words *w,
               enum git_kind     kind,
               struct cstr_view  word)
{
	w->word[w->n] = word;
	w->kind[w->n++] = kind;
	++w->of[kind];
}

/* Returns the next word of the kind @p kind of @p w from the index @p *at
 * on, and moves @p *at past it. The caller reads at most w->of[kind]
 * words.
 */
static struct cstr_view
git_words_next (struct git_words const *w,
                enum git_kind           kind,
                size_t                 *at)
{
	while (w->kind[*at] != kind)
		++*at;
	return w->word[(*at)++];
}

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
	struct git_words       words;
	struct dstr            made;     /* the words that git_argv() wrote, each
	                                    with a NUL: an option with its value, a
	                                    path in a tree, the commit of a tree */
	char const            *argv[GIT_RUN_ARGV];
	size_t                 command;
	size_t                 n_revisions;
	struct git_revision    revisions[REVISIONS + 1];  /* and the HEAD of maybe */
	size_t                 ignored;   /* the roots of the pin before the
	                                     revisions: the values of --ignore-rev */
	struct git_read        read;
	enum git_phase         phase;
	uint64_t               grep;  /* GIT_GREP_WITHOUT and GIT_GREP_INVERT */
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
	O("--merges"), O("--follow"), O("--max-count="), O("-n#"), O("-#"),
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
	O("--max-count="), O("-m#"), O("--break"), O("--heading"), O("--no-heading"),
	O("-p"), O("--show-function"), O("-W"), O("--function-context"), O("--all-match"), {}
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
	/* paths, after "--" */
	GIT_TAKES_FILES = 1 << 0,

	/* --end-of-options, except where git takes it amiss */
	GIT_TAKES_END_OF_OPTIONS = 1 << 1,

	/* a required pattern, after -e */
	GIT_TAKES_PATTERN = 1 << 2,

	/* a committed path after a revision's colon */
	GIT_TAKES_OBJECT = 1 << 3,

	/* a pair of commits separated by .. or ... */
	GIT_TAKES_RANGE = 1 << 4,

	/* exactly two commits, separately or as a range */
	GIT_TAKES_PAIR = 1 << 5,

	/* at most one revision */
	GIT_TAKES_SINGLE = 1 << 6,

	/* the line and count arguments, and numbered lines of one committed
	 * file that cat-file reads
	 */
	GIT_TAKES_LINES = 1 << 7,

	/* exactly one option, which selects what to show */
	GIT_TAKES_MODE = 1 << 8,

	/* exactly one path, the file */
	GIT_TAKES_ONEPATH = 1 << 9,

	/* in argv, a tree as commit:path, which becomes the commit and the
	 * path with a slash at its end
	 */
	GIT_TAKES_TREE = 1 << 10,
};

/* The commands: X(ID, name, options, takes, description, forced...), an
 * ID that no code uses, the name, the options that it accepts, the flags
 * above for what it takes, its help, and the options that askai adds.
 * grep runs in one thread: with --break, -C or -W, a grep in threads
 * drops the line "Binary file ... matches" if that line comes first
 * (git 2.55).
 */
#define GIT_COMMAND_LIST(X)                                                                 \
        X(READ,      "read",      read_options,                                             \
          GIT_TAKES_END_OF_OPTIONS | GIT_TAKES_OBJECT | GIT_TAKES_SINGLE | GIT_TAKES_LINES, \
          "Numbered lines of a committed file. Give the file as the one revision "          \
          "commit:path, such as HEAD:src/journal.c. line is the first line to "             \
          "return. The default line is 1. count is the most lines to return. The "          \
          "default count is as many lines as fit in one result. The first line of "         \
          "the output names the commit. If the file goes on, the output ends with "         \
          "the arguments of the next read. The arguments contain the commit ID in "         \
          "place of HEAD.", "blob")                                                         \
        X(LOG,       "log",       log_options,                                              \
          GIT_TAKES_FILES | GIT_TAKES_END_OF_OPTIONS | GIT_TAKES_RANGE,                     \
          "Committed history. The default revision is HEAD. Paths limit the history.",      \
          "--no-ext-diff", "--no-textconv")                                                 \
        X(SHOW,      "show",      show_options,                                             \
          GIT_TAKES_FILES | GIT_TAKES_END_OF_OPTIONS | GIT_TAKES_OBJECT,                    \
          "A commit with the patch. The default revision is HEAD. The revision "            \
          "commit:path shows a file.", "--no-ext-diff", "--no-textconv")                    \
        X(DIFF,      "diff",      diff_options,                                             \
          GIT_TAKES_FILES | GIT_TAKES_END_OF_OPTIONS | GIT_TAKES_RANGE | GIT_TAKES_PAIR,    \
          "Compare two commits. Give two revisions, or one range A..B or A...B. Paths "     \
          "limit the comparison. --stat or --name-status prints a summary.",                \
          "--no-ext-diff", "--no-textconv")                                                 \
        X(GREP,      "grep",      grep_options,                                             \
          GIT_TAKES_FILES | GIT_TAKES_END_OF_OPTIONS | GIT_TAKES_PATTERN,                   \
          "Search committed files. The default revision is HEAD. The pattern argument "     \
          "is an extended regex. -G, -F and -P select basic regex, plain text and Perl "    \
          "regex. -i ignores case. With one revision, the first line of the output "        \
          "names the commit. Each file path is on a separate line, above the matches "      \
          "as line:text. With several revisions, each path starts with the commit ID "      \
          "and a colon. To read a match, call read with commit:path and the line of "       \
          "the match. --no-heading puts the path before each match. -l lists the "          \
          "files with a match. -L lists the files without a match. -c counts the "          \
          "matching lines in each file. -C3 adds three lines of context. -p adds the "      \
          "first line of the function that contains a match. -W adds the whole "            \
          "function. --max-count=N stops after N matches in a file.",                       \
          "--no-textconv", "-n", "-E", "--heading", "--break", "--threads=1")               \
        X(BLAME,     "blame",     blame_options,                                            \
          GIT_TAKES_FILES | GIT_TAKES_SINGLE | GIT_TAKES_ONEPATH,                           \
          "The commit that last changed each line of a committed file. The default "        \
          "revision is HEAD. Give the file as the one path, such as \"paths\": "            \
          "[\"src/git.c\"]. -L10,30 limits the output to lines 10 to 30. -s leaves out "    \
          "the author and the date.", "--no-textconv")                                      \
        X(LS_TREE,   "ls-tree",   ls_tree_options,                                          \
          GIT_TAKES_FILES | GIT_TAKES_SINGLE | GIT_TAKES_TREE,                              \
          "Entries of a committed tree. The default revision is HEAD. The path \"src/\" "   \
          "lists the entries in src. The path \"src\" lists only src. -r recurses. -d "     \
          "lists only trees.")                                                              \
        X(CAT_FILE,  "cat-file",  cat_file_options,                                         \
          GIT_TAKES_END_OF_OPTIONS | GIT_TAKES_OBJECT | GIT_TAKES_SINGLE | GIT_TAKES_MODE,  \
          "Inspect a committed object. The default revision is HEAD. commit:path names a "  \
          "file. Give exactly one option: -p prints the contents, -t the type, -s the "     \
          "size in bytes. -e only checks that the object exists.")                          \
        X(REV_PARSE, "rev-parse", rev_parse_options, 0,                                     \
          "Print the commit ID of each revision. The default revision is HEAD. --verify "   \
          "requires exactly one valid revision.", "--revs-only")

static struct {
	struct cstr_view const *options;
	char const             *forced[6];  /* the options that askai adds */
	char const             *description;
	uint64_t                takes;
} const commands[] = {
#define GIT_COMMAND_ROW(id, name, options, takes, text, ...) { options, { __VA_ARGS__ }, text, takes },
	GIT_COMMAND_LIST(GIT_COMMAND_ROW)
#undef GIT_COMMAND_ROW
};

static struct cstr_view const git_command_names[] = { GIT_COMMAND_LIST(XL_VIEW) };

static_assert(PREFIX + 1 + array_size(commands[0].forced) + OPTIONS + 2 + 1 + REVISIONS + 1 +
              PATHS + 1 <= GIT_RUN_ARGV, "a command fits argv");
static_assert(PREFIX + 10 + GIT_ROOTS + 1 <= GIT_RUN_ARGV, "history validation fits argv");

/* Sets up @p r for the repository at @p path. The function stores the
 * name, the real path and the safe.directory setting of the repository in
 * one allocation. The options of git for the repository turn off implicit
 * worktree operations and the diff of a submodule, which a superproject's
 * show or log -p runs as a git of its own, without these options. They
 * also make git write path names as they are, not quoted, unless they
 * contain a '"', a '\' or a control character, and set safe.directory.
 */
static int
git_repo_init (struct git_repo *r,
               char const      *path)
{
	static char const safe[] = "safe.directory=";
	struct cstr_view const name = git_run_name(path);
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
	real = nullptr;
	*(char *)mempcpy(copy, name.data, name.len) = '\0';
	*r = (struct git_repo){
		.name = { copy, name.len },
		.path = s,
		.opts = {
			"-c", "core.bare=true",
			"-c", "diff.submodule=short",
			"-c", "diff.autoRefreshIndex=false",
			"-c", "core.quotePath=false",
			"-c", s + size,
		},
	};
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
		struct cstr_view const parent = len ? cstr_view_n(p, len) : CSTR_VIEW("/");
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
	char *env[] = { (char *)dstr_raw(&git->ceilings), "GIT_OPTIONAL_LOCKS=0", nullptr, nullptr };
	e = git_run_dir_init(&git->dirs[0], nullptr, nullptr, env, 0);
	/* git reads the attributes from HEAD in a view that contains HEAD. */
	env[2] = "GIT_ATTR_SOURCE=HEAD";
	return e ? e : git_run_dir_init(&git->dirs[1], nullptr, nullptr, env, 0);
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
	memset(git->dirs, 0, sizeof git->dirs);
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
	struct cstr_view const required[] = { CSTR_VIEW("repo") };
	return jb_object(
		"name", json_object_new_string("git"),
		"title", json_object_new_string("Git"),
		"description", json_object_new_string(
			"Run a read-only git command in a repository of the server. The "
			"result is the output of git with the error messages. The output is "
			"cut at 4 MiB. help:true lists the options and limits of a command. "
			"Only committed files are visible. A commit must be reachable from "
			"HEAD, a branch, a tag or a remote-tracking ref. The default revision "
			"is HEAD. Give diff two commits or one range. To read a file, give "
			"read the file as the revision. Example: {\"command\": \"read\", "
			"\"revisions\": [\"HEAD:src/journal.c\"], \"line\": 120, \"count\": "
			"40} returns lines 120 to 159 with line numbers. If the file goes on, "
			"the read output ends with the arguments of the next read. The "
			"arguments contain the commit ID in place of HEAD. With one revision, "
			"the first line of the grep output names the commit. Each file path "
			"is on a separate line, above the matches as <line>:<text>. To read a "
			"match, call read with \"revisions\": [\"<commit>:<path>\"] and "
			"\"line\": <line>. An option takes its value in the same entry or in "
			"the next: --max-count=20, --max-count 20, -n20 and -n 20 are the same. "
			"A call can give argv in place of command, "
			"options, revisions, paths and pattern. argv is the words of a git "
			"command line after git. Example: {\"argv\": [\"grep\", \"-i\", "
			"\"todo\", \"HEAD~1\", \"--\", \"src/\"]}."),
		"inputSchema", schema_object(
			jb_object(
				"repo", schema_enum(repos, nullptr),
				"command", schema_enum(cmds, nullptr),
				"argv", schema_array(schema_property("string", nullptr), ARGV,
					"The words of a git command line after git: the command, "
					"then options, revisions and paths, as in [\"log\", "
					"\"--oneline\", \"-n\", \"5\", \"HEAD\", \"--\", "
					"\"src/\"]. A word after -- is a path. Before --, a word is "
					"a revision if the word names a commit. The first word "
					"that names no commit is a path, and so is every word "
					"after it, as in [\"grep\", \"todo\", \"main\", "
					"\"src/git.c\"]. For grep, the first word that is no "
					"option is the pattern. Write a path that has the name of "
					"a branch or a tag after --."),
				"help", schema_property("boolean",
					"Describe the command instead of running git."),
				"options", schema_array(schema_property("string", nullptr), OPTIONS,
					"Options, such as --oneline, -n 20 or --max-count=20. The "
					"value of an option is in its entry or in the next entry. "
					"help:true lists the allowed options."),
				"revisions", schema_array(schema_property("string", nullptr), REVISIONS,
					"Commits, such as main, HEAD~3 or a commit hash. Only log "
					"and diff accept a range, such as main..topic. For read, "
					"show and cat-file, HEAD:path names a file. Stash, reflogs, "
					"the index (:path) and blob or tree hashes are not "
					"available."),
				"paths", schema_array(schema_property("string", nullptr), PATHS,
					"Paths relative to the top of the repository, such as "
					"src/. Paths limit the output. Give blame exactly one "
					"path, the file. read, cat-file and rev-parse accept no "
					"paths."),
				"pattern", schema_property("string",
					"The grep pattern, an extended regex. -G, -F and -P select "
					"basic regex, plain text and Perl regex."),
				"offset", schema_count(0, TOOL_OFFSET_ABOUT),
				"line", schema_count(1,
					"The first line that read returns. The default is 1."),
				"count", schema_count(1,
					"The most lines that read returns. The default is as many "
					"as fit in one result."),
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
	*match = cstr_view(nullptr);
	int e = ENOENT;
	for (; allowed->data; ++allowed) {
		size_t const n = allowed->len;
		char const last = allowed->data[n - 1];
		/* -# is -NUMBER, as in log -5. It matches only an option whose
		 * second byte is a digit, so that it is not the match of every
		 * other option that starts with a dash. */
		bool const number = opt.len > 1 && opt.data[1] >= '0' && opt.data[1] <= '9';
		if (n == 2 && last == '#' && !number)
			continue;
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
	struct cstr_view const ref = cstr_view_n(dest, base);
	if (!base || *dest == '-' || *dest == '/' || strstr(dest, "..") ||
	    cstr_view_equal(ref, CSTR_VIEW("stash")) || cstr_view_equal(ref, CSTR_VIEW("refs/stash")))
		return EINVAL;
	for (size_t i = base; i < len;) {
		if (cstr_view_starts(cstr_view_n(dest + i, len - i), suffix)) {
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

/* Appends a part to the message about what is wrong with the arguments of
 * a call, as util_text_printf_() does. The message is @p *len bytes and a
 * NUL in the @p size bytes at @p why. A space is appended before the part
 * unless the message is empty. Returns EINVAL.
 */
#define git_wrong(len, why, size, ...)                                     \
        (util_text_printf_((len), (why), (size), "%s", *(len) ? " " : ""), \
         util_text_printf_((len), (why), (size), __VA_ARGS__), EINVAL)

/** Validates the option @p opt of the command @p command, the option at
 * @p index in the options argument. Returns 0, or EINVAL after it has
 * appended to the message in @p why a part that explains only how to repair
 * the option (git_wrong()).
 */
static int
git_option_check (struct cstr_view  opt,
                  size_t            command,
                  size_t            index,
                  uint32_t         *len,
                  char             *why,
                  size_t            size)
{
	struct cstr_view match = {};
	int e = git_option_find(commands[command].options, opt, &match);
	if (!e)
		return 0;
	int quoted = (int)jb_utf8(opt.data, opt.len < 80 ? opt.len : 80);
	if (match.data) {
		char last = match.data[match.len - 1];
		size_t at = match.len - (last != '=');
		char const *value = last == '#' ? "NUMBER" : "VALUE";
		char const *digits = last == '#' ? ", with decimal digits for NUMBER" : "";
		if (e == ENODATA)
			return git_wrong(len, why, size,
			                 "The value of options[%zu] '%.*s' is missing. Give "
			                 "'%.*s%s', or %s as the next entry%s.", index, quoted,
			                 opt.data, (int)at, match.data, value, value, digits);
		return git_wrong(len, why, size,
		                 "The value of options[%zu] '%.*s' is not a decimal number. "
		                 "Give '%.*s%s'%s.", index, quoted, opt.data, (int)at,
		                 match.data, value, digits);
	}
	if (!git_option_find(source_options, opt, &match)) {
		char const *source = commands[command].takes & GIT_TAKES_PAIR
			? "two commits or one range A..B or A...B in revisions."
			: "a commit in revisions. The default revision is HEAD.";
		return git_wrong(len, why, size,
		                 "options[%zu] '%.*s' is not allowed. Only committed "
		                 "files are available, not the index or the working tree. "
		                 "Remove the option and give %s", index, quoted, opt.data, source);
	}
	return git_wrong(len, why, size, "options[%zu] '%.*s' is not allowed for git %s. help:true "
	                 "lists the allowed options.", index, quoted, opt.data,
	                 git_command_names[command].data);
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

/* Appends to the message of @p *len bytes in the @p size bytes at @p why
 * the names of the repositories that a call can select. Returns EINVAL.
 */
static int
git_repos_wrong (struct git const *git,
                 uint32_t         *len,
                 char             *why,
                 size_t            size)
{
	util_text_printf_(len, why, size, "The repo argument must be ");
	for (size_t i = 0; i < git->n; ++i)
		util_text_printf_(len, why, size, "%s%.*s",
		                  !i ? "" : i + 1 < git->n ? ", " : " or ",
		                  (int)git->repo[i].name.len, git->repo[i].name.data);
	util_text_printf_(len, why, size, ".");
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

void
git_pin_dir (struct git_pin const *pin,
             struct git_run_dir   *dir)
{
	struct git_view const *view = pin->view;
	if (!view) {
		*dir = pin->git->dirs[0];
		dir->path = pin->repo->path;
	} else {
		*dir = pin->git->dirs[git_view_has_head(view)];
		dir->path = git_view_directory(view);
		dir->flags = flag64(GIT_RUN_DOT);
	}
	dir->opts = pin->repo->opts;
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
	/* The words of the command, after the options of git. */
	char const *argv[GIT_RUN_ARGV];
	size_t k = 0;
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
		if (git_view_has_head(pin->view))
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
	struct git_run_dir dir = {};
	git_pin_dir(pin, &dir);
	struct git_run cmd;
	struct child_cmd const how = child_cmd(
		.max = TOOL_TEXT_MAX,
		.deadline = pin->deadline,
		.grace = GIT_GRACE,
		.flags = { flags }
	);
	int e = git_run_init(&cmd, &dir, argv, &how);
	if (!e)
		pin->child = mcp_slot_spawn(slot, &cmd.c, &e);
	return e;
}

/* Replaces each root of @p pin with a commit ID from @p out, the output of
 * rev-parse, which stops at the first root that names no commit. If that
 * root is one of the last pin->n_maybe roots, the function removes it and
 * the roots after it from the pin, and pin->n_maybe becomes the number of
 * these roots that are left. Returns 0, or EINVAL if @p out does not
 * contain one ID of the same length for each other root, and nothing
 * else.
 */
static int
git_pin_resolve (struct git_pin *pin,
                 struct span     out)
{
	struct cstr_view text = cstr_view_span(out);
	struct oid *roots = pin->roots;
	size_t const firm = pin->n_roots - pin->n_maybe;
	size_t n = 0;
	for (; n < pin->n_roots && (n < firm || text.len); ++n)
		if (oid_init_field(&roots[n], &text, '\n') || oid_len(&roots[n]) != oid_len(&roots[0]))
			return EINVAL;
	if (text.len)
		return EINVAL;
	pin->n_maybe = n - firm;
	pin->n_roots = n;
	return 0;
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
			c->out = buf();
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
	bool const late = flag_any(c->end.flags, CHILD_LATE);
	child_destroy(&pin->child);
	if (!e)
		return ++pin->phase == GIT_PIN_DONE ? 0 : git_pin_spawn(pin, slot, nullptr, 0);
	/* rev-parse fails (EINVAL) for a root that is not a commit, and rev-list
	 * prints a commit that is not reachable from the refs (EACCES). Both
	 * errors use the same text, so that a caller cannot tell from the text
	 * whether an unreachable object exists. */
	bool const revision = pin->phase >= GIT_PIN_RESOLVE && (e == EINVAL || e == EACCES);
	*why = late ? CSTR_VIEW("git took too long to check the revisions.")
	     : revision ? CSTR_VIEW("A revision names no commit that the git tool can read. "
	                            "Possible causes: a name or hash that does not exist, ~N "
	                            "past the first commit, a blob or tree hash, or a commit "
	                            "that no branch, tag, remote-tracking ref or HEAD "
	                            "reaches. Give a file as commit:path, not as a blob "
	                            "hash.")
	     : CSTR_VIEW("Git cannot read the repository.");
	return e;
}

void
git_step_failed (struct mcp_slot  *slot,
                 int               e,
                 struct cstr_view  why,
                 char const       *name)
{
	if (e == ENOMEM) {
		mcp_slot_fail(slot, e);
		return;
	}
	if (!why.data) {
		struct tool_scratch *scratch = mcp_slot_scratch(slot);
		why = child_spawn_why(e, name, scratch->text, sizeof scratch->text);
	}
	mcp_slot_tell(slot, why, TOOL_FAILED);
}

/* GIT_ARG_LIST(X) expands to X(ID, name) for each argument of the git
 * tool. With help:true, git_call_command() returns before it reads OPTIONS
 * and the arguments after it.
 */
#define GIT_ARG_LIST(X)           \
        X(REPO,      "repo")      \
        X(COMMAND,   "command")   \
        X(HELP,      "help")      \
        X(OFFSET,    "offset")    \
        X(OPTIONS,   "options")   \
        X(REVISIONS, "revisions") \
        X(PATHS,     "paths")     \
        X(PATTERN,   "pattern")   \
        X(LINE,      "line")      \
        X(COUNT,     "count")     \
        X(ARGV,      "argv")

#define GIT_ARG_ENUM(id, ...) GIT_ARG_##id,
enum git_arg : uint8_t { GIT_ARG_LIST(GIT_ARG_ENUM) GIT_ARGS };
#undef GIT_ARG_ENUM

static struct cstr_view const git_arg_names[] = { GIT_ARG_LIST(XL_VIEW) };
#undef GIT_ARG_LIST

/* Returns how the option @p word of argv takes the next word of argv as its
 * value, by the options @p allowed of the command: '=' for a long option,
 * as in --max-count 20, '#' for a short option, as in -n 20, or 0 if
 * @p word is whole as it is or is no option of the command.
 */
static char
git_argv_value (struct cstr_view const *allowed,
                struct cstr_view        word)
{
	char how = 0;
	for (; allowed->data; ++allowed) {
		if (cstr_view_equal(*allowed, word))
			return 0;
		if (allowed->len != word.len + 1 || memcmp(allowed->data, word.data, word.len))
			continue;
		char const last = allowed->data[word.len];
		how = last == '=' ? '=' : last == '#' || last == '*' ? '#' : how;
	}
	return how;
}

/* Appends the texts @p part and a NUL to call->made, and returns the new
 * string. Before the first string, the function reserves @p room bytes,
 * which is room for every string that the words of argv can make, so that
 * a later string does not move an earlier one. Returns a view whose data
 * is nullptr if out of memory.
 */
static struct cstr_view
git_argv_make (struct git_call        *call,
               size_t                  room,
               struct cstr_view const  part[static 3])
{
	struct dstr *d = &call->made;
	if (!dstr_len(d) && !dstr_reserve(d, room))
		return cstr_view(nullptr);
	size_t const at = dstr_len(d);
	for (size_t i = 0; i < 3; ++i)
		if (part[i].len)
			(void)dstr_cat_n(d, part[i].data, part[i].len);
	(void)dstr_cat_n(d, "", 1);
	return cstr_view_n(dstr_get(d) + at, dstr_len(d) - at - 1);
}

/* Adds the tree @p word of an ls-tree, commit:dir with the colon at
 * @p colon, to the words of @p call: the commit as a revision, and the
 * directory with a slash at its end as the tree of the words
 * (git_argv_tree()). commit: is the tree of the commit and has no
 * directory. Returns 1, or 0 if out of memory.
 */
static size_t
git_argv_tree_word (struct git_call  *call,
                    struct cstr_view  word,
                    char const       *colon,
                    size_t            room)
{
	struct git_words *w = &call->words;
	size_t const at = (size_t)(colon - word.data);
	struct cstr_view const dir = cstr_view_slice(word, at + 1, SIZE_MAX);
	struct cstr_view const head[3] = { cstr_view_n(word.data, at) };
	struct cstr_view const commit = git_argv_make(call, room, head);
	if (!commit.data)
		return 0;
	git_words_add(w, GIT_REVISION, commit);
	if (!dir.len)
		return 1;
	/* A directory with a slash at its end is the end of the word. */
	struct cstr_view const tail[3] = { dir, CSTR_VIEW("/") };
	w->tree = dir.data[dir.len - 1] == '/' ? dir : git_argv_make(call, room, tail);
	return w->tree.data != nullptr;
}

/* Adds the word @p i of @p argv, a word before --, to the words of @p call
 * for the command @p c: an option, with the next word as its value if the
 * option takes one (git_argv_value()); the pattern of a grep, which is the
 * word after -e or the first other word; a tree commit:dir of ls-tree
 * (git_argv_tree_word()); a path, if the word contains *, ? or [ or ends
 * with a slash, which no name of a revision does; or else a revision. The
 * function reads no file: the tool shows only committed trees, so the
 * files of a working tree do not decide what a word is. Returns the number
 * of words that the function read, 1 or 2, or 0 if out of memory.
 */
static size_t
git_argv_word (struct git_call    *call,
               size_t              c,
               struct json_object *argv,
               size_t              i,
               size_t              room)
{
	struct git_words *w = &call->words;
	struct cstr_view const word = jb_view(jb_at(argv, i));
	struct cstr_view const next = jb_view(jb_at(argv, i + 1));
	uint64_t const takes = commands[c].takes;
	bool const dash_e = (takes & GIT_TAKES_PATTERN) && cstr_view_equal(word, CSTR_VIEW("-e"));
	if (word.len > 1 && word.data[0] == '-') {
		if (next.data && dash_e) {
			w->pattern = next;
			++w->patterns;
			return 2;
		}
		char const how = next.data ? git_argv_value(commands[c].options, word) : 0;
		struct cstr_view const joined[3] = {
			word, how == '=' ? CSTR_VIEW("=") : cstr_view(nullptr), next
		};
		struct cstr_view const option = how ? git_argv_make(call, room, joined) : word;
		if (!option.data)
			return 0;
		git_words_add(w, GIT_OPTION, option);
		return how ? 2 : 1;
	}
	if ((takes & GIT_TAKES_PATTERN) && !w->patterns) {
		w->pattern = word;
		++w->patterns;
		return 1;
	}
	char const *const colon = takes & GIT_TAKES_TREE ? memchr(word.data, ':', word.len)
	                                                 : nullptr;
	if (colon)
		return git_argv_tree_word(call, word, colon, room);
	git_words_add(w, strpbrk(word.data, "*?[") || word.data[word.len - 1] == '/'
	                 ? GIT_PATH : GIT_REVISION, word);
	return 1;
}

/* Puts the paths of an ls-tree of the tree commit:dir into the directory
 * w->tree: git reads the paths of such a call in that tree, so each path
 * becomes dir/path, and a call without paths lists dir/. Returns 0 or
 * ENOMEM.
 */
static int
git_argv_tree (struct git_call *call,
               size_t           room)
{
	struct git_words *w = &call->words;
	if (!w->of[GIT_PATH]) {
		git_words_add(w, GIT_PATH, w->tree);
		return 0;
	}
	for (size_t i = 0; i < w->n; ++i) {
		if (w->kind[i] != GIT_PATH)
			continue;
		struct cstr_view const inside[3] = { w->tree, w->word[i] };
		w->word[i] = git_argv_make(call, room, inside);
		if (!w->word[i].data)
			return ENOMEM;
	}
	return 0;
}

/* Makes paths of the revisions of @p w after the first @p keep. A command
 * that takes a fixed number of revisions reads the words after them as
 * paths, as git does.
 */
static void
git_argv_rest (struct git_words *w,
               size_t            keep)
{
	for (size_t i = 0; i < w->n; ++i) {
		if (w->kind[i] != GIT_REVISION)
			continue;
		if (keep) {
			--keep;
			continue;
		}
		w->kind[i] = GIT_PATH;
		--w->of[GIT_REVISION];
		++w->of[GIT_PATH];
	}
}

/* Returns whether the word @p word of argv can be a path as well as a
 * revision: a relative path inside the repository, without the characters
 * of the syntax of revisions, that is no abbreviated commit ID. Only a ref
 * of the view of the pin can have such a name, so the answer of git to it
 * says nothing about a commit that no ref reaches.
 */
static bool
git_argv_plain (struct cstr_view word)
{
	size_t const hex = strspn(word.data, "0123456789abcdefABCDEF");
	return util_path_inside(word) && !strpbrk(word.data, "^~@{}:") &&
	       !strstr(word.data, "..") && (hex < word.len || hex < 4);
}

/* Counts in w->maybe the last revisions of @p w that can be paths
 * (git_argv_plain()), at most REVISIONS. git resolves the revisions in
 * their order and reads the first word that names no commit, and the words
 * after it, as paths (git_argv_settle()). If there are such revisions, the
 * function puts HEAD before the words: the call runs on HEAD if git reads
 * all its words as paths. The words of argv without its command are fewer
 * than ARGV, so there is room for HEAD.
 */
static void
git_argv_maybe (struct git_words *w)
{
	size_t plain = 0;
	for (size_t i = 0; i < w->n; ++i)
		if (w->kind[i] == GIT_REVISION)
			plain = git_argv_plain(w->word[i]) ? plain + 1 : 0;
	if (!plain || w->of[GIT_REVISION] > REVISIONS)
		return;
	memmove(w->word + 1, w->word, w->n * sizeof *w->word);
	memmove(w->kind + 1, w->kind, w->n * sizeof *w->kind);
	w->word[0] = CSTR_VIEW("HEAD");
	w->kind[0] = GIT_REVISION;
	++w->n;
	++w->of[GIT_REVISION];
	w->maybe = (uint8_t)plain;
}

/* Makes paths of the last revisions of an argv call that git did not
 * resolve, once the pin is done (git_argv_maybe()): in call->argv, the
 * word -- moves before their words, which take the places of their
 * revisions and of the old --. revisions[0] is the HEAD that
 * git_argv_maybe() added. The call uses it only if no other revision is
 * left.
 */
static void
git_argv_settle (struct git_call *call)
{
	struct git_words const *w = &call->words;
	char const **base = call->argv;
	while (*base != git_derived)
		++base;
	size_t const n = call->n_revisions;
	size_t const kept = n - w->maybe + call->pin.n_maybe;
	size_t at = 0;
	for (size_t i = 0; i < kept; ++i)
		(void)git_words_next(w, GIT_REVISION, &at);
	base[kept] = "--";
	for (size_t i = kept; i < n; ++i)
		base[i + 1] = git_words_next(w, GIT_REVISION, &at).data;
	call->n_revisions = kept;
	call->words.maybe = 0;
	if (kept == 1)
		return;
	memmove(call->revisions, call->revisions + 1, (kept - 1) * sizeof *call->revisions);
	--call->n_revisions;
	size_t rest = 0;
	while (base[rest])
		++rest;
	memmove(base, base + 1, rest * sizeof *base);
}

/* Appends to the refusal of an argv call in the @p size bytes at @p why,
 * @p *len bytes long, how the words of argv were read: the command, the
 * options, the revisions, the paths and the pattern, as the JSON of the
 * other arguments of the tool. Out of memory, the function appends
 * nothing.
 */
static void
git_argv_read (struct git_call const *call,
               uint32_t              *len,
               char                  *why,
               size_t                 size)
{
	struct git_words const *w = &call->words;
	struct json_object *lists[GIT_KINDS] = {
		json_object_new_array(), json_object_new_array(), json_object_new_array()
	};
	for (size_t i = 0; i < w->n; ++i)
		(void)jb_push(lists[w->kind[i]], jb_string(w->word[i]));
	struct json_object *read = jb_object("command", jb_string(w->command),
	                                     "options", lists[GIT_OPTION],
	                                     "revisions", lists[GIT_REVISION],
	                                     "paths", lists[GIT_PATH], nullptr);
	if (read && w->pattern.data)
		read = jb_set_or_free(read, "pattern", jb_string(w->pattern));
	struct cstr_view const text = jb_wire(read);
	if (text.data)
		util_text_printf_(len, why, size, " The words of argv were read as %.*s.",
		                  (int)text.len, text.data);
	json_object_put(read);
}

/* Sorts the words of the argument @p argv, the words of a git command line
 * after "git", into the words of @p call: the command, then options,
 * revisions and paths (git_argv_word()), and after a word -- only paths.
 * Without --, the words after the revisions that the command takes are
 * paths: blame takes a revision and then the file, ls-tree a commit and
 * then paths, and diff two commits or a range and then paths. log, show
 * and grep take any number of revisions and then paths
 * (git_argv_maybe()). @p seen has the bits of the arguments of the call.
 * Returns 0, ENOMEM, or EINVAL with a message in @p why (git_wrong()).
 */
static int
git_argv (struct git_call    *call,
          struct json_object *argv,
          uint64_t            seen,
          uint32_t           *len,
          char               *why,
          size_t              size)
{
	static_assert(ARGV == 64 && WORD == 1024, "the message says");
	struct git_words *w = &call->words;
	size_t const n = jb_len(argv);
	size_t total = 0;
	size_t longest = 0;
	size_t i = 0;
	for (struct cstr_view word;
	     i < n && (word = jb_text(jb_at(argv, i), 1, WORD, nullptr)).data; ++i) {
		total += word.len;
		longest = word.len > longest ? word.len : longest;
	}
	if (!n || n > ARGV || i < n)
		return git_wrong(len, why, size,
		                 "The argv argument must be an array of 1 to 64 strings of 1 to "
		                 "1024 bytes with no NUL: the words of a git command line after "
		                 "git, such as [\"log\", \"--oneline\", \"-n\", \"5\", \"HEAD\", "
		                 "\"--\", \"src/\"].");
	uint64_t const replaced = 1U << GIT_ARG_COMMAND | 1U << GIT_ARG_OPTIONS |
	                          1U << GIT_ARG_REVISIONS | 1U << GIT_ARG_PATHS |
	                          1U << GIT_ARG_PATTERN;
	if (seen & replaced)
		return git_wrong(len, why, size, "Give argv without command, options, revisions, "
		                 "paths and pattern.");
	/* A word "git" before the command is not a word of the command. */
	i = n > 1 && cstr_view_equal(jb_view(jb_at(argv, 0)), CSTR_VIEW("git"));
	w->argv = 1;
	w->command = jb_view(jb_at(argv, i));
	size_t const c = cstr_view_index(git_command_names, array_size(git_command_names),
	                                 w->command);
	/* git_call_command() refuses an unknown command, and the words after
	 * it are not read. */
	if (c == array_size(git_command_names))
		return 0;
	uint64_t const takes = commands[c].takes;
	/* Each word is a part of at most one string of call->made, with at
	 * most 2 more bytes, and a path of a tree is also a part of dir/path. */
	size_t const room = total + 2 * n +
	                    (takes & GIT_TAKES_TREE ? total + n * (longest + 1) : 0);
	/* paths becomes true at the word --. */
	bool paths = false;
	for (size_t read = 1; (i += read) < n; ) {
		struct cstr_view const word = jb_view(jb_at(argv, i));
		read = 1;
		if (paths)
			git_words_add(w, GIT_PATH, word);
		else if (cstr_view_equal(word, CSTR_VIEW("--")))
			paths = true;
		else if (!(read = git_argv_word(call, c, argv, i, room)))
			return ENOMEM;
	}
	size_t const revisions = w->of[GIT_REVISION];
	size_t at = 0;
	char const *const first = revisions ? git_words_next(w, GIT_REVISION, &at).data : "";
	size_t const keep =
		paths ? revisions
		: (takes & GIT_TAKES_ONEPATH) && !w->of[GIT_PATH] ? revisions > 1
		: (takes & GIT_TAKES_FILES) && (takes & GIT_TAKES_SINGLE) ? 1
		: takes & GIT_TAKES_PAIR ? 2 - !!strstr(first, "..")
		: revisions;
	git_argv_rest(w, keep);
	if (w->tree.data && git_argv_tree(call, room))
		return ENOMEM;
	if (!paths && (takes & GIT_TAKES_FILES) && !(takes & (GIT_TAKES_SINGLE | GIT_TAKES_PAIR)))
		git_argv_maybe(w);
	if (w->patterns > 1)
		return git_wrong(len, why, size, "Give git grep one pattern. Run one search for "
		                 "each pattern, or join the patterns with | in one extended "
		                 "regex.");
	return 0;
}

/*
 * Validate the call and retain its arguments and revision roots. Returns 0,
 * ENOMEM, or EINVAL with a message about what is wrong with the arguments.
 * The message is @p *len bytes and a NUL in the @p size bytes at @p why.
 * The caller sets @p *len to 0 before the call.
 */
static int
git_call_command (struct git_call  *call,
                  struct git const *git,
                  uint32_t         *len,
                  char             *why,
                  size_t            size)
{
	struct json_object *args = call->args;
	if (!args)
		return git_wrong(len, why, size, "Tool arguments must be an object.");
	struct json_object *of[GIT_ARGS] = {};
	uint64_t seen;
	struct cstr_view other = {};
	if (jb_args(args, git_arg_names, GIT_ARGS, UINT64_MAX, of, &seen, &other))
		return git_wrong(len, why, size, "%.*s is not an argument of the git tool.",
		                 (int)other.len, other.data);

	if (!call->pin.repo)
		return git_repos_wrong(git, len, why, size);
	struct git_words *w = &call->words;
	if (seen >> GIT_ARG_ARGV & 1) {
		int const e = git_argv(call, of[GIT_ARG_ARGV], seen, len, why, size);
		if (e)
			return e;
	} else {
		w->command = jb_text(of[GIT_ARG_COMMAND], 1, WORD, nullptr);
	}
	struct cstr_view const cmd = w->command;
	size_t const c = cstr_view_index(git_command_names, array_size(git_command_names), cmd);
	if (c == array_size(git_command_names)) {
		if (cstr_view_equal(cmd, CSTR_VIEW("status")) ||
		    cstr_view_equal(cmd, CSTR_VIEW("ls-files")))
			return git_wrong(len, why, size,
			                 "git %.*s is not available. Only committed files "
			                 "are visible. To list the committed files, call "
			                 "ls-tree with \"options\": [\"-r\", \"--name-only\"].",
			                 (int)cmd.len, cmd.data);
		return git_wrong(len, why, size, "The command argument must be "
		                 XL_JOIN(", ", " or ", GIT_COMMAND_LIST(XL_TEXT))
		                 ". help:true lists the options of a command.");
	}
	call->command = c;
	uint64_t const takes = commands[c].takes;
	bool const read = takes & GIT_TAKES_LINES;
	struct json_object *help = of[GIT_ARG_HELP];
	if ((seen >> GIT_ARG_HELP & 1) && !json_object_is_type(help, json_type_boolean))
		return git_wrong(len, why, size, "The help argument must be a boolean.");
	if (json_object_get_boolean(help)) {
		call->phase = GIT_HELP;
		return 0;
	}

	static size_t const most[GIT_ARGS] = {
		[GIT_ARG_OPTIONS] = OPTIONS, [GIT_ARG_REVISIONS] = REVISIONS,
		[GIT_ARG_PATHS] = PATHS,
	};
	/* The options, the revisions and the paths are the arrays of the
	 * arguments, which become the words of the call here, or the words that
	 * git_argv() sorted. git_argv() made some of its words, so they are
	 * checked here as well. */
	static_assert(GIT_ARG_REVISIONS == GIT_ARG_OPTIONS + GIT_REVISION &&
	              GIT_ARG_PATHS == GIT_ARG_OPTIONS + GIT_PATH);
	size_t lens[GIT_ARGS] = {};
	for (size_t l = GIT_ARG_OPTIONS; l <= GIT_ARG_PATHS; ++l) {
		enum git_kind const kind = (enum git_kind)(l - GIT_ARG_OPTIONS);
		char const *const name = git_arg_names[l].data;
		if (!w->argv && (seen >> l & 1) && !json_object_is_type(of[l], json_type_array))
			return git_wrong(len, why, size,
			                 "The %s argument must be an array of at most %zu "
			                 "strings.", name, most[l]);
		lens[l] = w->argv ? w->of[kind] : jb_len(of[l]);
		/* An argv call with maybe revisions has one more, HEAD. */
		if (lens[l] > most[l] + (kind == GIT_REVISION && w->maybe))
			return git_wrong(len, why, size,
			                 "The %s argument can contain at most %zu entries, "
			                 "not %zu.", name, most[l], lens[l]);
		size_t at = 0;
		for (size_t i = 0; i < lens[l]; ++i) {
			struct cstr_view const word = w->argv ? git_words_next(w, kind, &at)
			                            : jb_text(jb_at(of[l], i), 1, WORD, nullptr);
			if (!word.data || !word.len || word.len > WORD)
				return git_wrong(len, why, size,
				                 "%s[%zu] must be a string of 1 to %d bytes with "
				                 "no NUL.", name, i, WORD);
			if (!w->argv)
				git_words_add(w, kind, word);
		}
	}
	/* The counts above keep argv in bounds, so the checks below go on
	 * after a problem. */
	int bad = 0;
	if ((takes & GIT_TAKES_SINGLE) && lens[GIT_ARG_REVISIONS] > 1)
		bad = git_wrong(len, why, size, "Give git %.*s at most one revision.", (int)cmd.len,
		                cmd.data);
	if (lens[GIT_ARG_PATHS] && !(takes & GIT_TAKES_FILES))
		bad = git_wrong(len, why, size,
		                "Paths are not allowed for git %.*s.%s", (int)cmd.len,
		                cmd.data,
		                takes & GIT_TAKES_OBJECT ? " To name a file, give the revision "
		                                           "HEAD:path." : "");
	if ((takes & GIT_TAKES_ONEPATH) && lens[GIT_ARG_PATHS] != 1)
		bad = git_wrong(len, why, size, "Give git blame the file as the one path, such as "
		                "\"paths\": [\"src/git.c\"].");
	if ((takes & GIT_TAKES_MODE) && lens[GIT_ARG_OPTIONS] != 1)
		bad = git_wrong(len, why, size,
		                "Give git cat-file exactly one option: -p prints the "
		                "contents, -t the type, -s the size. -e checks that the "
		                "object exists.");
	if (!w->argv)
		w->pattern = jb_text(of[GIT_ARG_PATTERN], 1, WORD, nullptr);
	struct cstr_view const pat = w->pattern;
	if (takes & GIT_TAKES_PATTERN) {
		if (!pat.data)
			bad = git_wrong(len, why, size,
			                "Give git grep a pattern argument of 1 to %d bytes.", WORD);
	} else if (of[GIT_ARG_PATTERN]) {
		bad = git_wrong(len, why, size, "The pattern argument is only for git grep.");
	}
	uint64_t numbers = seen & (1U << GIT_ARG_LINE | 1U << GIT_ARG_COUNT);
	if (numbers && !read) {
		bad = git_wrong(len, why, size,
		                "The line and count arguments are only for git read. To "
		                "get lines of a file, call read with the revision "
		                "commit:path.");
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
			bad = git_wrong(len, why, size,
			                "The %s argument must be a whole number from 1.",
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
	 * checked as it goes in. An entry of the argument options takes the
	 * value of the option from the next entry, as a command line does:
	 * -n 20 is -n20. git_argv_word() joined the words of argv already. */
	size_t room = 0;
	for (size_t i = 0; !w->argv && i < w->n; ++i)
		room += w->kind[i] == GIT_OPTION ? w->word[i].len + 2 : 0;
	size_t at = 0;
	for (size_t i = 0; i < lens[GIT_ARG_OPTIONS]; ++i) {
		size_t const entry = i;
		struct cstr_view opt = git_words_next(w, GIT_OPTION, &at);
		char const how = !w->argv && i + 1 < lens[GIT_ARG_OPTIONS]
		               ? git_argv_value(commands[c].options, opt) : 0;
		if (how) {
			struct cstr_view const joined[3] = {
				opt, how == '=' ? CSTR_VIEW("=") : cstr_view(nullptr),
				git_words_next(w, GIT_OPTION, &at)
			};
			++i;
			opt = git_argv_make(call, room, joined);
			if (!opt.data)
				return ENOMEM;
		}
		int e = git_option_check(opt, c, entry, len, why, size);
		if (e) {
			bad = e;
			continue;
		}
		if (cstr_view_equal(opt, CSTR_VIEW("-L")) ||
		    cstr_view_equal(opt, CSTR_VIEW("--files-without-match")))
			call->grep |= GIT_GREP_WITHOUT;
		if (cstr_view_equal(opt, CSTR_VIEW("-v")) ||
		    cstr_view_equal(opt, CSTR_VIEW("--invert-match")))
			call->grep |= GIT_GREP_INVERT;
		if (!cstr_view_starts(opt, git_ignore_rev)) {
			argv[k++] = opt.data;
			continue;
		}
		e = git_pin_add(&call->pin, cstr_view_slice(opt, git_ignore_rev.len, SIZE_MAX));
		if (e == ENOMEM)
			return e;
		if (e)
			bad = git_wrong(len, why, size,
			                "The value of --ignore-rev must be a commit name or hash.");
		argv[k++] = git_derived;
	}
	if (pat.data) {
		argv[k++] = "-e";
		argv[k++] = pat.data;
	}
	if (takes & GIT_TAKES_END_OF_OPTIONS)
		argv[k++] = "--end-of-options";
	call->ignored = call->pin.n_roots;
	size_t const listed = lens[GIT_ARG_REVISIONS];
	size_t endpoints = listed;
	call->n_revisions = listed ? listed : 1;
	at = 0;
	for (size_t i = 0; i < call->n_revisions; ++i) {
		struct cstr_view const s = listed ? git_words_next(w, GIT_REVISION, &at)
		                                  : CSTR_VIEW("HEAD");
		char const *path = strchr(s.data, ':');
		size_t root = path ? (size_t)(path - s.data) : s.len;
		struct git_revision *rev = &call->revisions[i];
		rev->path = cstr_view_slice(s, root, SIZE_MAX);
		char const *range = strstr(s.data, "..");
		if (range && range >= s.data + root)
			range = nullptr;
		endpoints += range != nullptr;
		if (path && (!(takes & GIT_TAKES_OBJECT) ||
		             !util_path_inside(cstr_view_slice(rev->path, 1, SIZE_MAX)))) {
			bad = git_wrong(len, why, size, takes & GIT_TAKES_OBJECT
			                ? "In revisions[%zu], give git %.*s a relative path "
			                  "after the colon, with no .. component."
			                : takes & GIT_TAKES_FILES
			                ? "In revisions[%zu], give git %.*s only the commit. "
			                  "Give the path in paths. A directory path ends with "
			                  "a slash, such as src/."
			                : "In revisions[%zu], give git %.*s a commit, not "
			                  "commit:path.", i, (int)cmd.len, cmd.data);
			continue;
		}
		if (range &&
		    (!(takes & GIT_TAKES_RANGE) || ((takes & GIT_TAKES_PAIR) && listed != 1))) {
			bad = git_wrong(len, why, size,
			                "In revisions[%zu], a range is allowed only for "
			                "log and diff. Give diff a range only as the one "
			                "revision, A..B or A...B.", i);
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
			bad = git_wrong(len, why, size,
			                "revisions[%zu] must be a commit name or hash, with "
			                "optional ^ and ~ suffixes. Stash, reflogs, index "
			                "selectors and revision expansions are not available.", i);
		argv[k++] = git_derived;
	}
	if (read && !call->revisions[0].path.len)
		bad = git_wrong(len, why, size, "Give git read the file as the revision, such as "
		                "\"revisions\": [\"HEAD:src/journal.c\"].");
	if ((takes & GIT_TAKES_PAIR) && endpoints != 2)
		bad = git_wrong(len, why, size, "Give git diff two commits, or one range A..B or "
		                "A...B.");
	if (takes & GIT_TAKES_FILES)
		argv[k++] = "--";
	at = 0;
	for (size_t i = 0; i < lens[GIT_ARG_PATHS]; ++i) {
		struct cstr_view const s = git_words_next(w, GIT_PATH, &at);
		if (!util_path_inside(s) || cstr_view_starts(s, CSTR_VIEW(":(")))
			bad = git_wrong(len, why, size,
			                "paths[%zu] must be relative to the top of the "
			                "repository, with no .. component and no :(...) pathspec "
			                "magic.", i);
		argv[k++] = s.data;
	}
	argv[k] = nullptr;
	/* The roots of the maybe revisions are the last roots of the pin. */
	call->pin.n_maybe = w->maybe;
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
			"An option ending in = takes a value after the =, or in the next "
			"entry. An option ending in # takes digits in place of the #, or "
			"the digits in the next entry. An option ending in * takes any "
			"value in place of the *, or in the next entry. Examples: "
			"--max-count=20, --max-count 20, -n20, -n 20, -5, -C3."),
		"pagination", json_object_new_string(takes & GIT_TAKES_LINES
			? "git output is cut at 4 MiB. read returns at most count whole lines in "
			  "at most window_bytes. Each line starts with the line number and a "
			  "tab. If the file goes on, the output ends with the revision and the "
			  "line to read on from. A line longer than window_bytes is cut. The "
			  "result then ends with a note that gives the next offset."
			: "git output is cut at 4 MiB. A result is at most window_bytes. A "
			  "longer output ends with a note that names the exact next offset for "
			  "the same arguments."),
		"limits", jb_object(
			"options", json_object_new_int(takes & GIT_TAKES_MODE ? 1 : OPTIONS),
			"revisions", json_object_new_int(takes & GIT_TAKES_SINGLE ? 1 :
			                                takes & GIT_TAKES_PAIR ? 2 : REVISIONS),
			"paths", json_object_new_int(takes & GIT_TAKES_ONEPATH ? 1 :
			                            takes & GIT_TAKES_FILES ? PATHS : 0),
			"argument_bytes", json_object_new_int(WORD),
			"capture_bytes", json_object_new_int(TOOL_TEXT_MAX),
			"window_bytes", json_object_new_int64((int64_t)mcp_slot_window_max(slot)),
			nullptr),
		"pattern_required", json_object_new_boolean(takes & GIT_TAKES_PATTERN),
		nullptr);
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
	dstr_cleanup(&call->made);
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
	*call = (struct git_call){
		.made = DSTR_INIT,
	};
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
	for (size_t i = 0; ok && i < call->ignored; ++i)
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
	                                    commands[call->command].takes & GIT_TAKES_LINES ? 0
	                                                                         : CHILD_CMD_STDERR);
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
	if (!e && !call->pin.child && call->words.maybe)
		git_argv_settle(call);
	if (!e && !call->pin.child)
		e = git_call_spawn(call, slot);
	if (!e)
		return;
	/* A word of argv that had to name a commit may be a path that was
	 * written before --. The refusal says how the words were read. */
	struct tool_scratch *scratch = mcp_slot_scratch(slot);
	uint32_t len = 0;
	if (e == EINVAL && why.len && call->words.argv) {
		util_text_printf_(&len, scratch->text, sizeof scratch->text, "%.*s In argv, write "
		                  "a path after --.", (int)why.len, why.data);
		git_argv_read(call, &len, scratch->text, sizeof scratch->text);
		why = cstr_view_n(scratch->text, len);
	}
	git_drop(slot);
	git_step_failed(slot, e, why, "git");
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
 * after the output. Then it finishes the call.
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
	struct cstr_view note = child_end_why(&c->end, "git", git->timeout, c->max, buf,
	                                      sizeof buf);
	int const status = child_status(c);
	bool failed = true;
	if (flag_get(c->end.flags, CHILD_CUT | CHILD_LATE) == CHILD_CUT) {
		note = CSTR_VIEW("The output is cut at 4 MiB.");
		failed = c->end.code == CLD_EXITED && c->end.status;
	} else if (!status) {
		note = out.len ? cstr_view(nullptr) : CSTR_VIEW("No output.");
		failed = false;
	} else if (status == 1 && (takes & GIT_TAKES_PATTERN)) {
		/* grep exits with status 1 when it writes no line. The meaning
		 * of no line depends on -L and -v. */
		static struct cstr_view const none[] = {
			[0]                = CSTR_VIEW_INIT("No matches."),
			[GIT_GREP_WITHOUT] = CSTR_VIEW_INIT("Every file contains a match."),
			[GIT_GREP_INVERT]  = CSTR_VIEW_INIT("Every line matches."),
			[GIT_GREP_WITHOUT | GIT_GREP_INVERT] =
				CSTR_VIEW_INIT("Every file contains a line "
				               "that does not match."),
		};
		note = none[call->grep];
		failed = false;
	} else if (status > 0 && takes & GIT_TAKES_OBJECT) {
		static char const fmt[] = "No file at the path in commit %s. ls-tree lists "
		                          "the files of the commit.%s";
		static char const hint[] = " If :N after the file name is a line number, "
		                           "give the revision without :N and \"line\": N.";
		static_assert(sizeof fmt - 4 + OID_SHA256 + sizeof hint - 1 <= sizeof buf,
		              "the note fits");
		struct git_revision const *rev = &call->revisions[0];
		note = takes & (GIT_TAKES_LINES | GIT_TAKES_MODE) && rev->path.len
		     ? util_format_view(buf, sizeof buf, fmt, call->pin.roots[rev->root].hex,
		                        git_line_hint(rev->path.data + 1) ? hint : "")
		     : util_format_view(buf, sizeof buf, "git could not read the committed "
		                        "object. git exited with status %d.", status);
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
	size_t reserve = sizeof "[Repository , file  at commit . Lines  to  of at least .]\n" +
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
	struct cstr_view const text = cstr_view_span(blob);
	for (struct cstr_view line = {}; loop_lines_next(&read->scan, loop, text, &line); )
		git_read_line(read, (size_t)(line.data - text.data), line.len);
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
	char lines[sizeof ". Lines  to  of at least .]\n" + 3 * UTIL_DECIMAL];
	struct cstr_view const p[] = {
		CSTR_VIEW("[Repository "), call->pin.repo->name, CSTR_VIEW(", file "),
		cstr_view_n(dstr_get(&call->pin.text) + n + 1, read->rev_len - n - 1),
		CSTR_VIEW(" at commit "), oid_view(commit),
		read->total ? util_format_view(lines, sizeof lines, ". Lines %zu to %zu of "
		                               "%s%zu.]\n", read->line, read->last,
		                               cut ? "at least " : "", read->total)
		            : CSTR_VIEW(". The file is empty.]"),
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
			e = mcp_slot_write(slot,
			                   cstr_view_n((char const *)blob.ptr + read->scan.at, n),
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
		  cstr_view_n(dstr_get(&call->pin.text), read->rev_len),
		  CSTR_VIEW("\"] and \"line\": "), CSTR_VIEW(".]") },
		{ CSTR_VIEW("[Line "), cstr_view(nullptr), cstr_view(nullptr),
		  CSTR_VIEW(" is the end of the file.]") },
		{ CSTR_VIEW("[Line "), cstr_view(nullptr), cstr_view(nullptr),
		  CSTR_VIEW(" is the end of the file. The file does not end in a newline.]") },
		{ CSTR_VIEW("[The file is cut after line "), cstr_view(nullptr), cstr_view(nullptr),
		  CSTR_VIEW(", at the 4 MiB limit of the git tool.]") },
		{ CSTR_VIEW("[Line "), cstr_view(nullptr), cstr_view(nullptr),
		  CSTR_VIEW(" is cut at the 4 MiB limit of the git tool.]") },
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
 * read->head to @p end in the output, for the ring of the calls. If the 4 MiB
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
	struct seen_source const src = {
		.repo   = call->pin.repo->name,
		.commit = call->pin.roots[rev->root],
		.path   = cstr_view_slice(rev->path, 1, SIZE_MAX),
		.output = { read->head, (part ? read->mark : end) - read->head },
		.lines  = { read->line, read->last - part },
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
			mcp_slot_tell(slot, CSTR_VIEW("The file contains a NUL byte. read "
			                              "accepts only text files. cat-file with "
			                              "-s prints the size of the file."),
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
		/* git_drop() frees the call, and the read is a part of the call. */
		size_t const line = read->line;
		size_t const total = read->total;
		bool const cut = flag_any(c->end.flags, CHILD_CUT);
		git_drop(slot);
		if (!total)
			mcp_slot_tellf(slot, TOOL_FAILED,
			               "Line %zu is past the end of the file. The "
			               "file is empty.", line);
		else if (cut)
			mcp_slot_tellf(slot, TOOL_FAILED,
			               "Line %zu is past the 4 MiB limit of the git tool.", line);
		else
			mcp_slot_tellf(slot, TOOL_FAILED,
			               "Line %zu is past the end of the file. Give a "
			               "line from 1 to %zu.", line, total);
		return;
	}
	read->scan = loop_lines(read->from, read->from);
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
	bool const cut = flag_any(c->end.flags, CHILD_CUT);
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

/* Copy the output of git in @p c to the output of the call in @p slot, as
 * far as the LOOP_COPY room of the step goes, and make it UTF-8 text. A
 * character that the end of the room cuts is left to the next step.
 * Return 0 once all of the output is copied, EAGAIN, or the error of
 * mcp_slot_write().
 */
static int
git_copy (struct mcp_slot *slot,
          struct child    *c)
{
	struct span left = buf_peek(&c->out);
	size_t room = loop_room(c->loop, LOOP_COPY);
	size_t n = left.len < room ? left.len : room;
	int e = mcp_slot_write(slot, cstr_view_n((char const *)left.ptr, n),
	                       n < left.len ? TOOL_MORE : 0, &n);
	if (e)
		return e;
	buf_consume(&c->out, n);
	return n < left.len ? EAGAIN : 0;
}

/* Copy the output of the grep of one commit in @p call to @p slot as
 * git_copy() does, after a header line that names the repository and the
 * commit. The commit ID and the colon after it are left out at the start
 * of each line, and after "Binary file " at the start of a line. Each line
 * counts LINE_COST in addition to the bytes of the line, and a line can go
 * on from one step to the next, as in git_read_lines(). Return 0 once all
 * of the output is copied, EAGAIN, or the error of mcp_slot_write().
 */
static int
git_grep_copy (struct git_call *call,
               struct mcp_slot *slot)
{
	static struct cstr_view const binary = CSTR_VIEW_INIT("Binary file ");
	struct oid const *commit = &call->pin.roots[call->revisions[0].root];
	if (call->phase == GIT_RUN) {
		struct cstr_view const p[] = {
			CSTR_VIEW("[Repository "), call->pin.repo->name, CSTR_VIEW(", commit "),
			oid_view(commit), CSTR_VIEW("]\n"),
		};
		int e = git_write(slot, p, array_size(p));
		if (e)
			return e;
		call->phase = GIT_LINE;
	}
	char id[sizeof commit->hex + 1];
	struct cstr_view const prefix = util_format_view(id, sizeof id, "%s:", commit->hex);
	struct child *c = call->pin.child;
	struct loop *loop = c->loop;
	for (struct span left; (left = buf_peek(&c->out)).len; ) {
		/* Write a piece only if LINE_COST, "Binary file " and 4 more bytes
		 * fit in the room of the step. The work of the step then stays
		 * within the bound, and the piece contains at least one byte of
		 * the line, as in git_read_lines(). */
		size_t room = loop_room(loop, LOOP_COPY);
		if (room < LINE_COST + binary.len + 4)
			return EAGAIN;
		room -= LINE_COST;
		struct cstr_view text = cstr_view_span(left);
		size_t taken;
		if (call->phase == GIT_LINE) {
			call->phase = GIT_COPY;
			size_t const at = cstr_view_starts(text, binary) ? binary.len : 0;
			if (cstr_view_starts(cstr_view_slice(text, at, SIZE_MAX), prefix)) {
				int e = at ? mcp_slot_write(slot, binary, 0, &taken) : 0;
				if (e)
					return e;
				buf_consume(&c->out, at + prefix.len);
				text = cstr_view_slice(text, at + prefix.len, SIZE_MAX);
				room -= at;
			}
		}
		size_t n = text.len < room ? text.len : room;
		char const *nl = memchr(text.data, '\n', n);
		if (nl)
			n = (size_t)(nl - text.data) + 1;
		bool const ends = nl || n == text.len;
		int e = mcp_slot_write(slot, cstr_view_n(text.data, n),
		                       ends ? 0 : TOOL_MORE, &taken);
		if (e)
			return e;
		buf_consume(&c->out, taken);
		if (!ends)
			return EAGAIN;
		loop_work(loop, LOOP_COPY, LINE_COST);
		call->phase = GIT_LINE;
	}
	return 0;
}

/* Copy the output of git to the output of the call in @p slot with
 * git_copy(), or with git_grep_copy() for a grep of one commit. When all
 * of the output is copied, end the call, in the next step if the room left
 * is too small for the note. A read numbers the lines of the file instead.
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
	if ((takes & GIT_TAKES_OBJECT) && c->end.code == CLD_EXITED && c->end.status) {
		git_ended(slot, call, takes);
		return;
	}
	/* When the output goes past 4 MiB, git is killed. A read then writes
	 * the lines before the cut, as it does after a git that exited in
	 * time. */
	if (takes & GIT_TAKES_LINES) {
		uint32_t const end = flag_get(c->end.flags, CHILD_CUT | CHILD_LATE);
		if (end ? end != CHILD_CUT : c->end.code != CLD_EXITED)
			git_ended(slot, call, takes);
		else if (call->phase == GIT_RUN)
			git_scan(slot, call);
		else
			git_number(slot, call);
		return;
	}
	int e = (takes & GIT_TAKES_PATTERN) && call->n_revisions == 1 ? git_grep_copy(call, slot)
	                                                              : git_copy(slot, c);
	if (e == EAGAIN || (!e && loop_room(c->loop, LOOP_COPY) < GIT_NOTE)) {
		mcp_slot_ready(slot);
		return;
	}
	if (e) {
		mcp_slot_fail(slot, e);
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
	struct git_call *call = git_call_create(args, git,
	                                        mcp_slot_loop(slot)->now + git->timeout, &e);
	if (e) {
		mcp_slot_fail(slot, e);
		return;
	}
	*mcp_slot_own(slot) = call;
	struct tool_scratch *scratch = mcp_slot_scratch(slot);
	uint32_t len = 0;
	e = git_call_command(call, git, &len, scratch->text, sizeof scratch->text);
	/* The refusals name the arguments that git_argv() made from argv. */
	if (e == EINVAL && call->words.argv)
		git_argv_read(call, &len, scratch->text, sizeof scratch->text);
	if (e == EINVAL)
		mcp_slot_tell(slot, cstr_view_n(scratch->text, len), TOOL_FAILED);
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
