/** @file xlist.h
 *
 * Expansions for lists of rows (X-macros), and XL_JOIN().
 *
 * A list is a macro NAME_LIST(X) whose rows are X(...). A comment above
 * the list names its columns. In a list of names, the first two columns
 * are ID, the part of an enum constant after its prefix, and name, a
 * string literal. An expansion of a list names the columns that it uses
 * and takes the rest as "...". It is defined right before its use and
 * undefined right after it. This header adds no symbol, so a plugin can
 * include it.
 */
#ifndef ASKAI_SRC_XLIST_H_
#define ASKAI_SRC_XLIST_H_

/* The name of a row X(ID, name, ...) as a struct cstr_view initializer
 * (cstr_view.h), as text for XL_JOIN(), and as a JSON string for
 * XL_JOIN().
 */
#define XL_VIEW(id, name, ...) CSTR_VIEW_INIT(name),
#define XL_TEXT(id, name, ...) name,
#define XL_JSON(id, name, ...) "\"" name "\"",

/* XL_JOIN(sep, last, a, b, ..., z,) is one string literal: the string
 * literals a to z, with sep between two of them and last before z. Each
 * literal is followed by a comma, as XL_TEXT and XL_JSON write them. There
 * are at most 16 literals, and a 17th is a compile error.
 */
#define XL_JOIN(sep, last, ...)                                         \
        XL_JOIN_N(__VA_ARGS__ XL_J16, XL_J15, XL_J14, XL_J13, XL_J12,   \
                  XL_J11, XL_J10, XL_J9, XL_J8, XL_J7, XL_J6, XL_J5,    \
                  XL_J4, XL_J3, XL_J2, XL_J1, XL_J0,)(sep, last, __VA_ARGS__)
#define XL_JOIN_N(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, x, ...) x
#define XL_J0(s, l, ...)
#define XL_J1(s, l, a, ...)  a
#define XL_J2(s, l, a, ...)  a l XL_J1(s, l, __VA_ARGS__)
#define XL_J3(s, l, a, ...)  a s XL_J2(s, l, __VA_ARGS__)
#define XL_J4(s, l, a, ...)  a s XL_J3(s, l, __VA_ARGS__)
#define XL_J5(s, l, a, ...)  a s XL_J4(s, l, __VA_ARGS__)
#define XL_J6(s, l, a, ...)  a s XL_J5(s, l, __VA_ARGS__)
#define XL_J7(s, l, a, ...)  a s XL_J6(s, l, __VA_ARGS__)
#define XL_J8(s, l, a, ...)  a s XL_J7(s, l, __VA_ARGS__)
#define XL_J9(s, l, a, ...)  a s XL_J8(s, l, __VA_ARGS__)
#define XL_J10(s, l, a, ...) a s XL_J9(s, l, __VA_ARGS__)
#define XL_J11(s, l, a, ...) a s XL_J10(s, l, __VA_ARGS__)
#define XL_J12(s, l, a, ...) a s XL_J11(s, l, __VA_ARGS__)
#define XL_J13(s, l, a, ...) a s XL_J12(s, l, __VA_ARGS__)
#define XL_J14(s, l, a, ...) a s XL_J13(s, l, __VA_ARGS__)
#define XL_J15(s, l, a, ...) a s XL_J14(s, l, __VA_ARGS__)
#define XL_J16(s, l, a, ...) a s XL_J15(s, l, __VA_ARGS__)

#endif /* ASKAI_SRC_XLIST_H_ */
