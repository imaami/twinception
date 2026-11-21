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

#include <stddef.h>

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
 * it ends, and can open /dev/tty. Returns 0 or an error number, with words
 * for the user in the @p size bytes at @p error.
 */
extern int
bearer_login (char const *path,
              char const *command,
              char       *error,
              size_t      size);

/**
 * Replaces @p header with the Authorization header of the token in the
 * file @p path. A token that is missing, that is of another command, or
 * that expires within a minute, is first replaced as bearer_login() stores
 * one, but the command never gets a terminal. Returns 0 or an error
 * number, with @p header empty and words for the user in the @p size bytes
 * at @p error.
 */
extern int
bearer_header (char const  *path,
               char const  *command,
               struct dstr *header,
               char        *error,
               size_t       size);

#endif /* ASKAI_SRC_BEARER_H_ */
