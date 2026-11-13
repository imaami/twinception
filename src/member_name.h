/** @file member_name.h
 *
 * The value of a member of the session. A member is of one kind, with one
 * name. The registry of the members, the logins and the branches are in
 * src/member.h.
 */
#ifndef ASKAI_SRC_MEMBER_NAME_H_
#define ASKAI_SRC_MEMBER_NAME_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "compat.h"
#include "cstr_view.h"
#include "util.h"
#include "xlist.h"

enum {
	MEMBER_NAME = UTIL_WORD_MAX + 1,  /**< bytes of a member name, NUL
	                                       included */
};

/** The kinds of members: X(ID, name), the constant MEMBER_ID of enum
 *  member_kind and the name in the trailer line Kind of a record and in the
 *  refs of the branches. A person is a human or a service account, and an
 *  agent is an AI member.
 */
#define MEMBER_KIND_LIST(X) \
        X(PERSON, "person") \
        X(AGENT,  "agent")

enum member_kind : uint8_t {
#define MEMBER_KIND_ENUM(id, ...) MEMBER_##id,
	MEMBER_KIND_LIST(MEMBER_KIND_ENUM)
#undef MEMBER_KIND_ENUM
	MEMBER_KINDS
};

/** The names of the kinds. Each is NUL-terminated. */
static struct cstr_view const member_kinds[MEMBER_KINDS] = { MEMBER_KIND_LIST(XL_VIEW) };

/** A member of the session. */
struct member {
	enum member_kind kind;
	char             name[MEMBER_NAME];  /**< "" for no member */
};

/**
 * Returns true if @p name can be the name of a member: 1 to UTIL_WORD_MAX
 * characters from [A-Za-z0-9_.@-], not starting with . or -, without ..,
 * not ending with . or .lock, not @, and not a letter followed only by
 * digits, such as c7. The name of a service of askai is of the same
 * form.
 */
static inline bool
member_name_valid (struct cstr_view name)
{
	if (!name.data || !name.len || name.len > UTIL_WORD_MAX || *name.data == '.' ||
	    *name.data == '-' || name.data[name.len - 1] == '.' ||
	    cstr_view_ends(name, CSTR_VIEW(".lock")) || cstr_view_equal(name, CSTR_VIEW("@")))
		return false;
	bool digits = name.len > 1;
	for (size_t i = 0; i < name.len; ++i) {
		char const c = name.data[i];
		bool const digit = c >= '0' && c <= '9';
		bool const letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		if (!digit && !letter && c != '_' && c != '.' && c != '@' && c != '-')
			return false;
		if (c == '.' && i && name.data[i - 1] == '.')
			return false;
		/* The first character is a letter, and the others are digits. */
		if (i ? !digit : !letter)
			digits = false;
	}
	return !digits;
}

/**
 * Writes the name @p name to the MEMBER_NAME bytes at @p dest, and 0 to
 * every byte of @p dest after the name. Code copies a name as the whole
 * array, so the function writes every byte of the array. The caller
 * checks the name with member_name_valid() first. Of a longer name, the
 * function writes only the first UTIL_WORD_MAX bytes. For an empty view,
 * the name is "".
 */
static inline void
member_name_set (char             dest[static MEMBER_NAME],
                 struct cstr_view name)
{
	size_t const len = name.len < MEMBER_NAME ? name.len : MEMBER_NAME - 1;
	if (len)
		memcpy(dest, name.data, len);
	memset(dest + len, 0, MEMBER_NAME - len);
}

/** Returns the member of the kind @p kind with the name @p name. The name
 *  is written as member_name_set() writes it.
 */
static force_inline struct member
member (enum member_kind kind,
        struct cstr_view name)
{
	struct member m = { .kind = kind };
	member_name_set(m.name, name);
	return m;
}

#endif /* ASKAI_SRC_MEMBER_NAME_H_ */
