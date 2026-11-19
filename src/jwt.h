/** @file jwt.h
 * JSON Web Tokens as text: where a token is in a text, and when it
 * expires. Nothing here checks a signature.
 */
#ifndef ASKAI_SRC_JWT_H_
#define ASKAI_SRC_JWT_H_

#include <stdint.h>
#include <string.h>

#include "cstr_view.h"
#include "util.h"

struct buf;

enum { JWT_LIMIT = 64 << 10 };  /**< the bytes of the longest token */

/** The characters of base64url. */
#define JWT_SET UTIL_ALNUM "-_"

/**
 * Returns the first JSON Web Token in @p text, or an empty view with data
 * nullptr. A token is a whole run of base64url characters and periods:
 * three parts with a period between them, of which the first starts with
 * "eyJ", as the encoding of a JSON object does. A run of more than
 * JWT_LIMIT bytes is no token.
 */
static inline struct cstr_view
jwt_find (struct cstr_view text)
{
	while (text.len) {
		struct cstr_view rest = cstr_view_n(text.data, cstr_view_spn(text, JWT_SET "."));
		struct cstr_view const run = rest;
		struct cstr_view part;
		if (run.len <= JWT_LIMIT && cstr_view_starts(run, CSTR_VIEW("eyJ")) &&
		    cstr_view_cut(&rest, '.', &part) && cstr_view_cut(&rest, '.', &part) &&
		    part.len && rest.len && !memchr(rest.data, '.', rest.len))
			return run;

		/* Past the run, or past a byte of no run. */
		text = cstr_view_slice(text, run.len + !run.len, SIZE_MAX);
	}
	return cstr_view(nullptr);
}

/**
 * Replaces the content of @p bytes with the bytes that @p input encodes as
 * base64url without padding, as a part of a token is encoded. Returns 0,
 * EINVAL if @p input is empty, longer than JWT_LIMIT or no such encoding,
 * or ENOMEM.
 */
extern int
jwt_decode (struct cstr_view  input,
            struct buf       *bytes);

/**
 * Writes to @p *exp the time of the exp claim of @p token, in seconds
 * since the epoch. Returns 0, EINVAL if the second part of @p token does
 * not encode a JSON object, ENOENT if the object has no exp claim that is
 * an integer, or ENOMEM.
 */
extern int
jwt_expiry (struct cstr_view  token,
            uint64_t         *exp);

#endif /* ASKAI_SRC_JWT_H_ */
