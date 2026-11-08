/** @file notice.c
 *
 * The [askai: ...] notices and their matcher.
 */
#define _GNU_SOURCE /* memrchr() */
#include <string.h>

#include "cstr_view.h"
#include "member.h"
#include "notice.h"

/* The notices that a turn's result can end with. */
static char const *const notice_formats[] = {
	NOTICE_NOTEBOOK, NOTICE_COMPACT, NOTICE_REMIND, NOTICE_LAST,
};

/* Returns the length of the argument of the printf conversion @p conv at
 * the start of the @p len bytes at @p s, or 0 if there is none: one or
 * more digits for 'u', and for 's' the name of a member and its kind in
 * parentheses, such as "bro (agent)", up to the next ']'.
 */
static size_t
notice_arg (char        conv,
            char const *s,
            size_t      len)
{
	switch (conv) {
	case 's': {
		char const *end = memchr(s, ']', len);
		if (!end)
			return 0;
		size_t const n = (size_t)(end - s);
		struct cstr_view kind = { s, n }, name;
		if (!cstr_view_cut(&kind, ' ', &name) || !member_name_valid(name))
			return 0;
		if (!cstr_view_starts(kind, CSTR_VIEW("(")) || !cstr_view_ends(kind, CSTR_VIEW(")")))
			return 0;
		struct cstr_view const word = cstr_view_slice(kind, 1, kind.len - 2);
		return cstr_view_index(member_kinds, array_size(member_kinds), word) <
		       array_size(member_kinds) ? n : 0;
	}
	case 'u': {
		size_t n = 0;
		while (n < len && s[n] >= '0' && s[n] <= '9')
			++n;
		return n;
	}
	default:
		return 0;
	}
}

/* Returns true if the @p len bytes at @p s are a notice of the printf
 * format @p pattern. Its other bytes match one for one, "%%" matches '%',
 * and a conversion matches what notice_arg() takes for it.
 */
static bool
notice_is (char const *pattern,
           char const *s,
           size_t      len)
{
	size_t i = 0;
	for (char const *p = pattern; *p; ++p) {
		size_t n;
		if (*p == '%' && p[1] != '%') {
			p += 1 + strspn(p + 1, "hljzt");
			n = notice_arg(*p, s + i, len - i);
		} else {
			p += *p == '%';
			n = i < len && s[i] == *p;
		}
		if (!n)
			return false;
		i += n;
	}
	return i == len;
}

size_t
notice_kept (char const *text,
             size_t      len)
{
	if (!text)
		return 0;
	size_t kept = len;
	size_t end = len;
	for (;;) {
		end = cstr_view_trim_end((struct cstr_view){ text, end }).len;
		char const *nl = memrchr(text, '\n', end);
		size_t line = nl ? (size_t)(nl - text) + 1 : 0;
		bool notice = false;
		for (size_t i = 0; !notice && i < array_size(notice_formats); ++i)
			notice = notice_is(notice_formats[i], text + line, end - line);
		if (!notice)
			break;
		kept = end = line;
	}
	/* A removed line starts after a line feed, or at the start. */
	return kept == len ? len : !end ? 0
	     : (size_t)((char const *)memchr(text + end, '\n', kept - end) - text) + 1;
}
