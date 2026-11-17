/** @file cxi_ast.h
 *
 * What askai-code (src/cxi.h) reads from the syntax tree of clang because
 * libclang has no function for it. src/cxi_ast.cpp is C++ and includes the
 * headers of clang, which must be those of the libclang that askai-code
 * links.
 */
#ifndef ASKAI_SRC_CXI_AST_H_
#define ASKAI_SRC_CXI_AST_H_

#include <clang-c/Index.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Writes to @p *chosen the index of the selected association of the
 * _Generic selection @p c among the children of @p c that libclang visits.
 * Writes to @p *first the index of the first association: 1 if the
 * selection is written with a controlling expression, which is then the
 * first child, or 0 if it is written with a type. Returns 0, or EINVAL if
 * @p c is not a _Generic selection or a pointer is null.
 */
extern int
cxi_ast_generic (CXCursor  c,
                 size_t   *first,
                 size_t   *chosen);

#ifdef __cplusplus
}
#endif

#endif /* ASKAI_SRC_CXI_AST_H_ */
