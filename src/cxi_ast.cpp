/** @file cxi_ast.cpp
 *
 * clang knows the selected association of a _Generic selection, and
 * libclang returns it through no function. The expression of a cursor of
 * libclang is an object of clang, so this file reads the index from that
 * object. libclang does not document where a cursor keeps the expression.
 * The object also tells if two cursors are of one expression.
 */
#include <cerrno>
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
	 * declaration: getCursorDecl() in the same file. The second one of
	 * the cursor of a statement is the statement, as of an expression. */
	if (clang_isDeclaration(c.kind))
		return c.data[0];
	return clang_isExpression(c.kind) || clang_isStatement(c.kind)
	       ? c.data[1] : nullptr;
}
