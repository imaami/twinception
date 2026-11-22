/** @file chatgpt.h
 * Sign in with ChatGPT credentials. Blocking work belongs on a blocker.
 */
#ifndef ASKAI_SRC_CHATGPT_H_
#define ASKAI_SRC_CHATGPT_H_

#include <stddef.h>
#include <stdint.h>

struct dstr;

/** Replace @p path with askai's default private credential path under
 *  XDG_STATE_HOME, or HOME/.local/state. Returns 0 or an error number.
 */
extern int
chatgpt_path (struct dstr *path);

/** Authorize the account at @p path, displaying a browser URL and waiting
 *  up to five minutes for its loopback callback. Store only verified
 *  credentials. Returns 0 or an error number, with a public diagnostic
 *  in @p error; each credential path selects one account registration.
 */
extern int
chatgpt_login (char const *path,
               char       *error,
               size_t      size);

/** Replace @p header with an Authorization header for @p path, refreshing
 *  expiring credentials under a process-shared lock, and write the time
 *  at which the access token ends to @p *expires. With AUTH_ASK_FILE_ONLY in
 *  @p flags, of enum auth_flag (src/auth.h), nothing is refreshed: ENOKEY
 *  then for credentials that end within AUTH_EARLY seconds. Returns 0 or an
 *  error number, which chatgpt_why() puts in words, and leaves @p header
 *  empty and @p *expires 0 on error.
 */
extern int
chatgpt_header (char const  *path,
                uint32_t     flags,
                struct dstr *header,
                uint64_t    *expires);

/** Returns a safe diagnostic for the error @p e of chatgpt_header(): a
 *  static text, or the text of strerror().
 */
extern char const *
chatgpt_why (int e);

#endif /* ASKAI_SRC_CHATGPT_H_ */
