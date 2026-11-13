/** @file hexid.h
 *
 * Random ids of 128 bits, as lowercase hexadecimal digits: the ids of
 * ledger records, jobs, journals, threads of notes and MCP sessions.
 */
#ifndef ASKAI_SRC_HEXID_H_
#define ASKAI_SRC_HEXID_H_

#include <errno.h>
#include <string.h>

#include "compat.h"
#include "cstr_view.h"
#include "util.h"

/** The digits of an id. */
enum { HEXID_DIGITS = 32 };

/** An id: HEXID_DIGITS lowercase hexadecimal digits and a NUL. The empty
 *  string is no id.
 */
struct hexid {
	char hex[HEXID_DIGITS + 1];
};

/** Returns true if @p hex is HEXID_DIGITS lowercase hexadecimal digits. */
static inline bool
hexid_valid (struct cstr_view hex)
{
	return hex.len == HEXID_DIGITS && util_is_hex(hex.data, hex.len);
}

/** Makes a new random id in @p id. Returns 0, or the error number of
 *  util_random() with no id in @p id.
 */
static inline int
hexid_make (struct hexid *id)
{
	unsigned char bits[HEXID_DIGITS / 2];
	*id = (struct hexid){};
	int e = util_random(bits, sizeof bits);
	if (!e)
		util_hex(id->hex, bits, sizeof bits);
	return e;
}

/** Copies the id @p hex into @p id. Returns 0, or EINVAL with no id in
 *  @p id if hexid_valid() rejects @p hex.
 */
static inline int
hexid_init (struct hexid     *id,
            struct cstr_view  hex)
{
	*id = (struct hexid){};
	if (!hexid_valid(hex))
		return EINVAL;
	memcpy(id->hex, hex.data, hex.len);
	return 0;
}

/** Returns the id @p hex and stores 0 in @p *e. If hexid_valid() rejects
 *  @p hex, the function returns no id and stores EINVAL in @p *e.
 */
static force_inline struct hexid
hexid (struct cstr_view  hex,
       int              *e)
{
	struct hexid id = {};
	*e = hexid_init(&id, hex);
	return id;
}

/** Returns a view of the digits of @p id, which is empty for no id. */
static inline struct cstr_view
hexid_view (struct hexid const *id)
{
	return cstr_view_n(id->hex, *id->hex ? HEXID_DIGITS : 0);
}

/** Returns true if @p a and @p b are the same id, or both no id. */
static inline bool
hexid_equal (struct hexid const *a,
             struct hexid const *b)
{
	return !strcmp(a->hex, b->hex);
}

#endif /* ASKAI_SRC_HEXID_H_ */
