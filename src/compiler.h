/** @file compiler.h
 *
 * Compiler version tests.
 */
#ifndef ASKAI_SRC_COMPILER_H_
#define ASKAI_SRC_COMPILER_H_

/** @name Compiler version tests
 *
 * gcc_at_least_version(major, minor, patch) is 1 if the compiler is gcc of
 * that version or later, and gcc_older_than_version() is 1 if it is gcc of
 * an earlier version. The clang tests are the same for clang. The minor
 * and the patch number are optional and 0 by default. Each test is 0 with
 * another compiler, also the gcc tests with clang, which defines __GNUC__
 * too. A test of a range is the conjunction of two tests.
 *
 * @{
 */

#ifdef __clang__
# define v_clang() GEN_V(__clang_major__,    \
                         __clang_minor__,    \
                         __clang_patchlevel__)
# define clang_at_least_version(...)   CMP_V_(>=, clang, __VA_ARGS__)
# define clang_older_than_version(...) CMP_V_(<, clang, __VA_ARGS__)
#else
# define clang_at_least_version(...)   0
# define clang_older_than_version(...) 0
#endif

#if defined __GNUC__ && ! defined __clang__
# define v_gcc() GEN_V(__GNUC__,          \
                       __GNUC_MINOR__,    \
                       __GNUC_PATCHLEVEL__)
# define gcc_at_least_version(...)   CMP_V_(>=, gcc, __VA_ARGS__)
# define gcc_older_than_version(...) CMP_V_(<, gcc, __VA_ARGS__)
#else
# define gcc_at_least_version(...)   0
# define gcc_older_than_version(...) 0
#endif

#if defined v_clang || defined v_gcc
# define CMP_V_(op, id, ...) (v_##id() op GEN_V(__VA_ARGS__))
# define GEN_V(...) GEN_V_(__VA_ARGS__ + 0, 0, 0, )
# define GEN_V_(a, b, c, ...)     \
        ( (((a) & 0x3ffU) << 21U) \
        | (((b) & 0x3ffU) << 11U) \
        |  ((c) & 0x7ffU)         )
#endif

/** @} */

#endif /* ASKAI_SRC_COMPILER_H_ */
