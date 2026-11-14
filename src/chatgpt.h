/** @file chatgpt.h
 * Sign in with ChatGPT credentials. Blocking work belongs on a blocker.
 */
#ifndef ASKAI_SRC_CHATGPT_H_
#define ASKAI_SRC_CHATGPT_H_

#include <stddef.h>

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
 *  expiring credentials under a process-shared lock. Returns 0 or an error
 *  number, leaves @p header empty on error, and writes a safe diagnostic.
 */
extern int
chatgpt_header (char const  *path,
                struct dstr *header,
                char        *error,
                size_t       size);

#endif /* ASKAI_SRC_CHATGPT_H_ */
