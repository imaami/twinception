/** @file jwt.c
 * The parts of a JSON Web Token.
 */
#include <errno.h>
#include <json-c/json.h>
#include <openssl/evp.h>

#include "buf.h"
#include "cstr_view.h"
#include "dstr.h"
#include "jb.h"
#include "jwt.h"
#include "util.h"

int
jwt_decode (struct cstr_view  input,
            struct buf       *bytes)
{
	buf_clear(bytes);
	if (!input.len || input.len > JWT_LIMIT || input.len % 4 == 1 ||
	    cstr_view_spn(input, UTIL_ALNUM "-_") != input.len)
		return EINVAL;
	dstr_auto encoded = dstr(nullptr);
	int e = cstr_view_store(input, &encoded);
	unsigned char *text = dstr_raw(&encoded);
	for (size_t i = 0; !e && i < input.len; ++i)
		if (text[i] == '-') text[i] = '+';
		else if (text[i] == '_') text[i] = '/';
	size_t pad = (4 - input.len % 4) % 4;
	if (!e)
		e = cstr_view_cat(cstr_view_n("===", pad), &encoded);
	struct span room = {};
	if (!e)
		e = buf_reserve(bytes, dstr_len(&encoded) / 4 * 3, JWT_LIMIT, &room);
	if (!e) {
		int n = EVP_DecodeBlock(room.ptr, dstr_raw(&encoded), (int)dstr_len(&encoded));
		if (n < 0)
			e = EINVAL;
		else
			buf_commit(bytes, (size_t)n - pad);
	}
	return e;
}

int
jwt_expiry (struct cstr_view  token,
            uint64_t         *exp)
{
	struct cstr_view part;
	if (!cstr_view_cut(&token, '.', &part) || !cstr_view_cut(&token, '.', &part))
		return EINVAL;

	struct buf bytes = buf();
	int e = jwt_decode(part, &bytes);
	struct span const text = buf_peek(&bytes);
	struct json_object *claims = e ? nullptr : jb_parse((char const *)text.ptr, text.len,
	                                                    JSON_TOKENER_STRICT, 16);
	if (!e && !json_object_is_type(claims, json_type_object))
		e = EINVAL;
	if (!e && jb_uint_checked(jb_get(claims, "exp"), INT64_MAX, exp))
		e = ENOENT;
	json_object_put(claims);
	buf_fini(&bytes);
	return e;
}
