/** @file tty.h
 *
 * The modes of a terminal. Both functions return an error number, and
 * call again after a signal, so that a caller needs no errno and no loop.
 */
#ifndef ASKAI_SRC_TTY_H_
#define ASKAI_SRC_TTY_H_

#include <termios.h>

/** Reads the modes of the terminal @p fd into @p *modes. Returns 0, or the
 *  error number of tcgetattr(), such as ENOTTY for a file that is no
 *  terminal.
 */
extern int
tty_get (int             fd,
         struct termios *modes);

/** Sets the modes of the terminal @p fd to @p *modes. @p when is TCSANOW,
 *  TCSADRAIN or TCSAFLUSH. Returns 0, or the error number of tcsetattr().
 */
extern int
tty_set (int                   fd,
         int                   when,
         struct termios const *modes);

#endif /* ASKAI_SRC_TTY_H_ */
