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
#include <inttypes.h>
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
	CXI_LINES = 40,       /* the most lines of a declaration in describe */
	CXI_ROWS  = 2000,     /* the most member rows of a layout */
	CXI_BITS  = 48,       /* the size of the text of an offset or a size */
	CXI_NAME  = 256,      /* the longest part of the name of describe */
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
 * parse of the file. For describe, the record line "lines FIRST LAST TOTAL
 * AT LEN PATH" comes before the answer: it names the numbered lines of a
 * file of the repository in the answer, at the offset AT for LEN bytes.
 */
struct cxi {
	struct cxi_out     out;
	struct dstr        record;
	struct cstr_view   key[CXI_KEYS];  /* each value, or data nullptr */
	char const        *file;           /* the path of the file in the tree */
	char const        *warning[CXI_WARNINGS];
	CXTranslationUnit  tu;
	uint64_t           n_warnings;
	uint64_t           action;         /* enum cxi_action */
	uint64_t           line;           /* the value of line, or 0 */
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

/* Returns @p s without the path of the tree, normalized into @p norm if
 * absolute, because clang names a file as #include spells it.
 */
static struct cstr_view
cxi_out_rel (struct cxi_out *out,
             char const     *s,
             struct dstr    *norm)
{
	if (*s == '/' && cdb_norm(norm, (struct cstr_view){}, cstr_view(s)))
		out->e = ENOMEM;
	struct cstr_view const p = dstr_len(norm) ? cstr_view_dstr(norm) : cstr_view(s);
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

static void
cxi_out_place (struct cxi_out   *out,
               char const       *before,
               CXSourceLocation  loc,
               char const       *after)
{
	CXFile file;
	CXString name;
	unsigned line;
	clang_getExpansionLocation(loc, &file, &line, nullptr, nullptr);
	/* A macro of clang or of -D is in <built-in> or <command line>. */
	if (file)
		name = clang_getFileName(file);
	else
		clang_getPresumedLocation(loc, &name, &line, nullptr);
	char const *path = clang_getCString(name);
	struct dstr norm = DSTR_INIT;
	cxi_out_printf(out, "%s", before);
	cxi_out_path(out, cxi_out_rel(out, path ? path : "", &norm));
	cxi_out_printf(out, ":%u%s", line, after);
	dstr_cleanup(&norm);
	clang_disposeString(name);
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
		[CXCursor_FieldDecl] = "field", [CXCursor_EnumConstantDecl] = "enumerator",
		[CXCursor_ParmDecl] = "parameter",
	};
	enum CXCursorKind const k = clang_getCursorKind(c);
	return k == CXCursor_MacroDefinition ? "macro"
	     : (size_t)k < array_size(words) ? words[k] : nullptr;
}

/* Returns "static " for a declaration of static storage, else "". Only a
 * declaration may go to clang_Cursor_getStorageClass(), which reads any
 * cursor as one.
 */
static char const *
cxi_static (CXCursor c)
{
	return clang_isDeclaration(clang_getCursorKind(c)) &&
	       clang_Cursor_getStorageClass(c) == CX_SC_Static ? "static " : "";
}

/* Returns true for a struct, union or enum without a name, also one named
 * by a typedef, which clang spells with the typedef's name. The USR of such
 * a declaration has a kind that ends in A, such as @SA@.
 */
static bool
cxi_unnamed (CXCursor c)
{
	CXString usr = clang_getCursorUSR(c);
	char const *u = clang_getCString(usr);
	char const *at = u ? strrchr(u, '@') : nullptr;
	bool const unnamed = at && at - u >= 3 && at[-1] == 'A' && at[-3] == '@';
	clang_disposeString(usr);
	return unnamed || clang_Cursor_isAnonymous(c);
}

static void
cxi_out_name (struct cxi_out *out,
              CXCursor        c)
{
	if (cxi_tag(clang_getCursorKind(c)) && cxi_unnamed(c))
		cxi_out_printf(out, "(anonymous)");
	else
		cxi_out_clang(out, clang_getCursorSpelling(c));
}

/* Returns the definition of @p c in the translation unit, or @p c. */
static CXCursor
cxi_shown (CXCursor c)
{
	CXCursor const d = clang_getCursorKind(c) == CXCursor_MacroDefinition
	                   ? c : clang_getCursorDefinition(c);
	return clang_Cursor_isNull(d) ? c : d;
}

static void
cxi_head (struct cxi *x)
{
	cxi_out_printf(&x->out, "[%s at %s: ", x->key[CXI_KEY_REPO].data,
	               x->key[CXI_KEY_COMMIT].data);
}

/* Cursors that a visitor collects; a failed push sets out->e. */
struct cxi_list {
	UTIL_ARRAY(CXCursor);
	struct cxi_out *out;
};

static bool
cxi_list_push (struct cxi_list *list,
               CXCursor         c)
{
	if (util_grow(list->items, list->cap, list->n + 1)) {
		list->out->e = ENOMEM;
		return false;
	}
	list->items[list->n++] = c;
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
		qsort(list.items, list.n, sizeof *list.items, cxi_order);
	cxi_head(x);
	cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
	cxi_out_printf(&x->out, ", %zu name%s.]\n", list.n, util_s(list.n));
	for (size_t i = 0; i < list.n; ++i) {
		CXCursor const c = list.items[i];
		enum CXCursorKind const k = clang_getCursorKind(c);
		CXSourceRange const extent = clang_getCursorExtent(c);
		unsigned const first = cxi_line(clang_getRangeStart(extent));
		unsigned const last = cxi_line(clang_getRangeEnd(extent));
		cxi_out_printf(&x->out, "%u %s%s ", cxi_line(clang_getCursorLocation(c)),
		               cxi_static(c), cxi_kind(c));
		cxi_out_name(&x->out, c);
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
	free(list.items);
	return (int)x->out.e;
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
	*line = (struct cstr_view){};
	if (!text)
		return 0;
	size_t total = 0;
	for (char const *p = text, *end = text + size; p < end; ++total) {
		char const *nl = memchr(p, '\n', (size_t)(end - p));
		struct cstr_view const here = { p, nl ? (size_t)(nl - p) : (size_t)(end - p) };
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
	for (size_t i = 0, k; i < s.len; i += k ? k : 1) {
		k = s.data[i] == '\t' ? 1 : cxi_char(s.data + i, s.len - i);
		if (k)
			cxi_out_put(out, s.data + i, k);
		else
			cxi_out_printf(out, "\\x%02x", (unsigned char)s.data[i]);
	}
}

/* Stores the body of a function in the CXCursor at @p data. */
static enum CXChildVisitResult
cxi_body (CXCursor     c,
          CXCursor,
          CXClientData data)
{
	if (clang_getCursorKind(c) != CXCursor_CompoundStmt)
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
	clang_getExpansionLocation(clang_getRangeStart(extent), &file, &first, nullptr, nullptr);
	clang_getExpansionLocation(clang_getRangeStart(comment), &in, &from, nullptr, nullptr);
	unsigned last = cxi_line(clang_getRangeEnd(extent));
	unsigned const to = cxi_line(clang_getRangeEnd(comment));
	if (!clang_Range_isNull(comment) && clang_File_isEqual(in, file) && to + 1 >= first &&
	    from <= last) {
		first = from < first ? from : first;
		last = to > last ? to : last;
	}
	CXCursor body = clang_getNullCursor();
	if (clang_getCursorKind(c) == CXCursor_FunctionDecl)
		clang_visitChildren(c, cxi_body, &body);
	unsigned line;
	unsigned column;
	clang_getExpansionLocation(clang_getCursorLocation(body), nullptr, &line, &column, nullptr);
	last = clang_Cursor_isNull(body) ? last : column == 1 && line > 1 ? line - 1 : line;
	size_t size = 0;
	char const *text = file ? clang_getFileContents(x->tu, file, &size) : nullptr;
	struct cstr_view here;
	size_t const total = cxi_find_line(text, size, first, &here);
	last = last < first ? first : last - first < CXI_LINES ? last : first + CXI_LINES - 1;
	last = last < total ? last : (unsigned)total;
	if (!here.data)
		return;
	char const *end = text + size;
	CXString name = clang_getFileName(file);
	char const *path = clang_getCString(name);
	struct dstr norm = DSTR_INIT;
	struct cstr_view const rel = cxi_out_rel(&x->out, path ? path : "", &norm);
	cxi_out_path(&x->out, rel);
	if (first == last)
		cxi_out_printf(&x->out, " line %u of %zu:\n", first, total);
	else
		cxi_out_printf(&x->out, " lines %u to %u of %zu:\n", first, last, total);
	size_t const at = dstr_len(&x->out.text);
	for (unsigned n = first; n <= last; ++n) {
		cxi_out_printf(&x->out, "%u\t", n);
		cxi_out_line(&x->out, here);
		cxi_out_printf(&x->out, "\n");
		char const *next = here.data + here.len + (here.data + here.len < end);
		char const *nl = memchr(next, '\n', (size_t)(end - next));
		here = (struct cstr_view){ next, nl ? (size_t)(nl - next) : (size_t)(end - next) };
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
	dstr_cleanup(&norm);
	clang_disposeString(name);
}

static enum CXChildVisitResult
cxi_call (CXCursor     c,
          CXCursor,
          CXClientData data)
{
	enum CXCursorKind const k = clang_getCursorKind(clang_getCursorReferenced(c));
	bool const call = clang_getCursorKind(c) == CXCursor_CallExpr && k == CXCursor_FunctionDecl;
	return !call || cxi_list_push(data, c) ? CXChildVisit_Recurse : CXChildVisit_Break;
}

static CXCursor
cxi_callee (CXCursor c)
{
	return clang_getCanonicalCursor(clang_getCursorReferenced(c));
}

/* Appends the row of the function @p f to @p out. @p f is the callee of
 * calls->items[i]. The row contains the place of that call, the lines of
 * the later calls to @p f, the name of @p f and the place of its
 * declaration.
 */
static void
cxi_out_call (struct cxi_out        *out,
              struct cxi_list const *calls,
              size_t                 i,
              CXCursor               f)
{
	CXSourceLocation const at = clang_getCursorLocation(calls->items[i]);
	unsigned line = cxi_line(at);
	cxi_out_place(out, "  ", at, "");
	for (unsigned l; ++i < calls->n; ) {
		l = cxi_line(clang_getCursorLocation(calls->items[i]));
		if (!clang_equalCursors(cxi_callee(calls->items[i]), f) || l == line)
			continue;
		cxi_out_printf(out, ",%u", l);
		line = l;
	}
	cxi_out_printf(out, " ");
	cxi_out_clang(out, clang_getCursorSpelling(f));
	cxi_out_place(out, " (", clang_getCursorLocation(f), ")\n");
}

static void
cxi_out_calls (struct cxi_out *out,
               CXCursor        c)
{
	struct cxi_list calls = { .out = out };
	clang_visitChildren(c, cxi_call, &calls);
	if (calls.n)
		cxi_out_printf(out, "calls:\n");
	for (size_t i = 0, j = 0; i < calls.n; ++i, j = 0) {
		CXCursor const f = cxi_callee(calls.items[i]);
		while (!clang_equalCursors(cxi_callee(calls.items[j]), f))
			++j;
		if (j == i)
			cxi_out_call(out, &calls, i, f);
	}
	free(calls.items);
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

/* Writes the offset @p bits to @p text as BYTES or BYTES+BITSb. */
static char const *
cxi_offset (char     text[static CXI_BITS],
            uint64_t bits)
{
	if (bits % 8)
		util_format(text, CXI_BITS, "%" PRIu64 "+%" PRIu64 "b", bits / 8, bits % 8);
	else
		util_format(text, CXI_BITS, "%" PRIu64, bits / 8);
	return text;
}

/* Writes the size @p bits to @p text as BYTES, or BITSb. */
static char const *
cxi_size (char     text[static CXI_BITS],
          uint64_t bits)
{
	util_format(text, CXI_BITS, bits % 8 ? "%" PRIu64 "b" : "%" PRIu64,
	            bits % 8 ? bits : bits / 8);
	return text;
}

static void
cxi_layout_padding (struct cxi_layout *l,
                    uint64_t           from,
                    uint64_t           to,
                    char const        *what)
{
	char at[CXI_BITS];
	char size[CXI_BITS];
	if (to <= from)
		return;
	cxi_out_printf(l->out, "%7s%7s%16s%*s(%s)\n", cxi_offset(at, from),
	               cxi_size(size, to - from), "", (int)(2 * l->depth), "", what);
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
		cxi_layout_padding(l, l->end, at, "padding");
	CXCursor const decl = clang_getTypeDeclaration(clang_getCanonicalType(type));
	enum CXCursorKind const k = clang_getCursorKind(decl);
	bool const nested = (k == CXCursor_StructDecl || k == CXCursor_UnionDecl) &&
	                    clang_Cursor_isAnonymous(decl);
	char bits[2][CXI_BITS];
	cxi_out_printf(l->out, "%7s%7s%7lld%7u  %*s", cxi_offset(bits[0], at),
	               cxi_size(bits[1], size), align > 0 ? align : 0,
	               cxi_line(clang_getCursorLocation(c)), (int)(2 * l->depth), "");
	if (clang_Cursor_isAnonymousRecordDecl(decl)) {
		cxi_out_printf(l->out, "(anonymous %s)\n", cxi_kind(decl));
	} else {
		cxi_out_clang(l->out, clang_getCursorSpelling(c));
		cxi_out_printf(l->out, ": ");
		if (nested)
			cxi_out_printf(l->out, "%s (unnamed)", cxi_kind(decl));
		else
			cxi_out_clang(l->out, clang_getTypeSpelling(type));
		if (bitfield)
			cxi_out_printf(l->out, " : %lld", n);
		cxi_out_printf(l->out, "\n");
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
	in.is_union = clang_getCursorKind(clang_getTypeDeclaration(type)) == CXCursor_UnionDecl;
	clang_Type_visitFields(type, cxi_layout_field, &in);
	long long const size = clang_Type_getSizeOf(type);
	if (in.rows && size > 0)
		cxi_layout_padding(&in, in.end, base + 8 * (uint64_t)size, depth
		                   ? "padding at the end of this member" : "padding at the end");
	l->padding = in.padding;
	l->rows = in.rows;
	l->hidden = in.hidden;
}

/* Appends the size, alignment, padding and target of the record @p type,
 * and its rows, at most CXI_ROWS, to @p rows.
 */
static void
cxi_layout (struct cxi     *x,
            struct cxi_out *rows,
            CXType          type)
{
	struct cxi_layout l = { .out = rows, .rows = CXI_ROWS };
	cxi_out_printf(rows, " offset   size  align   line  member: type\n");
	cxi_layout_record(&l, type, 0, 0);
	if (l.hidden)
		cxi_out_printf(rows, "[%" PRIu64 " more members; describe struct.member.]\n",
		               l.hidden);
	cxi_out_printf(&x->out, ": %lld bytes, alignment %lld, %s%" PRIu64 " byte%s",
	               clang_Type_getSizeOf(type), clang_Type_getAlignOf(type),
	               l.rows ? "" : "at least ", l.padding / 8, util_s(l.padding / 8));
	if (l.padding % 8)
		cxi_out_printf(&x->out, " and %" PRIu64 " bit%s", l.padding % 8,
		               util_s(l.padding % 8));
	CXTargetInfo info = clang_getTranslationUnitTargetInfo(x->tu);
	cxi_out_printf(&x->out, " of padding, ");
	cxi_out_clang(&x->out, clang_TargetInfo_getTriple(info));
	clang_TargetInfo_dispose(info);
	cxi_out_printf(&x->out, "\n");
}

/* A search for a name, of the kind kind if not 0, or with part for the
 * names of the tree that contain it.
 */
struct cxi_find {
	struct cxi_list  list;
	struct cstr_view name;
	uint32_t         kind;
	uint32_t         part;
};

static bool
cxi_in_tree (struct cxi_out const *out,
             CXCursor              c)
{
	CXFile file;
	clang_getExpansionLocation(clang_getCursorLocation(c), &file, nullptr, nullptr, nullptr);
	CXString name = clang_getFileName(file);
	char const *path = clang_getCString(name);
	bool const in_tree = path && cstr_view_starts(cstr_view(path), out->tree);
	clang_disposeString(name);
	return in_tree;
}

/* Collects each declaration or macro once that matches the cxi_find at
 * @p data.
 */
static enum CXChildVisitResult
cxi_match (CXCursor     c,
           CXCursor,
           CXClientData data)
{
	struct cxi_find *find = data;
	enum CXCursorKind const k = clang_getCursorKind(c);
	bool const tag = cxi_tag(k);
	CXString spelling = clang_getCursorSpelling(c);
	char const *s = clang_getCString(spelling);
	bool const named = s && (find->part ? find->name.len && strstr(s, find->name.data)
	                                    : !strcmp(s, find->name.data));
	clang_disposeString(spelling);
	bool const match = named && cxi_kind(c) && k != CXCursor_FieldDecl &&
	                   k != CXCursor_ParmDecl && (!find->kind || k == find->kind) &&
	                   !(tag && cxi_unnamed(c)) &&
	                   (!find->part || cxi_in_tree(find->list.out, c)) &&
	                   !(k == CXCursor_MacroDefinition && clang_Cursor_isMacroBuiltin(c));
	CXCursor const canonical = clang_getCanonicalCursor(c);
	size_t i = 0;
	while (match && i < find->list.n &&
	       !clang_equalCursors(clang_getCanonicalCursor(find->list.items[i]), canonical))
		++i;
	if (match && i == find->list.n && !cxi_list_push(&find->list, c))
		return CXChildVisit_Break;
	return tag ? CXChildVisit_Recurse : CXChildVisit_Continue;
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
	struct cxi_find find = { .list.out = &x->out, .name = cstr_view(name), .kind = kind };
	clang_visitChildren(clang_getTranslationUnitCursor(x->tu), cxi_match, &find);
	size_t const n = find.list.n;
	if (n == 1) {
		*c = cxi_shown(find.list.items[0]);
		free(find.list.items);
		return (int)x->out.e;
	}
	if (n) {
		size_t tag = 0;
		while (tag < n && !cxi_tag(clang_getCursorKind(find.list.items[tag])))
			++tag;
		cxi_out_printf(&x->out, "%zu symbols in the translation unit of ", n);
		cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
		cxi_out_printf(&x->out, " are named ");
		cxi_out_text(&x->out, x->key[CXI_KEY_NAME].data);
		if (tag < n)
			cxi_out_printf(&x->out, "; give %s %s for the type, or line:\n",
			               cxi_kind(find.list.items[tag]), name);
		else
			cxi_out_printf(&x->out, "; give line:\n");
	} else {
		find.part = 1;
		find.kind = 0;
		clang_visitChildren(clang_getTranslationUnitCursor(x->tu), cxi_match, &find);
		cxi_out_printf(&x->out, "No symbol ");
		cxi_out_text(&x->out, x->key[CXI_KEY_NAME].data);
		cxi_out_printf(&x->out, " in the translation unit of ");
		cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
		cxi_out_printf(&x->out, "%s", find.list.n
		               ? ". These names of the repository contain it:\n"
		               : ", and no name of the repository contains it.");
	}
	for (size_t i = 0; i < find.list.n; ++i) {
		CXCursor const at = cxi_shown(find.list.items[i]);
		cxi_out_place(&x->out, "  ", clang_getCursorLocation(at), " ");
		cxi_out_printf(&x->out, "%s%s ", cxi_static(at), cxi_kind(at));
		cxi_out_name(&x->out, at);
		cxi_out_printf(&x->out, "\n");
	}
	free(find.list.items);
	return x->out.e ? (int)x->out.e : EINVAL;
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
cxi_use (CXCursor     c,
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
	enum CXCursorKind const k = clang_getCursorKind(c);
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
	struct cxi_use use = { .expr = clang_getNullCursor(), .file = file };
	clang_getExpansionLocation(clang_getRangeStart(extent), nullptr, nullptr, nullptr,
	                           &use.from);
	clang_getExpansionLocation(clang_getRangeEnd(extent), nullptr, nullptr, nullptr, &use.to);
	clang_visitChildren(clang_getTranslationUnitCursor(x->tu), cxi_use, &use);
	if (clang_Cursor_isNull(use.expr))
		return;
	cxi_out_printf(&x->out, "at ");
	cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
	cxi_out_printf(&x->out, ":%u: an expression of type ", line);
	cxi_out_clang(&x->out, clang_getTypeSpelling(clang_getCursorType(use.expr)));
	CXEvalResult value = clang_Cursor_Evaluate(use.expr);
	switch (value ? clang_EvalResult_getKind(value) : CXEval_UnExposed) {
	case CXEval_Int:
		if (clang_EvalResult_isUnsignedInt(value))
			cxi_out_printf(&x->out, ", value %llu",
			               clang_EvalResult_getAsUnsigned(value));
		else
			cxi_out_printf(&x->out, ", value %lld",
			               clang_EvalResult_getAsLongLong(value));
		break;
	case CXEval_Float:
		cxi_out_printf(&x->out, ", value %g", clang_EvalResult_getAsDouble(value));
		break;
	case CXEval_StrLiteral:
		cxi_out_printf(&x->out, ", value \"");
		cxi_out_text(&x->out, clang_EvalResult_getAsStr(value));
		cxi_out_printf(&x->out, "\"");
		break;
	default:
		break;
	}
	if (value)
		clang_EvalResult_dispose(value);
	cxi_out_printf(&x->out, "\n");
}

static bool
cxi_is_named (CXCursor    c,
              char const *name)
{
	CXString spelling = clang_getCursorSpelling(c);
	char const *s = clang_getCString(spelling);
	bool const named = cxi_kind(c) && s && !strcmp(s, name);
	clang_disposeString(spelling);
	return named;
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
	struct cstr_view words;
	size_t const total = cxi_find_line(text, size, line, &words);
	size_t const len = strlen(name);
	/* A cursor inside a longer word refers to a name that is not @p name. */
	for (size_t i = 0; i + len <= words.len; ++i) {
		if (memcmp(words.data + i, name, len))
			continue;
		*column = (unsigned)i + 1;
		CXSourceLocation const loc = clang_getLocation(x->tu, file, line, *column);
		CXCursor const at = clang_getCursor(x->tu, loc);
		CXCursor const referenced = clang_getCursorReferenced(at);
		*c = clang_Cursor_isNull(referenced) ? at : referenced;
		if (cxi_is_named(*c, name))
			return 0;
	}
	if (!words.data) {
		cxi_out_printf(&x->out, "Line %u is past the end of ", line);
		cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
		cxi_out_printf(&x->out, ", which has %zu line%s.", total, util_s(total));
		return EINVAL;
	}
	cxi_out_printf(&x->out, "Line %u of ", line);
	cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
	cxi_out_printf(&x->out, " has no word ");
	cxi_out_text(&x->out, name);
	cxi_out_printf(&x->out, ".");
	return EINVAL;
}

/* Returns the complete struct or union of the type of @p c, or null. */
static CXCursor
cxi_record_of (CXCursor c)
{
	CXType const type = clang_getCanonicalType(clang_getCursorType(c));
	CXCursor const decl = clang_getCursorDefinition(clang_getTypeDeclaration(type));
	enum CXCursorKind const k = clang_getCursorKind(decl);
	return (k == CXCursor_StructDecl || k == CXCursor_UnionDecl) &&
	       clang_Type_getSizeOf(type) >= 0 ? decl : clang_getNullCursor();
}

struct cxi_seek {
	char const *name;
	CXCursor    found;
};

/* Finds the member seek->name, also inside anonymous members. */
static enum CXVisitorResult
cxi_seek (CXCursor     c,
          CXClientData data)
{
	struct cxi_seek *seek = data;
	CXType const type = clang_getCanonicalType(clang_getCursorType(c));
	if (clang_Cursor_isAnonymousRecordDecl(clang_getTypeDeclaration(type)))
		clang_Type_visitFields(type, cxi_seek, seek);
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
	struct cxi_find find = { .list.out = &x->out, .name = cstr_view(name) };
	clang_visitChildren(clang_getTranslationUnitCursor(x->tu), cxi_match, &find);
	CXCursor record = clang_getNullCursor();
	for (size_t i = 0; clang_Cursor_isNull(record) && i < find.list.n; ++i)
		record = cxi_record_of(find.list.items[i]);
	free(find.list.items);
	if (clang_Cursor_isNull(record)) {
		cxi_out_printf(&x->out, "No struct, union or typedef of one named %s in the "
		               "translation unit of ", name);
		cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
		cxi_out_printf(&x->out, ".");
		return EINVAL;
	}
	struct cxi_seek seek = { member, clang_getNullCursor() };
	clang_Type_visitFields(clang_getCursorType(record), cxi_seek, &seek);
	*c = seek.found;
	if (!clang_Cursor_isNull(*c))
		return (int)x->out.e;
	cxi_out_clang(&x->out, clang_getTypeSpelling(clang_getCursorType(record)));
	cxi_out_printf(&x->out, " has no member %s.", member);
	return EINVAL;
}

/* Returns the length of the C identifier at the start of the NUL-terminated
 * string @p s, or 0 if @p s does not start with one.
 */
static size_t
cxi_ident (char const *s)
{
	size_t const n = strspn(s, UTIL_ALNUM "_");
	return n && (*s < '0' || *s > '9') ? n : 0;
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
	static struct { struct cstr_view tag; uint64_t kind; } const tags[] = {
		{ CSTR_VIEW_INIT("struct "), CXCursor_StructDecl },
		{ CSTR_VIEW_INIT("union "), CXCursor_UnionDecl },
		{ CSTR_VIEW_INIT("enum "), CXCursor_EnumDecl },
		{ CSTR_VIEW_INIT(""), 0 },
	};
	struct cstr_view rest = x->key[CXI_KEY_NAME];
	size_t t = 0;
	while (!cstr_view_starts(rest, tags[t].tag))
		++t;
	*kind = tags[t].kind;
	rest = cstr_view_slice(rest, tags[t].tag.len, SIZE_MAX);
	/* rest is the end of a string in argv, so rest.data is NUL-terminated. */
	size_t const a = cxi_ident(rest.data);
	struct cstr_view const dot = cstr_view_slice(rest, a, SIZE_MAX);
	size_t const b = dot.len && *dot.data == '.' ? cxi_ident(dot.data + 1) : 0;
	if (!a || a > CXI_NAME || b > CXI_NAME || dot.len != (b ? b + 1 : 0)) {
		cxi_out_printf(&x->out, "name must be a name of C, such as git_start, struct dstr "
		               "or child.flags.");
		return EINVAL;
	}
	memcpy(name, rest.data, a);
	name[a] = '\0';
	memcpy(member, dot.data + !!b, b);
	member[b] = '\0';
	return 0;
}

/* Appends the field @p c with its place in the outermost named record. */
static void
cxi_member (struct cxi *x,
            CXCursor    c)
{
	CXCursor const parent = clang_getCursorSemanticParent(c);
	CXCursor outer = parent;
	while (clang_Cursor_isAnonymousRecordDecl(outer))
		outer = clang_getCursorSemanticParent(outer);
	CXType const record = clang_getCursorType(outer);
	CXType const type = clang_getCursorType(c);
	CXString name = clang_getCursorSpelling(c);
	long long const offset = clang_Type_getOffsetOf(record, clang_getCString(name));
	long long const size = clang_Cursor_isBitField(c) ? clang_getFieldDeclBitWidth(c)
	                     : 8 * clang_Type_getSizeOf(type);
	cxi_out_printf(&x->out, "field ");
	cxi_out_clang(&x->out, name);
	cxi_out_printf(&x->out, " of ");
	cxi_out_clang(&x->out, clang_getTypeSpelling(record));
	if (!clang_equalCursors(parent, outer))
		cxi_out_printf(&x->out, ", in an anonymous %s", cxi_kind(parent));
	cxi_out_printf(&x->out, ": ");
	cxi_out_clang(&x->out, clang_getTypeSpelling(type));
	char bits[2][CXI_BITS];
	cxi_out_printf(&x->out, "\noffset %s, size %s, alignment %lld\n",
	               cxi_offset(bits[0], offset > 0 ? (uint64_t)offset : 0),
	               cxi_size(bits[1], size > 0 ? (uint64_t)size : 0),
	               clang_Type_getAlignOf(type));
}

/* Appends the kind, name and type of @p c, the value of an enumerator, and
 * the size of a record, whose layout goes to @p rows.
 */
static void
cxi_summary (struct cxi     *x,
             struct cxi_out *rows,
             CXCursor        c)
{
	enum CXCursorKind const k = clang_getCursorKind(c);
	bool const alias = k == CXCursor_TypedefDecl;
	CXCursor const parent = clang_getCursorSemanticParent(c);
	bool const local = clang_getCursorKind(parent) == CXCursor_FunctionDecl;
	cxi_out_printf(&x->out, "%s%s%s ", cxi_static(c),
	               k == CXCursor_VarDecl && local ? "local " : "", cxi_kind(c));
	cxi_out_name(&x->out, c);
	if (local) {
		cxi_out_printf(&x->out, k == CXCursor_ParmDecl ? " of " : " in ");
		cxi_out_clang(&x->out, clang_getCursorSpelling(parent));
	}
	if (k == CXCursor_EnumConstantDecl) {
		CXType const t = clang_getCanonicalType(clang_getEnumDeclIntegerType(parent));
		cxi_out_printf(&x->out, " of enum ");
		cxi_out_name(&x->out, parent);
		if (clang_Type_getSizeOf(t) > 8)
			cxi_out_printf(&x->out, ": a value of more than 64 bits");
		else if (t.kind >= CXType_Bool && t.kind <= CXType_ULongLong)
			cxi_out_printf(&x->out, ": %llu", clang_getEnumConstantDeclUnsignedValue(c));
		else
			cxi_out_printf(&x->out, ": %lld", clang_getEnumConstantDeclValue(c));
	}
	CXType const type = alias ? clang_getTypedefDeclUnderlyingType(c)
	                  : k == CXCursor_EnumDecl ? clang_getEnumDeclIntegerType(c)
	                  : clang_getCursorType(c);
	if (k != CXCursor_EnumConstantDecl && k != CXCursor_MacroDefinition &&
	    k != CXCursor_StructDecl && k != CXCursor_UnionDecl) {
		cxi_out_printf(&x->out, ": ");
		cxi_out_clang(&x->out, clang_getTypeSpelling(type));
	}
	CXCursor const record = cxi_record_of(c);
	if (clang_Cursor_isNull(record) || (!alias && !clang_equalCursors(record, c))) {
		cxi_out_printf(&x->out, "\n");
		return;
	}
	if (alias) {
		cxi_out_printf(&x->out, "\n");
		cxi_out_clang(&x->out, clang_getTypeSpelling(type));
	}
	cxi_layout(x, rows, clang_getCursorType(record));
}

static int
cxi_describe (struct cxi *x)
{
	char name[CXI_NAME + 1];
	char member[CXI_NAME + 1];
	uint32_t kind = 0;
	unsigned column = 0;
	CXCursor c = clang_getNullCursor();
	int e = cxi_parse_name(x, name, member, &kind);
	if (e)
		return e;
	e = x->line ? cxi_lookup_line(x, *member ? member : name, &c, &column)
	  : *member ? cxi_lookup_member(x, name, member, &c)
	  : cxi_lookup(x, name, kind, &c);
	if (e)
		return e;
	enum CXCursorKind const k = clang_getCursorKind(c);
	CXCursor const shown = cxi_shown(c);
	CXCursor const first = clang_getCanonicalCursor(c);
	bool const defined = k == CXCursor_MacroDefinition || clang_isCursorDefinition(shown);
	cxi_head(x);
	cxi_out_printf(&x->out, "%s ", cxi_kind(c));
	if (*member && !x->line)
		cxi_out_printf(&x->out, "%s.%s", name, member);
	else
		cxi_out_name(&x->out, c);
	cxi_out_printf(&x->out, " in the translation unit of ");
	cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
	cxi_out_printf(&x->out, ".]\n");
	struct cxi_out rows = { .text = DSTR_INIT, .tree = x->out.tree };
	if (k == CXCursor_FieldDecl)
		cxi_member(x, shown);
	else
		cxi_summary(x, &rows, shown);
	if (!defined) {
		cxi_out_place(&x->out, "declared ", clang_getCursorLocation(first), "\n");
		cxi_out_printf(&x->out, "not defined in this translation unit\n");
	} else {
		if (!clang_equalCursors(shown, first))
			cxi_out_place(&x->out, "declared ", clang_getCursorLocation(first), "\n");
		cxi_out_place(&x->out, "defined ", clang_getCursorLocation(shown), "\n");
		if (k == CXCursor_FunctionDecl)
			cxi_out_calls(&x->out, shown);
	}
	cxi_out_put(&x->out, dstr_get(&rows.text), dstr_len(&rows.text));
	if (k == CXCursor_MacroDefinition && x->line)
		cxi_value(x, column);
	cxi_declaration(x, shown);
	x->out.e = x->out.e ? x->out.e : rows.e;
	dstr_cleanup(&rows.text);
	return (int)x->out.e;
}

static void
cxi_out_diagnostic (struct cxi_out            *out,
                    CXDiagnostic               d,
                    enum CXDiagnosticSeverity  severity)
{
	CXSourceLocation const loc = clang_getDiagnosticLocation(d);
	CXFile file;
	clang_getExpansionLocation(loc, &file, nullptr, nullptr, nullptr);
	if (file)
		cxi_out_place(out, "", loc, ": ");
	cxi_out_printf(out, "%s: ", severity >= CXDiagnostic_Error ? "error" : "warning");
	cxi_out_clang(out, clang_getDiagnosticSpelling(d));
	CXString option = clang_getDiagnosticOption(d, nullptr);
	char const *o = clang_getCString(option);
	if (o && *o) {
		cxi_out_printf(out, " [");
		cxi_out_text(out, o);
		cxi_out_printf(out, "]");
	}
	clang_disposeString(option);
	cxi_out_printf(out, "\n");
}

/* Appends the errors and warnings, but not the error that clang cannot
 * open a file: every file of the tree can be opened, and for one outside
 * it the reason, such as "Permission denied", would show whether it exists.
 * The error that the file is not found follows it.
 */
static int
cxi_diagnostics (struct cxi *x)
{
	static char const unopened[] = "cannot open file '";
	struct cxi_out rows = { .text = DSTR_INIT, .tree = x->out.tree };
	unsigned errors = 0;
	unsigned warnings = 0;
	for (unsigned i = 0, n = clang_getNumDiagnostics(x->tu); i < n; ++i) {
		CXDiagnostic d = clang_getDiagnostic(x->tu, i);
		CXString text = clang_getDiagnosticSpelling(d);
		char const *s = clang_getCString(text);
		enum CXDiagnosticSeverity const severity =
			s && !strncmp(s, unopened, sizeof unopened - 1)
			? CXDiagnostic_Ignored : clang_getDiagnosticSeverity(d);
		clang_disposeString(text);
		errors += severity >= CXDiagnostic_Error;
		warnings += severity == CXDiagnostic_Warning;
		if (severity >= CXDiagnostic_Warning)
			cxi_out_diagnostic(&rows, d, severity);
		clang_disposeDiagnostic(d);
	}
	cxi_head(x);
	cxi_out_printf(&x->out, "diagnostics of ");
	cxi_out_path(&x->out, x->key[CXI_KEY_PATH]);
	cxi_out_printf(&x->out, " with the build's flags");
	for (size_t i = 0; i < x->n_warnings; ++i)
		cxi_out_printf(&x->out, "%s%s", i ? " " : " and ", x->warning[i]);
	cxi_out_printf(&x->out, ": %u error%s, %u warning%s.]\n", errors, util_s(errors),
	               warnings, util_s(warnings));
	cxi_out_put(&x->out, dstr_get(&rows.text), dstr_len(&rows.text));
	x->out.e = x->out.e ? x->out.e : rows.e;
	dstr_cleanup(&rows.text);
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
		bool const warning = k == CXI_KEY_WARNING && x->n_warnings < CXI_WARNINGS;
		bool const once = k < CXI_KEYS && (!x->key[k].data || warning) &&
		                  (k != CXI_KEY_UNCONFINED || cstr_view_equal(value, CSTR_VIEW("1")));
		size_t i = 0;
		while (i < value.len && (unsigned char)value.data[i] >= 0x20 && value.data[i] != 0x7f)
			++i;
		if (!once) {
			cxi_out_path(&x->out, cstr_view(argv[*at]));
			cxi_out_printf(&x->out, " is not a word that askai-code takes: it takes each "
			               "key once, warning up to 16 times, and unconfined=1.");
			return EINVAL;
		}
		if (i < value.len) {
			cxi_out_printf(&x->out, "%s has a control character.", cxi_keys[k].data);
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

/* Checks the action, the keys of the action and the path. A warning is
 * -W or -Wno-, a name of a-z, 0-9 and '-', and maybe =DIGITS.
 */
static int
cxi_valid (struct cxi *x)
{
	struct cstr_view const line = x->key[CXI_KEY_LINE];
	struct cstr_view const path = x->key[CXI_KEY_PATH];
	x->action = cstr_view_index(cxi_actions, CXI_ACTIONS, x->key[CXI_KEY_ACTION]);
	bool const describe = x->action == CXI_ACTION_DESCRIBE;
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
	char const *why = x->action == CXI_ACTIONS
	                  ? "action must be " XL_JOIN(", ", " or ", CXI_ACTION_LIST(XL_TEXT)) "."
	                : describe && !x->key[CXI_KEY_NAME].data
	                  ? "describe needs name: a symbol such as git_start, or struct.member "
	                    "such as child.flags."
	                : !describe && (x->key[CXI_KEY_NAME].data || line.data)
	                  ? "name and line are only for describe."
	                : line.data && (util_number(line.data, line.len, UINT32_MAX, &x->line) ||
	                                !x->line)
	                  ? "line must be a whole number from 1."
	                : x->n_warnings && x->action != CXI_ACTION_DIAGNOSTICS
	                  ? "warnings is only for diagnostics." : nullptr;
	if (why) {
		cxi_out_printf(&x->out, "%s", why);
	} else if (i < x->n_warnings) {
		cxi_out_printf(&x->out, "Each warning must be -W or -Wno- and the name of a warning, "
		               "such as -Wpadded: ");
		cxi_out_path(&x->out, cstr_view(x->warning[i]));
		cxi_out_printf(&x->out, ".");
	} else if (memchr(path.data, ':', path.len)) {
		cxi_out_printf(&x->out, "Give the commit as revision and the path alone.");
	} else if (!util_path_inside(path)) {
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
 * and points x->file at its last word, the file in @p tree. A relative
 * path of the compile commands is relative to the repository's directory.
 */
static int
cxi_command (struct cxi       *x,
             struct cstr_view  tree,
             struct dstr      *words)
{
	char const *const repo = x->key[CXI_KEY_REPO].data;
	char const *const file = x->key[CXI_KEY_CDB].data;
	int dir = open(x->key[CXI_KEY_TOP].data, O_PATH | O_DIRECTORY | O_CLOEXEC);
	int fd = openat(dir, file, O_RDONLY | O_CLOEXEC);
	util_close(&dir);
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
	static int (*const action[CXI_ACTIONS])(struct cxi *) = {
		[CXI_ACTION_DESCRIBE] = cxi_describe,
		[CXI_ACTION_OUTLINE] = cxi_outline,
		[CXI_ACTION_DIAGNOSTICS] = cxi_diagnostics,
	};
	/* -fparse-all-comments gives describe the plain comments of a
	 * declaration; with it, -Wdocumentation would warn about them. */
	static char const *const extra[] = {
		"-Xclang", "-disable-pragma-debug-crash", "-fparse-all-comments",
	};
	size_t const n_extra = array_size(extra) - (x->action != CXI_ACTION_DESCRIBE);
	size_t n = 0;
	for (size_t i = 0; i < dstr_len(words); ++i)
		n += !dstr_get(words)[i];
	size_t const argc = n + n_extra + x->n_warnings;
	char const **argv = malloc(argc * sizeof *argv);
	if (!argv)
		return ENOMEM;
	char const *word = dstr_get(words);
	for (size_t i = 0; i < n; ++i, word += strlen(word) + 1)
		argv[i] = word;
	memcpy(argv + n, extra, n_extra * sizeof *argv);
	memcpy(argv + n + n_extra, x->warning, x->n_warnings * sizeof *argv);
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
		e = action[x->action](x);
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
	struct cxi x = { .out.text = DSTR_INIT, .record = DSTR_INIT };
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
	struct cstr_view const record = e ? (struct cstr_view){} : cstr_view_dstr(&x.record);
	if (util_write_all(fd, record.data, record.len) || util_write_all(fd, text, len) ||
	    (over && util_write_all(fd, cut, sizeof cut - 1)))
		e = EIO;
	dstr_cleanup(&x.record);
	dstr_cleanup(&x.out.text);
	return e ? 1 : 0;
}
