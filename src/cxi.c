/** @file cxi.c
 *
 * askai-code, the helper program of the code tool. A function that returns
 * an int returns 0, EINVAL with a tool error in the answer, or another
 * error number.
 */
#define _GNU_SOURCE /* clearenv(), memmem(), pipe2() */
#include <clang-c/Index.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "buf.h"
#include "cdb.h"
#include "child.h"
#include "clk.h"
#include "compat.h"
#include "confine.h"
#include "cstr_view.h"
#include "cxi.h"
#include "cxi_ast.h"
#include "dstr.h"
#include "git_run.h"
#include "jb.h"
#include "oid.h"
#include "tool.h"
#include "util.h"
#include "xlist.h"

enum {
	CXI_FILES = 100000,   /* the most files and directories of a commit */
	CXI_LIST  = 2 << 20,  /* the most output of ls-tree */
	CXI_CDB   = 64 << 20, /* the largest compile_commands.json */
	CXI_DEPTH = 16,       /* the deepest nesting of its JSON */
	CXI_LINES = 40,       /* the most lines of a declaration in describe */
	CXI_ROWS  = 2000,     /* the most member rows of a layout, and the most
	                         enumerator rows of an enum */
	CXI_BITS  = 48,       /* the size of the text of an offset or a size */
	CXI_FMT   = 256,      /* the size of a text of cxi_out_fmt() */
	CXI_NAME  = 256,      /* the longest part of the name of describe, and
	                         the longest name of callers */
	CXI_BYTES = 1 << 30,  /* the largest total size of the files of a commit */
};

/* The git processes of an export get SIGKILL this long before askai kills
 * askai-code, so that none of them holds the lock of the cache after that.
 */
#define CXI_EARLY (200 * MS)

static struct cstr_view const cxi_keys[] = { CXI_KEY_LIST(XL_VIEW) };
static struct cstr_view const cxi_actions[] = { CXI_ACTION_LIST(XL_VIEW) };

/* An output of askai-code: the answer or rows of it. A text from clang or
 * from the commit can contain any bytes, so the functions that append one
 * escape control bytes and bytes that are not UTF-8.
 */
struct cxi_out {
	struct dstr      text;
	struct dstr      norm;  /* the path of the last cxi_out_rel() in normal
	                           form */
	struct cstr_view tree;  /* the tree of the commit with a trailing slash,
	                           the prefix that cxi_out_text() and
	                           cxi_out_rel() remove from a path */
	int64_t          e;     /* the error of an append that failed */
};

/* Returns an empty output. The functions that append a path to the output
 * remove the prefix @p tree from the path.
 */
static force_inline struct cxi_out
cxi_out (struct cstr_view tree)
{
	return (struct cxi_out){
		.text = DSTR_INIT,
		.norm = DSTR_INIT,
		.tree = tree
	};
}

/* Frees the text of @p out. */
static void
cxi_out_fini (struct cxi_out *out)
{
	dstr_cleanup(&out->norm);
	dstr_cleanup(&out->text);
}

/* A call of askai-code: the request from its arguments, the answer, the
 * compile commands, and the parse of the file. For describe, the record
 * line "lines FIRST LAST TOTAL AT LEN PATH" comes before the answer: it
 * names the numbered lines of a file of the repository in the answer, at
 * the offset AT for LEN bytes.
 */
struct cxi {
	struct cxi_out      out;
	struct dstr         record;
	struct cstr_view    key[CXI_KEYS];  /* each value, or data nullptr */
	struct cstr_view    words;          /* the clang command line of the file,
	                                       each word followed by a NUL */
	struct json_object *cmds;           /* the compile commands */
	char const         *file;           /* the path of the file in the tree */
	char const         *warning[CXI_WARNINGS];
	CXIndex             index;
	CXTranslationUnit   tu;             /* the parse that the functions of
	                                       an action look at */
	uint64_t            deadline;       /* of the call, in loop_clock() time */
	uint64_t            n_warnings;
	uint64_t            action;         /* enum cxi_action */
	uint64_t            line;           /* the value of line, or 0 */
};

/* Returns the call before cxi_words() reads the request. The answer and
 * the record line are empty, and every other member is zero.
 */
static force_inline struct cxi
cxi (void)
{
	return (struct cxi){
		.out    = cxi_out(cstr_view(nullptr)),
		.record = DSTR_INIT
	};
}

/* Frees the answer and the record line of @p x. */
static void
cxi_fini (struct cxi *x)
{
	dstr_cleanup(&x->record);
	cxi_out_fini(&x->out);
}

/* Appends the @p n bytes at @p p. */
static void
cxi_out_put (struct cxi_out *out,
             char const     *p,
             size_t          n)
{
	if (!dstr_cat_n(&out->text, p, n))
		out->e = ENOMEM;
}

/* Appends the string literal @p s. */
#define cxi_out_lit(out, s) cxi_out_put((out), "" s, sizeof (s) - 1)

static force_inline void
cxi_out_view (struct cxi_out   *out,
              struct cstr_view  s)
{
	cxi_out_put(out, s.data, s.len);
}

/* Appends @p n in decimal. */
static void
cxi_out_num (struct cxi_out *out,
             uint64_t        n)
{
	char digits[UTIL_DECIMAL];
	cxi_out_view(out, util_decimal(digits + sizeof digits, n, 0));
}

/* Appends the @p n bytes that snprintf() wrote to the @p size bytes at
 * @p buf. Sets the error of @p out if snprintf() failed or the text did not
 * fit.
 */
static force_inline void
cxi_out_fmt_ (struct cxi_out *out,
              char const     *buf,
              int             size,
              int             n)
{
	if (n < 0 || n >= size)
		out->e = EOVERFLOW;
	else
		cxi_out_put(out, buf, (size_t)n);
}

/* Appends the text that snprintf() makes of a format and arguments: for a
 * text that contains a number. The text must fit in CXI_FMT bytes with the
 * NUL: an argument of %s is a short text of askai-code.
 */
#define cxi_out_fmt(out, ...) do {              \
        char cxi_fmt_[CXI_FMT];                 \
        cxi_out_fmt_((out), cxi_fmt_,           \
                     (int)sizeof cxi_fmt_,      \
                     snprintf(cxi_fmt_,         \
                              sizeof cxi_fmt_,  \
                              __VA_ARGS__));    \
} while (0)

/* Appends the byte @p c as \xNN. */
static void
cxi_out_hex (struct cxi_out *out,
             unsigned char   c)
{
	char text[4] = { '\\', 'x' };
	util_hex(text + 2, &c, 1);
	cxi_out_put(out, text, sizeof text);
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
	/* The bytes from from to i are appended as they are, in one piece. */
	size_t from = 0;
	for (size_t i = 0; i < n; ) {
		size_t const col = s[i] == ':' && i && s[i - 1] >= '0' && s[i - 1] <= '9'
		                   ? strspn(s + i + 1, "0123456789") : 0;
		struct cstr_view const rest = cstr_view_n(s + i, n - i);
		/* The test of the first byte spares most calls of memcmp(). */
		size_t const skip = out->tree.len && s[i] == *out->tree.data &&
		                    cstr_view_starts(rest, out->tree) ? out->tree.len
		                  : col && s[i + 1 + col] == ')' ? 1 + col : 0;
		size_t k;
		if (skip) {
			cxi_out_put(out, s + from, i - from);
			i += skip;
			from = i;
		} else if ((k = cxi_char(s + i, n - i))) {
			i += k;
		} else {
			cxi_out_put(out, s + from, i - from);
			unsigned char const c = (unsigned char)s[i++];
			if (c == '\n')
				cxi_out_lit(out, "\\n");
			else if (c == '\t')
				cxi_out_lit(out, "\\t");
			else
				cxi_out_hex(out, c);
			from = i;
		}
	}
	cxi_out_put(out, s + from, n - from);
}

/* Returns the path @p s without the path of the tree. An absolute path is
 * first normalized, because clang names a file as #include spells it: the
 * view is then valid until the next call with @p out.
 */
static struct cstr_view
cxi_out_rel (struct cxi_out *out,
             char const     *s)
{
	struct cstr_view p = cstr_view(s);
	if (*s == '/') {
		if (cdb_norm(&out->norm, cstr_view(nullptr), p))
			out->e = ENOMEM;
		else
			p = cstr_view_dstr(&out->norm);
	}
	return out->tree.len && cstr_view_starts(p, out->tree)
	       ? cstr_view_slice(p, out->tree.len, SIZE_MAX) : p;
}

/* Appends the path or request word @p p, quoted as git quotes a path that
 * contains '"', '\\', a control character or bytes that are not UTF-8.
 */
static void
cxi_out_path (struct cxi_out   *out,
              struct cstr_view  p)
{
	size_t i = 0;
	for (size_t k; i < p.len
	            && (k = cxi_char(p.data + i, p.len - i))
	            && !memchr("\"\\", p.data[i], 2); i += k)
		;

	bool const quote = i < p.len;
	if (quote)
		cxi_out_put(out, "\"", 1);
	cxi_out_put(out, p.data, i);

	while (i < p.len) {
		unsigned char const c = (unsigned char)p.data[i];
		/* A backslash and the byte, or three octal digits. */
		char esc[4] = {
			'\\',
			(char)c,
			(char)('0' + (c >> 3 & 7)),
			(char)('0' + (c & 7))
		};
		size_t k = 1;
		if (c == '"' || c == '\\') {
			cxi_out_put(out, esc, 2);
		} else if (c >= '\a' && c <= '\r') {
			esc[1] = "abtnvfr"[c - '\a'];
			cxi_out_put(out, esc, 2);
		} else if ((k = cxi_char(p.data + i, p.len - i))) {
			cxi_out_put(out, p.data + i, k);
		} else {
			esc[1] = (char)('0' + (c >> 6));
			cxi_out_put(out, esc, sizeof esc);
		}
		i += k ? k : 1;
	}

	if (quote)
		cxi_out_put(out, "\"", 1);
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
	clang_getFileLocation(loc, nullptr, &line, nullptr, nullptr);
	return line;
}

/* Appends the path of the file of @p loc and returns the line of @p loc. */
static unsigned
cxi_out_file (struct cxi_out   *out,
              CXSourceLocation  loc)
{
	CXFile file;
	CXString name;
	unsigned line;
	clang_getFileLocation(loc, &file, &line, nullptr, nullptr);
	/* A macro of clang or of -D is in <built-in> or <command line>. */
	if (file)
		name = clang_getFileName(file);
	else
		clang_getPresumedLocation(loc, &name, &line, nullptr);
	char const *path = clang_getCString(name);
	cxi_out_path(out, cxi_out_rel(out, path ? path : ""));
	clang_disposeString(name);
	return line;
}

/* Appends the place @p loc as PATH:LINE. */
static void
cxi_out_place (struct cxi_out   *out,
               CXSourceLocation  loc)
{
	unsigned const line = cxi_out_file(out, loc);
	cxi_out_lit(out, ":");
	cxi_out_num(out, line);
}

/* Appends the lines of the declaration @p c as FIRST-LAST, or as FIRST if
 * the declaration is one line.
 */
static void
cxi_out_lines (struct cxi_out *out,
               CXCursor        c)
{
	CXSourceRange const extent = clang_getCursorExtent(c);
	unsigned const first = cxi_line(clang_getRangeStart(extent));
	unsigned const last = cxi_line(clang_getRangeEnd(extent));
	cxi_out_num(out, first);
	if (last != first) {
		cxi_out_lit(out, "-");
		cxi_out_num(out, last);
	}
}

static bool
cxi_tag (enum CXCursorKind k)
{
	return k == CXCursor_StructDecl || k == CXCursor_UnionDecl || k == CXCursor_EnumDecl;
}

/* Returns the word for the kind @p k of a cursor, with data nullptr for a
 * kind to skip.
 */
static struct cstr_view
cxi_kind (enum CXCursorKind k)
{
	static struct cstr_view const words[] = {
		[CXCursor_StructDecl] = CSTR_VIEW_INIT("struct"),
		[CXCursor_UnionDecl] = CSTR_VIEW_INIT("union"),
		[CXCursor_EnumDecl] = CSTR_VIEW_INIT("enum"),
		[CXCursor_FunctionDecl] = CSTR_VIEW_INIT("function"),
		[CXCursor_VarDecl] = CSTR_VIEW_INIT("variable"),
		[CXCursor_TypedefDecl] = CSTR_VIEW_INIT("typedef"),
		[CXCursor_FieldDecl] = CSTR_VIEW_INIT("field"),
		[CXCursor_EnumConstantDecl] = CSTR_VIEW_INIT("enumerator"),
		[CXCursor_ParmDecl] = CSTR_VIEW_INIT("parameter"),
	};
	return k == CXCursor_MacroDefinition ? CSTR_VIEW("macro")
	     : (size_t)k < array_size(words) ? words[k] : cstr_view(nullptr);
}

/* Returns "static " for a declaration of static storage, else an empty
 * view. Only a declaration may go to clang_Cursor_getStorageClass(), which
 * reads any cursor as one.
 */
static struct cstr_view
cxi_static (CXCursor c)
{
	return clang_isDeclaration(c.kind) &&
	       clang_Cursor_getStorageClass(c) == CX_SC_Static ? CSTR_VIEW("static ")
	                                                       : cstr_view(nullptr);
}

/* Returns true for a struct, union or enum without a name, also one named
 * by a typedef, which clang spells with the typedef's name. The USR of such
 * a declaration has a kind that ends in A, such as @SA@.
 */
static bool
cxi_unnamed (CXCursor c)
{
	bool unnamed = clang_Cursor_isAnonymous(c);
	if (!unnamed) {
		CXString usr = clang_getCursorUSR(c);
		char const *u = clang_getCString(usr);
		char const *at = u ? strrchr(u, '@') : nullptr;
		unnamed = at && at - u >= 3
	                     && at[-1] == 'A'
	                     && at[-3] == '@';
		clang_disposeString(usr);
	}
	return unnamed;
}

static void
cxi_out_name (struct cxi_out *out,
              CXCursor        c)
{
	if (cxi_tag(c.kind) && cxi_unnamed(c))
		cxi_out_lit(out, "(anonymous)");
	else
		cxi_out_clang(out, clang_getCursorSpelling(c));
}

/* Returns the definition of @p c in the translation unit, or @p c. */
static CXCursor
cxi_shown (CXCursor c)
{
	CXCursor const d = c.kind == CXCursor_MacroDefinition
	                   ? c : clang_getCursorDefinition(c);
	return clang_Cursor_isNull(d) ? c : d;
}

/* Appends the start of the first line of the answer: the repository, the
 * file and the commit.
 */
static void
cxi_head (struct cxi *x)
{
	cxi_out_lit(&x->out, "[Repository ");
	cxi_out_view(&x->out, x->key[CXI_KEY_REPO]);
	cxi_out_lit(&x->out, ", file ");
	cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
	cxi_out_lit(&x->out, " at commit ");
	cxi_out_view(&x->out, x->key[CXI_KEY_COMMIT]);
	cxi_out_lit(&x->out, ". ");
}

/* Cursors that a visitor collects. A zeroed list is empty. A visitor of a
 * list stops the visit only if a push fails, so that a visit that
 * clang_visitChildren() reports as stopped is out of memory.
 */
struct cxi_list {
	UTIL_ARRAY(CXCursor);
};

/* Frees the cursors of @p list. */
static void
cxi_list_fini (struct cxi_list *list)
{
	free(list->items);
	*list = (struct cxi_list){};
}

/* Appends each child of a cursor to the cxi_list at @p data. */
static enum CXChildVisitResult
cxi_list_child (CXCursor     c,
                CXCursor,
                CXClientData data)
{
	struct cxi_list *list = data;
	return util_push(*list, c) ? CXChildVisit_Break : CXChildVisit_Continue;
}

/* Appends the children of @p c to @p list. Returns 0 or ENOMEM. */
static int
cxi_list_children (struct cxi_list *list,
                   CXCursor         c)
{
	return clang_visitChildren(c, cxi_list_child, list) ? ENOMEM : 0;
}

/* Appends each name that the main file declares or defines to the cxi_list
 * at @p data.
 */
static enum CXChildVisitResult
cxi_list_declared (CXCursor     c,
                   CXCursor,
                   CXClientData data)
{
	struct cxi_list *list = data;
	enum CXCursorKind const k = c.kind;
	bool const skip = !cxi_kind(k).data ||
	                  !clang_Location_isFromMainFile(clang_getCursorLocation(c)) ||
	                  (cxi_tag(k) && !clang_isCursorDefinition(c)) ||
	                  (k == CXCursor_MacroDefinition && clang_Cursor_isMacroBuiltin(c));
	return !skip && util_push(*list, c) ? CXChildVisit_Break : CXChildVisit_Continue;
}

/* Orders cursors by place: the visit gives the macros first. */
static int
cxi_order (void const *a,
           void const *b)
{
	unsigned x;
	unsigned y;
	clang_getFileLocation(clang_getCursorLocation(*(CXCursor const *)a),
	                      nullptr, nullptr, nullptr, &x);
	clang_getFileLocation(clang_getCursorLocation(*(CXCursor const *)b),
	                      nullptr, nullptr, nullptr, &y);
	return (x > y) - (x < y);
}

static int
cxi_outline (struct cxi *x)
{
	struct cxi_list list = {};
	if (clang_visitChildren(clang_getTranslationUnitCursor(x->tu),
	                        cxi_list_declared, &list))
		x->out.e = ENOMEM;
	if (list.n)
		qsort(list.items, list.n, sizeof *list.items, cxi_order);
	cxi_head(x);
	cxi_out_fmt(&x->out, "%zu name%s.]\n", list.n, util_s(list.n));
	for (size_t i = 0; i < list.n; ++i) {
		CXCursor const c = list.items[i];
		enum CXCursorKind const k = c.kind;
		CXSourceRange const extent = clang_getCursorExtent(c);
		unsigned const first = cxi_line(clang_getRangeStart(extent));
		unsigned const last = cxi_line(clang_getRangeEnd(extent));
		cxi_out_num(&x->out, cxi_line(clang_getCursorLocation(c)));
		cxi_out_lit(&x->out, " ");
		cxi_out_view(&x->out, cxi_static(c));
		cxi_out_view(&x->out, cxi_kind(c.kind));
		cxi_out_lit(&x->out, " ");
		cxi_out_name(&x->out, c);
		CXType const type = k == CXCursor_TypedefDecl ? clang_getTypedefDeclUnderlyingType(c)
		                                              : clang_getCursorType(c);
		if (k == CXCursor_FunctionDecl || k == CXCursor_VarDecl ||
		    k == CXCursor_TypedefDecl) {
			cxi_out_lit(&x->out, ": ");
			cxi_out_clang(&x->out, clang_getTypeSpelling(type));
		}
		if (k == CXCursor_FunctionDecl && !clang_isCursorDefinition(c))
			cxi_out_lit(&x->out, ", declared");
		if (last > first)
			cxi_out_fmt(&x->out, " (%u-%u)", first, last);
		cxi_out_lit(&x->out, "\n");
	}
	cxi_list_fini(&list);
	return x->out.e;
}

/* Writes line @p n of @p text to @p *line, data nullptr if there is none,
 * and returns the number of lines.
 */
static size_t
cxi_find_line (char const       *text,
               size_t            size,
               unsigned          n,
               struct cstr_view *line)
{
	*line = cstr_view(nullptr);
	if (!text)
		return 0;
	size_t total = 0;
	for (char const *p = text, *end = text + size; p < end; ++total) {
		char const *nl = memchr(p, '\n', (size_t)(end - p));
		struct cstr_view const here = cstr_view_n(p, nl ? (size_t)(nl - p)
		                                                : (size_t)(end - p));
		*line = total + 1 == n ? here : *line;
		p = nl ? nl + 1 : end;
	}
	return total;
}

/* Appends the line @p s of a file with tabs as they are, and other control
 * bytes and bytes that are not UTF-8 as \xNN.
 */
static void
cxi_out_line (struct cxi_out   *out,
              struct cstr_view  s)
{
	/* The bytes from from to i are appended as they are, in one piece. */
	size_t from = 0;
	for (size_t i = 0, k; i < s.len; i += k ? k : 1) {
		k = s.data[i] == '\t' ? 1 : cxi_char(s.data + i, s.len - i);
		if (k)
			continue;
		cxi_out_put(out, s.data + from, i - from);
		cxi_out_hex(out, (unsigned char)s.data[i]);
		from = i + 1;
	}
	cxi_out_put(out, s.data + from, s.len - from);
}

/* Stores the body of a function in the CXCursor at @p data. */
static enum CXChildVisitResult
cxi_body (CXCursor     c,
          CXCursor,
          CXClientData data)
{
	if (c.kind != CXCursor_CompoundStmt)
		return CXChildVisit_Continue;
	*(CXCursor *)data = c;
	return CXChildVisit_Break;
}

/* Appends the numbered lines of the declaration @p c, from its comment if
 * that ends next to it, and to the head of a function, and sets the record
 * line for them.
 */
static void
cxi_declaration (struct cxi *x,
                 CXCursor    c)
{
	CXSourceRange const extent = clang_getCursorExtent(c);
	CXSourceRange const comment = clang_Cursor_getCommentRange(c);
	CXFile file;
	CXFile in;
	unsigned first;
	unsigned from;
	clang_getFileLocation(clang_getRangeStart(extent), &file, &first, nullptr, nullptr);
	clang_getFileLocation(clang_getRangeStart(comment), &in, &from, nullptr, nullptr);
	unsigned last = cxi_line(clang_getRangeEnd(extent));
	unsigned const to = cxi_line(clang_getRangeEnd(comment));
	if (!clang_Range_isNull(comment) && clang_File_isEqual(in, file) && to + 1 >= first &&
	    from <= last) {
		first = from < first ? from : first;
		last = to > last ? to : last;
	}
	CXCursor body = clang_getNullCursor();
	if (c.kind == CXCursor_FunctionDecl)
		clang_visitChildren(c, cxi_body, &body);
	unsigned line;
	unsigned column;
	clang_getFileLocation(clang_getCursorLocation(body), nullptr, &line, &column, nullptr);
	last = clang_Cursor_isNull(body) ? last : column == 1 && line > 1 ? line - 1 : line;
	size_t size = 0;
	char const *text = file ? clang_getFileContents(x->tu, file, &size) : nullptr;
	struct cstr_view here = {};
	size_t const total = cxi_find_line(text, size, first, &here);
	last = last < first ? first : last - first < CXI_LINES ? last : first + CXI_LINES - 1;
	last = last < total ? last : (unsigned)total;
	if (!here.data)
		return;
	char const *end = text + size;
	CXString name = clang_getFileName(file);
	char const *path = clang_getCString(name);
	struct cstr_view const rel = cxi_out_rel(&x->out, path ? path : "");
	cxi_out_path(&x->out, rel);
	if (first == last)
		cxi_out_fmt(&x->out, " line %u of %zu:\n", first, total);
	else
		cxi_out_fmt(&x->out, " lines %u to %u of %zu:\n", first, last, total);
	size_t const at = dstr_len(&x->out.text);
	for (unsigned n = first; n <= last; ++n) {
		cxi_out_num(&x->out, n);
		cxi_out_lit(&x->out, "\t");
		cxi_out_line(&x->out, here);
		cxi_out_lit(&x->out, "\n");
		char const *next = here.data + here.len + (here.data + here.len < end);
		char const *nl = memchr(next, '\n', (size_t)(end - next));
		here = cstr_view_n(next, nl ? (size_t)(nl - next) : (size_t)(end - next));
	}
	/* The record names a file of the tree that is UTF-8 without control
	 * bytes, so that askai can read it. */
	size_t i = 0;
	for (size_t k; i < rel.len && (k = cxi_char(rel.data + i, rel.len - i)); i += k)
		;
	if (rel.len && *rel.data != '/' && i == rel.len &&
	    !dstr_printf(&x->record, "lines %u %u %zu %zu %zu %s\n", first, last, total, at,
	                 dstr_len(&x->out.text) - at, rel.data))
		x->out.e = ENOMEM;
	clang_disposeString(name);
}

/* A record in a layout. padding, rows (left to print) and hidden (members
 * of the outermost record left out) are copied back from nested records.
 * base and end are in bits.
 */
struct cxi_layout {
	struct cxi_out *out;
	uint64_t        padding;
	uint64_t        rows;
	uint64_t        hidden;
	uint64_t        base;
	uint64_t        end;
	uint32_t        depth;
	uint32_t        is_union;
};

/* Writes the offset @p bits to the end of @p text as BYTES or BYTES+BITSb,
 * and returns the string.
 */
static char const *
cxi_offset (char     text[static CXI_BITS],
            uint64_t bits)
{
	char *end = text + CXI_BITS - 1;
	*end = '\0';
	if (bits % 8) {
		*--end = 'b';
		*--end = (char)('0' + bits % 8);
		*--end = '+';
	}
	return util_decimal(end, bits / 8, 0).data;
}

/* Writes the size @p bits to the end of @p text as BYTES, or BITSb, and
 * returns the string.
 */
static char const *
cxi_size (char     text[static CXI_BITS],
          uint64_t bits)
{
	char *end = text + CXI_BITS - 1;
	*end = '\0';
	if (bits % 8)
		*--end = 'b';
	return util_decimal(end, bits % 8 ? bits : bits / 8, 0).data;
}

/* Appends the indentation of a row of the record @p l. */
static void
cxi_layout_indent (struct cxi_layout const *l)
{
	for (uint32_t i = 0; i < l->depth; ++i)
		cxi_out_lit(l->out, "  ");
}

/* Appends the row of the padding from the bit @p from to the bit @p to,
 * which ends with @p what.
 */
static void
cxi_layout_padding (struct cxi_layout *l,
                    uint64_t           from,
                    uint64_t           to,
                    struct cstr_view   what)
{
	char at[CXI_BITS];
	char size[CXI_BITS];
	if (to <= from)
		return;
	cxi_out_fmt(l->out, "%7s%7s%16s", cxi_offset(at, from), cxi_size(size, to - from), "");
	cxi_layout_indent(l);
	cxi_out_view(l->out, what);
	l->padding += to - from;
}

static void cxi_layout_record (struct cxi_layout *l, CXType type, uint64_t base,
                               uint32_t depth);

/* Appends the padding before the field @p c, its row, and the rows of an
 * unnamed struct or union in it.
 */
static enum CXVisitorResult
cxi_layout_field (CXCursor     c,
                  CXClientData data)
{
	struct cxi_layout *l = data;
	if (!l->rows) {
		l->hidden += !l->depth;
		return CXVisit_Continue;
	}
	--l->rows;
	CXType const type = clang_getCursorType(c);
	long long const offset = clang_Cursor_getOffsetOfField(c);
	bool const bitfield = clang_Cursor_isBitField(c);
	long long const n = bitfield ? clang_getFieldDeclBitWidth(c)
	                  : 8 * clang_Type_getSizeOf(type);
	uint64_t const size = n > 0 ? (uint64_t)n : 0;
	long long const align = clang_Type_getAlignOf(type);
	uint64_t const at = l->base + (uint64_t)(offset > 0 ? offset : 0);
	if (!l->is_union)
		cxi_layout_padding(l, l->end, at, CSTR_VIEW("(padding)\n"));
	CXCursor const decl = clang_getTypeDeclaration(clang_getCanonicalType(type));
	enum CXCursorKind const k = decl.kind;
	bool const nested = (k == CXCursor_StructDecl || k == CXCursor_UnionDecl) &&
	                    clang_Cursor_isAnonymous(decl);
	char bits[2][CXI_BITS];
	cxi_out_fmt(l->out, "%7s%7s%7lld%7u  ", cxi_offset(bits[0], at), cxi_size(bits[1], size),
	            align > 0 ? align : 0, cxi_line(clang_getCursorLocation(c)));
	cxi_layout_indent(l);
	if (clang_Cursor_isAnonymousRecordDecl(decl)) {
		cxi_out_lit(l->out, "(anonymous ");
		cxi_out_view(l->out, cxi_kind(decl.kind));
		cxi_out_lit(l->out, ")\n");
	} else {
		cxi_out_clang(l->out, clang_getCursorSpelling(c));
		cxi_out_lit(l->out, ": ");
		if (nested) {
			cxi_out_view(l->out, cxi_kind(decl.kind));
			cxi_out_lit(l->out, " (unnamed)");
		} else {
			cxi_out_clang(l->out, clang_getTypeSpelling(type));
		}
		if (bitfield)
			cxi_out_fmt(l->out, " : %lld", n);
		cxi_out_lit(l->out, "\n");
	}
	if (nested)
		cxi_layout_record(l, clang_getCanonicalType(type), at, l->depth + 1);
	uint64_t const end = at + size;
	l->end = !l->is_union || end > l->end ? end : l->end;
	return CXVisit_Continue;
}

static void
cxi_layout_record (struct cxi_layout *l,
                   CXType             type,
                   uint64_t           base,
                   uint32_t           depth)
{
	struct cxi_layout in = *l;
	in.base = in.end = base;
	in.depth = depth;
	in.is_union = clang_getTypeDeclaration(type).kind == CXCursor_UnionDecl;
	clang_Type_visitFields(type, cxi_layout_field, &in);
	long long const size = clang_Type_getSizeOf(type);
	if (in.rows && size > 0)
		cxi_layout_padding(&in, in.end, base + 8 * (uint64_t)size, depth
		                   ? CSTR_VIEW("(padding at the end of the member)\n")
		                   : CSTR_VIEW("(padding at the end)\n"));
	l->padding = in.padding;
	l->rows = in.rows;
	l->hidden = in.hidden;
}

/* Appends the size, alignment, padding and target of the record @p type,
 * and its rows, at most CXI_ROWS, to @p rows.
 */
static void
cxi_describe_layout (struct cxi     *x,
                     struct cxi_out *rows,
                     CXType          type)
{
	struct cxi_layout l = {
		.out  = rows,
		.rows = CXI_ROWS
	};
	cxi_out_lit(rows, " offset   size  align   line  member: type\n");
	cxi_layout_record(&l, type, 0, 0);
	if (l.hidden)
		cxi_out_fmt(rows, "[%" PRIu64 " more members. To describe one member, give "
		            "name as TYPE.MEMBER, such as child.flags.]\n", l.hidden);
	cxi_out_fmt(&x->out, ": %lld bytes, alignment %lld, %s%" PRIu64 " byte%s",
	            clang_Type_getSizeOf(type), clang_Type_getAlignOf(type),
	            l.rows ? "" : "at least ", l.padding / 8, util_s(l.padding / 8));
	if (l.padding % 8)
		cxi_out_fmt(&x->out, " and %" PRIu64 " bit%s", l.padding % 8,
		            util_s(l.padding % 8));
	CXTargetInfo info = clang_getTranslationUnitTargetInfo(x->tu);
	cxi_out_lit(&x->out, " of padding, ");
	cxi_out_clang(&x->out, clang_TargetInfo_getTriple(info));
	clang_TargetInfo_dispose(info);
	cxi_out_lit(&x->out, "\n");
}

/* A search for a name, of the kind kind if not 0, or with part for the
 * names of the tree that contain it. tree is the tree of the commit with a
 * trailing slash.
 */
struct cxi_find {
	struct cxi_list  list;
	struct cstr_view name;
	struct cstr_view tree;
	uint32_t         kind;
	uint32_t         part;
};

/* Returns a search with an empty list for the name @p name, of the kind
 * @p kind if not 0. @p tree is the tree of the commit with a trailing slash.
 */
static force_inline struct cxi_find
cxi_find (struct cstr_view tree,
          struct cstr_view name,
          uint32_t         kind)
{
	return (struct cxi_find){
		.name = name,
		.tree = tree,
		.kind = kind
	};
}

/* Returns true if @p c is in a file of @p tree, the tree of the commit with
 * a trailing slash.
 */
static bool
cxi_in_tree (struct cstr_view tree,
             CXCursor         c)
{
	CXFile file;
	clang_getExpansionLocation(clang_getCursorLocation(c), &file, nullptr, nullptr, nullptr);
	CXString name = clang_getFileName(file);
	char const *path = clang_getCString(name);
	bool const in_tree = path && cstr_view_starts(cstr_view(path), tree);
	clang_disposeString(name);
	return in_tree;
}

/* Returns true if @p c is spelled @p name. */
static bool
cxi_spelled (CXCursor    c,
             char const *name)
{
	CXString spelling = clang_getCursorSpelling(c);
	char const *s = clang_getCString(spelling);
	bool const spelled = s && !strcmp(s, name);
	clang_disposeString(spelling);
	return spelled;
}

/* Appends each declaration or macro that matches the cxi_find at @p data
 * to the list of the search, once. The spelling of a cursor is compared
 * only if the kind of the cursor matches.
 */
static enum CXChildVisitResult
cxi_find_match (CXCursor     c,
                CXCursor,
                CXClientData data)
{
	struct cxi_find *find = data;
	enum CXCursorKind const k = c.kind;
	bool const tag = cxi_tag(k);
	/* An enumerator is a child of the enum. */
	enum CXChildVisitResult const next = tag ? CXChildVisit_Recurse : CXChildVisit_Continue;
	if (!cxi_kind(k).data || k == CXCursor_FieldDecl || k == CXCursor_ParmDecl ||
	    (find->kind && k != find->kind))
		return next;
	CXString spelling = clang_getCursorSpelling(c);
	char const *s = clang_getCString(spelling);
	bool const named = s && (find->part ? find->name.len && strstr(s, find->name.data)
	                                    : !strcmp(s, find->name.data));
	clang_disposeString(spelling);
	if (!named || (tag && cxi_unnamed(c)) || (find->part && !cxi_in_tree(find->tree, c)) ||
	    (k == CXCursor_MacroDefinition && clang_Cursor_isMacroBuiltin(c)))
		return next;
	CXCursor const canonical = clang_getCanonicalCursor(c);
	for (size_t i = 0; i < find->list.n; ++i) {
		if (clang_equalCursors(clang_getCanonicalCursor(find->list.items[i]), canonical))
			return next;
	}
	return util_push(find->list, c) ? CXChildVisit_Break : next;
}

/* Writes the symbol named @p name to @p *c. The error lists the matches,
 * or for none the names of the tree that contain @p name.
 */
static int
cxi_lookup (struct cxi *x,
            char const *name,
            uint32_t    kind,
            CXCursor   *c)
{
	CXCursor const unit = clang_getTranslationUnitCursor(x->tu);
	struct cxi_find find = cxi_find(x->out.tree, cstr_view(name), kind);
	if (clang_visitChildren(unit, cxi_find_match, &find))
		x->out.e = ENOMEM;
	size_t const n = find.list.n;
	if (n == 1) {
		*c = cxi_shown(find.list.items[0]);
		cxi_list_fini(&find.list);
		return x->out.e;
	}
	if (n) {
		size_t tag = 0;
		while (tag < n && !cxi_tag(find.list.items[tag].kind))
			++tag;
		cxi_out_fmt(&x->out, "%zu symbols in the translation unit of ", n);
		cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
		cxi_out_lit(&x->out, " are named ");
		cxi_out_text(&x->out, x->key[CXI_KEY_NAME].data);
		if (tag < n) {
			cxi_out_lit(&x->out, ". Give the name ");
			cxi_out_view(&x->out, cxi_kind(find.list.items[tag].kind));
			cxi_out_lit(&x->out, " ");
			cxi_out_view(&x->out, find.name);
			cxi_out_lit(&x->out, " for the type, or give the line argument. The "
			            "symbols:\n");
		} else {
			cxi_out_lit(&x->out, ". Give the line argument. The symbols:\n");
		}
	} else {
		find.part = 1;
		find.kind = 0;
		if (clang_visitChildren(unit, cxi_find_match, &find))
			x->out.e = ENOMEM;
		cxi_out_view(&x->out, kind == CXCursor_FunctionDecl ? CSTR_VIEW("No function ")
		                                                    : CSTR_VIEW("No symbol "));
		cxi_out_text(&x->out, x->key[CXI_KEY_NAME].data);
		cxi_out_lit(&x->out, " in the translation unit of ");
		cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
		cxi_out_view(&x->out, find.list.n
		             ? CSTR_VIEW(". Repository names in the translation unit that contain ")
		             : CSTR_VIEW(". No repository name in the translation unit contains "));
		cxi_out_text(&x->out, name);
		cxi_out_view(&x->out, find.list.n ? CSTR_VIEW(":\n") : CSTR_VIEW("."));
	}
	for (size_t i = 0; i < find.list.n; ++i) {
		CXCursor const at = cxi_shown(find.list.items[i]);
		cxi_out_lit(&x->out, "  ");
		cxi_out_place(&x->out, clang_getCursorLocation(at));
		cxi_out_lit(&x->out, " ");
		cxi_out_view(&x->out, cxi_static(at));
		cxi_out_view(&x->out, cxi_kind(at.kind));
		cxi_out_lit(&x->out, " ");
		cxi_out_name(&x->out, at);
		cxi_out_lit(&x->out, "\n");
	}
	cxi_list_fini(&find.list);
	return x->out.e ? x->out.e : EINVAL;
}

/* The search for the expression of a macro use at from to to in file. */
struct cxi_use {
	CXCursor expr;
	CXFile   file;
	uint32_t from;
	uint32_t to;
};

/* Finds the expression, not an unexposed one such as an implicit
 * conversion, whose extent is that of the use.
 */
static enum CXChildVisitResult
cxi_use_visit (CXCursor     c,
               CXCursor,
               CXClientData data)
{
	struct cxi_use *use = data;
	CXSourceRange const extent = clang_getCursorExtent(c);
	CXFile file;
	unsigned from;
	unsigned to;
	clang_getExpansionLocation(clang_getRangeStart(extent), &file, nullptr, nullptr, &from);
	clang_getExpansionLocation(clang_getRangeEnd(extent), nullptr, nullptr, nullptr, &to);
	enum CXCursorKind const k = c.kind;
	if (!clang_File_isEqual(file, use->file) || from > use->from || to < use->to)
		return CXChildVisit_Continue;
	if (!clang_isExpression(k) || k == CXCursor_UnexposedExpr || from != use->from ||
	    to != use->to)
		return CXChildVisit_Recurse;
	use->expr = c;
	return CXChildVisit_Break;
}

/* Appends the type and value of the macro use at @p column of the line. */
static void
cxi_value (struct cxi *x,
           unsigned    column)
{
	unsigned const line = (unsigned)x->line;
	CXFile const file = clang_getFile(x->tu, x->file);
	CXSourceRange const extent = clang_getCursorExtent(
		clang_getCursor(x->tu, clang_getLocation(x->tu, file, line, column)));
	struct cxi_use use = {
		.expr = clang_getNullCursor(),
		.file = file
	};
	clang_getExpansionLocation(clang_getRangeStart(extent), nullptr, nullptr, nullptr,
	                           &use.from);
	clang_getExpansionLocation(clang_getRangeEnd(extent), nullptr, nullptr, nullptr, &use.to);
	clang_visitChildren(clang_getTranslationUnitCursor(x->tu), cxi_use_visit, &use);
	if (clang_Cursor_isNull(use.expr))
		return;
	cxi_out_lit(&x->out, "at ");
	cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
	cxi_out_fmt(&x->out, ":%u: an expression of type ", line);
	cxi_out_clang(&x->out, clang_getTypeSpelling(clang_getCursorType(use.expr)));
	CXEvalResult value = clang_Cursor_Evaluate(use.expr);
	switch (value ? clang_EvalResult_getKind(value) : CXEval_UnExposed) {
	case CXEval_Int:
		if (clang_EvalResult_isUnsignedInt(value))
			cxi_out_fmt(&x->out, ", value %llu", clang_EvalResult_getAsUnsigned(value));
		else
			cxi_out_fmt(&x->out, ", value %lld", clang_EvalResult_getAsLongLong(value));
		break;
	case CXEval_Float:
		cxi_out_fmt(&x->out, ", value %g", clang_EvalResult_getAsDouble(value));
		break;
	case CXEval_StrLiteral:
		cxi_out_lit(&x->out, ", value \"");
		cxi_out_text(&x->out, clang_EvalResult_getAsStr(value));
		cxi_out_lit(&x->out, "\"");
		break;
	default:
		break;
	}
	if (value)
		clang_EvalResult_dispose(value);
	cxi_out_lit(&x->out, "\n");
}

static bool
cxi_is_named (CXCursor    c,
              char const *name)
{
	return cxi_kind(c.kind).data && cxi_spelled(c, name);
}

/* Returns what the name at @p line and @p column of @p file refers to: the
 * definition of the macro of a macro use, or the declaration that an
 * expression or a type refers to. Returns the cursor at the place if it
 * refers to nothing.
 */
static CXCursor
cxi_symbol (CXTranslationUnit tu,
            CXFile            file,
            unsigned          line,
            unsigned          column)
{
	CXCursor const at = clang_getCursor(tu, clang_getLocation(tu, file, line, column));
	CXCursor const referenced = clang_getCursorReferenced(at);
	return clang_Cursor_isNull(referenced) ? at : referenced;
}

/* Returns the definition of the macro through which the token at @p loc
 * is written, or null if the token is part of no macro body. The macro is
 * the one that is used at the place of the token in the file: for
 * LOG(WRAPPER(x)) and a token of the body of WRAPPER, the macro WRAPPER.
 * libclang records no use of a macro whose name is an argument of another
 * macro, as X is in LIST(X). The macro is then the outermost one that is
 * used at the place, LIST.
 */
static CXCursor
cxi_macro (CXTranslationUnit tu,
           CXSourceLocation  loc)
{
	CXFile file;
	CXFile spelled;
	unsigned line;
	unsigned column;
	unsigned at;
	unsigned spelled_at;
	clang_getFileLocation(loc, &file, &line, &column, &at);
	clang_getSpellingLocation(loc, &spelled, nullptr, nullptr, &spelled_at);
	if (clang_File_isEqual(file, spelled) && at == spelled_at)
		return clang_getNullCursor();
	CXCursor macro = cxi_symbol(tu, file, line, column);
	if (macro.kind != CXCursor_MacroDefinition) {
		clang_getExpansionLocation(loc, &file, &line, &column, nullptr);
		macro = cxi_symbol(tu, file, line, column);
	}
	return macro.kind == CXCursor_MacroDefinition ? macro
	                                                              : clang_getNullCursor();
}

/* Appends the end of the sentence about a name that an expansion of
 * @p macro writes, without a newline: the macro, and the place and the
 * last line of the definition of the macro. The caller appends the place
 * of the expansion before.
 */
static void
cxi_out_expansion (struct cxi_out *out,
                   CXCursor        macro)
{
	CXSourceLocation const at = clang_getCursorLocation(macro);
	unsigned const last = cxi_line(clang_getRangeEnd(clang_getCursorExtent(macro)));
	cxi_out_lit(out, " is the expansion of the macro ");
	cxi_out_clang(out, clang_getCursorSpelling(macro));
	cxi_out_lit(out, " (");
	cxi_out_place(out, at);
	if (last > cxi_line(at)) {
		cxi_out_lit(out, "-");
		cxi_out_num(out, last);
	}
	cxi_out_lit(out, ").");
}

/* Writes the first symbol named @p name on the line to @p *c. */
static int
cxi_lookup_line (struct cxi *x,
                 char const *name,
                 CXCursor   *c,
                 unsigned   *column)
{
	unsigned const line = (unsigned)x->line;
	CXFile const file = clang_getFile(x->tu, x->file);
	size_t size = 0;
	char const *text = clang_getFileContents(x->tu, file, &size);
	struct cstr_view words = {};
	size_t const total = cxi_find_line(text, size, line, &words);
	size_t const len = strlen(name);
	/* A cursor inside a longer word refers to a name that is not @p name. */
	for (size_t i = 0; i + len <= words.len; ++i) {
		if (memcmp(words.data + i, name, len))
			continue;
		*column = (unsigned)i + 1;
		*c = cxi_symbol(x->tu, file, line, *column);
		if (cxi_is_named(*c, name))
			return 0;
	}
	if (!words.data) {
		cxi_out_fmt(&x->out, "Line %u is past the end of ", line);
		cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
		cxi_out_fmt(&x->out, ". The file contains %zu line%s.", total, util_s(total));
		return EINVAL;
	}
	cxi_out_fmt(&x->out, "Line %u of ", line);
	cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
	cxi_out_lit(&x->out, " contains no symbol named ");
	cxi_out_text(&x->out, name);
	cxi_out_lit(&x->out, ".");
	return EINVAL;
}

/* Returns the complete struct, union or enum of the type of @p c, or null. */
static CXCursor
cxi_tag_of (CXCursor c)
{
	CXType const type = clang_getCanonicalType(clang_getCursorType(c));
	CXCursor const decl = clang_getCursorDefinition(clang_getTypeDeclaration(type));
	return cxi_tag(decl.kind) && clang_Type_getSizeOf(type) >= 0
	       ? decl : clang_getNullCursor();
}

struct cxi_seek {
	char const *name;
	CXCursor    found;
};

/* Finds the member seek->name, also inside anonymous members. */
static enum CXVisitorResult
cxi_seek_visit (CXCursor     c,
                CXClientData data)
{
	struct cxi_seek *seek = data;
	CXType const type = clang_getCanonicalType(clang_getCursorType(c));
	if (clang_Cursor_isAnonymousRecordDecl(clang_getTypeDeclaration(type)))
		clang_Type_visitFields(type, cxi_seek_visit, seek);
	seek->found = clang_Cursor_isNull(seek->found) && cxi_is_named(c, seek->name) ? c
	            : seek->found;
	return clang_Cursor_isNull(seek->found) ? CXVisit_Continue : CXVisit_Break;
}

/* Writes @p member of the record of the first @p name of one to @p *c. */
static int
cxi_lookup_member (struct cxi *x,
                   char const *name,
                   char const *member,
                   CXCursor   *c)
{
	struct cxi_find find = cxi_find(x->out.tree, cstr_view(name), 0);
	if (clang_visitChildren(clang_getTranslationUnitCursor(x->tu), cxi_find_match, &find))
		x->out.e = ENOMEM;
	CXCursor record = clang_getNullCursor();
	for (size_t i = 0; clang_Cursor_isNull(record) && i < find.list.n; ++i) {
		CXCursor const tag = cxi_tag_of(find.list.items[i]);
		record = tag.kind == CXCursor_EnumDecl ? record : tag;
	}
	cxi_list_fini(&find.list);
	if (clang_Cursor_isNull(record)) {
		cxi_out_lit(&x->out, "No struct or union type named ");
		cxi_out_view(&x->out, find.name);
		cxi_out_lit(&x->out, " in the translation unit of ");
		cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
		cxi_out_lit(&x->out, ".");
		return EINVAL;
	}
	struct cxi_seek seek = {
		.name  = member,
		.found = clang_getNullCursor()
	};
	clang_Type_visitFields(clang_getCursorType(record), cxi_seek_visit, &seek);
	*c = seek.found;
	if (!clang_Cursor_isNull(*c))
		return x->out.e;
	cxi_out_lit(&x->out, "No member ");
	cxi_out_view(&x->out, cstr_view(member));
	cxi_out_lit(&x->out, " in ");
	cxi_out_clang(&x->out, clang_getTypeSpelling(clang_getCursorType(record)));
	cxi_out_lit(&x->out, ".");
	return EINVAL;
}

/* Returns the length of the C identifier at the start of the view @p s,
 * or 0 if @p s does not start with one. Reads at most s.len bytes.
 */
static size_t
cxi_ident (struct cstr_view s)
{
	size_t const n = cstr_view_spn(s, UTIL_ALNUM "_");
	return n && (s.data[0] < '0' || s.data[0] > '9') ? n : 0;
}

/* Splits [struct |union |enum ]NAME[.MEMBER] into @p *kind, @p name and
 * @p member.
 */
static int
cxi_parse_name (struct cxi *x,
                char        name[static CXI_NAME + 1],
                char        member[static CXI_NAME + 1],
                uint32_t   *kind)
{
	/* The empty tag of the last row matches any name. */
	static struct {
		struct cstr_view tag;
		uint64_t         kind;
	} const tags[] = {
		{ CSTR_VIEW_INIT("struct "), CXCursor_StructDecl },
		{ CSTR_VIEW_INIT("union "),  CXCursor_UnionDecl  },
		{ CSTR_VIEW_INIT("enum "),   CXCursor_EnumDecl   },
		{ CSTR_VIEW_INIT(""),        0                   },
	};
	struct cstr_view rest = x->key[CXI_KEY_NAME];
	size_t t = 0;
	while (!cstr_view_starts(rest, tags[t].tag))
		++t;
	*kind = tags[t].kind;
	rest = cstr_view_slice(rest, tags[t].tag.len, SIZE_MAX);
	size_t const a = cxi_ident(rest);
	struct cstr_view const dot = cstr_view_slice(rest, a, SIZE_MAX);
	size_t const b = dot.len && *dot.data == '.' ? cxi_ident(cstr_view_slice(dot, 1, SIZE_MAX))
	                                             : 0;
	if (!a || a > CXI_NAME || b > CXI_NAME || dot.len != (b ? b + 1 : 0)) {
		cxi_out_lit(&x->out, "The name argument must be a C name, such as git_start, "
		            "struct dstr or child.flags.");
		return EINVAL;
	}
	memcpy(name, rest.data, a);
	name[a] = '\0';
	memcpy(member, dot.data + !!b, b);
	member[b] = '\0';
	return 0;
}

/* Returns the struct or union @p c, or the struct or union around @p c
 * that is not an anonymous member of another one.
 */
static CXCursor
cxi_record_around (CXCursor c)
{
	while (clang_Cursor_isAnonymousRecordDecl(c))
		c = clang_getCursorSemanticParent(c);
	return c;
}

/* Appends the field @p c with its place in the outermost named record. */
static void
cxi_member (struct cxi *x,
            CXCursor    c)
{
	CXCursor const parent = clang_getCursorSemanticParent(c);
	CXCursor const outer = cxi_record_around(parent);
	CXType const record = clang_getCursorType(outer);
	CXType const type = clang_getCursorType(c);
	CXString name = clang_getCursorSpelling(c);
	long long const offset = clang_Type_getOffsetOf(record, clang_getCString(name));
	long long const size = clang_Cursor_isBitField(c) ? clang_getFieldDeclBitWidth(c)
	                     : 8 * clang_Type_getSizeOf(type);
	cxi_out_lit(&x->out, "field ");
	cxi_out_clang(&x->out, name);
	cxi_out_lit(&x->out, " of ");
	cxi_out_clang(&x->out, clang_getTypeSpelling(record));
	if (!clang_equalCursors(parent, outer)) {
		cxi_out_lit(&x->out, ", in an anonymous ");
		cxi_out_view(&x->out, cxi_kind(parent.kind));
	}
	cxi_out_lit(&x->out, ": ");
	cxi_out_clang(&x->out, clang_getTypeSpelling(type));
	char bits[2][CXI_BITS];
	cxi_out_fmt(&x->out, "\noffset %s, size %s, alignment %lld\n",
	            cxi_offset(bits[0], offset > 0 ? (uint64_t)offset : 0),
	            cxi_size(bits[1], size > 0 ? (uint64_t)size : 0),
	            clang_Type_getAlignOf(type));
}

/* Appends the value of the enumerator @p c, signed or unsigned as @p type
 * is, the canonical integer type of the enum.
 */
static void
cxi_out_enum_value (struct cxi_out *out,
                    CXCursor        c,
                    CXType          type)
{
	if (clang_Type_getSizeOf(type) > 8)
		cxi_out_lit(out, "a value of more than 64 bits");
	else if (type.kind >= CXType_Bool && type.kind <= CXType_ULongLong)
		cxi_out_num(out, clang_getEnumConstantDeclUnsignedValue(c));
	else
		cxi_out_fmt(out, "%lld", clang_getEnumConstantDeclValue(c));
}

/* Appends the enum @p c in the spelling of clang, with the word enum
 * before a spelling without the word. clang spells an enum without a tag
 * as "enum (unnamed at PLACE)", or as the name of the typedef of the enum.
 */
static void
cxi_out_enum_name (struct cxi_out *out,
                   CXCursor        c)
{
	CXString spelling = clang_getTypeSpelling(clang_getCursorType(c));
	if (!cstr_view_starts(cstr_view(clang_getCString(spelling)), CSTR_VIEW("enum ")))
		cxi_out_lit(out, "enum ");
	cxi_out_clang(out, spelling);
}

/* Appends the place of the enumerator @p e in the rows of an enum that is
 * defined in @p file: @p before and the line in @p width columns, or the
 * path and the line if the enumerator is written in another file.
 */
static void
cxi_out_enum_place (struct cxi_out *out,
                    CXCursor        e,
                    CXFile          file,
                    char const     *before,
                    int             width)
{
	CXSourceLocation const at = clang_getCursorLocation(e);
	CXFile in;
	unsigned line;
	clang_getFileLocation(at, &in, &line, nullptr, nullptr);
	if (clang_File_isEqual(in, file))
		cxi_out_fmt(out, "%s%*u", before, width, line);
	else
		cxi_out_place(out, at);
}

/* Appends the line before the rows of the enumerators that one macro
 * expansion writes, from the enumerator @p i of the @p n enumerators
 * @p kids on: the place where the macro is used, the macro, and the number
 * of these enumerators. One macro writes the names at one line. Returns
 * the index after the last of these enumerators, or @p i + 1 if no macro
 * writes the name of the enumerator @p i.
 */
static size_t
cxi_enum_run (struct cxi            *x,
              struct cxi_out        *rows,
              struct cxi_list const *kids,
              size_t                 n,
              size_t                 i,
              CXFile                 file)
{
	CXSourceLocation const at = clang_getCursorLocation(kids->items[i]);
	CXCursor const macro = cxi_macro(x->tu, at);
	if (clang_Cursor_isNull(macro))
		return i + 1;
	CXFile in;
	unsigned line;
	size_t end = i + 1;
	clang_getFileLocation(at, &in, &line, nullptr, nullptr);
	for (; end < n; ++end) {
		CXSourceLocation const next = clang_getCursorLocation(kids->items[end]);
		CXFile next_in;
		unsigned next_line;
		clang_getFileLocation(next, &next_in, &next_line, nullptr, nullptr);
		if (next_line != line || !clang_File_isEqual(next_in, in) ||
		    !clang_equalCursors(cxi_macro(x->tu, next), macro))
			break;
	}
	cxi_out_enum_place(rows, kids->items[i], file, "line ", 0);
	cxi_out_expansion(rows, macro);
	cxi_out_fmt(rows, " The expansion writes the next %zu enumerator%s.\n", end - i,
	            util_s(end - i));
	return end;
}

/* Appends the integer type and the number of enumerators of the enum @p c
 * to @p head, and a row for each enumerator, at most CXI_ROWS, to @p rows.
 * A line before the rows of the enumerators that one macro expansion
 * writes contains the macro and the number of these enumerators.
 */
static void
cxi_enum (struct cxi     *x,
          struct cxi_out *head,
          struct cxi_out *rows,
          CXCursor        c)
{
	CXType const type = clang_getEnumDeclIntegerType(c);
	CXType const canonical = clang_getCanonicalType(type);
	struct cxi_list kids = {};
	CXFile file;
	size_t n = 0;
	clang_getFileLocation(clang_getCursorLocation(c), &file, nullptr, nullptr, nullptr);
	if (cxi_list_children(&kids, c))
		rows->e = ENOMEM;
	/* A child is also an attribute or the name of the integer type. */
	for (size_t i = 0; i < kids.n; ++i) {
		if (kids.items[i].kind == CXCursor_EnumConstantDecl)
			kids.items[n++] = kids.items[i];
	}
	size_t const shown = n < CXI_ROWS ? n : CXI_ROWS;
	cxi_out_lit(head, ": ");
	cxi_out_clang(head, clang_getTypeSpelling(type));
	cxi_out_fmt(head, ", %zu enumerator%s\n", n, util_s(n));
	cxi_out_lit(rows, "   line  enumerator = value\n");
	/* The enumerators from i to end - 1 are those of one expansion. */
	for (size_t i = 0, end = 0; i < shown; ++i) {
		CXCursor const e = kids.items[i];
		if (i == end)
			end = cxi_enum_run(x, rows, &kids, n, i, file);
		cxi_out_enum_place(rows, e, file, "", 7);
		cxi_out_lit(rows, "  ");
		cxi_out_clang(rows, clang_getCursorSpelling(e));
		cxi_out_lit(rows, " = ");
		cxi_out_enum_value(rows, e, canonical);
		cxi_out_lit(rows, "\n");
	}
	if (n > shown)
		cxi_out_fmt(rows, "[%zu more enumerator%s. To describe one enumerator, give "
		            "the name of the enumerator.]\n", n - shown, util_s(n - shown));
	cxi_list_fini(&kids);
}

/* Appends the kind, name and type of @p c and the value of an enumerator.
 * For a record, an enum and a typedef of one, appends the size of the
 * record or the number of enumerators, and the layout or the enumerators
 * to @p rows. For an enumerator of an enum without a name, appends the
 * enum, the number of enumerators and the enumerators to @p rows, and
 * returns true: these rows contain the macro that writes the enumerator.
 */
static bool
cxi_summary (struct cxi     *x,
             struct cxi_out *rows,
             CXCursor        c)
{
	enum CXCursorKind const k = c.kind;
	bool const alias = k == CXCursor_TypedefDecl;
	CXCursor const parent = clang_getCursorSemanticParent(c);
	bool const local = parent.kind == CXCursor_FunctionDecl;
	cxi_out_view(&x->out, cxi_static(c));
	if (k == CXCursor_VarDecl && local)
		cxi_out_lit(&x->out, "local ");
	cxi_out_view(&x->out, cxi_kind(c.kind));
	cxi_out_lit(&x->out, " ");
	cxi_out_name(&x->out, c);
	if (local) {
		cxi_out_view(&x->out, k == CXCursor_ParmDecl ? CSTR_VIEW(" of ")
		                                             : CSTR_VIEW(" in "));
		cxi_out_clang(&x->out, clang_getCursorSpelling(parent));
	}
	if (k == CXCursor_EnumConstantDecl) {
		cxi_out_lit(&x->out, " of ");
		cxi_out_enum_name(&x->out, parent);
		cxi_out_lit(&x->out, ": ");
		cxi_out_enum_value(&x->out, c, clang_getCanonicalType(
			clang_getEnumDeclIntegerType(parent)));
	}
	CXType const type = alias ? clang_getTypedefDeclUnderlyingType(c)
	                  : k == CXCursor_EnumDecl ? clang_getEnumDeclIntegerType(c)
	                  : clang_getCursorType(c);
	/* tag is c for the definition of a struct, union or enum. In C, the
	 * type of an enumerator is int if the value fits. */
	CXCursor const tag = k == CXCursor_EnumConstantDecl ? parent : cxi_tag_of(c);
	bool const is_tag = clang_equalCursors(tag, c);
	bool const listed = k == CXCursor_EnumConstantDecl && cxi_unnamed(tag);
	if (k != CXCursor_EnumConstantDecl && k != CXCursor_MacroDefinition &&
	    k != CXCursor_StructDecl && k != CXCursor_UnionDecl && !is_tag) {
		cxi_out_lit(&x->out, ": ");
		cxi_out_clang(&x->out, clang_getTypeSpelling(type));
	}
	if (clang_Cursor_isNull(tag) || !(alias || is_tag || listed)) {
		cxi_out_lit(&x->out, "\n");
		return false;
	}
	if (!is_tag)
		cxi_out_lit(&x->out, "\n");
	if (alias)
		cxi_out_clang(&x->out, clang_getTypeSpelling(type));
	if (listed)
		cxi_out_enum_name(rows, tag);
	if (tag.kind == CXCursor_EnumDecl)
		cxi_enum(x, listed ? rows : &x->out, rows, tag);
	else
		cxi_describe_layout(x, rows, clang_getCursorType(tag));
	return listed;
}

/* How the program evaluates an expression of a function. The state of an
 * expression is the largest of the states of its place in each expression
 * that contains it.
 */
enum cxi_state : uint32_t {
	CXI_EVALUATED,    /* evaluated when the program reaches the code */
	CXI_UNPROVEN,     /* the tool cannot tell if the program evaluates it */
	CXI_UNEVALUATED,  /* never evaluated, such as an operand of sizeof */
};

enum cxi_call_flag : uint32_t {
	CXI_CALL_ELEMENT = 1U << 0,  /* the callee is an element of an array */
	CXI_CALL_BUILTIN = 1U << 1,  /* the callee is a builtin function that no
	                                file declares */
	CXI_CALL_CLEANUP = 1U << 2,  /* the call is that of the attribute cleanup
	                                of a variable */
};

/* A call of a function. at is the call expression, or the variable with
 * the attribute cleanup: the program calls the function of the attribute
 * at the end of the scope of the variable. The callees are the n
 * declarations from first on in the member to of struct cxi_walk. One
 * callee is the first declaration of a function, or of the field, variable
 * or parameter of a function pointer. Several callees are the functions of
 * which an expression selects one. n is 0 for any other callee. macro is
 * the definition of the macro that the call is written through
 * (cxi_macro()), or null. For a call that is CXI_UNEVALUATED, n is 0 and
 * macro is null: the call is only counted. row is the index of the first
 * call of the function with the same state, flags, macro and callees
 * (cxi_calls_push()): an answer prints these calls as one row.
 */
struct cxi_call {
	CXCursor at;
	CXCursor macro;
	uint64_t row;
	uint32_t first;
	uint32_t n;
	uint32_t state;  /* enum cxi_state */
	uint32_t flags;  /* enum cxi_call_flag */
};

static force_inline struct cxi_call
cxi_call (CXCursor           at,
          uint32_t           first,
          uint32_t           n,
          enum cxi_call_flag flags)
{
	return (struct cxi_call){
		.at    = at,
		.macro = clang_getNullCursor(),
		.first = first,
		.n     = n,
		.flags = flags,
	};
}

/* A node of the path of a walk: a cursor, and the number of the children of
 * the cursor that the walk reached. The child on the path is the last one
 * of them.
 */
struct cxi_node {
	CXCursor c;
	uint64_t kids;
};

/* A walk through the code of a function, in the order of
 * clang_visitChildren(). The nodes are the path from the function to the
 * cursor of the visit. kids and to are stacks. A function that looks at the
 * children of an expression appends them to kids, and removes them before
 * it returns. to contains the callees of calls (cxi_walk_callee()). e is
 * the error that stopped the visit, which a visitor cannot return.
 */
struct cxi_walk {
	UTIL_ARRAY(struct cxi_node);
	struct cxi_list   kids;
	struct cxi_list   to;
	CXTranslationUnit tu;
	int64_t           e;
};

/* Frees the path and the stacks of @p walk. */
static void
cxi_walk_fini (struct cxi_walk *walk)
{
	cxi_list_fini(&walk->to);
	cxi_list_fini(&walk->kids);
	free(walk->items);
	*walk = (struct cxi_walk){};
}

/* Starts a walk through the function @p root of @p tu: the path of @p walk
 * is @p root. The stacks keep their memory. Returns 0 or ENOMEM.
 */
static int
cxi_walk_start (struct cxi_walk   *walk,
                CXTranslationUnit  tu,
                CXCursor           root)
{
	walk->n = walk->kids.n = walk->to.n = 0;
	walk->tu = tu;
	walk->e = 0;
	return util_push(*walk, (struct cxi_node){ .c = root });
}

/* Moves the path of @p walk to @p c, the cursor of a visit, which is a
 * child of the node @p parent of the path. Returns 0, ENOMEM, or EALREADY
 * if @p c is the child before it once more: libclang visits some operands
 * more than once, such as the first operand of ?: without a second.
 */
static int
cxi_walk_step (struct cxi_walk *walk,
               CXCursor         c,
               CXCursor         parent)
{
	while (walk->n > 1 && !clang_equalCursors(walk->items[walk->n - 1].c, parent))
		--walk->n;
	struct cxi_node *up = &walk->items[walk->n - 1];
	/* The node after the path is the child of parent before c: the walk
	 * left it there when it moved the path back to parent. */
	if (up->kids++ && clang_equalCursors(up[1].c, c))
		return EALREADY;
	return util_push(*walk, (struct cxi_node){ .c = c });
}

/* Stores the first child of a cursor in the CXCursor at @p data. */
static enum CXChildVisitResult
cxi_first (CXCursor     c,
           CXCursor,
           CXClientData data)
{
	*(CXCursor *)data = c;
	return CXChildVisit_Break;
}

/* Returns the text of a file from the first token of @p c to the end of
 * the file, and writes the text before the token to @p *before. The file
 * is the one in which the token is written: for a token of the body of a
 * macro, the file of the macro definition. Both texts are empty if the
 * token is written in no file.
 */
static struct cstr_view
cxi_spelling (CXTranslationUnit  tu,
              CXCursor           c,
              struct cstr_view  *before)
{
	CXFile file;
	unsigned at;
	clang_getSpellingLocation(clang_getRangeStart(clang_getCursorExtent(c)), &file, nullptr,
	                          nullptr, &at);
	size_t size = 0;
	char const *text = file ? clang_getFileContents(tu, file, &size) : nullptr;
	struct cstr_view const all = cstr_view_n(text, text ? size : 0);
	*before = cstr_view_slice(all, 0, at);
	return cstr_view_slice(all, at, SIZE_MAX);
}

/* Returns @p s without the white space at the start (cstr_view_space()). */
static struct cstr_view
cxi_skip_space (struct cstr_view s)
{
	while (s.len && cstr_view_space(*s.data))
		s = cstr_view_slice(s, 1, SIZE_MAX);
	return s;
}

/* Returns true if clang evaluates @p c to an integer constant, and writes
 * to @p *nonzero whether the constant is not 0.
 */
static bool
cxi_constant (CXCursor  c,
              bool     *nonzero)
{
	CXEvalResult result = clang_Cursor_Evaluate(c);
	bool const constant = result && clang_EvalResult_getKind(result) == CXEval_Int;
	*nonzero = constant && clang_EvalResult_getAsLongLong(result);
	if (result)
		clang_EvalResult_dispose(result);
	return constant;
}

/* Returns true if the expression @p c is of the canonical type @p type. */
static bool
cxi_of_type (CXCursor c,
             CXType   type)
{
	return clang_equalTypes(clang_getCanonicalType(clang_getCursorType(c)), type);
}

/* Returns the index of the child that the compiler selects of the @p n
 * children @p kids of the _Generic selection @p c, or @p n if the tool
 * cannot prove which child that is. The program evaluates no other child.
 * Writes 1 to @p *first if the first child is the controlling expression,
 * which is never selected, else 0.
 *
 * clang knows the selected association (cxi_ast_generic()). The selection
 * is of the type of the selected association: a child of another type is
 * not accepted as the selected one.
 */
static size_t
cxi_generic (CXCursor        c,
             CXCursor const *kids,
             size_t          n,
             size_t         *first)
{
	size_t at;
	if (!cxi_ast_generic(c, first, &at) && at < n &&
	    cxi_of_type(kids[at], clang_getCanonicalType(clang_getCursorType(c))))
		return at;
	*first = 0;
	return n;
}

/* Returns true if the unexposed expression @p c is written as a
 * __builtin_choose_expr. In libclang, that is an unexposed expression of
 * three children, the condition first. An implicit conversion of one is
 * written in the same way, and is of one child.
 */
static bool
cxi_choose_written (CXTranslationUnit tu,
                    CXCursor          c)
{
	struct cstr_view before;
	struct cstr_view const rest = cxi_spelling(tu, c, &before);
	return cstr_view_equal(cstr_view_slice(rest, 0, cxi_ident(rest)),
	                       CSTR_VIEW("__builtin_choose_expr"));
}

/* Returns the index of the child that the compiler selects of the three
 * children @p kids of a __builtin_choose_expr, 1 or 2, or 3 if clang does
 * not evaluate the condition, the first child, to a constant.
 */
static size_t
cxi_choose (CXCursor const kids[static 3])
{
	bool nonzero;
	return cxi_constant(kids[0], &nonzero) ? 2 - nonzero : 3;
}

/* Returns the state of @p c as an operand of typeof, or CXI_EVALUATED if
 * @p c is none. The operand is an expression in parentheses, and the
 * keyword is written before it with only white space between them. The
 * program evaluates the operand only if the type of the operand is
 * variably modified. A number, a struct, a union and an enum are not.
 */
static uint32_t
cxi_typeof (CXTranslationUnit tu,
            CXCursor          c)
{
	static struct cstr_view const words[] = {
		CSTR_VIEW_INIT("typeof"),
		CSTR_VIEW_INIT("typeof_unqual"),
		CSTR_VIEW_INIT("__typeof__"),
		CSTR_VIEW_INIT("__typeof"),
		CSTR_VIEW_INIT("__typeof_unqual__"),
		CSTR_VIEW_INIT("__typeof_unqual"),
	};
	struct cstr_view before;
	(void)cxi_spelling(tu, c, &before);
	/* line is the line of the word before the parenthesis, up to the end
	 * of the word. */
	struct cstr_view const line = cstr_view_last_line(before);
	size_t n = 0;
	while (n < line.len && line.data[line.len - 1 - n] &&
	       strchr(UTIL_ALNUM "_", line.data[line.len - 1 - n]))
		++n;
	struct cstr_view const word = cstr_view_slice(line, line.len - n, SIZE_MAX);
	if (cstr_view_index(words, array_size(words), word) == array_size(words))
		return CXI_EVALUATED;
	/* A line feed ends a directive and a comment that starts with //: the
	 * word in such a line is not the keyword of a parenthesis in a later
	 * line. */
	char const *end = line.data + line.len;
	if (memchr(end, '\n', (size_t)(before.data + before.len - end)) &&
	    (memchr(line.data, '#', line.len) || memmem(line.data, line.len, "//", 2)))
		return CXI_EVALUATED;
	enum CXTypeKind const t = clang_getCanonicalType(clang_getCursorType(c)).kind;
	return (t >= CXType_FirstBuiltin && t <= CXType_LastBuiltin) || t == CXType_Record ||
	       t == CXType_Enum ? CXI_UNEVALUATED : CXI_UNPROVEN;
}

static int
cxi_walk_callee (struct cxi_walk *walk,
                 CXCursor         e,
                 struct cxi_call *call);

/* Appends the first declaration of what the name @p e refers to to the
 * callees of @p call, unless they contain it. Returns ENOENT if @p e refers
 * to no function, variable, parameter or field.
 */
static int
cxi_walk_callee_name (struct cxi_walk *walk,
                      CXCursor         e,
                      struct cxi_call *call)
{
	CXCursor const d = clang_getCanonicalCursor(clang_getCursorReferenced(e));
	switch (d.kind) {
	case CXCursor_FunctionDecl:
		/* The name of a builtin function that no file declares is of a
		 * type of its own, which libclang does not expose. */
		if (clang_getCanonicalType(clang_getCursorType(e)).kind == CXType_Unexposed)
			call->flags |= CXI_CALL_BUILTIN;
		[[fallthrough]];
	case CXCursor_VarDecl:
	case CXCursor_ParmDecl:
	case CXCursor_FieldDecl:
		for (size_t i = call->first; i < walk->to.n; ++i) {
			if (clang_equalCursors(walk->to.items[i], d))
				return 0;
		}
		return util_push(walk->to, d);
	default:
		return ENOENT;
	}
}

/* Appends the callees of the children of an expression from the index
 * @p i to the index @p end of the stack kids to the callees of @p call:
 * the callee is one of several. With a canonical type @p of, only the
 * children of that type. Several callees are valid only if each one is a
 * function: returns ENOENT otherwise.
 */
static int
cxi_walk_callee_several (struct cxi_walk *walk,
                         size_t           i,
                         size_t           end,
                         CXType const    *of,
                         struct cxi_call *call)
{
	int e = 0;
	for (; !e && i < end; ++i) {
		CXCursor const kid = walk->kids.items[i];
		if (!of || cxi_of_type(kid, *of))
			e = cxi_walk_callee(walk, kid, call);
	}
	for (i = call->first; !e && i < walk->to.n; ++i)
		e = walk->to.items[i].kind == CXCursor_FunctionDecl ? 0 : ENOENT;
	return e;
}

/* Appends the callees of the selection @p e to the callees of @p call. The
 * @p n children of @p e are in the stack kids from the index @p at on, and
 * the compiler selects the child @p chosen of them. If @p chosen is @p n,
 * the tool cannot prove which child is selected: the callee is then each
 * child from @p first on that is of the type of the selection.
 */
static int
cxi_walk_callee_selected (struct cxi_walk *walk,
                          CXCursor         e,
                          size_t           at,
                          size_t           n,
                          size_t           first,
                          size_t           chosen,
                          struct cxi_call *call)
{
	if (chosen < n)
		return cxi_walk_callee(walk, walk->kids.items[at + chosen], call);
	CXType const type = clang_getCanonicalType(clang_getCursorType(e));
	return cxi_walk_callee_several(walk, at + first, at + n, &type, call);
}

/* Appends what the expression @p e refers to to the callees of @p call
 * (cxi_walk_callee()). @p e is no name, and its @p n children are in the
 * stack kids from the index @p at on.
 */
static int
cxi_walk_callee_through (struct cxi_walk *walk,
                         CXCursor         e,
                         size_t           at,
                         size_t           n,
                         struct cxi_call *call)
{
	CXCursor const *kids = walk->kids.items + at;
	switch (e.kind) {
	case CXCursor_GenericSelectionExpr: {
		size_t first;
		size_t const chosen = cxi_generic(e, kids, n, &first);
		return cxi_walk_callee_selected(walk, e, at, n, first, chosen, call);
	}
	case CXCursor_ConditionalOperator:
		/* The second or the third operand of ?:. The condition is never
		 * the callee. */
		return n == 3 ? cxi_walk_callee_several(walk, at + 1, at + 3, nullptr, call)
		              : ENOENT;
	case CXCursor_ArraySubscriptExpr:
		if (n != 2)
			return ENOENT;
		/* The array is the child that is a pointer. */
		call->flags |= CXI_CALL_ELEMENT;
		return cxi_walk_callee(walk, kids[clang_getCanonicalType(clang_getCursorType(
			kids[0])).kind != CXType_Pointer], call);
	case CXCursor_UnaryOperator: {
		/* * and & refer to what the operand refers to. */
		enum CXUnaryOperatorKind const op = clang_getCursorUnaryOperatorKind(e);
		return n == 1 && (op == CXUnaryOperator_Deref || op == CXUnaryOperator_AddrOf)
		       ? cxi_walk_callee(walk, kids[0], call) : ENOENT;
	}
	case CXCursor_ParenExpr:
		return n == 1 ? cxi_walk_callee(walk, kids[0], call) : ENOENT;
	case CXCursor_UnexposedExpr:
		if (n == 3 && cxi_choose_written(walk->tu, e))
			return cxi_walk_callee_selected(walk, e, at, n, 1, cxi_choose(kids), call);
		/* An unexposed expression of the extent of the child, such as an
		 * implicit conversion, refers to what the child refers to. */
		return n == 1 && clang_equalRanges(clang_getCursorExtent(e),
		                                   clang_getCursorExtent(kids[0]))
		       ? cxi_walk_callee(walk, kids[0], call) : ENOENT;
	default:
		return ENOENT;
	}
}

/* Appends what the callee expression @p e refers to to the callees of
 * @p call, which are the members of walk->to from call->first on, and sets
 * the flags of the callee in @p call. Returns 0, ENOMEM, or ENOENT if the
 * tool finds no declaration for @p e, as for the result of a call: the
 * callees of @p call are then not valid.
 */
static int
cxi_walk_callee (struct cxi_walk *walk,
                 CXCursor         e,
                 struct cxi_call *call)
{
	if (e.kind == CXCursor_DeclRefExpr || e.kind == CXCursor_MemberRefExpr)
		return cxi_walk_callee_name(walk, e, call);
	size_t const at = walk->kids.n;
	int err = cxi_list_children(&walk->kids, e);
	if (!err)
		err = cxi_walk_callee_through(walk, e, at, walk->kids.n - at, call);
	walk->kids.n = at;
	return err;
}

/* Sets the callees of the call expression @p c in @p call, from call->first
 * on (cxi_walk_callee()). The callee expression is the first child of
 * @p c. A call without a declaration of the callee is of no callee and
 * without flags.
 */
static int
cxi_walk_call (struct cxi_walk *walk,
               CXCursor         c,
               struct cxi_call *call)
{
	CXCursor callee = clang_getNullCursor();
	clang_visitChildren(c, cxi_first, &callee);
	int e = clang_Cursor_isNull(callee) ? ENOENT : cxi_walk_callee(walk, callee, call);
	if (e == ENOENT) {
		walk->to.n = call->first;
		call->flags = 0;
		e = 0;
	}
	call->n = (uint32_t)walk->to.n - call->first;
	return e;
}

/* Writes to @p *state the state of the child @p kid of the expression
 * @p c, if @p c is a _Generic selection or a __builtin_choose_expr: the
 * program evaluates the child that the compiler selects, and never the
 * controlling expression or the condition, or another child if the tool
 * can prove which child is selected.
 */
static int
cxi_walk_selected (struct cxi_walk *walk,
                   CXCursor         c,
                   size_t           kid,
                   uint32_t        *state)
{
	bool const generic = c.kind == CXCursor_GenericSelectionExpr;
	if (!generic && !cxi_choose_written(walk->tu, c))
		return 0;
	size_t const at = walk->kids.n;
	int const e = cxi_list_children(&walk->kids, c);
	size_t const n = walk->kids.n - at;
	if (!e && (generic || n == 3)) {
		size_t first = 1;
		size_t const chosen = generic ? cxi_generic(c, walk->kids.items + at, n, &first)
		                              : cxi_choose(walk->kids.items + at);
		*state = kid == chosen ? CXI_EVALUATED
		       : kid < first || chosen < n ? CXI_UNEVALUATED : CXI_UNPROVEN;
	}
	walk->kids.n = at;
	return e;
}

/* Writes CXI_UNPROVEN to @p *state if the call expression @p c is a call
 * of a builtin function whose arguments clang does not evaluate for side
 * effects. The program evaluates such an argument only if clang finds no
 * side effect in the argument.
 */
static int
cxi_walk_unevaluated (struct cxi_walk *walk,
                      CXCursor         c,
                      uint32_t        *state)
{
	static struct cstr_view const names[] = {
		CSTR_VIEW_INIT("__builtin_constant_p"),
		CSTR_VIEW_INIT("__builtin_object_size"),
		CSTR_VIEW_INIT("__builtin_dynamic_object_size"),
		CSTR_VIEW_INIT("__builtin_classify_type"),
		CSTR_VIEW_INIT("__builtin_assume"),
		CSTR_VIEW_INIT("__assume"),
	};
	CXCursor const d = clang_getCursorReferenced(c);
	if (d.kind != CXCursor_FunctionDecl)
		return 0;
	CXString name = clang_getCursorSpelling(d);
	bool const listed = cstr_view_index(names, array_size(names),
	                                    cstr_view(clang_getCString(name))) < array_size(names);
	clang_disposeString(name);
	if (!listed)
		return 0;
	/* A function of the program can be named as one of these builtin
	 * functions: the flag of the callee tells them apart. */
	struct cxi_call call = cxi_call(clang_getNullCursor(),
	                                (uint32_t)walk->to.n, 0, 0);
	int const e = cxi_walk_call(walk, c, &call);
	walk->to.n = call.first;
	if (call.flags & CXI_CALL_BUILTIN)
		*state = CXI_UNPROVEN;
	return e;
}

/* Writes the state of the cursor of the visit of @p walk, the last node of
 * the path, to @p *state: the largest state of the place of a node in the
 * node before it. The places that are not CXI_EVALUATED are an operand of
 * a sizeof or alignof, which the program does not evaluate if the
 * expression is a constant, a child of a selection (cxi_walk_selected()),
 * an argument of some builtin functions (cxi_walk_unevaluated()), and the
 * operand of typeof (cxi_typeof()).
 */
static int
cxi_walk_state (struct cxi_walk *walk,
                uint32_t        *state)
{
	int e = 0;
	*state = CXI_EVALUATED;
	for (size_t i = 0; !e && i + 1 < walk->n && *state < CXI_UNEVALUATED; ++i) {
		CXCursor const up = walk->items[i].c;
		CXCursor const c = walk->items[i + 1].c;
		uint32_t place = CXI_EVALUATED;
		bool nonzero;
		switch (up.kind) {
		case CXCursor_GenericSelectionExpr:
		case CXCursor_UnexposedExpr:
			e = cxi_walk_selected(walk, up, walk->items[i].kids - 1, &place);
			break;
		case CXCursor_UnaryExpr:
			place = cxi_constant(up, &nonzero) ? CXI_UNEVALUATED : CXI_UNPROVEN;
			break;
		case CXCursor_CallExpr:
			e = cxi_walk_unevaluated(walk, up, &place);
			break;
		default:
			break;
		}
		uint32_t const operand = c.kind == CXCursor_ParenExpr ? cxi_typeof(walk->tu, c)
		                                                      : CXI_EVALUATED;
		place = place > operand ? place : operand;
		*state = *state > place ? *state : place;
	}
	return e;
}

/* Returns true if the attribute @p c is written as cleanup: the word is
 * cleanup or __cleanup__, after gnu:: or __gnu__:: in [[ ]].
 */
static bool
cxi_cleanup_written (CXTranslationUnit tu,
                     CXCursor          c)
{
	static struct cstr_view const words[] = {
		CSTR_VIEW_INIT("cleanup"),
		CSTR_VIEW_INIT("__cleanup__"),
	};
	struct cstr_view before;
	struct cstr_view rest = cxi_spelling(tu, c, &before);
	size_t word = cxi_ident(rest);
	struct cstr_view const scope = cxi_skip_space(cstr_view_slice(rest, word, SIZE_MAX));
	if (cstr_view_starts(scope, CSTR_VIEW("::"))) {
		rest = cxi_skip_space(cstr_view_slice(scope, 2, SIZE_MAX));
		word = cxi_ident(rest);
	}
	return cstr_view_index(words, array_size(words), cstr_view_slice(rest, 0, word)) <
	       array_size(words);
}

/* Writes the name of the function of the attribute cleanup of the variable
 * @p c to @p name, and returns the length of the name, or 0 if the variable
 * is without the attribute. libclang returns no function of an attribute.
 * clang prints the declaration of the variable from the syntax tree, with
 * the attribute as __attribute__((cleanup(NAME))) or [[gnu::cleanup(NAME)]]:
 * NAME is the function.
 */
static size_t
cxi_cleanup_name (CXCursor c,
                  char     name[static CXI_NAME + 1])
{
	static struct cstr_view const forms[] = {
		CSTR_VIEW_INIT("__attribute__((cleanup("),
		CSTR_VIEW_INIT("[[gnu::cleanup("),
	};
	CXPrintingPolicy policy = clang_getCursorPrintingPolicy(c);
	clang_PrintingPolicy_setProperty(policy, CXPrintingPolicy_SuppressInitializers, 1);
	CXString printed = clang_getCursorPrettyPrinted(c, policy);
	clang_PrintingPolicy_dispose(policy);
	char const *text = clang_getCString(printed);
	char const *form = nullptr;
	size_t f = 0;
	for (; text && !form && f < array_size(forms); ++f)
		form = strstr(text, forms[f].data);
	size_t len = 0;
	if (form) {
		/* The text after the form ends with a NUL. */
		char const *rest = form + forms[f - 1].len;
		size_t const n = cxi_ident(cstr_view(rest));
		if (n <= CXI_NAME && rest[n] == ')') {
			memcpy(name, rest, n);
			len = n;
		}
	}
	name[len] = '\0';
	clang_disposeString(printed);
	return len;
}

/* Writes to @p *macro the macro through which the attribute cleanup of the
 * variable @p c is written (cxi_macro()), or null: the macro of the
 * attribute that is written as cleanup (cxi_cleanup_written()).
 */
static int
cxi_walk_cleanup_macro (struct cxi_walk *walk,
                        CXCursor         c,
                        CXCursor        *macro)
{
	size_t const at = walk->kids.n;
	int const e = cxi_list_children(&walk->kids, c);
	CXCursor written = clang_getNullCursor();
	for (size_t i = at; !e && i < walk->kids.n; ++i) {
		CXCursor const kid = walk->kids.items[i];
		if (clang_isAttribute(kid.kind) && cxi_cleanup_written(walk->tu, kid))
			written = kid;
	}
	walk->kids.n = at;
	*macro = clang_Cursor_isNull(written) ? written
	       : cxi_macro(walk->tu, clang_getCursorLocation(written));
	return e;
}

/* The calls of a function, with the walk that collects them. x is the call
 * of askai-code. The visit of the code of a function (cxi_calls_collect())
 * calls call for a call expression and cleanup for a variable with
 * attributes. Each appends the call that an action wants to the calls, and
 * returns 0 or the error that stops the visit.
 */
struct cxi_calls {
	UTIL_ARRAY(struct cxi_call);
	struct cxi_walk   walk;
	struct cxi       *x;
	int             (*call) (struct cxi_calls *calls, CXCursor c);
	int             (*cleanup) (struct cxi_calls *calls, CXCursor c);
};

/* Returns the calls of a function before a visit collects them, with the
 * functions @p call and @p cleanup of an action.
 */
static force_inline struct cxi_calls
cxi_calls (struct cxi  *x,
           int        (*call) (struct cxi_calls *calls, CXCursor c),
           int        (*cleanup) (struct cxi_calls *calls, CXCursor c))
{
	return (struct cxi_calls){
		.x       = x,
		.call    = call,
		.cleanup = cleanup
	};
}

/* Frees the calls and the walk of @p calls. */
static void
cxi_calls_fini (struct cxi_calls *calls)
{
	cxi_walk_fini(&calls->walk);
	free(calls->items);
	*calls = (struct cxi_calls){};
}

/* Returns true if the calls @p a and @p b of @p calls are of one row. */
static bool
cxi_calls_same (struct cxi_calls const *calls,
                size_t                  a,
                size_t                  b)
{
	struct cxi_call const *p = &calls->items[a];
	struct cxi_call const *q = &calls->items[b];

	if (p->state != q->state ||
	    p->flags != q->flags ||
	    p->n     != q->n     ||
	    !clang_equalCursors(p->macro, q->macro))
		return false;

	for (uint32_t i = 0; i < p->n; ++i) {
		if (!clang_equalCursors(calls->walk.to.items[p->first + i],
		                        calls->walk.to.items[q->first + i]))
			return false;
	}

	return true;
}

/* Appends @p call to @p calls and sets the row of the call. Returns 0 or
 * ENOMEM.
 */
static int
cxi_calls_push (struct cxi_calls *calls,
                struct cxi_call   call)
{
	int const e = util_push(*calls, call);
	if (e)
		return e;
	size_t const last = calls->n - 1;
	size_t row = 0;
	while (!cxi_calls_same(calls, row, last))
		++row;
	calls->items[last].row = row;
	return 0;
}

/* Appends the call expression @p c, the cursor of the visit, to @p calls,
 * with the state of the call, and for a call that the program can evaluate
 * the macro and the callees.
 */
static int
cxi_calls_add (struct cxi_calls *calls,
               CXCursor          c)
{
	struct cxi_walk *walk = &calls->walk;
	struct cxi_call call = cxi_call(c, (uint32_t)walk->to.n, 0, 0);
	int e = cxi_walk_state(walk, &call.state);
	if (!e && call.state != CXI_UNEVALUATED) {
		call.macro = cxi_macro(walk->tu, clang_getCursorLocation(c));
		e = cxi_walk_call(walk, c, &call);
	}
	return e ? e : cxi_calls_push(calls, call);
}

/* The search for the first declaration at file scope of the function
 * name.
 */
struct cxi_function {
	char const *name;
	CXCursor    found;
};

/* Stores the first function named as the cxi_function at @p data wants in
 * it.
 */
static enum CXChildVisitResult
cxi_function_named (CXCursor     c,
                    CXCursor,
                    CXClientData data)
{
	struct cxi_function *fn = data;
	if (c.kind != CXCursor_FunctionDecl || !cxi_spelled(c, fn->name))
		return CXChildVisit_Continue;

	fn->found = c;
	return CXChildVisit_Break;
}

/* Appends to @p calls the call of the function of the attribute cleanup of
 * the variable @p c, the cursor of the visit, if the variable is one with
 * the attribute and the program can evaluate the declaration. The program
 * calls the function at the end of the scope of the variable. The call is
 * of no callee if no declaration at file scope declares the function.
 */
static int
cxi_calls_add_cleanup (struct cxi_calls *calls,
                       CXCursor          c)
{
	char name[CXI_NAME + 1];
	struct cxi_walk *walk = &calls->walk;
	struct cxi_function function = {
		.name  = name,
		.found = clang_getNullCursor()
	};
	struct cxi_call call = cxi_call(c, (uint32_t)walk->to.n,
	                                0, CXI_CALL_CLEANUP);
	if (!cxi_cleanup_name(c, name))
		return 0;
	int e = cxi_walk_state(walk, &call.state);
	if (e || call.state == CXI_UNEVALUATED)
		return e;
	e = cxi_walk_cleanup_macro(walk, c, &call.macro);
	clang_visitChildren(clang_getTranslationUnitCursor(walk->tu), cxi_function_named,
	                    &function);
	if (!e && !clang_Cursor_isNull(function.found)) {
		e = util_push(walk->to, clang_getCanonicalCursor(function.found));
		call.n = !e;
	}
	return e ? e : cxi_calls_push(calls, call);
}

/* Moves the walk of the cxi_calls at @p data to @p c, and calls the
 * function of the calls for a call expression or for a variable with
 * attributes.
 */
static enum CXChildVisitResult
cxi_calls_visit (CXCursor     c,
                 CXCursor     parent,
                 CXClientData data)
{
	struct cxi_calls *calls = data;
	int e = cxi_walk_step(&calls->walk, c, parent);
	if (e == EALREADY)
		return CXChildVisit_Continue;
	if (!e && c.kind == CXCursor_CallExpr)
		e = calls->call(calls, c);
	else if (!e && c.kind == CXCursor_VarDecl && clang_Cursor_hasAttrs(c))
		e = calls->cleanup(calls, c);
	calls->walk.e = e;
	return e ? CXChildVisit_Break : CXChildVisit_Recurse;
}

/* Replaces the calls of @p calls with those in the code of the function
 * @p c. Returns 0 or the error that stopped the visit.
 */
static int
cxi_calls_collect (struct cxi_calls *calls,
                   CXCursor          c)
{
	calls->n = 0;
	int const e = cxi_walk_start(&calls->walk, calls->x->tu, c);
	if (e)
		return e;
	clang_visitChildren(c, cxi_calls_visit, calls);
	return (int)calls->walk.e;
}

/* Appends the file of the call @p i of @p calls, the first one of a row,
 * @p sep, the line of the call, and the lines of the later calls of the
 * row.
 */
static void
cxi_calls_places (struct cxi_calls const *calls,
                  size_t                  i,
                  char                    sep)
{
	struct cxi_out *out = &calls->x->out;
	unsigned line = cxi_out_file(out, clang_getCursorLocation(calls->items[i].at));
	cxi_out_put(out, &sep, 1);
	cxi_out_num(out, line);
	for (size_t j = i + 1; j < calls->n; ++j) {
		unsigned const l = cxi_line(clang_getCursorLocation(calls->items[j].at));
		if (l == line || calls->items[j].row != i)
			continue;
		cxi_out_lit(out, ",");
		cxi_out_num(out, l);
		line = l;
	}
}

/* Appends the row of the call @p i of @p calls: the places of the calls of
 * the row, the callees, the attribute cleanup, the macro that the calls of
 * the row are written through, and what the tool cannot tell about the
 * calls.
 */
static void
cxi_calls_row (struct cxi_calls const *calls,
               size_t                  i)
{
	struct cxi_out *out = &calls->x->out;
	struct cxi_call const *call = &calls->items[i];
	bool const cleanup = call->flags & CXI_CALL_CLEANUP;
	/* which is true for several functions, and for the function of an
	 * attribute cleanup that the tool did not find. */
	bool const which = call->n > 1 || (cleanup && !call->n);
	bool const unproven = call->state == CXI_UNPROVEN;
	cxi_out_lit(out, "  ");
	cxi_calls_places(calls, i, ':');
	cxi_out_lit(out, " ");
	for (uint32_t f = 0; f < call->n; ++f) {
		CXCursor const to = calls->walk.to.items[call->first + f];
		enum CXCursorKind const k = to.kind;
		if (f)
			cxi_out_lit(out, " or ");
		if (k != CXCursor_FunctionDecl) {
			cxi_out_view(out, call->flags & CXI_CALL_ELEMENT
			             ? CSTR_VIEW("indirect, through an element of the ")
			             : CSTR_VIEW("indirect, through the "));
			cxi_out_view(out, k == CXCursor_FieldDecl ? CSTR_VIEW("member")
			                                          : cxi_kind(k));
			cxi_out_lit(out, " ");
		}
		cxi_out_clang(out, clang_getCursorSpelling(to));
		if (k == CXCursor_FieldDecl) {
			cxi_out_lit(out, " of ");
			cxi_out_clang(out, clang_getTypeSpelling(clang_getCursorType(
				cxi_record_around(clang_getCursorSemanticParent(to)))));
		}
		if (call->flags & CXI_CALL_BUILTIN) {
			cxi_out_lit(out, " (builtin)");
		} else {
			cxi_out_lit(out, " (");
			cxi_out_place(out, clang_getCursorLocation(to));
			cxi_out_lit(out, ")");
		}
	}
	if (!call->n)
		cxi_out_view(out, cleanup ? CSTR_VIEW("a function")
		                          : CSTR_VIEW("indirect, through an expression"));
	if (cleanup)
		cxi_out_lit(out, ", at the end of the scope of a variable (attribute cleanup)");
	if (!clang_Cursor_isNull(call->macro)) {
		cxi_out_lit(out, ", through the macro ");
		cxi_out_clang(out, clang_getCursorSpelling(call->macro));
		cxi_out_lit(out, " (");
		cxi_out_place(out, clang_getCursorLocation(call->macro));
		cxi_out_lit(out, ")");
	}
	if (which || unproven)
		cxi_out_lit(out, ".");
	if (which)
		cxi_out_lit(out, " The tool cannot tell which function is called.");
	if (unproven)
		cxi_out_lit(out, " The tool cannot tell if the program evaluates the call.");
	cxi_out_lit(out, "\n");
}

/* Appends the calls of the function @p c: a row for each callee, and after
 * the rows the count and the places of the call expressions that the
 * program does not evaluate.
 */
static int
cxi_describe_calls (struct cxi *x,
                    CXCursor    c)
{
	struct cxi_calls calls = cxi_calls(x, cxi_calls_add, cxi_calls_add_cleanup);
	int const e = cxi_calls_collect(&calls, c);
	size_t rows = 0;
	size_t unevaluated = 0;
	size_t first = 0;
	for (size_t i = 0; !e && i < calls.n; ++i) {
		struct cxi_call const *call = &calls.items[i];
		if (call->state == CXI_UNEVALUATED) {
			first = call->row;
			++unevaluated;
		} else if (call->row == i) {
			if (!rows++)
				cxi_out_lit(&x->out, "calls:\n");
			cxi_calls_row(&calls, i);
		}
	}
	if (unevaluated) {
		cxi_out_fmt(&x->out, "[The program does not evaluate %zu call expression%s at ",
		            unevaluated, util_s(unevaluated));
		cxi_calls_places(&calls, first, ':');
		cxi_out_view(&x->out, unevaluated == 1
		             ? CSTR_VIEW(". The rows do not contain this call expression.]\n")
		             : CSTR_VIEW(". The rows do not contain these call expressions.]\n"));
	}
	cxi_calls_fini(&calls);
	return e;
}

static int
cxi_describe (struct cxi *x)
{
	char name[CXI_NAME + 1];
	char member[CXI_NAME + 1];
	uint32_t kind;
	unsigned column = 0;
	CXCursor c;
	int e = cxi_parse_name(x, name, member, &kind);
	if (e)
		return e;
	e = x->line ? cxi_lookup_line(x, *member ? member : name, &c, &column)
	  : *member ? cxi_lookup_member(x, name, member, &c)
	  : cxi_lookup(x, name, kind, &c);
	if (e)
		return e;
	enum CXCursorKind const k = c.kind;
	CXCursor const shown = cxi_shown(c);
	CXCursor const first = clang_getCanonicalCursor(c);
	bool const defined = k == CXCursor_MacroDefinition || clang_isCursorDefinition(shown);
	cxi_head(x);
	cxi_out_view(&x->out, cxi_kind(c.kind));
	cxi_out_lit(&x->out, " ");
	if (*member && !x->line) {
		cxi_out_view(&x->out, cstr_view(name));
		cxi_out_lit(&x->out, ".");
		cxi_out_view(&x->out, cstr_view(member));
	} else {
		cxi_out_name(&x->out, c);
	}
	cxi_out_lit(&x->out, " in the translation unit.]\n");
	struct cxi_out rows = cxi_out(x->out.tree);
	bool listed = false;
	int calls = 0;
	if (k == CXCursor_FieldDecl)
		cxi_member(x, shown);
	else
		listed = cxi_summary(x, &rows, shown);
	if (!defined || !clang_equalCursors(shown, first)) {
		cxi_out_lit(&x->out, "declared ");
		cxi_out_place(&x->out, clang_getCursorLocation(first));
		cxi_out_lit(&x->out, "\n");
	}
	if (defined) {
		cxi_out_lit(&x->out, "defined ");
		cxi_out_place(&x->out, clang_getCursorLocation(shown));
		cxi_out_lit(&x->out, "\n");
		if (k == CXCursor_FunctionDecl)
			calls = cxi_describe_calls(x, shown);
	} else {
		cxi_out_lit(&x->out, "not defined in the translation unit\n");
	}
	cxi_out_put(&x->out, dstr_get(&rows.text), dstr_len(&rows.text));
	if (k == CXCursor_MacroDefinition && x->line)
		cxi_value(x, column);
	/* The rows of the enum of an enumerator contain the macro. */
	CXSourceLocation const at = clang_getCursorLocation(shown);
	CXCursor const macro = listed ? clang_getNullCursor() : cxi_macro(x->tu, at);
	if (!clang_Cursor_isNull(macro)) {
		cxi_out_lit(&x->out, "line ");
		cxi_out_num(&x->out, cxi_line(at));
		cxi_out_expansion(&x->out, macro);
		cxi_out_lit(&x->out, "\n");
	}
	cxi_declaration(x, shown);
	x->out.e = x->out.e ? x->out.e : rows.e;
	cxi_out_fini(&rows);
	return x->out.e ? x->out.e : calls;
}

static void
cxi_out_diagnostic (struct cxi_out            *out,
                    CXDiagnostic               d,
                    enum CXDiagnosticSeverity  severity)
{
	CXSourceLocation const loc = clang_getDiagnosticLocation(d);
	CXFile file;
	clang_getExpansionLocation(loc, &file, nullptr, nullptr, nullptr);
	if (file) {
		cxi_out_place(out, loc);
		cxi_out_lit(out, ": ");
	}
	cxi_out_view(out, severity >= CXDiagnostic_Error ? CSTR_VIEW("error: ")
	                                                 : CSTR_VIEW("warning: "));
	cxi_out_clang(out, clang_getDiagnosticSpelling(d));
	CXString option = clang_getDiagnosticOption(d, nullptr);
	char const *o = clang_getCString(option);
	if (o && *o) {
		cxi_out_lit(out, " [");
		cxi_out_text(out, o);
		cxi_out_lit(out, "]");
	}
	clang_disposeString(option);
	cxi_out_lit(out, "\n");
}

/* Returns the severity of @p d in an answer: clang's, or
 * CXDiagnostic_Ignored for the message that clang cannot open a file.
 */
static enum CXDiagnosticSeverity
cxi_severity (CXDiagnostic d)
{
	CXString text = clang_getDiagnosticSpelling(d);
	bool const open = cstr_view_starts(cstr_view(clang_getCString(text)),
	                                   CSTR_VIEW("cannot open file '"));
	clang_disposeString(text);
	return open ? CXDiagnostic_Ignored : clang_getDiagnosticSeverity(d);
}

/* Appends the errors and warnings, but not the error that clang cannot
 * open a file: every file of the tree can be opened, and for one outside
 * it the reason, such as "Permission denied", would show whether it exists.
 * The error that the file is not found follows it.
 */
static int
cxi_diagnostics (struct cxi *x)
{
	struct cxi_out rows = cxi_out(x->out.tree);
	unsigned errors = 0;
	unsigned warnings = 0;
	for (unsigned i = 0, n = clang_getNumDiagnostics(x->tu); i < n; ++i) {
		CXDiagnostic d = clang_getDiagnostic(x->tu, i);
		enum CXDiagnosticSeverity const severity = cxi_severity(d);
		errors += severity >= CXDiagnostic_Error;
		warnings += severity == CXDiagnostic_Warning;
		if (severity >= CXDiagnostic_Warning)
			cxi_out_diagnostic(&rows, d, severity);
		clang_disposeDiagnostic(d);
	}
	cxi_head(x);
	cxi_out_fmt(&x->out, "%u error%s and %u warning%s with the flags of the build",
	            errors, util_s(errors), warnings, util_s(warnings));
	for (size_t i = 0; i < x->n_warnings; ++i) {
		cxi_out_view(&x->out, i ? CSTR_VIEW(" ") : CSTR_VIEW(" and "));
		cxi_out_view(&x->out, cstr_view(x->warning[i]));
	}
	cxi_out_lit(&x->out, ".]\n");
	cxi_out_put(&x->out, dstr_get(&rows.text), dstr_len(&rows.text));
	x->out.e = x->out.e ? x->out.e : rows.e;
	cxi_out_fini(&rows);
	return x->out.e;
}

/* Returns the last word of @p words, the path of the file of a clang
 * command line (cdb_command()).
 */
static char const *
cxi_last_word (struct cstr_view words)
{
	char const *last = words.len ? memrchr(words.data, '\0', words.len - 1) : nullptr;
	return last ? last + 1 : words.data;
}

/* Parses the file of the clang command line @p words into @p *tu, with the
 * options of askai-code and the warnings of the request after the words.
 * Returns the error of libclang, or CXError_Crashed if out of memory.
 */
static enum CXErrorCode
cxi_tu (struct cxi        *x,
        struct cstr_view   words,
        CXTranslationUnit *tu)
{
	/* -fparse-all-comments gives describe the plain comments of a
	 * declaration; with it, -Wdocumentation would warn about them. */
	static char const *const extra[] = {
		"-Xclang", "-disable-pragma-debug-crash", "-fparse-all-comments",
	};
	size_t const n_extra = array_size(extra) - (x->action != CXI_ACTION_DESCRIBE);
	size_t n = 0;
	for (size_t i = 0; i < words.len; ++i)
		n += !words.data[i];
	size_t const argc = n + n_extra + x->n_warnings;
	char const **argv = malloc(argc * sizeof *argv);
	if (!argv)
		return CXError_Crashed;
	char const *word = words.data;
	for (size_t i = 0; i < n; ++i, word += strlen(word) + 1)
		argv[i] = word;
	memcpy(argv + n, extra, n_extra * sizeof *argv);
	memcpy(argv + n + n_extra, x->warning, x->n_warnings * sizeof *argv);
	enum CXErrorCode const failed = clang_parseTranslationUnit2FullArgv(
		x->index, nullptr, argv, (int)argc, nullptr, 0,
		CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_KeepGoing, tu);
	free(argv);
	return failed;
}

/* A translation unit of callers: the search for the calls of the functions
 * of a name in one parse. One translation unit declares one function of a
 * name. named is the first declaration of that function, or null while the
 * search has reached none. wanted is true if the function is one of the
 * search, and key is then the callee of its rows (cxi_unit_found()).
 *
 * linkage is CXLinkage_Invalid for a search for each function of the name.
 * With a path in the request, the search is for the one function that the
 * translation unit of the path declares: linkage is the linkage of that
 * function, and a static function is known by file, the file of its first
 * declaration. The search goes through several parses, and a cursor is of
 * one parse.
 */
struct cxi_unit {
	struct cxi_calls calls;    /* of the functions of the search, in the
	                              function of a visit */
	struct cxi_out   key;
	CXCursor         named;
	CXFileUniqueID   file;
	uint32_t         linkage;  /* enum CXLinkageKind */
	uint32_t         wanted;
};

static int
cxi_unit_call (struct cxi_calls *calls,
               CXCursor          c);

static int
cxi_unit_cleanup (struct cxi_calls *calls,
                  CXCursor          c);

/* Returns a search for each function of the name of the request of @p x,
 * before the search reaches a function.
 */
static force_inline struct cxi_unit
cxi_unit (struct cxi *x)
{
	return (struct cxi_unit){
		.calls   = cxi_calls(x, cxi_unit_call, cxi_unit_cleanup),
		.key     = cxi_out(x->out.tree),
		.named   = clang_getNullCursor(),
		.linkage = CXLinkage_Invalid,
	};
}

/* Frees the calls and the callee of @p unit. */
static void
cxi_unit_fini (struct cxi_unit *unit)
{
	cxi_out_fini(&unit->key);
	cxi_calls_fini(&unit->calls);
}

/* Limits the search of @p unit to the function of the name that the
 * translation unit of the path of the request declares, which is parsed.
 * The error is that of cxi_lookup().
 */
static int
cxi_unit_limit (struct cxi_unit *unit)
{
	struct cxi *x = unit->calls.x;
	CXCursor c;
	CXFile file;
	int const e = cxi_lookup(x, x->key[CXI_KEY_NAME].data, CXCursor_FunctionDecl, &c);
	if (e)
		return e;
	CXCursor const first = clang_getCanonicalCursor(c);
	unit->linkage = clang_getCursorLinkage(first);
	clang_getFileLocation(clang_getCursorLocation(first), &file, nullptr, nullptr, nullptr);
	if (file)
		(void)clang_getFileUniqueID(file, &unit->file);
	return 0;
}

/* Makes @p d, the first declaration of a function of the name, the function
 * of the name of @p unit. If the function is one of the search, sets the
 * callee of its rows, and appends the row of a callee without a call.
 *
 * The callee is a letter and the place of the function as FILE:FIRST-LAST.
 * The place is that of the definition in the translation unit, or of @p d
 * if there is none. The letter is S for a static function and E for any
 * other one, in lower case if the translation unit does not define the
 * function: the definition of an e is then in another translation unit, or
 * in none.
 */
static void
cxi_unit_found (struct cxi_unit *unit,
                CXCursor         d)
{
	struct cxi_out *out = &unit->calls.x->out;
	uint32_t const linkage = clang_getCursorLinkage(d);
	unit->named = d;
	unit->wanted = unit->linkage == CXLinkage_Invalid || linkage == unit->linkage;
	if (unit->wanted && unit->linkage == CXLinkage_Internal) {
		CXFile file;
		CXFileUniqueID id;
		clang_getFileLocation(clang_getCursorLocation(d), &file, nullptr, nullptr, nullptr);
		unit->wanted = file && !clang_getFileUniqueID(file, &id) &&
		               !memcmp(&id, &unit->file, sizeof id);
	}
	if (!unit->wanted)
		return;
	CXCursor const definition = clang_getCursorDefinition(d);
	bool const defined = !clang_Cursor_isNull(definition);
	CXCursor const at = defined ? definition : d;
	cxi_out_put(&unit->key, &"SsEe"[2 * (linkage != CXLinkage_Internal) + !defined], 1);
	(void)cxi_out_file(&unit->key, clang_getCursorLocation(at));
	cxi_out_lit(&unit->key, ":");
	cxi_out_lines(&unit->key, at);
	cxi_out_view(out, cstr_view_dstr(&unit->key.text));
	cxi_out_lit(out, "\t\t\n");
}

/* Returns true if @p d, the first declaration of a callee, is a function of
 * the search of @p unit.
 */
static bool
cxi_unit_is (struct cxi_unit *unit,
             CXCursor         d)
{
	if (d.kind != CXCursor_FunctionDecl)
		return false;
	/* While no declaration at file scope has declared a function of the
	 * name, a declaration in a block or an implicit one can. */
	if (clang_Cursor_isNull(unit->named) &&
	    cxi_spelled(d, unit->calls.x->key[CXI_KEY_NAME].data))
		cxi_unit_found(unit, d);
	return unit->wanted && clang_equalCursors(d, unit->named);
}

/* Appends the call expression @p c, the cursor of the visit, to @p calls,
 * the calls of a cxi_unit, if a callee is a function of the search and the
 * program can evaluate the call. The callee of most calls is a name, and
 * libclang returns the declaration of the name for the call. The state and
 * the macro of a call are looked for only if a callee is a function of the
 * search.
 */
static int
cxi_unit_call (struct cxi_calls *calls,
               CXCursor          c)
{
	struct cxi_unit *unit = container_of(calls, struct cxi_unit, calls);
	struct cxi_walk *walk = &calls->walk;
	struct cxi_call call = cxi_call(c, (uint32_t)walk->to.n, 0, 0);
	CXCursor const d = clang_getCursorReferenced(c);
	uint32_t i = 0;
	int e = 0;
	if (clang_Cursor_isNull(d)) {
		/* The callee is an expression, such as a _Generic selection. */
		e = cxi_walk_call(walk, c, &call);
		while (!e && i < call.n && !cxi_unit_is(unit, walk->to.items[call.first + i]))
			++i;
	} else if (d.kind == CXCursor_FunctionDecl &&
	           cxi_unit_is(unit, clang_getCanonicalCursor(d))) {
		e = util_push(walk->to, unit->named);
		call.n = 1;
	}
	if (!e && i < call.n)
		e = cxi_walk_state(walk, &call.state);
	if (e || i == call.n || call.state == CXI_UNEVALUATED) {
		walk->to.n = call.first;
		return e;
	}
	call.macro = cxi_macro(walk->tu, clang_getCursorLocation(c));
	return cxi_calls_push(calls, call);
}

/* Appends to @p calls, the calls of a cxi_unit, the call of the function
 * of the attribute cleanup of the variable @p c, the cursor of the visit,
 * if the function is one of the search and the program can evaluate the
 * declaration. The function of an attribute is declared at file scope
 * before the variable.
 */
static int
cxi_unit_cleanup (struct cxi_calls *calls,
                  CXCursor          c)
{
	char name[CXI_NAME + 1];
	struct cxi_unit *unit = container_of(calls, struct cxi_unit, calls);
	struct cxi_walk *walk = &calls->walk;
	struct cxi_call call = cxi_call(c, (uint32_t)walk->to.n,
	                                1, CXI_CALL_CLEANUP);
	if (!unit->wanted || !cstr_view_equal(cstr_view_n(name, cxi_cleanup_name(c, name)),
	                                      calls->x->key[CXI_KEY_NAME]))
		return 0;
	int e = cxi_walk_state(walk, &call.state);
	if (e || call.state == CXI_UNEVALUATED)
		return e;
	e = cxi_walk_cleanup_macro(walk, c, &call.macro);
	if (!e)
		e = util_push(walk->to, unit->named);
	return e ? e : cxi_calls_push(calls, call);
}

/* Appends the rows of the calls of @p unit, which are in the function
 * @p c. A row is the callee (cxi_unit_found()), a tab, the file, a tab, the
 * lines of the calls of a row of describe, the function @p c with its
 * lines, and in brackets a note for each of: the macro that the calls are
 * written through, the attribute cleanup, a callee that is one of several,
 * and a call that the program maybe does not evaluate.
 */
static void
cxi_unit_rows (struct cxi_unit const *unit,
               CXCursor               c)
{
	struct cxi_calls const *calls = &unit->calls;
	struct cxi_out *out = &calls->x->out;
	for (size_t i = 0; i < calls->n; ++i) {
		struct cxi_call const *call = &calls->items[i];
		if (call->row != i)
			continue;
		cxi_out_view(out, cstr_view_dstr(&unit->key.text));
		cxi_out_lit(out, "\t");
		cxi_calls_places(calls, i, '\t');
		cxi_out_lit(out, " ");
		cxi_out_clang(out, clang_getCursorSpelling(c));
		cxi_out_lit(out, " (");
		cxi_out_lines(out, c);
		cxi_out_lit(out, ")");
		if (!clang_Cursor_isNull(call->macro)) {
			cxi_out_lit(out, " [macro ");
			cxi_out_clang(out, clang_getCursorSpelling(call->macro));
			cxi_out_lit(out, "]");
		}
		if (call->flags & CXI_CALL_CLEANUP)
			cxi_out_lit(out, " [attribute cleanup]");
		if (call->n > 1)
			cxi_out_lit(out, " [or another function]");
		if (call->state == CXI_UNPROVEN)
			cxi_out_lit(out, " [maybe not evaluated]");
		cxi_out_lit(out, "\n");
	}
}

/* Looks at each function at file scope for the cxi_unit at @p data. The
 * first one of the name is the function of the name of the translation
 * unit. The calls in the code of a function that a file of the tree
 * defines are searched, and their rows appended.
 */
static enum CXChildVisitResult
cxi_unit_function (CXCursor     c,
                   CXCursor,
                   CXClientData data)
{
	struct cxi_unit *unit = data;
	struct cxi *x = unit->calls.x;
	if (c.kind != CXCursor_FunctionDecl)
		return CXChildVisit_Continue;
	if (clang_Cursor_isNull(unit->named) && cxi_spelled(c, x->key[CXI_KEY_NAME].data))
		cxi_unit_found(unit, clang_getCanonicalCursor(c));
	if (!clang_isCursorDefinition(c) || !cxi_in_tree(x->out.tree, c))
		return CXChildVisit_Continue;
	if (cxi_calls_collect(&unit->calls, c))
		return CXChildVisit_Break;
	cxi_unit_rows(unit, c);
	return x->out.e ? CXChildVisit_Break : CXChildVisit_Continue;
}

/* A row of callers (cxi_unit_rows()): the callee, the file, the text
 * after the second tab, and the first line of the text. The row of a
 * callee without a call is the callee and two tabs.
 */
struct cxi_row {
	struct cstr_view callee;
	struct cstr_view file;
	struct cstr_view text;
	uint64_t         line;
};

/* Orders the rows of callers by callee, file, line and text. */
static int
cxi_row_order (void const *a,
               void const *b)
{
	struct cxi_row const *p = a;
	struct cxi_row const *q = b;
	int const callee = cstr_view_compare(p->callee, q->callee);
	int const file = callee ? callee : cstr_view_compare(p->file, q->file);
	return file ? file
	     : p->line != q->line ? (p->line > q->line) - (p->line < q->line)
	     : cstr_view_compare(p->text, q->text);
}

/* Appends the rows @p text of callers for the functions @p name: for each
 * callee the name and the place, and under it the file and the text of each
 * row, in the order of the files and the lines. A row that several
 * translation units give, as for a function that a header defines, is
 * appended once. A callee e (cxi_unit_found()) is the one E of the
 * rows. If the rows contain no E or several, the callee is the
 * declaration, as is a static function without a definition: the place is
 * then appended after the word declared. Without a row, the function is
 * not found.
 */
static void
cxi_out_rows (struct cxi_out   *out,
              struct cstr_view  name,
              struct cstr_view  text)
{
	size_t const n = cstr_view_count(text, '\n');
	if (!n) {
		cxi_out_lit(out, "- callee: ");
		cxi_out_view(out, name);
		cxi_out_lit(out, " (not found)\n");
		return;
	}
	struct cxi_row *row = malloc(n * sizeof *row);
	if (!row) {
		out->e = ENOMEM;
		return;
	}
	/* defined is an E of the rows, and several is true if the rows contain
	 * more than one. */
	struct cstr_view defined = {};
	bool several = false;
	for (size_t i = 0; i < n; ++i) {
		struct cxi_row *r = &row[i];
		*r = (struct cxi_row){};
		cstr_view_cut(&text, '\n', &r->text);
		cstr_view_cut(&r->text, '\t', &r->callee);
		cstr_view_cut(&r->text, '\t', &r->file);
		(void)util_digits(r->text.data, r->text.len, UINT64_MAX, &r->line);
		if (!cstr_view_starts(r->callee, CSTR_VIEW("E")))
			continue;
		several = several || (defined.data && !cstr_view_equal(r->callee, defined));
		defined = r->callee;
	}
	for (size_t i = 0; defined.data && !several && i < n; ++i) {
		if (cstr_view_starts(row[i].callee, CSTR_VIEW("e")))
			row[i].callee = defined;
	}
	qsort(row, n, sizeof *row, cxi_row_order);
	bool bare = false;
	for (size_t i = 0; i < n; ++i) {
		struct cxi_row const *r = &row[i];
		bool const first = !i || !cstr_view_equal(r->callee, r[-1].callee);
		if (first) {
			cxi_out_lit(out, "- callee: ");
			cxi_out_view(out, name);
			cxi_out_view(out, r->callee.len && *r->callee.data >= 'a'
			             ? CSTR_VIEW(" (declared ") : CSTR_VIEW(" ("));
			cxi_out_view(out, cstr_view_slice(r->callee, 1, SIZE_MAX));
			cxi_out_lit(out, ")\n");
			bare = true;
		}
		if (r->text.len && (first || !cstr_view_equal(r->file, r[-1].file) ||
		                    !cstr_view_equal(r->text, r[-1].text))) {
			if (bare)
				cxi_out_lit(out, "  - callers\n");
			bare = false;
			cxi_out_lit(out, "    - ");
			cxi_out_view(out, r->file);
			cxi_out_lit(out, ":");
			/* The byte after the text of a row is the line feed of the
			 * row. */
			cxi_out_put(out, r->text.data, r->text.len + 1);
		}
		if (bare && (i + 1 == n || !cstr_view_equal(r->callee, r[1].callee)))
			cxi_out_lit(out, "  - callers: none\n");
	}
	free(row);
}

/* Appends the file of the clang command line @p words to @p list, after a
 * comma if @p list is not empty.
 */
static void
cxi_out_unit (struct cxi_out   *list,
              struct cstr_view  words)
{
	if (dstr_len(&list->text))
		cxi_out_lit(list, ", ");
	cxi_out_path(list, cxi_out_rel(list, cxi_last_word(words)));
}

/* Appends a line of @p before and the files of @p list (cxi_out_unit()) to
 * @p out if @p list is not empty. Moves the error of @p list to @p out and
 * frees @p list.
 */
static void
cxi_out_units (struct cxi_out   *out,
               struct cstr_view  before,
               struct cxi_out   *list)
{
	if (dstr_len(&list->text)) {
		cxi_out_view(out, before);
		cxi_out_put(out, dstr_get(&list->text), dstr_len(&list->text));
		cxi_out_lit(out, "\n");
	}
	out->e = out->e ? out->e : list->e;
	cxi_out_fini(list);
}

/* Returns true if clang reported an error for @p tu. */
static bool
cxi_tu_errors (CXTranslationUnit tu)
{
	bool errors = false;
	for (unsigned i = 0, n = clang_getNumDiagnostics(tu); !errors && i < n; ++i) {
		CXDiagnostic d = clang_getDiagnostic(tu, i);
		errors = cxi_severity(d) >= CXDiagnostic_Error;
		clang_disposeDiagnostic(d);
	}
	return errors;
}

/* Appends what read() returns from @p fd up to the end of the file to
 * @p out. Returns 0 or the error of read().
 */
static int
cxi_out_read (struct cxi_out *out,
              int             fd)
{
	char buf[4096];
	for (;;) {
		ssize_t const n = read(fd, buf, sizeof buf);
		if (n > 0)
			cxi_out_put(out, buf, (size_t)n);
		else if (!n || errno != EINTR)
			return n ? errno : 0;
	}
}

/* The exit status of the child process of a translation unit of callers:
 * the rows are written, they are written and clang reported an error, or
 * no row is written.
 */
enum {
	CXI_UNIT_OK,
	CXI_UNIT_ERRORS,
	CXI_UNIT_FAILED
};

/* Runs in the child process of a translation unit of callers, which ends
 * with its parent: writes the rows of the translation unit of the clang
 * command line @p words to @p fd and exits with a status of the enum above.
 * The rows are one without a call for the function of the search that the
 * translation unit declares (cxi_unit_found()), and the rows of the calls
 * (cxi_unit_rows()). The answer is empty when the child starts, and the
 * child collects the rows in it. The parse of the file of the path serves
 * as the translation unit with the same command line. The process exits
 * without freeing its memory.
 */
[[noreturn]] static void
cxi_unit_run (struct cxi_unit  *unit,
              struct cstr_view  words,
              int               fd)
{
	struct cxi *x = unit->calls.x;
	if (prctl(PR_SET_PDEATHSIG, SIGKILL) ||
	    (!cstr_view_equal(words, x->words) && cxi_tu(x, words, &x->tu)) ||
	    clang_visitChildren(clang_getTranslationUnitCursor(x->tu), cxi_unit_function, unit) ||
	    x->out.e || unit->key.e ||
	    util_write_all(fd, dstr_get(&x->out.text), dstr_len(&x->out.text)))
		_exit(CXI_UNIT_FAILED);
	_exit(cxi_tu_errors(x->tu) ? CXI_UNIT_ERRORS : CXI_UNIT_OK);
}

/* callers: the calls of the functions named name in each translation unit
 * of the compile commands (cdb_next()). With a path, only the calls of the
 * function that the translation unit of the path declares. Each translation
 * unit is parsed in a child process, so that a crash of clang loses only
 * the calls of one file. No child starts unless twice the time of the
 * longest one so far is left before the deadline. The answer is a list: the
 * repository, the commit, the callees with their callers (cxi_out_rows()),
 * and a line for each of: the files that clang could not parse, the files
 * with errors, and the translation units that the deadline left out.
 */
static int
cxi_callers (struct cxi *x)
{
	struct cxi_unit unit = cxi_unit(x);
	int e = x->key[CXI_KEY_PATH].data ? cxi_unit_limit(&unit) : 0;
	if (e) {
		cxi_unit_fini(&unit);
		return e;
	}
	struct cdb_map const map = cdb_map(x->key[CXI_KEY_TOP],
	                                   cstr_view_slice(x->out.tree, 0, x->out.tree.len - 1));
	struct cxi_out rows = cxi_out(x->out.tree);
	struct cxi_out failed = cxi_out(x->out.tree);
	struct cxi_out errors = cxi_out(x->out.tree);
	struct dstr words = DSTR_INIT;
	uint64_t longest = 0;
	size_t late = 0;
	for (size_t at = 0; !rows.e && !(e = cdb_next(&map, x->cmds, &at, &words));) {
		struct cstr_view const w = cstr_view_dstr(&words);
		uint64_t const start = loop_clock();
		if (late || start + 2 * longest + CXI_EARLY > x->deadline) {
			++late;
			continue;
		}
		int end[2] = { -1, -1 };
		pid_t const child = pipe2(end, O_CLOEXEC) ? -1 : fork();
		if (!child)
			cxi_unit_run(&unit, w, end[1]);
		e = child < 0 ? errno : 0;
		util_close(&end[1]);
		size_t const before = dstr_len(&rows.text);
		e = e ? e : cxi_out_read(&rows, end[0]);
		util_close(&end[0]);
		int status = 0;
		while (!e && waitpid(child, &status, 0) < 0)
			e = errno == EINTR ? 0 : errno;
		if (e)
			break;
		bool const ok = WIFEXITED(status) && WEXITSTATUS(status) < CXI_UNIT_FAILED;
		/* Without the rows of a child that did not write all of them. */
		if (!ok && !dstr_erase(&rows.text, before, SIZE_MAX))
			rows.e = EINVAL;
		if (!ok || WEXITSTATUS(status) == CXI_UNIT_ERRORS)
			cxi_out_unit(ok ? &errors : &failed, w);
		uint64_t const took = loop_clock() - start;
		longest = took > longest ? took : longest;
	}
	dstr_cleanup(&words);
	cxi_unit_fini(&unit);
	cxi_out_lit(&x->out, "- repo: ");
	cxi_out_view(&x->out, x->key[CXI_KEY_REPO]);
	cxi_out_lit(&x->out, "\n- commit: ");
	cxi_out_view(&x->out, x->key[CXI_KEY_COMMIT]);
	cxi_out_lit(&x->out, "\n");
	cxi_out_rows(&x->out, x->key[CXI_KEY_NAME], cstr_view_dstr(&rows.text));
	cxi_out_units(&x->out, CSTR_VIEW("- not parsed: "), &failed);
	cxi_out_units(&x->out, CSTR_VIEW("- parsed with errors: "), &errors);
	if (late)
		cxi_out_fmt(&x->out, "- not searched: %zu translation unit%s (time limit)\n", late,
		            util_s(late));
	x->out.e = x->out.e ? x->out.e : rows.e;
	cxi_out_fini(&rows);
	return x->out.e ? x->out.e : e == ENOENT ? 0 : e;
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
		bool const warning = k == CXI_KEY_WARNING && x->n_warnings < CXI_WARNINGS;
		bool const once = k < CXI_KEYS && (!x->key[k].data || warning) &&
		                  (k != CXI_KEY_UNCONFINED || cstr_view_equal(value, CSTR_VIEW("1")));
		size_t i = 0;
		while (i < value.len && (unsigned char)value.data[i] >= 0x20 && value.data[i] != 0x7f)
			++i;
		if (!once) {
			cxi_out_lit(&x->out, "askai-code does not accept the word ");
			cxi_out_path(&x->out, cstr_view(argv[*at]));
			cxi_out_lit(&x->out, ". Each key is allowed once, warning up to 16 times, "
			            "and unconfined only as unconfined=1.");
			return EINVAL;
		}
		if (i < value.len) {
			cxi_out_lit(&x->out, "The ");
			cxi_out_view(&x->out, k == CXI_KEY_WARNING ? CSTR_VIEW("warnings")
			                                           : cxi_keys[k]);
			cxi_out_lit(&x->out, " argument contains a control character.");
			return EINVAL;
		}
		x->key[k] = value;
		if (warning)
			x->warning[x->n_warnings++] = value.data;
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
	int e = util_mkdir_parents(path);
	if (!e && mkdir(path, 0700) && errno != EEXIST)
		e = errno;
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
		cxi_out_lit(&x->out, "cache ");
		cxi_out_path(&x->out, cache);
		cxi_out_lit(&x->out, ": ");
		cxi_out_view(&x->out, cstr_view(why ? why : strerror(e)));
		return EINVAL;
	}
	if (x->key[CXI_KEY_UNCONFINED].data) {
		cxi_out_clang(&x->out, clang_getClangVersion());
		cxi_out_lit(&x->out, "\nnone (--code-unconfined)\n");
		return 0;
	}
	int const abi = confine_abi();
	e = confine_cage(nullptr);
	if (e) {
		cxi_out_view(&x->out, !abi ? CSTR_VIEW("no Landlock") : e == ENOTSUP
		             ? CSTR_VIEW("no seccomp filter for this architecture")
		             : cstr_view(strerror(e)));
		cxi_out_lit(&x->out, "; --code-unconfined runs askai-code without Landlock and "
		            "seccomp");
		return EINVAL;
	}
	cxi_out_clang(&x->out, clang_getClangVersion());
	cxi_out_fmt(&x->out, "\nLandlock ABI %d, seccomp\n", abi);
	return 0;
}

/* Checks the action, the keys of the action and the path. Only callers
 * works without a path. A warning is -W or -Wno-, a name of a-z, 0-9 and
 * '-', and maybe =DIGITS.
 */
static int
cxi_valid (struct cxi *x)
{
	struct cstr_view const line = x->key[CXI_KEY_LINE];
	struct cstr_view const path = x->key[CXI_KEY_PATH];
	x->action = cstr_view_index(cxi_actions, CXI_ACTIONS, x->key[CXI_KEY_ACTION]);
	struct cstr_view const name = x->key[CXI_KEY_NAME];
	bool const describe = x->action == CXI_ACTION_DESCRIBE;
	bool const callers = x->action == CXI_ACTION_CALLERS;
	size_t i = 0;
	for (; i < x->n_warnings; ++i) {
		char const *w = x->warning[i];
		size_t const at = !strncmp(w, "-Wno-", 5) ? 5 : !strncmp(w, "-W", 2) ? 2 : 0;
		size_t const name = at ? strspn(w + at, "abcdefghijklmnopqrstuvwxyz0123456789-") : 0;
		char const *rest = w + at + name;
		size_t const digits = *rest == '=' ? strspn(rest + 1, "0123456789") : 0;
		if (!name || (*rest && (!digits || rest[1 + digits])))
			break;
	}
	struct cstr_view const why =
		x->action == CXI_ACTIONS
		? CSTR_VIEW("The action argument must be "
		            XL_JOIN(", ", " or ", CXI_ACTION_LIST(XL_TEXT)) ".")
		: describe && !name.data
		? CSTR_VIEW("Give describe a name argument, such as git_start or child.flags.")
		: callers && (!name.data || name.len > CXI_NAME || cxi_ident(name) != name.len)
		? CSTR_VIEW("Give callers a name argument, the name of a function, such as "
		            "git_start.")
		: !describe && !callers && name.data
		? CSTR_VIEW("The name argument is only for describe and callers.")
		: !callers && !path.data
		? CSTR_VIEW("Give a path argument, such as src/git.c. Only callers works without "
		            "path.")
		: !describe && line.data
		? CSTR_VIEW("The line argument is only for describe.")
		: line.data && (util_number(line.data, line.len, UINT32_MAX, &x->line) || !x->line)
		? CSTR_VIEW("The line argument must be a whole number from 1.")
		: x->n_warnings && x->action != CXI_ACTION_DIAGNOSTICS
		? CSTR_VIEW("The warnings argument is only for diagnostics.") : cstr_view(nullptr);
	if (why.data) {
		cxi_out_view(&x->out, why);
	} else if (i < x->n_warnings) {
		cxi_out_lit(&x->out, "The warnings entry ");
		cxi_out_path(&x->out, cstr_view(x->warning[i]));
		cxi_out_lit(&x->out, " must be -W or -Wno- and the name of a warning, such as "
		            "-Wpadded.");
	} else if (!path.data) {
		return 0;
	} else if (memchr(path.data, ':', path.len)) {
		cxi_out_lit(&x->out, "The path argument must not contain a colon. Give the "
		            "commit as the revision argument.");
	} else if (!util_path_inside(path)) {
		cxi_out_lit(&x->out, "The path argument must be relative to the top of the "
		            "repository, with no .. component.");
	} else if (!cstr_view_ends(path, CSTR_VIEW(".c")) && !cstr_view_ends(path, CSTR_VIEW(".h"))) {
		cxi_out_lit(&x->out, "The path argument must be a .c or .h file, not ");
		cxi_out_path(&x->out, path);
		cxi_out_lit(&x->out, ".");
	} else {
		return 0;
	}
	return EINVAL;
}

/* Reads the compile commands into x->cmds, writes the clang command line of
 * the file of the path to @p words (cdb_command()), and points x->words at
 * the words and x->file at the last word, the file in @p tree. Without a
 * path, x->words and x->file stay null, and the compile commands must
 * contain a translation unit (cdb_next()). A relative path of the compile
 * commands is relative to the repository's directory.
 */
static int
cxi_command (struct cxi       *x,
             struct cstr_view  tree,
             struct dstr      *words)
{
	struct cstr_view const repo = x->key[CXI_KEY_REPO];
	struct cstr_view const file = x->key[CXI_KEY_CDB];
	int dir = open(x->key[CXI_KEY_TOP].data, O_PATH | O_DIRECTORY | O_CLOEXEC);
	int fd = openat(dir, file.data, O_RDONLY | O_CLOEXEC);
	util_close(&dir);
	struct stat st;
	int e = fd < 0 || fstat(fd, &st) ? errno
	      : !S_ISREG(st.st_mode) || st.st_size > CXI_CDB ? EFBIG : 0;
	x->cmds = e ? nullptr : json_object_from_fd_ex(fd, CXI_DEPTH);
	util_close(&fd);
	if (!x->cmds) {
		cxi_out_lit(&x->out, "The compile commands of ");
		cxi_out_view(&x->out, repo);
		cxi_out_lit(&x->out, " in ");
		cxi_out_path(&x->out, file);
		cxi_out_lit(&x->out, " cannot be read (");
		cxi_out_view(&x->out, e == EFBIG ? CSTR_VIEW("not a regular file of at most 64 MiB")
		                    : e ? cstr_view(strerror(e)) : CSTR_VIEW("not JSON"));
		cxi_out_lit(&x->out, ").");
		return EINVAL;
	}
	struct cdb_map const map = cdb_map(x->key[CXI_KEY_TOP],
	                                   cstr_view_slice(tree, 0, tree.len - 1));
	struct cstr_view const path = x->key[CXI_KEY_PATH];
	size_t at = 0;
	if (path.data) {
		e = cdb_command(&map, x->cmds, path, words);
		x->words = cstr_view_dstr(words);
		x->file = cxi_last_word(x->words);
	} else if ((e = cdb_next(&map, x->cmds, &at, words)) == ENOENT) {
		/* No translation unit is the EINVAL of cdb_command(). */
		e = EINVAL;
	}
	if (e == ENOENT) {
		cxi_out_path(&x->out, path);
		cxi_out_lit(&x->out, " is not in the compile commands of ");
		cxi_out_view(&x->out, repo);
		cxi_out_lit(&x->out, ".");
	} else if (e == EINVAL) {
		cxi_out_lit(&x->out, "The compile commands of ");
		cxi_out_view(&x->out, repo);
		cxi_out_lit(&x->out, " in ");
		cxi_out_path(&x->out, file);
		cxi_out_lit(&x->out, " contain no entry with arguments for a file of the "
		            "repository.");
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
	                                 subcommand, up to a nullptr */
	uint64_t           deadline;  /* of the git processes, in loop_clock()
	                                 time */
	struct child_fds   fds;       /* of the git processes, with the file
	                                 lock open as fds.keep */
	int                fd;
	int                state;
};

/* Returns the cache of a call before cxi_cache_open() opens the directory
 * and the files. The two paths are empty, and every descriptor is -1.
 * @p git is the git command line up to the subcommand and a nullptr.
 * @p deadline is the deadline of the git processes, a loop_clock() time.
 */
static force_inline struct cxi_cache
cxi_cache (char const *const *git,
           uint64_t           deadline)
{
	return (struct cxi_cache){
		.dir      = DSTR_INIT,
		.tree     = DSTR_INIT,
		.git      = git,
		.deadline = deadline,
		.fds      = child_fds(),
		.fd       = -1,
		.state    = -1,
	};
}

/* Closes the directory and the files of @p cache, and frees the two paths. */
static void
cxi_cache_fini (struct cxi_cache *cache)
{
	util_close(&cache->state);
	util_close(&cache->fd);
	util_close(&cache->fds.keep);
	cache->git = nullptr;
	dstr_cleanup(&cache->tree);
	dstr_cleanup(&cache->dir);
}

/* Runs git with @p words and the lock kept open, as child_cmd_run() runs
 * a command. The output replaces the content of @p out, up to @p max
 * bytes, or goes to /dev/null if @p out is nullptr. Returns EIO for an
 * exit status other than 0.
 */
static int
cxi_cache_git (struct cxi_cache const *cache,
               char const *const      *words,
               struct buf             *out,
               size_t                  max)
{
	char const *const *const parts[] = { cache->git, words };
	char const *argv[256];
	int const e = child_argv(argv, array_size(argv), parts, array_size(parts));
	extern char **environ;
	struct child_cmd const cmd = child_cmd(
		.file     = "git",
		.argv     = argv,
		.envp     = environ,
		.max      = max,
		.deadline = cache->deadline
	);
	struct child_end end = child_end();
	return e ? e : child_cmd_run(&cmd, cache->fds, out, &end);
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
	int e = 0;
	if (!oid_valid(was)) {
		e = nftw(dstr_get(&cache->tree), cxi_cache_remove, 16, FTW_DEPTH | FTW_PHYS) &&
		    errno != ENOENT ? errno : 0;
		for (char const *const *f = (char const *const[]){ "index", "index.lock", nullptr };
		     !e && *f; ++f)
			e = unlinkat(cache->fd, *f, 0) && errno != ENOENT ? errno : 0;
	}
	if (!e && mkdirat(cache->fd, "tree", 0700) && errno != EEXIST)
		e = errno;
	struct buf list = buf();
	if (!e)
		e = cxi_cache_git(cache, count, &list, CXI_LIST);
	struct cstr_view rest = cstr_view_span(buf_peek(&list));
	size_t files = 0;
	uint64_t bytes = 0;
	/* The size of a submodule is "-", which util_digits() reads as 0. */
	for (struct cstr_view field = {}; cstr_view_cut(&rest, '\0', &field); ++files) {
		uint64_t size;
		(void)util_digits(field.data, field.len, UINT64_MAX, &size);
		bytes += size;
	}
	buf_fini(&list);
	static_assert(CXI_FILES == 100000 && CXI_BYTES == 1 << 30, "update the text of the limits");
	if (e == EFBIG || (!e && (files > CXI_FILES || bytes > CXI_BYTES))) {
		cxi_out_lit(out, "Commit ");
		cxi_out_view(out, commit);
		cxi_out_lit(out, " is too large for the code tool. The limits are 100000 files and "
		            "directories and 1 GiB.");
		return EINVAL;
	}
	if (!e)
		e = cxi_cache_set_state(cache, CSTR_VIEW("-"));
	if (!e)
		e = cxi_cache_git(cache, export, nullptr, 0);
	if (e == EIO || e == ETIMEDOUT) {
		cxi_out_lit(out, "git cannot export commit ");
		cxi_out_view(out, commit);
		cxi_out_view(out, e == EIO ? CSTR_VIEW(".") : CSTR_VIEW(" in time."));
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
		size_t n;
		if (flock(cache->fds.keep, op))
			return errno;
		if ((e = util_read_all(cache->state, now, sizeof now, 0, &n)))
			return e;
		struct cstr_view const was = cstr_view_n(now, n);
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
	if (!e && ((cache->fds.keep = openat(cache->fd, "lock", flags, 0600)) < 0 ||
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
	cxi_out_lit(out, "The cache directory ");
	cxi_out_path(out, cstr_view_dstr(&cache->dir));
	cxi_out_lit(out, " cannot be used (");
	cxi_out_view(out, cstr_view(why ? why : strerror(e)));
	cxi_out_lit(out, ").");
	return EINVAL;
}

/* Parses the file of the path, if the request contains one, with x->words
 * (cxi_tu()) and appends the answer of the action. The function first
 * copies fd 1 to @p *fd for the answer and points fd 1 at /dev/null, so
 * that clang cannot write into the answer. Then it changes to @p tree,
 * clears the environment and confines the process.
 */
static int
cxi_parse (struct cxi *x,
           char const *tree,
           int        *fd)
{
	static int (*const action[CXI_ACTIONS])(struct cxi *) = {
		[CXI_ACTION_DESCRIBE] = cxi_describe,
		[CXI_ACTION_OUTLINE] = cxi_outline,
		[CXI_ACTION_DIAGNOSTICS] = cxi_diagnostics,
		[CXI_ACTION_CALLERS] = cxi_callers,
	};
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
		cxi_out_lit(&x->out, "askai-code cannot confine the process (");
		cxi_out_view(&x->out, cstr_view(strerror(e)));
		cxi_out_lit(&x->out, ").");
		return EINVAL;
	}
	x->index = clang_createIndex(0, 0);
	enum CXErrorCode const failed = x->words.data ? cxi_tu(x, x->words, &x->tu)
	                                              : CXError_Success;
	if (failed) {
		cxi_out_view(&x->out, failed == CXError_Crashed
		             ? CSTR_VIEW("clang ran out of memory or crashed in the parse of ")
		             : CSTR_VIEW("clang could not parse "));
		cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
		cxi_out_lit(&x->out, ".");
		e = EINVAL;
	} else {
		e = action[x->action](x);
	}
	clang_disposeTranslationUnit(x->tu);
	clang_disposeIndex(x->index);
	return e;
}

/* Runs the request with @p git, the git command line up to the
 * subcommand and a nullptr.
 */
static int
cxi_run (struct cxi         *x,
         char const *const  *git,
         int                *fd)
{
	struct cstr_view const *key = x->key;
	struct cstr_view const repo = key[CXI_KEY_REPO];
	struct cstr_view const commit = key[CXI_KEY_COMMIT];
	/* The name of the repository is a directory of the cache. */
	if (!key[CXI_KEY_ACTION].data || !git_run_name_valid(repo) || !key[CXI_KEY_CDB].data ||
	    !oid_valid(commit) ||
	    !cstr_view_starts(key[CXI_KEY_TOP], CSTR_VIEW("/")) ||
	    !cstr_view_starts(key[CXI_KEY_CACHE], CSTR_VIEW("/")) ||
	    util_number(key[CXI_KEY_DEADLINE].data, key[CXI_KEY_DEADLINE].len, UINT64_MAX,
	                &x->deadline)) {
		cxi_out_lit(&x->out, "askai-code requires the words action, repo, cdb, commit, "
		            "deadline, top and cache. repo is the name of a --git "
		            "repository. commit is a commit ID. deadline is in nanoseconds. "
		            "top and cache are absolute paths.");
		return EINVAL;
	}
	int e = cxi_valid(x);
	if (e)
		return e;
	struct cxi_cache cache = cxi_cache(git, x->deadline > CXI_EARLY
	                                      ? x->deadline - CXI_EARLY
	                                      : 1);
	struct dstr words = DSTR_INIT;
	/* clang's names are compared with the tree in normal form. */
	if (cdb_norm(&cache.dir, cstr_view(nullptr), key[CXI_KEY_CACHE]) ||
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
	if (!e && x->file &&
	    (fstatat(cache.fd, x->file, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode))) {
		cxi_out_path(&x->out, key[CXI_KEY_PATH]);
		cxi_out_lit(&x->out, " is not a file of commit ");
		cxi_out_view(&x->out, commit);
		cxi_out_lit(&x->out, ".");
		e = EINVAL;
	}
	if (!e)
		e = cxi_parse(x, dstr_get(&cache.tree), fd);
	x->out.tree = cstr_view(nullptr);
	json_object_put(x->cmds);
	x->cmds = nullptr;
	x->words = cstr_view(nullptr);
	dstr_cleanup(&words);
	cxi_cache_fini(&cache);
	return e;
}

#undef CXI_EARLY

int
main (int    argc,
      char **argv)
{
	struct cxi x = cxi();
	bool const check = argc > 1 && !strcmp(argv[1], "--check");
	int at = 1 + check;
	int fd = 1;
	int e = child_std_open();
	if (!e)
		e = cxi_words(&x, argc, argv, &at);
	if (!e)
		e = check ? cxi_check(&x)
		          : cxi_run(&x, (char const *const *)argv + at, &fd);
	if (x.out.e || e == ENOMEM || (e && !dstr_len(&x.out.text))) {
		int const why = x.out.e ? x.out.e : e;
		dstr_clear(&x.out.text);
		x.out.e = 0;
		cxi_out_lit(&x.out, "askai-code failed (");
		cxi_out_view(&x.out, cstr_view(strerror(why)));
		cxi_out_lit(&x.out, ").");
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
	struct cstr_view const record = e ? cstr_view(nullptr) : cstr_view_dstr(&x.record);
	if (util_write_all(fd, record.data, record.len) || util_write_all(fd, text, len) ||
	    (over && util_write_all(fd, cut, sizeof cut - 1)))
		e = EIO;
	cxi_fini(&x);
	return e ? 1 : 0;
}
