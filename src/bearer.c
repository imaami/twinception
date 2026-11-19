/** @file bearer.c
 * The token of --auth-command and its file.
 */
#include <errno.h>
#include <json-c/json.h>
#include <string.h>

#include "bearer.h"
#include "buf.h"
#include "child.h"
#include "clk.h"
#include "cred.h"
#include "cstr_view.h"
#include "dstr.h"
#include "jb.h"
#include "jwt.h"
#include "loop.h"
#include "util.h"

enum {
	BEARER_WAIT  = 300,           /* the seconds that the command may run */
	BEARER_EARLY = 60,            /* a token is replaced this many seconds
	                                 before it expires */
	BEARER_OUT   = 4 * JWT_LIMIT, /* the bytes of output that are read */
};

extern char **environ;

int
bearer_path (struct dstr *path)
{
	char const *base;
	char const *rest;
	int const e = util_state_path(&base, &rest);
	dstr_clear(path);
	return e ? e : dstr_printf(path, "%s%s/bearer.json", base, rest) ? 0 : ENOMEM;
}

/* Returns the token of the record of @p file, or an empty view. */
static struct cstr_view
bearer_token (struct cred const *file)
{
	return jb_text(jb_get(file->record, "token"), 1, JWT_LIMIT, JWT_SET ".");
}

/* Runs @p command and replaces the record of @p file with the token of its
 * output and the time of the exp claim. @p now is the time in seconds.
 * Returns 0, ENOMSG if the output contains no token, ESTALE if the token
 * has expired, the error of jwt_expiry(), of child_cmd_run() or of
 * cred_save(), or ENOMEM.
 */
static int
bearer_renew (struct cred *file,
              char const  *command,
              uint64_t     now)
{
	char const *const argv[] = { "sh", "-c", command, nullptr };
	struct child_cmd const cmd = child_cmd(
		.file     = "/bin/sh",
		.argv     = argv,
		.envp     = environ,
		.max      = BEARER_OUT,
		.deadline = loop_clock() + BEARER_WAIT * NS,
		.grace    = NS,
	);
	struct buf out = buf();
	struct child_end end;
	int e = child_cmd_run(&cmd, child_fds(), &out, &end);
	struct cstr_view const token = e ? cstr_view(nullptr)
	                                 : jwt_find(cstr_view_span(buf_peek(&out)));
	uint64_t exp = 0;
	if (!e)
		e = token.data ? jwt_expiry(token, &exp) : ENOMSG;
	if (!e && exp <= now)
		e = ESTALE;
	struct json_object *record = e ? nullptr : json_object_new_object();
	if (!e)
		e = jb_set(record, "token", json_object_new_string_len(token.data, (int)token.len));
	if (!e)
		e = jb_set(record, "expires_at", json_object_new_uint64(exp));
	if (!e)
		e = cred_save(file, record);
	if (e) {
		json_object_put(record);
	} else {
		json_object_put(file->record);
		file->record = record;
	}
	buf_wipe(&out);
	buf_fini(&out);
	return e;
}

/* Writes words for the user about the error @p e of a token to the
 * @p size bytes at @p error, or an empty string for 0. Returns @p e.
 */
static int
bearer_say (int     e,
            char   *error,
            size_t  size)
{
	static char const *const words[] = {
		[EBUSY]     = "the token file is in use by another process; try again",
		[EACCES]    = "the token file or its directory is not private to its owner",
		[ENOMSG]    = "the output of the auth command contains no JSON Web Token",
		[ESTALE]    = "the token of the auth command has expired",
		[ENOENT]    = "the token of the auth command has no exp claim",
		[EINVAL]    = "the token of the auth command or the token file is malformed",
		[ETIMEDOUT] = "the auth command did not end in time",
		[EIO]       = "the auth command failed",
		[EFBIG]     = "the auth command wrote too much",
	};
	char const *known = e > 0 && (size_t)e < array_size(words) ? words[e] : nullptr;
	char const *text = !e ? "" : known ? known : strerror(e);
	if (size) {
		size_t const n = strnlen(text, size - 1);
		memcpy(error, text, n);
		error[n] = '\0';
	}
	return e;
}

int
bearer_login (char const *path,
              char const *command,
              char       *error,
              size_t      size)
{
	int e;
	struct cred file = cred(path, CRED_CREATE, &e);
	uint64_t const now = clk_realtime_ns() / NS;
	if (!e)
		e = now ? bearer_renew(&file, command, now) : EIO;
	cred_fini(&file);
	return bearer_say(e, error, size);
}

int
bearer_header (char const  *path,
               char const  *command,
               struct dstr *header,
               char        *error,
               size_t       size)
{
	dstr_clear(header);
	int e;
	struct cred file = cred(path, CRED_CREATE, &e);
	uint64_t const now = clk_realtime_ns() / NS;
	if (!e && !now)
		e = EIO;
	if (!e && (!bearer_token(&file).len ||
	           jb_uint(jb_get(file.record, "expires_at"), 0) <= now + BEARER_EARLY))
		e = bearer_renew(&file, command, now);
	if (!e && !dstr_printf(header, "Authorization: Bearer %s", bearer_token(&file).data))
		e = ENOMEM;
	cred_fini(&file);
	if (e)
		dstr_clear(header);
	return bearer_say(e, error, size);
}
