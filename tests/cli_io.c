/** @file Blocking CLI jobs retain completion ownership when the UI is full. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>

#include "cli_io.h"
#include "cli_loop.h"
#include "cli_mailbox.h"
#include "compat.h"
#include "loop.h"

struct cli_io_test_job {
	struct cli_io_job job;
	size_t *completed;
	size_t value;
};

static int
cli_io_test_work (struct cli_io_job *job)
{
	struct cli_io_test_job *work = container_of(job, struct cli_io_test_job, job);
	work->value = 123;
	return 0;
}

static void
cli_io_test_done (struct cli_io_job *job)
{
	struct cli_io_test_job *work = container_of(job, struct cli_io_test_job, job);
	assert(!job->error);
	assert(work->value == 123);
	++*work->completed;
}

static void *
cli_io_test_network (void *arg)
{
	assert(!loop_exec(arg));
	return nullptr;
}

static void
cli_io_test_stop (struct loop *loop,
                  struct msg  *message)
{
	(void)message;
	loop_exit(loop);
}

static void
cli_io_test_run (struct loop *network,
                 bool         closed)
{
	struct cli_loop ui;
	assert(!cli_loop_init(&ui));
	struct cli_mailbox mailbox;
	/* No control capacity forces completion delivery through the IO
	 * owner's existing wake descriptor. Closing tests shutdown rejection. */
	assert(!cli_mailbox_init(&mailbox, &ui, (struct cli_mailbox_limits){}));
	if (closed)
		cli_mailbox_close(&mailbox);
	int e;
	struct cli_io *io = cli_io_create(network, &mailbox, &e);
	assert(io && !e);
	size_t completed = 0;
	struct cli_io_test_job jobs[33];
	for (size_t i = 0; i < array_size(jobs); ++i) {
		jobs[i] = (struct cli_io_test_job){
			.job.work = cli_io_test_work,
			.job.done = cli_io_test_done,
			.completed = &completed,
		};
		e = cli_io_submit(io, &jobs[i].job);
		assert(e == (i == 32 ? EAGAIN : 0));
	}
	uint64_t deadline = cli_loop_clock() + 5000000000;
	while (completed != 32) {
		assert(cli_loop_clock() < deadline);
		assert(!cli_loop_turn(&ui, deadline));
	}
	cli_io_stop(io);
	while (!cli_io_done(io)) {
		assert(cli_loop_clock() < deadline);
		assert(!cli_loop_turn(&ui, deadline));
	}
	cli_io_destroy(&io);
	assert(!io);
	cli_mailbox_fini(&mailbox);
	cli_loop_fini(&ui);
}

int
main (void)
{
	struct loop network = loop();
	assert(!loop_init(&network));
	pthread_t thread;
	assert(!pthread_create(&thread, nullptr, cli_io_test_network, &network));
	cli_io_test_run(&network, false);
	cli_io_test_run(&network, true);
	struct msg stop = { .fn = cli_io_test_stop };
	loop_post(&network, &stop);
	assert(!pthread_join(thread, nullptr));
	loop_fini(&network);
	return 0;
}
