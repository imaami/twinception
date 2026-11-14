/** @file chatgpt.h
 *
 * Sign in with ChatGPT credentials. These functions perform blocking file
 * and network operations; serving threads must call them on a blocker.
 */
#ifndef ASKAI_SRC_CHATGPT_H_
#define ASKAI_SRC_CHATGPT_H_

#include <stddef.h>

/** Allocate the default credential path under XDG_CONFIG_HOME or HOME.
 *  Returns 0, ENOENT if neither is usable, or ENOMEM.
 */
extern int
chatgpt_path (char **path);

/** Authorize the account associated with @p path through a loopback OAuth
 *  callback. Prints the authorization URL, waits up to five minutes, and
 *  stores the validated credentials. Each path selects one account/client
 *  registration. Returns 0 or an error number; @p error receives a public
 *  diagnostic, never a credential or the server's untrusted response body.
 */
extern int
chatgpt_login (char const *path,
               char       *error,
               size_t      size);

/** Load the account at @p path, refreshing expiring credentials under a
 *  process-shared lock. Allocate an Authorization header in @p *header,
 *  which the caller frees. Returns 0 or an error number and writes a safe
 *  diagnostic to @p error. On error, @p *header is nullptr.
 */
extern int
chatgpt_header (char const *path,
                char      **header,
                char       *error,
                size_t      size);

#endif /* ASKAI_SRC_CHATGPT_H_ */
