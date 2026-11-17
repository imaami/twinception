/** @file cxi_ast.cpp
 *
 * clang knows the selected association of a _Generic selection, and
 * libclang returns it through no function. The expression of a cursor of
 * libclang is an object of clang, so this file reads the index from that
 * object. libclang does not document where a cursor keeps the expression.
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
