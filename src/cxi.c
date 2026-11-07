/** @file cxi.c
 *
 * askai-code, the helper program of the code tool. A function that returns
 * an int returns 0, EINVAL with a tool error in the answer, or another
 * error number.
 */
#define _GNU_SOURCE /* clearenv() */
#include <clang-c/Index.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <json-c/json.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cdb.h"
#include "child.h"
#include "clk.h"
#include "compat.h"
#include "confine.h"
#include "cstr_view.h"
#include "cxi.h"
#include "dstr.h"
#include "jb.h"
#include "oid.h"
#include "tool.h"
#include "util.h"
#include "xlist.h"

enum {
	CXI_FILES = 100'000,  /* the most files and directories of a commit */
	CXI_LIST  = 2 << 20,  /* the most output of ls-tree */
	CXI_CDB   = 64 << 20, /* the largest compile_commands.json */
	CXI_DEPTH = 16,       /* the deepest nesting of its JSON */
};

/* The largest total size of the files of a commit. */
static constexpr uint64_t CXI_BYTES = UINT64_C(1) << 30;

/* The git processes of an export get SIGKILL this long before askai kills
 * askai-code, so that none of them holds the lock of the cache after that.
 */
static constexpr uint64_t CXI_EARLY = 200 * MS;

static struct cstr_view const cxi_keys[] = { CXI_KEY_LIST(XL_VIEW) };
static struct cstr_view const cxi_actions[] = { CXI_ACTION_LIST(XL_VIEW) };

/* An output of askai-code: the answer or rows of it. A text from clang or
 * from the commit can contain any bytes, so the functions that append one
 * escape control bytes and bytes that are not UTF-8.
 */
struct cxi_out {
	struct dstr      text;
	struct cstr_view tree;  /* the tree of the commit with a trailing slash,
	                           which paths in the output lose */
	int64_t          e;     /* ENOMEM after an append failed */
};

/* A call of askai-code: the request from its arguments, the answer, and the
 * parse of the file.
 */
struct cxi {
	struct cxi_out     out;
	struct cstr_view   key[CXI_KEYS];  /* each value, or data nullptr */
	char const        *file;           /* the path of the file in the tree */
	CXTranslationUnit  tu;
	uint64_t           action;         /* enum cxi_action */
};

static void
cxi_out_put (struct cxi_out *out,
             char const     *p,
             size_t          n)
{
	if (!out->e && n && !dstr_cat_n(&out->text, p, n))
		out->e = ENOMEM;
}

/* Appends askai-code's own text, as printf() formats it, to @p out. */
[[gnu::format(printf, 2, 3)]]
static void
cxi_out_printf (struct cxi_out *out,
                char const     *fmt,
                ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	size_t n = util_vformat(buf, sizeof buf, fmt, ap);
	va_end(ap);
	cxi_out_put(out, buf, n);
}

/* Returns the length of the UTF-8 character at @p p, or 0 for a control
 * character or bytes that are not UTF-8.
 */
static size_t
cxi_char (char const *p,
          size_t      n)
{
	unsigned char const c = (unsigned char)*p;
	size_t const k = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
	if (c < 0x20 || c == 0x7f || k > n || (c == 0xc2 && (unsigned char)p[1] < 0xa0))
		return 0;
	return k == 1 || jb_utf8(p, k) == k ? k : 0;
}

/* Appends @p s from clang, the commit or the request without the path of
 * the tree and the column of a place, as in (unnamed at src/x.h:7:2), with
 * control bytes and bytes that are not UTF-8 as \n, \t or \xNN.
 */
static void
cxi_out_text (struct cxi_out *out,
              char const     *s)
{
	size_t const n = strlen(s);
	for (size_t i = 0; i < n; ) {
		size_t const col = s[i] == ':' && i && s[i - 1] >= '0' && s[i - 1] <= '9'
		                   ? strspn(s + i + 1, "0123456789") : 0;
		struct cstr_view const rest = { s + i, n - i };
		size_t const skip = out->tree.len && cstr_view_starts(rest, out->tree) ? out->tree.len
		                  : col && s[i + 1 + col] == ')' ? 1 + col : 0;
		size_t k;
		if (skip) {
			i += skip;
		} else if ((k = cxi_char(s + i, n - i))) {
			cxi_out_put(out, s + i, k);
			i += k;
		} else {
			unsigned char const c = (unsigned char)s[i++];
			cxi_out_printf(out, c == '\n' ? "\\n" : c == '\t' ? "\\t" : "\\x%02x", c);
		}
	}
}

/* Appends the path or request word @p p, quoted as git quotes a path that
 * contains '"', '\\', a control character or bytes that are not UTF-8.
 */
static void
cxi_out_path (struct cxi_out   *out,
              struct cstr_view  p)
{
	size_t i = 0;
	for (size_t k; i < p.len && (k = cxi_char(p.data + i, p.len - i)) &&
	               !memchr("\"\\", p.data[i], 2); i += k)
		;
	size_t const quote = i < p.len;
	cxi_out_put(out, "\"", quote);
	cxi_out_put(out, p.data, i);
	while (i < p.len) {
		unsigned char const c = (unsigned char)p.data[i];
		size_t k = 1;
		if (c == '"' || c == '\\')
			cxi_out_printf(out, "\\%c", c);
		else if (c >= '\a' && c <= '\r')
			cxi_out_printf(out, "\\%c", "abtnvfr"[c - '\a']);
		else if ((k = cxi_char(p.data + i, p.len - i)))
			cxi_out_put(out, p.data + i, k);
		else
			cxi_out_printf(out, "\\%03o", c);
		i += k ? k : 1;
	}
	cxi_out_put(out, "\"", quote);
}

static void
cxi_out_clang (struct cxi_out *out,
               CXString        s)
{
	char const *c = clang_getCString(s);
	cxi_out_text(out, c ? c : "");
	clang_disposeString(s);
}

static unsigned
cxi_line (CXSourceLocation loc)
{
	unsigned line;
	clang_getExpansionLocation(loc, nullptr, &line, nullptr, nullptr);
	return line;
}

static bool
cxi_tag (enum CXCursorKind k)
{
	return k == CXCursor_StructDecl || k == CXCursor_UnionDecl || k == CXCursor_EnumDecl;
}

/* Returns the word for the kind of @p c, or nullptr for a kind to skip. */
static char const *
cxi_kind (CXCursor c)
{
	static char const *const words[] = {
		[CXCursor_StructDecl] = "struct", [CXCursor_UnionDecl] = "union",
		[CXCursor_EnumDecl] = "enum", [CXCursor_FunctionDecl] = "function",
		[CXCursor_VarDecl] = "variable", [CXCursor_TypedefDecl] = "typedef",
	};
	enum CXCursorKind const k = clang_getCursorKind(c);
	return k == CXCursor_MacroDefinition ? "macro"
	     : (size_t)k < array_size(words) ? words[k] : nullptr;
}

/* Cursors that a visitor collects; a failed push sets out->e. */
struct cxi_list {
	CXCursor       *c;
	size_t          n;
	size_t          cap;
	struct cxi_out *out;
};

static bool
cxi_list_push (struct cxi_list *list,
               CXCursor         c)
{
	if (list->n == list->cap) {
		size_t const cap = list->cap ? 2 * list->cap : 64;
		CXCursor *more = realloc(list->c, cap * sizeof *more);
		if (!more) {
			list->out->e = ENOMEM;
			return false;
		}
		list->c = more;
		list->cap = cap;
	}
	list->c[list->n++] = c;
	return true;
}

/* Collects the names that the main file declares or defines. */
static enum CXChildVisitResult
cxi_declared (CXCursor     c,
              CXCursor,
              CXClientData data)
{
	enum CXCursorKind const k = clang_getCursorKind(c);
	bool const skip = !cxi_kind(c) ||
	                  !clang_Location_isFromMainFile(clang_getCursorLocation(c)) ||
	                  (cxi_tag(k) && !clang_isCursorDefinition(c)) ||
	                  (k == CXCursor_MacroDefinition && clang_Cursor_isMacroBuiltin(c));
	return skip || cxi_list_push(data, c) ? CXChildVisit_Continue : CXChildVisit_Break;
}

/* Orders cursors by place: the visit gives the macros first. */
static int
cxi_order (void const *a,
           void const *b)
{
	unsigned x;
	unsigned y;
	clang_getExpansionLocation(clang_getCursorLocation(*(CXCursor const *)a), nullptr, nullptr,
	                           nullptr, &x);
	clang_getExpansionLocation(clang_getCursorLocation(*(CXCursor const *)b), nullptr, nullptr,
	                           nullptr, &y);
	return (x > y) - (x < y);
}

static int
cxi_outline (struct cxi *x)
{
	struct cxi_list list = { .out = &x->out };
	clang_visitChildren(clang_getTranslationUnitCursor(x->tu), cxi_declared, &list);
	if (list.n)
		qsort(list.c, list.n, sizeof *list.c, cxi_order);
	cxi_out_printf(&x->out, "[%s at %s: ", x->key[CXI_KEY_REPO].data,
	               x->key[CXI_KEY_COMMIT].data);
	cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
	cxi_out_printf(&x->out, ", %zu name%s.]\n", list.n, util_s(list.n));
	for (size_t i = 0; i < list.n; ++i) {
		CXCursor const c = list.c[i];
		enum CXCursorKind const k = clang_getCursorKind(c);
		CXSourceRange const extent = clang_getCursorExtent(c);
		unsigned const first = cxi_line(clang_getRangeStart(extent));
		unsigned const last = cxi_line(clang_getRangeEnd(extent));
		cxi_out_printf(&x->out, "%u %s%s ", cxi_line(clang_getCursorLocation(c)),
		               clang_isDeclaration(k) &&
		               clang_Cursor_getStorageClass(c) == CX_SC_Static ? "static " : "",
		               cxi_kind(c));
		if (cxi_tag(k) && clang_Cursor_isAnonymous(c))
			cxi_out_printf(&x->out, "(anonymous)");
		else
			cxi_out_clang(&x->out, clang_getCursorSpelling(c));
		CXType const type = k == CXCursor_TypedefDecl ? clang_getTypedefDeclUnderlyingType(c)
		                                              : clang_getCursorType(c);
		if (k == CXCursor_FunctionDecl || k == CXCursor_VarDecl ||
		    k == CXCursor_TypedefDecl) {
			cxi_out_printf(&x->out, ": ");
			cxi_out_clang(&x->out, clang_getTypeSpelling(type));
		}
		if (k == CXCursor_FunctionDecl && !clang_isCursorDefinition(c))
			cxi_out_printf(&x->out, ", declared");
		if (last > first)
			cxi_out_printf(&x->out, " (%u-%u)", first, last);
		cxi_out_printf(&x->out, "\n");
	}
	free(list.c);
	return (int)x->out.e;
}

/* Reads the words KEY=VALUE from @p *at up to "--", and moves past it. */
static int
cxi_words (struct cxi  *x,
           int          argc,
           char       **argv,
           int         *at)
{
	for (; *at < argc && strcmp(argv[*at], "--"); ++*at) {
		struct cstr_view value = cstr_view(argv[*at]);
		struct cstr_view key = {};
		size_t const k = cstr_view_cut(&value, '=', &key)
		                 ? cstr_view_index(cxi_keys, CXI_KEYS, key) : CXI_KEYS;
		bool const once = k < CXI_KEYS && !x->key[k].data &&
		                  (k != CXI_KEY_UNCONFINED || cstr_view_equal(value, CSTR_VIEW("1")));
		size_t i = 0;
		while (i < value.len && (unsigned char)value.data[i] >= 0x20 && value.data[i] != 0x7f)
			++i;
		if (!once) {
			cxi_out_path(&x->out, cstr_view(argv[*at]));
			cxi_out_printf(&x->out, " is not a word that askai-code takes: it takes each "
			               "key once and unconfined=1.");
			return EINVAL;
		}
		if (i < value.len) {
			cxi_out_printf(&x->out, "%s has a control character.", cxi_keys[k].data);
			return EINVAL;
		}
		x->key[k] = value;
	}
	*at += *at < argc;
	return 0;
}

/* Makes the directory @p path and its parents with the mode 0700, opens it
 * as @p *fd and checks that it is private, with the reason in @p *why.
 */
static int
cxi_dir (char         *path,
         int          *fd,
         char const  **why)
{
	*fd = -1;
	int e = 0;
	for (char *p = path + 1; !e; ++p) {
		if (*p && *p != '/')
			continue;
		char const c = *p;
		*p = '\0';
		e = mkdir(path, 0700) && errno != EEXIST ? errno : 0;
		*p = c;
		if (!c)
			break;
	}
	struct stat st;
	if (!e && ((*fd = open(path, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0 ||
	           fstat(*fd, &st)))
		e = errno;
	*why = e ? strerror(e) : util_private(&st, S_IFDIR);
	return e ? e : *why ? EACCES : 0;
}

/* askai-code --check: makes the cache, confines the process to test that,
 * and writes the version of libclang and the confinement.
 */
static int
cxi_check (struct cxi *x)
{
	struct cstr_view const cache = x->key[CXI_KEY_CACHE];
	struct dstr dir = DSTR_INIT;
	char const *why = cstr_view_starts(cache, CSTR_VIEW("/")) ? nullptr : "it is not absolute";
	int fd = -1;
	int e = why ? EINVAL : cstr_view_store(cache, &dir) ? ENOMEM
	      : cxi_dir((char *)dstr_raw(&dir), &fd, &why);
	util_close(&fd);
	dstr_cleanup(&dir);
	if (e) {
		cxi_out_printf(&x->out, "cache ");
		cxi_out_path(&x->out, cache);
		cxi_out_printf(&x->out, ": %s", why ? why : strerror(e));
		return EINVAL;
	}
	if (x->key[CXI_KEY_UNCONFINED].data) {
		cxi_out_clang(&x->out, clang_getClangVersion());
		cxi_out_printf(&x->out, "\nnone (--code-unconfined)\n");
		return 0;
	}
	int const abi = confine_abi();
	e = confine_cage(nullptr);
	if (e) {
		cxi_out_printf(&x->out, "%s; --code-unconfined runs askai-code without Landlock and "
		               "seccomp", !abi ? "no Landlock" : e == ENOTSUP
		               ? "no seccomp filter for this architecture" : strerror(e));
		return EINVAL;
	}
	cxi_out_clang(&x->out, clang_getClangVersion());
	cxi_out_printf(&x->out, "\nLandlock ABI %d, seccomp\n", abi);
	return 0;
}

/* Checks the action and the path. */
static int
cxi_valid (struct cxi *x)
{
	struct cstr_view const path = x->key[CXI_KEY_PATH];
	x->action = cstr_view_index(cxi_actions, CXI_ACTIONS, x->key[CXI_KEY_ACTION]);
	struct cstr_view rest = path;
	struct cstr_view part = {};
	int e = !path.len || *path.data == '/' ? EINVAL : 0;
	while (!e && !(e = util_path_next(&rest, &part)) && part.len)
		;
	if (x->action == CXI_ACTIONS) {
		cxi_out_printf(&x->out, "action must be "
		               XL_JOIN(", ", " or ", CXI_ACTION_LIST(XL_TEXT)) ".");
	} else if (memchr(path.data, ':', path.len)) {
		cxi_out_printf(&x->out, "Give the commit as revision and the path alone.");
	} else if (e) {
		cxi_out_printf(&x->out, "path must be relative to the repository's top, with no .. "
		               "component.");
	} else if (!cstr_view_ends(path, CSTR_VIEW(".c")) && !cstr_view_ends(path, CSTR_VIEW(".h"))) {
		cxi_out_path(&x->out, path);
		cxi_out_printf(&x->out, " is not a C file; path must be a .c or .h file.");
	} else {
		return 0;
	}
	return EINVAL;
}

/* Writes the clang command line of the file to @p words (cdb_command())
 * and points x->file at its last word, the file in @p tree.
 */
static int
cxi_command (struct cxi       *x,
             struct cstr_view  tree,
             struct dstr      *words)
{
	char const *const repo = x->key[CXI_KEY_REPO].data;
	char const *const file = x->key[CXI_KEY_CDB].data;
	int fd = open(file, O_RDONLY | O_CLOEXEC);
	struct stat st;
	int e = fd < 0 || fstat(fd, &st) ? errno
	      : !S_ISREG(st.st_mode) || st.st_size > CXI_CDB ? EFBIG : 0;
	struct json_object *cmds = e ? nullptr : json_object_from_fd_ex(fd, CXI_DEPTH);
	util_close(&fd);
	if (!cmds) {
		cxi_out_printf(&x->out, "The compile commands of %s cannot be read: ", repo);
		cxi_out_path(&x->out, cstr_view(file));
		cxi_out_printf(&x->out, ": %s.", e == EFBIG ? "it is not a regular file of at most "
		                                              "64 MiB"
		                                 : e ? strerror(e) : "it is not JSON");
		return EINVAL;
	}
	struct cdb_map const map = { x->key[CXI_KEY_TOP], cstr_view_slice(tree, 0, tree.len - 1) };
	e = cdb_command(&map, cmds, x->key[CXI_KEY_PATH], words);
	json_object_put(cmds);
	char const *last = e ? nullptr : memrchr(dstr_get(words), '\0', dstr_len(words) - 1);
	x->file = last ? last + 1 : dstr_get(words);
	if (e == ENOENT) {
		cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
		cxi_out_printf(&x->out, " is not in the compile commands of %s.", repo);
	} else if (e == EINVAL) {
		cxi_out_printf(&x->out, "The compile commands of %s have no entry with arguments "
		               "for a file of the repository: ", repo);
		cxi_out_path(&x->out, cstr_view(file));
		cxi_out_printf(&x->out, ".");
	}
	return e == ENOENT ? EINVAL : e;
}

/* The cache of the repository of a call: the directory <cache>/<repo>/,
 * open as fd, with the exported tree in tree/, the file lock, which the git
 * processes of an export keep open so that the lock outlives a killed
 * askai-code while they run, and the file commit, open as state, with the
 * ID of the commit in the tree, or "-" while an export runs.
 */
struct cxi_cache {
	struct dstr        dir;
	struct dstr        tree;
	char const *const *git;       /* the git command line up to the
	                                 subcommand, n_git words */
	uint64_t           deadline;  /* of the git processes, in loop_clock()
	                                 time */
	int                n_git;
	int                fd;
	int                lock;
	int                state;
};

/* Runs git with @p words and the lock kept open, its output into @p buf
 * or /dev/null. Returns EIO for an exit status other than 0.
 */
static int
cxi_cache_git (struct cxi_cache const *cache,
               char const *const      *words,
               char                   *buf,
               size_t                  size,
               size_t                 *len)
{
	char const *argv[256];
	int e = child_argv(argv, array_size(argv), cache->git, (size_t)cache->n_git, words);
	extern char **environ;
	struct child_cmd const cmd = {
		.file = "git", .argv = argv, .envp = environ, .deadline = cache->deadline
	};
	struct child_fds fds = child_fds();
	fds.keep = cache->lock;
	int status = 0;
	if (!e)
		e = buf ? child_capture(&cmd, fds, buf, size, len, &status)
		        : child_run(&cmd, fds, &status);
	return e ? e : status ? EIO : 0;
}

static int
cxi_cache_remove (char const        *path,
                  struct stat const *,
                  int                type,
                  struct FTW        *)
{
	return (type == FTW_DP ? rmdir(path) : unlink(path)) ? errno : 0;
}

static int
cxi_cache_set_state (struct cxi_cache const *cache,
                     struct cstr_view        text)
{
	return pwrite(cache->state, text.data, text.len, 0) != (ssize_t)text.len ||
	       ftruncate(cache->state, (off_t)text.len) ? errno ? errno : EIO : 0;
}

/* Exports @p commit with git read-tree, which rewrites only the files that
 * differ. If @p was, the file commit, is no commit ID, an export was cut
 * short, and the tree and the index go first.
 */
static int
cxi_cache_export (struct cxi_cache *cache,
                  struct cstr_view  commit,
                  struct cstr_view  was,
                  struct cxi_out   *out)
{
	char const *const count[] = {
		"ls-tree", "-r", "-t", "-z", "--format=%(objectsize)", commit.data, nullptr
	};
	char const *const export[] = {
		"-c", "core.symlinks=false", "-c", "core.autocrlf=false", "read-tree", "--reset",
		"-u", commit.data, nullptr
	};
	char *list = malloc(CXI_LIST);
	int e = list ? 0 : ENOMEM;
	if (!e && !oid_valid(was)) {
		e = nftw(dstr_get(&cache->tree), cxi_cache_remove, 16, FTW_DEPTH | FTW_PHYS) &&
		    errno != ENOENT ? errno : 0;
		for (char const *const *f = (char const *const[]){ "index", "index.lock", nullptr };
		     !e && *f; ++f)
			e = unlinkat(cache->fd, *f, 0) && errno != ENOENT ? errno : 0;
	}
	if (!e && mkdirat(cache->fd, "tree", 0700) && errno != EEXIST)
		e = errno;
	size_t len = 0;
	if (!e)
		e = cxi_cache_git(cache, count, list, CXI_LIST, &len);
	size_t files = 0;
	uint64_t bytes = 0;
	/* The size of a submodule is "-", which util_digits() reads as 0. */
	for (char *p = list, *z; p && (z = memchr(p, '\0', len - (size_t)(p - list)));
	     p = z + 1, ++files) {
		uint64_t size;
		(void)util_digits(p, (size_t)(z - p), UINT64_MAX, &size);
		bytes += size;
	}
	free(list);
	static_assert(CXI_FILES == 100'000 && CXI_BYTES == 1 << 30, "update the text of the limits");
	if (e == EFBIG || (!e && (files > CXI_FILES || bytes > CXI_BYTES))) {
		cxi_out_printf(out, "Commit %s has more than 100000 files and directories or more "
		               "than 1 GiB, which the code tool reads at most.", commit.data);
		return EINVAL;
	}
	if (!e)
		e = cxi_cache_set_state(cache, CSTR_VIEW("-"));
	if (!e)
		e = cxi_cache_git(cache, export, nullptr, 0, nullptr);
	if (e == EIO || e == ETIMEDOUT) {
		cxi_out_printf(out, "git cannot export commit %s%s.", commit.data,
		               e == EIO ? "" : " in time");
		return EINVAL;
	}
	return e ? e : cxi_cache_set_state(cache, commit);
}

/* Takes a shared lock once the tree contains @p commit, exported under the
 * exclusive lock if needed.
 */
static int
cxi_cache_lock (struct cxi_cache *cache,
                struct cstr_view  commit,
                struct cxi_out   *out)
{
	for (int op = LOCK_SH, e;; op = op == LOCK_SH ? LOCK_EX : LOCK_SH) {
		char now[OID_SHA256 + 3];
		size_t n = 0;
		if (flock(cache->lock, op))
			return errno;
		if ((e = util_read_all(cache->state, now, sizeof now, 0, &n)))
			return e;
		struct cstr_view const was = { now, n };
		bool const same = cstr_view_equal(was, commit);
		if (same && op == LOCK_SH)
			return 0;
		if (!same && op == LOCK_EX && (e = cxi_cache_export(cache, commit, was, out)))
			return e;
	}
}

/* Opens the cache directory and its files lock and commit, and sets the
 * environment of the git processes of an export: the empty tree is the
 * source of attributes, so that none apply.
 */
static int
cxi_cache_open (struct cxi_cache *cache,
                struct cstr_view  commit,
                struct cxi_out   *out)
{
	char const *why = nullptr;
	int const flags = O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC;
	struct dstr index = DSTR_INIT;
	int e = cxi_dir((char *)dstr_raw(&cache->dir), &cache->fd, &why);
	if (!e && ((cache->lock = openat(cache->fd, "lock", flags, 0600)) < 0 ||
	           (cache->state = openat(cache->fd, "commit", flags, 0600)) < 0))
		e = errno;
	if (!e && !dstr_printf(&index, "%sindex", dstr_get(&cache->dir)))
		e = ENOMEM;
	if (!e && (setenv("GIT_INDEX_FILE", dstr_get(&index), 1) ||
	           setenv("GIT_WORK_TREE", dstr_get(&cache->tree), 1) ||
	           setenv("GIT_ATTR_SOURCE", commit.len == OID_SHA1
	                  ? "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
	                  : "6ef19b41225c5369f1c104d45d8d85efa9b057b53b14b4b9b939dd74decc5321", 1)))
		e = errno;
	dstr_cleanup(&index);
	if (!e)
		return 0;
	cxi_out_printf(out, "The cache directory ");
	cxi_out_path(out, cstr_view_dstr(&cache->dir));
	cxi_out_printf(out, " cannot be used: %s.", why ? why : strerror(e));
	return EINVAL;
}

/* Parses the file with @p words and appends the answer of the action.
 * The function first copies fd 1 to @p *fd for the answer and points fd 1
 * at /dev/null, so that clang cannot write into the answer. Then it
 * changes to @p tree, clears the environment and confines the process.
 */
static int
cxi_parse (struct cxi        *x,
           char const        *tree,
           struct dstr const *words,
           int               *fd)
{
	static char const *const extra[] = { "-Xclang", "-disable-pragma-debug-crash" };
	size_t n = 0;
	for (size_t i = 0; i < dstr_len(words); ++i)
		n += !dstr_get(words)[i];
	size_t const argc = n + array_size(extra);
	char const **argv = malloc(argc * sizeof *argv);
	if (!argv)
		return ENOMEM;
	char const *word = dstr_get(words);
	for (size_t i = 0; i < n; ++i, word += strlen(word) + 1)
		argv[i] = word;
	memcpy(argv + n, extra, sizeof extra);
	int const copy = fcntl(1, F_DUPFD_CLOEXEC, 3);
	int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
	int e = copy < 0 || null < 0 || dup2(null, 1) < 0 || chdir(tree) || clearenv() ? errno : 0;
	util_close(&null);
	*fd = copy < 0 ? *fd : copy;
	if (!e)
		e = confine_limits();
	if (!e && !x->key[CXI_KEY_UNCONFINED].data)
		e = confine_cage(tree);
	if (e) {
		cxi_out_printf(&x->out, "askai-code cannot confine itself: %s.", strerror(e));
		free(argv);
		return EINVAL;
	}
	CXIndex index = clang_createIndex(0, 0);
	enum CXErrorCode const failed = clang_parseTranslationUnit2FullArgv(
		index, nullptr, argv, (int)argc, nullptr, 0,
		CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_KeepGoing, &x->tu);
	free(argv);
	if (failed) {
		cxi_out_printf(&x->out, failed == CXError_Crashed
		               ? "clang ran out of memory or crashed while it parsed "
		               : "clang could not parse ");
		cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
		cxi_out_printf(&x->out, ".");
		e = EINVAL;
	} else {
		e = cxi_outline(x);
	}
	clang_disposeTranslationUnit(x->tu);
	clang_disposeIndex(index);
	return e;
}

/* Runs the request with @p git, the git command line up to the
 * subcommand.
 */
static int
cxi_run (struct cxi         *x,
         char const *const  *git,
         int                 n_git,
         int                *fd)
{
	struct cstr_view const *key = x->key;
	struct cstr_view const repo = key[CXI_KEY_REPO];
	struct cstr_view const commit = key[CXI_KEY_COMMIT];
	uint64_t deadline = 0;
	/* The name of the repository is a directory of the cache. */
	if (!key[CXI_KEY_ACTION].data || !repo.len || repo.len > 64 || *repo.data == '.' ||
	    strspn(repo.data, UTIL_ALNUM "_.-") < repo.len || !key[CXI_KEY_PATH].data ||
	    !key[CXI_KEY_CDB].data || !oid_valid(commit) ||
	    !cstr_view_starts(key[CXI_KEY_TOP], CSTR_VIEW("/")) ||
	    !cstr_view_starts(key[CXI_KEY_CACHE], CSTR_VIEW("/")) ||
	    util_number(key[CXI_KEY_DEADLINE].data, key[CXI_KEY_DEADLINE].len, UINT64_MAX,
	                &deadline)) {
		cxi_out_printf(&x->out, "askai-code needs action, the name of a --git repository as "
		               "repo, path, cdb, a commit ID, a deadline in nanoseconds, and "
		               "absolute paths top and cache.");
		return EINVAL;
	}
	int e = cxi_valid(x);
	if (e)
		return e;
	struct cxi_cache cache = {
		.dir = DSTR_INIT, .tree = DSTR_INIT, .git = git,
		.deadline = deadline > CXI_EARLY ? deadline - CXI_EARLY : 1,
		.n_git = n_git, .fd = -1, .lock = -1, .state = -1,
	};
	struct dstr words = DSTR_INIT;
	/* clang's names are compared with the tree in normal form. */
	if (cdb_norm(&cache.dir, (struct cstr_view){}, key[CXI_KEY_CACHE]) ||
	    !dstr_printf(&cache.dir, "/%s/", repo.data) ||
	    !dstr_printf(&cache.tree, "%stree/", dstr_get(&cache.dir)))
		e = ENOMEM;
	x->out.tree = cstr_view_dstr(&cache.tree);
	if (!e)
		e = cxi_command(x, x->out.tree, &words);
	if (!e)
		e = cxi_cache_open(&cache, commit, &x->out);
	if (!e)
		e = cxi_cache_lock(&cache, commit, &x->out);
	struct stat st;
	if (!e && (fstatat(cache.fd, x->file, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode))) {
		cxi_out_path(&x->out, key[CXI_KEY_PATH]);
		cxi_out_printf(&x->out, " is not a file of commit %s.", commit.data);
		e = EINVAL;
	}
	if (!e)
		e = cxi_parse(x, dstr_get(&cache.tree), &words, fd);
	x->out.tree = (struct cstr_view){};
	dstr_cleanup(&words);
	util_close(&cache.state);
	util_close(&cache.lock);
	util_close(&cache.fd);
	dstr_cleanup(&cache.tree);
	dstr_cleanup(&cache.dir);
	return e;
}

int
main (int    argc,
      char **argv)
{
	struct cxi x = { .out.text = DSTR_INIT };
	bool const check = argc > 1 && !strcmp(argv[1], "--check");
	int at = 1 + check;
	int fd = 1;
	int e = child_std_open();
	if (!e)
		e = cxi_words(&x, argc, argv, &at);
	if (!e)
		e = check ? cxi_check(&x)
		          : cxi_run(&x, (char const *const *)argv + at, argc - at, &fd);
	if (x.out.e || e == ENOMEM || (e && !dstr_len(&x.out.text))) {
		int const why = x.out.e ? (int)x.out.e : e;
		dstr_clear(&x.out.text);
		x.out.e = 0;
		cxi_out_printf(&x.out, "askai-code failed: %s.", strerror(why));
	}
	/* An answer longer than TOOL_TEXT_MAX is cut at the end of a line. */
	static char const cut[] = "\n[The answer is cut at 4 MiB.]";
	static_assert(TOOL_TEXT_MAX == 4 << 20, "update the text of cut");
	char const *text = dstr_get(&x.out.text);
	size_t len = dstr_len(&x.out.text);
	bool const over = len > TOOL_TEXT_MAX;
	if (over) {
		char const *end = memrchr(text, '\n', TOOL_TEXT_MAX - (sizeof cut - 1));
		len = end ? (size_t)(end - text) : 0;
	}
	if (util_write_all(fd, text, len) || (over && util_write_all(fd, cut, sizeof cut - 1)))
		e = EIO;
	dstr_cleanup(&x.out.text);
	return e ? 1 : 0;
}
