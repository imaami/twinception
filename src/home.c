/** @file Home's jobs: admission, the registry of jobs in flight, and their
 *  runs over the curl bridge (home.h). */
#include <errno.h>
#include <inttypes.h>
#include <stdbit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "buf.h"
#include "hist.h"
#include "home.h"
#include "http.h"
#include "httpd.h"
#include "job.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "util.h"

enum {
	MAX_RESPONSE = 16 << 20,  /* the most bytes of an answer, until S6's
	                             1 MiB (design section 4) */
	KEYS_MAX     = 1 << 16,   /* the registry's buckets at most */
};

/* What home notes of a job's run, in its flags. */
enum {
	OVERFLOW  = 1,  /* the answer passed MAX_RESPONSE */
	DROPPED   = 2,  /* its front let go of it while it ran */
	WITHDRAWN = 4,  /* its client cancelled it, by its key, while it ran */
};

/* The jobs in flight are on one of three lists, each oldest first: admitted,
 * to start; running upstream; or dropped, running but let go of, to cancel.
 * Messages only admit, refuse or move jobs between them, and bulk steps
 * start and cancel them, a share at a time, as each such call into libcurl
 * costs some tens of µs.  The running jobs are in the order of their
 * deadlines, as a front gives each job it submits the same timeout and home
 * starts them first in first out (with several fronts, nearly so): home
 * ends them as those pass, whatever libcurl does.  The jobs in flight are
 * in the registry too, chained in buckets by a hash of their keys. */
struct home {
	struct loop_step          start;     /* starts the jobs admitted */
	struct loop_step          stop;      /* cancels the jobs dropped */
	struct loop_step          drain;     /* cancels every job in flight */
	struct loop_step          overdue;   /* ends the running jobs past their
	                                        deadline */
	struct loop_step          resume;    /* lets the answers held back on */
	struct loop_timer         deadline;  /* the oldest running job's */
	struct list               admitted;
	struct list               running;
	struct list               dropped;
	struct list               held;      /* the jobs whose answer a step's share
	                                        held back, by their held hooks */
	struct home_metrics      *metrics;
	struct loop              *loop;
	struct http              *http;
	struct httpd_home        *door;      /* to make room for descriptors */
	struct ai_endpoint const *ep;
	struct job              **keys;      /* the registry's buckets */
	size_t                    mask;      /* buckets - 1 */
	uint32_t                  max;       /* jobs in flight at most */
	uint16_t                  draining;  /* bool */
	uint16_t                  alone;     /* bool: the client loops have exited */
};

/* The thread's home: the handler of jobs' messages finds it here. */
static thread_local struct home *self;

/* The link that heads the registry's bucket of the keys of @p hash, which
 * the front works out, seeded, so that no client can pick keys that crowd
 * one. */
static struct job **
home_bucket (struct home const *home,
             uint64_t           hash)
{
	return &home->keys[hash & home->mask];
}

/* Drop @p n references to @p job, and hand it back to its front if they were
 * the last. */
static void
home_put (struct job *job,
          uint32_t    n)
{
	if (job_put(job, n)) {
		atomic_store_explicit(&job->front_bits, JOB_RETURN, memory_order_relaxed);
		loop_post(job->front, &job->to_front);
	}
}

/* End @p job as @p status, counted in @p count, if any: its completion goes
 * to its front.  The result of a job cancelled, which its front does not
 * read, goes back to the stash at once, and so does that of one its front let
 * go of as it ran, which home lets go of too.  Alone, home has its loop exit
 * once no job is in flight. */
static void
job_end (struct home      *home,
         struct job       *job,
         enum job_status   status,
         _Atomic uint64_t *count)
{
	if (!list_is_empty(&job->queue)) {
		list_del(&job->queue);
		struct job **p = home_bucket(home, job->hash);
		while (*p != job)
			p = &(*p)->chain;
		*p = job->chain;
		hist_add(&home->metrics->in_flight, -1ULL);
	}
	list_del(&job->held);
	if (count)
		hist_add(count, 1);
	bool dropped = job->flags & DROPPED;
	if (dropped || status == JOB_CANCELLED || status == JOB_WITHDRAWN)
		buf_free(&job->result);
	job->status = (uint8_t)status;
	job_post(job, job->front, &job->to_front, &job->front_bits, JOB_DONE);
	if (dropped)
		home_put(job, 1);
	if (home && home->alone && !hist_get(&home->metrics->in_flight))
		loop_exit(home->loop);
}

/* Gather the upstream's answer, up to MAX_RESPONSE bytes, as much a step as
 * its share of receiving takes: curl holds the rest back until a later step
 * lets it on (resume_step()).  A write over a whole share, which curl does
 * not make, would count in full. */
static size_t
job_write (char const *data,
           size_t      len,
           void       *user)
{
	struct job *job = user;
	struct home *home = self;
	if (!len)
		return 0;
	uint64_t room = loop_room(home->loop, LOOP_RECV);
	if (room < len && room < LOOP_STEP_RECV) {
		if (list_is_empty(&job->held))
			list_append(&home->held, &job->held);
		loop_ready_bulk(home->loop, &home->resume);
		return CURL_WRITEFUNC_PAUSE;
	}
	struct span s = buf_reserve(&job->result, len, MAX_RESPONSE);
	if (!s.len) {
		job->flags |= errno == ENOBUFS ? OVERFLOW : 0;
		return 0;
	}
	memcpy(s.ptr, data, len);
	buf_commit(&job->result, len);
	loop_work(home->loop, LOOP_RECV, len);
	return len;
}

/* The upstream call has ended: complete the job with the answer, or with
 * why there is none in its place.  Out of memory, that is empty. */
static void
job_done (CURLcode    result,
          long        status,
          char const *error,
          void       *user)
{
	struct job *job = user;
	struct home *home = self;
	job->transfer = nullptr;
	if (result == CURLE_OK) {
		job->http = (uint16_t)status;
		job_end(home, job, JOB_ANSWERED, &home->metrics->completed);
		return;
	}
	char const *why = curl_easy_strerror(result);
	buf_consume(&job->result, buf_peek(&job->result).len);
	struct span s = buf_reserve(&job->result, 64 + strlen(why) + CURL_ERROR_SIZE,
	                            SIZE_MAX);
	char *text = (char *)s.ptr;
	int n = !s.len ? 0
	      : job->flags & OVERFLOW
	      ? snprintf(text, s.len, "Upstream response exceeds %d MiB.",
	                 MAX_RESPONSE >> 20)
	      : result == CURLE_OPERATION_TIMEDOUT
	      ? snprintf(text, s.len, "Upstream request timed out after %.0f s.",
	                 (double)(home->loop->now - job->since) / 1e9)
	      : snprintf(text, s.len, "Upstream request failed: %s%s%s", why,
	                 error && *error ? ": " : "", error ? error : "");
	buf_commit(&job->result, n > 0 ? (size_t)n : 0);
	job_end(home, job, JOB_FAILED, &home->metrics->completed);
}

/* Start the jobs admitted, first in first out, as many a step as its share
 * of requests allows; one that cannot start ends at once, with no reason
 * given. */
static void
start_step (struct loop      *loop,
            struct loop_step *step)
{
	struct home *home = container_of(step, struct home, start);
	struct ai_endpoint const *ep = home->ep;
	for (; !list_is_empty(&home->admitted); loop_work(loop, LOOP_REQS, 1)) {
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		struct job *job = container_of(home->admitted.next, struct job, queue);
		struct span body = buf_peek(&job->request);
		list_del(&job->queue);
		list_append(&home->running, &job->queue);
		if (job->deadline && home->deadline.due == UINT64_MAX)
			(void)loop_timer_set(loop, &home->deadline, job->deadline);
		if (http_post_json(home->http, ep->url, (char const *)body.ptr, body.len,
		                   false, ep->headers, 0, job_write, job_done, job,
		                   &job->transfer))
			job_end(home, job, JOB_FAILED, &home->metrics->completed);
	}
}

static void
deadline_event (struct loop       *loop,
                struct loop_timer *timer)
{
	loop_ready_bulk(loop, &container_of(timer, struct home, deadline)->overdue);
}

/* End the running jobs past their deadline as timed out, oldest first, as
 * many a step as its share of requests allows; then wait for the next one's.
 * libcurl short of memory may lose a transfer's own timeout. */
static void
overdue_step (struct loop      *loop,
              struct loop_step *step)
{
	struct home *home = container_of(step, struct home, overdue);
	for (; !list_is_empty(&home->running); loop_work(loop, LOOP_REQS, 1)) {
		struct job *job = container_of(home->running.next, struct job, queue);
		if (!job->deadline || job->deadline > loop->now) {
			(void)loop_timer_set(loop, &home->deadline,
			                     job->deadline ? job->deadline : UINT64_MAX);
			return;
		}
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		http_end(home->http, &job->transfer, CURLE_OPERATION_TIMEDOUT);
	}
}

/* Let the answers held back on, oldest first, while the step's share of
 * receiving has room for a write from curl, which may hold one back again;
 * curl reads on in a later step. */
static void
resume_step (struct loop      *loop,
             struct loop_step *step)
{
	struct home *home = container_of(step, struct home, resume);
	while (!list_is_empty(&home->held) &&
	       loop_room(loop, LOOP_RECV) >= CURL_MAX_WRITE_SIZE) {
		struct job *job = container_of(home->held.next, struct job, held);
		list_del(&job->held);
		CURLcode e = http_resume(job->transfer);
		if (e)
			http_end(home->http, &job->transfer, e);
	}
	if (!list_is_empty(&home->held))
		loop_ready_bulk(loop, step);
}

/* Cancel the jobs on @p list, as many as the step's share of requests allows;
 * false if some are left. */
static bool
cancel_some (struct home *home,
             struct loop *loop,
             struct list *list)
{
	for (; !list_is_empty(list); loop_work(loop, LOOP_REQS, 1)) {
		if (!loop_room(loop, LOOP_REQS))
			return false;
		struct job *job = container_of(list->next, struct job, queue);
		http_cancel(home->http, &job->transfer);
		job_end(home, job, job->flags & WITHDRAWN ? JOB_WITHDRAWN : JOB_CANCELLED,
		        &home->metrics->cancelled);
	}
	return true;
}

static void
stop_step (struct loop      *loop,
           struct loop_step *step)
{
	struct home *home = container_of(step, struct home, stop);
	if (!cancel_some(home, loop, &home->dropped))
		loop_ready_bulk(loop, step);
}

/* Draining: cancel every job in flight. */
static void
drain_step (struct loop      *loop,
            struct loop_step *step)
{
	struct home *home = container_of(step, struct home, drain);
	if (!cancel_some(home, loop, &home->dropped) ||
	    !cancel_some(home, loop, &home->admitted) ||
	    !cancel_some(home, loop, &home->running))
		loop_ready_bulk(loop, step);
}

/* End the jobs in flight that the key of @p carrier names, as their client
 * asked: at once those not started, and by a step those running, the same
 * that cancels those their front let go of.  One its front let go of ends so
 * already. */
static void
home_withdraw (struct home      *home,
               struct job const *carrier)
{
	struct span key = buf_peek(&carrier->key);
	struct job *next;
	for (struct job *job = *home_bucket(home, carrier->hash); job; job = next) {
		struct span k = buf_peek(&job->key);
		next = job->chain;
		if (job->hash != carrier->hash || k.len != key.len ||
		    memcmp(k.ptr, key.ptr, key.len) || job->flags & (DROPPED | WITHDRAWN))
			continue;
		if (!job->transfer) {
			job_end(home, job, JOB_WITHDRAWN, &home->metrics->cancelled);
			continue;
		}
		job->flags |= WITHDRAWN;
		list_del(&job->queue);
		list_append(&home->dropped, &job->queue);
		loop_ready_bulk(home->loop, &home->stop);
	}
}

void
home_job (struct loop *loop,
          struct msg  *msg)
{
	struct job *job = container_of(msg, struct job, to_home);
	uint32_t bits = atomic_exchange(&job->home_bits, 0);
	/* Once the loop has stopped, jobs end at once, and nothing counts. */
	struct home *home = loop->exit ? nullptr : self;
	struct home_metrics *m = home ? home->metrics : nullptr;
	if (bits & JOB_WITHDRAW) {
		/* A carrier, which goes back to its front at once. */
		if (home)
			home_withdraw(home, job);
		home_put(job, 2);
		return;
	}
	if (bits & JOB_SUBMIT) {
		/* A job may come back for another run: what home noted of the
		 * last goes before any end. */
		job->flags = 0;
		if (m)
			hist_add(&m->submitted, 1);
		if (!home || bits & JOB_CANCEL) {
			job_end(home, job, JOB_CANCELLED, m ? &m->cancelled : nullptr);
		} else if (home->draining) {
			job_end(home, job, JOB_CANCELLED, &m->rejected);
		} else if (hist_get(&m->in_flight) >= home->max) {
			job_end(home, job, JOB_BUSY, &m->rejected);
		} else {
			struct job **p = home_bucket(home, job->hash);
			job->chain = *p;
			*p = job;
			list_append(&home->admitted, &job->queue);
			hist_add(&m->in_flight, 1);
			job->since = loop->now;
			loop_ready_bulk(loop, &home->start);
			/* It may need a descriptor upstream. */
			httpd_home_crowd(home->door);
		}
	} else if (bits & JOB_CANCEL && job->transfer) {
		/* Cancelling it takes a step, which then lets go of it. */
		job->flags |= DROPPED;
		list_del(&job->queue);
		list_append(&home->dropped, &job->queue);
		loop_ready_bulk(loop, &home->stop);
		home_put(job, 1);
		return;
	} else if (bits & JOB_CANCEL && !list_is_empty(&job->queue)) {
		job_end(home, job, JOB_CANCELLED, &m->cancelled);
	}
	/* Let go of the job as its front has, the result's storage back to the
	 * stash it came from. */
	if (bits & JOB_CANCEL)
		buf_free(&job->result);
	home_put(job, bits & JOB_CANCEL ? 2 : 1);
}

int
home_init (struct home               **p_home,
           struct loop                *loop,
           struct blockers            *blockers,
           struct httpd_home          *door,
           struct ai_endpoint const   *ep,
           uint32_t                    max,
           struct home_metrics        *metrics,
           struct http_metrics        *http)
{
	size_t buckets = stdc_bit_ceil(max < KEYS_MAX ? max : (uint32_t)KEYS_MAX);
	struct home *home = malloc(sizeof *home);
	struct job **keys = calloc(buckets, sizeof *keys);
	if (!home || !keys) {
		free(keys);
		free(home);
		return ENOMEM;
	}
	*home = (struct home){
		.start.fn = start_step,
		.stop.fn = stop_step,
		.drain.fn = drain_step,
		.overdue.fn = overdue_step,
		.resume.fn = resume_step,
		.deadline.fn = deadline_event,
		.metrics = metrics,
		.loop = loop,
		.door = door,
		.ep = ep,
		.keys = keys,
		.mask = buckets - 1,
		.max = max
	};
	list_init(&home->admitted);
	list_init(&home->running);
	list_init(&home->dropped);
	list_init(&home->held);
	/* In the heap from the start, the timer is set without fail. */
	int e = loop_timer_set(loop, &home->deadline, UINT64_MAX);
	if (!e)
		e = http_init(&home->http, loop, http);
	if (e) {
		loop_timer_stop(loop, &home->deadline);
		free(keys);
		free(home);
		return e;
	}
	http_resolver(home->http, blockers);
	self = *p_home = home;
	return 0;
}

void
home_fini (struct home **p_home)
{
	struct home *home = *p_home;
	if (!home)
		return;
	*p_home = nullptr;
	struct list *const lists[] = { &home->dropped, &home->admitted, &home->running };
	for (size_t i = 0; i < ARRAY_SIZE(lists); ++i) {
		while (!list_is_empty(lists[i])) {
			struct job *job = container_of(lists[i]->next, struct job, queue);
			http_cancel(home->http, &job->transfer);
			job_end(home, job, JOB_CANCELLED, &home->metrics->cancelled);
		}
	}
	struct home_metrics const *m = home->metrics;
	LOG(LOG_INFO, "jobs: %" PRIu64 " submitted, %" PRIu64 " completed, %" PRIu64
	    " cancelled, %" PRIu64 " rejected", hist_get(&m->submitted),
	    hist_get(&m->completed), hist_get(&m->cancelled), hist_get(&m->rejected));
	loop_unready(&home->start);
	loop_unready(&home->stop);
	loop_unready(&home->drain);
	loop_unready(&home->overdue);
	loop_unready(&home->resume);
	loop_timer_stop(home->loop, &home->deadline);
	http_fini(&home->http);
	self = nullptr;
	free(home->keys);
	free(home);
}

void
home_drain (struct home *home)
{
	home->draining = true;
	loop_ready_bulk(home->loop, &home->drain);
}

void
home_alone (struct home *home)
{
	home->alone = true;
	if (!home->draining)
		home_drain(home);
	if (!hist_get(&home->metrics->in_flight))
		loop_exit(home->loop);
}
