/** @file cxi_ast.cpp
 *
 * clang knows the selected association of a _Generic selection, and
 * libclang returns it through no function. The expression of a cursor of
 * libclang is an object of clang, so this file reads the index from that
 * object. libclang does not document where a cursor keeps the expression.
 * The object also tells if two cursors are of one expression. The same is
 * true of the function of an attribute cleanup.
 */
#include <cerrno>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>

#include "cxi_ast.h"

int
cxi_ast_generic (CXCursor  c,
                 size_t   *first,
                 size_t   *chosen)
{
	if (!first || !chosen || c.kind != CXCursor_GenericSelectionExpr)
		return EINVAL;
	/* The second data pointer of the cursor of an expression is the
	 * expression: getCursorStmt() in clang's tools/libclang/CXCursor.cpp. */
	auto const *g = llvm::dyn_cast_or_null<clang::GenericSelectionExpr>(
		static_cast<clang::Stmt const *>(c.data[1]));
	/* The result of a selection depends on a template parameter only in
	 * C++. */
	if (!g || g->isResultDependent())
		return EINVAL;
	*first = g->isExprPredicate();
	*chosen = *first + g->getResultIndex();
	return 0;
}

void const *
cxi_ast_node (CXCursor c)
{
	/* The first data pointer of the cursor of a declaration is the
	 * declaration, and that of the cursor of a macro definition is the
	 * record of the definition: getCursorDecl() and
	 * getCursorMacroDefinition() in the same file. The second one of the
	 * cursor of a statement is the statement, as of an expression. */
	if (clang_isDeclaration(c.kind) || c.kind == CXCursor_MacroDefinition)
		return c.data[0];
	return clang_isExpression(c.kind) || clang_isStatement(c.kind)
	       ? c.data[1] : nullptr;
}

CXCursor
cxi_ast_cleanup (CXCursor c)
{
	if (!clang_isAttribute(c.kind))
		return clang_getNullCursor();
	/* The second data pointer of the cursor of an attribute is the
	 * attribute, and the third one of each cursor is the translation
	 * unit: MakeCXCursor() in the same file. */
	auto const *cleanup = llvm::dyn_cast_or_null<clang::CleanupAttr>(
		static_cast<clang::Attr const *>(c.data[1]));
	clang::FunctionDecl const *fn = cleanup ? cleanup->getFunctionDecl()
	                                        : nullptr;
	if (!fn)
		return clang_getNullCursor();
	/* libclang makes the cursor of the first declaration from one of any
	 * declaration of the function. */
	return clang_getCanonicalCursor(
		CXCursor{ CXCursor_FunctionDecl, 0, { fn, nullptr, c.data[2] } });
}
