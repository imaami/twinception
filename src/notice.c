/** @file notice.c
 *
 * The [askai: ...] notices and their matcher, and the checks of the text
 * of a turn.
 */
#define _GNU_SOURCE /* memmem(), memrchr() */
#include <string.h>

#include "cstr_view.h"
#include "member.h"
#include "notice.h"
#include "tool.h"
#include "util.h"

/* The notices that a turn's result can end with. */
static char const *const notice_formats[] = {
	NOTICE_NOTEBOOK, NOTICE_CALL, NOTICE_COMPACT, NOTICE_REMIND, NOTICE_LAST,
};

/* Returns the length of the argument of the printf conversion @p conv at
 * the start of the @p len bytes at @p s, or 0 if there is none. For 'u',
 * the argument is one or more digits. The only 's' conversion of the
 * notices is the member of NOTICE_NOTEBOOK, so for 's' the argument is a
 * member as member_text() writes it, NAME (KIND), up to the byte @p end,
 * the byte after the conversion in the format. The name and the kind are
 * each a name that member_name_valid() accepts.
 */
static size_t
notice_arg (char        conv,
            char        end,
            char const *s,
            size_t      len)
{
	switch (conv) {
	case 's': {
		char const *at = memchr(s, end, len);
		if (!at)
			return 0;
		struct cstr_view rest = { s, (size_t)(at - s) };
		struct cstr_view name;
		struct cstr_view kind;
		if (!cstr_view_cut(&rest, ' ', &name) || !member_name_valid(name))
			return 0;
		if (!cstr_view_cut(&rest, ')', &kind) || rest.len ||
		    !cstr_view_starts(kind, CSTR_VIEW("(")))
			return 0;
		return member_name_valid(cstr_view_slice(kind, 1, SIZE_MAX)) ? (size_t)(at - s) : 0;
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
			n = notice_arg(*p, p[1], s + i, len - i);
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

/* Returns the length of the @p len bytes at @p text without the lines at
 * the end that are notices. The kept text ends with the first line feed
 * after its last non-blank byte. It is empty if only spaces, tabs and line
 * ends come before the notices. Returns @p len if the text does not end
 * with a notice, and 0 for a null @p text.
 */
static size_t
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

/* Returns "</tool_call>" or "<tool_call>" if the @p len bytes at @p text
 * contain "</function>" and later that tag, or else nullptr. "</tool_call>"
 * is checked first. Such a text is the text argument of a tool call whose
 * text ended early. The next tool call of the turn became part of the
 * text. Returns nullptr for a null @p text.
 */
static char const *
notice_markup (char const *text,
               size_t      len)
{
	static struct cstr_view const end = CSTR_VIEW_INIT("</function>");
	static struct cstr_view const tags[] = {
		CSTR_VIEW_INIT("</tool_call>"),
		CSTR_VIEW_INIT("<tool_call>"),
	};
	char const *at = text ? memmem(text, len, end.data, end.len) : nullptr;
	if (!at)
		return nullptr;
	at += end.len;
	size_t const rest = len - (size_t)(at - text);
	for (size_t i = 0; i < array_size(tags); ++i)
		if (memmem(at, rest, tags[i].data, tags[i].len))
			return tags[i].data;
	return nullptr;
}

int
notice_text_check (struct cstr_view  text,
                   char const       *what,
                   size_t           *kept,
                   char             *why,
                   size_t            len)
{
	static_assert(TOOL_TEXT_MAX == 4U << 20, "update the refusal");
	if (!text.len || text.len > TOOL_TEXT_MAX || memchr(text.data, '\0', text.len))
		return util_refuse(why, len, "The %s argument must be a string of 1 byte to 4 MiB "
		                   "without NUL bytes.", what);
	if (!kept)
		return 0;
	*kept = notice_kept(text.data, text.len);
	if (!*kept)
		return util_refuse(why, len, "The %s contains only [askai: ...] lines. askai adds "
		                   "[askai: ...] lines to tool results. Give the %s.", what, what);
	char const *tag = notice_markup(text.data, *kept);
	if (tag)
		return util_refuse(why, len, "The %s contains </function> and then %s. The text of "
		                   "the call ended early. The next tool call became part of the %s. "
		                   "Nothing was recorded. Make the call again with the %s closed "
		                   "correctly.", what, tag, what, what);
	return 0;
}
