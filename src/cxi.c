/** @file cxi.c
 *
 * askai-code, the helper program of the code tool. A function that returns
 * an int returns 0, EINVAL with a tool error in the answer, or another
 * error number.
 */
#define _GNU_SOURCE /* clearenv(), memmem(), memrchr() */
#include <clang-c/Index.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
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
#include "cxi_out.h"
#include "dstr.h"
#include "git_run.h"
#include "graph.h"
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
	struct stat         cdb;            /* the file of the compile commands
	                                       as it was read */
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

/* Returns the name of @p file, the file of @p loc. Without a file, returns
 * the name that clang presumes for @p loc and writes the line that it
 * presumes to @p *line: a macro of clang or of -D is in <built-in> or
 * <command line>. The caller disposes of the name.
 */
static CXString
cxi_file_name (CXSourceLocation  loc,
               CXFile            file,
               unsigned         *line)
{
	CXString name;
	if (file)
		return clang_getFileName(file);

	clang_getPresumedLocation(loc, &name, line, nullptr);
	return name;
}

/* Appends the path of the file of @p loc and returns the line of @p loc. */
static unsigned
cxi_out_file (struct cxi_out   *out,
              CXSourceLocation  loc)
{
	CXFile file;
	unsigned line;
	clang_getFileLocation(loc, &file, &line, nullptr, nullptr);
	CXString name = cxi_file_name(loc, file, &line);
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
		k = s.data[i] == '\t' ? 1 : cxi_out_char(s.data + i, s.len - i);
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
	for (size_t k; i < rel.len && (k = cxi_out_char(rel.data + i, rel.len - i)); i += k)
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

/* The search for the first member of a record that is named name
 * (cxi_seek_member()). found is null while the search has reached none.
 */
struct cxi_seek {
	char const *name;
	CXCursor    found;
};

/* Returns a search for @p name that has reached no cursor. */
static force_inline struct cxi_seek
cxi_seek (char const *name)
{
	return (struct cxi_seek){
		.name  = name,
		.found = clang_getNullCursor()
	};
}

/* Stores the member that the cxi_seek at @p data names in it, also a
 * member of an anonymous member.
 */
static enum CXVisitorResult
cxi_seek_member (CXCursor     c,
                 CXClientData data)
{
	struct cxi_seek *seek = data;
	CXType const type = clang_getCanonicalType(clang_getCursorType(c));
	if (clang_Cursor_isAnonymousRecordDecl(clang_getTypeDeclaration(type)))
		clang_Type_visitFields(type, cxi_seek_member, seek);
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
	struct cxi_seek seek = cxi_seek(member);
	clang_Type_visitFields(clang_getCursorType(record), cxi_seek_member, &seek);
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

/* A call of a function. at is the call expression, or the variable with
 * the attribute cleanup: the program calls the function of the attribute
 * at the end of the scope of the variable. The callees are the n
 * declarations from first on in the member to of struct cxi_walk. One
 * callee is the first declaration of a function, or of the field, variable
 * or parameter of a function pointer. Several callees are the functions of
 * which an expression selects one. n is 0 for any other callee. macro is
 * the definition of the macro that the call is written through
 * (cxi_macro()), or null. For a call that is GRAPH_UNEVALUATED, n is 0 and
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
	uint32_t state;  /* enum graph_state */
	uint32_t flags;  /* enum graph_call_flag */
};

static force_inline struct cxi_call
cxi_call (CXCursor           at,
          uint32_t           first,
          uint32_t           n,
          enum graph_call_flag flags)
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

/* Returns the state of @p c as an operand of typeof, or GRAPH_EVALUATED if
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
		return GRAPH_EVALUATED;
	/* A line feed ends a directive and a comment that starts with //: the
	 * word in such a line is not the keyword of a parenthesis in a later
	 * line. */
	char const *end = line.data + line.len;
	if (memchr(end, '\n', (size_t)(before.data + before.len - end)) &&
	    (memchr(line.data, '#', line.len) || memmem(line.data, line.len, "//", 2)))
		return GRAPH_EVALUATED;
	enum CXTypeKind const t = clang_getCanonicalType(clang_getCursorType(c)).kind;
	return (t >= CXType_FirstBuiltin && t <= CXType_LastBuiltin) || t == CXType_Record ||
	       t == CXType_Enum ? GRAPH_UNEVALUATED : GRAPH_UNPROVEN;
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
			call->flags |= GRAPH_CALL_BUILTIN;
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
		call->flags |= GRAPH_CALL_ELEMENT;
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
		*state = kid == chosen ? GRAPH_EVALUATED
		       : kid < first || chosen < n ? GRAPH_UNEVALUATED : GRAPH_UNPROVEN;
	}
	walk->kids.n = at;
	return e;
}

/* Writes GRAPH_UNPROVEN to @p *state if the call expression @p c is a call
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
	if (call.flags & GRAPH_CALL_BUILTIN)
		*state = GRAPH_UNPROVEN;
	return e;
}

/* Writes the state of the cursor of the visit of @p walk, the last node of
 * the path, to @p *state: the largest state of the place of a node in the
 * node before it. The places that are not GRAPH_EVALUATED are an operand of
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
	*state = GRAPH_EVALUATED;
	for (size_t i = 0; !e && i + 1 < walk->n && *state < GRAPH_UNEVALUATED; ++i) {
		CXCursor const up = walk->items[i].c;
		CXCursor const c = walk->items[i + 1].c;
		uint32_t place = GRAPH_EVALUATED;
		bool nonzero;
		switch (up.kind) {
		case CXCursor_GenericSelectionExpr:
		case CXCursor_UnexposedExpr:
			e = cxi_walk_selected(walk, up, walk->items[i].kids - 1, &place);
			break;
		case CXCursor_UnaryExpr:
			place = cxi_constant(up, &nonzero) ? GRAPH_UNEVALUATED : GRAPH_UNPROVEN;
			break;
		case CXCursor_CallExpr:
			e = cxi_walk_unevaluated(walk, up, &place);
			break;
		default:
			break;
		}
		uint32_t const operand = c.kind == CXCursor_ParenExpr ? cxi_typeof(walk->tu, c)
		                                                      : GRAPH_EVALUATED;
		place = place > operand ? place : operand;
		*state = *state > place ? *state : place;
	}
	return e;
}

/* Writes to @p *fn the first declaration of the function of the attribute
 * cleanup of the variable @p c (cxi_ast_cleanup()), and to @p *attr the
 * attribute. @p *fn is null for a variable without the attribute. clang
 * calls the function of the first attribute cleanup in the syntax tree of a
 * variable. clang 23 keeps only the last one that is written.
 */
static int
cxi_walk_cleanup (struct cxi_walk *walk,
                  CXCursor         c,
                  CXCursor        *attr,
                  CXCursor        *fn)
{
	size_t const at = walk->kids.n;
	int const e = cxi_list_children(&walk->kids, c);
	*attr = clang_getNullCursor();
	*fn = *attr;
	for (size_t i = at;
	     !e && clang_Cursor_isNull(*fn) && i < walk->kids.n; ++i) {
		*attr = walk->kids.items[i];
		*fn = cxi_ast_cleanup(*attr);
	}
	walk->kids.n = at;
	return e;
}

/* The number of a node of a parse. */
struct cxi_id {
	void const *node;
	uint64_t    id;
};

/* The numbers of the nodes of a parse: a hash table of room slots, a power
 * of two, of which n contain a node. The node of an empty slot is null. At
 * most half of the slots contain a node.
 */
struct cxi_ids {
	struct cxi_id *slots;
	size_t         n;
	size_t         room;
};

/* Frees the slots of @p ids. */
static void
cxi_ids_fini (struct cxi_ids *ids)
{
	free(ids->slots);
	*ids = (struct cxi_ids){};
}

/* Returns the slot of @p node among the @p room slots at @p slots: the one
 * that contains @p node, or the empty one where @p node belongs. At least
 * one slot is empty.
 */
static struct cxi_id *
cxi_ids_slot (struct cxi_id *slots,
              size_t         room,
              void const    *node)
{
	/* The low bits of a pointer to an object are 0. */
	uint64_t const hash = ((uintptr_t)node >> 4) *
	                      UINT64_C(0x9e3779b97f4a7c15);
	size_t i = (size_t)(hash >> 32) & (room - 1);
	while (slots[i].node && slots[i].node != node)
		i = (i + 1) & (room - 1);
	return &slots[i];
}

/* Returns true and writes the number of @p node to @p *id if @p ids
 * contains @p node.
 */
static bool
cxi_ids_get (struct cxi_ids const *ids,
             void const           *node,
             uint32_t             *id)
{
	if (!ids->room)
		return false;

	struct cxi_id const *slot = cxi_ids_slot(ids->slots, ids->room, node);
	if (!slot->node)
		return false;

	*id = (uint32_t)slot->id;
	return true;
}

/* Gives @p node, which @p ids does not contain, the number @p id. Returns
 * 0, ENOMEM, or EINVAL for a null pointer.
 */
static int
cxi_ids_put (struct cxi_ids *ids,
             void const     *node,
             uint32_t        id)
{
	if (!node)
		return EINVAL;

	if (2 * (ids->n + 1) > ids->room) {
		size_t const room = ids->room ? 2 * ids->room : 256;
		struct cxi_id *slots = calloc(room, sizeof *slots);
		if (!slots)
			return ENOMEM;
		for (size_t i = 0; i < ids->room; ++i) {
			struct cxi_id const *old = &ids->slots[i];
			if (old->node)
				*cxi_ids_slot(slots, room, old->node) = *old;
		}
		free(ids->slots);
		ids->slots = slots;
		ids->room = room;
	}

	*cxi_ids_slot(ids->slots, ids->room, node) = (struct cxi_id){
		.node = node,
		.id   = id
	};
	++ids->n;
	return 0;
}

/* The calls of a function, with the walk that collects them
 * (cxi_calls_collect()). nodes contains the node of each call that the
 * calls contained, also for an earlier function: a node is in the code of
 * one function. x is the call of askai-code.
 */
struct cxi_calls {
	UTIL_ARRAY(struct cxi_call);
	struct cxi_walk  walk;
	struct cxi_ids   nodes;
	struct cxi      *x;
};

/* Returns the calls of a function before a visit collects them. */
static force_inline struct cxi_calls
cxi_calls (struct cxi *x)
{
	return (struct cxi_calls){ .x = x };
}

/* Frees the calls, the nodes and the walk of @p calls. */
static void
cxi_calls_fini (struct cxi_calls *calls)
{
	cxi_ids_fini(&calls->nodes);
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

/* Appends @p call to @p calls and sets the row of the call. Returns 0,
 * ENOMEM, or EALREADY if the calls have contained a call at the same node
 * of the syntax tree (cxi_ast_node()): the callees of @p call are then
 * removed. libclang visits some expressions more than once, and not only
 * as the child before (cxi_walk_step()): the first operand of ?: without a
 * second once more in an implicit conversion, and the operand of typeof
 * for each variable of a declaration. The cursors of two visits of an
 * expression are not always equal for clang_equalCursors().
 */
static int
cxi_calls_push (struct cxi_calls *calls,
                struct cxi_call   call)
{
	void const *node = cxi_ast_node(call.at);
	uint32_t id;
	if (cxi_ids_get(&calls->nodes, node, &id)) {
		calls->walk.to.n = call.first;
		return EALREADY;
	}

	int e = cxi_ids_put(&calls->nodes, node, 0);
	if (!e)
		e = util_push(*calls, call);
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
	if (!e && call.state != GRAPH_UNEVALUATED) {
		call.macro = cxi_macro(walk->tu, clang_getCursorLocation(c));
		e = cxi_walk_call(walk, c, &call);
	}
	return e ? e : cxi_calls_push(calls, call);
}

/* Appends to @p calls the call of the function of the attribute cleanup of
 * the variable @p c, the cursor of the visit, if the variable is one with
 * the attribute and the program can evaluate the declaration. The program
 * calls the function at the end of the scope of the variable.
 */
static int
cxi_calls_add_cleanup (struct cxi_calls *calls,
                       CXCursor          c)
{
	struct cxi_walk *walk = &calls->walk;
	CXCursor attr;
	CXCursor fn;
	int e = cxi_walk_cleanup(walk, c, &attr, &fn);
	if (e || clang_Cursor_isNull(fn))
		return e;

	struct cxi_call call = cxi_call(c, (uint32_t)walk->to.n,
	                                1, GRAPH_CALL_CLEANUP);
	e = cxi_walk_state(walk, &call.state);
	if (e || call.state == GRAPH_UNEVALUATED)
		return e;

	call.macro = cxi_macro(walk->tu, clang_getCursorLocation(attr));
	e = util_push(walk->to, fn);
	return e ? e : cxi_calls_push(calls, call);
}

/* Moves the walk of the cxi_calls at @p data to @p c, and appends the call
 * of a call expression or of a variable with the attribute cleanup. The
 * visit does not enter a cursor that it has entered before: the walk or
 * the calls then return EALREADY.
 */
static enum CXChildVisitResult
cxi_calls_visit (CXCursor     c,
                 CXCursor     parent,
                 CXClientData data)
{
	struct cxi_calls *calls = data;
	int e = cxi_walk_step(&calls->walk, c, parent);

	if (!e && c.kind == CXCursor_CallExpr)
		e = cxi_calls_add(calls, c);
	else if (!e && c.kind == CXCursor_VarDecl && clang_Cursor_hasAttrs(c))
		e = cxi_calls_add_cleanup(calls, c);

	if (e == EALREADY)
		return CXChildVisit_Continue;

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
	bool const which = call->n > 1;
	bool const unproven = call->state == GRAPH_UNPROVEN;
	cxi_out_lit(out, "  ");
	cxi_calls_places(calls, i, ':');
	cxi_out_lit(out, " ");
	for (uint32_t f = 0; f < call->n; ++f) {
		CXCursor const to = calls->walk.to.items[call->first + f];
		enum CXCursorKind const k = to.kind;
		if (f)
			cxi_out_lit(out, " or ");
		if (k != CXCursor_FunctionDecl) {
			cxi_out_view(out, call->flags & GRAPH_CALL_ELEMENT
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
		if (call->flags & GRAPH_CALL_BUILTIN) {
			cxi_out_lit(out, " (builtin)");
		} else {
			cxi_out_lit(out, " (");
			cxi_out_place(out, clang_getCursorLocation(to));
			cxi_out_lit(out, ")");
		}
	}
	if (!call->n)
		cxi_out_lit(out, "indirect, through an expression");
	if (call->flags & GRAPH_CALL_CLEANUP)
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
	struct cxi_calls calls = cxi_calls(x);
	int const e = cxi_calls_collect(&calls, c);
	size_t rows = 0;
	size_t unevaluated = 0;
	size_t first = 0;
	for (size_t i = 0; !e && i < calls.n; ++i) {
		struct cxi_call const *call = &calls.items[i];
		if (call->state == GRAPH_UNEVALUATED) {
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

/* The graph of the translation unit of a parse while a visit fills it
 * (cxi_graph_function()): the draft, the numbers in the draft of the
 * nodes of the parse, and the calls of the function of the visit. The
 * number of a function is the index of its record. The number of a file is
 * the offset of the text of its path, and that of the definition of a
 * macro the offset of the text of its name.
 */
struct cxi_graph {
	struct cxi_calls   calls;
	struct graph_draft draft;
	struct cxi_ids     ids;
};

/* Returns an empty graph for a parse of the call @p x of askai-code. */
static force_inline struct cxi_graph
cxi_graph (struct cxi *x)
{
	return (struct cxi_graph){
		.calls = cxi_calls(x),
		.draft = graph_draft()
	};
}

/* Frees the draft, the numbers and the calls of @p g. */
static void
cxi_graph_fini (struct cxi_graph *g)
{
	cxi_ids_fini(&g->ids);
	graph_draft_fini(&g->draft);
	cxi_calls_fini(&g->calls);
}

/* Writes to @p *file the offset in the text of @p g of the path of the
 * file of @p loc, as cxi_out_file() appends the path before it quotes it,
 * and to @p *line the line of @p loc.
 */
static int
cxi_graph_file (struct cxi_graph *g,
                CXSourceLocation  loc,
                uint32_t         *file,
                uint32_t         *line)
{
	struct cxi_out *out = &g->calls.x->out;
	CXFile f;
	unsigned l;
	int e = 0;
	clang_getFileLocation(loc, &f, &l, nullptr, nullptr);
	if (!f || !cxi_ids_get(&g->ids, f, file)) {
		CXString name = cxi_file_name(loc, f, &l);
		char const *path = clang_getCString(name);
		e = graph_draft_text(&g->draft,
		                     cxi_out_rel(out, path ? path : ""), file);
		clang_disposeString(name);
		if (!e && f)
			e = cxi_ids_put(&g->ids, f, *file);
	}
	*line = l;
	return e;
}

/* Writes the file and the lines of the declaration @p c to @p *place, as
 * cxi_graph_file() and the extent of @p c give them.
 */
static int
cxi_graph_place (struct cxi_graph   *g,
                 CXCursor            c,
                 struct graph_place *place)
{
	CXSourceRange const extent = clang_getCursorExtent(c);
	uint32_t line;
	place->first = cxi_line(clang_getRangeStart(extent));
	place->last = cxi_line(clang_getRangeEnd(extent));
	return cxi_graph_file(g, clang_getCursorLocation(c), &place->file,
	                      &line);
}

/* Writes to @p *id the index in @p g of the function whose first
 * declaration is @p d, and appends the function if @p g does not contain
 * it: the name, the linkage, the place of @p d, and the place of the
 * definition if the translation unit has one.
 */
static int
cxi_graph_fn (struct cxi_graph *g,
              CXCursor          d,
              uint32_t         *id)
{
	void const *node = cxi_ast_node(d);
	if (cxi_ids_get(&g->ids, node, id))
		return 0;

	struct graph_fn fn = {
		.flags   = clang_getCursorLinkage(d) == CXLinkage_Internal
		           ? GRAPH_FN_STATIC : 0,
		.defined = { .file = GRAPH_NONE }
	};
	CXString name = clang_getCursorSpelling(d);
	int e = graph_draft_text(&g->draft, cstr_view(clang_getCString(name)),
	                         &fn.name);
	clang_disposeString(name);
	if (!e)
		e = cxi_graph_place(g, d, &fn.declared);

	CXCursor const definition = clang_getCursorDefinition(d);
	if (!e && !clang_Cursor_isNull(definition))
		e = cxi_graph_place(g, definition, &fn.defined);

	*id = (uint32_t)g->draft.fns.n;
	if (!e)
		e = util_push(g->draft.fns, fn);
	return e ? e : cxi_ids_put(&g->ids, node, *id);
}

/* Writes to @p *at the offset in the text of @p g of the name of the macro
 * that @p macro defines, and appends the name if @p g does not contain it.
 */
static int
cxi_graph_macro (struct cxi_graph *g,
                 CXCursor          macro,
                 uint32_t         *at)
{
	void const *node = cxi_ast_node(macro);
	if (cxi_ids_get(&g->ids, node, at))
		return 0;

	CXString name = clang_getCursorSpelling(macro);
	int e = graph_draft_text(&g->draft, cstr_view(clang_getCString(name)),
	                         at);
	clang_disposeString(name);
	return e ? e : cxi_ids_put(&g->ids, node, *at);
}

/* Appends @p call, one of the calls of @p g, to the draft of @p g. A
 * callee is in the graph only if it is a function: the one callee that is
 * a variable, a parameter or a field is left out.
 */
static int
cxi_graph_call (struct cxi_graph      *g,
                struct cxi_call const *call)
{
	struct graph_call to = {
		.to    = (uint32_t)g->draft.to.n,
		.macro = GRAPH_NONE,
		.state = call->state,
		.flags = call->flags
	};
	int e = cxi_graph_file(g, clang_getCursorLocation(call->at), &to.file,
	                       &to.line);
	for (uint32_t i = 0; !e && i < call->n; ++i) {
		CXCursor const d = g->calls.walk.to.items[call->first + i];
		uint32_t id;
		if (d.kind != CXCursor_FunctionDecl)
			continue;
		e = cxi_graph_fn(g, d, &id);
		if (!e)
			e = util_push(g->draft.to, id);
	}
	to.n = (uint32_t)g->draft.to.n - to.to;
	if (!e && !clang_Cursor_isNull(call->macro))
		e = cxi_graph_macro(g, call->macro, &to.macro);
	return e ? e : util_push(g->draft.calls, to);
}

/* Appends each function at file scope to the cxi_graph at @p data, and the
 * calls in the code of a function that a file of the tree defines. A visit
 * that clang_visitChildren() reports as stopped has failed, with the error
 * in the walk of the calls.
 */
static enum CXChildVisitResult
cxi_graph_function (CXCursor     c,
                    CXCursor,
                    CXClientData data)
{
	struct cxi_graph *g = data;
	struct cxi_calls *calls = &g->calls;
	if (c.kind != CXCursor_FunctionDecl)
		return CXChildVisit_Continue;

	uint32_t id;
	int e = cxi_graph_fn(g, clang_getCanonicalCursor(c), &id);
	if (!e && (!clang_isCursorDefinition(c) ||
	           !cxi_in_tree(calls->x->out.tree, c)))
		return CXChildVisit_Continue;

	e = e ? e : cxi_calls_collect(calls, c);
	uint32_t const first = (uint32_t)g->draft.calls.n;
	for (size_t i = 0; !e && i < calls->n; ++i)
		e = cxi_graph_call(g, &calls->items[i]);
	if (e) {
		calls->walk.e = e;
		return CXChildVisit_Break;
	}

	/* A callee can have moved the functions of the draft. */
	g->draft.fns.items[id].calls = first;
	g->draft.fns.items[id].n_calls = (uint32_t)g->draft.calls.n - first;
	return CXChildVisit_Continue;
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

/* Writes the graph of the translation unit of the parse of @p x to @p fd.
 * Returns 0, the error of the visit that fills the graph, or the error of
 * the write.
 */
static int
cxi_graph_write (struct cxi *x,
                 int         fd)
{
	struct cxi_graph g = cxi_graph(x);
	int e = clang_visitChildren(clang_getTranslationUnitCursor(x->tu),
	                            cxi_graph_function, &g)
	        ? (int)g.calls.walk.e : (int)x->out.e;
	g.draft.flags = cxi_tu_errors(x->tu) ? GRAPH_ERRORS : 0;
	if (!e)
		e = graph_draft_write(&g.draft, fd);
	cxi_graph_fini(&g);
	return e;
}

/* callers: what askai needs to answer from the graphs of the build
 * (src/build.h), after the request is checked and the commit exported. The
 * answer is the name of the functions, a NUL, a letter and the path of a
 * file, a NUL, the name of the build, a NUL, and for each translation unit
 * of the compile commands (cdb_next()) the index of its entry, a NUL, the
 * path of its file, and a NUL.
 *
 * The letter is A for each function of the name. With a path, the callee is
 * only the function of the name that the translation unit of the path
 * declares, which is parsed: the letter is S if that function is static and
 * E if not, and the file is that of its first declaration. The name of the
 * build is the repository, the commit, and the device, the inode, the size
 * and the time of the last change of the file of the compile commands: a
 * change of that file makes another build.
 */
static int
cxi_callers (struct cxi *x)
{
	struct cxi_out *out = &x->out;
	struct cstr_view const name = x->key[CXI_KEY_NAME];
	CXCursor first = clang_getNullCursor();
	if (x->key[CXI_KEY_PATH].data) {
		CXCursor c;
		int const e = cxi_lookup(x, name.data, CXCursor_FunctionDecl, &c);
		if (e)
			return e;
		first = clang_getCanonicalCursor(c);
	}
	/* The name is a word of the request, so a NUL follows it. */
	cxi_out_put(out, name.data, name.len + 1);
	if (clang_Cursor_isNull(first)) {
		cxi_out_lit(out, "A");
	} else {
		CXSourceLocation const loc = clang_getCursorLocation(first);
		CXFile f;
		unsigned line;
		clang_getFileLocation(loc, &f, &line, nullptr, nullptr);
		CXString file = cxi_file_name(loc, f, &line);
		char const *path = clang_getCString(file);
		cxi_out_view(out, clang_getCursorLinkage(first) == CXLinkage_Internal
		             ? CSTR_VIEW("S") : CSTR_VIEW("E"));
		cxi_out_view(out, cxi_out_rel(out, path ? path : ""));
		clang_disposeString(file);
	}
	cxi_out_put(out, "", 1);
	cxi_out_view(out, x->key[CXI_KEY_REPO]);
	cxi_out_lit(out, " ");
	cxi_out_view(out, x->key[CXI_KEY_COMMIT]);
	uint64_t const state[] = {
		x->cdb.st_dev,
		x->cdb.st_ino,
		(uint64_t)x->cdb.st_size,
		(uint64_t)x->cdb.st_mtim.tv_sec,
		(uint64_t)x->cdb.st_mtim.tv_nsec,
	};
	for (size_t i = 0; i < array_size(state); ++i) {
		cxi_out_lit(out, " ");
		cxi_out_num(out, state[i]);
	}
	cxi_out_put(out, "", 1);

	struct cdb_map const map = cdb_map(x->key[CXI_KEY_TOP],
	                                   cstr_view_slice(out->tree, 0, out->tree.len - 1));
	struct dstr words = DSTR_INIT;
	int e;
	for (size_t at = 0; !(e = cdb_next(&map, x->cmds, &at, &words));) {
		char const *last = cxi_last_word(cstr_view_dstr(&words));
		struct cstr_view const file = cxi_out_rel(out, last);
		cxi_out_num(out, at - 1);
		cxi_out_put(out, "", 1);
		/* The file is a word of the command line, so a NUL follows it. */
		cxi_out_put(out, file.data, file.len + 1);
	}
	dstr_cleanup(&words);
	return out->e ? (int)out->e : e == ENOENT ? 0 : e;
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
 * the words and x->file at the last word, the file in @p tree. With a unit
 * in place of a path, the command line is that of the translation unit of
 * that entry (cdb_next()). Without both, x->words and x->file stay null,
 * and the compile commands must contain a translation unit. A relative
 * path of the compile commands is relative to the repository's directory.
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
	int e = fd < 0 || fstat(fd, &x->cdb) ? errno
	      : !S_ISREG(x->cdb.st_mode) || x->cdb.st_size > CXI_CDB ? EFBIG : 0;
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
	struct cstr_view const unit = x->key[CXI_KEY_UNIT];
	uint64_t entry = 0;
	size_t at = 0;
	if (path.data) {
		e = cdb_command(&map, x->cmds, path, words);
	} else if (unit.data && util_number(unit.data, unit.len, SIZE_MAX - 1, &entry)) {
		e = EINVAL;
	} else {
		/* No translation unit, or none at the entry of the unit, is the
		 * EINVAL of cdb_command(). */
		at = (size_t)entry;
		e = cdb_next(&map, x->cmds, &at, words);
		if (e == ENOENT || (!e && unit.data && at != entry + 1))
			e = EINVAL;
	}
	if (!e && (path.data || unit.data)) {
		x->words = cstr_view_dstr(words);
		x->file = cxi_last_word(x->words);
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
		cxi_out_view(&x->out, unit.data
		             ? CSTR_VIEW(" contain no translation unit at the entry of "
		                         "the unit.")
		             : CSTR_VIEW(" contain no entry with arguments for a file "
		                         "of the repository."));
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
	                                 subcommand, up to a nullptr, or nullptr
	                                 for a request that exports no commit */
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
 * exclusive lock if needed. Returns ESTALE if the tree contains another
 * commit and the request exports none.
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
		if (!same && !cache->git)
			return ESTALE;
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

/* Parses the file of the path or of the unit, if the request contains
 * one, with x->words (cxi_tu()) and appends the answer of the action. For
 * a request with a unit, it writes the graph of the parse to @p *fd in
 * place of an answer. The function first copies fd 1 to @p *fd for the
 * answer and points fd 1 at /dev/null, so that clang cannot write into the
 * answer. Then it changes to @p tree, clears the environment and confines
 * the process.
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
		cxi_out_path(&x->out, x->key[CXI_KEY_PATH].data
		             ? x->key[CXI_KEY_PATH]
		             : cxi_out_rel(&x->out, x->file));
		cxi_out_lit(&x->out, ".");
		e = EINVAL;
	} else if (x->key[CXI_KEY_UNIT].data) {
		e = cxi_graph_write(x, *fd);
	} else {
		e = action[x->action](x);
	}
	clang_disposeTranslationUnit(x->tu);
	clang_disposeIndex(x->index);
	return e;
}

/* Runs the request with @p git, the git command line up to the
 * subcommand and a nullptr. A request with a unit runs no git.
 */
static int
cxi_run (struct cxi         *x,
         char const *const  *git,
         int                *fd)
{
	struct cstr_view const *key = x->key;
	struct cstr_view const repo = key[CXI_KEY_REPO];
	struct cstr_view const commit = key[CXI_KEY_COMMIT];
	bool const unit = key[CXI_KEY_UNIT].data;
	/* The name of the repository is a directory of the cache. */
	if (unit == (key[CXI_KEY_ACTION].data != nullptr) ||
	    !git_run_name_valid(repo) || !key[CXI_KEY_CDB].data ||
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
	/* A request with a unit contains no argument of a tool call. */
	x->action = CXI_ACTIONS;
	int e = unit ? 0 : cxi_valid(x);
	if (e)
		return e;
	struct cxi_cache cache = cxi_cache(unit ? nullptr : git,
	                                   x->deadline > CXI_EARLY
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
		cxi_out_path(&x->out, unit ? cxi_out_rel(&x->out, x->file)
		                           : key[CXI_KEY_PATH]);
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
	/* The answer to callers is no text of the tool: askai reads it. */
	struct cstr_view const text = !e && x.action == CXI_ACTION_CALLERS
	                              ? cstr_view_dstr(&x.out.text)
	                              : cxi_out_answer(&x.out);
	struct cstr_view const record = e ? cstr_view(nullptr) : cstr_view_dstr(&x.record);
	if (util_write_all(fd, record.data, record.len) ||
	    util_write_all(fd, text.data, text.len))
		e = EIO;
	cxi_fini(&x);
	return e == ESTALE ? CXI_STALE : e ? 1 : 0;
}
