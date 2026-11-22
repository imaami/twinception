/** @file bearer.h
 * A bearer token from a command. askai runs the command of --auth-command
 * when it has no token of that command that is still valid, takes the JSON
 * Web Token from the output, and keeps it with the command in a private
 * file until a minute before the time of its exp claim. The signature of
 * the token is not checked: the upstream does that. Blocking work belongs
 * on a blocker.
 */
#ifndef ASKAI_SRC_BEARER_H_
#define ASKAI_SRC_BEARER_H_

#include <stdint.h>

struct dstr;

/** Replaces @p path with the default path of the token file, under
 *  XDG_STATE_HOME, or HOME/.local/state. Returns 0 or an error number.
 */
extern int
bearer_path (struct dstr *path);

/**
 * Runs @p command with /bin/sh, up to five minutes, and stores the token
 * of its output in the file @p path. The command runs with the environment
 * of askai, its standard input and its standard error /dev/null. If askai
 * is the foreground process group of a terminal, the command is that until
 * it ends, and can open /dev/tty (struct child_fds). Returns 0 or an error
 * number, which bearer_why() puts in words.
 */
extern int
bearer_login (char const *path,
              char const *command);

/**
 * Replaces @p header with the Authorization line of the token in the file
 * @p path, and writes the time of the exp claim of the token to
 * @p *expires. A token that is missing, that is of another command, or
 * that expires within AUTH_EARLY seconds, is first replaced as
 * bearer_login() stores one, but the command never gets a terminal. With
 * AUTH_ASK_FILE_ONLY in @p flags, of enum auth_flag (src/auth.h), the command
 * does not run and the file is not made: the function then returns ENOKEY,
 * or ENOENT without the file. Returns 0 or an error number, which
 * bearer_why() puts in words, with @p header empty and @p *expires 0.
 */
extern int
bearer_header (char const  *path,
               char const  *command,
               uint32_t     flags,
               struct dstr *header,
               uint64_t    *expires);

/** Returns words for the user about the error @p e of bearer_login() or
 *  bearer_header(): a static text, or the text of strerror().
 */
extern char const *
bearer_why (int e);

#endif /* ASKAI_SRC_BEARER_H_ */
