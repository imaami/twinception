/** @file Tests of the UI loop of askai-cli: errors, turns, steps and watched
 *  descriptors.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "cli_loop.h"
#include "clk.h"
#include "compat.h"
#include "loop.h"

struct cli_loop_test {
	struct cli_loop loop;
	struct loop_step step;
	size_t turns;
	size_t steps;
	size_t released;
};

struct cli_loop_test_ref {
	struct loop_ref ref;
	struct cli_loop_test *test;
};

static void
cli_loop_test_step (struct loop      *loop,
                    struct loop_step *step)
{
	struct cli_loop_test *test = container_of(step, struct cli_loop_test, step);
	assert(cli_loop_turn(&test->loop, 0) == EBUSY);
	if (++test->steps < 3)
		loop_ready(loop, step);
}

static void
cli_loop_test_after (struct cli_loop *loop)
{
	struct cli_loop_test *test = container_of(loop, struct cli_loop_test, loop);
	assert(!loop->dispatching);
	++test->turns;
	if (test->turns == 1)
		assert(!cli_loop_turn(loop, 0));
}

static void
cli_loop_test_event (struct loop     *loop,
                     struct loop_ref *ref,
                     uint32_t         events)
{
	assert(events & EPOLLIN);
	struct cli_loop_test_ref *watch = container_of(ref, struct cli_loop_test_ref, ref);
	eventfd_t count = 0;
	assert(!eventfd_read(ref->fd, &count) && count == 1);
	loop_close(loop, ref);
	assert(ref->fd < 0 && !loop->error);
	++watch->test->released;
	free(watch);
	watch = nullptr;
}

/* loop_fail() stores the first error number in loop->error and ignores 0
 * and later errors.
 */
static void
cli_loop_test_errors (struct cli_loop_test *test)
{
	assert(!test->loop.loop.error);
	loop_fail(&test->loop.loop, 0);
	assert(!test->loop.loop.error);
	loop_fail(&test->loop.loop, EBADF);
	loop_fail(&test->loop.loop, EIO);
	loop_fail(&test->loop.loop, 0);
	assert(test->loop.loop.error == EBADF);
	assert(cli_loop_turn(&test->loop, 0) == EBADF);
	test->loop.loop.error = 0;
}

static void
cli_loop_test_turns (struct cli_loop_test *test)
{
	test->step.fn = cli_loop_test_step;
	test->loop.after_turn = cli_loop_test_after;
	loop_ready(&test->loop.loop, &test->step);
	assert(!cli_loop_turn(&test->loop, UINT64_MAX));
	assert(test->turns == 2 && test->steps == 2);
	assert(!cli_loop_turn(&test->loop, UINT64_MAX));
	assert(test->turns == 3 && test->steps == 3);
	test->loop.after_turn = nullptr;
	uint64_t due = loop_clock() + 10 * MS;
	test->loop.until = due;
	assert(!cli_loop_turn(&test->loop, UINT64_MAX));
	assert(loop_clock() >= due);
	test->loop.until = UINT64_MAX;
	for (size_t i = 0; i < 4; ++i) {
		struct cli_loop_test_ref *watch = malloc(sizeof(*watch));
		assert(watch);
		*watch = (struct cli_loop_test_ref){ .ref.fd = -1, .test = test };
		int fd = eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
		assert(fd >= 0);
		assert(!loop_watch(&test->loop.loop, &watch->ref, fd, EPOLLIN, cli_loop_test_event,
		                   nullptr));
	}
	assert(!cli_loop_turn(&test->loop, loop_clock() + NS));
	assert(test->released == 4);
}

int
main (void)
{
	struct cli_loop_test test = {};
	assert(!cli_loop_init(&test.loop));
	cli_loop_test_errors(&test);
	cli_loop_test_turns(&test);
	cli_loop_fini(&test.loop);
	return 0;
}
