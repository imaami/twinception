/** @file cli_loop.c
 *
 * The loop of the UI thread of askai-cli.
 */
#include <errno.h>
#include <unistd.h>

#include "cli_loop.h"

int
cli_loop_ref_counter (struct loop_ref const *ref,
                      uint64_t              *count)
{
	*count = 0;
	uint64_t value;
	ssize_t n;
	do {
		n = read(ref->fd, &value, sizeof value);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return errno == EAGAIN ? 0 : errno;
	if (n != (ssize_t)sizeof value)
		return EIO;
	*count = value;
	return 0;
}

int
cli_loop_init (struct cli_loop *loop)
{
	loop->after_turn = nullptr;
	loop->until = UINT64_MAX;
	loop->dispatching = 0;
	int e = loop_init(&loop->loop);
	if (e)
		loop_fini(&loop->loop);
	return e;
}

void
cli_loop_close (struct cli_loop *loop,
                struct loop_ref *ref)
{
	int fd = ref->fd;
	if (fd < 0)
		return;
	loop_unwatch(&loop->loop, ref);
	(void)close(fd);
}

int
cli_loop_turn (struct cli_loop *loop,
               uint64_t         deadline)
{
	if (loop->dispatching)
		return EBUSY;
	if (loop->loop.error)
		return loop->loop.error;
	loop->dispatching = 1;
	int e = loop_turn(&loop->loop, deadline < loop->until ? deadline : loop->until);
	loop->dispatching = 0;
	if (e)
		return e;
	if (!loop->loop.error && loop->after_turn)
		loop->after_turn(loop);
	return loop->loop.error;
}

void
cli_loop_fini (struct cli_loop *loop)
{
	loop_fini(&loop->loop);
	loop->after_turn = nullptr;
}
