/** @file tty.c
 *
 * The modes of a terminal.
 */
#include <errno.h>
#include <termios.h>

#include "tty.h"

int
tty_get (int             fd,
         struct termios *modes)
{
	while (tcgetattr(fd, modes))
		if (errno != EINTR)
			return errno;
	return 0;
}

int
tty_set (int                   fd,
         int                   when,
         struct termios const *modes)
{
	while (tcsetattr(fd, when, modes))
		if (errno != EINTR)
			return errno;
	return 0;
}
