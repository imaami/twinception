/** @file Home's jobs: admission, the scheduler, the registry of jobs in
 *  flight, their runs over the curl bridge, streamed answers and the breaker
 *  (home.h). */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ai.h"
#include "buf.h"
#include "config.h"
#include "hist.h"
#include "home.h"
#include "http.h"
#include "httpd.h"
#include "jb.h"
#include "job.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "shelf.h"
#include "util.h"

enum {
	MAX_RESPONSE = 16 << 20,  /* the most bytes of an answer, of a streamed
	                             one's text, and of an event of it */
	MAX_SCALAR   = 1 << 10,   /* bytes of a JSON number or literal, which
	                             json-c converts whole */
	MAX_PROPS    = 1 << 20,   /* bytes of a /props answer */
	PROPS_MS     = 10'000,    /* the most /props may take, or the timeout */
	KEYS_MAX     = 1 << 16,   /* the registry's buckets at most, and the
	                             principals' */
	QUEUED       = 8,         /* jobs queued for a principal at most */
	SLOTS        = 4,         /* upstream calls at once, unless told or found */
	FAILURES     = 5,         /* failures in a row that open the breaker */
	WITHDRAWALS  = 16,        /* withdrawals of calls between their turns'
	                             jobs kept, the oldest given up first */
};

static constexpr uint64_t NS = 1'000'000'000;
static constexpr uint64_t BACKOFF_MIN = NS;
static constexpr uint64_t BACKOFF_MAX = 60 * NS;
static constexpr uint64_t WITHDRAWAL_NS = 600 * NS;  /* how long one is kept */

/* What home notes of a job's run, in its flags. */
enum {
	OVERFLOW  = 1 << 0,   /* the answer passed MAX_RESPONSE */
	DROPPED   = 1 << 1,   /* its front let go of it while it ran */
	WITHDRAWN = 1 << 2,   /* its client cancelled it, by its key, while it ran */
	RUNNING   = 1 << 3,   /* it holds an upstream slot */
	PROBE     = 1 << 4,   /* it is the breaker's probe */
	SEEN      = 1 << 5,   /* its answer has begun */
	STREAM    = 1 << 6,   /* the answer is an event stream */
	BACKED    = 1 << 7,   /* held back until its stream is read on */
	DELIVERED = 1 << 8,   /* libcurl has handed over all of the stream */
	EVENT     = 1 << 9,   /* an event of it is whole, and is being parsed */
	TRUNCATED = 1 << 10,  /* its stop reason is the output token limit */
	ERRED     = 1 << 11,  /* the result says what went wrong upstream */
	STALLED   = 1 << 12,  /* the chunk timeout ended it */
	ENDED     = 1 << 13,  /* its stream said the answer has ended */
	READY     = 1 << 14,  /* a slot was free for it: on a ready list */
	PREEMPTED = 1 << 15,  /* an interactive job preempted it once, which no
	                         other may again */
};

/* The breaker's states, as /metrics gives them. */
enum { CLOSED, OPEN, HALF };

/* Whom jobs of a class are for: an identity, a session, or a client's
 * address.  An entry exists while the principal has jobs of the class
 * queued, which tell who it is, and is kept for reuse after. */
struct principal {
	struct list       ring;    /* on home's ring of its class, or its shelf */
	struct list       jobs;    /* queued, oldest first, by their queue hooks */
	struct principal *chain;   /* the next in its bucket */
	uint64_t          hash;
	uint32_t          queued;
	uint32_t          class;   /* 0 interactive, 1 background */
};

/*
 * Every job in flight is on the timeline, in the order of its deadline: the
 * front's, from its submission, while it waits, and from its start upstream
 * once it runs, which moves it to the end.  A job waiting is on its
 * principal's queue, and the principals with jobs queued are on the ring of
 * their jobs' class, the next to start one first: each starts one, then
 * goes to the back; or, if a slot was free for it as it came and nobody
 * waited, it is ready, to start before them.  Interactive jobs start before
 * background ones, which hold all slots but one at most, so that one stays
 * free for interactive work; with one slot, an interactive job that has to
 * wait preempts a background one running, which starts again later, ready,
 * before the other background jobs.  A running background job is among
 * those of its class, unless its front let go of it: then it is among
 * those dropped, to cancel.  Messages only admit, refuse or move jobs, and bulk
 * steps start, read and cancel them, a share at a time, as each call into
 * libcurl costs some tens of µs.  The jobs in flight are in the registry
 * too, chained in buckets by a hash of their keys, as the principals are by
 * theirs.
 */
struct home {
	struct loop_step          start;       /* starts the jobs waiting */
	struct loop_step          stop;        /* cancels the jobs dropped */
	struct loop_step          drain;       /* cancels every job in flight */
	struct loop_step          overdue;     /* ends the jobs past their deadline */
	struct loop_step          silent;      /* ends the answers silent too long */
	struct loop_step          resume;      /* lets the answers held back on */
	struct loop_step          feed;        /* reads the streams */
	struct loop_step          reap;        /* frees the events' values */
	struct loop_step          shed;        /* ends the jobs waiting, the breaker
	                                          open */
	struct loop_step          preempt;     /* preempts the jobs preempting */
	struct loop_step          props;       /* reads /props */
	struct loop_timer         deadline;    /* the timeline's first */
	struct loop_timer         quiet;       /* the answer silent longest */
	struct loop_timer         breaker;     /* when the breaker half-opens */
	struct list               ring[2];     /* by class */
	struct list               ready[2];    /* by class, by the jobs' queue
	                                          hooks */
	struct list               background;  /* the background jobs running, by
	                                          their queue hooks */
	struct list               preempting;  /* those to preempt, likewise */
	struct list               timeline;
	struct list               dropped;
	struct list               held;        /* the jobs whose answer a step's share
	                                          held back, by their held hooks */
	struct list               idle;        /* the answers under way, the one
	                                          silent longest first */
	struct list               feeding;     /* the streams with bytes to read */
	struct shelf              spare;       /* principals kept for reuse */
	struct jb_pool            pool;        /* the events' tokeners */
	struct jb_reaper          reaper;      /* the events' values left to free */
	struct jb_parser          parser;      /* /props's */
	struct buf                answer;      /* /props's */
	struct home_metrics      *metrics;
	struct loop              *loop;
	struct http              *http;
	struct httpd_home        *door;        /* to make room for descriptors */
	struct ai_endpoint const *ep;
	struct job              **keys;        /* the registry's buckets */
	struct principal        **whom;        /* the principals' buckets */
	struct http_request      *ask;         /* GET /props, under way */
	uint64_t                  timeout;     /* ns from a start to its deadline */
	uint64_t                  silence;     /* ns an answer may pause */
	uint64_t                  backoff;     /* the breaker's next open spell */
	uint64_t                  until;       /* when the breaker half-opens */
	struct {
		uint64_t hash;
		uint64_t until;
		size_t   len;
	}                         gone[WITHDRAWALS];  /* withdrawals that found no
	                                                 job: the key's hash and
	                                                 length, and when they are
	                                                 given up; for the turns
	                                                 of a call that lends
	                                                 tools, between jobs */
	size_t                    mask;        /* buckets - 1 */
	uint32_t                  max;         /* jobs in flight at most */
	uint32_t                  slots;       /* upstream calls at once at most */
	uint32_t                  context;     /* tokens of the upstream's context,
	                                          as told or found; 0: unknown */
	uint32_t                  failures;    /* in a row, closed */
	uint32_t                  readied[2];  /* jobs ready, by class */
	uint32_t                  running_bg;  /* background jobs running */
	uint32_t                  gone_at;     /* the next of gone to fill */
	uint16_t                  draining;    /* bool, widened over the padding */
	uint16_t                  alone;       /* bool: the client loops have exited */
	uint8_t                   state;       /* the breaker's */
	uint8_t                   probing;     /* bool: a probe is under way */
	uint8_t                   paused;      /* bool: /props is held back */
	uint8_t                   fifo;        /* bool: one queue for all */
};

/* The thread's home: the handler of jobs' messages finds it here. */
static thread_local struct home *self;

static void preempt_excess (struct home *home);

/* The link that heads the registry's bucket of the keys of @p hash, which
 * the front works out, seeded, so that no client can pick keys that crowd
 * one. */
static struct job **
home_bucket (struct home const *home,
             uint64_t           hash)
{
	return &home->keys[hash & home->mask];
}

/* @p job's class: 0 interactive, 1 background. */
static uint32_t
class_of (struct job const *job)
{
	return (job->asks & JOB_BACKGROUND) != 0;
}

/* The upstream slots that background jobs may hold at most: all but one,
 * which stays for interactive work, unless there is only one. */
static uint32_t
background_slots (struct home const *home)
{
	return home->slots - (home->slots >= 2);
}

/* Whether @p job is for @p p, who its first job queued is for. */
static bool
for_principal (struct principal const *p,
               struct job const       *job)
{
	struct job const *first = container_of(p->jobs.next, struct job, queue);
	return p->hash == job->principal && p->class == class_of(job) &&
	       first->session == job->session &&
	       (job->session ? !memcmp(buf_peek(&first->key).ptr,
	                               buf_peek(&job->key).ptr, job->session)
	                     : first->peer[0] == job->peer[0] &&
	                       first->peer[1] == job->peer[1]);
}

/* The link that holds the principal @p job is for, or where it would be
 * appended; for all of a class one in a queue first in first out. */
static struct principal **
principal_slot (struct home const *home,
                struct job const  *job)
{
	struct principal **p = &home->whom[home->fifo ? 0 : job->principal & home->mask];
	while (*p && (home->fifo ? (*p)->class != class_of(job) : !for_principal(*p, job)))
		p = &(*p)->chain;
	return p;
}

/* Publish @p job's progress, and tell its front if it asked. */
static void
job_progress (struct job *job,
              uint64_t    progress)
{
	atomic_store_explicit(&job->progress, progress, memory_order_relaxed);
	if (job->asks & JOB_NOTIFY && !(job->flags & (DROPPED | WITHDRAWN)))
		job_post(job, job->front, &job->to_front, &job->front_bits, JOB_PROGRESS);
}

/* Take @p job off its principal's queue, the jobs behind it each a place
 * nearer, at most QUEUED - 1 (in one queue for all, which has no cap, they
 * keep the places they had); a principal with none left goes on the
 * shelf. */
static void
queue_leave (struct home *home,
             struct job  *job)
{
	struct principal *p = job->who;
	struct list *node = job->queue.next;
	list_del(&job->queue);
	job->who = nullptr;
	hist_add(&home->metrics->queued, -1ULL);
	if (--p->queued) {
		for (; !home->fifo && node != &p->jobs; node = node->next) {
			struct job *next = container_of(node, struct job, queue);
			job_progress(next, atomic_load_explicit(&next->progress,
			                                        memory_order_relaxed) - 1);
		}
		return;
	}
	struct principal **link = &home->whom[home->fifo ? 0 : p->hash & home->mask];
	while (*link != p)
		link = &(*link)->chain;
	*link = p->chain;
	list_del(&p->ring);
	shelf_put(&home->spare, &p->ring);
	hist_add(&home->metrics->principals, -1ULL);
}

/* Give the storage of @p job's answer back to the stash. */
static void
answer_free (struct job *job)
{
	buf_free(&job->result);
	buf_free(&job->calls);
	buf_free(&job->thought);
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
 * go of as it ran, which home lets go of too.  Its slot, if it had one, goes
 * to the next.  Alone, home has its loop exit once no job is in flight. */
static void
job_end (struct home      *home,
         struct job       *job,
         enum job_status   status,
         _Atomic uint64_t *count)
{
	if (!list_is_empty(&job->due)) {
		struct home_metrics *m = home->metrics;
		list_del(&job->due);
		struct job **p = home_bucket(home, job->hash);
		while (*p != job)
			p = &(*p)->chain;
		*p = job->chain;
		hist_add(&m->in_flight, -1ULL);
		if (job->who)
			queue_leave(home, job);
		home->readied[class_of(job)] -= (job->flags & READY) != 0;
		list_del(&job->queue);
		list_del(&job->idle);
		list_del(&job->feed);
		if (job->flags & RUNNING) {
			hist_add(&m->running, -1ULL);
			if (class_of(job)) {
				hist_add(&m->background, -1ULL);
				--home->running_bg;
			}
			home->probing &= !(job->flags & PROBE);
			loop_ready_bulk(home->loop, &home->start);
		}
		jb_parse_end(&job->parser, &home->pool, &home->reaper);
		if (home->reaper.n)
			loop_ready_bulk(home->loop, &home->reap);
		buf_free(&job->wire);
	}
	list_del(&job->held);
	if (count)
		hist_add(count, 1);
	bool dropped = job->flags & DROPPED;
	if (dropped || status == JOB_CANCELLED || status == JOB_WITHDRAWN)
		answer_free(job);
	job->status = (uint8_t)status;
	job_post(job, job->front, &job->to_front, &job->front_bits, JOB_DONE);
	if (dropped)
		home_put(job, 1);
	if (home && home->alone && !hist_get(&home->metrics->in_flight))
		loop_exit(home->loop);
}

/* Put what @p fmt says, formatted, in @p job's result in its answer's place;
 * empty if out of memory. */
[[gnu::format(printf, 2, 3)]]
static void
job_say (struct job *job,
         char const *fmt,
         ...)
{
	buf_consume(&job->result, buf_peek(&job->result).len);
	for (size_t room = 128;;) {
		struct span s = buf_reserve(&job->result, room, SIZE_MAX);
		va_list ap;
		va_start(ap, fmt);
		int n = s.len ? vsnprintf((char *)s.ptr, s.len, fmt, ap) : -1;
		va_end(ap);
		if (n < 0)
			return;
		if ((size_t)n < s.len) {
			buf_commit(&job->result, (size_t)n);
			return;
		}
		room = (size_t)n + 1;
	}
}

/* The breaker is open: end @p job, which may not go upstream, with when to
 * try again. */
static void
job_refuse (struct home *home,
            struct job  *job)
{
	uint64_t left = home->until > home->loop->now ? home->until - home->loop->now : 0;
	job_say(job, "The upstream is unavailable; try again in %" PRIu64 " s.",
	        (left + NS - 1) / NS);
	job_end(home, job, JOB_FAILED, &home->metrics->rejected);
}

static void
breaker_state (struct home *home,
               uint8_t      state)
{
	home->state = state;
	atomic_store_explicit(&home->metrics->breaker, state, memory_order_relaxed);
}

/* Refuse the upstream jobs for a spell: what Retry-After asked for, @p retry
 * seconds, or else the backoff, which doubles for the next. */
static void
breaker_open (struct home *home,
              long         retry)
{
	struct loop *loop = home->loop;
	uint64_t spell = retry > 0 ? (uint64_t)retry * NS : home->backoff;
	spell = spell < BACKOFF_MIN ? BACKOFF_MIN : spell > BACKOFF_MAX ? BACKOFF_MAX : spell;
	home->backoff = home->backoff < BACKOFF_MAX / 2 ? 2 * home->backoff : BACKOFF_MAX;
	home->until = loop->now + spell;
	breaker_state(home, OPEN);
	hist_add(&home->metrics->opens, 1);
	(void)loop_timer_set(loop, &home->breaker, home->until);
	LOG(LOG_WARN, "upstream unavailable: tool calls refused for %" PRIu64 " s",
	    spell / NS);
	loop_ready_bulk(loop, &home->shed);
}

static void
breaker_event (struct loop       *loop,
               struct loop_timer *timer)
{
	struct home *home = container_of(timer, struct home, breaker);
	breaker_state(home, HALF);
	LOG(LOG_INFO, "upstream breaker half-open: a probe goes next");
	loop_ready_bulk(loop, &home->start);
}

/* Count how @p job went upstream: a failure in a row of FAILURES, or of the
 * probe, opens the breaker, as does one whose answer said when to retry; the
 * probe's success, which its answer's beginning tells, closes it.  Other
 * jobs' outcomes count only while it is closed, and a job cancelled counts
 * not at all. */
static void
breaker_note (struct home *home,
              struct job  *job,
              bool         failure,
              long         retry)
{
	if (job->flags & PROBE) {
		job->flags &= (uint16_t)~PROBE;
		home->probing = false;
		if (failure) {
			breaker_open(home, retry);
		} else {
			breaker_state(home, CLOSED);
			home->failures = 0;
			home->backoff = BACKOFF_MIN;
			LOG(LOG_INFO, "upstream breaker closed: the probe went through");
			loop_ready_bulk(home->loop, &home->start);
			/* An interactive job may wait for the probe's slot. */
			preempt_excess(home);
		}
		return;
	}
	if (home->state != CLOSED)
		return;
	home->failures = failure ? home->failures + 1 : 0;
	if (failure && (home->failures >= FAILURES || retry > 0))
		breaker_open(home, retry);
}

/* @p job's run upstream is over, with @p result and libcurl's @p error, and
 * @p retry the seconds its answer's Retry-After asked for: complete it with
 * the answer, its text, or why there is none in its place. */
static void
job_finish (struct home *home,
            struct job  *job,
            CURLcode     result,
            char const  *error,
            long         retry)
{
	struct home_metrics *m = home->metrics;
	uint16_t flags = job->flags;
	breaker_note(home, job, result ? result != CURLE_WRITE_ERROR &&
	                                 result != CURLE_OUT_OF_MEMORY
	                               : job->http >= 500 || job->http == 429, retry);
	if (result == CURLE_OK && !(flags & STREAM)) {
		job_end(home, job, JOB_ANSWERED, &m->completed);
		return;
	}
	struct span text = buf_peek(&job->result);
	bool calls = buf_peek(&job->calls).len;
	if (result == CURLE_OK && !(flags & ERRED) && (text.len || calls)) {
		/* Cut short, a tool call's arguments are, which the front tells. */
		struct span s = flags & TRUNCATED && !calls
		              ? buf_reserve(&job->result, sizeof AI_CUT_NOTE - 1, SIZE_MAX)
		              : (struct span){};
		if (s.len) {
			memcpy(s.ptr, AI_CUT_NOTE, sizeof AI_CUT_NOTE - 1);
			buf_commit(&job->result, sizeof AI_CUT_NOTE - 1);
		}
		job_end(home, job, JOB_TEXT, &m->completed);
		return;
	}
	if (result == CURLE_OK && !(flags & ERRED)) {
		char why[96];
		(void)ai_no_answer(why, sizeof why, *job->reason ? job->reason : nullptr);
		job_say(job, "%s", why);
	} else if (flags & OVERFLOW) {
		job_say(job, "Upstream response exceeds %d MiB.", MAX_RESPONSE >> 20);
	} else if (flags & STALLED) {
		job_say(job, "Upstream answer stalled: nothing came for %.0f s.",
		         (double)home->silence / 1e9);
	} else if (result == CURLE_OPERATION_TIMEDOUT) {
		job_say(job, "Upstream request timed out after %.0f s.",
		        (double)(home->loop->now - job->since) / 1e9);
	} else if (result) {
		job_say(job, "Upstream request failed: %s%s%s", curl_easy_strerror(result),
		        *error ? ": " : "", error);
	}
	job_end(home, job, JOB_FAILED, &m->completed);
}

/* End @p job's run upstream now, with @p result, cancelling its transfer if
 * it still has one: the upstream sees a hang-up. */
static void
job_cut (struct home *home,
         struct job  *job,
         CURLcode     result)
{
	http_cancel(home->http, &job->transfer);
	job_finish(home, job, result, "", 0);
}

/* Gather the upstream's answer, a stream into the wire and a plain one into
 * the result, up to MAX_RESPONSE bytes, as much a step as its share of
 * receiving takes: curl holds the rest back until a later step lets it on
 * (resume_step()).  A stream is held back too while a step's share of it
 * waits to be read.  A write over a whole share, which curl does not make,
 * would count in full.  Each write counts as the answer's sign of life. */
static size_t
job_write (char const *data,
           size_t      len,
           void       *user)
{
	struct job *job = user;
	struct home *home = self;
	struct loop *loop = home->loop;
	if (!len)
		return 0;
	if (!(job->flags & SEEN)) {
		long status;
		char const *type;
		http_response(job->transfer, &status, &type);
		job->flags |= SEEN;
		job->http = (uint16_t)status;
		if (status >= 200 && status < 300 && type &&
		    !strncasecmp(type, "text/event-stream", 17))
			job->flags |= STREAM;
		/* The upstream answers: the other jobs need not wait for the
		 * probe's end, which may be minutes away. */
		if (job->flags & PROBE && status < 500 && status != 429)
			breaker_note(home, job, false, 0);
	}
	job->active = loop->now;
	list_del(&job->idle);
	list_append(&home->idle, &job->idle);
	if (home->silence && home->quiet.due == UINT64_MAX)
		(void)loop_timer_set(loop, &home->quiet, job->active + home->silence);

	uint64_t room = loop_room(loop, LOOP_RECV);
	bool stream = job->flags & STREAM;
	if (stream && buf_peek(&job->wire).len - job->scan >= LOOP_STEP_RECV) {
		job->flags |= BACKED;
		return CURL_WRITEFUNC_PAUSE;
	}
	if (room < len && room < LOOP_STEP_RECV) {
		if (list_is_empty(&job->held))
			list_append(&home->held, &job->held);
		loop_ready_bulk(loop, &home->resume);
		return CURL_WRITEFUNC_PAUSE;
	}
	struct buf *to = stream ? &job->wire : &job->result;
	struct span s = buf_reserve(to, len, MAX_RESPONSE);
	if (!s.len) {
		job->flags |= errno == ENOBUFS ? OVERFLOW : 0;
		return 0;
	}
	memcpy(s.ptr, data, len);
	buf_commit(to, len);
	loop_work(loop, LOOP_RECV, len);
	if (stream) {
		if (list_is_empty(&job->feed))
			list_append(&home->feeding, &job->feed);
		loop_ready_bulk(loop, &home->feed);
	}
	return len;
}

/* The upstream call has ended.  A stream's end, once all of it is read; any
 * other answer, or none, at once. */
static void
job_done (CURLcode    result,
          long        status,
          char const *error,
          void       *user)
{
	struct job *job = user;
	struct home *home = self;
	job->transfer = nullptr;
	job->http = (uint16_t)status;
	list_del(&job->idle);
	list_del(&job->held);
	if (result == CURLE_OK && job->flags & STREAM) {
		job->flags |= DELIVERED;
		if (list_is_empty(&job->feed))
			list_append(&home->feeding, &job->feed);
		loop_ready_bulk(home->loop, &home->feed);
		return;
	}
	job_finish(home, job, result, error, http_retry_after(home->http));
}

/* Start @p job upstream, at the end of the timeline, its deadline the
 * latest; half-open, as the breaker's probe. */
static void
job_start (struct home *home,
           struct job  *job)
{
	struct home_metrics *m = home->metrics;
	struct loop *loop = home->loop;
	struct ai_endpoint const *ep = home->ep;
	if (home->state == HALF) {
		job->flags |= PROBE;
		home->probing = true;
		hist_add(&m->probes, 1);
	}
	job->flags |= RUNNING;
	hist_add(&m->running, 1);
	hist_add(&m->dispatched, 1);
	if (class_of(job)) {
		list_append(&home->background, &job->queue);
		hist_add(&m->background, 1);
		++home->running_bg;
	}
	job->since = loop->now;
	job->context = home->context;
	job->deadline = home->timeout ? loop->now + home->timeout : 0;
	list_del(&job->due);
	list_append(&home->timeline, &job->due);
	if (job->deadline && home->deadline.due == UINT64_MAX)
		(void)loop_timer_set(loop, &home->deadline, job->deadline);
	job_progress(job, 0);
	struct span body = buf_peek(&job->request);
	if (http_post_json(home->http, ep->url, (char const *)body.ptr, body.len, false,
	                   ep->headers, 0, job_write, job_done, job, &job->transfer))
		job_end(home, job, JOB_FAILED, &m->completed);
}

/* The job to start next: of the interactive ones, then of the background
 * ones, the first ready, else the oldest of the ring's first principal; but
 * none of the background ones to @p start while they hold all the slots
 * they may; nullptr if none waits. */
static struct job *
job_next (struct home const *home,
          bool               start)
{
	for (uint32_t c = 0; c < 2; ++c) {
		if (c && start && home->running_bg >= background_slots(home))
			break;
		if (!list_is_empty(&home->ready[c]))
			return container_of(home->ready[c].next, struct job, queue);
		if (list_is_empty(&home->ring[c]))
			continue;
		struct principal *p = container_of(home->ring[c].next, struct principal,
		                                   ring);
		return container_of(p->jobs.next, struct job, queue);
	}
	return nullptr;
}

/* Start jobs waiting while slots are free, home does not drain, the breaker
 * lets them and the step's share of requests allows: those ready first, then
 * the next principal on the ring starts its first and goes to the back. */
static void
start_step (struct loop      *loop,
            struct loop_step *step)
{
	struct home *home = container_of(step, struct home, start);
	while (!home->draining && home->state != OPEN && !home->probing &&
	       hist_get(&home->metrics->running) < home->slots) {
		struct job *job = job_next(home, true);
		if (!job)
			return;
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		if (job->flags & READY) {
			job->flags &= (uint16_t)~READY;
			--home->readied[class_of(job)];
			list_del(&job->queue);
		} else {
			list_del(&job->who->ring);
			list_append(&home->ring[class_of(job)], &job->who->ring);
			queue_leave(home, job);
		}
		job_start(home, job);
		loop_work(loop, LOOP_REQS, 1);
	}
}

static void
deadline_event (struct loop       *loop,
                struct loop_timer *timer)
{
	loop_ready_bulk(loop, &container_of(timer, struct home, deadline)->overdue);
}

/* Cancel @p job, which its front let go of, its client withdrew, or a drain
 * ends: one whose request had gone upstream counts as cancelled there. */
static void
job_cancel (struct home *home,
            struct job  *job)
{
	struct home_metrics *m = home->metrics;
	if (job->transfer && http_sent(job->transfer))
		hist_add(&m->aborted, 1);
	http_cancel(home->http, &job->transfer);
	job_end(home, job, job->flags & WITHDRAWN ? JOB_WITHDRAWN : JOB_CANCELLED,
	        &m->cancelled);
}

/* End the jobs past their deadline, oldest first, as many a step as its share
 * of requests allows; then wait for the next one's.  A job running times out,
 * one waiting for a slot does too, refused, and one dropped is cancelled.
 * libcurl short of memory may lose a transfer's own timeout. */
static void
overdue_step (struct loop      *loop,
              struct loop_step *step)
{
	struct home *home = container_of(step, struct home, overdue);
	for (; !list_is_empty(&home->timeline); loop_work(loop, LOOP_REQS, 1)) {
		struct job *job = container_of(home->timeline.next, struct job, due);
		if (!job->deadline || job->deadline > loop->now) {
			(void)loop_timer_set(loop, &home->deadline,
			                     job->deadline ? job->deadline : UINT64_MAX);
			return;
		}
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		if (job->flags & (DROPPED | WITHDRAWN)) {
			job_cancel(home, job);
		} else if (job->flags & RUNNING) {
			job_cut(home, job, CURLE_OPERATION_TIMEDOUT);
		} else if (job->flags & PREEMPTED) {
			/* It went upstream, and keeps its deadline from then. */
			job_say(job, "Timed out after %.0f s: an interactive call preempted it, "
			        "and it could not start again in time.",
			        (double)home->ep->timeout_ms / 1e3);
			job_end(home, job, JOB_FAILED, &home->metrics->completed);
		} else {
			job_say(job, "Timed out after %.0f s waiting for an upstream slot.",
			        (double)home->ep->timeout_ms / 1e3);
			job_end(home, job, JOB_FAILED, &home->metrics->rejected);
		}
	}
	(void)loop_timer_set(loop, &home->deadline, UINT64_MAX);
}

static void
quiet_event (struct loop       *loop,
             struct loop_timer *timer)
{
	loop_ready_bulk(loop, &container_of(timer, struct home, quiet)->silent);
}

/* End the answers that sent nothing for the chunk timeout, the one silent
 * longest first, as many a step as its share of requests allows; then wait
 * for the next. */
static void
silent_step (struct loop      *loop,
             struct loop_step *step)
{
	struct home *home = container_of(step, struct home, silent);
	for (; !list_is_empty(&home->idle); loop_work(loop, LOOP_REQS, 1)) {
		struct job *job = container_of(home->idle.next, struct job, idle);
		if (job->active + home->silence > loop->now) {
			(void)loop_timer_set(loop, &home->quiet, job->active + home->silence);
			return;
		}
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		job->flags |= STALLED;
		hist_add(&home->metrics->stalls, 1);
		job_cut(home, job, CURLE_OPERATION_TIMEDOUT);
	}
	(void)loop_timer_set(loop, &home->quiet, UINT64_MAX);
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
			job_cut(home, job, e);
	}
	if (!list_is_empty(&home->held))
		loop_ready_bulk(loop, step);
}

/* Add @p len bytes of text at @p text to the answer of the job @p user, if
 * they are UTF-8 (json-c lets overlong forms through), and the answer stays
 * within MAX_RESPONSE; it is copied whole. */
static int
put_text (void       *user,
          char const *text,
          size_t      len)
{
	struct job *job = user;
	if (jb_utf8(text, len) < len)
		return EILSEQ;
	struct span s = len ? buf_reserve(&job->result, len, MAX_RESPONSE) : (struct span){};
	if (!s.len)
		return len ? errno : 0;
	memcpy(s.ptr, text, len);
	buf_commit(&job->result, len);
	loop_work(self->loop, LOOP_COPY, len);
	return 0;
}

/* Add @p len bytes of thinking at @p text to the job @p user's, as
 * put_text() does its answer. */
static int
put_thought (void       *user,
             char const *text,
             size_t      len)
{
	struct job *job = user;
	if (jb_utf8(text, len) < len)
		return EILSEQ;
	struct span s = len ? buf_reserve(&job->thought, len, MAX_RESPONSE)
	                    : (struct span){};
	if (!s.len)
		return len ? errno : 0;
	memcpy(s.ptr, text, len);
	buf_commit(&job->thought, len);
	loop_work(self->loop, LOOP_COPY, len);
	return 0;
}

/* Add what @p call says of a tool call to the job @p user's calls: a call
 * begins with an index none had, after which those before it are whole, or,
 * for a server that gives no index, with an id unlike the last call's; its
 * id comes before its name, and its name before its arguments, which may
 * come in pieces; an id or a name said again, as some servers do, is the
 * same.  EPROTO for a call that breaks that order, or for more than
 * JOB_CALLS of them; EILSEQ for what is not UTF-8. */
static int
put_call (void                 *user,
          struct ai_call const *call)
{
	struct job *job = user;
	struct span all = buf_peek(&job->calls);
	struct job_call last = {};
	size_t at = 0;
	uint32_t n = 0;
	for (size_t off = 0; off < all.len; ++n) {
		memcpy(&last, all.ptr + off, sizeof last);
		at = off;
		off += sizeof last + last.id + last.name + last.args;
	}
	unsigned char const *had = n ? all.ptr + at + sizeof last : nullptr;
	size_t id = call->id_len;
	bool same_id = n && id && id == last.id && !memcmp(had, call->id, id);
	uint32_t index = call->index != AI_CALL_NEXT ? call->index
	               : !n || (id && !same_id) ? n : last.index;
	bool next = !n || last.index != index;
	/* A call that came before the last may not go on. */
	for (size_t off = 0; next && off < at; ) {
		struct job_call c;
		memcpy(&c, all.ptr + off, sizeof c);
		if (c.index == index)
			return EPROTO;
		off += sizeof c + c.id + c.name + c.args;
	}
	if (next && n == JOB_CALLS)
		return EPROTO;
	if (next) {
		last = (struct job_call){ .index = index };
		at = all.len;
		had = nullptr;
	}
	size_t name = call->name_len;
	size_t args = call->args_len;
	if (!next && same_id)
		id = 0;
	if (!next && name && name == last.name && !memcmp(had + last.id, call->name, name))
		name = 0;
	if ((id && (last.name || last.args)) || (name && last.args))
		return EPROTO;
	if (jb_utf8(call->id, id) < id || jb_utf8(call->name, name) < name ||
	    jb_utf8(call->args, args) < args)
		return EILSEQ;
	size_t head = next ? sizeof last : 0;
	size_t len = head + id + name + args;
	if (!len)
		return 0;
	struct span s = buf_reserve(&job->calls, len, MAX_RESPONSE);
	if (!s.len)
		return errno;
	/* Only the last part grows, at the end of the last call. */
	unsigned char *p = s.ptr + head;
	if (id)
		memcpy(p, call->id, id);
	if (name)
		memcpy(p + id, call->name, name);
	if (args)
		memcpy(p + id + name, call->args, args);
	last.id += (uint32_t)id;
	last.name += (uint32_t)name;
	last.args += (uint32_t)args;
	buf_commit(&job->calls, len);
	memcpy(buf_peek(&job->calls).ptr + at, &last, sizeof last);
	loop_work(self->loop, LOOP_COPY, len);
	return 0;
}

/* Parse on with @p parser, the @p len bytes at @p json, a slice as far as
 * the step's shares allow, which it counts; true once the parse is over. */
static bool
parse_slice (struct home      *home,
             struct jb_parser *parser,
             char const       *json,
             size_t            len)
{
	struct loop *loop = home->loop;
	uint64_t bytes = loop_room(loop, LOOP_PARSE);
	uint64_t values = loop_room(loop, LOOP_VALUES);
	if (bytes < 4 || !values)
		return false;
	size_t off = parser->off;
	uint32_t seen = parser->cut.values;
	size_t copied = parser->cut.copied;
	bool over = jb_parse_some(parser, &home->pool, json, len, bytes, (uint32_t)values);
	loop_work(loop, LOOP_PARSE, parser->off - off);
	loop_work(loop, LOOP_VALUES, parser->cut.values - seen);
	loop_work(loop, LOOP_COPY, parser->cut.copied - copied);
	return over;
}

/* Parse the event whole in @p job's wire, a slice a step, take what it says,
 * and free it; then go on past it.  0 if the step's share ran out first, -1
 * if the event ended the job.  An event that is not JSON text ends it, as a
 * whole answer that is not would. */
static int
read_event (struct home *home,
            struct job  *job)
{
	struct loop *loop = home->loop;
	struct jb_parser *parser = &job->parser;
	char const *data = (char const *)buf_peek(&job->wire).ptr + job->ev;
	struct ai_event ev = {};
	int e = 0;
	if (job->evlen == 6 && !memcmp(data, "[DONE]", 6)) {
		job->flags |= ENDED;
	} else {
		if (!parse_slice(home, parser, data, job->evlen))
			return 0;
		uint32_t n = parser->cut.values;
		struct json_object *root = jb_parse_take(parser);
		bool json = root;
		if (!json && (!parser->tok ||
		              json_tokener_get_error(parser->tok) == json_tokener_error_memory))
			e = ENOMEM;
		jb_parse_end(parser, &home->pool, &home->reaper);
		/* Each event is a chunk, and one with text or thinking a token. */
		if (json) {
			size_t had = buf_peek(&job->result).len;
			bool tools = job->asks & JOB_TOOLS;
			struct ai_sink const sink = {
				.text = put_text,
				.thought = tools ? put_thought : nullptr,
				.call = tools ? put_call : nullptr,
				.user = job
			};
			e = ai_event(home->ep->protocol, root, &sink, &ev);
			job->used = ev.used ? ev.used : job->used;
			hist_add(&home->metrics->chunks, 1);
			bool text = buf_peek(&job->result).len > had;
			if (text || ev.thought)
				job_progress(job, (uint64_t)++job->chunks << 32 |
				                  (text ? 0 : JOB_THINKING));
		}
		if (ev.reason) {
			size_t len = jb_utf8(ev.reason, strnlen(ev.reason, sizeof job->reason - 1));
			memcpy(job->reason, ev.reason, len);
			job->reason[len] = '\0';
			job->flags = (uint16_t)(ev.truncated ? job->flags | TRUNCATED
			                                     : job->flags & ~TRUNCATED);
		}
		job->flags |= ev.end ? ENDED : 0;
		bool bad = e == EILSEQ || e == EPROTO || (!json && !e);
		if (ev.error) {
			size_t len = strnlen(ev.error, 4096);
			job_say(job, "Upstream error (HTTP %d): %.*s", job->http,
			        (int)jb_utf8(ev.error, len), ev.error);
		} else if (bad) {
			job_say(job, "Upstream error (HTTP %d): %s", job->http,
			        e == EPROTO ? "tool calls out of order, or too many"
			        : json ? "an answer that is not text"
			        : "an event that is not JSON text");
		}
		job->flags |= ev.error || bad ? ERRED : 0;
		if (json && n <= loop_room(loop, LOOP_VALUES)) {
			json_object_put(root);
			loop_work(loop, LOOP_VALUES, n);
		} else if (json) {
			jb_reap(&home->reaper, root);
		}
		if (home->reaper.n)
			loop_ready_bulk(loop, &home->reap);
	}
	buf_consume(&job->wire, job->line);
	job->ev = job->evlen = job->line = job->scan = 0;
	job->flags &= (uint16_t)~EVENT;
	if (job->flags & ERRED || e) {
		job->flags |= e == ENOBUFS ? OVERFLOW : 0;
		job_cut(home, job, job->flags & ERRED ? CURLE_OK
		                 : e == ENOBUFS ? CURLE_WRITE_ERROR : CURLE_OUT_OF_MEMORY);
		return -1;
	}
	return 1;
}

/* Take in the line of @p job's stream that ends at @p end, its line break:
 * a data line's data joins the event's, moved down to follow it, and a blank
 * line ends the event, which has some; every other line says nothing, and
 * goes once no event's data precedes it (SSE). */
static void
read_line (struct job *job,
           size_t      end)
{
	unsigned char *wire = buf_peek(&job->wire).ptr;
	unsigned char *line = wire + job->line;
	size_t len = end - job->line;
	len -= len && line[len - 1] == '\r';
	if (len >= 5 && !memcmp(line, "data:", 5)) {
		size_t skip = 5 + (len > 5 && line[5] == ' ');
		size_t n = len - skip;
		if (!job->evlen) {
			job->ev = job->line + (uint32_t)skip;
		} else if (n) {
			wire[job->ev + job->evlen++] = '\n';
			memmove(wire + job->ev + job->evlen, line + skip, n);
		}
		job->evlen += (uint32_t)n;
	} else if (!len && job->evlen) {
		job->flags |= EVENT;
	}
	job->line = job->scan;
	if (!job->evlen) {
		buf_consume(&job->wire, job->line);
		job->line = job->scan = 0;
	}
}

/* Read @p job's stream as far as the step's shares allow: its lines, and
 * each event whole.  True once it has nothing more to read for now, and is
 * off the list of streams to read, or has ended; false if the shares ran
 * out first.  A stream held back for reading goes on, and one that libcurl
 * has delivered whole ends with its last event read: cut off, as a transfer
 * cut short, unless an event said the answer had ended. */
static bool
read_stream (struct home *home,
             struct job  *job)
{
	struct loop *loop = home->loop;
	for (;;) {
		int r = job->flags & EVENT ? read_event(home, job) : 1;
		if (r <= 0)
			return r < 0;
		struct span w = buf_peek(&job->wire);
		if (job->scan < w.len) {
			uint64_t room = loop_room(loop, LOOP_PARSE);
			if (!room)
				return false;
			size_t n = w.len - job->scan < room ? w.len - job->scan : room;
			unsigned char const *nl = memchr(w.ptr + job->scan, '\n', n);
			size_t end = nl ? (size_t)(nl - w.ptr) : job->scan + n;
			loop_work(loop, LOOP_PARSE, end + !!nl - job->scan);
			job->scan = (uint32_t)(end + !!nl);
			if (nl)
				read_line(job, end);
			continue;
		}
		/* The last line, and the last event, need no line breaks. */
		if (job->flags & DELIVERED && job->line < job->scan) {
			read_line(job, job->scan);
			continue;
		}
		if (job->flags & DELIVERED && job->evlen) {
			job->flags |= EVENT;
			continue;
		}
		if (job->flags & DELIVERED) {
			bool whole = job->flags & ENDED;
			job_finish(home, job, whole ? CURLE_OK : CURLE_PARTIAL_FILE,
			           whole ? "" : "the stream ended before the answer did", 0);
			return true;
		}
		list_del(&job->feed);
		if (job->flags & BACKED) {
			job->flags &= (uint16_t)~BACKED;
			list_append(&home->held, &job->held);
			loop_ready_bulk(loop, &home->resume);
		}
		return true;
	}
}

/* Read the streams, each as far as the step's shares allow, the one that
 * used them up going to the back for the next step. */
static void
feed_step (struct loop      *loop,
           struct loop_step *step)
{
	struct home *home = container_of(step, struct home, feed);
	while (!list_is_empty(&home->feeding)) {
		struct job *job = container_of(home->feeding.next, struct job, feed);
		if (!read_stream(home, job)) {
			list_del(&job->feed);
			list_append(&home->feeding, &job->feed);
			loop_ready_bulk(loop, step);
			return;
		}
	}
}

static void
reap_step (struct loop      *loop,
           struct loop_step *step)
{
	struct home *home = container_of(step, struct home, reap);
	loop_work(loop, LOOP_VALUES, jb_reap_some(&home->reaper,
	                                          loop_room(loop, LOOP_VALUES)));
	if (home->reaper.n)
		loop_ready_bulk(loop, step);
}

/* The breaker opened: end the jobs waiting, in the order they would have
 * started, as many a step as its share of requests allows. */
static void
shed_step (struct loop      *loop,
           struct loop_step *step)
{
	struct home *home = container_of(step, struct home, shed);
	while (home->state == OPEN) {
		struct job *job = job_next(home, false);
		if (!job)
			return;
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		job_refuse(home, job);
		loop_work(loop, LOOP_REQS, 1);
	}
}

/* Whether @p job, a background job running, may be cut: not as the probe,
 * nor once its answer is all in. */
static bool
cuttable (struct job const *job)
{
	return job->transfer && !(job->flags & (PROBE | DELIVERED | ENDED));
}

/* How many background jobs running, of those not being preempted already,
 * hold slots they may not: those beyond the S - 1 they may hold, as S may
 * shrink; and with one slot, the one an interactive job waits for. */
static uint32_t
preempt_count (struct home const *home)
{
	uint32_t may = background_slots(home);
	uint32_t over = home->running_bg > may ? home->running_bg - may : 0;
	if (home->slots == 1 && home->running_bg && !list_is_empty(&home->ring[0]))
		over = 1;
	struct list const *node;
	list_foreach (node, &home->preempting)
		over -= over > 0;
	return over;
}

/* Whether a background job preempting is still to be. */
static bool
preempt_needed (struct home const *home)
{
	uint32_t may = background_slots(home);
	return home->running_bg > may ||
	       (home->slots == 1 && !list_is_empty(&home->ring[0]));
}

/* Have the background jobs that hold slots they may not preempted, the first
 * to have started first. */
static void
preempt_excess (struct home *home)
{
	uint32_t over = preempt_count(home);
	struct list *node;
	struct list *next;
	list_foreach_safe (node, next, &home->background) {
		struct job *job = container_of(node, struct job, queue);
		if (!over)
			break;
		/* Once at most. */
		if (!cuttable(job) || job->flags & PREEMPTED)
			continue;
		job->flags |= PREEMPTED;
		list_del(&job->queue);
		list_append(&home->preempting, &job->queue);
		loop_ready_bulk(home->loop, &home->preempt);
		--over;
	}
}

/* Preempt the background jobs that interactive ones preempt: cut each one's
 * upstream call, and have it wait again, ready, first of the background
 * jobs, its deadline as it was; as many a step as its share of requests
 * allows.  A preemption is no outcome of the upstream's, which the breaker
 * counts. */
static void
preempt_step (struct loop      *loop,
              struct loop_step *step)
{
	struct home *home = container_of(step, struct home, preempt);
	struct home_metrics *m = home->metrics;
	for (; !list_is_empty(&home->preempting); loop_work(loop, LOOP_REQS, 1)) {
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		struct job *job = container_of(home->preempting.next, struct job, queue);
		list_del(&job->queue);
		/* Its answer may be all in by now, or the interactive job gone. */
		if (!cuttable(job) || !preempt_needed(home)) {
			job->flags &= (uint16_t)~PREEMPTED;
			list_append(&home->background, &job->queue);
			continue;
		}
		if (job->transfer && http_sent(job->transfer))
			hist_add(&m->aborted, 1);
		http_cancel(home->http, &job->transfer);
		list_del(&job->idle);
		list_del(&job->feed);
		list_del(&job->held);
		jb_parse_end(&job->parser, &home->pool, &home->reaper);
		if (home->reaper.n)
			loop_ready_bulk(loop, &home->reap);
		buf_free(&job->wire);
		buf_consume(&job->result, buf_peek(&job->result).len);
		buf_consume(&job->calls, buf_peek(&job->calls).len);
		buf_consume(&job->thought, buf_peek(&job->thought).len);
		job->flags = PREEMPTED | READY;
		job->http = 0;
		job->ev = job->evlen = job->line = job->scan = job->chunks = job->used = 0;
		job->reason[0] = '\0';
		hist_add(&m->running, -1ULL);
		hist_add(&m->background, -1ULL);
		hist_add(&m->preempted, 1);
		--home->running_bg;
		list_append(home->ready[1].next, &job->queue);
		++home->readied[1];
		job_progress(job, 1);
		loop_ready_bulk(loop, &home->start);
	}
}


/* Cancel the jobs on @p list, by their queue hooks or, @p due, their due
 * hooks, as many as the step's share of requests allows; false if some are
 * left. */
static bool
cancel_some (struct home *home,
             struct loop *loop,
             struct list *list,
             bool         due)
{
	for (; !list_is_empty(list); loop_work(loop, LOOP_REQS, 1)) {
		if (!loop_room(loop, LOOP_REQS))
			return false;
		job_cancel(home, due ? container_of(list->next, struct job, due)
		                      : container_of(list->next, struct job, queue));
	}
	return true;
}

static void
stop_step (struct loop      *loop,
           struct loop_step *step)
{
	struct home *home = container_of(step, struct home, stop);
	if (!cancel_some(home, loop, &home->dropped, false))
		loop_ready_bulk(loop, step);
}

/* Draining: cancel every job in flight. */
static void
drain_step (struct loop      *loop,
            struct loop_step *step)
{
	struct home *home = container_of(step, struct home, drain);
	if (!cancel_some(home, loop, &home->timeline, true))
		loop_ready_bulk(loop, step);
}

/* Let the slots of the upstream be @p n, and say so, and why, unless
 * @p why is nullptr: for now. */
static void
home_slots (struct home *home,
            uint32_t     n,
            char const  *why)
{
	home->slots = n;
	atomic_store_explicit(&home->metrics->slots, n, memory_order_relaxed);
	http_connections(home->http, n);
	if (why)
		LOG(LOG_INFO, "upstream slots: %" PRIu32 " (%s)", n, why);
	loop_ready_bulk(home->loop, &home->start);
	/* Fewer slots may leave background jobs holding more than they may. */
	preempt_excess(home);
}

/* Gather /props's answer, up to MAX_PROPS bytes, as much a step as its share
 * of receiving takes, the rest held back for the props step. */
static size_t
props_write (char const *data,
             size_t      len,
             void       *user)
{
	struct home *home = user;
	if (!len)
		return 0;
	if (loop_room(home->loop, LOOP_RECV) < len) {
		home->paused = true;
		loop_ready_bulk(home->loop, &home->props);
		return CURL_WRITEFUNC_PAUSE;
	}
	struct span s = buf_reserve(&home->answer, len, MAX_PROPS);
	if (!s.len)
		return 0;
	memcpy(s.ptr, data, len);
	buf_commit(&home->answer, len);
	loop_work(home->loop, LOOP_RECV, len);
	return len;
}

static void
props_done (CURLcode    result,
            long        status,
            char const *error,
            void       *user)
{
	struct home *home = user;
	home->ask = nullptr;
	home->paused = false;
	if (result == CURLE_OK && status == 200) {
		loop_ready_bulk(home->loop, &home->props);
		return;
	}
	char why[64 + CURL_ERROR_SIZE];
	if (result)
		snprintf(why, sizeof why, "the default: /props: %s", *error ? error
		         : curl_easy_strerror(result));
	else
		snprintf(why, sizeof why, "the default: /props: HTTP %ld", status);
	buf_free(&home->answer);
	if (home->ep->slots)
		http_connections(home->http, home->slots);
	else
		home_slots(home, SLOTS, why);
}

/* Let /props's answer on, once the step has the share for it, or parse it a
 * slice a step, and take the slots its total_slots says, unless told, and
 * the context its slots' n_ctx says, likewise. */
static void
props_step (struct loop      *loop,
            struct loop_step *step)
{
	struct home *home = container_of(step, struct home, props);
	if (home->paused) {
		home->paused = false;
		CURLcode e = http_resume(home->ask);
		if (e)
			http_end(home->http, &home->ask, e);
		return;
	}
	struct jb_parser *parser = &home->parser;
	struct span json = buf_peek(&home->answer);
	if (!parse_slice(home, parser, json.len ? (char const *)json.ptr : "", json.len)) {
		loop_ready_bulk(loop, step);
		return;
	}
	struct json_object *root = jb_parse_take(parser);
	jb_parse_end(parser, &home->pool, &home->reaper);
	struct json_object *total = jb_get(root, "total_slots");
	struct json_object *ctx = jb_get(jb_get(root, "default_generation_settings"),
	                                 "n_ctx");
	int64_t n = json_object_is_type(total, json_type_int)
	          ? json_object_get_int64(total) : 0;
	int64_t tokens = json_object_is_type(ctx, json_type_int)
	               ? json_object_get_int64(ctx) : 0;
	jb_reap(&home->reaper, root);
	loop_ready_bulk(loop, &home->reap);
	buf_free(&home->answer);
	if (tokens > 0 && !home->context) {
		home->context = tokens < INT32_MAX ? (uint32_t)tokens : INT32_MAX;
		LOG(LOG_INFO, "upstream context: %" PRIu32 " tokens (llama-server's /props)",
		    home->context);
	}
	/* Told the slots, /props had a connection of its own. */
	if (home->ep->slots) {
		http_connections(home->http, home->slots);
		return;
	}
	if (n > 0)
		home_slots(home, n < UINT16_MAX ? (uint32_t)n : UINT16_MAX,
		           "llama-server's /props");
	else
		home_slots(home, SLOTS, "the default: /props gives no total_slots");
}

/* Ask the upstream how many slots it has, as llama-server says at GET /props
 * of its endpoint's server, unless it speaks Gemini's protocol, which
 * llama-server does not, or is a well-known cloud API; false if not asked.
 * The answer may take PROPS_MS, or the calls' timeout if shorter. */
static bool
props_ask (struct home *home)
{
	static char const *const clouds[] = {
		"api.openai.com", "api.anthropic.com", "openrouter.ai", "api.groq.com",
		"api.mistral.ai", "api.together.xyz", "api.deepseek.com", "api.x.ai",
		"api.fireworks.ai",
	};
	static char const azure[] = ".openai.azure.com";
	struct ai_endpoint const *ep = home->ep;
	if (ep->protocol == AI_GEMINI)
		return false;
	long ms = ep->timeout_ms > 0 && ep->timeout_ms < PROPS_MS ? ep->timeout_ms : PROPS_MS;
	CURLU *u = curl_url();
	char *host = nullptr;
	char *url = nullptr;
	bool ok = u && !curl_url_set(u, CURLUPART_URL, ep->url, 0) &&
	          !curl_url_get(u, CURLUPART_HOST, &host, 0);
	for (size_t i = 0; ok && i < ARRAY_SIZE(clouds); ++i)
		ok = strcasecmp(host, clouds[i]);
	size_t len = ok ? strlen(host) : 0;
	ok = ok && (len < sizeof azure ||
	            strcasecmp(host + len - (sizeof azure - 1), azure)) &&
	     !curl_url_set(u, CURLUPART_PATH, "/props", 0) &&
	     !curl_url_set(u, CURLUPART_QUERY, nullptr, 0) &&
	     !curl_url_set(u, CURLUPART_FRAGMENT, nullptr, 0) &&
	     !curl_url_get(u, CURLUPART_URL, &url, 0) &&
	     !http_post_json(home->http, url, nullptr, 0, false, ep->headers, ms,
	                     props_write, props_done, home, &home->ask);
	curl_free(url);
	curl_free(host);
	curl_url_cleanup(u);
	return ok;
}

/* End the jobs in flight that the key of @p carrier names, as their client
 * asked: at once those not running upstream, and by a step those running,
 * the same that cancels those their front let go of.  One its front let go
 * of ends so already.  A key that names none is kept a while, as it may
 * name a call that lends tools between the jobs of its turns. */
static void
home_withdraw (struct home      *home,
               struct job const *carrier)
{
	struct span key = buf_peek(&carrier->key);
	struct job *next;
	bool found = false;
	for (struct job *job = *home_bucket(home, carrier->hash); job; job = next) {
		struct span k = buf_peek(&job->key);
		next = job->chain;
		if (job->hash != carrier->hash || k.len != key.len ||
		    memcmp(k.ptr, key.ptr, key.len) || job->flags & (DROPPED | WITHDRAWN))
			continue;
		found = true;
		job->flags |= WITHDRAWN;
		if (!job->transfer) {
			job_end(home, job, JOB_WITHDRAWN, &home->metrics->cancelled);
			continue;
		}
		list_del(&job->queue);
		list_append(&home->dropped, &job->queue);
		loop_ready_bulk(home->loop, &home->stop);
	}
	if (!found)
		home->gone[home->gone_at++ % WITHDRAWALS] = (typeof(home->gone[0])){
			carrier->hash, home->loop->now + WITHDRAWAL_NS, key.len
		};
}

/* Whether @p job, of a call that lends tools, was withdrawn between its
 * turns' jobs: a withdrawal kept for its key ends a turn after the first,
 * and goes; a first turn's makes it go, as it was the call's before, whose
 * id this one has again. */
static bool
withdrawn (struct home *home,
           struct job  *job)
{
	size_t len = buf_peek(&job->key).len;
	for (size_t i = 0; i < WITHDRAWALS; ++i) {
		if (home->gone[i].until <= home->loop->now || home->gone[i].hash != job->hash ||
		    home->gone[i].len != len)
			continue;
		home->gone[i].until = 0;
		if (job->asks & JOB_AGAIN)
			return true;
	}
	return false;
}

/* Admit @p job, or end it at once: draining, with the breaker open, with
 * --max-calls in flight, or with QUEUED of its class queued for its
 * principal (unless all of a class share one queue, first in first out).
 * If a slot is free for it, the breaker closed and no job of its class or
 * before it waiting (and, a background job, the background ones holding
 * fewer slots than they may), it is ready, queued for none, so that a burst
 * the slots free take at once is not refused; else it joins its principal's
 * queue, and its principal its class's ring, if new to it, at the back.  An
 * interactive job that waits with one slot preempts a background one. */
static void
home_admit (struct home *home,
            struct job  *job)
{
	struct home_metrics *m = home->metrics;
	if (home->draining) {
		job_end(home, job, JOB_CANCELLED, &m->rejected);
		return;
	}
	if (job->asks & JOB_TOOLS && withdrawn(home, job)) {
		job_end(home, job, JOB_WITHDRAWN, &m->cancelled);
		return;
	}
	if (home->state == OPEN) {
		job_refuse(home, job);
		return;
	}
	struct principal **slot = principal_slot(home, job);
	struct principal *p = *slot;
	if (hist_get(&m->in_flight) >= home->max ||
	    (p && !home->fifo && p->queued >= QUEUED)) {
		job_end(home, job, JOB_BUSY, &m->rejected);
		return;
	}
	uint32_t c = class_of(job);
	bool ready = home->state == CLOSED && list_is_empty(&home->ring[0]) &&
	             hist_get(&m->running) + home->readied[0] + home->readied[1] <
	             home->slots &&
	             (!c || (list_is_empty(&home->ring[1]) &&
	                     home->running_bg + home->readied[1] < background_slots(home)));
	if (!p && !ready) {
		struct list *spare = shelf_take(&home->spare);
		p = spare ? container_of(spare, struct principal, ring) : malloc(sizeof *p);
		if (!p) {
			job_end(home, job, JOB_FAILED, &m->rejected);
			return;
		}
		*p = (struct principal){ .hash = job->principal, .class = c };
		list_init(&p->jobs);
		*slot = p;
		list_append(&home->ring[c], &p->ring);
		hist_add(&m->principals, 1);
	}
	struct job **link = home_bucket(home, job->hash);
	job->chain = *link;
	*link = job;
	list_append(&home->timeline, &job->due);
	if (job->deadline && home->deadline.due == UINT64_MAX)
		(void)loop_timer_set(home->loop, &home->deadline, job->deadline);
	hist_add(&m->in_flight, 1);
	if (ready) {
		list_append(&home->ready[c], &job->queue);
		job->flags |= READY;
		++home->readied[c];
	} else {
		list_append(&p->jobs, &job->queue);
		job->who = p;
		hist_add(&m->queued, 1);
		job_progress(job, ++p->queued);
		if (!c)
			preempt_excess(home);
	}
	loop_ready_bulk(home->loop, &home->start);
	/* It may need a descriptor upstream. */
	httpd_home_crowd(home->door);
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
		job->flags = job->http = 0;
		job->ev = job->evlen = job->line = job->scan = job->chunks = job->used = 0;
		job->reason[0] = '\0';
		if (m)
			hist_add(&m->submitted, 1);
		if (!home || bits & JOB_CANCEL)
			job_end(home, job, JOB_CANCELLED, m ? &m->cancelled : nullptr);
		else
			home_admit(home, job);
	} else if (bits & JOB_CANCEL && job->transfer) {
		/* Cancelling it takes a step, which then lets go of it. */
		job->flags |= DROPPED;
		list_del(&job->queue);
		list_append(&home->dropped, &job->queue);
		loop_ready_bulk(loop, &home->stop);
		home_put(job, 1);
		return;
	} else if (bits & JOB_CANCEL && !list_is_empty(&job->due)) {
		job_end(home, job, JOB_CANCELLED, &m->cancelled);
	}
	/* Let go of the job as its front has, the result's storage back to the
	 * stash it came from. */
	if (bits & JOB_CANCEL)
		answer_free(job);
	home_put(job, bits & JOB_CANCEL ? 2 : 1);
}

int
home_init (struct home               **p_home,
           struct loop                *loop,
           struct blockers            *blockers,
           struct httpd_home          *door,
           struct ai_endpoint const   *ep,
           struct config const        *cfg,
           struct home_metrics        *metrics,
           struct http_metrics        *http)
{
	uint32_t max = cfg->calls;
	size_t buckets = stdc_bit_ceil(max < KEYS_MAX ? max : (uint32_t)KEYS_MAX);
	struct home *home = malloc(sizeof *home);
	struct job **keys = calloc(buckets, sizeof *keys);
	struct principal **whom = calloc(buckets, sizeof *whom);
	if (!home || !keys || !whom) {
		free(whom);
		free(keys);
		free(home);
		return ENOMEM;
	}
	*home = (struct home){
		.start.fn = start_step,
		.stop.fn = stop_step,
		.drain.fn = drain_step,
		.overdue.fn = overdue_step,
		.silent.fn = silent_step,
		.resume.fn = resume_step,
		.feed.fn = feed_step,
		.reap.fn = reap_step,
		.shed.fn = shed_step,
		.preempt.fn = preempt_step,
		.props.fn = props_step,
		.deadline.fn = deadline_event,
		.quiet.fn = quiet_event,
		.breaker.fn = breaker_event,
		.pool = {
			.scalar = MAX_SCALAR,
			.flags = JSON_TOKENER_VALIDATE_UTF8
		},
		.metrics = metrics,
		.loop = loop,
		.door = door,
		.ep = ep,
		.keys = keys,
		.whom = whom,
		.mask = buckets - 1,
		.timeout = ep->timeout_ms > 0 ? (uint64_t)ep->timeout_ms * 1'000'000 : 0,
		.silence = ep->chunk_ms > 0 ? (uint64_t)ep->chunk_ms * 1'000'000 : 0,
		.backoff = BACKOFF_MIN,
		.max = max,
		.fifo = (cfg->flags & CFG_TEST_FIFO) != 0
	};
	for (size_t c = 0; c < 2; ++c) {
		list_init(&home->ring[c]);
		list_init(&home->ready[c]);
	}
	list_init(&home->background);
	list_init(&home->preempting);
	list_init(&home->timeline);
	list_init(&home->dropped);
	list_init(&home->held);
	list_init(&home->idle);
	list_init(&home->feeding);
	list_init(&home->spare.list);
	/* In the heap from the start, the timers are set without fail. */
	int e = loop_timer_set(loop, &home->deadline, UINT64_MAX);
	if (!e)
		e = loop_timer_set(loop, &home->quiet, UINT64_MAX);
	if (!e)
		e = loop_timer_set(loop, &home->breaker, UINT64_MAX);
	if (!e)
		e = http_init(&home->http, loop, http);
	if (e) {
		loop_timer_stop(loop, &home->breaker);
		loop_timer_stop(loop, &home->quiet);
		loop_timer_stop(loop, &home->deadline);
		free(whom);
		free(keys);
		free(home);
		return e;
	}
	http_resolver(home->http, blockers);
	self = *p_home = home;
	/* Until /props answers, if asked, the default, said once it has; and
	 * the context, which compacting it takes, unless told. */
	home->context = ep->context > 0 ? (uint32_t)ep->context : 0;
	bool asked = (!ep->slots || (cfg->flags & CFG_UPSTREAM_TOOLS && !home->context)) &&
	             props_ask(home);
	if (ep->slots)
		home_slots(home, ep->slots, "--slots");
	else
		home_slots(home, SLOTS, asked ? nullptr : "the default");
	/* Told the slots, /props takes no call's connection. */
	if (ep->slots && asked)
		http_connections(home->http, ep->slots + 1L);
	return 0;
}

void
home_fini (struct home **p_home)
{
	struct home *home = *p_home;
	if (!home)
		return;
	*p_home = nullptr;
	while (!list_is_empty(&home->timeline))
		job_cancel(home, container_of(home->timeline.next, struct job, due));
	struct home_metrics const *m = home->metrics;
	LOG(LOG_INFO, "jobs: %" PRIu64 " submitted, %" PRIu64 " completed, %" PRIu64
	    " cancelled, %" PRIu64 " rejected; %" PRIu64 " cancelled upstream; %"
	    PRIu64 " preempted", hist_get(&m->submitted), hist_get(&m->completed),
	    hist_get(&m->cancelled), hist_get(&m->rejected), hist_get(&m->aborted),
	    hist_get(&m->preempted));
	http_cancel(home->http, &home->ask);
	struct loop_step *const steps[] = {
		&home->start, &home->stop, &home->drain, &home->overdue, &home->silent,
		&home->resume, &home->feed, &home->reap, &home->shed, &home->preempt,
		&home->props,
	};
	for (size_t i = 0; i < ARRAY_SIZE(steps); ++i)
		loop_unready(steps[i]);
	loop_timer_stop(home->loop, &home->breaker);
	loop_timer_stop(home->loop, &home->quiet);
	loop_timer_stop(home->loop, &home->deadline);
	http_fini(&home->http);
	jb_parse_end(&home->parser, &home->pool, nullptr);
	jb_reaper_fini(&home->reaper);
	buf_free(&home->answer);
	while (home_relieve(home, BUF_CRITICAL, SIZE_MAX))
		;
	self = nullptr;
	free(home->whom);
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

void
home_age (struct home *home)
{
	shelf_age(&home->spare);
}

size_t
home_relieve (struct home       *home,
              enum buf_pressure  level,
              size_t             max)
{
	/* A tokener keeps the buffer of the longest string it parsed: at high
	 * pressure one longer than a slice goes, at critical any, each in a
	 * call of its own. */
	size_t size = jb_pool_release(&home->pool, level >= BUF_CRITICAL ? 0
	                                                          : LOOP_STEP_PARSE);
	if (size)
		return buf_units(size);
	/* Principals' entries keep no storage, so each is a unit. */
	size_t done = 0;
	for (size_t n = shelf_unused(&home->spare, level); n && done < max; --n, ++done)
		free(container_of(shelf_drop(&home->spare), struct principal, ring));
	return done;
}
