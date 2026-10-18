/** @file CLI reactor and mailbox ownership, ordering and wakeup tests. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "cli_loop.h"
#include "cli_mailbox.h"
#include "compat.h"

struct cli_loop_test {
	struct cli_loop loop;
	struct cli_mailbox mailbox;
	struct cli_loop_step step;
	size_t received[4];
	size_t turns;
	size_t steps;
	size_t released;
};

struct cli_loop_test_msg {
	struct cli_mailbox_msg msg;
	size_t producer;
	size_t sequence;
};

struct cli_loop_test_producer {
	struct cli_mailbox *mailbox;
	size_t number;
};

struct cli_loop_test_ref {
	struct cli_loop_ref ref;
	struct cli_loop_test *test;
};

static void
cli_loop_test_receive (struct cli_mailbox     *mailbox,
                      struct cli_mailbox_msg *msg)
{
	struct cli_loop_test *test = container_of(mailbox, struct cli_loop_test, mailbox);
	struct cli_loop_test_msg *message = container_of(msg, struct cli_loop_test_msg, msg);
	assert(message->sequence == test->received[message->producer]++);
	free(message);
	message = nullptr;
}

static struct cli_loop_test_msg *
cli_loop_test_message (size_t producer,
                      size_t sequence)
{
	struct cli_loop_test_msg *message = malloc(sizeof(*message));
	assert(message);
	*message = (struct cli_loop_test_msg){
		.msg.fn = cli_loop_test_receive,
		.msg.bytes = sizeof(*message),
		.producer = producer,
		.sequence = sequence,
	};
	return message;
}

static void *
cli_loop_test_produce (void *arg)
{
	struct cli_loop_test_producer *producer = arg;
	for (size_t sequence = 0; sequence < 2000; ++sequence) {
		struct cli_loop_test_msg *message = cli_loop_test_message(producer->number, sequence);
		int e;
		do {
			e = cli_mailbox_post(producer->mailbox, &message->msg, false);
			if (e == EAGAIN)
				sched_yield();
		} while (e == EAGAIN);
		assert(!e);
	}
	return nullptr;
}

static void
cli_loop_test_step (struct cli_loop      *loop,
                    struct cli_loop_step *step)
{
	struct cli_loop_test *test = container_of(step, struct cli_loop_test, step);
	assert(cli_loop_turn(loop, 0) == EBUSY);
	if (++test->steps < 3)
		cli_loop_ready(loop, step);
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
cli_loop_test_released (struct cli_loop_ref *ref)
{
	struct cli_loop_test_ref *watch = container_of(ref, struct cli_loop_test_ref, ref);
	++watch->test->released;
	free(watch);
	watch = nullptr;
}

static void
cli_loop_test_event (struct cli_loop     *loop,
                     struct cli_loop_ref *ref,
                     uint32_t             events)
{
	assert(events & EPOLLIN);
	assert(!close(ref->fd));
	cli_loop_release(loop, ref, cli_loop_test_released);
}

static void
cli_loop_test_turns (struct cli_loop_test *test)
{
	test->step.fn = cli_loop_test_step;
	test->loop.after_turn = cli_loop_test_after;
	cli_loop_ready(&test->loop, &test->step);
	assert(!cli_loop_turn(&test->loop, UINT64_MAX));
	assert(test->turns == 2 && test->steps == 2);
	assert(!cli_loop_turn(&test->loop, UINT64_MAX));
	assert(test->turns == 3 && test->steps == 3);
	test->loop.after_turn = nullptr;
	uint64_t due = cli_loop_clock() + 10000000;
	test->loop.until = due;
	assert(!cli_loop_turn(&test->loop, UINT64_MAX));
	assert(cli_loop_clock() >= due);
	test->loop.until = UINT64_MAX;
	for (size_t i = 0; i < 4; ++i) {
		struct cli_loop_test_ref *watch = malloc(sizeof(*watch));
		assert(watch);
		*watch = (struct cli_loop_test_ref){ .ref.fd = -1, .test = test };
		int fd = eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
		assert(fd >= 0);
		assert(!cli_loop_watch(&test->loop, &watch->ref, fd, EPOLLIN, cli_loop_test_event));
	}
	assert(!cli_loop_turn(&test->loop, cli_loop_clock() + 1000000000));
	assert(test->released == 4);
}

static void
cli_loop_test_limits (struct cli_loop_test *test)
{
	struct cli_mailbox_limits limits = {
		.messages = 1,
		.bytes = sizeof(struct cli_loop_test_msg),
		.control_messages = 1,
		.control_bytes = sizeof(struct cli_loop_test_msg),
	};
	assert(!cli_mailbox_init(&test->mailbox, &test->loop, limits));
	struct cli_loop_test_msg *first = cli_loop_test_message(0, 0);
	struct cli_loop_test_msg *next = cli_loop_test_message(0, 1);
	assert(!cli_mailbox_post(&test->mailbox, &first->msg, false));
	assert(cli_mailbox_post(&test->mailbox, &next->msg, false) == EAGAIN);
	assert(!cli_mailbox_post(&test->mailbox, &next->msg, true));
	assert(!cli_loop_turn(&test->loop, UINT64_MAX));
	assert(test->received[0] == 2);
	cli_mailbox_close(&test->mailbox);
	next = cli_loop_test_message(0, 2);
	assert(cli_mailbox_post(&test->mailbox, &next->msg, true) == ESHUTDOWN);
	free(next);
	next = nullptr;
	cli_mailbox_fini(&test->mailbox);
	test->received[0] = 0;
}

static void
cli_loop_test_threads (struct cli_loop_test *test)
{
	struct cli_mailbox_limits limits = {
		.messages = 64,
		.bytes = 64 * sizeof(struct cli_loop_test_msg),
		.control_messages = 1,
		.control_bytes = sizeof(struct cli_loop_test_msg),
	};
	assert(!cli_mailbox_init(&test->mailbox, &test->loop, limits));
	pthread_t threads[4];
	struct cli_loop_test_producer producers[4];
	for (size_t i = 0; i < array_size(threads); ++i) {
		producers[i] = (struct cli_loop_test_producer){ &test->mailbox, i };
		assert(!pthread_create(&threads[i], nullptr, cli_loop_test_produce, &producers[i]));
	}
	uint64_t deadline = cli_loop_clock() + 10000000000;
	for (;;) {
		size_t received = 0;
		for (size_t i = 0; i < array_size(test->received); ++i)
			received += test->received[i];
		if (received == 8000)
			break;
		assert(cli_loop_clock() < deadline);
		assert(!cli_loop_turn(&test->loop, deadline));
	}
	for (size_t i = 0; i < array_size(threads); ++i)
		assert(!pthread_join(threads[i], nullptr));
	cli_mailbox_fini(&test->mailbox);
}

int
main (void)
{
	struct cli_loop_test test = {};
	assert(!cli_loop_init(&test.loop));
	cli_loop_test_turns(&test);
	cli_loop_test_limits(&test);
	cli_loop_test_threads(&test);
	cli_loop_fini(&test.loop);
	return 0;
}
