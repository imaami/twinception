/** @file pragma.h
 *
 * Compiler pragma compat macros.
 */
#ifndef TWINCEPTION_SRC_PRAGMA_H_
#define TWINCEPTION_SRC_PRAGMA_H_

/** @name Pragmas
 *
 * pragma_gcc(x) is _Pragma("GCC x") with gcc and pragma_clang(x) is
 * _Pragma("clang x") with clang.  Each is empty with another compiler.
 * diag_gcc(x) and diag_clang(x) are the same for "diagnostic x", and
 * diag(x) is both, for a warning that both compilers have.
 *
 * @{
 */

#define diag(...)       diag_clang(__VA_ARGS__) diag_gcc(__VA_ARGS__)
#define diag_clang(...) pragma_clang(diagnostic __VA_ARGS__)
#define diag_gcc(...)   pragma_gcc(diagnostic __VA_ARGS__)

#ifdef __clang__
# define pragma_clang(...) ligma(clang __VA_ARGS__)
#elif defined __GNUC__
# define pragma_gcc(...)   ligma(GCC __VA_ARGS__)
#endif

#define ligma(...)  _Pragma(ligma_(__VA_ARGS__))
#define ligma_(...) # __VA_ARGS__

#ifndef pragma_clang
# define pragma_clang(...)
#endif

#ifndef pragma_gcc
# define pragma_gcc(...)
#endif

/** @} */

#endif /* TWINCEPTION_SRC_PRAGMA_H_ */
