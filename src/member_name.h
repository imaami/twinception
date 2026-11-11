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

#endif /* ASKAI_SRC_MEMBER_NAME_H_ */
