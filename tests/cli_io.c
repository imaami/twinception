/** @file Blocking CLI jobs: the blockers post each job back to the UI loop. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>

#include "cli_io.h"
#include "cli_loop.h"
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
	assert(!job->blocker.error);
	assert(work->value == 123);
	++*work->completed;
}

/* At most 32 jobs are pending, every accepted job comes back to the UI
 * loop, and the threads stop once the jobs are done.
 */
static void
cli_io_test_run (void)
{
	struct cli_loop ui = {};
	assert(!cli_loop_init(&ui));
	int e;
	struct cli_io *io = cli_io_create(&ui.loop, &e);
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
	uint64_t deadline = loop_clock() + 5000000000;
	while (completed != 32) {
		assert(loop_clock() < deadline);
		assert(!cli_loop_turn(&ui, deadline));
	}
	cli_io_stop(io);
	assert(cli_io_submit(io, &jobs[32].job) == ESHUTDOWN);
	while (!cli_io_done(io)) {
		assert(loop_clock() < deadline);
		assert(!cli_loop_turn(&ui, deadline));
	}
	cli_io_destroy(&io);
	assert(!io);
	cli_loop_fini(&ui);
}

int
main (void)
{
	cli_io_test_run();
	return 0;
}
