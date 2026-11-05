/** @file cli_loop.c
 *
 * The loop of the UI thread of askai-cli.
 */
#include <errno.h>

#include "cli_loop.h"

int
cli_loop_init (struct cli_loop *loop)
{
	loop->after_turn = nullptr;
	loop->until = UINT64_MAX;
	loop->dispatching = 0;
	return loop_init(&loop->loop);
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
