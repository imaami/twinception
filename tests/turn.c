/** @file These unit tests check the order in which a turn of the event
 *  loop (src/loop.c) runs the steps on its ready lists.  The test includes
 *  src/loop.c and replaces clock_gettime() with a fake clock.  Only the
 *  handlers and steps of the test advance the fake clock, so the results do
 *  not depend on the speed or the load of the machine. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* clock_ns is the time of the fake clock in nanoseconds. */
static uint64_t clock_ns;

/* This function replaces clock_gettime() in src/loop.c.  It ignores @p id
 * and stores the time clock_ns in @p ts. */
static int
turn_clock_gettime (clockid_t        id,
                    struct timespec *ts)
{
	(void)id;
	*ts = (struct timespec){
		.tv_sec = (time_t)(clock_ns / 1'000'000'000),
		.tv_nsec = (long)(clock_ns % 1'000'000'000)
	};
	return 0;
}

#define clock_gettime turn_clock_gettime
#include "loop.c"
#undef clock_gettime

static unsigned failures;

#define CHECK(cond) \
        ((cond) ? (void)0 \
                : (void)(++failures, fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                                             __FILE__, __LINE__, #cond)))

/*
 * The variable t stores the state of one run.  In each turn, the handler of
 * the message mail advances the clock by mail_ns, queues the entry fresh and
 * posts the message again.  The handler runs outside of the steps, so
 * loop_ready() appends the entry fresh to the fresh list.  The step of the
 * entry ongoing advances the clock by ongoing_ns and queues the entry again.
 * A step calls loop_ready() there, so loop_ready() appends the entry to the
 * ongoing list.  The step of the entry bulk counts the turns and ends the
 * loop after the last turn.
 */
static struct {
	struct loop_step fresh;
	struct loop_step ongoing;
	struct loop_step bulk;
	struct msg       mail;
	uint64_t         mail_ns;        /* the time in ns that the handler of
	                                    mail adds to the clock */
	uint64_t         fresh_ns;       /* the time in ns that a step of fresh
	                                    adds to the clock */
	uint64_t         ongoing_ns;     /* the time in ns that a step of
	                                    ongoing adds to the clock */
	uint64_t         fresh_steps;    /* the number of steps of fresh */
	uint64_t         ongoing_steps;  /* the number of steps of ongoing */
	uint64_t         turns;          /* the number of steps of bulk, one in
	                                    each turn */
	uint64_t         last;           /* the number of turns to run */
} t;

static void
turn_mail (struct loop *loop,
           struct msg  *msg)
{
	/* loop_fini() sets the exit flag and then calls this handler for the
	 * message that is still posted.  In that case the handler must not post
	 * the message again. */
	if (loop->exit)
		return;
	clock_ns += t.mail_ns;
	loop_ready(loop, &t.fresh);
	CHECK(!loop_post(loop, msg));
}

static void
turn_fresh (struct loop      *loop,
            struct loop_step *step)
{
	(void)loop;
	(void)step;
	++t.fresh_steps;
	clock_ns += t.fresh_ns;
}

static void
turn_ongoing (struct loop      *loop,
              struct loop_step *step)
{
	++t.ongoing_steps;
	clock_ns += t.ongoing_ns;
	loop_ready(loop, step);
}

static void
turn_bulk (struct loop      *loop,
           struct loop_step *step)
{
	clock_ns += 1'000;
	if (++t.turns == t.last)
		loop_exit(loop);
	else
		loop_ready_bulk(loop, step);
}

/* Run the loop for @p turns turns.  @p mail, @p fresh and @p ongoing are
 * the times in µs that the handler of mail and the steps of fresh and
 * ongoing add to the clock. */
static void
turn_run (uint64_t mail,
          uint64_t fresh,
          uint64_t ongoing,
          uint64_t turns)
{
	t = (typeof(t)){
		.fresh.fn = turn_fresh,
		.ongoing.fn = turn_ongoing,
		.bulk.fn = turn_bulk,
		.mail.fn = turn_mail,
		.mail_ns = mail * 1'000,
		.fresh_ns = fresh * 1'000,
		.ongoing_ns = ongoing * 1'000,
		.last = turns
	};
	struct loop l = loop();
	if (loop_init(&l)) {
		fprintf(stderr, "loop_init failed\n");
		++failures;
		loop_fini(&l);
		return;
	}
	/* Outside of a step, loop_ready() appends an entry to the fresh list.
	 * So the test appends the entry ongoing to the ongoing list itself. */
	list_append(&l.ready[ONGOING], &t.ongoing.hook);
	loop_ready_bulk(&l, &t.bulk);
	CHECK(!loop_post(&l, &t.mail));
	CHECK(!loop_exec(&l));
	loop_fini(&l);
	printf("mail=%3" PRIu64 "us fresh=%3" PRIu64 "us ongoing=%3" PRIu64 "us:"
	       " turns=%" PRIu64 " fresh steps=%" PRIu64 " ongoing steps=%" PRIu64
	       "\n", mail, fresh, ongoing, t.turns, t.fresh_steps, t.ongoing_steps);
}

/* Each turn has time for both steps, so both entries run in every turn. */
static void
test_room (void)
{
	turn_run(0, 50, 50, 100);
	CHECK(t.fresh_steps == 100 && t.ongoing_steps == 100);
}

/* After the handler, each turn has time to start one interactive step.  The
 * fresh entry and the ongoing entry must run in alternate turns. */
static void
test_one_step (void)
{
	turn_run(400, 200, 200, 100);
	CHECK(t.fresh_steps >= 49 && t.ongoing_steps >= 49);
}

/* The handler advances the clock by 600 µs, past the 500 µs that a turn has
 * for interactive steps.  Each turn must still run one interactive step.
 * The fresh entry and the ongoing entry must run in alternate turns. */
static void
test_no_room (void)
{
	turn_run(600, 100, 100, 100);
	CHECK(t.fresh_steps >= 49 && t.ongoing_steps >= 49);
}

int
main (void)
{
	test_room();
	test_one_step();
	test_no_room();
	if (failures) {
		fprintf(stderr, "turn: %u checks failed\n", failures);
		return 1;
	}
	printf("turn: all checks passed\n");
	return 0;
}
