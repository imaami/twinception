/** @file oid.h
 *
 * Names of Git objects.
 */
#ifndef ASKAI_SRC_OID_H_
#define ASKAI_SRC_OID_H_

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "compat.h"
#include "cstr_view.h"
#include "util.h"

/** The object formats of Git, as the number of hexadecimal digits of a
 *  name.
 */
enum oid_format : uint8_t {
	OID_NONE   = 0,
	OID_SHA1   = 40,
	OID_SHA256 = 64,
};

/** The name of a Git object: OID_SHA1 or OID_SHA256 lowercase hexadecimal
 *  digits and a NUL. The empty string is no name.
 */
struct oid {
	char hex[OID_SHA256 + 1];
};

/** Returns the number of digits of @p oid: 0, OID_SHA1 or OID_SHA256. */
static inline size_t
oid_len (struct oid const *oid)
{
	return !oid->hex[0] ? 0 : oid->hex[OID_SHA1] ? OID_SHA256 : OID_SHA1;
}

/** Returns a view of the digits of @p oid. */
static inline struct cstr_view
oid_view (struct oid const *oid)
{
	return (struct cstr_view){ oid->hex, oid_len(oid) };
}

/** Returns true if @p hex is OID_SHA1 or OID_SHA256 lowercase hexadecimal
 *  digits.
 */
static inline bool
oid_valid (struct cstr_view hex)
{
	return (hex.len == OID_SHA1 || hex.len == OID_SHA256) && util_is_hex(hex.data, hex.len);
}

/** Copies the name @p hex into @p oid. Returns 0, or EINVAL with @p oid
 *  empty if oid_valid() rejects @p hex.
 */
static inline int
oid_init (struct oid       *oid,
          struct cstr_view  hex)
{
	*oid = (struct oid){};
	if (!oid_valid(hex))
		return EINVAL;
	memcpy(oid->hex, hex.data, hex.len);
	return 0;
}

/** Copies the name at the start of @p text, which the byte @p end must
 *  follow, into @p oid, and drops the name and @p end from @p text.
 *  Returns 0, or EINVAL with @p oid empty and @p text unchanged.
 */
static inline int
oid_init_field (struct oid       *oid,
                struct cstr_view *text,
                char              end)
{
	size_t n = text->len > OID_SHA1 && text->data[OID_SHA1] == end ? OID_SHA1
	         : text->len > OID_SHA256 && text->data[OID_SHA256] == end ? OID_SHA256 : 0;
	int e = oid_init(oid, cstr_view_slice(*text, 0, n));
	if (!e)
		*text = cstr_view_slice(*text, n + 1, SIZE_MAX);
	return e;
}

/** Copies the name in @p out, the output of a git command that prints one
 *  name and a line feed, into @p oid. Returns 0, or EINVAL with @p oid
 *  empty if @p out is anything else.
 */
static inline int
oid_init_line (struct oid       *oid,
               struct cstr_view  out)
{
	bool line = out.len && out.data[out.len - 1] == '\n';
	return oid_init(oid, cstr_view_slice(out, 0, line ? out.len - 1 : 0));
}

/** Returns true if @p a and @p b are the same name, or both empty. */
static inline bool
oid_equal (struct oid const *a,
           struct oid const *b)
{
	return !strcmp(a->hex, b->hex);
}

/** Returns true if the digits of @p oid start with @p prefix. */
static inline bool
oid_starts (struct oid const *oid,
            struct cstr_view  prefix)
{
	return cstr_view_starts(oid_view(oid), prefix);
}

/** Returns the object format @p name, as git rev-parse
 *  --show-object-format prints it without the line feed, or OID_NONE if
 *  @p name is neither "sha1" nor "sha256".
 */
static inline enum oid_format
oid_format (struct cstr_view name)
{
	return cstr_view_equal(name, CSTR_VIEW("sha1"))   ? OID_SHA1
	     : cstr_view_equal(name, CSTR_VIEW("sha256")) ? OID_SHA256 : OID_NONE;
}

#endif /* ASKAI_SRC_OID_H_ */
